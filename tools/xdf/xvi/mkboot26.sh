#!/usr/bin/env bash
# mkboot26.sh -- Compact XVI の新しい起動フロッピー (BOOT26.XDF) を作る。
#   usage: mkboot26.sh HUMAN302.XDF XVIA.XDF OUT.XDF
#   HUMAN302.XDF  シャープが無償公開した Human68k 3.02 のシステムディスク。起動セクタ、HUMAN.SYS、COMMAND.X をここから取る
#   XVIA.XDF      XVI の今の A: を xdfread.x で読んだもの (2026-10-26)。CONFIG.SYS、AUTOEXEC.BAT と、そこから使うファイルを取る
# HUMAN302 のほかのファイルは入らない (XVI のファイルと合わせると容量を超える) ので、BIN の小さな道具だけ選んで足す。
# XVI の A: にあった控え (*.old、*.z6、AUTOEXEC.ORG など)、試験の記録、ベンチマーク、使っていない古い版は入れない。
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
fs="python3 -I $here/../xdffs.py"
base=$1 xvi=$2 out=$3
tmp=$(mktemp -d "${TMPDIR:-/tmp}/mkboot26.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

# HUMAN302 から足す道具 (A:\BIN)。xdfwrite.x / xdfread.x も入れる
$fs extract "$base" "$tmp/h"
mkdir -p "$tmp/add/BIN"
for f in FORMAT.X CHKDSK.X COPYALL.X SYS.X ATTRIB.X MORE.X TREE.X FC.X FIND.X SORT.X WHERE.X; do
  cp "$tmp/h/BIN/$f" "$tmp/add/BIN/"
done
cp "$here/../xdfwrite.x" "$tmp/add/BIN/XDFWRITE.X"
cp "$here/../xdfread.x" "$tmp/add/BIN/XDFREAD.X"

# HUMAN302 から残すのは起動セクタ (IPL)、ボリューム名、HUMAN.SYS、COMMAND.X だけ。
# (XVI の A: の HUMAN.SYS と COMMAND.X は HUMAN302 のものと同じ中身。--merge で置き換わっても変わらない)
drop_base=(/CONFIG.SYS /KEY.SYS /USKCG.SYS /BEEP.SYS /STARTUP.ENV /AUTOEXEC.BAT /SYS /HIS /BIN /BASIC2 /ASK /ETC)
# XVI の A: から入れないもの
drop_xvi=(
  # 控えと古い設定
  /AUTOEXEC.ORG /AUTOEXEC.CON /AUTOEXEC.NET /config.slp /autoexec.slp /config.zm1 /config.old /autoexec.old
  /config.z6 /autoexec.z6 /rx.old /SYS/zmux.old /BIN/zmux.old
  # 試験の記録と出力
  /snap1.txt /snap2.txt /snap3.txt /RX.OUT /TMP/rcmd.out
  # ベンチマーク
  /cpuspd.x /cpupower.r /rtcbench.x /idlebench.x
  # CONFIG.SYS / AUTOEXEC.BAT が使っていない古い版 (今は ZMUX8.SYS + ZMUX7.X、CTE78.SYS、A:\rx.x、zosh.x)
  /SYS/zmux.sys /SYS/ZMUX7.SYS /SYS/contee.sys /BIN/zmux.x /BIN/RX.X /BIN/ZOSHR.X
)
args=()
for p in "${drop_base[@]}"; do args+=(--rm "$p"); done
args+=(--merge "$xvi")
for p in "${drop_xvi[@]}"; do args+=(--rm "$p"); done
args+=(--add "$tmp/add")
$fs build "$out" "$base" "${args[@]}"
