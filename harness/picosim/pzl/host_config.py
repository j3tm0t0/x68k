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
open(out, "w", encoding="utf-8").write(asm.sub(lambda m: "/* picosim: .incbin の代わり */\n" + arr, s, count=1))
