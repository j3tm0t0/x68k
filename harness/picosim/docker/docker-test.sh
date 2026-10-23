#!/usr/bin/env bash
# docker-test.sh [SUITE...] -- picosim の試験を Docker の中で流す (docker/README.md)。SUITE は host / tsp / zos / tsppzl (既定は全部)。
#   同じものを何度も並べてよい (例: zos zos zos zos zos)。1 つのコンテナが 1 回の試験 (口も作業場所もコンテナごと)。
#   同時に動かすのは JOBS 個 (既定 2。XEiJ の鍵交換がホストの負荷で遅くならないように)。
#   記録は harness/picosim/build/docker-out/<日時>/<番号>-<SUITE>/ (run.log、test-*.log、落ちたときの作業場所)
# 第三者のもの (XEiJ・x68kcli の stage・ssh.x・フォークの driver/ と x68kserremote・lwIP・x68k-zos の kernel/) は image に入れず、
# ここで見つけて読むだけでつなぐ。場所は環境変数で変えられる (下)
set -uo pipefail
cd "$(dirname "$0")/.."
H=$PWD; TOP=$(cd ../.. && pwd)
SRC=$HOME/Dropbox/src/github.com/j3tm0t0
IMAGE=${IMAGE:-x68k-picoharness:latest}
PLATFORM=${PLATFORM:-}            # 既定はホストと同じ (Mac・raspi5 は arm64)。CI の x86-64 なら linux/amd64
JOBS=${JOBS:-2}
SUITES=("${@:-host}"); [ $# -eq 0 ] && SUITES=(host tsp zos tsppzl)
real() { [ -e "$1" ] && (cd "$(dirname "$1")" && echo "$(pwd -P)/$(basename "$1")"); }
XEIJ=${XEIJ:-$(real "$TOP/x68kcli/tools/xeij" || real "$SRC/x68kcli/tools/xeij")}
STAGE=${STAGE:-$( [ -d "$TOP/x68kcli/stage/USR/BIN" ] && echo "$TOP/x68kcli/stage" || echo "$SRC/x68k-picoharness/x68kcli/stage")}
FW=${FW:-$HOME/src/github.com/yunkya2/x68kzremotedrv-hid/driver}
RS=${RS:-$HOME/src/github.com/yunkya2/x68kzremotedrv-hid/x68kserremote}
LWIP=${LWIP:-$HOME/src/github.com/yunkya2/x68kzremotedrv/pico-sdk/lib/lwip}
ZOS=${ZOS:-$SRC/x68k-zos-kernel/kernel}
SSH_X=${SSH_X:-$(ls -t "$SRC"/x68k*/net/ssh/dropbear/ssh.x 2>/dev/null | head -1)}
PZLREPO=${PZLREPO:-$SRC/x68k-zos-pzl}
RCMD_X=${RCMD_X:-${TMPDIR:-/tmp}/tistest/rcmd.x}   # tsptest の Mac → rcmd.x (net/tistack/tests が作るもの。無ければその 2 項目は飛ばす)
for v in XEIJ STAGE FW RS LWIP ZOS SSH_X; do [ -e "${!v}" ] || echo "docker-test: $v not found (${!v:-unset}); the suites that need it will SKIP"; done

# image (1 回だけ作る。Dockerfile が変われば作り直す)
docker build -q ${PLATFORM:+--platform "$PLATFORM"} -t "$IMAGE" docker >/dev/null || { echo "docker build failed"; exit 1; }
# m68k のもの (npd.sys・npd.x・tis.x・tisnet.x・tisrun.x) はホストの elf2x68k で作っておく (arm64 の image には elf2x68k が
# 無い)。コンテナは作業木を写すので、できたものもそのまま入る
if command -v m68k-xelf-gcc >/dev/null; then
  make -s -C "$TOP/net/npd" >/dev/null && make -s -C "$TOP/net/tistack" >/dev/null && \
    make -s -C "$TOP/net/tistack/tests" tisnet.x tisrun.x >/dev/null || echo "docker-test: m68k build on the host failed"
fi
# PZL の写し (ホストの git と elf2x68k で。コンテナには写し済みのものを渡す)
PZLSRC=
if [ -d "$PZLREPO" ] && make -s -C pzl build/src/.done PZLREPO="$PZLREPO" >/dev/null; then
  cp pzl/build/rev pzl/build/src/rev 2>/dev/null; PZLSRC=$H/pzl/build/src/pico
fi
OUT=$H/build/docker-out/$(date +%Y%m%d-%H%M%S); mkdir -p "$OUT"
mnt() { [ -n "$2" ] && [ -e "$2" ] && printf -- '-v %s:%s:ro ' "$2" "$1"; }
run_one() {
  local n=$1 s=$2 d="$OUT/$1-$2"
  mkdir -p "$d"
  # shellcheck disable=SC2046
  docker run --rm ${PLATFORM:+--platform "$PLATFORM"} --cpus "${CPUS:-4}" --shm-size 256m \
    -v "$TOP":/repo:ro $(mnt /mnt/xeij "$XEIJ") $(mnt /mnt/stage "$STAGE") $(mnt /mnt/fw "$FW") $(mnt /mnt/rs "$RS") \
    $(mnt /mnt/lwip "$LWIP") $(mnt /mnt/zos "$ZOS") $(mnt /mnt/ssh.x "$SSH_X") $(mnt /mnt/rcmd.x "$RCMD_X") $(mnt /mnt/pzlsrc "$PZLSRC") \
    -v "$d":/out "$IMAGE" "$s" > "$d/console.log" 2>&1
  local rc=$?
  echo "$rc" > "$d/rc"
  local st=FAIL
  if [ $rc -eq 0 ]; then st=PASS; grep -q "^PASS" "$d/run.log" 2>/dev/null || st=SKIP; fi
  echo "$st" > "$d/status"
  printf '%-14s %s  (%s)\n' "$n-$s" "$st" "$d"
}
export -f run_one mnt; export OUT PLATFORM IMAGE TOP XEIJ STAGE FW RS LWIP ZOS SSH_X RCMD_X PZLSRC CPUS
echo "docker-test: ${#SUITES[@]} run(s), $JOBS at a time, image $IMAGE ($PLATFORM), out $OUT"
i=0; for s in "${SUITES[@]}"; do i=$((i + 1)); printf '%02d %s\n' "$i" "$s"; done | xargs -P "$JOBS" -L 1 bash -c 'run_one "$0" "$1"'
fails=$(cat "$OUT"/*/status | grep -vc '^PASS$')
grep -h -E "^(PASS|FAIL|SKIP)" "$OUT"/*/run.log 2>/dev/null | sort | uniq -c | sort -rn | head -40
echo "docker-test: $fails of ${#SUITES[@]} run(s) did not pass (FAIL or SKIP)  ($OUT)"
exit $((fails > 0))
