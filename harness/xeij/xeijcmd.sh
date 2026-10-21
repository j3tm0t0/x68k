#!/usr/bin/env bash
# 起動中の XEiJ (エージェント付き) にコマンドを送る: screenshot | textcopy | call <class> <method>
cd "$(dirname "$0")/../../x68kcli"   # build/ と tools/ (XEiJ, rb-cli) は x68kcli のもの
printf '%s\n' "$*" > build/agent.cmd.tmp && mv build/agent.cmd.tmp build/agent.cmd
