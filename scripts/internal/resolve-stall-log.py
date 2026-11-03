#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Thanos Athanasopoulos
# Adds function names to the "GUI stall" stacks of an OPSREMOTE_LOG file.
# Frames are logged as module+offset; this resolves those of the OpsRemote
# executable with its symbol table (nm, demangled), for Windows builds
# without debug information as well as Linux ones, and those of the Windows
# bundle's DLLs (Qt, libssh, FreeRDP, ...) with their export tables: the
# nearest exported function, so an internal one shows as the export before it.
#
#   resolve-stall-log.py LOG [EXECUTABLE]
#
# EXECUTABLE defaults to build/windows/opsremote.exe for opsremote.exe
# frames and build/linux/opsremote for opsremote frames; DLLs come from
# build/stage/OpsRemote-windows-x86_64, then from the folders listed in
# STALL_DLL_DIRS (':'-separated; e.g. Windows' own DLLs copied from the
# machine that wrote the log). All must be the versions that wrote the log.
import bisect
import os
import re
import subprocess
import sys
from pathlib import Path

project = Path(__file__).resolve().parents[2]


def symbols(executable):
    output = subprocess.run(['nm', '-C', '--defined-only', str(executable)],
                            capture_output=True, text=True, check=True).stdout
    table = []
    for line in output.splitlines():
        parts = line.split(' ', 2)
        if len(parts) == 3 and parts[1].lower() == 't':
            table.append((int(parts[0], 16), parts[2]))
    table.sort()
    return table


def exports(dll):
    # objdump -p lists the export addresses and the names by ordinal.
    output = subprocess.run(['objdump', '-p', str(dll)], capture_output=True, text=True).stdout
    addresses, names, section = {}, {}, None
    for line in output.splitlines():
        if line.startswith('Export Address Table --'):
            section = 'addresses'
        elif line.startswith('[Ordinal/Name Pointer] Table'):
            section = 'names'
        elif section and not line.startswith('\t'):
            section = None
        elif section == 'addresses':
            match = re.match(r'\s+\[\s*\d+\] \+base\[\s*(\d+)\] ([0-9a-f]+) Export RVA', line)
            if match:
                addresses[int(match.group(1))] = int(match.group(2), 16)
        elif section == 'names':
            match = re.match(r'\s+\[\s*\d+\] \+base\[\s*(\d+)\]\s+[0-9a-f]+ (\S+)', line)
            if match:
                names[int(match.group(1))] = match.group(2)
    mangled = [(addresses[ordinal], name) for ordinal, name in names.items() if ordinal in addresses]
    demangled = subprocess.run(['c++filt'], input='\n'.join(name for _, name in mangled),
                               capture_output=True, text=True).stdout.splitlines()
    if len(demangled) == len(mangled):
        mangled = [(address, name) for (address, _), name in zip(mangled, demangled)]
    return sorted(mangled)


def image_base(executable):
    # PE files: addresses in nm are ImageBase + offset. ELF (PIE): offset itself.
    output = subprocess.run(['objdump', '-p', str(executable)], capture_output=True, text=True).stdout
    match = re.search(r'ImageBase\s+([0-9a-fA-F]+)', output)
    return int(match.group(1), 16) if match else 0


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__ if __doc__ else 'usage: resolve-stall-log.py LOG [EXECUTABLE]')
    log = Path(sys.argv[1])
    tables = {}

    def lookup(module, offset):
        if module not in tables and module.endswith('.dll'):
            folders = [project / 'build/stage/OpsRemote-windows-x86_64']
            folders += [Path(folder) for folder in os.environ.get('STALL_DLL_DIRS', '').split(':') if folder]
            matches = [path for folder in folders for path in folder.rglob('*') if path.name.lower() == module]
            table = exports(matches[0]) if matches else []
            tables[module] = (table, [address for address, _ in table], 0)
        if module not in tables:
            if len(sys.argv) > 2:
                executable = Path(sys.argv[2])
            elif module.endswith('.exe'):
                executable = project / 'build/windows/opsremote.exe'
            else:
                executable = project / 'build/linux/opsremote'
            table = symbols(executable)
            tables[module] = (table, [address for address, _ in table], image_base(executable))
        table, addresses, base = tables[module]
        index = bisect.bisect_right(addresses, base + offset) - 1
        if index < 0:
            return None
        address, name = table[index]
        return f'{name}+0x{base + offset - address:x}'

    frame = re.compile(r'^(\s+#\d+ )(opsremote(?:\.exe)?|[\w.+-]+\.dll)\+0x([0-9a-f]+)\s*$', re.IGNORECASE)
    for line in log.read_text(errors='replace').splitlines():
        match = frame.match(line)
        if match:
            name = lookup(match.group(2).lower(), int(match.group(3), 16))
            if name:
                line = f'{match.group(1)}{name} ({match.group(2)})'
        print(line)


main()
