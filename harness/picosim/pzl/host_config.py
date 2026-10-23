#!/usr/bin/env python3
"""PZL の src/config/config_file.c をホストでコンパイルできる形にする (pzl/Makefile から)。
ファームは config.tmpl.txt を ELF の .incbin で埋め込むが、Mac (Mach-O) の as は通らないので、その __asm__ の塊を
C の配列 config_template[] に置き換える。ほかは一字も変えない。
usage: host_config.py config_file.c config.tmpl.txt OUT.c"""
import re, sys
src, tmpl, out = sys.argv[1:4]
s = open(src, encoding="utf-8").read()
asm = re.compile(r'__asm__ \(\n(?:.*\n)*?\);\n\nextern char config_template\[\];\n')
if not asm.search(s):
    sys.exit("host_config: the config_template __asm__ block was not found (pzl changed?)")
t = open(tmpl, "rb").read() + b"\0"
arr = "char config_template[] = {" + ",".join(str(b) for b in t) + "};\n"
s = asm.sub(lambda m: "/* picosim: .incbin の代わり */\n" + arr, s, count=1)
# フラッシュの番地 (XIP 0x10000000) は、ホストではフラッシュの代わりの配列 pzlh_flash (pzl_host.c) に
fl = "((uint8_t *)(0x10000000 + CONFIG_FLASH_OFFSET))"
if fl not in s:
    sys.exit("host_config: CONFIG_FLASH_ADDR changed (pzl changed?)")
s = s.replace(fl, "(pzlh_flash + CONFIG_FLASH_OFFSET)").replace("#include \"config_file.h\"", "#include \"config_file.h\"\nextern uint8_t pzlh_flash[];", 1)
open(out, "w", encoding="utf-8").write(s)
