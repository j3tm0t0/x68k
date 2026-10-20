# ~/.bashrc の末尾に追加する (Raspberry Pi 側)
# X68000 (MuTERM, TERM=ansi) からのシリアルログイン時は readline の bracketed paste を切る
# (MuTERM は ESC [ ? 2004 h を解釈できず "2004h" と表示してしまう)
case "$TERM" in ansi) bind 'set enable-bracketed-paste off' 2>/dev/null ;; esac
