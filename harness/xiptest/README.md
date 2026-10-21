# xiptest: xip.x のパッチを Mac の上の 68000 で確かめる

xip.x (β5) は配布物のバイナリしかないので、書き換えた部分が元と同じに動くかを、
Unicorn (pip の `unicorn`) の 68000 で元のコードと並べて動かして比べる。xip.x 自体は入れていない
(`x68kcli/work/xip/xip.x` は `make fetch` で展開される)。

- `m68emu.py`: xip.x を番地に置いて関数を呼ぶ土台。`_CHANGE_PR` と `trap #15` の手前で止め、スレッドを切り替えたことにする
- `image.py` / `reloc.py`: X 形式の読み込みと再配置表
- `lock.S` / `lock.ld` / `build.sh`: 名前付きロック (0xF4E-0x1033) の書き直しと、その 230 バイトの作り方
- `difflock.py`: ロック・参照・アンロックを何本かのスレッドで乱数の順に呼び、戻り値・表の中身・譲った位置・
  壊したレジスタを元と比べる
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
