#!/usr/bin/env bash
# tis.x を XEiJ (X68000 SUPER、10MHz 68000、ネットワーク無し) で試す。
#   1. 作業ディレクトリ (TIS_OUT/boot) に HUMAN.SYS・COMMAND.X (XEiJ 同梱)、tis.x、試験プログラムを置く
#   2. AUTOEXEC.BAT が tis.x を常駐させ、試験を順に動かして結果を *.TXT に書き、最後に DONE.TXT を作る
#   3. XEiJ を -boot=<そのディレクトリ> -config=default -saveonexit=off で起動し (ユーザ設定に触れない)、
#      DONE.TXT ができたら終了させて結果を表示する
# 使い方: tests/xeij_test.sh [試験名 ...]   (既定: tistest rcmd ssh)
# 環境変数: XEIJ=XEiJ のディレクトリ、TIS_OUT=出力先、RCMD_X=rcmd.x、SSH_X=ssh.x (無ければその試験を飛ばす)
# java が HeadlessException で起動できないシェルでは、このスクリプトを .command にして open -a iTerm で動かす
set -euo pipefail
cd "$(dirname "$0")/.."
XEIJ=${XEIJ:-$HOME/Dropbox/src/github.com/j3tm0t0/x68kcli/tools/xeij}
OUT=${TIS_OUT:-${TMPDIR:-/tmp}/tistest}
RCMD_X=${RCMD_X:-$OUT/rcmd.x}
SSH_X=${SSH_X:-$HOME/Dropbox/src/github.com/j3tm0t0/x68k-ssh/net/ssh/dropbear/ssh.x}
B="$OUT/boot"
JAVA=java; [ -x /opt/homebrew/opt/openjdk/bin/java ] && JAVA=/opt/homebrew/opt/openjdk/bin/java
TESTS=${*:-tistest rcmd ssh}
[ -f "$XEIJ/XEiJ.jar" ] || { echo "XEiJ が見つからない: $XEIJ (XEIJ=... で指定)"; exit 1; }
make -s
make -s -C tests
# rcmd.x は X68000 上の gcc + ESP/X libnetwork でビルドしたもの。x68kcli のイメージから取り出す
if [ ! -f "$RCMD_X" ] && [ -x "$XEIJ/../rb-cli" ]; then
  for img in "$XEIJ"/../../build/*.hds; do
    "$XEIJ/../rb-cli" get "$img" /USR/BIN/rcmd.x "$RCMD_X" >/dev/null 2>&1 && break || true
  done
fi
rm -rf "$B" "$OUT/tmp"; mkdir -p "$B/TMP" "$OUT/tmp"
cp "$XEIJ/data/HUMAN.SYS" "$XEIJ/data/COMMAND.X" "$B/"
cp tis.x "$B/TIS.X"
cp tests/tistest.x "$B/TISTEST.X"; cp tests/tisrun.x "$B/TISRUN.X"
# PROCESS はスレッド (_OPEN_PR) と libsocket の poll の時計 (_TIME_PR) に要る
printf 'FILES = 30\r\nBUFFERS = 20 1024\r\nLASTDRIVE = Z:\r\nPROCESS = 16 10 20\r\n' > "$B/CONFIG.SYS"
{
  printf 'ECHO OFF\r\nPATH A:\\\r\n'
  printf 'TIS.X > TIS.TXT\r\n'
  for t in $TESTS; do
    case $t in
      tistest) printf 'TISTEST.X > TISTEST.TXT\r\n';;
      rcmd)
        if [ -f "$RCMD_X" ]; then
          cp "$RCMD_X" "$B/RCMD.X"
          # rcmd.x をフォアグラウンドで、試験のクライアントを tisrun.x のスレッドで動かす
          printf 'TISRUN.X rcmd RCMD.X -s A:\\COMMAND.X -d > RCMD.TXT\r\n'
        else echo "rcmd.x が無いので rcmd の試験は飛ばす"; fi;;
      ssh)
        if [ -f "$SSH_X" ]; then
          cp "$SSH_X" "$B/SSH.X"
          # ssh.x を 127.0.0.1:22 へ。サーバ (偽のバナー) は tisrun.x のスレッド
          printf 'TISRUN.X ssh SSH.X -y -y test@127.0.0.1 true > SSH.TXT\r\n'
        else echo "ssh.x が無いので ssh の試験は飛ばす"; fi;;
    esac
  done
  printf 'TIS.X -s > STAT.TXT\r\n'
  printf 'COPY CONFIG.SYS DONE.TXT\r\n'
} > "$B/AUTOEXEC.BAT"

"$JAVA" -Djava.io.tmpdir="$OUT/tmp" -jar "$XEIJ/XEiJ.jar" -config=default -saveonexit=off \
  -model=SUPER -boot="$B" -flrinformed=on -usecanvas=off > "$OUT/xeij.log" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT
for t in $(seq 1 ${TIS_TIMEOUT:-600}); do
  [ -f "$B/DONE.TXT" ] && break
  kill -0 $PID 2>/dev/null || { echo "XEiJ が終了した (ログ: $OUT/xeij.log)"; exit 1; }
  sleep 1
done
[ -f "$B/DONE.TXT" ] || { echo "時間切れ"; ls "$B"; exit 1; }
kill $PID 2>/dev/null || true
for f in TIS TISTEST RCMD SSH STAT; do
  [ -f "$B/$f.TXT" ] || continue
  echo "== $f.TXT"
  iconv -f CP932 -t UTF-8 "$B/$f.TXT" 2>/dev/null | tr -d '\r' || tr -d '\r' < "$B/$f.TXT"
done
