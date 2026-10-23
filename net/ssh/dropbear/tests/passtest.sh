#!/bin/bash
# パスワードの入力 (x68k_pass.c) のホスト試験を組んで流す (tests/passtest.c)。ASan/UBSan 付き。
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-unused-function \
  -o "$T/passtest" "$HERE/passtest.c"
"$T/passtest"
