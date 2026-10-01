# 对照 verif2 导入表与 DLL 实际导出，找出 ENTRYPOINT_NOT_FOUND 的元凶
import sys
import subprocess
import re
import struct

EXE = sys.argv[1] if len(sys.argv) > 1 else 'verif2.exe'


def exports(path):
    with open(path, 'rb') as f:
        data = f.read()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    opt = pe + 0x18
    magic = struct.unpack_from('<H', data, opt)[0]
    dd = opt + (0x70 if magic == 0x20b else 0x60)
    exp_rva = struct.unpack_from('<I', data, dd)[0]
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    opt_size = struct.unpack_from('<H', data, pe + 20)[0]
    sec = opt + opt_size

    def r2o(rva):
        for i in range(nsec):
            s = sec + i * 40
            vsz = struct.unpack_from('<I', data, s + 8)[0]
            va = struct.unpack_from('<I', data, s + 12)[0]
            rawsz = struct.unpack_from('<I', data, s + 16)[0]
            raw = struct.unpack_from('<I', data, s + 20)[0]
            if va <= rva < va + max(vsz, rawsz):
                return rva - va + raw
        return None

    eo = r2o(exp_rva)
    nname = struct.unpack_from('<I', data, eo + 24)[0]
    names_rva = struct.unpack_from('<I', data, eo + 32)[0]
    no = r2o(names_rva)
    out = set()
    for i in range(nname):
        r = struct.unpack_from('<I', data, no + i * 4)[0]
        fo = r2o(r)
        end = data.index(b'\x00', fo)
        out.add(data[fo:end].decode())
    return out


txt = subprocess.run(['objdump', '-p', EXE], capture_output=True, text=True).stdout
dll = None
imports = {}
for line in txt.splitlines():
    m = re.search(r'DLL Name:\s+(\S+)', line)
    if m:
        dll = m.group(1)
        imports.setdefault(dll, [])
        continue
    # 符号行形如 "  000187a8  <none>  0047  av_new_packet"
    m = re.match(r'\s*[0-9a-f]+\s+<none>\s+[0-9a-f]+\s+(\S+)', line)
    if m and dll:
        imports[dll].append(m.group(1))

for dll, syms in imports.items():
    if not dll.lower().endswith('.dll') or dll.lower().startswith(('kernel', 'user', 'ntdll', 'msvcrt', 'ucrt', 'api-', 'libgcc', 'libstdc')):
        continue
    exported = exports(dll)
    missing = [s for s in syms if s not in exported]
    print(f'{dll}: imports={len(syms)} missing={len(missing)}')
    for s in missing[:20]:
        print('   MISSING:', s)
