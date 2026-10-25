#!/usr/bin/env bash
# XEiJ を「システムフロッピーだけ、主記憶 2MB の Compact」として起動し、RS-232C を TCP に出す。
#   usage: xeij-fd.sh RUNDIR [XDF]
#   RUNDIR に fd0.xdf (XDF の書き込み用コピー)、XEiJControl (キー入力パイプ)、agent.cmd、xeij.log を置く。
#   RS-232C は 127.0.0.1:${XPORT:-54368} (XEiJ がサーバ)。
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
tools=${XEIJ_TOOLS:-$top/x68kcli/tools}
[ -d "$tools/xeij" ] || tools=/Users/moto/Dropbox/src/github.com/j3tm0t0/x68kcli/tools
run=$(mkdir -p "$1" && cd "$1" && pwd)
xdf=${2:-$tools/xeij/HUMAN302.XDF}
if [ ! -f "$run/fd0.xdf" ]; then
  cp "$xdf" "$run/fd0.xdf"
  chmod u+w "$run/fd0.xdf"
  # XEiJ は起動時に RSDRV.SYS 2.02 を見つけると、受信バッファを 64 バイトから 1024 バイトに広げるパッチをあてる。
  # 実機と同じ条件で試すため、既定では試験用の RSBUF64.X を AUTOEXEC.BAT から起動して 64 バイトに戻す
  # (XEIJ_RSBUF=1024 なら XEiJ のままにする)。版の文字列を変えてパッチを止める方法は、XEiJ が XON/XOFF の
  # 設定を読めなくなり、X68000 が送った 13h でデータの受け渡しを止めてしまうので使わない
  if [ "${XEIJ_RSBUF:-64}" = 64 ]; then
    [ -s "$here/rsbuf64.x" ] || m68k-xelf-gcc -m68000 -Os -nostartfiles -o "$here/rsbuf64.x" "$here/rsbuf64.c" -Wl,-e,main -s
    python3 "$here/testfloppy.py" "$run/fd0.xdf" "$tools/rb-cli" "$here/rsbuf64.x"
  fi
fi
JAVA=java
[ -x /opt/homebrew/opt/openjdk/bin/java ] && JAVA=/opt/homebrew/opt/openjdk/bin/java
AGENT=
[ -f "$tools/xeijagent.jar" ] && AGENT="-javaagent:$tools/xeijagent.jar=$run/agent.cmd"
cd "$run"
# shellcheck disable=SC2086
exec "$JAVA" $AGENT -Djava.io.tmpdir="$run" -jar "$tools/xeij/XEiJ.jar" \
  -config=default -model=Compact -memory=${XMEM:-2} -boot=fd0 -fd0="$run/fd0.xdf" \
  -pastepipe=on -pipeencoding=utf8 -flrinformed=on -usecanvas=off \
  -tcpiphost=127.0.0.1 -tcpipport=${XPORT:-54368} -tcpiputf8=off \
  -rs232cconnection='TCP%2FIP+%E2%87%94+AUX' >"$run/xeij.log" 2>&1
