"""Synthetic-only tests for the local validation runner; no real input paths."""
import importlib.util
import contextlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "validate_corpus", Path(__file__).resolve().parents[1] / "tools" / "validate_corpus.py")
corpus = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(corpus)
INSPECTOR = str(Path(sys.argv.pop(1)).resolve())


class CorpusTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ps5rt-corpus-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name).resolve()
        self.root = self.base / "inputs"
        self.root.mkdir()
        self.file = self.root / "broken.elf"
        self.file.write_bytes(b"synthetic")

    def invoke(self, *extra, expected=0):
        command = [sys.executable, str(Path(corpus.__file__)), "--inspector", INSPECTOR,
                   "--root", str(self.root), "--output", str(self.base / "reports"), *map(str, extra)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        # Neither normal output nor failure output may reveal input paths.
        self.assertNotIn(str(self.root), result.stdout + result.stderr)
        self.assertNotIn(self.file.name, result.stdout + result.stderr)
        return result

    def test_real_inspector_local_report_and_overwrite_refusal(self):
        self.invoke()
        folder = self.base / "reports"
        summary = json.loads((folder / "summary.json").read_text())
        self.assertTrue(summary["run_complete"])
        self.assertFalse(summary["execution_tested"])
        self.assertEqual(summary["runner_sha256"], corpus.fingerprint(Path(corpus.__file__)))
        self.assertEqual(summary["policy"]["max_files"], 10000)
        self.assertEqual(summary["outcomes"], {"inspection_rejected": 1})
        original = (folder / "records.jsonl").read_bytes()
        record = json.loads(original)
        self.assertEqual(record["relative_path"], "broken.elf")
        self.assertEqual(record["sha256"], corpus.fingerprint(self.file))
        self.invoke(expected=2)
        self.assertEqual((folder / "records.jsonl").read_bytes(), original)

    def test_reject_output_in_source_and_git(self):
        with self.assertRaises(ValueError):
            corpus.validate_output(self.root, self.root / "reports")
        checkout = self.base / "checkout"
        checkout.mkdir()
        (checkout / ".git").write_text("gitdir: somewhere", encoding="utf-8")
        with self.assertRaises(ValueError):
            corpus.validate_output(self.root, checkout / "ignored" / "reports")
        corpus.validate_output(self.root, self.base / "outside")

    def test_magic_detection_and_directory_exclusion(self):
        (self.root / "no_extension").write_bytes(b"\x7fELF")
        (self.root / "notes.txt").write_text("synthetic", encoding="utf-8")
        (self.root / ".git").mkdir()
        (self.root / ".git" / "hidden.elf").write_bytes(b"\x7fELF")
        paths, counts = corpus.candidates(self.root, 10)
        self.assertEqual({p.name for p in paths}, {"broken.elf", "no_extension"})
        self.assertEqual(counts["non_candidates"], 1)
        self.assertEqual(counts["excluded_directories"], 1)
        with self.assertRaises(ValueError):
            corpus.candidates(self.root, 1)

    def test_empty_inventory_and_limits_do_not_report_success(self):
        self.file.unlink()
        self.invoke(expected=2)
        self.assertFalse((self.base / "reports").exists())
        self.invoke("--max-files", "0", expected=2)
        self.invoke("--timeout", "nan", expected=2)

    def test_links_are_excluded_without_following(self):
        # Mock link identification so Windows need not grant symlink privileges.
        with patch.object(corpus, "linked", return_value=True):
            paths, counts = corpus.candidates(self.root, 10)
        self.assertEqual(paths, [])
        self.assertEqual(counts["excluded_links_or_special_files"], 1)

    def fake_report(self, code=0, status="accepted_subset", plan="offline_only"):
        report = {"schema_version": 1, "execution_supported": False,
                  "inspection_status": status, "diagnostics": []}
        if plan is not None:
            report["plan_status"] = plan
        return subprocess.CompletedProcess([], code, json.dumps(report).encode(), b"")

    def inspect_fake(self, result):
        with patch.object(corpus.subprocess, "run", return_value=result):
            return corpus.inspect_one(Path(INSPECTOR), self.file, self.root, 1)

    def test_outcomes_distinguish_plan_rejection_from_inspection(self):
        self.assertEqual(self.inspect_fake(self.fake_report())["outcome"], "offline_plan_only")
        self.assertEqual(self.inspect_fake(self.fake_report(4, plan="rejected"))["outcome"], "plan_rejected")
        self.assertEqual(self.inspect_fake(self.fake_report(1, "rejected", None))["outcome"], "inspection_rejected")
        self.assertEqual(self.inspect_fake(self.fake_report(3, "not_inspected", None))["outcome"], "host_error")

    def test_protocol_mismatches_and_uncontrolled_output(self):
        cases = [self.fake_report(1), self.fake_report(0, "rejected", None),
                 subprocess.CompletedProcess([], -9, b"", b"private path"),
                 subprocess.CompletedProcess([], 0, b"[]", b""),
                 subprocess.CompletedProcess([], 0, b'{"schema_version":1,"execution_supported":false,"diagnostics":[null]}', b"")]
        extra = self.fake_report()
        extra.stderr = b"private path"
        cases.append(extra)
        for result in cases:
            with self.subTest(result=result):
                record = self.inspect_fake(result)
                self.assertEqual(record["outcome"], "inspector_protocol_error")
                self.assertNotIn("private path", json.dumps(record))

    def test_timeout_and_changed_inputs(self):
        with patch.object(corpus.subprocess, "run", side_effect=subprocess.TimeoutExpired([], 1)):
            record = corpus.inspect_one(Path(INSPECTOR), self.file, self.root, 1)
        self.assertEqual(record["outcome"], "inspector_timeout")
        with patch.object(corpus, "fingerprint", side_effect=["before", "after"]):
            self.assertEqual(self.inspect_fake(self.fake_report())["outcome"], "input_changed")

    def test_size_limit_precedes_read_and_subprocess(self):
        with patch.object(corpus, "MAX_FILE_BYTES", 1), patch.object(corpus.subprocess, "run") as process:
            record = corpus.inspect_one(Path(INSPECTOR), self.file, self.root, 1)
            process.assert_not_called()
        self.assertEqual(record["outcome"], "input_limit")
        self.assertNotIn("sha256", record)

    def test_only_inspector_is_executed(self):
        with patch.object(corpus.subprocess, "run", return_value=self.fake_report()) as process:
            corpus.inspect_one(Path(INSPECTOR), self.file, self.root, 1)
            process.assert_called_once_with([INSPECTOR, "plan", str(self.file), "--json"],
                                            capture_output=True, timeout=1)

    def test_real_supported_layout_still_reports_no_execution(self):
        data = bytearray(272)
        struct.pack_into("<16sHHIQQQIHHHHHH", data, 0,
                         b"\x7fELF\x02\x01\x01" + bytes(9), 2, 62, 1,
                         0x400100, 64, 0, 0, 64, 56, 1, 0, 0, 0)
        struct.pack_into("<IIQQQQQQ", data, 64, 1, 5, 256, 0x400100, 0, 16, 32, 256)
        self.file.write_bytes(data)
        self.invoke()
        summary = json.loads((self.base / "reports" / "summary.json").read_text())
        self.assertEqual(summary["outcomes"], {"offline_plan_only": 1})
        self.assertFalse(summary["execution_tested"])

    def test_finished_run_with_tool_error_is_not_green(self):
        argv = ["validate_corpus.py", "--inspector", INSPECTOR, "--root", str(self.root),
                "--output", str(self.base / "reports")]
        with patch.object(sys, "argv", argv), patch.object(corpus, "inspect_one", return_value={
                "relative_path": "broken.elf", "outcome": "inspector_timeout"}), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(corpus.main(), 1)
        summary = json.loads((self.base / "reports" / "summary.json").read_text())
        self.assertFalse(summary["run_complete"])


if __name__ == "__main__":
    unittest.main()
