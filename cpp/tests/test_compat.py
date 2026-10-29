"""Differential acceptance tests. Python/Rebot and Node are test-only dependencies."""
from __future__ import annotations
import base64
import copy
import io
import json
import os
from pathlib import Path
import subprocess
import zlib

import pytest
from robot.rebot import Rebot

ROOT = Path(__file__).resolve().parents[2]
_exe_name = 'rf-report-cpp.exe' if os.name == 'nt' else 'rf-report-cpp'
_default_exe = ROOT / 'build/cpp' / _exe_name
if os.name == 'nt' and not _default_exe.exists():
    _default_exe = ROOT / 'build/cpp/Release' / _exe_name
EXE = Path(os.environ.get('RF_REPORT_CPP', _default_exe)).resolve()
EXTRACT = Path(__file__).with_name('extract_model.js')
MALFORMED = ROOT / 'tests/fixtures/test.xml'


def model(path):
    data = json.loads(subprocess.check_output(['node', str(EXTRACT), str(path)], encoding='utf-8'))
    output = data['output']
    strings = [s[1:] if s.startswith('*') else zlib.decompress(base64.b64decode(s)).decode('utf-8') for s in output['strings']]
    def string(index): return strings[index]
    def status(value):
        value = list(value)
        if value[1] is not None: value[1] += output['baseMillis']
        if len(value) > 3: value[3] = string(value[3])
        return value
    def message(value):
        value = list(value)
        if value[0] is not None: value[0] += output['baseMillis']
        value[2] = string(value[2])
        if len(value) > 3: value[3] = string(value[3])
        return value
    def item(value):
        if len(value) == 3: return message(value)
        value = list(value)
        for i in range(1, 8): value[i] = string(value[i])
        value[8] = status(value[8]); value[9] = [item(x) for x in value[9]]
        return value
    def test(value):
        value = list(value)
        for i in range(3): value[i] = string(value[i])
        value[3] = [string(t) for t in value[3]]
        value[4] = status(value[4]); value[5] = [item(x) for x in value[5]]
        return value
    def suite(value):
        value = list(value)
        for i in range(4): value[i] = string(value[i])
        value[4] = [string(x) for x in value[4]]
        value[5] = status(value[5]); value[6] = [suite(x) for x in value[6]]
        value[7] = [test(x) for x in value[7]]; value[8] = [item(x) for x in value[8]]
        return value
    output['suite'] = suite(output['suite'])
    if 'errors' in output: output['errors'] = [message(x) for x in output['errors']]
    for key in ('strings', 'generated', 'baseMillis'): output.pop(key)
    return {'output': output, 'settings': data['settings']}


def assert_equal(a, b, path='root'):
    assert type(a) is type(b), f'{path}: {a!r} != {b!r}'
    if isinstance(a, dict):
        assert a.keys() == b.keys(), f'{path}: keys differ'
        for k in a: assert_equal(a[k], b[k], f'{path}/{k}')
    elif isinstance(a, list):
        assert len(a) == len(b), f'{path}: length {len(a)} != {len(b)}'
        for i, (x, y) in enumerate(zip(a, b)): assert_equal(x, y, f'{path}/{i}')
    else:
        assert a == b, f'{path}: {a!r} != {b!r}'


def compare(xml, tmp_path, level='TRACE', log='log.html', report='report.html', logtitle='', reporttitle='',
            remove='', flatten='', split=False, background='', statlevel=None, executable=None):
    native = tmp_path / 'native'; reference = tmp_path / 'reference'
    args = [str(executable or EXE), str(xml), '--outputdir', str(native), '--loglevel', level,
            '--log', log, '--report', report, '--logtitle', logtitle, '--reporttitle', reporttitle]
    extra = {}
    for key, value in [('removekeywords', remove), ('flattenkeywords', flatten),
                       ('reportbackground', background), ('suitestatlevel', statlevel)]:
        if value is not None and value != '':
            args.extend([f'--{key}', str(value)])
            extra[key] = value
    if split:
        args.append('--splitlog')
        extra['splitlog'] = True
    result = subprocess.run(args, encoding='utf-8', capture_output=True, timeout=60)
    rc = Rebot().execute(str(xml), outputdir=str(reference), log=log, report=report,
                         loglevel=level, logtitle=logtitle, reporttitle=reporttitle,
                         stdout=io.StringIO(), stderr=io.StringIO(), **extra)
    assert result.returncode == rc, result.stderr
    assert rc < 252
    for name in (log, report):
        if name != 'NONE':
            actual, expected = model(native / name), model(reference / name)
            assert_equal(actual, expected)
    return native


def symlink_or_skip(link, target):
    try:
        link.symlink_to(target)
    except OSError as error:
        if os.name == 'nt' and error.winerror == 1314:
            pytest.skip('Windows requires Developer Mode or symlink privilege for this case')
        raise


@pytest.fixture(scope='session')
def recovered_fixture(tmp_path_factory):
    # The repository fixture is truncated. Exercise exactly the same repaired input
    # with both engines; the CLI itself intentionally does not repair XML.
    from lxml import etree as ET
    path = tmp_path_factory.mktemp('malformed-input') / 'recovered.xml'
    ET.parse(str(MALFORMED), ET.XMLParser(recover=True, huge_tree=True)).write(str(path), encoding='UTF-8', xml_declaration=True)
    return path


@pytest.mark.parametrize('level', ['TRACE', 'DEBUG', 'INFO', 'WARN', 'NONE', 'TRACE:INFO'])
def test_malformed_fixture(tmp_path, level, recovered_fixture):
    compare(recovered_fixture, tmp_path, level)


def test_only_log_and_custom_names(tmp_path, recovered_fixture):
    compare(recovered_fixture, tmp_path, log='日志 中文.html', report='NONE', logtitle='中文 & <日志>')


def test_only_report(tmp_path, recovered_fixture):
    compare(recovered_fixture, tmp_path, log='NONE', report='report.html', reporttitle='中文报告')

@pytest.fixture(scope='session')
def feature_xml(tmp_path_factory):
    from robot import run
    directory = tmp_path_factory.mktemp('features')
    rc = run(str(Path(__file__).with_name('fixtures') / 'features.robot'), outputdir=str(directory),
             log='NONE', report='NONE', loglevel='TRACE', stdout=io.StringIO(), stderr=io.StringIO())
    assert rc == 3
    return directory / 'output.xml'


@pytest.mark.parametrize('level', ['TRACE', 'DEBUG', 'INFO', 'WARN', 'NONE', 'TRACE:INFO'])
def test_features(tmp_path, feature_xml, level):
    compare(feature_xml, tmp_path, level)


def test_edge_cases(tmp_path):
    compare(Path(__file__).with_name('fixtures') / 'edge_cases.xml', tmp_path)


@pytest.fixture(scope='session')
def advanced_xml(tmp_path_factory):
    from robot import run
    directory = tmp_path_factory.mktemp('advanced')
    rc = run(str(Path(__file__).with_name('fixtures') / 'advanced.robot'), outputdir=str(directory),
             log='NONE', report='NONE', loglevel='TRACE', stdout=io.StringIO(), stderr=io.StringIO())
    assert rc == 1
    return directory / 'output.xml'


@pytest.mark.parametrize('remove', ['', 'passed', 'wuks', 'for', 'while', 'all'])
@pytest.mark.parametrize('flatten', ['', 'for', 'while', 'iteration'])
@pytest.mark.parametrize('split', [False, True])
def test_advanced_transformations(tmp_path, advanced_xml, remove, flatten, split):
    compare(advanced_xml, tmp_path, remove=remove, flatten=flatten, split=split)


@pytest.mark.parametrize('remove', ['passed', 'wuks', 'for', 'while', 'all'])
@pytest.mark.parametrize('flatten', ['for', 'while', 'iteration'])
def test_advanced_report_only(tmp_path, advanced_xml, remove, flatten):
    compare(advanced_xml, tmp_path, log='NONE', remove=remove, flatten=flatten, split=True)
    assert not list((tmp_path / 'native').glob('*.js'))


@pytest.mark.parametrize('level', ['INFO', 'WARN', 'NONE'])
@pytest.mark.parametrize('remove', ['passed', 'for', 'while', 'wuks', 'all'])
def test_removal_precedes_message_filter(tmp_path, advanced_xml, level, remove):
    compare(advanced_xml, tmp_path, level=level, remove=remove, split=True)


@pytest.mark.parametrize('level', ['INFO', 'WARN', 'NONE'])
@pytest.mark.parametrize('flatten', ['for', 'while', 'iteration'])
@pytest.mark.parametrize('remove', ['', 'passed', 'all'])
def test_flattened_messages_and_filter(tmp_path, advanced_xml, level, flatten, remove):
    compare(advanced_xml, tmp_path, level=level, flatten=flatten, remove=remove, split=True)


@pytest.mark.parametrize('remove', ['', 'passed', 'all'])
@pytest.mark.parametrize('flatten', ['', 'for', 'while', 'iteration'])
def test_control_flow_transformations(tmp_path, feature_xml, remove, flatten):
    compare(feature_xml, tmp_path, remove=remove, flatten=flatten, split=True, level='WARN')


def test_passed_suite_fixtures(tmp_path, advanced_xml):
    from xml.etree import ElementTree as ET
    tree = ET.parse(advanced_xml)
    suite = tree.getroot().find('suite')
    for test in suite.findall('test'):
        if test.find('status').get('status') == 'FAIL':
            suite.remove(test)
    source = tmp_path / 'passing.xml'
    tree.write(source, encoding='utf-8', xml_declaration=True)
    compare(source, tmp_path, remove='passed', split=True)


def test_passed_implicit_suite_fixtures(tmp_path):
    source = tmp_path / 'implicit.xml'
    source.write_text('''<robot generator="Robot 6.1.1" schemaversion="4"><suite name="Root">
      <kw name="Implicit Before"><msg level="INFO">setup body</msg><status status="PASS"/></kw>
      <test name="Passing"><kw name="Body"><msg level="INFO">body</msg><status status="PASS"/></kw>
      <status status="PASS"/></test>
      <kw name="Implicit After"><msg level="INFO">teardown body</msg><status status="PASS"/></kw>
      <status status="PASS"/></suite></robot>''')
    compare(source, tmp_path, remove='passed', split=True)


@pytest.mark.parametrize('level', [-1, 0, 1, 2, 3, 99])
@pytest.mark.parametrize('background', ['green:red', '#eaffea:#ffeaea:#fff0cc'])
def test_statistics_and_background(tmp_path, level, background):
    from xml.etree import ElementTree as ET
    tree = ET.parse(Path(__file__).with_name('fixtures') / 'edge_cases.xml')
    root = tree.getroot()
    suite = root.find('suite')
    root.remove(suite)
    outer = ET.Element('suite', name='Outer')
    outer.append(suite)
    root.insert(0, outer)
    source = tmp_path / 'nested-statistics.xml'
    tree.write(source, encoding='utf-8', xml_declaration=True)
    compare(source, tmp_path,
            statlevel=level, background=background, split=True)


def test_split_custom_unicode_names(tmp_path, advanced_xml):
    native = compare(advanced_xml, tmp_path, log='日志/中文 日志.html', report='报告/结果.html', split=True)
    parts = list((native / '日志').glob('中文 日志-*.js'))
    assert len(parts) == 8  # six tests plus suite setup and teardown


def test_split_sidecar_cannot_overwrite_input(tmp_path, feature_xml):
    source = tmp_path / 'log-1.js'
    original = feature_xml.read_bytes()
    source.write_bytes(original)
    result = subprocess.run([str(EXE), str(source), '--outputdir', str(tmp_path), '--splitlog'],
                            capture_output=True)
    assert result.returncode == 252
    assert source.read_bytes() == original
    assert not (tmp_path / 'log.html').exists()


def test_split_output_failure_preserves_html(tmp_path, feature_xml):
    (tmp_path / 'log.html').write_text('existing')
    (tmp_path / 'log-2.js').mkdir()
    result = subprocess.run([str(EXE), str(feature_xml), '--outputdir', str(tmp_path), '--splitlog'],
                            capture_output=True)
    assert result.returncode == 255
    assert (tmp_path / 'log.html').read_text() == 'existing'
    assert not (tmp_path / 'log-1.js').exists()


@pytest.mark.parametrize('alias', ['direct', 'symlink', 'hardlink'])
def test_split_output_collision(tmp_path, feature_xml, alias):
    split = tmp_path / 'log-1.js'
    split.write_text('existing split')
    target = tmp_path / 'report.html'
    if alias == 'direct':
        target = split
    elif alias == 'symlink':
        symlink_or_skip(target, split)
    else:
        os.link(split, target)
    result = subprocess.run([str(EXE), str(feature_xml), '--outputdir', str(tmp_path), '--splitlog',
                             '--report', str(target)], capture_output=True)
    assert result.returncode == 252, result.stderr
    assert split.read_text() == 'existing split'
    assert not (tmp_path / 'log.html').exists()


@pytest.mark.parametrize('log,report,split', [
    ('log.html', 'LOG.HTML', False),
    ('log.html', 'LOG-1.js', True),
    ('caf\u00e9.html', 'cafe\u0301.html', False),
])
def test_new_output_filesystem_aliases(tmp_path, feature_xml, log, report, split):
    # Probe the actual output filesystem: case/normalization aliases must be
    # rejected there, while distinct names remain valid on sensitive volumes.
    probe = tmp_path / 'probe'
    probe.mkdir()
    first = Path(log).stem + '-1.js' if split else log
    (probe / first).write_text('probe')
    aliases = (probe / report).exists()
    (probe / first).unlink()
    probe.rmdir()
    original = feature_xml.read_bytes()
    output = tmp_path / 'out'
    args = [str(EXE), str(feature_xml), '--outputdir', str(output),
            '--log', log, '--report', report]
    if split:
        args.append('--splitlog')
    result = subprocess.run(args, capture_output=True)
    assert feature_xml.read_bytes() == original
    if aliases:
        assert result.returncode == 252, result.stderr
        assert not output.exists() or not list(output.iterdir())
    else:
        assert result.returncode == 3, result.stderr
        assert model(output / log)['output']['suite']
        assert model(output / report)['output']['suite']
        if split:
            subprocess.run(['node', '--check', str(output / first)], check=True,
                           capture_output=True)
        assert not list(output.glob('*.tmp-*'))


@pytest.mark.parametrize('level', ['TRACE', 'INFO', 'WARN', 'NONE', 'TRACE:INFO'])
@pytest.mark.parametrize('log,report', [('log.html', 'report.html'),
                                      ('NONE', 'report.html'), ('log.html', 'NONE')])
def test_schema3(tmp_path, level, log, report):
    # Generated by RF 6.0.2 from features.robot; only the source path is made relative.
    compare(Path(__file__).with_name('fixtures') / 'schema3.xml', tmp_path,
            level=level, log=log, report=report)


def test_schema3_merged_parent(tmp_path):
    from xml.etree import ElementTree as ET
    tree = ET.parse(Path(__file__).with_name('fixtures') / 'schema3.xml')
    root = tree.getroot()
    original = root.find('suite')
    root.remove(original)
    # The merger creates ancestor suites without status or timestamps.
    parent = ET.Element('suite', name='合并父套件')
    parent.append(original)
    root.insert(0, parent)
    xml = tmp_path / 'merged-schema3.xml'
    tree.write(xml, encoding='utf-8', xml_declaration=True)
    compare(xml, tmp_path)


@pytest.mark.parametrize('remove', ['passed', 'wuks', 'for', 'while', 'all'])
@pytest.mark.parametrize('flatten', ['', 'for', 'while', 'iteration'])
def test_schema3_advanced(tmp_path, remove, flatten):
    compare(Path(__file__).with_name('fixtures') / 'schema3.xml', tmp_path,
            remove=remove, flatten=flatten, split=True, level='WARN')


@pytest.mark.parametrize('version', [None, '', '2', '5', '03', 'unknown'])
def test_unsupported_schema(tmp_path, version):
    attr = '' if version is None else f' schemaversion="{version}"'
    xml = tmp_path / 'unsupported.xml'
    xml.write_text(f'<robot{attr}><suite name="Test"><test name="T">'
                   '<status status="PASS"/></test></suite></robot>')
    result = subprocess.run([str(EXE), str(xml), '--outputdir', str(tmp_path)],
                            capture_output=True, encoding='utf-8')
    assert result.returncode == 252
    assert 'supported versions are 3 and 4' in result.stderr
    assert not list(tmp_path.glob('*.html'))


@pytest.mark.parametrize('state', ['FAIL', 'SKIP'])
@pytest.mark.parametrize('log', ['log.html', 'NONE'])
@pytest.mark.parametrize('remove', ['', 'passed', 'all'])
def test_suite_teardown(tmp_path, state, log, remove):
    xml = tmp_path / 'input.xml'
    xml.write_text(f'''<robot generator="Robot 6.1.1" schemaversion="4"><suite name="Root">
      <suite name="Child"><test name="Passed"><kw name="Body"><msg level="INFO">retained</msg>
      <status status="PASS"/></kw><status status="PASS"/></test>
      <test name="Failed"><status status="FAIL">earlier</status></test>
      <test name="Skipped"><status status="SKIP">skip reason</status></test></suite>
      <kw name="Teardown" type="TEARDOWN"><status status="{state}">teardown reason</status></kw>
      </suite><errors/></robot>''', encoding='utf-8')
    compare(xml, tmp_path, log=log, remove=remove, split=True)


@pytest.mark.parametrize('remove', ['', 'all', 'passed', 'for', 'while', 'wuks'])
@pytest.mark.parametrize('teardown', ['', 'FAIL', 'SKIP'])
def test_multiple_root_suites_with_keyword_removal(tmp_path, remove, teardown):
    # The merger can write multiple root suites. RF 6.1.1 accumulates their
    # direct tests and child suites in one result, using the last root's name.
    def test(name, state):
        return f'''<test name="{name}"><kw name="Body"><msg level="INFO">{name} body</msg>
          <status status="PASS"/></kw><status status="{state}">{name} result</status></test>'''
    fixture = (f'''<kw name="Teardown" type="TEARDOWN"><status status="{teardown}">
      teardown reason</status></kw>''' if teardown else '')
    source = tmp_path / 'multiple-roots.xml'
    source.write_text(f'''<robot generator="Robot 6.1.1" schemaversion="4">
      <suite name="RootA">{test('DirectA', 'FAIL')}
        <suite name="ChildA">{test('NestedA', 'PASS')}</suite></suite>
      <suite name="RootB">{test('DirectB', 'PASS')}
        <suite name="ChildB">{test('NestedB', 'FAIL')}</suite>{fixture}</suite>
      <errors/></robot>''', encoding='utf-8')
    compare(source, tmp_path, remove=remove, split=True)


@pytest.mark.parametrize('metadata_first', [False, True])
def test_report_ignores_namespaced_attributes(tmp_path, metadata_first):
    def attrs(key, value, metadata):
        values = [f'{key}="{value}"', f'meta:{key}="{metadata}"']
        return ' '.join(reversed(values) if metadata_first else values)
    source = tmp_path / 'namespaced.xml'
    source.write_text(f'''<robot xmlns:meta="urn:metadata" generator="Robot 6.1.1" schemaversion="4">
      <suite {attrs('name', 'Suite', 'Other suite')}>
        <test {attrs('name', 'Failed test', 'Other test')}>
          <status {attrs('status', 'FAIL', 'PASS')}>Failure</status>
        </test><status status="FAIL"/>
      </suite><errors/></robot>''', encoding='utf-8')
    compare(source, tmp_path)


def test_application_merge(tmp_path, feature_xml):
    from xml_merge_tool.application.merge_service import XmlMergeService
    from xml_merge_tool.infrastructure.rf_reader import RFResultReader
    service = XmlMergeService()
    candidates = RFResultReader().load_candidates_serial([str(feature_xml)]).candidates
    xml = tmp_path / 'merged.xml'
    service._write_merged_xml(candidates, str(xml))
    compare(xml, tmp_path)


def test_different_output_directories(tmp_path, feature_xml):
    compare(feature_xml, tmp_path, log='日志/log.html', report='报告/report.html')


def test_unicode_input_path(tmp_path, feature_xml):
    source = tmp_path / '输入目录 中文' / '运行结果.xml'
    source.parent.mkdir()
    source.write_bytes(feature_xml.read_bytes())
    compare(source, tmp_path / '输出目录 中文', log='日志.html', report='报告.html', split=True)


@pytest.mark.skipif(os.name != 'nt', reason='Windows drive-letter paths')
@pytest.mark.parametrize('split', [False, True])
def test_windows_cross_drive_source(tmp_path, split):
    import ctypes
    import xml.etree.ElementTree as ET
    # SUBST supplies a second drive even on Windows hosts with only one volume.
    # Always remove this test-owned mapping, including when comparison fails.
    mask = ctypes.windll.kernel32.GetLogicalDrives()
    drive = next((f'{letter}:' for i, letter in reversed(list(enumerate('ABCDEFGHIJKLMNOPQRSTUVWXYZ')))
                  if i > 2 and not mask & (1 << i)), None)
    assert drive is not None, 'No unused drive letter for the cross-drive regression'
    mapped = tmp_path / '映射目录 中文'
    mapped.mkdir()
    subprocess.run(['subst', drive, str(mapped)], check=True, capture_output=True)
    try:
        source = Path(drive + '/') / '源用例 & #%.robot'
        source.write_text('*** Test Cases ***\nCase\n    No Operation\n', encoding='utf-8')
        root = ET.Element('robot', schemaversion='4', generator='Robot 6.1.1')
        suite = ET.SubElement(root, 'suite', name='Cross drive', source=str(source))
        test = ET.SubElement(suite, 'test', name='Case')
        ET.SubElement(test, 'status', status='PASS')
        xml = tmp_path / 'input.xml'
        ET.ElementTree(root).write(xml, encoding='utf-8', xml_declaration=True)
        native = compare(xml, tmp_path / '报告目录 中文', split=split)
        source_link = model(native / 'log.html')['output']['suite'][2]
        assert source_link.startswith(f'<a href="file:///{drive}/')
        assert '%E6%BA%90' in source_link and '%23%25.robot' in source_link
    finally:
        subprocess.run(['subst', drive, '/D'], check=True, capture_output=True)


def test_rpa(tmp_path, feature_xml):
    xml = tmp_path / 'rpa.xml'
    xml.write_text(feature_xml.read_text(encoding='utf-8').replace('rpa="false"', 'rpa="true"'), encoding='utf-8')
    compare(xml, tmp_path)


def test_native_without_python_or_resources(tmp_path, feature_xml):
    import shutil
    exe = tmp_path / ('独立程序 中文.exe' if os.name == 'nt' else '独立程序 中文')
    shutil.copy2(EXE, exe)
    environment = {'PATH': '', 'LANG': 'en_US.UTF-8'}
    if os.name == 'nt':
        # The Windows CLI links native dependencies dynamically. Keep its local
        # DLLs beside the copied executable, as in the release build directory.
        for dependency in EXE.parent.iterdir():
            if dependency.is_file() and dependency.suffix.lower() == '.dll':
                shutil.copy2(dependency, tmp_path / dependency.name)
        # Windows process/system DLL initialization needs these variables;
        # PATH remains empty and no Python runtime/resources are supplied.
        environment.update({key: os.environ[key] for key in ('SystemRoot', 'WINDIR', 'TEMP', 'TMP')
                            if key in os.environ})
    result = subprocess.run([str(exe), str(feature_xml), '--outputdir', str(tmp_path / 'out')],
                            cwd=tmp_path, env=environment, capture_output=True)
    assert result.returncode == 3, result.stderr
    assert (tmp_path / 'out/log.html').stat().st_size > 10000


@pytest.mark.parametrize('args', [ ['--removekeywords','INVALID'], ['--flattenkeywords','INVALID'],
    ['--reportbackground','red'], ['--suitestatlevel','many'], ['--suitestatlevel','3tail'],
    ['--unknown','x'], ['--loglevel','INVALID'], ['--loglevel','TRACE:BOGUS'], ['--loglevel','TRACE:INFO:WARN'],
    ['--log',''], ['--report','same.html','--log','same.html'] ])
def test_bad_options(tmp_path, feature_xml, args):
    result = subprocess.run([str(EXE), str(feature_xml), '--outputdir', str(tmp_path), *args], capture_output=True)
    assert result.returncode == 252, result.stderr
    assert not list(tmp_path.glob('*.html'))


@pytest.mark.parametrize('content', ['<robot>', '<wrong/>', '<robot schemaversion="5"/>',
    '<robot schemaversion="4"><suite name="Empty"/></robot>',
    '<!DOCTYPE robot [<!ENTITY x "test">]><robot schemaversion="4"><suite name="&x;"/></robot>'])
def test_bad_xml(tmp_path, content):
    xml = tmp_path / 'bad.xml'; xml.write_text(content)
    result = subprocess.run([str(EXE),str(xml),'--outputdir',str(tmp_path)],capture_output=True)
    assert result.returncode == 252, result.stderr
    assert not list(tmp_path.glob('*.html'))


def test_original_truncated_fixture(tmp_path):
    result = subprocess.run([str(EXE),str(MALFORMED),'--outputdir',str(tmp_path)],capture_output=True)
    assert result.returncode == 252


@pytest.mark.parametrize('alias', ['direct', 'symlink', 'hardlink'])
def test_input_cannot_be_overwritten(tmp_path, feature_xml, alias):
    xml = tmp_path / 'input.xml'; original = feature_xml.read_bytes(); xml.write_bytes(original)
    target = xml
    if alias != 'direct':
        target = tmp_path / 'alias.html'
        if alias == 'symlink': symlink_or_skip(target, xml)
        else: os.link(xml, target)
    result = subprocess.run([str(EXE),str(xml),'--log',str(target),'--report','NONE'],capture_output=True)
    assert result.returncode == 252
    assert xml.read_bytes() == original


def test_output_failure_preserves_existing(tmp_path, feature_xml):
    target = tmp_path / 'log.html'; target.write_text('old report')
    blocker = tmp_path / 'not-a-directory'; blocker.write_text('file')
    result = subprocess.run([str(EXE),str(feature_xml),'--outputdir',str(tmp_path),
                             '--report','not-a-directory/report.html'],capture_output=True)
    assert result.returncode == 255
    assert target.read_text() == 'old report'
    assert not list(tmp_path.rglob('*.tmp-*'))


def test_unicode_normalization(tmp_path):
    xml=tmp_path/'unicode.xml'
    xml.write_text('''<robot generator="Robot 6.1.1" schemaversion="4"><suite name="Unicode">
    <meta name="a_b">underscore</meta><meta name="a b">space</meta>
    <test name="中文"><tag>ÄBC</tag><tag>äbc</tag><tag>A　B</tag><tag>ab</tag><tag>İ</tag><tag>i̇</tag>
    <tag>z</tag><tag>á</tag><status status="PASS"/></test>
    <test name="second"><tag>äBC</tag><status status="PASS"/></test>
    </suite></robot>''',encoding='utf-8')
    compare(xml,tmp_path)


def test_large_string_table_and_nested_body(tmp_path):
    import xml.etree.ElementTree as ET
    root = ET.Element('robot', schemaversion='4', generator='Robot 6.1.1')
    suite = ET.SubElement(root, 'suite', name='Large')
    test = ET.SubElement(suite, 'test', name='Many messages')
    parent = test
    for i in range(80):
        parent = ET.SubElement(parent, 'kw', name=f'Nested {i}')
        ET.SubElement(parent, 'status', status='PASS', starttime='20260101 00:00:00.000', endtime='20260101 00:00:01.000')
    for i in range(10000):
        ET.SubElement(parent, 'msg', level='INFO').text = f'中文消息 {i}: </script> & ' + ('compress repeated ' * 20)
    ET.SubElement(test, 'status', status='PASS')
    xml=tmp_path/'large.xml'; ET.ElementTree(root).write(xml,encoding='UTF-8')
    compare(xml,tmp_path)


def test_exit_code_cap(tmp_path):
    xml=tmp_path/'many-failures.xml'
    xml.write_text('<robot schemaversion="4"><suite name="Failures">'+
                   ''.join(f'<test name="Test {i}"><status status="FAIL"/></test>' for i in range(260))+
                   '</suite></robot>')
    compare(xml,tmp_path)


def test_missing_input(tmp_path):
    result=subprocess.run([str(EXE),str(tmp_path/'missing.xml')],capture_output=True)
    assert result.returncode == 252


def test_output_is_directory(tmp_path,feature_xml):
    target=tmp_path/'report.html';target.mkdir()
    old=tmp_path/'log.html';old.write_text('old')
    result=subprocess.run([str(EXE),str(feature_xml),'--outputdir',str(tmp_path)],capture_output=True)
    assert result.returncode == 255
    assert old.read_text() == 'old'


def test_source_path_and_direct_test_message(tmp_path):
    xml=tmp_path/'paths.xml'
    xml.write_text('''<robot schemaversion="4" generator="Robot 6.1.1"><suite name="Paths" source="./a/../b.robot">
    <test name="Direct message"><msg timestamp="20260101 00:00:00.000" level="INFO">test message</msg>
    <kw name="K"><msg timestamp="20260101 00:00:00.001" level="INFO">keyword message</msg><status status="PASS"/></kw>
    <status status="PASS"/></test></suite></robot>''')
    compare(xml,tmp_path,level='WARN')


def formatting_xml(path, texts):
    import xml.etree.ElementTree as ET
    root = ET.Element('robot', schemaversion='4', generator='Robot 6.1.1')
    suite = ET.SubElement(root, 'suite', name='Formatting')
    for i, text in enumerate(texts):
        test = ET.SubElement(suite, 'test', name=f'Formatting {i}')
        ET.SubElement(test, 'doc').text = text
        kw = ET.SubElement(test, 'kw', name='Log')
        ET.SubElement(kw, 'msg', level='INFO').text = text
        ET.SubElement(kw, 'status', status='PASS')
        ET.SubElement(test, 'status', status='PASS')
    ET.ElementTree(root).write(path, encoding='UTF-8')
    return path


def test_long_formatting(tmp_path):
    text = 'x' * 100000
    # Exercise the complete XML -> model -> HTML path against RF 6.1.1.
    compare(formatting_xml(tmp_path / 'long.xml', [
        'https://example.test/' + text,
        '*' + text + '*', '_' + text + '_', '``' + text + '``',
        '[data:image/png;base64,' + text + '|image]',
        '[https://example.test/' + text + '|label]',
        '= ' + text + ' =', '| ' + text + ' |', '-' * 100000,
    ]), tmp_path)


def test_formatting_boundaries(tmp_path):
    import random
    rng = random.Random(611)
    texts = [
        'https://host/a). https://host/b, ftp://host/c! custom+scheme://host/path',
        '(https://host/a) [https://host/b] {https://host/c}',
        'https://host/|tail https://host/|| https://host/?!.',
        'https://host/a\ttext\nhttps://host/b',
        'prefixhttps://host/a https:// https://| x://.',
        '*bold* _italic_ ``code`` _*both*_ "(*bold*)"',
        '*one*tail *two* ** *** _two_words_ ``a`b``',
        '[target|] [|label] [|x|] [a]b|c] [[nested|label]]',
        '[a.png|image] [target|b.svg] [target|label|extra]',
        '=== Heading ===', '=  Heading  =', '==== Not heading ====',
        '| |', '| a | b |', '| no closing bar', '---', '--',
    ]
    tokens = ['x', ' ', '  ', '*', '_', '``', '"', "'", '(', ')', '[', ']',
              '{', '}', '|', '.', ',', '!', '?', ':', ';', '=', '-', '\t',
              'https://host/a', 'image.png', '中文']
    texts += [''.join(rng.choices(tokens, k=rng.randint(1, 35))) for _ in range(400)]
    compare(formatting_xml(tmp_path / 'formatting.xml', texts), tmp_path)


@pytest.mark.parametrize('zone', ['UTC', 'Asia/Shanghai', 'America/New_York'])
@pytest.mark.parametrize('stamp', [
    '20260308 01:30:00.000', '20260308 02:30:00.000', '20260308 03:30:00.000',
    '20261101 00:30:00.000', '20261101 01:30:00.000', '20261101 02:30:00.000',
])
def test_timezone_compatibility(tmp_path, monkeypatch, zone, stamp):
    import time
    if not hasattr(time, 'tzset'):
        pytest.skip('Named TZ switching requires time.tzset')
    xml = tmp_path / 'time.xml'
    xml.write_text(f'''<robot schemaversion="4" generator="Robot 6.1.1">
      <suite name="Time"><test name="DST"><kw name="Log">
      <msg timestamp="{stamp}" level="INFO">message</msg>
      <status status="PASS" starttime="{stamp}" endtime="{stamp}"/></kw>
      <status status="PASS" starttime="{stamp}" endtime="{stamp}"/></test>
      <status status="PASS" starttime="{stamp}" endtime="{stamp}"/>
      </suite></robot>''', encoding='utf-8')
    try:
        with monkeypatch.context() as env:
            env.setenv('TZ', zone)
            time.tzset()
            compare(xml, tmp_path)
    finally:
        time.tzset()


def timestamp_xml(path, stamp):
    path.write_text(f'''<robot schemaversion="4" generator="Robot 6.1.1">
      <suite name="Time"><test name="Range"><kw name="Log">
      <msg timestamp="{stamp}" level="INFO">message</msg>
      <status status="PASS" starttime="{stamp}" endtime="{stamp}"/></kw>
      <status status="PASS" starttime="{stamp}" endtime="{stamp}"/></test>
      <status status="PASS" starttime="{stamp}" endtime="{stamp}"/>
      </suite></robot>''', encoding='utf-8')
    return path


@pytest.mark.parametrize('stamp', [
    '19000101 00:00:00.000', '19691231 23:59:59.000',
    '30010101 00:00:00.000', '99991231 23:59:59.000',
])
def test_platform_timestamp_range(tmp_path, stamp):
    from robot.utils.robottime import timestamp_to_secs
    xml = timestamp_xml(tmp_path / 'range.xml', stamp)
    try:
        timestamp_to_secs(stamp)
    except ValueError:
        out = tmp_path / 'native'
        result = subprocess.run([str(EXE), str(xml), '--outputdir', str(out)],
                                capture_output=True, encoding='utf-8')
        assert result.returncode == 252, result.stderr
        assert 'outside the platform time range' in result.stderr
        assert not out.exists()
    else:
        compare(xml, tmp_path)


def test_valid_minus_one_timestamp(tmp_path):
    import time
    from robot.utils.robottime import timestamp_to_secs
    try:
        stamp = time.strftime('%Y%m%d %H:%M:%S.000', time.localtime(-1))
        seconds = timestamp_to_secs(stamp)
    except (OverflowError, OSError, ValueError):
        pytest.skip('The platform does not support timestamps before the epoch')
    assert seconds == -1
    compare(timestamp_xml(tmp_path / 'minus-one.xml', stamp), tmp_path)
