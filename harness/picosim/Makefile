# harness/picosim -- Mac の上の Pico W の代わり (README.md)。libslirp は初回に取ってきて build/ でビルドする (meson 不要)
#   make            -> build/picosim
#   make SVC="a.c b.c" SVCFLAGS="-I..."   ほかのサービス (picosim_register_svc を呼ぶもの) を一緒にリンクする
SLIRP_VER = 4.8.0
SLIRP_SHA = 2a98852e65666db313481943e7a1997abff0183bd9bea80caec1b5da89fda28c
SLIRP_DIR = build/libslirp-v$(SLIRP_VER)
GLIB ?= $(shell brew --prefix glib 2>/dev/null || echo /opt/homebrew/opt/glib)
GLIB_CFLAGS = -I$(GLIB)/include/glib-2.0 -I$(GLIB)/lib/glib-2.0/include
GLIB_LIBS = -L$(GLIB)/lib -lglib-2.0
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
SVC ?=
# 待ち IPL / hdsboot / settingui の 68000 のバイナリ (フォークの driver/ でビルドしたもの。git には入れない)
FW ?= $(HOME)/src/github.com/yunkya2/x68kzremotedrv-hid/driver
SVCFLAGS ?=

all: build/picosim

build/picosim: picosim.c netpipe.c lan_slirp.c bootwait.c picosim.h build/libslirp.a $(SVC) $(REMOTE_SRC) $(TSP)
	cc $(CFLAGS) -I$(SLIRP_DIR)/src -I$(ACCEL) -I$(ACCEL)/monocypher $(GLIB_CFLAGS) $(SVCFLAGS) -o $@ \
	  picosim.c netpipe.c lan_slirp.c bootwait.c $(SVC) $(REMOTE_SRC) $(TSP) $(ACCEL)/accel_ops.c $(ACCEL)/monocypher/monocypher.c \
	  $(ACCEL)/monocypher/monocypher-ed25519.c build/libslirp.a $(GLIB_LIBS) -lresolv -liconv

$(TSPDIR)/build/tsp_all.o: $(TSPDIR)/tsp_glue.c $(wildcard $(TSPDIR)/../tsp_svc.c $(TSPDIR)/../tsp_svc.h)
	$(MAKE) -C $(TSPDIR) build/tsp_all.o LWIP=$(LWIP)

REMOTE_CFLAGS = -O2 -g -Iremote -I$(RS)/include -I$(RS)/service -I.
build/remote.o: remote.c remote/config.h picosim.h
	mkdir -p build && cc $(REMOTE_CFLAGS) -Wall -c -o $@ remote.c
build/remoteserv.o: $(RS)/service/remoteserv.c remote/config.h
	mkdir -p build && cc $(REMOTE_CFLAGS) -w -c -o $@ $<

# 試験用: 0xA6 をそのまま返すサービス入り
build/picosim-echo: picosim.c netpipe.c lan_slirp.c bootwait.c picosim.h build/libslirp.a tests/echosvc.c
	cc $(CFLAGS) -I$(SLIRP_DIR)/src -I$(ACCEL) -I$(ACCEL)/monocypher $(GLIB_CFLAGS) -o $@ \
	  picosim.c netpipe.c lan_slirp.c bootwait.c tests/echosvc.c $(ACCEL)/accel_ops.c $(ACCEL)/monocypher/monocypher.c \
	  $(ACCEL)/monocypher/monocypher-ed25519.c build/libslirp.a $(GLIB_LIBS) -lresolv

build/libslirp.tar.gz:
	mkdir -p build
	curl -fsSL -o $@.tmp https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v$(SLIRP_VER)/libslirp-v$(SLIRP_VER).tar.gz
	[ "$$(shasum -a 256 $@.tmp | cut -c1-64)" = "$(SLIRP_SHA)" ] || { echo "libslirp sha256 mismatch"; exit 1; }
	mv $@.tmp $@

build/libslirp.a: build/libslirp.tar.gz
	rm -rf $(SLIRP_DIR) build/slirp-obj && mkdir -p build/slirp-obj
	tar xzf $< -C build
	# macOS: get_dns_addr_libresolv が res_ninit に初期化していない __res_state を渡し、DNS の口が見つからない
	# (Z の DNS が "net unreachable" になった)。0 にしてから呼ぶ
	sed -i '' 's/    if (res_ninit(&state) != 0) {/    memset(\&state, 0, sizeof(state));\n    if (res_ninit(\&state) != 0) {/' $(SLIRP_DIR)/src/slirp.c
	grep -q 'memset(&state, 0' $(SLIRP_DIR)/src/slirp.c
	sed -e 's/@SLIRP_MAJOR_VERSION@/4/;s/@SLIRP_MINOR_VERSION@/8/;s/@SLIRP_MICRO_VERSION@/0/;s/@SLIRP_VERSION_STRING@/"$(SLIRP_VER)"/' \
	  $(SLIRP_DIR)/src/libslirp-version.h.in > $(SLIRP_DIR)/src/libslirp-version.h
	for f in $(SLIRP_DIR)/src/*.c; do \
	  cc -O2 -w -DG_LOG_DOMAIN='"Slirp"' -DBUILDING_LIBSLIRP -I$(SLIRP_DIR)/src $(GLIB_CFLAGS) -c $$f -o build/slirp-obj/$$(basename $${f%.c}).o || exit 1; \
	done
	ar rcs $@ build/slirp-obj/*.o

test: build/picosim build/picosim-echo
	python3 -I tests/bridgetest.py build/picosim-echo
	python3 -I tests/bootwaittest.py build/picosim $(FW)
	python3 -I tests/remotetest.py build/picosim $(FW)
	tests/tsptest.sh

clean:
	rm -f build/picosim build/picosim-echo

.PHONY: all test clean
