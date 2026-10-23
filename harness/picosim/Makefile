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
CFLAGS ?= -O2 -g -Wall
SVC ?=
SVCFLAGS ?=

all: build/picosim

build/picosim: picosim.c netpipe.c lan_slirp.c picosim.h build/libslirp.a $(SVC)
	cc $(CFLAGS) -I$(SLIRP_DIR)/src -I$(ACCEL) -I$(ACCEL)/monocypher $(GLIB_CFLAGS) $(SVCFLAGS) -o $@ \
	  picosim.c netpipe.c lan_slirp.c $(SVC) $(ACCEL)/accel_ops.c $(ACCEL)/monocypher/monocypher.c \
	  $(ACCEL)/monocypher/monocypher-ed25519.c build/libslirp.a $(GLIB_LIBS) -lresolv

# 試験用: 0xA6 をそのまま返すサービス入り
build/picosim-echo: picosim.c netpipe.c lan_slirp.c picosim.h build/libslirp.a tests/echosvc.c
	cc $(CFLAGS) -I$(SLIRP_DIR)/src -I$(ACCEL) -I$(ACCEL)/monocypher $(GLIB_CFLAGS) -o $@ \
	  picosim.c netpipe.c lan_slirp.c tests/echosvc.c $(ACCEL)/accel_ops.c $(ACCEL)/monocypher/monocypher.c \
	  $(ACCEL)/monocypher/monocypher-ed25519.c build/libslirp.a $(GLIB_LIBS) -lresolv

build/libslirp.tar.gz:
	mkdir -p build
	curl -fsSL -o $@.tmp https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v$(SLIRP_VER)/libslirp-v$(SLIRP_VER).tar.gz
	[ "$$(shasum -a 256 $@.tmp | cut -c1-64)" = "$(SLIRP_SHA)" ] || { echo "libslirp sha256 mismatch"; exit 1; }
	mv $@.tmp $@

build/libslirp.a: build/libslirp.tar.gz
	rm -rf $(SLIRP_DIR) build/slirp-obj && mkdir -p build/slirp-obj
	tar xzf $< -C build
	sed -e 's/@SLIRP_MAJOR_VERSION@/4/;s/@SLIRP_MINOR_VERSION@/8/;s/@SLIRP_MICRO_VERSION@/0/;s/@SLIRP_VERSION_STRING@/"$(SLIRP_VER)"/' \
	  $(SLIRP_DIR)/src/libslirp-version.h.in > $(SLIRP_DIR)/src/libslirp-version.h
	for f in $(SLIRP_DIR)/src/*.c; do \
	  cc -O2 -w -DG_LOG_DOMAIN='"Slirp"' -DBUILDING_LIBSLIRP -I$(SLIRP_DIR)/src $(GLIB_CFLAGS) -c $$f -o build/slirp-obj/$$(basename $${f%.c}).o || exit 1; \
	done
	ar rcs $@ build/slirp-obj/*.o

test: build/picosim-echo
	python3 -I tests/bridgetest.py build/picosim-echo

clean:
	rm -f build/picosim build/picosim-echo

.PHONY: all test clean
