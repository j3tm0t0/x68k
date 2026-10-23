# harness/picosim/docker -- picosim の試験を Docker の中で (1 コンテナ = 1 回の試験、並べて流せる)

```sh
make -C harness/picosim docker-test                       # host / tsp / zos を 1 つずつ、2 つずつ並べて
make -C harness/picosim docker-test SUITES="zos zos zos zos zos"   # 同じ試験を 5 回
JOBS=3 CPUS=4 harness/picosim/docker/docker-test.sh host tsp zos
```

- 記録: `harness/picosim/build/docker-out/<日時>/<番号>-<SUITE>/` (`run.log` に PASS/FAIL、`test-*.log` に全部、落ちたら
  作業場所 `picosim-zostest*` / `picosim-tsptest*` も)。最後に PASS/FAIL を数えた表と、落ちた回の数
- SUITE: `host` (bridgetest・bootwaittest・remotetest・pzltest、数十秒) / `tsp` (tis.x -p を XEiJ で) / `zos` (Z-OS の
  ペインで ssh.x を TSP 越しに)。`make test-<SUITE>` と同じもの
- 普段の (Docker を使わない) `make -C harness/picosim test` はそのまま使える (Mac で直に、test-host・test-tsp・test-zos を確かめた)

確かめたこと (2026-10-23、OrbStack Docker 29.4、arm64、MacBook Pro): `make docker-test` で host・tsp・zos がどれも PASS
(tsp は rcmd.x まで)、`SUITES="zos zos"` を 2 つ並べても PASS。1 回: host 約 1 分、tsp 約 6 分、zos 約 4 分 (ビルド込み)

## なぜ Docker か (ネイティブでぶつかっていたもの)

XEiJ + picosim を 2 つ同時に流すと: 決まった TCP の口 (tsptest 26968/23468/7787、zostest 2224/26869、ホストの試験
16868〜16871、手で動かすとき 6868/23268)、決まった作業場所 ($TMPDIR/picosim-tsptest・picosim-zostest・tispico・zostest)、
`-config=default -saveonexit=off` を付けない run.sh の ~/XEiJ.ini、textcopy の Mac のクリップボード、がぶつかる。
コンテナごとにネットワークとファイルが分かれるので、口も作業場所もそのままで並べられる。並べすぎると XEiJ の鍵交換が
ホストの負荷で遅くなるので、既定は 2 つずつ (`JOBS`)、1 コンテナ 4 CPU まで (`CPUS`)。

## image (Dockerfile)

eclipse-temurin:26-jdk-noble (XEiJ.jar は class version 70 = Java 26) + Xvfb (XEiJ の GUI を画面なしで) + C の道具と glib +
openssh-server (試験の sshd、ユーザ権限) + dropbear-bin (dropbearkey) + python3・perl・rsync・git。amd64 のときだけ elf2x68k
(Linux 版は x86-64 だけ)。ホストと同じアーキテクチャで作る (Mac・raspi5 は arm64)。Mac で amd64 を Rosetta で動かすと
tar や objcopy が "Function not implemented" で落ちたので使わない。

**image に入れないもの** (第三者のもの・大きいもの): XEiJ (XEiJ.jar と data/: HUMAN.SYS・ROM)、x68kcli の stage (Human68k・
gcc・zsh・ksh...)、HDS、ssh.x、フォークの driver/ のバイナリと x68kserremote、pico-sdk の lwIP、x68k-zos の kernel/。
`docker-test.sh` がホストで見つけ、読むだけで `/mnt/*` につなぐ (場所は環境変数 `XEIJ` `STAGE` `FW` `RS` `LWIP` `ZOS` `SSH_X`
`PZLREPO` で変えられる。無いものを要る試験は SKIP する)。x68k の作業木は `/repo` に読むだけでつなぎ、コンテナの中に
写してからビルドする (ほかのコンテナとビルドの残りがぶつからない)。

**ホストでしておくこと** (`docker-test.sh` が自動で): m68k のもの (npd.sys・npd.x・tis.x・tisnet.x・tisrun.x) をホストの
elf2x68k で作る (arm64 の image には elf2x68k が無い。できたものは作業木ごとコンテナに入る)。PZL の写し
(`pzl/build/src/pico`、driver/*.inc 入り) を作り、`PZLSRC` としてつなぐ。

CI (GitHub Actions など) はまだ。x86-64 の CI なら amd64 の image (elf2x68k 入り) で m68k のものもコンテナの中で作れるが、
XEiJ と stage は CI に置けない (第三者のもの) ので、置き場所を決めるまでは host の試験だけになる。

## Linux で気づいたこと

- XEiJ のホストのディレクトリからの起動は、Linux では名前の大文字小文字を区別する。Human68k の既定のシェル名 `COMMAND.x` で
  `COMMAND.X` が見つからなかった → 試験の CONFIG.SYS に `SHELL = A:\COMMAND.X` を書く (net/tistack/tests/xeij_pico.sh)
- 同じ理由で、rcmd.x が書く `A:\tmp` は `TMP` とは別 (xeij_pico.sh が両方作る)。zostest はファイルを小文字の名前で置いて
  小文字で打つ (xeijkeys.py は大文字を Shift で打つが、取りこぼして ^Z が入ったことがある)。ペインの前面がどちらから
  始まるかも Mac と違うことがあったので、ssh.x の入ったペインを画面から見つけて判定する
- `objcopy --keep-global-symbols` に空の一覧を渡すと失敗する (tools/ld-hide.sh で場合分け)。`struct sockaddr_in` に
  `sin_len` は無い。`-liconv` は要らない (glibc)
