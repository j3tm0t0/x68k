#!/usr/bin/env bash
# Z-OS (zos.x) の煙試験: XEiJ + picosim (TSP 入り) で、2 つのペインの片方で ssh.x を TSP (tis.x -p) 越しに Mac の試験用 sshd へ。
#   1. XEiJ を x68kcli の stage (ksh・zsh など) から起動。CONFIG.SYS に npd.sys 1.2、AUTOEXEC.BAT で tis.x -p と npd.x
#      (xip.x は使わない。TCP は picosim の lwIP が話す)
#   2. zos.x (ksh のペイン 2 つ、左右)。右のペインで ssh.x -t、相手は 127.0.0.1:$SSHPORT の sshd (ユーザ権限、鍵 1 つ、
#      ForceCommand の demo.sh: 端末の大きさを出し、何行か出してから 1 行読む)
#   3. 左のペインで echo を打ち、右に abc を打ち、画面 (テキスト VRAM を tvram2txt.py で文字に) を見る
# 合格: 右に "hello from mac sshd" と "got: abc"、左に echo の結果、sshd が公開鍵で受け入れた、demo.sh が見た端末がペインの大きさ
# Z-OS の道具 (xeij_run.sh・xeijkeys.py・tvram2txt.py・zos.x) は j3tm0t0/x68k-zos の kernel/ (ZOS=、読むだけ。写して使う)。
# 無ければ SKIP。NOXEIJ=1 でも飛ばす
set -uo pipefail
cd "$(dirname "$0")/.."
TOP=$PWD/../..
SRC=$HOME/Dropbox/src/github.com/j3tm0t0
ZOS=${ZOS:-$SRC/x68k-zos-kernel/kernel}
STAGE=${STAGE:-$TOP/x68kcli/stage}
[ -d "$STAGE/USR/BIN" ] || STAGE=$SRC/x68k-picoharness/x68kcli/stage
SSH_X=${SSH_X:-$(ls -t "$SRC"/x68k*/net/ssh/dropbear/ssh.x 2>/dev/null | head -1)}
XEIJ=${XEIJ:-$TOP/x68kcli/tools/xeij}
AGENT=$TOP/x68kcli/tools/xeijagent.jar
WORK=${WORK:-${TMPDIR:-/tmp}/picosim-zostest}
SSHPORT=${SSHPORT:-2224} PICOPORT=${PICOPORT:-26869}
skip() { echo "SKIP zostest: $*"; exit 0; }
[ -n "${NOXEIJ:-}" ] && skip "NOXEIJ"
[ -f "$ZOS/zos.x" ] && [ -f "$ZOS/tests/xeij_run.sh" ] || skip "no Z-OS kernel at $ZOS (ZOS=.../x68k-zos/kernel)"
[ -d "$STAGE/USR/BIN" ] || skip "no x68kcli stage ($STAGE; make -C x68kcli stage)"
[ -f "$SSH_X" ] || skip "no ssh.x (SSH_X=)"
[ -f "$XEIJ/XEiJ.jar" ] && [ -f "$AGENT" ] || skip "no XEiJ / agent in x68kcli/tools"
grep -qa "socket service for tis.x" build/picosim 2>/dev/null || skip "build/picosim has no TSP (lwIP)"
. "$(dirname "$0")/benchlock.sh"
benchlock_take zostest || skip "bench locked by $(cat "$XEIJLOCK")"
trap benchlock_release EXIT
make -s -C "$TOP/net/npd" && make -s -C "$TOP/net/tistack" || { echo "FAIL build npd / tis.x"; benchlock_release; exit 1; }
rm -rf "$WORK/t" && mkdir -p "$WORK/t" && cp "$ZOS"/tests/{xeij_run.sh,xeijkeys.py,tvram2txt.py,env.bat.txt} "$WORK/t/"
# 置くファイルの名前を大文字にしない (Linux (Docker) のホストのディレクトリの起動は大文字小文字を区別し、xeijkeys.py は
# 大文字を打てない。名前は小文字のまま置き、小文字で打つ)
sed -i.orig 's/ | tr a-z A-Z//' "$WORK/t/xeij_run.sh"
OUT=$WORK/out; mkdir -p "$OUT"

# ---- 試験用の sshd (ssh_test.sh と同じ形、ユーザ権限で 127.0.0.1 だけ) と dropbear 形式の鍵 ----
D=$WORK/sshd; mkdir -p "$D"
if [ ! -x "$WORK/dropbearkey" ] && command -v dropbearkey >/dev/null; then
  ln -sf "$(command -v dropbearkey)" "$WORK/dropbearkey"     # Linux (Docker) の dropbear-bin
fi
if [ ! -x "$WORK/dropbearkey" ]; then
  TB=$(ls "$SRC"/x68k*/net/ssh/dropbear/work/dropbear-*.tar.bz2 2>/dev/null | head -1)
  [ -n "$TB" ] || skip "no dropbear tarball for dropbearkey (net/ssh/dropbear/fetch.sh)"
  rm -rf "$WORK/db" && mkdir -p "$WORK/db" && tar xjf "$TB" -C "$WORK/db"
  (cd "$WORK"/db/dropbear-* && ./configure --disable-zlib -q >/dev/null 2>&1 && make -s -j8 PROGRAMS=dropbearkey >/dev/null 2>&1)
  cp "$WORK"/db/dropbear-*/dropbearkey "$WORK/" || { echo "FAIL dropbearkey"; exit 1; }
fi
if [ ! -f "$D/idzos" ]; then
  "$WORK/dropbearkey" -t ed25519 -f "$D/idzos" 2>&1 | grep '^ssh-ed25519' > "$D/idzos.pub"
  ssh-keygen -q -t ed25519 -f "$D/hostkey" -N ''
fi
cat > "$D/demo.sh" <<'EOF'
#!/bin/sh
echo "hello from mac sshd ($(tput cols 2>/dev/null)x$(tput lines 2>/dev/null))"
i=0
while [ $i -lt 12 ]; do echo "mac line $i"; i=$((i+1)); sleep 0.3; done
printf 'type something: '
read x
echo "got: $x"
echo "size $(stty size 2>/dev/null | awk "{print \$2 \"x\" \$1}")"
sleep 2
EOF
chmod +x "$D/demo.sh"
printf 'command="%s",no-port-forwarding,no-agent-forwarding,no-X11-forwarding %s\n' "$D/demo.sh" "$(cat "$D/idzos.pub")" > "$D/authorized_keys"
cat > "$D/sshd_config" <<EOF
Port $SSHPORT
ListenAddress 127.0.0.1
HostKey $D/hostkey
AuthorizedKeysFile $D/authorized_keys
PasswordAuthentication no
KbdInteractiveAuthentication no
UsePAM no
StrictModes no
PidFile $D/sshd.pid
LoginGraceTime 600
ForceCommand $D/demo.sh
AllowUsers $(whoami)
EOF
cp "$D/idzos" "$WORK/idzos"

/usr/sbin/sshd -D -e -o LogLevel=INFO -f "$D/sshd_config" > "$WORK/sshd.log" 2>&1 &
SPID=$!
TSP_DNS=192.168.1.3 build/picosim -l 127.0.0.1:$PICOPORT -L none -s 30 -p "$WORK/lan.pcap" > "$WORK/picosim.log" 2>&1 &
PPID_=$!
trap 'kill $SPID $PPID_ 2>/dev/null || true; benchlock_release' EXIT
sleep 1

IFS=$'\n' ENVL=($(cat "$WORK/t/env.bat.txt") 'SET SHELL=A:/usr/bin/ksh.x' 'tis.x -p > TIS.TXT' 'npd.x 192.168.1.68 > NPD.TXT'); unset IFS
U=$(whoami)
# ksh の起動 (XEiJ 10MHz で 2〜3 秒) を待ってから。F10 = 次のペイン、OPT.1+Q = Z-OS を終える
# ACCEL=1: 鍵交換を picosim の暗号アクセラレータで (-oAccel=all)
SSHOPT=; [ -n "${ACCEL:-}" ] && SSHOPT="-oAccel=all "
KEYS="sleep 12;key f10;type ssh.x -t -y -y ${SSHOPT}-i a:/idzos -p $SSHPORT $U@192.168.1.1\\n;waittext type something 240"
KEYS="$KEYS;key f10;type echo left pane while ssh waits\\n;key f10;type abc\\n;waittext got: abc 60;sleep 3"
KEYS="$KEYS;dump zos.bin 0xE00000 0x80000;key f10;type a:/tis.x -s > a:/TISS.TXT\\n;sleep 3;key f10;sleep 1;type exit\\n;sleep 2;key opt1 q"
AGENTJAR=$AGENT XEIJ=$XEIJ ZOS_OUT=$OUT ZOS_JAVA_OPTS="-Dpicosim=127.0.0.1:$PICOPORT -Dpicosim.ids=6" \
FILES="$ZOS/zos.x $SSH_X $WORK/idzos $TOP/net/tistack/tis.x $TOP/net/npd/npd.sys $TOP/net/npd/npd.x" ZOS_STAGE="$STAGE" \
ZOS_CONFIG='DEVICE = \npd.sys;DEVICE = \SYS\FLOAT2.X;ENVSET = 8192' ZOS_TIMEOUT=${ZOS_TIMEOUT:-420} ZOS_KEYS="$KEYS" \
  "$WORK/t/xeij_run.sh" "${ENVL[@]}" "zos.x ${ZOSARGS:+$ZOSARGS }-x A:\\USR\\BIN\\ksh.x A:\\USR\\BIN\\ksh.x" > "$WORK/run.log" 2>&1 || true

fails=0
ok() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
[ -f "$OUT/zos.bin" ] && python3 -I "$WORK/t/tvram2txt.py" "$OUT/zos.bin" "$OUT/zos.png" > "$OUT/zos.txt" 2>/dev/null
touch "$OUT/zos.txt"
left=$(cut -c1-47 "$OUT/zos.txt"); right=$(cut -c49- "$OUT/zos.txt")
# ssh.x が入ったペインを見て決める (どちらのペインが前面で始まるかは、ksh の起動の速さで変わることがある。Docker では左だった)
if ! echo "$right" | grep -q 'hello from mac sshd' && echo "$left" | grep -q 'hello from mac sshd'; then
  t=$left; left=$right; right=$t; echo "     (ssh.x ran in the left pane this time)"
fi
ok "Z-OS: tis.x -p is up" "grep -q 'TCP/IP via Pico' '$OUT/boot/TIS.TXT' 2>/dev/null"
ok "Z-OS: ssh.x in one pane reached the Mac's sshd over TSP" "echo \"\$right\" | grep -q 'hello from mac sshd'"
ok "Z-OS: keys typed in the ssh.x pane reach the sshd (got: abc)" "echo \"\$right\" | grep -q 'got: abc'"
ok "Z-OS: the other pane ran echo while ssh.x waited" "echo \"\$left\" | grep -q '^left pane while ssh waits'"
ok "sshd accepted the ssh.x key" "grep -q 'Accepted publickey' '$WORK/sshd.log'"
sz=$(echo "$right" | grep -o '^size [0-9]*x[0-9]*' | head -1)
echo "     terminal size seen by the sshd: ${sz:-?} (screen: $OUT/zos.txt)"
grep -q "tsp: first 0xA6" "$WORK/picosim.log" && echo "     picosim: lwIP joined the LAN on the first 0xA6"
grep -a "pico xchg" "$OUT/boot/TISS.TXT" 2>/dev/null | tr -d '\r' | sed 's/^ */     tis.x -s: /'   # stall / gap (npd.x の交換の間の最長)
# 鍵交換の時間: ssh.x を打ち終えてから画面に "type something" (相手の demo.sh の問い) が出るまで (xeijkeys.py の waittext の待ち時間)
kex=$(grep -m1 "waittext 'type something' True" "$OUT/keys.log" 2>/dev/null | awk '{print $NF}')
echo "     ssh.x connect + kex + login (to the demo prompt): ${kex:-?} s${ACCEL:+ (accel)}"
if [ $fails -gt 0 ]; then
  cat "$OUT/zos.txt"; tail -5 "$WORK/sshd.log"; cat "$OUT/boot/TISS.TXT" 2>/dev/null; grep "tsp: socket" "$WORK/picosim.log"
  # 落ちたときの記録を残す (次の回に上書きされないように。TSP のたまの失敗を x68k-tistack-2 が調べる)
  keep=$WORK.fail-$(date +%Y%m%d-%H%M%S); cp -R "$WORK" "$keep" 2>/dev/null && echo "     kept: $keep"
fi
echo "$fails failure(s)"
exit $((fails > 0))
