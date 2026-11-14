# tis.x の試験プログラム (libsocket でビルド。tis.x でも xip.x でも動く)
CROSS  = m68k-xelf-
CC     = $(CROSS)gcc
CFLAGS = -m68000 -O2 -fomit-frame-pointer -Wall -finput-charset=utf-8 -fexec-charset=cp932
LDLIBS = -lsocket

all: tistest.x tisrun.x

%.x: %.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

# ホスト (Mac) で API 層とループバックを試す: make host && ./hostsim [乱数の回数]
host: hostsim.c ../tis.c ../lo.c ../be.h
	cc -O1 -Wall -Wno-unused-function -I hostinc hostsim.c ../lo.c -o hostsim

# Pico backend (../pico.c) と Pico のソケット窓口 (../pico/tsp_svc.c) を本物の lwIP (pico-sdk の lib/lwip) で:
#   make picosim && ./picosim      (LWIP= で lwIP の場所を変えられる)
LWIP ?= $(HOME)/src/github.com/yunkya2/x68kzremotedrv/pico-sdk/lib/lwip
LWIPSRC = $(addprefix $(LWIP)/src/core/,def.c dns.c inet_chksum.c init.c ip.c mem.c memp.c netif.c pbuf.c stats.c \
	sys.c tcp.c tcp_in.c tcp_out.c timeouts.c udp.c ipv4/icmp.c ipv4/ip4.c ipv4/ip4_addr.c ipv4/ip4_frag.c)
LWIPINC = -I ../pico/host -I ../pico -I .. -I $(LWIP)/src/include -I $(LWIP)/contrib/ports/unix/port/include
picosim: ../pico/host/lwipopts.h ../pico/tsp_lwipopts.h picosim.c picoworld.c picoworld.h ../tis.c ../lo.c ../pico.c ../be.h ../tsp.h ../pico/tsp_svc.c ../pico/tsp_svc.h ../pico/tsp_lwipopts.h
	cc $(SAN) -O1 -g -Wall -Wno-unused-function -I hostinc picosim.c ../lo.c ../pico.c -c
	cc $(SAN) -O1 -g -Wall -DTSP_PLAT_HOST $(LWIPINC) picoworld.c ../pico/tsp_svc.c -c
	cc -O1 -w -DTSP_PLAT_HOST $(LWIPINC) $(LWIPSRC) -c
	cc $(SAN) -o $@ *.o
	rm -f *.o

# BlueSCSI v2 (Pico 2 W) の値で同じ試験: make picosim-bs && ./picosim-bs
BSDEFS = -DTSP_BS -DTSP_NPS=8 -DTSP_LAST_SIZE=1504 -DTSP_TXPOOL=2048 -DTSP_PCB_WND=5840 -DTSP_RX_COPY=256
picosim-bs: ../pico/host/lwipopts.h ../pico/tsp_lwipopts.h picosim.c picoworld.c picoworld.h ../tis.c ../lo.c ../pico.c ../be.h ../tsp.h ../pico/tsp_svc.c ../pico/tsp_svc.h
	mkdir -p bsobj
	cd bsobj && cc $(SAN) -O1 -g -Wall -Wno-unused-function $(BSDEFS) -I ../hostinc ../picosim.c ../../lo.c ../../pico.c -c
	cd bsobj && cc $(SAN) -O1 -g -Wall -DTSP_PLAT_HOST $(BSDEFS) $(subst -I ..,-I ../..,$(LWIPINC)) ../picoworld.c ../../pico/tsp_svc.c -c
	cd bsobj && cc -O1 -w -DTSP_PLAT_HOST $(BSDEFS) $(subst -I ..,-I ../..,$(LWIPINC)) $(LWIPSRC) -c
	cc $(SAN) -o $@ bsobj/*.o
	rm -rf bsobj

clean:
	rm -f *.x *.o *.elf* hostsim picosim picosim-bs
.PHONY: all clean
