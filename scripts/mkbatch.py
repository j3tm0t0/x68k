#!/usr/bin/env python3
"""stage/ ディレクトリを rb-cli batch 用 JSON に変換する。
usage: mkbatch.py STAGE_DIR IMAGE > batch.json
"""
import json, os, sys

stage, image = sys.argv[1], sys.argv[2]
# 3 番目の引数: `rb-cli ls IMG '/**'` の出力。既存ディレクトリの mkdir を省く
existing = set()
if len(sys.argv) > 3:
    for line in open(sys.argv[3]):
        parts = line.split()
        if len(parts) >= 3 and parts[0] == "DIR":
            existing.add(parts[-1])
ops = []
bad = []
renamed = []
for root, dirs, files in os.walk(stage):
    dirs.sort(); files.sort()
    rel = os.path.relpath(root, stage)
    if len(dirs) + len(files) > 250:   # 1 クラスタ (8KB) に収まるエントリ数を超えると rb-cli が壊す
        sys.stderr.write("mkbatch: WARNING %s has %d entries (>250)\n" % (rel, len(dirs) + len(files)))
    if rel != ".":
        d = "/" + rel.replace(os.sep, "/")
        if d not in existing:
            ops.append({"op": "mkdir", "path": d})
    for f in files:
        if f == ".DS_Store":
            continue
        base, ext = (f.rsplit(".", 1) + [""])[:2] if "." in f else (f, "")
        # Human68k: 基本名 <=18 bytes, 拡張子 <=3 (TwentyOne 前提)
        if len(base.encode("shift_jis", "replace")) > 18 or len(base) == 0:
            bad.append(os.path.join(rel, f)); continue
        name = f
        if len(ext) > 3:  # 拡張子は 3 バイトまで: README.HUMAN -> README_HUMAN
            name = base + "_" + ext
            renamed.append((os.path.join(rel, f), name))
        dst = "/" + (name if rel == "." else rel.replace(os.sep, "/") + "/" + name)
        ops.append({"op": "put", "src": os.path.join(root, f), "dst": dst, "force": True})
for src, dst in renamed:
    sys.stderr.write("mkbatch: renamed %s -> %s (extension > 3 chars)\n" % (src, dst))
if bad:
    sys.stderr.write("mkbatch: skipped (name too long / dotfile): %s\n" % ", ".join(bad))
json.dump({"schema": "rb-cli-batch/1", "target": image, "operations": ops}, sys.stdout, indent=1)
