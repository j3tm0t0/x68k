/* x68k dbclient 用の機能選択。68000/Human68k 向けに最小化する。
   鍵交換 curve25519 のみ、暗号 chacha20-poly1305 のみ、ホスト鍵/認証 ed25519 のみ。
   RSA/DSS/ECDSA と AES を落とすと libtommath の出番がほぼ無くなり、小さく速くなる。
   fork を使う機能 (proxycmd/netcat/multihop/各種フォワード/agent) は全部無効。 */

/* クライアントのみ。サーバ機能は落とすが、sysoptions.h が「サーバ認証を
   最低 1 つ」要求するので SVR_PUBKEY_AUTH だけ既定(1)のまま残す
   (dbclient には server のオブジェクトはリンクされない)。crypt() が無いので
   SVR_PASSWORD_AUTH は 0 にする。 */
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_SVR_PAM_AUTH 0
#define DROPBEAR_SVR_LOCALTCPFWD 0
#define DROPBEAR_SVR_REMOTETCPFWD 0
#define DROPBEAR_SVR_AGENTFWD 0
#define DROPBEAR_SVR_PUBKEY_OPTIONS 0

/* 鍵交換: curve25519 のみ */
#define DROPBEAR_CURVE25519 1
#define DROPBEAR_DH_GROUP14_SHA1 0
#define DROPBEAR_DH_GROUP14_SHA256 0
#define DROPBEAR_DH_GROUP16 0
#define DROPBEAR_ECDH 0
#define DROPBEAR_CURVE25519_DEP 0

/* 暗号: chacha20-poly1305 のみ (AEAD。別 MAC 不要、AES テーブルも不要) */
#define DROPBEAR_CHACHA20POLY1305 1
#define DROPBEAR_AES128 0
#define DROPBEAR_AES256 0
#define DROPBEAR_3DES 0
#define DROPBEAR_ENABLE_CTR_MODE 0
#define DROPBEAR_ENABLE_CBC_MODE 0

/* MAC: chacha20poly1305 が AEAD なので HMAC は保険で sha256 だけ残す */
#define DROPBEAR_SHA1_HMAC 0
#define DROPBEAR_SHA1_96_HMAC 0
#define DROPBEAR_SHA2_256_HMAC 1
#define DROPBEAR_SHA2_512_HMAC 0

/* 公開鍵: ed25519 のみ */
#define DROPBEAR_RSA 0
#define DROPBEAR_DSS 0
#define DROPBEAR_ECDSA 0
#define DROPBEAR_ED25519 1
#define DROPBEAR_SK_ECDSA 0
#define DROPBEAR_SK_ED25519 0

/* fork を使う / 不要な機能を全部無効 */
#define DROPBEAR_CLI_PROXYCMD 0
#define DROPBEAR_CLI_NETCAT 0
#define DROPBEAR_CLI_LOCALTCPFWD 0
#define DROPBEAR_CLI_REMOTETCPFWD 0
#define DROPBEAR_CLI_AGENTFWD 0
#define DROPBEAR_CLI_ASKPASS_HELPER 0
#define DROPBEAR_CLI_PASSWORD_AUTH 0
#define DROPBEAR_CLI_PUBKEY_AUTH 1
#define DROPBEAR_USE_SSH_CONFIG 0

/* 圧縮なし */
#define DROPBEAR_ZLIB 0

/* エントロピー源: /dev/urandom は無い。x68k シムで差し替える (dbrandom を自前化) */
#define DROPBEAR_X68K 1

/* RSA を切ったので RSA 署名も明示的に無効化 (既定で 1 のまま残ると signkey.c が
   DROPBEAR_SIGNKEY_RSA を参照してビルドに失敗する) */
#undef DROPBEAR_RSA_SHA1
#undef DROPBEAR_RSA_SHA256
#define DROPBEAR_RSA_SHA1 0
#define DROPBEAR_RSA_SHA256 0
#define DROPBEAR_CLIENT_TCP_FAST_OPEN 0
