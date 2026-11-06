" Z-OS の vim 9 (vim9.x、tiny) の既定。tiny には式 (eval) が無いので set だけ書く。
" /usr/share/vim/vim92/defaults.vim に置く (~/.vimrc が無いときに読む)
set nocompatible
" Shift_JIS のまま扱う (2 バイト文字を 1 字として動く・消す)
set encoding=cp932
" CR LF のファイルは CR LF のまま書く
set fileformats=dos,unix
set backspace=indent,eol,start
set ruler showcmd
" TwentyOne.x が無くても .swp と予備を 8+3 の名前で作る
set shortname
