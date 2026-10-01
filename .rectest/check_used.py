# 对照 rectest 用到的 FFmpeg 符号与三个 DLL 的导出
import struct

DLLS = ['avutil-56.dll', 'avcodec-58.dll', 'avformat-58.dll']
USED = ['av_frame_alloc', 'av_frame_free', 'av_frame_unref', 'av_malloc',
        'av_new_packet', 'av_packet_alloc', 'av_packet_free',
        'avcodec_find_decoder', 'avcodec_alloc_context3', 'avcodec_open2',
        'avcodec_send_packet', 'avcodec_receive_frame', 'avcodec_free_context']


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


available = set()
for dll in DLLS:
    try:
        available |= exports('E:/QtScrcpy/output/x64/RelWithDebInfo/' + dll)
    except FileNotFoundError:
        print(dll, 'not found')
for s in USED:
    print(s, '->', 'OK' if s in available else 'MISSING')
