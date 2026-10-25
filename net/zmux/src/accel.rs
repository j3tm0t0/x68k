//! 0xA5 (暗号アクセラレータ、net/accel/accel_proto.h) を Pi で計算する。Pico のファームと同じ受け渡し。
//!   要求: [MARK][op][id 2][引数...]
//!   応答: [MARK][op][id 2][status][reserved][usec 4][結果...]
//! 同じ要求がもう一度来たら (npd.x の送り直し) 前の結果をそのまま返す。

use ed25519_dalek::{Signature, Signer, SigningKey, Verifier, VerifyingKey};
use x25519_dalek::{PublicKey, StaticSecret};

pub const MARK: u8 = 0xa5;
const OP_PING: u8 = 0;
const OP_X25519: u8 = 1;
const OP_X25519_BASE: u8 = 2;
const OP_ED25519_VERIFY: u8 = 3;
const OP_ED25519_SIGN: u8 = 4;
const ST_OK: u8 = 0;
const ST_BADSIG: u8 = 1;
const ST_BADREQ: u8 = 2;
const ST_BADOP: u8 = 3;
const CAPS: u8 = 0x07; // verify / X25519 / sign
const MSG_MAX: usize = 1024;

#[derive(Default)]
pub struct Accel {
    last_req: Vec<u8>,
    last_res: Vec<u8>,
    pub count: u64,
}

fn arr32(b: &[u8]) -> [u8; 32] {
    let mut a = [0u8; 32];
    a.copy_from_slice(&b[..32]);
    a
}

impl Accel {
    pub fn exchange(&mut self, req: &[u8]) -> Vec<u8> {
        if req.len() < 4 || req[0] != MARK {
            return Vec::new();
        }
        if req == self.last_req.as_slice() && !self.last_res.is_empty() {
            return self.last_res.clone();
        }
        self.count += 1;
        let (op, a) = (req[1], &req[4..]);
        let t0 = std::time::Instant::now();
        let (status, reserved, result): (u8, u8, Vec<u8>) = match op {
            OP_PING => (ST_OK, CAPS, vec![1]),
            OP_X25519 if a.len() == 64 => {
                let s = StaticSecret::from(arr32(a));
                (ST_OK, 0, s.diffie_hellman(&PublicKey::from(arr32(&a[32..]))).as_bytes().to_vec())
            }
            OP_X25519_BASE if a.len() == 32 => (ST_OK, 0, PublicKey::from(&StaticSecret::from(arr32(a))).as_bytes().to_vec()),
            OP_ED25519_VERIFY if a.len() >= 96 && a.len() - 96 <= MSG_MAX => {
                let ok = VerifyingKey::from_bytes(&arr32(a)).ok().map(|pk| {
                    let mut sig = [0u8; 64];
                    sig.copy_from_slice(&a[32..96]);
                    pk.verify(&a[96..], &Signature::from_bytes(&sig)).is_ok()
                });
                (if ok == Some(true) { ST_OK } else { ST_BADSIG }, 0, Vec::new())
            }
            OP_ED25519_SIGN if a.len() >= 64 && a.len() - 64 <= MSG_MAX => {
                let sk = SigningKey::from_bytes(&arr32(a));
                (ST_OK, 0, sk.sign(&a[64..]).to_bytes().to_vec())
            }
            OP_PING..=OP_ED25519_SIGN => (ST_BADREQ, 0, Vec::new()),
            _ => (ST_BADOP, 0, Vec::new()),
        };
        let us = t0.elapsed().as_micros().min(u32::MAX as u128) as u32;
        let mut r = vec![MARK, op, req[2], req[3], status, reserved];
        r.extend_from_slice(&us.to_be_bytes());
        r.extend_from_slice(&result);
        self.last_req = req.to_vec();
        self.last_res = r.clone();
        r
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn x25519_rfc7748() {
        // RFC 7748 6.1: Alice の秘密鍵 -> 公開鍵
        let sk = hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
        let mut req = vec![MARK, OP_X25519_BASE, 0, 1];
        req.extend_from_slice(&sk);
        let r = Accel::default().exchange(&req);
        assert_eq!(r[4], ST_OK);
        assert_eq!(&r[10..], hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a").as_slice());
    }
    #[test]
    fn sign_then_verify() {
        let seed = [7u8; 32];
        let pk = SigningKey::from_bytes(&seed).verifying_key().to_bytes();
        let mut a = Accel::default();
        let mut req = vec![MARK, OP_ED25519_SIGN, 0, 2];
        req.extend_from_slice(&seed);
        req.extend_from_slice(&pk);
        req.extend_from_slice(b"hello");
        let r = a.exchange(&req);
        let sig = &r[10..];
        let mut v = vec![MARK, OP_ED25519_VERIFY, 0, 3];
        v.extend_from_slice(&pk);
        v.extend_from_slice(sig);
        v.extend_from_slice(b"hello");
        assert_eq!(a.exchange(&v)[4], ST_OK);
        v[40] ^= 1;
        assert_eq!(a.exchange(&v)[4], ST_BADSIG);
    }
    fn hex(s: &str) -> Vec<u8> {
        (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
    }
}
