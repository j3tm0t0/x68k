# harness/picosim/pzl -- Pico Z Loader (x68k-zos の branch pzl、pico/) をホストでビルドして picosim --pzl に載せる
#   make            -> build/pzl_all.o (PZL の virtual_disk / storage / config + pzl_host.c。外に出す名前は pzl_* だけ)
# PZL のソースは作業木ではなく、決まったコミット (PZLREV、既定は branch pzl の先) から git archive で写し出す
# (書きかけの作業木に左右されないように)。サブモジュール x68kserremote も記録されたコミットで。
# Z 側の 68000 のバイナリ (driver/*.inc: hdsboot・scsiremote・settingui など) は elf2x68k でここで作る
PZLREPO ?= $(HOME)/Dropbox/src/github.com/j3tm0t0/x68k-zos-pzl
PZLREV ?= pzl
B = build
S = $(B)/src/pico
PZLFILES = src/scsi/virtual_disk.c src/storage/storage.c src/storage/stor_ram.c
INC = -Ishim -I$(S)/src -I$(S)/src/scsi -I$(S)/src/storage -I$(S)/src/config -I$(S)/src/remote -I$(S)/include -I$(S)/driver \
      -I$(S)/lib/x68kserremote/include -I$(S)/lib/x68kserremote/service

all: $(B)/pzl_all.o

$(B)/rev: FORCE
	@mkdir -p $(B)
	@r=$$(git -C $(PZLREPO) rev-parse $(PZLREV)); [ "$$(cat $@ 2>/dev/null)" = "$$r" ] || echo $$r > $@

$(B)/src/.done: $(B)/rev
	rm -rf $(B)/src && mkdir -p $(B)/src
	git -C $(PZLREPO) archive $$(cat $(B)/rev) pico | tar x -C $(B)/src
	sm=$$(git -C $(PZLREPO) ls-tree $$(cat $(B)/rev) pico/lib/x68kserremote | awk '{print $$3}'); \
	  mkdir -p $(S)/lib/x68kserremote && git -C $(PZLREPO)/pico/lib/x68kserremote archive $$sm | tar x -C $(S)/lib/x68kserremote
	$(MAKE) -s -C $(S)/driver scsiremote.inc bootloader.inc hdsboot.inc settingui.inc
	@echo "pzl: $(PZLREV) = $$(cat $(B)/rev)"
	touch $@

$(B)/config_file_host.c: $(B)/src/.done host_config.py
	python3 -I host_config.py $(S)/src/config/config_file.c $(S)/config.tmpl.txt $@

$(B)/pzl_all.o: $(B)/src/.done $(B)/config_file_host.c pzl_host.c $(wildcard shim/hardware/*.h)
	rm -rf $(B)/o && mkdir -p $(B)/o
	for f in $(addprefix $(S)/,$(PZLFILES)) $(B)/config_file_host.c pzl_host.c; do \
	  cc -O1 -g -w -fno-common -DPZL_HOST $(INC) -c $$f -o $(B)/o/$$(basename $${f%.c}).o || exit 1; done
	printf '_pzl_init\n_pzl_poll\n_pzl_read\n_pzl_write\n_pzl_present\n_pzl_sects\n_pzl_sysstatus\n_pzl_ready\n_pzl_rebooted\n' > $(B)/exports
	ld -r -o $@ $(B)/o/*.o -exported_symbols_list $(B)/exports

clean:
	rm -rf $(B)
FORCE:
.PHONY: all clean FORCE
