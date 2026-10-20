# X68000 上で: cd /usr/src/rsspeed; make
rsspeed.x: rsspeed.c
	gcc.x rsspeed.c -o rsspeed.x -liocs -lgcc2

# /usr/bin に入れる (x68build もこれを使う)
install: rsspeed.x
	cp rsspeed.x /usr/bin/

.PHONY: install
