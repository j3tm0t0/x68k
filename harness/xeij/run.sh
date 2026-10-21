#!/usr/bin/env bash
# XEiJ で HDD イメージから起動する (SCSI ID 0)
set -euo pipefail
cd "$(dirname "$0")/../../x68kcli"   # build/ と tools/ (XEiJ, rb-cli) は x68kcli のもの
IMG=${IMG:-build/x68kcli.hds}
mkdir -p build
JAVA=java
[ -x /opt/homebrew/opt/openjdk/bin/java ] && JAVA=/opt/homebrew/opt/openjdk/bin/java
# -flrinformed=on: 初回の点滅光警告ダイアログを出さない (出ると MPU が起動しない)
# -usecanvas=off: エージェントの screenshot が黒画像にならないようにする
# java.io.tmpdir を build/ にすると貼り付けパイプ build/XEiJPaste, 制御パイプ build/XEiJControl が作られる
AGENT=
[ -f tools/xeijagent.jar ] && AGENT="-javaagent:$PWD/tools/xeijagent.jar=$PWD/build/agent.cmd"
# shellcheck disable=SC2086
exec "$JAVA" $AGENT -Djava.io.tmpdir="$PWD/build" -jar tools/xeij/XEiJ.jar \
  -boot=sc0 -si0="$PWD/$IMG" -pastepipe=on -pipeencoding=utf8 -flrinformed=on -usecanvas=off "$@"
