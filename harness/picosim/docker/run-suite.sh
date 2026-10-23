#!/usr/bin/env bash
# run-suite.sh SUITE... -- Docker の中の 1 回の試験 (docker/Dockerfile の ENTRYPOINT)。SUITE は host / tsp / zos (make test-*)
# 読むだけでつなぐもの (docker-test.sh が -v で):
#   /repo     x68k の作業木 (中に写してからビルドする。ほかのコンテナとビルドの残りがぶつからない)
#   /mnt/xeij XEiJ (XEiJ.jar と data/)       /mnt/stage x68kcli の stage (Human68k・zsh・ksh...)
#   /mnt/fw   起動待ち・hdsboot・settingui・scsiremote の 68000 のバイナリ (driver/)   /mnt/rs x68kserremote
#   /mnt/pzlsrc PZL の写し (pzl/build/src/pico)   /mnt/lwip pico-sdk の lib/lwip   /mnt/zos x68k-zos の kernel/
#   /mnt/ssh.x  試験に使う ssh.x
# 書くもの: /out (落ちたときの記録と、それぞれの試験の出力)
set -uo pipefail
mkdir -p /out
log=/out/run.log
exec > >(tee -a "$log") 2>&1
echo "== $(date +%T) suites: $*  ($(uname -m), $(java -version 2>&1 | head -1))"
W=$HOME/x68k
rsync -a --delete --exclude 'build/' --exclude 'x68kcli/tools' --exclude 'x68kcli/dl' --exclude 'x68kcli/stage' \
  --exclude 'x68kcli/work' --exclude '*.hds' --exclude '.git' /repo/ "$W/"
mkdir -p "$W/x68kcli/tools" && ln -sfn /mnt/xeij "$W/x68kcli/tools/xeij"
[ -d /mnt/stage ] && ln -sfn /mnt/stage "$W/x68kcli/stage"
(cd "$W/x68kcli" && make -s agent) || { echo "FAIL agent"; exit 1; }
export TMPDIR=/tmp/ CGROM=/mnt/xeij/data/CGROM_XEiJ.DAT STAGE=/mnt/stage ZOS=/mnt/zos SSH_X=/mnt/ssh.x XEIJLOCK_NAME=docker
Xvfb :99 -screen 0 1280x1024x24 -nolisten tcp >/tmp/xvfb.log 2>&1 &
export DISPLAY=:99
sleep 1
M="make -C $W/harness/picosim FW=/mnt/fw RS=/mnt/rs LWIP=/mnt/lwip PZLSRC=/mnt/pzlsrc PZLREPO=/nonexistent"
rc=0
for s in "$@"; do
  echo "== $(date +%T) test-$s"
  $M "test-$s" > /out/test-$s.log 2>&1
  r=$?
  grep -E "^(PASS|FAIL|SKIP)|failure\(s\)" /out/test-$s.log
  [ $r -eq 0 ] && ! grep -q "^FAIL" /out/test-$s.log || { rc=1; echo "== test-$s FAILED (rc $r)"; }
done
# 落ちたときの記録 (zostest の .fail-* と、作業場所)
for d in /tmp/picosim-zostest* /tmp/picosim-tsptest* /tmp/tispico; do
  [ -e "$d" ] && [ $rc -ne 0 ] && cp -R "$d" /out/ 2>/dev/null
done
cp "$W"/harness/picosim/tests/*.log /out/ 2>/dev/null
echo "== $(date +%T) done rc=$rc"
exit $rc
