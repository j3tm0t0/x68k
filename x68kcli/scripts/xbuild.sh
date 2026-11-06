#!/usr/bin/env bash
# GNU sed / grep / coreutils / findutils / diffutils / gzip を elf2x68k (brew install yunkya2/tap/elf2x68k) で Human68k 用にクロスビルドし build/x/ に置く
# elf2x68k が無ければ何もしない (stage.sh は build/x/*.x があるときだけイメージに入れる)
set -euo pipefail
cd "$(dirname "$0")/.."
command -v m68k-xelf-gcc >/dev/null || { echo "xbuild: m68k-xelf-gcc が無いので GNU の道具はスキップ"; exit 0; }
R=$PWD; W=$R/work/xbuild; mkdir -p "$W" build/x build/x/gnu
# newlib に無い関数 (src/xshim)。関数ごとの節で組み、--gc-sections で使う分だけ残す
# (ライブラリにしないのは、libc の _fstat_r が後から __wrap_fstat を求めるため)
m68k-xelf-gcc -Os -ffunction-sections -fdata-sections -Wall -I"$R/src/xshim/include" -c src/xshim/x68shim.c -o "$W/x68shim.o"
# 再配置を自分で当てる入口 (src/xshim/x68reloc.S)。どのプログラムもここから始め、組んだ後に scripts/fastreloc.py で表を移す
m68k-xelf-gcc -m68000 -c src/xshim/x68reloc.S -o "$W/x68reloc.o"
# termcap の関数 (tgetent など) も x68shim.o にある。less の configure は -ltermcap が見つからないと止まるので、空の libtermcap.a を置く
mkdir -p "$W/lib"; echo 'int x68_termcap_dummy;' > "$W/lib/dummy.c"
m68k-xelf-gcc -c "$W/lib/dummy.c" -o "$W/lib/dummy.o"; rm -f "$W/lib/libtermcap.a"; m68k-xelf-ar rcs "$W/lib/libtermcap.a" "$W/lib/dummy.o"
# libtool を使うもの (file の libmagic) は LIBS に .o を書くと断るので、ライブラリにして丸ごと入れる
rm -f "$W/lib/libx68shim.a"; m68k-xelf-ar rcs "$W/lib/libx68shim.a" "$W/x68shim.o" "$W/x68reloc.o"
# -fexec-charset は elf2x68k の gcc が後ろに cp932 を足すので変えられない (UTF-8 の文字列を持つ部分は外す)
# Human68k に無い dirent の型と fnmatch の拡張は数だけ合わせる
XCPP="-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -I$R/src/xshim/include -DSUPPORT_NON_GREG_CALENDARS_IN_STRFTIME=false -DDT_BLK=6 -DDT_CHR=2 -DDT_FIFO=1 -DDT_LNK=10 -DDT_SOCK=12 -DFNM_EXTMATCH=32 -D_POSIX_OPEN_MAX=20 -D_POSIX_ARG_MAX=4096 -DSA_RESTART=0"
CONF=(--host=m68k-elf --disable-nls CC=m68k-xelf-gcc AR=m68k-xelf-ar RANLIB=m68k-xelf-ranlib
      "CFLAGS=-Os -ffunction-sections -fdata-sections -Wframe-larger-than=40000" "CPPFLAGS=$XCPP"
      "LDFLAGS=-L$W/lib -Wl,-e,__x68_fastreloc_start -Wl,--gc-sections -Wl,--wrap=stat -Wl,--wrap=fstat -Wl,--wrap=close -Wl,--wrap=strtold -Wl,--wrap=getcwd -Wl,--wrap=open -Wl,--wrap=dup -Wl,--wrap=opendir -Wl,--wrap=closedir -Wl,--wrap=readdir -Wl,--wrap=rewinddir -Wl,--wrap=chdir -Wl,--wrap=exit -Wl,--wrap=read -Wl,--wrap=write -Wl,--wrap=poll -Wl,--wrap=main" "LIBS=$W/x68shim.o $W/x68reloc.o"
      # newlib の printf は C99 の大きさ指定・%a・inf/nan・%n に対応している (XEiJ で確かめた)。クロスでは試せず
      # gnulib が「無い」と見て自前の vasnprintf (15KB) を入れるので、あると答える (%b と C23 の %w は使われない前提)
      gl_cv_func_printf_sizes_c99=yes gl_cv_func_printf_sizes_c23=yes gl_cv_func_printf_directive_a=yes
      gl_cv_func_printf_directive_b=yes gl_cv_func_printf_directive_f=yes gl_cv_func_printf_directive_n=yes
      gl_cv_func_printf_flag_zero=yes gl_cv_func_printf_infinite=yes gl_cv_func_printf_infinite_long_double=yes
      gl_cv_func_printf_enomem=yes gl_cv_func_snprintf_directive_n=yes gl_cv_func_snprintf_retval_c99=yes
      gl_cv_func_snprintf_truncation_c99=yes gl_cv_func_vsnprintf_zerosize_c99=yes
      # utimes と futimes は src/xshim にある。使えると答えないと touch が名前で日時を付け、
      # その後で書き込みで開いた番号を閉じたときに今の時刻で上書きされる
      gl_cv_func_working_utimes=yes
      # diffutils の configure がクロスで試せず止まる
      gl_cv_func_strcasecmp_works=yes
      # getcwd(NULL, 0) は src/xshim の包みで使える。「無い」と見ると gnulib が .. をたどる版に替え、Human68k では動かない (find)
      gl_cv_func_getcwd_null=yes gl_cv_func_getcwd_path_max=yes gl_cv_func_getcwd_abort_bug=no gl_cv_func_getcwd_succeeds_beyond_4k=yes
      # fork は libnosys の ENOSYS だけ。無いと答えると diff3・sdiff は popen (src/xshim の一時ファイル版) で diff を動かす
      ac_cv_func_fork_works=no ac_cv_func_vfork_works=no ac_cv_func_fork=no ac_cv_func_vfork=no)
gbuild() {  # gbuild NAME TARBALL "configure の追加" 出力 (ソース木からの相対パス) ...
  local n=$1 tb=$2 extra=$3 d=$W/$1 o missing=0; shift 3
  for o in "$@"; do [ -s "build/x/gnu/$(basename "$o")" ] || [ -s "build/x/gnu/${GALIAS:-none}" ] || missing=1; done
  [ $missing = 1 ] || return 0
  rm -rf "$d"; mkdir -p "$d"; tar xf "$tb" -C "$d" --strip-components=1
  python3 -I scripts/gnulib_x68k.py pre "$d"
  (cd "$d" && ./configure "${CONF[@]}" $extra ${GEXTRA[@]+"${GEXTRA[@]}"} > configure.log 2>&1) || { echo "xbuild: $n の configure に失敗 ($d/configure.log)"; exit 1; }
  python3 -I scripts/gnulib_x68k.py post "$d"
  # テスト用プログラムはリンクできないものがあるので -k で進め、要るものだけ確かめる
  (cd "$d" && make -j8 -k > make.log 2>&1 || true)
  for o in "$@"; do
    [ -s "$d/$o" ] || { echo "xbuild: $n の $o ができない ($d/make.log)"; exit 1; }
    cp "$d/$o" "build/x/gnu/$(basename "$o")"
  done
  echo "xbuild: $n -> build/x/gnu/ ($# 本)"
}
gbuild sed  dl/gnu/sed-4.9.tar.xz  "--disable-acl --without-selinux --disable-i18n" sed/sed.x
gbuild grep dl/gnu/grep-3.11.tar.xz "--disable-perl-regexp" src/grep.x
cp build/x/gnu/sed.x build/x/gnu/grep.x build/x/
# coreutils: Human68k で意味の無いもの (chown chgrp mkfifo mknod nohup timeout stdbuf chroot install) と
# ファイル名にできない [ は入れない。ls cp mv rm などは ITA TOOLBOX と名前がぶつかる (stage.sh で置き場を分ける)
CU="b2sum base32 base64 basename basenc cat chmod cksum comm cp csplit cut date dd df dir dircolors dirname du echo env
    expand expr factor false fmt fold groups head id join link ln logname ls md5sum mkdir mktemp mv nl nproc numfmt od
    paste pathchk pr printenv printf ptx pwd readlink realpath rm rmdir seq sha1sum sha224sum sha256sum sha384sum
    sha512sum shred shuf sleep sort split stat sum sync tac tail tee test touch tr true truncate tsort tty uname unexpand
    uniq unlink vdir wc whoami yes"
gbuild coreutils dl/gnu/coreutils-9.12.tar.xz "--disable-acl --without-selinux --disable-xattr --disable-libcap --without-openssl --disable-threads" \
  $(for p in $CU; do echo src/$p.x; done)
gbuild findutils dl/gnu/findutils-4.11.0.tar.xz "--disable-acl --without-selinux --disable-threads" find/find.x xargs/xargs.x
# diff3 と sdiff は diff を popen で動かす (一時ファイル)
gbuild diffutils dl/gnu/diffutils-3.12.tar.xz "--disable-threads" src/diff.x src/cmp.x src/diff3.x src/sdiff.x
# gunzip と zcat は gzip と同じもの (名前で動きが変わる。元は sh のスクリプト)
gbuild gzip dl/gnu/gzip-1.15.tar.xz "" gzip.x
cp build/x/gnu/gzip.x build/x/gnu/gunzip.x; cp build/x/gnu/gzip.x build/x/gnu/zcat.x
gbuild patch dl/gnu/patch-2.8.tar.xz "--disable-xattr" src/patch.x
gbuild bc dl/gnu/bc-1.08.1.tar.xz "--without-readline --without-libedit" bc/bc.x dc/dc.x
# less: 端末の処理 (termcap と /dev/tty のキー) は src/xshim にある
gbuild less dl/gnu/less-710.tar.gz "" less.x
# file: 型の表は Z-OS 向けの小さい版 (src/magic/magic) を文字のまま A:/usr/share/misc/magic に置く (stage.sh)。
# .mgc にしないのは、ホスト (リトルエンディアン) で作った .mgc を m68k の file が読めなかったため。2.5KB なので起動時に読んでも速い
# newlib に POSIX の regex が無いので、grep を組んだときの gnulib の regex (rpl_regcomp など) を借りる
if [ ! -s build/x/gnu/file.x ]; then
  [ -f "$W/grep/lib/libgreputils.a" ] || { rm -f build/x/gnu/grep.x; gbuild grep dl/gnu/grep-3.11.tar.xz "--disable-perl-regexp" src/grep.x; }
  mkdir -p "$W/regex"
  { for f in regcomp regexec regerror regfree; do echo "#define $f rpl_$f"; done; cat "$W/grep/lib/regex.h"; } > "$W/regex/regex.h"
fi
GEXTRA=("CPPFLAGS=$XCPP -I$W/regex" "LIBS=-Wl,--whole-archive,$W/lib/libx68shim.a,--no-whole-archive $W/grep/lib/libgreputils.a")
gbuild file dl/gnu/file-5.46.tar.gz "--disable-shared --enable-static --disable-zlib --disable-bzlib --disable-xzlib --disable-zstdlib --disable-lzlib --disable-libseccomp --without-pic --datadir=A:/usr/share" src/file.x
GEXTRA=()
# vim 9 (tiny)。端末は src/xshim の termcap と、tcsetattr で生にした標準入力 (select / poll でキーを待つ)。
# クロスでは configure が試せない項目を答えておく。:! は fork が無いので system() (src/xshim の $SHELL 版) で動かす
GEXTRA=(vim_cv_toupper_broken=no vim_cv_terminfo=no vim_cv_tgetent=zero vim_cv_getcwd_broken=no
        vim_cv_stat_ignores_slash=no vim_cv_memmove_handles_overlap=yes vim_cv_bcopy_handles_overlap=yes
        vim_cv_memcpy_handles_overlap=no vim_cv_tty_group=world vim_cv_tty_mode=0620
        ac_cv_sizeof_int=4 ac_cv_sizeof_long=4 ac_cv_sizeof_time_t=8 ac_cv_sizeof_off_t=4 ac_cv_small_wchar_t=no
        vim_cv_timer_create=no vim_cv_timer_create_with_lrt=no
        vim_cv_uname_output=Human68k vim_cv_uname_r_output=3.02 vim_cv_uname_m_output=m68k)
GALIAS=vim9.x gbuild vim dl/gnu/vim-9.2.1302.tar.gz "--with-features=tiny --disable-gui --without-x --with-tlib=termcap --disable-nls --disable-acl --disable-gpm --disable-sysmouse --disable-channel --disable-netbeans --disable-terminal --disable-libsodium --disable-xattr --disable-selinux --disable-smack --disable-canberra --enable-multibyte --disable-darwin --datadir=/usr/share" src/vim.x
GEXTRA=()
# vi / vim は vim 3.0 (stage.sh) なので、こちらは vim9.x の名前で置く
[ -s build/x/gnu/vim.x ] && mv build/x/gnu/vim.x build/x/gnu/vim9.x
# 再配置の表を、Human68k に当てさせずに自分で当てる形にする (起動が速くなる。scripts/fastreloc.py の頭に理由)
python3 -I scripts/fastreloc.py build/x/gnu/*.x | grep -v ": already$\|: ok " || true
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
