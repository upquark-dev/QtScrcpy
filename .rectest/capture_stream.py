#!/usr/bin/env python3
"""真机抓取 scrcpy 视频流（与 QtScrcpy 相同的 server 启动方式），存成 stream.bin。

包格式与 demuxer 读到的原始流一致：
  8B ptsFlags(大端) + 4B len(大端) + len 字节负载
  - bit63 = SESSION 包（无负载，header[4..8]=w, header[8..12]=h）
  - bit62 = CONFIG 包（pts 无效）
  - bit61 = KEY FRAME
  - 其余低位 = pts 微秒
"""
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

ADB = r"E:\QtScrcpy\output\x64\RelWithDebInfo\adb.exe"
SERVER_LOCAL = r"E:\QtScrcpy\output\x64\RelWithDebInfo\scrcpy-server"
SERVER_REMOTE = "/data/local/tmp/scrcpy_server_rectest"
SCID = 0x12345678
SOCKET_NAME = "scrcpy_%08x" % SCID
PORT = 27183
DURATION_S = 15
OUT = Path(__file__).parent / "stream.bin"

FLAG_SESSION = 1 << 63
FLAG_CONFIG = 1 << 62
FLAG_KEY = 1 << 61
PTS_MASK = FLAG_KEY - 1


def run(*args, **kw):
    print("[adb]", " ".join(args))
    return subprocess.run([ADB, *args], capture_output=True, text=True, **kw)


def main():
    run("push", SERVER_LOCAL, SERVER_REMOTE)
    run("reverse", "localabstract:" + SOCKET_NAME, "tcp:%d" % PORT)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PORT))
    srv.listen(4)

    cmd = [
        "shell",
        "CLASSPATH=%s" % SERVER_REMOTE,
        "app_process", "/", "com.genymobile.scrcpy.Server",
        "4.1", "log_level=info",
        "video_bit_rate=8000000", "max_size=1600", "max_fps=60",
        "tunnel_forward=false", "control=false", "audio=false",
        "scid=%08x" % SCID,
    ]
    print("[adb]", " ".join(cmd))
    proc = subprocess.Popen([ADB, *cmd],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)

    def pump_log():
        for line in proc.stdout:
            line = line.strip()
            if line:
                print("[server]", line)
    threading.Thread(target=pump_log, daemon=True).start()

    try:
        srv.settimeout(20)
        sock, _ = srv.accept()          # 第一条 = video socket
        print("[ok] video socket connected")
        sock.settimeout(10)
        meta = b""
        while len(meta) < 64:           # 64 字节设备名元数据
            chunk = sock.recv(64 - len(meta))
            if not chunk:
                raise RuntimeError("connection closed before meta")
            meta += chunk
        print("[ok] device meta:", meta.rstrip(b"\x00").decode(errors="replace"))
        sock.send(b"\x00")   # send_dummy_byte=true：server 等 1 字节 ack 才开始发流
        print("[ok] dummy ack sent")

        n_bytes = 0
        n_pkts = 0
        n_cfg = 0
        n_key = 0
        session_size = None
        first_pts = None
        last_pts = None
        deadline = time.time() + DURATION_S
        with OUT.open("wb") as f:
            sock.settimeout(5)
            while time.time() < deadline:
                header = b""
                while len(header) < 12:
                    chunk = sock.recv(12 - len(header))
                    if not chunk:
                        raise RuntimeError("video stream closed")
                    header += chunk
                pts_flags, length = struct.unpack(">QI", header)
                if pts_flags & FLAG_SESSION:
                    w, h = struct.unpack(">II", header[4:12])
                    session_size = (w, h)
                    f.write(header)
                    n_pkts += 1
                    n_bytes += 12
                    continue
                data = b""
                while len(data) < length:
                    chunk = sock.recv(length - len(data))
                    if not chunk:
                        raise RuntimeError("video stream closed mid-packet")
                    data += chunk
                f.write(header)
                f.write(data)
                if pts_flags & FLAG_CONFIG:
                    n_cfg += 1
                else:
                    pts = pts_flags & PTS_MASK
                    if first_pts is None:
                        first_pts = pts
                    last_pts = pts
                    if pts_flags & FLAG_KEY:
                        n_key += 1
                n_pkts += 1
                n_bytes += 12 + length
        dur = (last_pts - first_pts) / 1e6 if first_pts and last_pts else 0
        print(f"[done] pkts={n_pkts} bytes={n_bytes} config={n_cfg} key={n_key} "
              f"session={session_size} span={dur:.2f}s -> {OUT}")
        if n_cfg == 0 or n_key < 2 or session_size is None:
            print("[FAIL] stream missing config/key/session packets")
            sys.exit(1)
    finally:
        proc.terminate()
        run("reverse", "--remove", "localabstract:" + SOCKET_NAME)
        srv.close()


if __name__ == "__main__":
    main()
