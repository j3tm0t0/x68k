#!/usr/bin/env python3
"""sendtest.x の相手。TCP で受けた並びを照合し、結果を表示して 1 バイト返して閉じる。
usage: sendsink.py [PORT=5099] [COUNT=1] [CAP]   (COUNT 回の接続を受けて終わる。CAP バイトを超えたら打ち切って照合
  (Z が同じデータを送り直し続けるとき、Z の送信を RST で止める))
最初の食い違いの位置と前後、欠け/重複らしさ (期待列の中で受信 16 バイトが見つかる位置のずれ) を出す。"""
import socket, sys, time

def pat(i):
    return (((i * 2654435761) & 0xffffffff) >> 24 ^ (i >> 16)) & 0xff

port = int(sys.argv[1]) if len(sys.argv) > 1 else 5099
count = int(sys.argv[2]) if len(sys.argv) > 2 else 1
cap = int(sys.argv[3]) if len(sys.argv) > 3 else 1 << 30
chat = float(sys.argv[4]) if len(sys.argv) > 4 else 0   # >0: この秒ごとに 52 バイト送り返す (Z は rx で読む)
ls = socket.socket()
ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
ls.bind(("0.0.0.0", port))
ls.listen(1)
print(f"sendsink: listening on {port}", flush=True)
for _ in range(count):
    c, a = ls.accept()
    c.settimeout(60)
    t0 = time.time()
    data = bytearray()
    last_chat = time.time()
    if chat > 0:
        c.settimeout(chat)
    try:
        while True:
            if chat > 0 and time.time() - last_chat >= chat:
                c.sendall(bytes(52)); last_chat = time.time()
            try:
                b = c.recv(65536)
            except socket.timeout:
                if chat > 0 and time.time() - t0 < 600 and (not data or time.time() - last_rx < 5):
                    continue
                raise
            last_rx = time.time()
            if not b:
                break
            data += b
            if len(data) > cap:
                print(f"CAP: more than {cap} bytes, stopping", flush=True)
                break
            # 3 秒何も来なければ終わりとみなす (Z は相手の返事を待っている)
            if chat == 0:
                c.settimeout(3)
    except socket.timeout:
        pass
    el = time.time() - t0
    exp = bytes(pat(i) for i in range(len(data)))
    bad = next((i for i in range(len(data)) if data[i] != exp[i]), None)
    if bad is None:
        print(f"OK {len(data)} bytes from {a[0]} in {el:.1f}s", flush=True)
    else:
        win = bytes(data[bad:bad + 16])
        # 受信した 16 バイトが期待列のどこにあるか (欠けなら先、重複なら後ろ)
        full = bytes(pat(i) for i in range(len(data) + 200000))
        where = full.find(win, max(0, bad - 200000))
        print(f"MISMATCH at {bad} of {len(data)} bytes (from {a[0]}, {el:.1f}s): got {win.hex()} expected {exp[bad:bad+16].hex()}"
              f" -> received bytes match expected stream at {where} (shift {where - bad if where >= 0 else 'n/a'})", flush=True)
        # 2 つ目以降の食い違いの区間もいくつか
        segs = []; i = bad
        while i < len(data) and len(segs) < 8:
            j = i
            while j < len(data) and data[j] != exp[j]:
                j += 1
            segs.append((i, j - i))
            k = j
            while k < len(data) and data[k] == exp[k]:
                k += 1
            i = k
        print("  bad runs (offset,len):", segs, flush=True)
    try:
        c.sendall(b"!")
    except OSError:
        pass
    c.close()
