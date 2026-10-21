ECHO OFF
PATH A:\;A:\SYS;A:\BIN;A:\USR\BIN;A:\ETC;A:\XC\BIN;A:\XC\CC;A:\USR\GCC295;
REM --- 21 文字ファイル名 / マルチピリオド / 特殊文字 (大文字小文字は同一視) ---
\USR\BIN\TwentyOne.x +TPS -C
REM --- UNIX 系ツール共通の環境変数 ---
SET USER=x68k
SET HOME=@HOMEDIR@
SET TERM=x68000
SET TERMCAP=A:/etc/termcap
SET temp=A:/tmp
SET TEMP=A:/tmp
SET EDITOR=vim.x
SET MAKE_SHELL=A:/usr/bin/zsh.x
SET SHELL=A:/usr/bin/zsh.x
REM --- PPP / TCP/IP (ppp.x は %PPP%\conf.ppp、xip.x は %SYSROOT%\etc を読む) ---
SET PPP=A:\ETC
SET SYSROOT=A:\
SET HOST=x68k
REM NETMODE / NETSPEED / NETZIP は make image の NET= / NETSPEED= / NETZIP= で埋め込む (zshrc の netup が見る)
SET NETMODE=@NETMODE@
SET NETSPEED=@NETSPEED@
SET NETZIP=@NETZIP@
REM --- C コンパイラ (gcc 真里子版 + LIBC + HAS/HLK) ---
SET include=A:/usr/include
SET lib=A:/usr/lib
SET GCC_AS=has.x
SET GCC_LINK=hlk.x
SET GCC_LIB=.a
SET GCC_NO_XCLIB=yes
SET HLK=-L A:/usr/lib
REM --- 起動時の自動接続 (zshrc の netauto が見る。Pi の pppd が居なければ 10 秒で諦める) ---
SET NETAUTO_BOOT=1
REM --- GraySaver.X: 5 分操作が無いと星が奥へ流れる (zsh の saver で変更、saver -r で解除) ---
\USR\LIB\GRAYSAVER\GraySaver.x -t5 rstar
REM --- ログインシェル (抜けると COMMAND.X に戻る) ---
\USR\BIN\zsh.x -l
