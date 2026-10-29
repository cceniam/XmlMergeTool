"""Generate TRACE split logs, compare RF models, then exercise both in a browser.

Test-only dependencies: the project's Python dev environment, Node, and the
Playwright package installed with ``npm ci --prefix cpp/tests``. For CI install
the bundled browser with ``node cpp/tests/node_modules/playwright/cli.js install chromium``
and select it using ``--browser-channel chromium`` or PLAYWRIGHT_CHANNEL.
"""
from __future__ import annotations

import argparse
import io
import json
import os
from pathlib import Path
import subprocess
import sys

from robot import run

from test_compat import EXE, ROOT, compare


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, default=EXE,
                        help='Native rf-report-cpp executable (or RF_REPORT_CPP).')
    parser.add_argument('--outputdir', type=Path,
                        default=ROOT / 'build/cpp/acceptance/features-split',
                        help='Directory for generated XML, pages, screenshots, and browser-results.json.')
    parser.add_argument('--browser-channel', default=os.environ.get('PLAYWRIGHT_CHANNEL', 'chrome'),
                        help='Use chromium for Playwright bundled Chromium; default is local chrome.')
    args = parser.parse_args()
    executable = args.executable.resolve()
    if not executable.is_file():
        parser.error(f'Native executable does not exist: {executable}')

    output = args.outputdir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    # A failed rerun must not leave an earlier passing marker behind.
    marker = output / 'browser-results.json'
    marker.unlink(missing_ok=True)
    fixture = Path(__file__).with_name('fixtures') / 'features.robot'
    stdout, stderr = io.StringIO(), io.StringIO()
    rc = run(str(fixture), outputdir=str(output / 'input'), log='NONE', report='NONE',
             loglevel='TRACE', stdout=stdout, stderr=stderr)
    if rc != 3:
        raise RuntimeError(f'Feature fixture returned {rc}, expected 3.\n{stdout.getvalue()}\n{stderr.getvalue()}')

    compare(output / 'input/output.xml', output, level='TRACE', split=True,
            executable=executable)
    environment = dict(os.environ, PLAYWRIGHT_CHANNEL=args.browser_channel)
    result = subprocess.run(['node', str(Path(__file__).with_name('browser.mjs')), str(output)],
                            env=environment, encoding='utf-8', capture_output=True, timeout=180)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if result.returncode:
        return result.returncode
    evidence = json.loads(marker.read_text(encoding='utf-8'))
    if not evidence.get('passed') or len(evidence.get('variants', [])) != 2:
        raise RuntimeError('Browser run did not produce complete passing evidence.')
    if not all(variant['splitFilesLoaded'] > 0 for variant in evidence['variants']):
        raise RuntimeError('Browser run did not load split-log data for both implementations.')
    print(f'Browser artifacts: {output}')
    return 0


if __name__ == '__main__':
    # CI pipes on Windows need explicit UTF-8 even when the console code page
    # differs. The subprocess protocol and all generated files are UTF-8 too.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, 'reconfigure'):
            stream.reconfigure(encoding='utf-8')
    raise SystemExit(main())
