# harness/picosim/docker -- XEiJ + picosim の試験を 1 つのコンテナで 1 回ずつ流すための image (docker/README.md)
#   中身: JDK 26 (XEiJ.jar は class version 70)、Xvfb (XEiJ の GUI を画面なしで)、C のビルド道具と glib (picosim・libslirp)、
#         elf2x68k (m68k の npd.x・tis.x・PZL の driver/*.inc。Linux 版は x86-64 だけなので amd64 の image にだけ入れる。
#         arm64 (Apple Silicon・raspi5) では docker-test.sh がホストで作ったものを持ち込む。amd64 を Rosetta で動かすと
#         tar や objcopy が "Function not implemented" で落ちたので、Mac では arm64 のまま使う)、
#         openssh-server (試験用の sshd)、dropbear-bin (dropbearkey)、python3、rsync、git
#   入れないもの: XEiJ・HDS・x68kcli の stage (Human68k・gcc・zsh...)・ssh.x などの第三者のバイナリ。走らせるときに /mnt に読むだけで
#         つなぐ (docker/run-suite.sh)
FROM eclipse-temurin:26-jdk-noble
ARG ELF2X68K=20261004
ARG TARGETARCH
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      xvfb build-essential pkg-config libglib2.0-dev python3 rsync git curl bzip2 ca-certificates \
      openssh-server dropbear-bin binutils procps iproute2 perl \
      libxrender1 libxtst6 libxi6 libxext6 fontconfig fonts-dejavu-core \
    && rm -rf /var/lib/apt/lists/*
RUN [ "$TARGETARCH" != amd64 ] || { curl -fsSL -o /tmp/e.tbz https://github.com/yunkya2/elf2x68k/releases/download/$ELF2X68K/elf2x68k-Linux-$ELF2X68K.tar.bz2 \
    && mkdir -p /opt/elf2x68k && tar xjf /tmp/e.tbz -C /opt/elf2x68k && rm /tmp/e.tbz \
    && ln -s /opt/elf2x68k/m68k-xelf/bin/* /usr/local/bin/; }
# 試験の sshd はユーザ権限で動かす (zostest.sh)
RUN useradd -m -s /bin/bash tester && mkdir -p /run/sshd
COPY run-suite.sh /usr/local/bin/run-suite.sh
USER tester
WORKDIR /home/tester
ENTRYPOINT ["/usr/local/bin/run-suite.sh"]
