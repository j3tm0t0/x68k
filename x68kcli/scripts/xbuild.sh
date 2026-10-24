#!/usr/bin/env bash
# GNU sed / grep を elf2x68k (brew install yunkya2/tap/elf2x68k) で Human68k 用にクロスビルドし build/x/ に置く
# elf2x68k が無ければ何もしない (stage.sh は build/x/*.x があるときだけイメージに入れる)
set -euo pipefail
cd "$(dirname "$0")/.."
command -v m68k-xelf-gcc >/dev/null || { echo "xbuild: m68k-xelf-gcc が無いので sed/grep はスキップ"; exit 0; }
R=$PWD; W=$R/work/xbuild; mkdir -p "$W" build/x
m68k-xelf-gcc -O2 -c src/xshim/x68shim.c -o "$W/x68shim.o"
CONF=(--host=m68k-elf --disable-nls CC=m68k-xelf-gcc AR=m68k-xelf-ar RANLIB=m68k-xelf-ranlib
      CFLAGS=-O2 "CPPFLAGS=-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0")
# gnulib の未移植部分 (getprogname / getdtablesize / pipe) と grep の同一ファイル判定 (Human68k は inode が常に同じ) を直す
patch_gnulib() {
  python3 -I - "$1" "$2" <<'PY'
import sys, os
d, prog = sys.argv[1], sys.argv[2]
def ed(p, old, new):
    p = os.path.join(d, p)
    if not os.path.exists(p): return
    s = open(p).read()
    if old in s: open(p, 'w').write(s.replace(old, new, 1))
ed('lib/getprogname.c', '#  error "getprogname module not ported to this OS"', '  return "%s";' % prog)
ed('lib/getdtablesize.c', '  struct rlimit lim;\n\n  if (getrlimit', '#ifndef RLIMIT_NOFILE\n  return 64;\n#else\n  struct rlimit lim;\n\n  if (getrlimit')
ed('lib/getdtablesize.c', '    return lim.rlim_cur;\n\n  return INT_MAX;\n}', '    return lim.rlim_cur;\n\n  return INT_MAX;\n#endif\n}')
ed('lib/pipe.c', '# error "This platform lacks a pipe function, and Gnulib doesn\'t provide a replacement. This is a bug in Gnulib."',
   '# include <errno.h>\nint pipe (int fd[2]) { (void) fd; errno = ENOSYS; return -1; }')
ed('src/grep.c', '      && S_ISREG (st.st_mode) && SAME_INODE (st, out_stat))', '      && 0 && S_ISREG (st.st_mode) && SAME_INODE (st, out_stat))')
PY
}
build() {  # build NAME TARBALL OUT EXTRA_CONFIGURE EXTRA_CFLAGS
  local n=$1 tb=$2 out=$3 extra=$4 cf=$5 d=$W/$1
  [ -s "build/x/$n.x" ] && return
  rm -rf "$d"; mkdir -p "$d"; tar xJf "$tb" -C "$d" --strip-components=1
  (cd "$d" && ./configure "${CONF[@]}" $extra > configure.log 2>&1)
  patch_gnulib "$d" "$n"
  # テスト用プログラムはリンクできないものがあるので、本体だけ作る
  (cd "$d" && make -j8 -k LIBS="$W/x68shim.o" CFLAGS="-O2 $cf" > make.log 2>&1 || true)
  [ -s "$d/$out" ] || { echo "xbuild: $n のビルドに失敗 ($d/make.log)"; exit 1; }
  cp "$d/$out" "build/x/$n.x"; echo "xbuild: build/x/$n.x"
}
build sed  dl/gnu/sed-4.9.tar.xz  sed/sed.x  "--disable-acl --without-selinux --disable-i18n" ""
build grep dl/gnu/grep-3.11.tar.xz src/grep.x "--disable-perl-regexp" "-DDT_BLK=6 -DDT_CHR=2 -DDT_FIFO=1 -DDT_LNK=10 -DDT_SOCK=12 -DFNM_EXTMATCH=32"
# slip.sys (ESP/X 用 SLIP ドライバ, ../net/slip)
make -s -C ../net/slip slip.sys >/dev/null && cp ../net/slip/slip.sys build/x/slip.sys && echo "xbuild: build/x/slip.sys"
m68k-xelf-gcc -O2 ../net/rsspeed/rsspeed.c -o build/x/rsspeed.x && rm -f build/x/rsspeed.x.elf && echo "xbuild: build/x/rsspeed.x"
make -s -C ../net/slipd slipd.x >/dev/null && cp ../net/slipd/slipd.x build/x/slipd.x && echo "xbuild: build/x/slipd.x"
# npd.sys / npd.x (Pico W の netpipe 用ドライバとスレッド, ../net/npd)
make -s -C ../net/npd >/dev/null 2>&1 && cp ../net/npd/npd.sys ../net/npd/npd.x build/x/ && echo "xbuild: build/x/npd.sys npd.x"
# clear.x (画面を消す。zsh の Ctrl+L と同じ ESC [ 2 J。Z-OS のペインではそのペインだけ, src/clear)
m68k-xelf-gcc -m68000 -o build/x/clear.x src/clear/clear.S -nostartfiles -nostdlib -s && rm -f build/x/clear.x.elf && echo "xbuild: build/x/clear.x"
# rcc.x (gcc の前置き: LAN の向こうの elf2x68k の gcc でコンパイルする。つながらなければ gcc.x, ../net/rcc)
make -s -C ../net/rcc rcc.x >/dev/null && cp ../net/rcc/rcc.x build/x/rcc.x && echo "xbuild: build/x/rcc.x"
# contee.sys (画面の出力を RS-232C にも流す。PROFILE=dev の CONFIG.SYS で組み込む, ../harness/contee)
make -s -C ../harness/contee contee.sys >/dev/null 2>&1 && cp ../harness/contee/contee.sys build/x/contee.sys && echo "xbuild: build/x/contee.sys"
