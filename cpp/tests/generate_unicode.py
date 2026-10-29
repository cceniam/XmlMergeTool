"""Maintenance-only generator; builds never require Python.

Regenerate with the Python used by the reference tests when updating the Unicode
baseline. Robot normalizes tags with str.lower() and removes Unicode whitespace.
"""
from pathlib import Path
import unicodedata

path = Path(__file__).resolve().parents[1] / 'src/unicode_data.hpp'
lines = [f'// Generated Unicode {unicodedata.unidata_version} lowercase/whitespace data.',
         '// Regenerate using cpp/tests/generate_unicode.py. No runtime Python dependency.',
         '#pragma once', '#include <cstdint>', '#include <string_view>',
         'namespace rf {',
         'struct Lowercase { char32_t code; std::u32string_view value; };',
         'inline constexpr Lowercase lowercase_data[] = {']
for i in range(128, 0x110000):
    c = chr(i)
    if c.lower() != c:
        value = ''.join(f'\\U{ord(ch):08x}' for ch in c.lower())
        lines.append(f'    {{0x{i:x}, U"{value}"}},')
lines += ['};', 'inline constexpr char32_t whitespace_data[] = {']
lines += ['    ' + ', '.join(hex(i) for i in range(0x110000) if chr(i).isspace())]
lines += ['};', '}']
path.write_text('\n'.join(lines) + '\n')
