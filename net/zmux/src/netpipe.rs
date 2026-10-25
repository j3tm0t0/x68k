//! チャネル 1: npd.x -m の交換を受けて答える (Pico W の netpipe と同じ形、harness/picosim/netpipe.c)。
//!
//!   要求: [magic "X6NR" 4][seq 4][len 2][rxmax 2][zip 4] + レコード [長さ 2][中身][偶数詰め物] の列
//!   応答: [magic 4][seq 4][len 2][more 2][stat 4] + レコード [長さ 2][空き 14][中身][偶数詰め物] の列
//! レコードの先頭 1 バイトで分ける: 0xA6 = TSP (tsp.rs)、0xA5 = 暗号 (まだ)、0xA7 = 端末のセッション (まだ)、4x = IPv4 (捨てる)。

use crate::tsp::{self, Tsp};
use crate::zss::{self, Zss};

pub const MAGIC_R: u32 = 0x5836_4e52;
const DATAMAX: usize = 4080;

#[derive(Default, Debug, Clone)]
pub struct Stats {
    pub xchg: u64,
    pub tsp: u64,
    pub dropped_ip: u64,
    pub unknown: u64,
}

pub struct Netpipe {
    pub tsp: Tsp,
    pub zss: Zss,
    pub zout: Vec<zss::Out>, // セッションから上の層への知らせ (デーモンが取り出す)
    pub stats: Stats,
}

fn g16(b: &[u8]) -> usize { ((b[0] as usize) << 8) | b[1] as usize }

impl Netpipe {
    pub fn new() -> Netpipe {
        Netpipe { tsp: Tsp::new(), zss: Zss::new(), zout: Vec::new(), stats: Stats::default() }
    }

    /// 要求のブロック 1 つに答える (応答のブロック)。形が変なら None
    pub fn exchange(&mut self, req: &[u8]) -> Option<Vec<u8>> {
        if req.len() < 16 || u32::from_be_bytes([req[0], req[1], req[2], req[3]]) != MAGIC_R {
            return None;
        }
        self.stats.xchg += 1;
        let txlen = g16(&req[8..]).min(req.len() - 16);
        let rxmax = g16(&req[10..]).min(DATAMAX);
        // TSP と 0xA7 の応答が両方載るように場所を分ける ([長さ 2][空き 14] + 偶数詰め物の分を引く)
        if self.zss.seen {
            self.tsp.res_max = (rxmax / 2).saturating_sub(18) & !1;
            self.zss.res_max = (rxmax / 2).saturating_sub(18) & !1;
        } else {
            self.tsp.res_max = rxmax.saturating_sub(18) & !1;
        }
        let mut res = vec![0u8; 16];
        let mut more = 0usize;
        let mut p = 16;
        let end = 16 + txlen;
        while p + 2 <= end {
            let len = g16(&req[p..]);
            if p + 2 + len > end { break; }
            let rec = &req[p + 2..p + 2 + len];
            if !rec.is_empty() && rec[0] == tsp::MARK {
                self.stats.tsp += 1;
                let r = self.tsp.exchange(rec);
                if !r.is_empty() && res.len() - 16 + 16 + ((r.len() + 1) & !1) <= rxmax {
                    if r[2] & 0x01 != 0 { more += 1; }
                    res.extend_from_slice(&(r.len() as u16).to_be_bytes());
                    res.extend_from_slice(&[0u8; 14]);
                    res.extend_from_slice(&r);
                    if r.len() & 1 == 1 { res.push(0); }
                }
            } else if !rec.is_empty() && rec[0] == zss::MARK {
                let r = self.zss.exchange(rec, &mut self.zout);
                if !r.is_empty() && res.len() + 16 + ((r.len() + 1) & !1) <= rxmax + 16 {
                    if r[2] & 0x01 != 0 { more += 1; }
                    res.extend_from_slice(&(r.len() as u16).to_be_bytes());
                    res.extend_from_slice(&[0u8; 14]);
                    res.extend_from_slice(&r);
                    if r.len() & 1 == 1 { res.push(0); }
                }
            } else if !rec.is_empty() && rec[0] >> 4 == 4 {
                self.stats.dropped_ip += 1;
            } else {
                self.stats.unknown += 1;
            }
            p += 2 + ((len + 1) & !1);
        }
        let n = res.len() - 16;
        if self.tsp.log {
            eprintln!("np: {:?} xchg seq {:08x} req {} res {} more {}", std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_millis() % 100000, u32::from_be_bytes([req[4], req[5], req[6], req[7]]), txlen, n, more);
        }
        res[0..8].copy_from_slice(&req[0..8]);
        res[8..10].copy_from_slice(&(n as u16).to_be_bytes());
        res[10..12].copy_from_slice(&(more as u16).to_be_bytes());
        Some(res)
    }
}
