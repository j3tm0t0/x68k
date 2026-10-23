# SAN=-fsanitize=address で ASan 入り
# harness/picosim に tsp_svc + lwIP を載せる。lwIP と libslirp は tcp_input などの名前がぶつかるので、
# lwIP・tsp_svc・tsp_glue を 1 個の .o にまとめて外に名前を出さない (ld -r、登録は constructor なので名前は要らない)
#   make            -> ../../../../harness/picosim/build/picosim (tis.x -p の相手入り)
LWIP ?= $(HOME)/src/github.com/yunkya2/x68kzremotedrv/pico-sdk/lib/lwip
L = $(LWIP)/src
LWIPSRC = $(addprefix $(L)/,core/def.c core/dns.c core/inet_chksum.c core/init.c core/ip.c core/mem.c core/memp.c \
	core/netif.c core/pbuf.c core/stats.c core/sys.c core/tcp.c core/tcp_in.c core/tcp_out.c core/timeouts.c core/udp.c \
	core/ipv4/acd.c core/ipv4/etharp.c core/ipv4/icmp.c core/ipv4/ip4.c core/ipv4/ip4_addr.c core/ipv4/ip4_frag.c \
	netif/ethernet.c)
HARNESS = ../../../../harness/picosim
INC = -I. -I.. -I../.. -I$(HARNESS) -I$(L)/include -I$(LWIP)/contrib/ports/unix/port/include
OBJ = build/tsp_all.o

all: $(HARNESS)/build/picosim

$(OBJ): tsp_glue.c lwipopts.h ../tsp_svc.c ../tsp_svc.h ../tsp_lwipopts.h ../../tsp.h
	rm -rf build && mkdir -p build/o
	cd build/o && for f in $(abspath tsp_glue.c ../tsp_svc.c) $(LWIPSRC); do \
	  cc -O2 -g -w $(SAN) -DTSP_PLAT_HOST $(addprefix -I$(CURDIR)/,. .. ../..) -I$(abspath $(HARNESS)) -I$(L)/include \
	    -I$(LWIP)/contrib/ports/unix/port/include -c $$f || exit 1; done
	: > build/exports
	ld -r -o $@ build/o/*.o -exported_symbols_list build/exports

$(HARNESS)/build/picosim: $(OBJ) $(HARNESS)/picosim.c $(HARNESS)/netpipe.c $(HARNESS)/lan_slirp.c
	rm -f $@ && $(MAKE) -C $(HARNESS) build/picosim SVC=$(abspath $(OBJ)) $(if $(SAN),CFLAGS="-O1 -g -Wall $(SAN)")

clean:
	rm -rf build
.PHONY: all clean
