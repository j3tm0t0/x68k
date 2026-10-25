# 実機 Compact XVI の手順 (raspi5 から)

Compact XVI を raspi5 と RS-232C でつなぎ、rx.x を送り込んでから、起動フロッピー (主記憶 2MB 用) を XVI 自身に書かせ、それで起動する。
システムフロッピーには何も書かない (作業は全部ドライブ 1 = B: の空きフロッピーで行う)。

## 用意 (済み)

- raspi5 に `raspi/setup.sh` を適用済み (`--dev /dev/ttyUSB0 --speed 9600 --mode console`)。
  - コマンド `sx` (sx.py)、`x68con` (ログ)、`x68type`、ログ係 `x68-serlog.service` (ログは `/var/log/x68k/x68con.log`)。
  - USB シリアルが挿さっていない間、ログ係は 5 秒ごとに開き直すだけで、挿せばそのまま使える。
  - 設定は `/etc/default/x68k-serial` (`X68_DEV=/dev/ttyUSB0`、`X68_BAUD=9600`)。`sx` はこの速さで始める。
- 起動フロッピーのイメージ: raspi5 の `~/x68k-serialboot/boot/serial/floppy/x68k-boot-2m.xdf` (12MB 用は `x68k-boot-12m.xdf`)。

## 要るもの

- RS-232C のクロスケーブル (XVI は D-sub 25 ピンのオス) と USB シリアル変換器。raspi5 の USB に挿す。
- Human68k のシステムフロッピー (ドライブ 0 = A:)。
- 空きの 2HD フロッピー 1 枚 (ドライブ 1 = B:)。

## 手順

### 1. つなぐ

1. USB シリアルを raspi5 に挿し、名前を確かめる。

   ```
   ls -l /dev/ttyUSB* /dev/serial/by-id/
   ```

   `/dev/ttyUSB0` でなければ、`/etc/default/x68k-serial` の `X68_DEV` をその名前 (by-id の名前が確実) にし、`sudo systemctl restart x68-serlog`。
2. クロスケーブルで XVI の RS-232C と USB シリアルをつなぐ。

### 2. XVI を起動し、B: を準備する (XVI のキーボードで)

1. システムフロッピーを A: に入れて電源を入れ、`A>` を待つ。
2. X-BASIC があるか確かめる。

   ```
   dir a:\basic2
   ```

   `BASIC.X` が無ければ、下の「うまくいかないとき」の 1 へ。
3. 空きフロッピーを B: に入れてフォーマットする (確かめの問いには Y)。

   ```
   format b:
   ```

4. B: に移ってから、コンソールを RS-232C にする。

   ```
   b:
   ctty aux
   ```

   これで画面への表示とキーボードからの入力が止まる。正常な動作。

### 3. rx.x を送り込む (raspi5 で)

```
cd ~/x68k-serialboot/boot/serial
sx probe                     # "prompt" と出れば 9600bps で通じている
sx stage1                    # COPY AUX RX.BAS → BASIC RX で rx.x を受ける (約 40 秒)
sx stage2                    # rx.x を起動して 19200bps に切り替える
```

`sx stage1` の最後に `RX.X ok: ... bytes, sum ...` と出れば成功。`sx stage2` で `rx.x at 19200: RX 1 4096 <空きKB>` と出る。
RX.BAS と RX.X は B: に書かれる (システムフロッピーには書かない)。段階 3 (`stage3`) は A: に書くので使わない。

### 4. (任意) 38400bps で取りこぼすかを見る

rx.x が 19200bps で動いている状態で:

```
head -c 60000 /dev/urandom > /tmp/r60k
sx --baud 19200 speed 38400                # rx.x と raspi5 を 38400bps に
sx --baud 38400 put /tmp/r60k 'B:\R60K.BIN'
sx --baud 38400 stats                       # 壊れていたフレームの数
sx --baud 38400 get 'B:\R60K.BIN' /tmp/r60k.back && cmp /tmp/r60k /tmp/r60k.back && echo SAME
sx --baud 38400 speed 19200                # 19200bps に戻す
```

`put` の行の KB/s と、`stats` の「bad frames」を控える。bad frames は送り直しで回復しているので、`SAME` なら中身は正しい。
38400bps で答えが返らなくなったら、XVI をリセットしてやり直す (手順 2 から)。

### 5. 起動フロッピーを書く (raspi5 で)

```
sx --baud 19200 writefd floppy/x68k-boot-2m.xdf B:
```

約 12 分。最後に `verify: ok` と出れば成功。B: のフロッピーは上書きされる (RX.X も消えるが、rx.x は主記憶で動いているので問題ない)。

### 6. 起動フロッピーで起動する

1. raspi5 の設定を 19200bps にする (起動フロッピーの AUTOEXEC.BAT は 19200bps で CTTY AUX にする)。

   ```
   sudo sed -i 's/^X68_BAUD=.*/X68_BAUD=19200/' /etc/default/x68k-serial
   sudo systemctl restart x68-serlog
   ```

2. XVI の電源を切り、書いたフロッピーを A: (ドライブ 0) に入れて電源を入れる。
3. 起動の最後に `rx: CTTY AUX (RS-232C) -- press a key within 2 s to keep the keyboard` と出る。
   - 何も押さなければ、コンソールが RS-232C になる。raspi5 で:

     ```
     sx probe
     sx run ver
     ```

   - 2 秒以内にキーを押せばキーボードのまま。`zosh` で zosh、`ssh -V` で ssh.x の版が出る。
     (主記憶 2MB では `zos` (Z-OS) のペインは起動しない。12MB に増やしたら `x68k-boot-12m.xdf` を使う)

コンソールの出力は `x68con` で追える (sx が動いている間は止まる)。

## うまくいかないとき

1. **BASIC.X が無い**: 段階 1 (`sx stage1`) は X-BASIC を使うので進めない。システムディスクの中身 (`dir a:\` と `dir a:\bin`) を控えて連絡する。
   無償公開版の Human68k 3.02 のディスクには `BASIC2\BASIC.X` が入っている。
2. **`sx probe` が "no prompt"**:
   - `ctty aux` を打ったか。打つ前の画面が `B>` だったか。
   - ケーブルがクロスか。USB シリアルの名前 (`X68_DEV`) が合っているか。`x68con` に何か出ているか。
   - SRAM の RS-232C の設定が 9600bps でない機械もある。`sx --baud 19200 probe`、`sx --baud 4800 probe` などを試す。
     通じた速さで `sx --baud <速さ> stage1` を実行する。XVI の `SWITCH.X` で 9600bps / 8 ビット / パリティなし / ストップビット 1 に戻してもよい。
   - 3 線だけのケーブルで XVI の送信が止まるなら、ハンドシェイク線も入ったフル結線のクロスケーブルを使う。
3. **`sx stage1` が途中で止まる**: 行ごとに検査して送り直すので、ふつうは待てば進む。X-BASIC がエラーで止まったら (XVI の画面に
   行番号付きのエラーと `Ok`)、XVI のキーボードで `system` と Enter を打って `B>` に戻し、`sx stage1` からやり直す。
4. **`sx stage2` で rx.x が答えない**: `sx term` で `B>` に `RX` と打ってみる (Ctrl+] で終わる)。画面に `rx: ready` が出ていれば
   速さの切り替えに失敗している。`sx --baud 19200 hello` を試す。
5. **`writefd` で `verify` が合わない**: もう一度 `writefd` を実行する。続けて合わなければ別のフロッピーで (`format b:` から)。
6. **起動フロッピーで起動しない**: システムフロッピーで起動し直し、手順 2 から。XVI の画面のメッセージを控えて連絡する。
7. **キーボードに戻したい**: raspi5 で `sx term` を開き、`ctty con` と打つ。
