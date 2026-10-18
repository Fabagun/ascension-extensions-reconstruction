"""The 0x5BC formatter check (AscHotPatch.cpp FormatterAllowed) against every shipped formatter and against
the Lua that servers smuggled through ChallengeRequirementTypes.

    python tests/hot_patch_formatter.py [--dbc path\\to\\ChallengeRequirementTypes.dbc]

The shipped formatters come from the client's ChallengeRequirementTypes.dbc: columns 38-40 there (name and
description are 17 localised columns each), fields 6-8 of the 9-field SMSG 0x5BC row. Every one must pass; every smuggling shape must be refused.
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'src/Ascension/AscHotPatch.cpp'
DEFAULT_DBC = Path(r'E:\oniwow-artifacts\dbc-ascension-full\DBFilesClient\ChallengeRequirementTypes.dbc')


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    index = opening + 1
    while depth:
        depth += (source[index] == '{') - (source[index] == '}')
        index += 1
    return source[start:index]


def shipped_formatters(path):
    data = path.read_bytes()
    count, fields, size, _ = struct.unpack_from('<4I', data, 4)
    strings = data[20 + count * size:]
    found = []
    for row in range(count):
        values = struct.unpack_from(f'<{fields}I', data, 20 + row * size)
        for field in (38, 39, 40):
            offset = values[field]
            if 0 < offset < len(strings):
                text = strings[offset:strings.index(b'\0', offset)].decode('utf-8')
                if text:
                    found.append(text)
    return found


SMUGGLED = [
    'end, (function() print(1) end)(), function(value)',
    'end, (function()\nAscensionClientLuaTokens = {}\nend)(), function(value)',
    'return value end, (function() X = 1 end)(), function(value) return value',
    'return SendChatMessage("hi", "SAY")',
    'return _G["SendChatMessage"]("hi")',
    'return getfenv(0).RunScript("x")',
    'return RunScript("x")',
    'return loadstring("x")()',
    'return value; X = 1',
    'return (function() return 1 end)()',
    'return value -- comment',
    'return [[long string]]',
    'return C_Quest.GetTitleByID(value):upper()',
    'return {value}',
    'return value == 1 and X',
    'X = value',
    'return SecondsToTime(value',
    'return "unterminated',
    'return',
    'value',
]

HARNESS = r'''
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// FORMATTER_ALLOWED

int main(int argc, char** argv)
{
    std::ifstream in(argv[1], std::ios::binary);
    std::stringstream all;
    all << in.rdbuf();
    std::string blob = all.str();
    int failures = 0;
    size_t pos = 0;
    while (pos < blob.size())
    {
        const char expect = blob[pos];
        const size_t end = blob.find('\x1e', pos);
        const std::string text = blob.substr(pos + 1, end - pos - 1);
        pos = end + 1;
        std::string reason;
        const bool allowed = FormatterAllowed(text, reason);
        if (allowed != (expect == '+'))
        {
            ++failures;
            std::cout << "FAIL " << (expect == '+' ? "refused shipped" : "allowed smuggled") << ": " << text
                      << " (" << reason << ")\n";
        }
    }
    std::cout << (failures ? "FAILED" : "OK") << "\n";
    return failures ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--dbc', type=Path, default=DEFAULT_DBC)
    args = parser.parse_args()

    shipped = shipped_formatters(args.dbc)
    assert len(shipped) >= 20, f'expected the shipped formatters, found {len(shipped)}'
    code = function(SOURCE.read_text(encoding='utf-8'), 'bool FormatterAllowed(')
    harness = HARNESS.replace('// FORMATTER_ALLOWED', code)

    compiler = shutil.which(os.environ.get('CXX', 'cl.exe' if os.name == 'nt' else 'c++'))
    if not compiler:
        raise SystemExit('A C++20 compiler is required.')
    with tempfile.TemporaryDirectory(prefix='hot-patch-formatter-') as directory:
        out = Path(directory)
        cpp = out / 'harness.cpp'
        exe = out / ('harness.exe' if os.name == 'nt' else 'harness')
        cpp.write_text(harness, encoding='utf-8')
        cases = out / 'cases.bin'
        cases.write_bytes(b''.join(
            [b'+' + text.encode('utf-8') + b'\x1e' for text in shipped] +
            [b'-' + text.encode('utf-8') + b'\x1e' for text in SMUGGLED]))
        flags = (['/nologo', '/std:c++20', '/EHsc', str(cpp), '/Fe' + str(exe)]
                 if Path(compiler).stem.lower() == 'cl' else ['-std=c++20', str(cpp), '-o', str(exe)])
        subprocess.run([compiler, *flags], cwd=out, check=True)
        result = subprocess.run([str(exe), str(cases)], cwd=out)
    print(json.dumps({'shipped': len(shipped), 'smuggled': len(SMUGGLED)}))
    raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
