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
[ -f "$run/fd0.xdf" ] || cp "$xdf" "$run/fd0.xdf"
chmod u+w "$run/fd0.xdf"
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
