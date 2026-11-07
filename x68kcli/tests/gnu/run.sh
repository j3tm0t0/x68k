#!/usr/bin/env bash
# GNU の道具 (build/x/gnu/*.x) を XEiJ で動かし、cases.txt の期待と比べる。
# 使い方: tests/gnu/run.sh [名前...]   (OUT=出力先、TIMEOUT=秒)
set -u
cd "$(dirname "$0")"
T=$PWD; X=$(cd ../.. && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/x68kcli-gnu}; rm -rf "$OUT"; mkdir -p "$OUT/in/DIR1"
# 入力 (CR LF、Shift_JIS の名前は 2 バイト目が 0x5C の 表 (95 5C) と ソ (83 5C))
I=$OUT/in
printf 'banana 3\r\napple 10\r\ncherry 2\r\napple 10\r\n' > "$I/IN.TXT"
printf '10\n9\n100\n-1\n' > "$I/NUM.TXT"
printf 'a\na\nb\n' > "$I/S.TXT"
printf 'x\n' > "$I/S2.TXT"
printf '\000\015\012\377' > "$I/BIN.DAT"
printf 'abcdef\n' > "$I/TR.TXT"
printf '1 a\n2 b\n' > "$I/J1.TXT"; printf '1 x\n2 y\n' > "$I/J2.TXT"
printf '1\t\223\372  x\r\n2   b\r\n' > "$I/J3.TXT"; printf ' 1 \226\173\r\n2\ty\r\n' > "$I/J4.TXT"   # タブ・続く空白・日本語の欄
printf 'a 1\nb 1\n  c\t1\n\223\372 2\n' > "$I/UF.TXT"
printf 'a\tb\n' > "$I/TAB.TXT"
printf 'a\nb\n' > "$I/S3.TXT"
printf 'gz\n' > "$I/G.TXT"
printf 'a\nb\nc\n' > "$I/D1.TXT"; printf 'a\nB\nc\n' > "$I/D2.TXT"; printf 'a\nb\nC\n' > "$I/D3.TXT"
printf '2^10\nscale=3\n1/3\nquit\n' > "$I/BCIN.TXT"
printf '\223\372\226\173\214\352\r\n' > "$I/SJ.TXT"   # 日本語 (Shift_JIS)
cp "$X/src/magic/magic" "$I/MAGIC.TXT"
printf 'a\r\n' > "$I/DIR1/A.TXT"
# XEiJ のホストファイルシステムは、Human68k の Shift_JIS の名前をホストでは UTF-8 の名前にする
printf 'hyo\r\n' > "$I/DIR1/表.TXT"
printf 'so\r\n' > "$I/DIR1/ソ.TXT"
python3 -I - "$T/cases.txt" "$OUT" "$@" <<'PY'
import sys
src, out, only = sys.argv[1], sys.argv[2], sys.argv[3:]
cases = []
cur = None
for line in open(src, encoding='utf-8'):
    line = line.rstrip('\n')
    if line.startswith('#') and (cur is None or cur[1] is None): continue
    if line.startswith('== '):
        cur = [line[3:].strip(), None, []]; cases.append(cur); continue
    if cur is None: continue
    if cur[1] is None: cur[1] = line
    else: cur[2].append(line)
cases = [c for c in cases if not only or c[0] in only]
with open(out + '/lines.txt', 'w', encoding='utf-8') as f:
    for i, (n, cmd, exp) in enumerate(cases):
        # COMMAND.X のバッチでは %0〜%9 が引数になるので %% にする。自分で > を書いた行は受けない
        cmd = cmd.replace('%', '%%')
        f.write((cmd if '>' in cmd else '%s > R%03d.TXT' % (cmd, i)) + '\n')
with open(out + '/cases.tsv', 'w', encoding='utf-8') as f:
    for i, (n, cmd, exp) in enumerate(cases):
        f.write('%d\t%s\t%s\n' % (i, n, '\x1f'.join(exp)))
PY
LINES=(); while IFS= read -r l; do LINES+=("$l"); done < "$OUT/lines.txt"
FILES="$(ls "$X"/build/x/gnu/*.x | tr '\n' ' ') $(ls "$I"/*.* | tr '\n' ' ')" DIRS="$I/DIR1" OUT=$OUT TIMEOUT=${TIMEOUT:-600} \
  "$X/tests/xeij_hfs.sh" "${LINES[@]}" > "$OUT/run.log" 2>&1 || { cat "$OUT/run.log"; }
python3 -I - "$OUT" <<'PY'
import sys, re, os
out = sys.argv[1]
ok = ng = xf = 0
for line in open(out + '/cases.tsv', encoding='utf-8'):
    i, n, exp = line.rstrip('\n').split('\t', 2)
    xfail = n.endswith(' xfail'); n = n.split()[0]
    exp = exp.split('\x1f') if exp else []
    p = '%s/boot/R%03d.TXT' % (out, int(i))
    if not exp and not os.path.exists(p):   # 出力を見ない行 (後の行で結果を見る)
        ok += 1; print('ok   %s' % n); continue
    if not os.path.exists(p):
        print('FAIL %-16s (出力が無い)' % n); ng += 1; continue
    got = open(p, 'rb').read().decode('cp932', 'replace').replace('\r', '').split('\n')
    if got and got[-1] == '': got.pop()
    got = [g.rstrip(' ') for g in got]   # 行末の空白は比べない (cases.txt に書けないので)
    good = len(got) == len(exp) and all(re.fullmatch(e[1:], g) if e.startswith('~') else e == g for e, g in zip(exp, got))
    if good: ok += 1; print('ok   %s' % n)
    elif xfail: xf += 1; print('XFAIL %s' % n)
    else:
        ng += 1; print('FAIL %-16s 期待 %r\n     %-16s 出力 %r' % (n, exp, '', got))
print('%d ok, %d FAIL, %d XFAIL (わかっている失敗)' % (ok, ng, xf))
sys.exit(1 if ng else 0)
PY
