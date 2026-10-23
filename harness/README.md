# harness

X68000 / X68000 Z を外から動かして様子を見るための道具。

| ディレクトリ | 中身 |
|---|---|
| [rcmd](rcmd) | `rcmd.x`: TCP 2323 でコマンドを受けて zsh で実行し、出力を返す (`-v` で出力をコンソールにも流す)。X68000 上でビルド |
| [raspi](raspi) | Pi / Mac 側: `x68` (コマンド実行) / `x68put` (ファイル送信) / `x68type` (コンソールにゆっくり打つ) / `x68key` (picokbd にキーを送る) / `x68con` (コンソールのログを読む)、シリアルログイン用の設定 |
| [picokbd](picokbd) | Pico W を USB キーボードにして Z に挿し、UDP 6869 でキーを送る (ESC、Ctrl+F11+Del のリセットなど) |
| [contee](contee) | `contee.sys`: 画面の出力 (IOCS `_B_PUTC` / `_B_PRINT`) を RS-232C にも流す。dev イメージの CONFIG.SYS に入る |
| [ztools](ztools) | Z の上で状態を見る小さな道具 (`sst.x` `ztap.x` `ztr.x` `zt.x` `memsz.x` `reboot.x`) |
| [xeij](xeij) | XEiJ (エミュレータ) で起動・操作・試験するスクリプトとエージェント。`build/` と `tools/` は [../x68kcli](../x68kcli) のものを使う |
| [picosim](picosim) | Mac の上の Pico W の代わり。XEiJ の SCSI ID 0/6 と Z のメールボックス 0xEA0060 をエージェントが TCP で渡し、netpipe・暗号アクセラレータ・LAN (libslirp) を答える。Z も Pico も使わずに npd.x / rcmd.x / ssh.x を試せる |
