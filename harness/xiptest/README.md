# xiptest: xip.x のパッチを Mac の上の 68000 で確かめる

xip.x (β5) は配布物のバイナリしかないので、書き換えた部分が元と同じに動くかを、
Unicorn (pip の `unicorn`) の 68000 で元のコードと並べて動かして比べる。xip.x 自体は入れていない
(`x68kcli/work/xip/xip.x` は `make fetch` で展開される)。

- `m68emu.py`: xip.x を番地に置いて関数を呼ぶ土台。`_CHANGE_PR` と `trap #15` の手前で止め、スレッドを切り替えたことにする
- `image.py` / `reloc.py`: X 形式の読み込みと再配置表
- `lock.S` / `lock.ld` / `build.sh`: 名前付きロック (0xF4E-0x1033) の書き直しと、その 230 バイトの作り方
- `difflock.py`: ロック・参照・アンロックを何本かのスレッドで乱数の順に呼び、戻り値・表の中身・譲った位置・
  壊したレジスタを元と比べる (スレッドは _CHANGE_PR でだけ切り替わる)
- `lockfix.S` / `mutex.py`: 元の不具合を直した版 (patchxip.py --lockfix) と、割り込みで乱数の命令数ごとに
  スレッドが切り替わる条件で「同じキーを 2 本が同時に持たない」かを調べる検査
- `perf.py`: ロックとアンロック 1 組の命令数

```sh
./build.sh
python3 difflock.py ../../x68kcli/work/xip/xip.x 400
python3 ../../net/patchxip.py ../../x68kcli/work/xip/xip.x /tmp/xip.x --lock
python3 difflock.py ../../x68kcli/work/xip/xip.x 200 /tmp/xip.x   # 当てた結果と比べる
```

差分テストで見つかったこと:

- 元のロックは d2 に触らない。最初の書き直しは d2 を壊していた
- 元のロックには、同じキーが表に 2 つできる不具合がある (待っている間に項目が別のキーに割り当て直されると、
  待っていた側が取ったあとでキーを書き戻す)。そのため「前回の項目から先に見る」速くし方は使えず、
  元と同じく先頭から探す
- mutex.py で、元のロックでも排他が破れる (割り込みの間隔 1〜200 命令、3 本、キー 3 個で 200 回中 29 回)。
  原因は 2 つ: 上の書き戻しと、空き項目の割り当てが「フラグが 0 か調べてから書く」ので、マスターロックを
  持たずに tas で待っている側と同じ項目を取り合うこと。lockfix.S は、取れたあとでキーを確かめ直し、
  割り当ても tas で取る。6 通りの条件で 2,400 回、破れなかった

```sh
python3 ../../net/patchxip.py ../../x68kcli/work/xip/xip.x /tmp/xipf.x --lockfix
python3 mutex.py ../../x68kcli/work/xip/xip.x 200 /tmp/xipf.x
```
