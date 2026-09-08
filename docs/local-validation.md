# Local corpus validation

A **corpus** is a collection of inputs used to exercise a program. Batch
inspection is useful for finding unsupported formats, malformed inputs, and
unexpected failures. It is not a compatibility test and does not run guest code.

`tools/validate_corpus.py` invokes an explicitly selected local `ps5rt` executable
using its `plan --json` command. Only select the inspector you built and trust;
the `--inspector` argument names a host program that really will be executed.
Never point it at an input executable from the corpus.

```powershell
python tools/validate_corpus.py --inspector build/Release/ps5rt.exe `
  --root D:/Validation/inputs --output D:/Validation/runs/new-run
```

The output must be a new directory outside the input tree and outside Git working
trees, including their ignored folders. Existing reports are never overwritten.
Keep this directory on approved local storage. Do not run private collections in
GitHub Actions or add their reports to issues, commits, or artifacts.

## What is recorded

The runner scans at most 10,000 files by default. It selects ELF magic bytes
(the initial bytes that identify an ELF file), irrespective of extension, plus
common executable/object suffixes and `.part` files. This is a selection rule,
not a claim that every selected input is an ELF executable. Symbolic links,
Windows reparse points, special files, and `.git` directories are excluded.

Each candidate has a SHA-256 **fingerprint**: a digest used to distinguish its
contents from other versions. It is not proof of authenticity. Files are hashed
before and after inspection, and their size and modification time are checked.
Keep inputs and the inspector unchanged during a run. These checks catch many
accidental modifications but do not provide an atomic filesystem snapshot.

`records.jsonl` contains one JSON object per candidate: its relative path, size,
fingerprint, process outcome, and structured inspection result when available.
**These records are sensitive, not anonymized.** JSON Lines allows completed
records to remain readable if a later inspection fails or the run is interrupted.

`summary.json` records the root and inspector paths, inspector and runner
fingerprints, configured policy, times, inventory counts, and aggregate outcomes.
It is also local-only. Missing summary
means the run did not finish. `run_complete: true` means the runner obtained the
expected protocol outcomes for every selected candidate; it does not mean that
every input passed inspection, that those judgments are correct, or that a game
is supported. Excluded/non-candidate files are not covered.

## Interpret results before changing code

| Outcome | Interpretation |
| --- | --- |
| `offline_plan_only` | The restricted offline layout policy accepted it; no execution tested |
| `inspection_rejected` | An implemented check rejected it; distinguish unsupported features from malformed inputs |
| `plan_rejected` | Inspection passed its subset, but the narrower loader policy rejected it |
| `input_limit` | Input exceeds the current 256 MiB cap; not inspected |
| `input_changed` | Input changed during validation; discard this observation and rerun on stable data |
| `host_error` | File access/resource/tool invocation failed; not an input compatibility judgment |
| `inspector_timeout` | Inspector exceeded the per-file time limit; investigate without declaring success |
| `inspector_protocol_error` | Exit code, JSON structure, or output contradicted the expected CLI contract |

Runner exit codes: 0 for a completed outcome collection; 1 for a finished run
containing incomplete/error outcomes; 2 for a setup or runner failure. No candidates
and exceeded file-count limits are failures, not empty green test results.
`--max-files` and `--timeout` can change the scan limit and per-file seconds
(default 15, maximum 300). Execution remains untested in every case.

For an unexpected result, preserve the local record, compare the relevant format
rule against a trustworthy reference, and build a new synthetic case with the
same general structure. A **regression test** checks that this behavior keeps
working after future changes. Do not copy original bytes, names, or data into the
synthetic fixture. Review the implementation and fixture before pushing. Maintain
provenance separately without claiming exclusively public evidence if that is not
true. Normal CI tests exercise the runner only with synthetic temporary inputs.
