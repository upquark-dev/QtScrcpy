#!/usr/bin/env python3
"""解析 mp4 box 树，找出结构错误。"""
import struct
import sys


def walk(data, off, end, depth):
    while off + 8 <= end:
        size, typ = struct.unpack(">I4s", data[off:off + 8])
        hdr = 8
        if size == 1:
            size = struct.unpack(">Q", data[off + 8:off + 16])[0]
            hdr = 16
        elif size == 0:
            size = end - off
        print("  " * depth + f"{typ.decode(errors='replace')} size={size} @0x{off:x}")
        if typ in (b"moov", b"trak", b"mdia", b"minf", b"stbl", b"udta", b"meta",
                   b"ilst", b"edts", b"dinf") and depth < 5:
            body = off + hdr
            if typ == b"meta":
                body += 4
            walk(data, body, off + size, depth + 1)
        elif typ == b"stsd":
            # stsd: version/flags(4) + entry_count(4) + entries
            body = off + 8
            entry_count = struct.unpack(">I", data[body + 4:body + 8])[0]
            print("  " * (depth + 1) + f"entry_count={entry_count}")
            p = body + 8
            for _ in range(entry_count):
                esize, etyp = struct.unpack(">I4s", data[p:p + 8])
                print("  " * (depth + 1) + f"{etyp.decode(errors='replace')} size={esize}")
                if etyp == b"avc1":
                    avcC_off = p + 86  # avc1: 8 hdr + 78 body，之后是子 box（avcC）
                    q = avcC_off
                    while q + 8 <= p + esize:
                        qsize, qtyp = struct.unpack(">I4s", data[q:q + 8])
                        print("  " * (depth + 2) + f"{qtyp.decode(errors='replace')} size={qsize} @0x{q:x}")
                        if qtyp == b"avcC":
                            avcc = data[q + 8:q + qsize]
                            if len(avcc) >= 8:
                                cfg = avcc[:8]
                                print("  " * (depth + 3)
                                      + f"avcC: config={cfg.hex()} len={len(avcc)}")
                                profile = cfg[1]
                                print("  " * (depth + 3)
                                      + f"profile_idc={profile} (66=Baseline,77=Main,100=High)")
                        q += qsize if qsize else esize
                p += esize if esize else 8
        off += size
        if size <= 0:
            break


def main(path):
    with open(path, "rb") as f:
        data = f.read()
    print(f"file size={len(data)}")
    walk(data, 0, len(data), 0)


if __name__ == "__main__":
    main(sys.argv[1])
