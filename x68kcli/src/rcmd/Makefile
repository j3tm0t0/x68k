# X68000 上で GNU make (make.x) から: cd /usr/src/rcmd; make
# gcc 真里子版 + LIBC + ESP/X libnetwork.a (/usr/lib)。-lgcc2 は zsh の gcc 関数と同じ
rcmd.x: rcmd.c
	gcc.x rcmd.c -o rcmd.x -lnetwork -liocs -ldos -lgcc2

# /usr/bin に入れる (x68build もこれを使う)
install: rcmd.x
	cp rcmd.x /usr/bin/

.PHONY: install
