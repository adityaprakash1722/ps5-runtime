"""Local-only batch inspection. Reports contain sensitive paths and metadata.

Never run this against private inputs in CI. No input is executed or uploaded.
Only the explicitly selected inspector executable runs. Output must be a new
directory outside the source tree and outside any ancestor Git working tree.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys

MAX_FILE_BYTES = 256 * 1024 * 1024
CANDIDATE_SUFFIXES = {".elf", ".self", ".prx", ".sprx", ".o", ".obj", ".part"}


def linked(path):
    info = path.lstat()
    return stat.S_ISLNK(info.st_mode) or bool(
        getattr(info, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0))


def validate_output(root, output):
    if output == root or root in output.parents:
        raise ValueError("Output must be outside the input tree.")
    for ancestor in (output, *output.parents):
        if (ancestor / ".git").exists():
            raise ValueError("Output must be outside Git working trees, even ignored directories.")
    if output.exists():
        raise ValueError("Output directory already exists; choose a new directory.")


def candidates(root, max_files):
    counts = Counter()
    selected = []

    def walk_error(_error):
        # Do not leak paths to the console or call an incomplete scan successful.
        raise OSError("Directory traversal failed.")

    for directory, dirs, files in os.walk(root, followlinks=False, onerror=walk_error):
        base = Path(directory)
        kept = []
        for name in sorted(dirs):
            if name == ".git" or linked(base / name):
                counts["excluded_directories"] += 1
            else:
                kept.append(name)
        dirs[:] = kept
        for name in sorted(files):
            counts["files_seen"] += 1
            if counts["files_seen"] > max_files:
                raise ValueError("File-count limit exceeded; no validation was started.")
            path = base / name
            if linked(path) or not path.is_file():
                counts["excluded_links_or_special_files"] += 1
                continue
            with path.open("rb") as stream:
                magic = stream.read(4)
            # Check magic as well as suffix: ELF objects need not end in .elf.
            if magic == b"\x7fELF" or path.suffix.lower() in CANDIDATE_SUFFIXES:
                selected.append(path)
            else:
                counts["non_candidates"] += 1
    return selected, counts


def fingerprint(path):
    digest = hashlib.sha256()
    count = 0
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            count += len(chunk)
            if count > MAX_FILE_BYTES:
                raise ValueError("File grew beyond the input limit.")
            digest.update(chunk)
    return digest.hexdigest()


def inspect_one(exe, path, root, timeout):
    record = {"relative_path": path.relative_to(root).as_posix()}
    try:
        before = path.stat()
        record["size"] = before.st_size
        if before.st_size > MAX_FILE_BYTES:
            record["outcome"] = "input_limit"
            return record
        record["sha256"] = fingerprint(path)
        result = subprocess.run([str(exe), "plan", str(path), "--json"],
                                capture_output=True, timeout=timeout)
        record["exit_code"] = result.returncode
        # Do not persist uncontrolled stderr or malformed stdout. A valid report
        # itself may include sensitive names and remains local only.
        try:
            report = json.loads(result.stdout)
        except (ValueError, UnicodeError):
            record["outcome"] = "inspector_protocol_error"
            return record
        if not isinstance(report, dict) or report.get("schema_version") != 1 or report.get("execution_supported") is not False:
            record["outcome"] = "inspector_protocol_error"
            return record
        for key in ("diagnostics", "plan_diagnostics"):
            items = report.get(key, [] if key == "plan_diagnostics" else None)
            if not isinstance(items, list) or not all(
                    isinstance(item, dict) and isinstance(item.get("code"), str) for item in items):
                record["outcome"] = "inspector_protocol_error"
                return record
        expected = {
            0: ("accepted_subset", "offline_only", "offline_plan_only"),
            1: ("rejected", None, "inspection_rejected"),
            3: ("not_inspected", None, "host_error"),
            4: ("accepted_subset", "rejected", "plan_rejected"),
        }.get(result.returncode)
        if (expected is None or report.get("inspection_status") != expected[0]
                or report.get("plan_status") != expected[1] or result.stderr):
            record["outcome"] = "inspector_protocol_error"
            return record
        after_hash = fingerprint(path)
        after = path.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns) or record["sha256"] != after_hash:
            record["outcome"] = "input_changed"
            return record
        record["report"] = report
        record["outcome"] = expected[2]
    except subprocess.TimeoutExpired:
        record["outcome"] = "inspector_timeout"
    except (OSError, ValueError):
        record["outcome"] = "host_error"
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inspector", required=True, type=Path)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--max-files", type=int, default=10000)
    parser.add_argument("--timeout", type=float, default=15)
    args = parser.parse_args()
    try:
        root = args.root.resolve(strict=True)
        exe = args.inspector.resolve(strict=True)
        output = args.output.resolve()
        if not root.is_dir() or not exe.is_file() or args.max_files < 1 or not 0 < args.timeout <= 300:
            raise ValueError("Invalid root, inspector, file limit, or timeout.")
        validate_output(root, output)
        paths, inventory = candidates(root, args.max_files)
        if not paths:
            raise ValueError("No candidate files found; no success report will be written.")
        exe_hash = fingerprint(exe)
        runner_hash = fingerprint(Path(__file__).resolve())
        output.mkdir(parents=True, exist_ok=False)
        outcomes = Counter()
        codes = Counter()
        started = datetime.now(timezone.utc).isoformat()
        with (output / "records.jsonl").open("x", encoding="utf-8") as records:
            for index, path in enumerate(paths, 1):
                record = inspect_one(exe, path, root, args.timeout)
                outcomes[record["outcome"]] += 1
                for key in ("diagnostics", "plan_diagnostics"):
                    for diagnostic in record.get("report", {}).get(key, []):
                        codes[diagnostic["code"]] += 1
                records.write(json.dumps(record, ensure_ascii=True) + "\n")
                records.flush()
                if index % 250 == 0:
                    print(f"Inspected {index}/{len(paths)} candidates", flush=True)
        clean_outcomes = {"offline_plan_only", "inspection_rejected", "plan_rejected"}
        complete = all(outcome in clean_outcomes for outcome in outcomes)
        if fingerprint(exe) != exe_hash:
            complete = False
        summary = {
            "schema_version": 1, "started_utc": started,
            "finished_utc": datetime.now(timezone.utc).isoformat(),
            "source_root": str(root), "inspector_path": str(exe),
            "inspector_sha256": exe_hash, "inventory": dict(inventory),
            "runner_sha256": runner_hash,
            "policy": {"max_files": args.max_files, "timeout_seconds": args.timeout,
                       "max_file_bytes": MAX_FILE_BYTES,
                       "candidate_suffixes": sorted(CANDIDATE_SUFFIXES)},
            "candidate_count": len(paths), "outcomes": dict(outcomes),
            "diagnostic_counts": dict(codes), "run_complete": complete,
            "execution_tested": False,
            "interpretation": "Completion records parser outcomes, not compatibility or correctness. Rejections require review.",
        }
        with (output / "summary.json").open("x", encoding="utf-8") as stream:
            json.dump(summary, stream, indent=2, ensure_ascii=True)
            stream.write("\n")
        print(json.dumps({"candidate_count": len(paths), "outcomes": dict(outcomes),
                          "run_complete": complete, "execution_tested": False}))
        return 0 if complete else 1
    except (OSError, ValueError):
        print("Validation could not complete. Check paths, limits, access, and that output is new and outside Git/input trees.", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
