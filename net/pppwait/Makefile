# X68000 上で: cd /usr/src/pppwait; make
pppwait.x: pppwait.c
	gcc.x pppwait.c -o pppwait.x -liocs -lgcc2

# /usr/bin に入れる (x68build もこれを使う)
install: pppwait.x
	cp pppwait.x /usr/bin/

.PHONY: install
