# net/accel -- Z 側の暗号アクセラレータ呼び出し (accel.c) と試験プログラム (acceltest.x)。elf2x68k でクロスビルド
CROSS  = m68k-xelf-
CC     = $(CROSS)gcc
CFLAGS = -m68000 -O2 -fomit-frame-pointer -Wall -finput-charset=utf-8 -fexec-charset=cp932

all: acceltest.x

acceltest.x: acceltest.c accel.c accel_etd.S accel.h accel_proto.h
	$(CC) $(CFLAGS) -o $@ acceltest.c accel.c accel_etd.S

# ホストの試験 (記録形式 + Monocypher の計算を cryptography と差分)
test:
	tests/run.sh

clean:
	rm -f *.o *.x *.elf*
.PHONY: all test clean
