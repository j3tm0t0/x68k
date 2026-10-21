# x68kcli: Human68k 3.02 + zsh/fish/ksh + vim + UNIX/GNU tools の HDD イメージを作る
# PROFILE: cli (通常) / dev (開発用: 自動接続・rcmd・GraySaver なし、コンソールを RS-232C に出す)
PROFILE ?= cli
IMG ?= $(if $(filter dev,$(PROFILE)),build/x68kdev.hds,build/x68kcli.hds)
SIZE_MB ?= 100
# 行末コメントを書くと値に空白が残るので、説明は上の行に書く
# NET: slip (slip.sys + slipd.x) / ppp (ppp.sys + ppp.x) / npd (npd.sys + npd.x: Pico W の netpipe のブリッジだけ)。
#   NETSPEED: 起動時の自動接続 (netup) の速度
#   NETZIP: NET=npd のときの Z の IP アドレス (Pico W が LAN で名乗る。DHCP の範囲外を使う)
#   (Pi 側の /etc/default/ppp-x68k の MODE / SPEED と揃える)
# KEYMAP: us にすると KeyWitch を US 配列設定で CONFIG.SYS に組み込む
# HOMEDIR: HOME。イメージを作り直しても残したいなら別のドライブ (B:/home など) にする
NET ?= slip
NETSPEED ?= 38400
NETZIP ?=
KEYMAP ?= jis
HOMEDIR ?= A:/home
# XIPMSS: xip.x の TCP の MSS (元は 512)。1 パケットごとに 100～150ms かかるので、大きいほど速い
#   (Pico W netpipe で 4KB/s → 8.5KB/s)。512 にすると元の xip.x のまま入れる
XIPMSS ?= 1460
# XIPTIMER: 1 なら xip.x のタイマーの表の掃除を 0.1 秒に 1 回に間引く (Pico W netpipe + npd で 36 → 40〜45KB/s)
XIPTIMER ?= 1

.PHONY: all tools fetch stage image run agent ls clean distclean

all: image

tools:            ## rb-cli / XEiJ を tools/ に導入
	scripts/tools.sh

fetch:            ## sources.txt のアーカイブを dl/ に取得 (sha256 検証)
	scripts/fetch.sh

stage: tools fetch      ## アーカイブを展開して stage/ を作る (elf2x68k があれば sed/grep もクロスビルド)
	scripts/xbuild.sh
	PROFILE=$(PROFILE) KEYMAP=$(KEYMAP) NET=$(NET) NETSPEED=$(NETSPEED) NETZIP=$(NETZIP) HOMEDIR=$(HOMEDIR) XIPMSS=$(XIPMSS) XIPTIMER=$(XIPTIMER) scripts/stage.sh

image: tools stage ## $(IMG) を生成
	IMG=$(IMG) SIZE_MB=$(SIZE_MB) scripts/build.sh

run: tools agent  ## XEiJ で起動
	IMG=$(IMG) ../harness/xeij/run.sh

JAVA_HOME ?= $(shell [ -d /opt/homebrew/opt/openjdk ] && echo /opt/homebrew/opt/openjdk || echo /usr)
agent: tools/xeijagent.jar  ## XEiJ 用エージェント (screenshot / textcopy をファイル経由で実行)
XAGENT = ../harness/xeij/agent
tools/xeijagent.jar: $(XAGENT)/XEiJAgent.java $(XAGENT)/manifest.txt
	rm -rf build/agent && mkdir -p build/agent tools
	$(JAVA_HOME)/bin/javac -d build/agent $(XAGENT)/XEiJAgent.java
	$(JAVA_HOME)/bin/jar cfm $@ $(XAGENT)/manifest.txt -C build/agent .

ls:               ## イメージの内容一覧
	tools/rb-cli ls $(IMG) '/**'

clean:
	rm -rf stage work build

distclean: clean
	rm -rf dl tools
