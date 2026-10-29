# Validation

The native application supports Robot Framework schema 3/4 and uses RF 6.1.1
as its report semantics reference. Test fixtures committed to this repository
are synthetic. Python, Node and Playwright are test tools; the distributed
native application does not launch them.

## Local checks

```sh
cmake -S cpp --preset desktop
cmake --build build/desktop --parallel 4
ctest --test-dir build/desktop --output-on-failure
uv sync --frozen --group dev
RF_REPORT_CPP="$PWD/build/desktop/rf-report-cpp" \
XML_MERGE_DRIVER="$PWD/build/desktop/xml-merge-driver" \
QT_QPA_PLATFORM=offscreen .venv/bin/python -m pytest \
  tests cpp/tests/test_compat.py cpp/tests/test_app_compat.py -q
```

The suite covers merge selection, full suite subtrees, malformed XML recovery,
Excel export, source-file preservation, advanced report options and decoded
native/Rebot report parity. Qt tests cover selection, context-menu XML export,
source-directory opening and background job lifecycle.

## CI and distribution checks

`native-desktop.yml` builds macOS arm64 and Windows x64 packages, runs native
and Python differential tests, starts the distributed GUI with a restricted
PATH and verifies offline reports in Chromium. `cpp-report.yml` separately
checks the report engine on Linux and Windows. See this repository's Actions
tab for actual run results.

Package automation and offscreen tests do not establish manual desktop click
acceptance. The macOS package is ad-hoc signed and is not notarized. RF 7 schema
is unsupported. Outputs are replaced atomically per file, not as one multi-file
transaction. Large inputs retain DOM/report structures and require separate
resource measurements. Benchmark commands are in `BENCHMARKS.md`.
