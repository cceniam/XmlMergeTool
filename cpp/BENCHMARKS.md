# Native application benchmarks

Build the Release desktop targets, then run the deterministic synthetic
benchmark from the repository root:

```sh
.venv/bin/python cpp/tests/benchmark_app.py --large-mib 64 --repeats 3
```

The generator uses 256 tests per leaf suite, one keyword and a unique
approximately 4 KiB INFO message per test, with PASS statuses. The repository
fixture is also synthetic and intentionally truncated to exercise recovery.

The script measures candidate loading, end-to-end merging (XML, conflict CSV,
log and report), and the standalone report CLI. It records wall time, peak RSS,
input/output sizes and SHA-256 hashes. Original inputs and measured binaries
must remain unchanged. macOS uses `/usr/bin/time -l`; Linux uses GNU time.

Results and generated inputs remain in the ignored `build/app-benchmark/`
directory. `latest.json` points to the latest completed measurement. Compare
results obtained on the same machine and input; warm-cache measurements do not
establish cold-start, UI responsiveness or arbitrary-size performance.

`--driver`, `--report` and `--out` override executables and output paths.
`--large-source` accepts a user-provided XML file for local measurement; neither
that input nor its generated reports should be committed.
