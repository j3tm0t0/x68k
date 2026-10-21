#!/usr/bin/env bash
# XEiJ の X68000 テキスト VRAM を数秒おきに読み、Human68k のエラーダイアログ (バスエラー等) が出たら 1 行出力する。
# ダイアログは textcopy に出ない。プレーン 0 と 1 を重ねて見る (白い枠がプレーン 1 だけに描かれることがある)。画面のスクリーンショットは XEiJ のウィンドウが隠れていると Mac の別の画面を撮って
# しまうので使わない。テキスト VRAM (0xE00000、1 プレーン 128 バイト/ライン) で、ほぼ白いバイト (8 ドット中 5 ドット以上点灯)
# が横に 30 個以上続く行 (ダイアログの白地) があるかで判定する (ふだんの画面はファンクションキー表示の 8 個が最大、
# 漢字の行も 6 未満。バスエラーのダイアログは 52)。
# Monitor から使う: harness/xeij/xeijwatch.sh [間隔秒]
cd "$(dirname "$0")/../../x68kcli"   # build/ と tools/ (XEiJ, rb-cli) は x68kcli のもの
iv=${1:-5}; last=""
while sleep "$iv"; do
  rm -f build/watch_tvram.bin
  ../harness/xeij/xeijcmd.sh dump "$PWD/build/watch_tvram.bin" 0xE00000 0x40000   # プレーン 0 と 1
  for i in 1 2 3 4 5 6; do [ -s build/watch_tvram.bin ] && break; sleep 0.5; done
  [ -s build/watch_tvram.bin ] || continue
  r=$(python3 -I - build/watch_tvram.bin <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
p0, p1 = d[:0x20000], d[0x20000:0x40000]   # ダイアログはプレーン 1 に描かれることがあるので 2 枚を重ねて見る
best = 0
for y in range(512):
    run = 0
    for i in range(y * 128, y * 128 + 96):
        run = run + 1 if bin(p0[i] | p1[i]).count('1') >= 5 else 0   # 白地に黒文字のバイトも数える
        best = max(best, run)
print("DIALOG" if best >= 30 else "ok")
PY
)
  if [ "$r" = DIALOG ] && [ "$last" != DIALOG ]; then echo "xeij: error dialog on screen ($(date +%T))"; fi
  last=$r
done
