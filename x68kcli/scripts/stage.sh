#!/usr/bin/env bash
# dl/ のアーカイブを展開し、HDD イメージに書き込むディレクトリツリー stage/ を作る
set -euo pipefail
cd "$(dirname "$0")/.."
chmod -R u+rwX stage work 2>/dev/null || true   # tar 展開物に書き込み不可のディレクトリがある
rm -rf stage work
mkdir -p stage/USR/BIN stage/USR/DOC stage/ETC stage/HOME stage/TMP work

x() { # x ARCHIVE DIR : アーカイブを work/DIR に展開
  local a="$1" d="work/$2"; mkdir -p "$d"
  case "$a" in
    *.lzh|*.LZH|*.Lzh) (cd "$d" && lha xqf "$OLDPWD/$a");;
    *.tgz)       tar xzf "$a" -C "$d";;
    *.zip|*.ZIP) unzip -oq "$a" -d "$d";;
    *) echo "unknown archive: $a"; exit 1;;
  esac
}
crlf() { python3 -I scripts/totext.py --crlf "$1" "$2"; } # UTF-8 -> Shift_JIS, CRLF, 末尾 ^Z
sjis() { python3 -I scripts/totext.py "$1" "$2"; }        # UTF-8 -> Shift_JIS (LF のまま)

# --- Human68k 3.02 システムファイル (XDF から rb-cli で取り出す。大文字名を保つため lha は使わない) ---
mkdir -p work/human
[ -s work/human/human302.xdf ] || (cd work/human && lha xqf ../../dl/human/HUMN302I.LZH human302.xdf)
tools/rb-cli ls work/human/human302.xdf '/**' | awk '$1=="FILE"{print $NF}' | while read -r f; do
  mkdir -p "stage/$(dirname "$f")"
  tools/rb-cli get work/human/human302.xdf "$f" "stage/$f" >/dev/null
done

# --- system files (Sharp のテンプレートを元にした独自版で上書き) ---
# KEYMAP=us なら KeyWitch を US 配列設定で CONFIG.SYS から常駐させる
if [ "${KEYMAP:-jis}" = us ]; then
  { cat templates/CONFIG.SYS; echo 'DEVICE    = \USR\BIN\KeyWitch.x -x -e \ETC\KEYWITCH\us.env'; } > work/CONFIG.SYS
else
  cp templates/CONFIG.SYS work/CONFIG.SYS
fi
# NET=slip / npd なら ppp.sys の代わりに slip.sys / npd.sys を組み込む (どれも /dev/ep0 なので 1 つだけ)
if [ "${NET:-slip}" = slip ] || [ "${NET:-slip}" = npd ]; then
  sed -i.bak "s/^DEVICE    = \\\\SYS\\\\ppp.sys/DEVICE    = \\\\SYS\\\\${NET:-slip}.sys/" work/CONFIG.SYS && rm -f work/CONFIG.SYS.bak
fi
# PROFILE=dev: RSDRV.SYS の直後に contee.sys (画面の出力を RS-232C にも流す) を組み込み、
#   以降のドライバの組み込みメッセージや画面で止まったときの表示 (エラーのダイアログなど) をシリアルで見られるようにする。
#   IOCS.X は画面出力の IOCS を自分の版に置き換えるので、contee.sys より前 (最初のドライバ) に移す。
#   contee.sys を RSDRV.SYS より前に置くと、RSDRV.SYS が 9600bps に戻してしまい化ける
if [ "${PROFILE:-cli}" = dev ] && [ -s build/x/contee.sys ]; then
  awk '/^DEVICE    = \\SYS\\IOCS\.X/ {next}
       /^DEVICE/ && !d {print "DEVICE    = \\SYS\\IOCS.X"; d = 1}
       {print}
       /^DEVICE    = \\SYS\\RSDRV\.SYS/ {print "DEVICE    = \\SYS\\contee.sys"}' work/CONFIG.SYS > work/CONFIG.dev && mv work/CONFIG.dev work/CONFIG.SYS
fi
crlf work/CONFIG.SYS        stage/CONFIG.SYS
sed -e "s/@NETMODE@/${NET:-slip}/" -e "s/@NETSPEED@/${NETSPEED:-38400}/" -e "s/@NETZIP@/${NETZIP:-}/" -e "s|@HOMEDIR@|${HOMEDIR:-A:/home}|" templates/AUTOEXEC.BAT > work/AUTOEXEC.BAT
# PROFILE=dev: 開発用。自動接続 / rcmd / GraySaver を起動せず、COMMAND.X のコンソールを RS-232C (38400bps) に出す
#   (Pi の screen /dev/ttyS0 38400 で操作する。Z の画面に戻すには CTTY CON。ダイアログは Z の画面に出る)
if [ "${PROFILE:-cli}" = dev ]; then
  awk '/NETAUTO_BOOT|自動接続|GraySaver/ {next}
       /^REM --- ログインシェル/ {print "REM --- dev: コンソールを RS-232C (38400bps) にする。戻すには CTTY CON ---"; print "\\USR\\BIN\\rsspeed.x 4C09"; print "CTTY AUX"; skip=1; next}
       skip && /zsh\.x/ {skip=0; next} {print}' work/AUTOEXEC.BAT > work/AUTOEXEC.dev && mv work/AUTOEXEC.dev work/AUTOEXEC.BAT
fi
crlf work/AUTOEXEC.BAT stage/AUTOEXEC.BAT
crlf templates/STARTUP.ENV  stage/STARTUP.ENV
for f in zshenv zshrc fishrc profile kshrc zoshrc; do sjis templates/etc/$f stage/ETC/$f; done
# ホームディレクトリのサンプル (hello.c など)
for f in templates/home/*; do crlf "$f" "stage/HOME/$(basename "$f")"; done

# --- KeyWitch (キー配置変更。US 配列キーボード対応) ---
x dl/misc/KEYWB213.LZH kw; x work/kw/KeyWEnv.Lzh kwenv
cp work/kw/KeyWitch.x stage/USR/BIN/
mkdir -p stage/ETC/KEYWITCH stage/USR/DOC/KEYWITCH
cp work/kwenv/*.env stage/ETC/KEYWITCH/
sjis templates/etc/keywitch/us.env stage/ETC/KEYWITCH/us.env
cp work/kw/KeyWitch.doc work/kw/KWTech.doc stage/USR/DOC/KEYWITCH/

# --- TwentyOne ---
x dl/misc/tw136c14.lzh tw
cp work/tw/TW136C14/TwentyOne.x stage/USR/BIN/
mkdir -p stage/USR/DOC/TWENTYONE && cp work/tw/TW136C14/VTwentyOne.doc stage/USR/DOC/TWENTYONE/

# --- zsh ---
x dl/zsh/ZSHX607B.LZH zsh; x dl/zsh/ZSHX607M.LZH zshm
cp work/zsh/zsh.x stage/USR/BIN/
cp work/zsh/termcap stage/ETC/termcap
mkdir -p stage/USR/DOC/ZSH/samples
cp work/zsh/readme.x68 work/zsh/Changes.x68 stage/USR/DOC/ZSH/
cp work/zsh/ONOzshrc work/zsh/ONOzshenv work/zsh/ONOzlogin work/zsh/ONOzlogout stage/USR/DOC/ZSH/samples/
# help/ (88 ファイル) は rb-cli のフォーマッタ使用時にクラスタのクロスリンクが出たため除外 (mkhds.py では未再検証。zsh.1 に同内容あり)
cp -R work/zshm/doc work/zshm/examples work/zshm/etc stage/USR/DOC/ZSH/
cp work/zshm/README.jp stage/USR/DOC/ZSH/

# --- fish ---
x dl/fish/fish080.lzh fish
cp work/fish/fish.x stage/USR/BIN/
mkdir -p stage/USR/DOC/FISH && cp work/fish/README work/fish/FISH.DOC work/fish/FAQ work/fish/Pfishrc work/fish/Plogin work/fish/Plogout stage/USR/DOC/FISH/

# --- ksh ---
x dl/ksh/KSHX26F.LZH ksh
cp work/ksh/ksh.x stage/USR/BIN/
mkdir -p stage/USR/DOC/KSH && cp work/ksh/readme.x68 work/ksh/ksh.man stage/USR/DOC/KSH/

# --- vim ---
x dl/vim/vim3x687.lzh vim
cp work/vim/vim.x work/vim/vim.hlp stage/USR/BIN/
mkdir -p stage/USR/DOC/VIM && cp work/vim/VimX68K.doc work/vim/README stage/USR/DOC/VIM/

# --- GNU tools ---
x dl/misc/gawk2154.tgz gawk
cp work/gawk/gawk.x stage/USR/BIN/
cp work/gawk/gawk.x stage/USR/BIN/awk.x   # awk でも呼べるように (rcmd の zsh -c や Makefile では alias が効かない)
mkdir -p stage/USR/DOC/GAWK && cp work/gawk/README.68k work/gawk/gawk.1 stage/USR/DOC/GAWK/
x dl/misc/MAKEHU11.ZIP make
cp work/make/make-3.77-human-1.1/make.x stage/USR/BIN/
mkdir -p stage/USR/DOC/MAKE && cp work/make/make-3.77-human-1.1/README.X68 stage/USR/DOC/MAKE/
x dl/misc/tgz13x16.tgz tgz
cp work/tgz/tar.x stage/USR/BIN/
mkdir -p stage/USR/DOC/TAR && cp work/tgz/README.x68 work/tgz/tgz.doc work/tgz/tgz.man stage/USR/DOC/TAR/
cp dl/misc/LHA_X662.x stage/USR/BIN/lha.x

# --- MuTERM (ターミナルソフト。RS-232C 経由で UNIX ホストにログインする) ---
# MUTERM.CNF は MUTERM.X と同じディレクトリから読まれる (終了時に書き戻される)
x dl/misc/MUTERM.LZH muterm
cp work/muterm/MUTERM.X stage/USR/BIN/muterm.x
crlf templates/MUTERM.CNF stage/USR/BIN/MUTERM.CNF
mkdir -p stage/USR/DOC/MUTERM && cp work/muterm/MUTERM.DOC work/muterm/MUTERM.MAN work/muterm/ESC.DOC stage/USR/DOC/MUTERM/

# --- スクリーンセーバー: GraySaver.X 1.23 (M.Kamada) + 外部サブルーチン (星 / ブロック崩し / メルトダウン) ---
# GraySaver はサブルーチンを自分のディレクトリからも探すので、まとめて /usr/lib/graysaver に置く (zshrc の saver 関数で起動)
x dl/misc/GRAYS123.LZH graysaver; x dl/misc/GSSMPL1.LZH graysaver2
mkdir -p stage/USR/LIB/GRAYSAVER stage/USR/DOC/GRAYSAVER
cp work/graysaver/GraySaver.x work/graysaver/star.x work/graysaver/rstar.x work/graysaver/rstar1.x \
   work/graysaver2/BLOCK1.X work/graysaver2/MELT1.X stage/USR/LIB/GRAYSAVER/
cp work/graysaver/GraySaver.doc work/graysaver2/GSSMPL1.DOC stage/USR/DOC/GRAYSAVER/

# --- PPP / TCP/IP (RS-232C 経由で Raspberry Pi などと TCP/IP 接続する) ---
# RS-232C ドライバ: tmsio.x 0.31 (フロー制御をソフトで選べる。X68000 Z の 3 線接続向け既定) / bsio.x 0.21 (軽量だが RTS/CTS 固定)
x dl/net/tmsio031.lzh tmsio; x dl/net/BSIO021.LZH bsio
cp work/tmsio/tmsio.x work/bsio/bsio.x stage/USR/BIN/
mkdir -p stage/USR/DOC/TMSIO stage/USR/DOC/BSIO
cp work/tmsio/tmsio.doc work/tmsio/update.doc stage/USR/DOC/TMSIO/; cp work/bsio/bsio.doc stage/USR/DOC/BSIO/
# IIJ-PPP on Human68k Rel.5: ppp.sys (ether_plus.sys 互換ドライバ、CONFIG.SYS で常駐) + ppp.x (本体)
x dl/net/pppxr5.lzh ppp
cp work/ppp/ppp.sys stage/SYS/; cp work/ppp/ppp.x work/ppp/kill.x stage/USR/BIN/
mkdir -p stage/USR/DOC/PPP
cp work/ppp/readme.doc work/ppp/howtouse.doc work/ppp/notice.doc work/ppp/qa.doc work/ppp/tech.doc work/ppp/espx.doc work/ppp/COPYRIGHT stage/USR/DOC/PPP/
cp work/ppp/conf.ppp stage/USR/DOC/PPP/conf_ppp.org
# ESP/X (計測技研 TCPPACKA): ping ftp telnet inetdconf ifconfig netstat arp + etc/protocols services + man
#   TCP/IP 本体は inetd.x の改良版 xip.x を使う (inetd.x も参考に残す)
x dl/net/TCPPACKA.LZH tcpa; x dl/net/xipb5.lzh xip
cp work/tcpa/bin/*.x stage/USR/BIN/
# XIPMSS (既定 1460) / XIPTIMER (既定 1、掃除の間引き): パッチした xip.x を入れる (改変済みのバイナリは配布せず、ここで元の xip.x から作る)
if [ "${XIPMSS:-1460}" = 512 ]; then
  cp work/xip/xip.x stage/USR/BIN/
else
  python3 ../net/patchxip.py work/xip/xip.x stage/USR/BIN/xip.x --mss "${XIPMSS:-1460}" $([ "${XIPTIMER:-1}" = 1 ] && echo --timer) $([ "${XIPLOCK:-}" = fix ] && echo --lockfix) $([ "${XIPYIELD:-1}" = 1 ] && echo --yield)
fi
cp work/tcpa/etc/protocols work/tcpa/etc/services stage/ETC/
mkdir -p stage/USR/DOC/ESPX stage/USR/DOC/XIP
cp work/tcpa/README_A.doc work/tcpa/man/* stage/USR/DOC/ESPX/; cp work/xip/xip.doc work/xip/xip.his stage/USR/DOC/XIP/
# ESP/X PACK-B (ソケットライブラリ): libnetwork.a libether.a -> /USR/LIB, network.h socket.h 等 -> /USR/INCLUDE
x dl/net/TCPPACKB.LZH tcpb
mkdir -p stage/USR/LIB stage/USR/INCLUDE
cp work/tcpb/lib/*.a stage/USR/LIB/; cp work/tcpb/include/*.h stage/USR/INCLUDE/
cp work/tcpb/README_B.doc work/tcpb/man/*.man stage/USR/DOC/ESPX/
mkdir -p stage/USR/SRC/ESPX && cp -R work/tcpb/src/. stage/USR/SRC/ESPX/
# 自作ツールのソース (../harness/rcmd, ../net/pppwait, ../net/rsspeed) -> /USR/SRC/<NAME>。X68000 上で make してビルドする (zsh の netup / rcmd が初回に自動ビルド)
#   rcmd    : リモートコマンド実行サーバ (ESP/X ソケット)
#   rsspeed : IOCS _SET232C 直接呼び出し (X68000 Z で UART を 38400 にする)
#   (../net/slip, ../net/slipd, src/xshim は Mac 側で elf2x68k でクロスビルドするもの。X68000 上では作れないので入れない)
for d in ../harness/rcmd ../net/pppwait ../net/rsspeed; do
  u=$(basename "$d" | tr '[:lower:]' '[:upper:]')
  mkdir -p "stage/USR/SRC/$u"
  for f in $d/*.c $d/Makefile; do crlf "$f" "stage/USR/SRC/$u/$(basename "$f")"; done
done
# 設定ファイル (hosts / networks / conf.ppp はテンプレートから)
for f in conf.ppp linkup.ppp hosts networks; do crlf templates/etc/$f stage/ETC/$f; done

# --- C コンパイラ ---
# LIBC 1.1.32 (+ DON 版パッチ 4 を上書き) -> /USR/INCLUDE, /USR/LIB
x dl/cc/libc32b.tgz libc; chmod -R u+rwX work/libc
mkdir -p work/libcdon && tar xzf dl/cc/libcd4.tgz -C work/libcdon
# tgz 内のディレクトリが読み取り専用で作られるため、展開ごとに権限を直す
(cd work/libcdon && tar xzf include.tgz && chmod -R u+rwX . && tar xzf lib.tgz && chmod -R u+rwX .)
mkdir -p stage/USR/INCLUDE stage/USR/LIB stage/USR/DOC/LIBC
cp -R work/libc/include/. stage/USR/INCLUDE/
cp work/libc/lib/*.a stage/USR/LIB/
cp -R work/libcdon/libc-d4/include/. stage/USR/INCLUDE/
cp work/libcdon/libc-d4/lib/*.a stage/USR/LIB/
cp work/libc/README work/libc/CHANGELOG stage/USR/DOC/LIBC/
cp work/libcdon/README.TXT work/libcdon/NEWS.TXT stage/USR/DOC/LIBC/
# HAS.X / HLK.X (gcc 用アセンブラ・リンカ)
x dl/cc/has309.lzh has; x dl/cc/HLK301B.LZH hlk
cp work/has/HAS.X stage/USR/BIN/has.x; cp work/hlk/hlk301.x stage/USR/BIN/hlk.x
mkdir -p stage/USR/DOC/HAS stage/USR/DOC/HLK
cp work/has/README.DOC work/has/HAS.MAN stage/USR/DOC/HAS/; cp work/hlk/readme.doc stage/USR/DOC/HLK/
# GCC 1.42 真里子版 (既定の gcc) -> /USR/BIN
x dl/cc/GCC142.LZH gcc142
cp work/gcc142/gcc.x work/gcc142/gcc_cc1.x work/gcc142/gcc_cpp.x stage/USR/BIN/
mkdir -p stage/USR/DOC/GCC142 && cp work/gcc142/GCC.DOC work/gcc142/GCC.HLP work/gcc142/CHANGE.DOC stage/USR/DOC/GCC142/
# GCC 2.95.2 beta4 -> /USR/GCC295 (gcc295 関数で呼ぶ。g2as/g2lk は GCC2 キットから)
x dl/cc/G295b04D.ZIP g295d; x dl/cc/G295b04C.ZIP g295c; x dl/cc/G3_20.LZH g320
mkdir -p stage/USR/GCC295/INCLUDE/SYS stage/USR/DOC/GCC295
cp work/g295d/gcc.x work/g295d/cpp.x work/g295d/gcov.x work/g295d/libiberty.a work/g295c/cc1.x stage/USR/GCC295/
cp work/g320/g2as.x stage/USR/GCC295/                       # アセンブラは GCC2 付属の g2as
mkdir -p work/hlkev && unzip -oq dl/cc/hlkev15.zip -d work/hlkev
cp work/hlkev/hlk.r stage/USR/GCC295/g2lk.r                 # リンカは HLK evolution (-l/-L 対応)
cp work/hlkev/hlkev.txt stage/USR/DOC/GCC295/
# ライブラリ: g2lk は環境変数 lib (1 ディレクトリ) しか見ないので /USR/LIB に置く。
#   libgcc.a  = GCC 2.95.2 用, libgcc2.a = GCC2 (2.6.3) 用 (gcc 1.42 真里子版が libc.a の浮動小数点ルーチン解決に使う)
cp work/g295d/libgcc.a stage/USR/LIB/libgcc.a
cp work/g320/libgcc.a stage/USR/LIB/libgcc2.a
cp work/g295d/libc/*.h stage/USR/GCC295/INCLUDE/; cp work/g295d/libc/sys/*.h stage/USR/GCC295/INCLUDE/SYS/
cp work/g295d/README.HUMAN work/g320/gcc2.doc work/g320/as/readme.doc stage/USR/DOC/GCC295/
# XC 2.1 (純正) -> /XC  (許諾条件.txt は Shift_JIS 名のため macOS の lha が失敗するので無視)
mkdir -p work/xc1 work/xc2
(cd work/xc1 && lha xqf ../../dl/cc/XC2101.LZH >/dev/null 2>&1 || true)
(cd work/xc2 && lha xqf ../../dl/cc/XC2102_02.LZH >/dev/null 2>&1 || true)
mkdir -p stage/XC/BIN stage/XC/CC stage/XC/INCLUDE stage/XC/LIB
for f in AS.X LK.X AR.X LIB.X CV.X DB.X SCD.X SCD.CNF SCD.HLP MAKE.X; do cp "work/xc1/BIN/$f" stage/XC/BIN/; done
cp work/xc2/CC/CC.X stage/XC/CC/; cp work/xc2/INCLUDE/* stage/XC/INCLUDE/; cp work/xc2/LIB/* stage/XC/LIB/

# --- GNU sed / grep (scripts/xbuild.sh が elf2x68k でクロスビルドしたもの。無ければ入れない) ---
for f in build/x/sed.x build/x/grep.x; do [ -s "$f" ] && cp "$f" stage/USR/BIN/; done
# slip.sys (SLIP ドライバ。CONFIG.SYS で ppp.sys の代わりに組み込む: NET=slip make image)
[ -s build/x/slip.sys ] && cp build/x/slip.sys stage/SYS/slip.sys
[ -s build/x/slipd.x ] && cp build/x/slipd.x stage/USR/BIN/slipd.x
# npd.sys / npd.x (Pico W の netpipe のブリッジ。NET=npd で CONFIG.SYS に npd.sys を組み込む)
[ -s build/x/npd.sys ] && cp build/x/npd.sys stage/SYS/npd.sys
[ -s build/x/npd.x ] && cp build/x/npd.x stage/USR/BIN/npd.x
[ -s build/x/contee.sys ] && cp build/x/contee.sys stage/SYS/contee.sys
[ -s build/x/rsspeed.x ] && cp build/x/rsspeed.x stage/USR/BIN/rsspeed.x
[ -s build/x/clear.x ] && cp build/x/clear.x stage/USR/BIN/clear.x

# --- ITA TOOLBOX ---
mkdir -p stage/USR/DOC/ITA
for a in dl/ita/*; do
  n=$(basename "$a"); n=${n%.*}
  x "$a" "ita/$n"
  find "work/ita/$n" -name '*.x' -exec cp {} stage/USR/BIN/ \;
  find "work/ita/$n" -name '*.1' -exec cp {} stage/USR/DOC/ITA/ \;
done

echo "stage: $(find stage -type f | wc -l | tr -d ' ') files"
du -sh stage
