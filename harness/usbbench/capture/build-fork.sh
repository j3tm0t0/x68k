#!/bin/bash
# build-fork.sh <fork の作業ツリー> <ビルド済みのディレクトリ> <出力の .elf>: fork (x68kzremotedrv) の採取用ビルド。
# 作業ツリーを一時ディレクトリに写し (元には触らない)、msc_disk.c に ZC の行、zcap.c、CFG_TUSB_DEBUG=2 を足して、
# <ビルド済みのディレクトリ> の CMakeCache と同じ設定 (C_FLAGS・ボード・オプション) でビルドする。ビルドのディレクトリは
# ツリーの中 (driver の目標が ../driver で m68k のドライバを作るため)。
# 例: build-fork.sh ~/src/github.com/yunkya2/x68kzremotedrv-tsp ~/src/github.com/yunkya2/x68kzremotedrv-tsp/build-p2w /tmp/zcap-fork.elf
set -euo pipefail
src=$(cd "$1" && pwd) ref=$(cd "$2" && pwd) out=$3
here=$(cd "$(dirname "$0")" && pwd)
w=$(mktemp -d "${TMPDIR:-/tmp}/forkcap.XXXXXX")
rsync -a --exclude 'build*' --exclude .git "$src/" "$w/s/"
for l in "$w"/s/*; do [ -L "$l" ] && ln -sfn "$(cd "$src" && cd "$(readlink "$src/$(basename "$l")")" && pwd)" "$l"; done
cp "$here/zcap.c" "$w/s/src/zcap.c"
python3 - "$w/s" <<'PY'
import re, sys
d = sys.argv[1]
p = d + "/src/msc_disk.c"; s = open(p).read()
for name, tag in (("tud_msc_read10_cb", "R"), ("tud_msc_write10_cb", "W")):
    m = re.search(r"int32_t %s\(uint8_t lun, uint32_t lba, uint32_t offset, (?:const )?(?:void|uint8_t)\* ?buffer, uint32_t bufsize\)\n\{\n" % name, s)
    s = s[:m.end()] + '  zcap_printf("  ZC %s lun=%%u lba=0x%%lx off=%%lu len=%%lu\\n", lun, (unsigned long)lba, (unsigned long)offset, (unsigned long)bufsize);\n' % tag + s[m.end():]
m = re.search(r"int32_t tud_msc_scsi_cb ?\(uint8_t lun, uint8_t const scsi_cmd\[16\], void\* ?buffer, uint16_t bufsize\)\n\{\n", s)
s = s[:m.end()] + '  zcap_printf("  ZC C lun=%u cdb=%02x %02x %02x %02x %02x %02x\\n", lun, scsi_cmd[0], scsi_cmd[1], scsi_cmd[2], scsi_cmd[3], scsi_cmd[4], scsi_cmd[5]);\n' + s[m.end():]
open(p, "w").write("int zcap_printf(const char *fmt, ...);\n" + s)
open(d + "/CMakeLists.txt", "a").write("\n# usbbench の採取用\ntarget_sources(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/zcap.c)\n"
    "target_compile_definitions(${PROJECT_NAME} PRIVATE CFG_TUSB_DEBUG=2 CFG_TUSB_DEBUG_PRINTF=zcap_printf)\n")
PY
# 元のビルドの設定を写す (BOOL / STRING / PATH の利用者向けの値)
args=()
while IFS= read -r l; do args+=("-D$l"); done < <(grep -E '^(CMAKE_C_FLAGS|CMAKE_BUILD_TYPE|CMAKE_MAKE_PROGRAM|CMAKE_POLICY_VERSION_MINIMUM|PICO_[A-Z_]+|FREERTOS_KERNEL_PATH|[A-Z0-9_]+_ENABLE|BOOTWAIT[A-Z_]*|HID_KEYBOARD|TSP|MSC_LOG|P2W_CORES):(BOOL|STRING|PATH|FILEPATH)=' "$ref/CMakeCache.txt" | grep -v '^PICO_SDK_FETCH')
export PICO_TOOLCHAIN_PATH=${PICO_TOOLCHAIN_PATH:-$(dirname "$(dirname "$(sed -n 's/^CMAKE_C_COMPILER:STRING=//p' "$ref/CMakeCache.txt")")")}
cmake -S "$w/s" -B "$w/s/b" "${args[@]}" > "$w/cmake.log" 2>&1 || { tail -20 "$w/cmake.log"; exit 1; }
cmake --build "$w/s/b" -j8 > "$w/build.log" 2>&1 || { grep -m5 -B5 -E " error|Error" "$w/build.log"; echo "(kept $w)"; exit 1; }
cmp -s <(sed -n 's/^C_FLAGS = //p' "$ref"/CMakeFiles/*.dir/flags.make | head -1) <(sed -n 's/^C_FLAGS = //p' "$w"/s/b/CMakeFiles/*.dir/flags.make | head -1) || echo "build-fork: warning: C_FLAGS differ from $ref"
cp "$w"/s/b/*.elf "$out"; rm -rf "$w"; echo "$out"
