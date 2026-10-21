# vt100 エミュレータ -- X68000 用 vtcat.x と、Mac 上でパーサを試す vthost をビルドする
CROSS   = m68k-xelf-
CC      = $(CROSS)gcc
CFLAGS  = -O2 -m68000 -fomit-frame-pointer -Wall -Wno-array-bounds -finput-charset=utf-8 -fexec-charset=cp932
SRC     = vtcat.c vt100.c vtdump.c

all: vtcat.x

# UTF-8 -> JIS X0208 変換表 (コミット済み。genkanji.py を変えたら再生成される)
kanjitab.h: genkanji.py
	python3 -I genkanji.py > $@

# 絵文字表 emojitab.h はコミット済み (Twemoji PNG を genemoji.py で 16x16・16色に変換)。
# 絵文字を足すときは Twemoji の <コードポイント>.png を集めて再生成する (要 Pillow):
#   python3 genemoji.py <PNGディレクトリ> > emojitab.h

vtcat.x: $(SRC) vt100.h kanjitab.h emojitab.h
	$(CC) $(CFLAGS) -o $@ $(SRC) -s

# Mac 上でパーサ・スクロール・属性だけを pyte と比べる (VRAM/IOCS はスタブ)
HOSTCC  = cc
vthost: $(SRC) vt100.h kanjitab.h emojitab.h
	$(HOSTCC) -O2 -Wall -DVT_HOSTTEST -o $@ $(SRC)

clean:
	rm -f vtcat.x vtcat.x.elf* vthost
.PHONY: all clean
