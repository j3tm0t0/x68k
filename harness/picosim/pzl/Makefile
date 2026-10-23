# harness/picosim/pzl -- Pico Z Loader (x68k-zos の branch pzl、pico/) をホストでビルドして picosim --pzl に載せる
#   make            -> build/pzl_all.o (PZL の virtual_disk / storage / config + pzl_host.c。外に出す名前は pzl_* だけ)
# PZL のソースは作業木ではなく、決まったコミット (PZLREV、既定は branch pzl の先) から git archive で写し出す
# (書きかけの作業木に左右されないように)。サブモジュール x68kserremote も記録されたコミットで。
# Z 側の 68000 のバイナリ (driver/*.inc: hdsboot・scsiremote・settingui など) は elf2x68k でここで作る
PZLREPO ?= $(HOME)/Dropbox/src/github.com/j3tm0t0/x68k-zos-pzl
# 既定は試したコミットに留める (pzl が進んでも main の make test が壊れないように)。最新に合わせるかは
#   make -C harness/picosim pzl-latest  (PZLREV=pzl で作って試す) で確かめてから、ここを進める
PZLREV ?= 0995be6
B = build
S = $(B)/src/pico
PZLFILES = src/scsi/virtual_disk.c src/storage/storage.c src/storage/stor_ram.c \
           src/remote/vd_command.c src/remote/iconv/iconv_mini.c lib/x68kserremote/service/remoteserv.c \
           src/netpipe/netpipe.c src/accel/accel.c src/accel/accel_ops.c src/accel/monocypher/monocypher.c \
           src/accel/monocypher/monocypher-ed25519.c src/bootwait/bootwait.c \
           $(if $(wildcard $(S)/src/tsp/tsp_svc.c),src/tsp/tsp_svc.c)
# lwIP (netpipe のブリッジと、Wi-Fi の代わりの netif)。pico-sdk の lib/lwip (net/tistack と同じもの)
LWIP ?= $(HOME)/src/github.com/yunkya2/x68kzremotedrv/pico-sdk/lib/lwip
L = $(LWIP)/src
LWIPSRC = $(addprefix $(L)/,core/def.c core/dns.c core/inet_chksum.c core/init.c core/ip.c core/mem.c core/memp.c \
	core/netif.c core/pbuf.c core/stats.c core/sys.c core/tcp.c core/tcp_in.c core/tcp_out.c core/timeouts.c core/udp.c \
	core/raw.c core/ipv4/acd.c core/ipv4/dhcp.c core/ipv4/etharp.c core/ipv4/icmp.c core/ipv4/ip4.c core/ipv4/ip4_addr.c \
	core/ipv4/ip4_frag.c netif/ethernet.c)
# 順番が大事: shim (pico-sdk・cyw43・lwip/sockets.h の代わり) → PZL の include (config.h: 15KB の応答) → src/net (lwipopts.h)
#   → x68kserremote の service (POSIX の fileop.h。Pico の src/remote/fileop.h は libsmb2 なので使わない) → PZL の各所 → lwIP
INC = -Ishim -I$(S)/include -I$(S)/src/net -I$(S)/lib/x68kserremote/include -I$(S)/lib/x68kserremote/service \
      -I$(S)/src -I$(S)/src/scsi -I$(S)/src/storage -I$(S)/src/config -I$(S)/src/remote -I$(S)/src/remote/iconv \
      -I$(S)/src/netpipe -I$(S)/src/tsp -I$(S)/src/accel -I$(S)/src/accel/monocypher -I$(S)/src/bootwait -I$(S)/driver \
      -I$(S)/lib/libsmb2/include -I$(S)/lib/libsmb2/include/smb2 -I$(L)/include -I$(LWIP)/contrib/ports/unix/port/include

all: $(B)/pzl_all.o

ifneq ($(PZLSRC),)
# PZLSRC=DIR: 写し出し済みの pico/ (この Makefile の build/src、driver/*.inc 入り) を使う (Docker の中など、git や elf2x68k が無い所)
$(B)/rev: FORCE
	@mkdir -p $(B)
	@r=$$(cat $(PZLSRC)/../rev 2>/dev/null || echo $(PZLSRC)); [ "$$(cat $@ 2>/dev/null)" = "$$r" ] || echo $$r > $@
$(B)/src/.done: $(B)/rev
	rm -rf $(B)/src && mkdir -p $(B)/src && cp -R $(PZLSRC) $(B)/src/pico
	@echo "pzl: from $(PZLSRC) ($$(cat $(B)/rev))"
	touch $@
else
$(B)/rev: FORCE
	@mkdir -p $(B)
	@r=$$(git -C $(PZLREPO) rev-parse $(PZLREV)); [ "$$(cat $@ 2>/dev/null)" = "$$r" ] || echo $$r > $@

$(B)/src/.done: $(B)/rev Makefile
	rm -rf $(B)/src && mkdir -p $(B)/src
	git -C $(PZLREPO) archive $$(cat $(B)/rev) pico | tar x -C $(B)/src
	for m in x68kserremote libsmb2; do \
	  sm=$$(git -C $(PZLREPO) ls-tree $$(cat $(B)/rev) pico/lib/$$m | awk '{print $$3}'); \
	  mkdir -p $(S)/lib/$$m && git -C $(PZLREPO)/pico/lib/$$m archive $$sm | tar x -C $(S)/lib/$$m || exit 1; done
	$(MAKE) -s -C $(S)/driver scsiremote.inc bootloader.inc hdsboot.inc settingui.inc bootwait.inc bootwait1.inc
	@echo "pzl: $(PZLREV) = $$(cat $(B)/rev)"
	touch $@
endif

$(B)/config_file_host.c: $(B)/src/.done host_config.py
	python3 -I host_config.py $(S)/src/config/config_file.c $(S)/config.tmpl.txt $@

$(B)/pzl_all.o: $(B)/rev $(B)/src/.done $(B)/config_file_host.c pzl_host.c $(wildcard shim/*/*.h)
	rm -rf $(B)/o && mkdir -p $(B)/o
	for f in $(addprefix $(S)/,$(PZLFILES)) $(B)/config_file_host.c pzl_host.c $(LWIPSRC); do \
	  cc -O1 -g -w -fno-common -fPIC -DPZL_HOST -DPZL_BOARD='"host"' -DGIT_REPO_VERSION='"picosim"' -DHID_KEYBOARD=0 $(if $(wildcard $(S)/src/tsp/tsp_svc.c),-DTSP_ENABLE=1) $(INC) -c $$f -o $(B)/o/$$(basename $${f%.c}).o || exit 1; done
	printf 'pzlh_init\npzlh_poll\npzlh_read\npzlh_write\npzlh_present\npzlh_sects\npzlh_sysstatus\npzlh_ready\npzlh_rebooted\n' > $(B)/exports
	../tools/ld-hide.sh $@ $(B)/exports $(B)/o/*.o

clean:
	rm -rf $(B)
FORCE:
.PHONY: all clean FORCE
