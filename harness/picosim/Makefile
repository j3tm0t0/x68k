# harness/picosim -- Mac の上の Pico W の代わり (README.md)。libslirp は初回に取ってきて build/ でビルドする (meson 不要)
#   make            -> build/picosim
#   make SVC="a.c b.c" SVCFLAGS="-I..."   ほかのサービス (picosim_register_svc を呼ぶもの) を一緒にリンクする
SLIRP_VER = 4.8.0
SLIRP_SHA = 2a98852e65666db313481943e7a1997abff0183bd9bea80caec1b5da89fda28c
SLIRP_DIR = build/libslirp-v$(SLIRP_VER)
UNAME := $(shell uname)
ifeq ($(UNAME),Darwin)
GLIB ?= $(shell brew --prefix glib 2>/dev/null || echo /opt/homebrew/opt/glib)
GLIB_CFLAGS = -I$(GLIB)/include/glib-2.0 -I$(GLIB)/lib/glib-2.0/include
GLIB_LIBS = -L$(GLIB)/lib -lglib-2.0
LIBICONV = -liconv
else
# Linux (Docker): glib は pkg-config、iconv と resolv は glibc に入っている
GLIB_CFLAGS = $(shell pkg-config --cflags glib-2.0)
GLIB_LIBS = $(shell pkg-config --libs glib-2.0) -lpthread
LIBICONV =
endif
ACCEL = ../../net/accel
# B: (リモートドライブ): フォークの x68kserremote (サブモジュール) の remoteserv.c をそこからコンパイルする (写さない)。
# 無ければ B: 無しでビルドする
RS ?= $(HOME)/src/github.com/yunkya2/x68kzremotedrv-hid/x68kserremote
REMOTE_SRC = $(if $(wildcard $(RS)/service/remoteserv.c),build/remote.o build/remoteserv.o)
# TSP (tis.x -p の相手): net/tistack の tsp_svc + lwIP (pico-sdk の lib/lwip)。lwIP があれば既定で入れる (TSP= で切る)。
# lwIP は libslirp と名前がぶつかるので、net/tistack/pico/picosim が 1 個の .o にまとめたものをリンクする。
# lwIP の口は最初の 0xA6 が来てから LAN に出る (それまでは npd.x + xip.x の Z と同じに動く)
LWIP ?= $(HOME)/src/github.com/yunkya2/x68kzremotedrv/pico-sdk/lib/lwip
TSPDIR = ../../net/tistack/pico/picosim
TSP ?= $(if $(wildcard $(LWIP)/src/core/tcp.c),$(TSPDIR)/build/tsp_all.o)
CFLAGS ?= -O2 -g -Wall
# 足すサービス (picosim_register_svc)。zss_mock.c = 0xA7 (x68k-zos の端末のセッション) の Pico 側のまね (外のものを使わない)
SVC ?= zss_mock.c
# 待ち IPL / hdsboot / settingui の 68000 のバイナリ (フォークの driver/ でビルドしたもの。git には入れない)
FW ?= $(HOME)/src/github.com/yunkya2/x68kzremotedrv-hid/driver
SVCFLAGS ?=

all: build/picosim

build/picosim: picosim.c netpipe.c lan_slirp.c bootwait.c ../../net/npd/zss.h picosim.h build/libslirp.a $(SVC) $(REMOTE_SRC) $(TSP)
	cc $(CFLAGS) -I$(SLIRP_DIR)/src -I$(ACCEL) -I$(ACCEL)/monocypher $(GLIB_CFLAGS) $(SVCFLAGS) -o $@ \
	  picosim.c netpipe.c lan_slirp.c bootwait.c $(SVC) $(REMOTE_SRC) $(TSP) $(ACCEL)/accel_ops.c $(ACCEL)/monocypher/monocypher.c \
	  $(ACCEL)/monocypher/monocypher-ed25519.c build/libslirp.a $(GLIB_LIBS) -lresolv $(LIBICONV)

$(TSPDIR)/build/tsp_all.o: $(TSPDIR)/tsp_glue.c $(wildcard $(TSPDIR)/../tsp_svc.c $(TSPDIR)/../tsp_svc.h)
	$(MAKE) -C $(TSPDIR) build/tsp_all.o LWIP=$(LWIP)

REMOTE_CFLAGS = -O2 -g -Iremote -I$(RS)/include -I$(RS)/service -I.
build/remote.o: remote.c remote/config.h picosim.h
	mkdir -p build && cc $(REMOTE_CFLAGS) -Wall -c -o $@ remote.c
build/remoteserv.o: $(RS)/service/remoteserv.c remote/config.h
	mkdir -p build && cc $(REMOTE_CFLAGS) -w -c -o $@ $<

PZLREPO ?= $(HOME)/Dropbox/src/github.com/j3tm0t0/x68k-zos-pzl
# Pico Z Loader (x68k-zos の branch pzl) の SCSI の層入り: picosim --pzl=config.txt (pzl/Makefile、PZLREPO= PZLREV=)
build/picosim-pzl: picosim.c netpipe.c lan_slirp.c bootwait.c picosim.h build/libslirp.a $(REMOTE_SRC) FORCE
	$(MAKE) -C pzl PZLREPO=$(PZLREPO) $(if $(PZLREV),PZLREV=$(PZLREV)) $(if $(PZLSRC),PZLSRC=$(PZLSRC))
	cc $(CFLAGS) -I$(SLIRP_DIR)/src -I$(ACCEL) -I$(ACCEL)/monocypher $(GLIB_CFLAGS) -o $@ \
	  picosim.c netpipe.c lan_slirp.c bootwait.c $(REMOTE_SRC) pzl/build/pzl_all.o $(ACCEL)/accel_ops.c $(ACCEL)/monocypher/monocypher.c \
	  $(ACCEL)/monocypher/monocypher-ed25519.c build/libslirp.a $(GLIB_LIBS) -lresolv $(LIBICONV)
FORCE:

# 試験用: 0xA6 をそのまま返すサービス入り
build/picosim-echo: picosim.c netpipe.c lan_slirp.c bootwait.c picosim.h build/libslirp.a tests/echosvc.c
	cc $(CFLAGS) -I$(SLIRP_DIR)/src -I$(ACCEL) -I$(ACCEL)/monocypher $(GLIB_CFLAGS) -o $@ \
	  picosim.c netpipe.c lan_slirp.c bootwait.c tests/echosvc.c $(ACCEL)/accel_ops.c $(ACCEL)/monocypher/monocypher.c \
	  $(ACCEL)/monocypher/monocypher-ed25519.c build/libslirp.a $(GLIB_LIBS) -lresolv

build/libslirp.tar.gz:
	mkdir -p build
	if [ -f "$(CI_ARTIFACTS)/libslirp-v$(SLIRP_VER).tar.gz" ]; then cp "$(CI_ARTIFACTS)/libslirp-v$(SLIRP_VER).tar.gz" $@.tmp; else \
	curl -fsSL -o $@.tmp https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v$(SLIRP_VER)/libslirp-v$(SLIRP_VER).tar.gz; fi
	[ "$$(shasum -a 256 $@.tmp | cut -c1-64)" = "$(SLIRP_SHA)" ] || { echo "libslirp sha256 mismatch"; exit 1; }
	mv $@.tmp $@

build/libslirp.a: build/libslirp.tar.gz
	rm -rf $(SLIRP_DIR) build/slirp-obj && mkdir -p build/slirp-obj
	tar xzf $< -C build
	# macOS: get_dns_addr_libresolv が res_ninit に初期化していない __res_state を渡し、DNS の口が見つからない
	# (Z の DNS が "net unreachable" になった)。0 にしてから呼ぶ
	perl -0pi -e 's/    if \(res_ninit\(&state\) != 0\) \{/    memset(&state, 0, sizeof(state));\n    if (res_ninit(&state) != 0) {/' $(SLIRP_DIR)/src/slirp.c
	grep -q 'memset(&state, 0' $(SLIRP_DIR)/src/slirp.c
	sed -e 's/@SLIRP_MAJOR_VERSION@/4/;s/@SLIRP_MINOR_VERSION@/8/;s/@SLIRP_MICRO_VERSION@/0/;s/@SLIRP_VERSION_STRING@/"$(SLIRP_VER)"/' \
	  $(SLIRP_DIR)/src/libslirp-version.h.in > $(SLIRP_DIR)/src/libslirp-version.h
	for f in $(SLIRP_DIR)/src/*.c; do \
	  cc -O2 -w -DG_LOG_DOMAIN='"Slirp"' -DBUILDING_LIBSLIRP -I$(SLIRP_DIR)/src $(GLIB_CFLAGS) -c $$f -o build/slirp-obj/$$(basename $${f%.c}).o || exit 1; \
	done
	ar rcs $@ build/slirp-obj/*.o

# pzl の先 (branch pzl) で作って pzltest を回す: 通ったら pzl/Makefile の PZLREV を進める
pzl-latest:
	$(MAKE) -C pzl PZLREPO=$(PZLREPO) PZLREV=pzl
	rm -f build/picosim-pzl && $(MAKE) build/picosim-pzl PZLREPO=$(PZLREPO) PZLREV=pzl
	python3 -I tests/pzltest.py build/picosim-pzl

# test = test-host (ホストだけ、数秒) + test-xeij (XEiJ を動かす 2 本、GUI のセッションか Xvfb が要る)
test: test-host test-xeij

test-host: build/picosim build/picosim-echo
	python3 -I tests/bridgetest.py build/picosim-echo
	python3 -I tests/zsstest.py build/picosim
	python3 -I tests/bootwaittest.py build/picosim $(FW)
	python3 -I tests/remotetest.py build/picosim $(FW)
	@if [ -d "$(PZLREPO)" ] || [ -n "$(PZLSRC)" ]; then $(MAKE) build/picosim-pzl PZLREPO=$(PZLREPO) PZLSRC=$(PZLSRC) && \
	  python3 -I tests/pzltest.py build/picosim-pzl; else echo "SKIP pzltest: no PZL repo ($(PZLREPO))"; fi

test-xeij: test-tsp test-zos test-tsppzl
test-tsp: build/picosim
	tests/tsptest.sh
# 同じ tis.x -p の試験を Pico Z Loader の TSP (x68k-zos pzl の src/tsp) で
test-tsppzl: build/picosim
	@if [ -d "$(PZLREPO)" ] || [ -n "$(PZLSRC)" ]; then $(MAKE) build/picosim-pzl PZLREPO=$(PZLREPO) PZLSRC=$(PZLSRC) && PZL=1 tests/tsptest.sh; \
	  else echo "SKIP tsptest (PZL): no PZL repo"; fi
test-zos: build/picosim
	tests/zostest.sh

# Docker の中で流す (docker/README.md)。SUITES="host tsp zos tsppzl" JOBS=2
docker-test:
	docker/docker-test.sh $(SUITES)

clean:
	rm -f build/picosim build/picosim-echo

.PHONY: all test test-host test-xeij test-tsp test-tsppzl test-zos docker-test pzl-latest clean
