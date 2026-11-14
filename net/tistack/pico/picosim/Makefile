# SAN=-fsanitize=address で ASan 入り
# harness/picosim に tsp_svc + lwIP を載せる。lwIP と libslirp は tcp_input などの名前がぶつかるので、
# lwIP・tsp_svc・tsp_glue を 1 個の .o にまとめて外に名前を出さない (ld -r、登録は constructor なので名前は要らない)
#   make            -> ../../../../harness/picosim/build/picosim (tis.x -p の相手入り)
#   make build-bs/tsp_all.o BS=1 -> tsp_svc を BlueSCSI v2 (Pico 2 W) の大きさで (ソケット 8、応答 1504、送信 2KB、窓 4 x MSS)。
#                     harness/picosim の make build/picosim-bs がこれを使う
LWIP ?= $(HOME)/src/github.com/yunkya2/x68kzremotedrv/pico-sdk/lib/lwip
L = $(LWIP)/src
LWIPSRC = $(addprefix $(L)/,core/def.c core/dns.c core/inet_chksum.c core/init.c core/ip.c core/mem.c core/memp.c \
	core/netif.c core/pbuf.c core/stats.c core/sys.c core/tcp.c core/tcp_in.c core/tcp_out.c core/timeouts.c core/udp.c \
	core/ipv4/acd.c core/ipv4/etharp.c core/ipv4/icmp.c core/ipv4/ip4.c core/ipv4/ip4_addr.c core/ipv4/ip4_frag.c \
	netif/ethernet.c)
HARNESS = ../../../../harness/picosim
INC = -I. -I.. -I../.. -I$(HARNESS) -I$(L)/include -I$(LWIP)/contrib/ports/unix/port/include
D = $(if $(BS),build-bs,build)
OBJ = $(D)/tsp_all.o
BSDEFS = $(if $(BS),-DTSP_BS -DTSP_NPS=8 -DTSP_LAST_SIZE=1504 -DTSP_TXPOOL=2048 -DTSP_PCB_WND=5840 -DTSP_RX_COPY=256)

all: $(HARNESS)/build/picosim

$(OBJ): tsp_glue.c lwipopts.h ../tsp_svc.c ../tsp_svc.h ../tsp_lwipopts.h ../../tsp.h
	rm -rf $(D) && mkdir -p $(D)/o
	cd $(D)/o && for f in $(abspath tsp_glue.c ../tsp_svc.c) $(LWIPSRC); do \
	  cc -O2 -g -w $(SAN) -DTSP_PLAT_HOST $(BSDEFS) $(addprefix -I$(CURDIR)/,. .. ../..) -I$(abspath $(HARNESS)) -I$(L)/include \
	    -I$(LWIP)/contrib/ports/unix/port/include -c $$f || exit 1; done
	: > $(D)/exports
	$(HARNESS)/tools/ld-hide.sh $@ $(D)/exports $(D)/o/*.o

$(HARNESS)/build/picosim: $(OBJ) $(HARNESS)/picosim.c $(HARNESS)/netpipe.c $(HARNESS)/lan_slirp.c
	rm -f $@ && $(MAKE) -C $(HARNESS) build/picosim TSP=$(abspath $(OBJ)) $(if $(SAN),CFLAGS="-O1 -g -Wall $(SAN)")

clean:
	rm -rf build build-bs
.PHONY: all clean
