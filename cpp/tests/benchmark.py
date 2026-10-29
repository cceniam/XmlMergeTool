"""Reproducible macOS acceptance artifacts and phase/memory measurements.

Run: .venv/bin/python cpp/tests/benchmark.py
Python/Node are test tools only; the native executable never launches either.
"""
from __future__ import annotations
import io
import json
from pathlib import Path
import platform
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/cpp/acceptance'
EXE = ROOT / 'build/cpp/rf-report-cpp'


def reference_worker(xml, out):
    from robot.conf import RebotSettings
    from robot.reporting.resultwriter import Results, ResultWriter
    begin = time.perf_counter()
    settings = RebotSettings({'outputdir': str(out), 'loglevel': 'TRACE'})
    results = Results(settings, str(xml))
    result = results.result
    parsed = time.perf_counter()
    rc = ResultWriter(result).write_results(settings)
    end = time.perf_counter()
    print(json.dumps({'parse_ms': (parsed-begin)*1000, 'generate_ms': (end-parsed)*1000,
                      'total_ms': (end-begin)*1000, 'failed_tests': rc}))
    return rc


def measured(command):
    start = time.perf_counter()
    p = subprocess.run(['/usr/bin/time', '-l', *command], capture_output=True, text=True)
    wall = (time.perf_counter()-start)*1000
    if p.returncode >= 252:
        raise RuntimeError(p.stderr)
    memory = re.search(r'(\d+)\s+maximum resident set size', p.stderr)
    result = {'process_wall_ms': wall, 'peak_rss_bytes': int(memory[1]), 'exit_code': p.returncode}
    if 'Timing (ms)' in p.stderr:
        timing = re.search(r'parse=(\S+) generate=(\S+) total=(\S+)', p.stderr)
        result.update(zip(('parse_ms', 'generate_ms', 'total_ms'), map(float, timing.groups())))
    else:
        result.update(json.loads(p.stdout.splitlines()[-1]))
    return result


def main():
    from lxml import etree as ET
    from robot import run
    from robot.rebot import Rebot
    from test_compat import compare
    from xml_merge_tool.application.merge_service import XmlMergeService
    from xml_merge_tool.infrastructure.rf_reader import RFResultReader

    OUT.mkdir(parents=True, exist_ok=True)
    features = OUT / 'features'; features.mkdir(exist_ok=True)
    run(str(ROOT/'cpp/tests/fixtures/features.robot'), outputdir=str(features), log='NONE',
        report='NONE', loglevel='TRACE', stdout=io.StringIO(), stderr=io.StringIO())
    compare(features/'output.xml', features)
    recovered = OUT/'recovered-fixture.xml'
    ET.parse(str(ROOT/'tests/fixtures/test.xml'), ET.XMLParser(recover=True, huge_tree=True)).write(
        str(recovered), encoding='UTF-8', xml_declaration=True)
    candidates = RFResultReader().load_candidates_serial([str(recovered)]).candidates
    merged = OUT/'merged-fixture.xml'
    XmlMergeService()._write_merged_xml(candidates, str(merged))
    measurements = {'platform': platform.platform(), 'python': sys.version,
                    'note': 'Single local run, warm caches; source fixture is truncated and repaired only by the test harness. Times are not performance guarantees.',
                    'samples': []}
    for name, xml in [('features', features/'output.xml'), ('recovered-fixture', recovered), ('merged-fixture', merged)]:
        dest = OUT/name; dest.mkdir(exist_ok=True)
        compare(xml, dest)
        native = measured([str(EXE), str(xml), '--outputdir', str(dest/'native')])
        reference = measured([sys.executable, str(Path(__file__).resolve()), '--worker', str(xml), str(dest/'benchmark-reference')])
        for record, directory in [(native,dest/'native'),(reference,dest/'benchmark-reference')]:
            record['html_bytes'] = {file.name: file.stat().st_size for file in directory.glob('*.html')}
        measurements['samples'].append({'name': name, 'input_bytes': xml.stat().st_size,
                                        'native': native, 'reference': reference})
    (OUT/'benchmark.json').write_text(json.dumps(measurements, ensure_ascii=False, indent=2)+'\n')
    print(json.dumps(measurements, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--worker':
        sys.exit(reference_worker(Path(sys.argv[2]), Path(sys.argv[3])))
    else:
        sys.path.insert(0, str(ROOT/'src'))
        main()
