# SSH クライアント移植 進捗 (2026-10-22)

## 動いていること
- dropbear 2022.83 の dbclient を elf2x68k でクロスビルドし **ssh.x (233KB, HU 形式)** を生成。
  一式は `net/ssh/dropbear/` (fetch.sh/build.sh、Mac で再現ビルド確認済み)。
- Z 実機で接続〜鍵交換まで通る: 同期ブロッキング connect → バナー交換 →
  KEXINIT → **curve25519 scalarmult ×2 完了** → session_loop(select=poll 自前) →
  現在 ed25519 ホスト鍵検証中。TCP/バナー/select/鍵交換の配管は全通。
- ソケットは elf2x68k 同梱 libsocket.a。xip 固有対処: 同期 connect / getaddrinfo を
  数値IP専用に上書き / タイマ由来の弱い乱数 / signal_pipe 無効化 (patches/)。

## 直近の実測 (b:pnetorig の素 xip, IPQoS none sshd:2222, ed25519 鍵)
- curve25519 が TweetNaCl i64[16] 実装で 68000 だと全乗算が libgcc __muldi3 経由。
  **ladder 1bit ≈ 1秒、scalarmult 1回 ≈ 5分**。1 回のフルハンドシェイクを実行中
  (scalarmult2回 + ed25519 検証 + 署名で合計 15〜20分規模)。まだ完走の出力は未取得。

## ブロッカー / リスク
- 性能のみ。暗号が遅すぎて実用に耐えない。ハンドシェイク完走に十数分。
- TOS≠0 受信破棄は lead が npd4.x で解決済み (本日より b:pnetorg4 + 素の sshd:22 に移行予定)。

## 次の手
1. b:pnetorg4 (TOS クリア) で素の sshd:22 に通ることを確認。
2. フルハンドシェイク完走 → `uname -a` 出力を取得し「実行実証」。
3. 実用化は curve25519/ed25519 の有限体乗算を 68000 向けに手最適化 (16x16 MULU)。別作業規模。
4. VT100 (branch vt100 の vt100.c) のリンク。出力1本/入力1本に集約済みで小変更。

## ETA
- 「1回完走の実証」: 本日中 (完走に ~20分/回 + 試行)。実用速度化は別タスク。
