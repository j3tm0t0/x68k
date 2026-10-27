//! 0xA7 (Z-OS の見えないペインのセッション、net/npd/zss.h) の Pi 側。Pico の zsshd の代わりに、`zmux attach` のクライアントが
//! セッションを開く。harness/picosim/zss_mock.c と同じ規則 (確認応答・epoch は TSP と同じ、CREDIT は開いたときと出力を渡したあと)。
//!
//! セッションは 4 つまで (ZSS_NSESS)。クライアントが切れてもセッションは残し (出力の直近 64KB を持つ)、`zmux attach N` でつなぎ直せる。
//! zosh が終わる (EXIT) か `zmux kill N` で閉じる。

use std::collections::VecDeque;

pub const MARK: u8 = 0xa7;
const HDR: usize = 12;
const MHDR: usize = 6;
pub const NSESS: usize = 4;
const RES_MAX: usize = 4064;

const E_OPEN: u8 = 0x81;
const E_INPUT: u8 = 0x82;
const E_WINCH: u8 = 0x83;
const E_SIGNAL: u8 = 0x84;
const E_EOF: u8 = 0x85;
const E_CLOSED: u8 = 0x86;
const E_CREDIT: u8 = 0x87;
const C_ACCEPT: u8 = 0x01;
const C_REJECT: u8 = 0x02;
const C_OUTPUT: u8 = 0x03;
const C_WINDOW: u8 = 0x04;
const C_EXIT: u8 = 0x05;
const PF_MORE: u8 = 0x01;
const OF_UTF8: u8 = 0x01;

const CREDIT: u32 = 4096;
const SCROLL: usize = 65536;

#[derive(Default)]
pub struct Sess {
    pub used: bool,
    pub gen: u8,
    want_open: bool,
    opened: bool,
    pub accepted: bool,
    want_closed: Option<String>,
    win: u32,
    pub input: VecDeque<u8>,
    winch: Option<(u16, u16)>,
    sig: Option<String>,
    eof: bool,
    credit_dirty: bool,
    pub cols: u16,
    pub rows: u16,
    term: String,
    pub scroll: VecDeque<u8>,
    pub exit: Option<u32>,
}

/// 上の層 (デーモン) に知らせること
pub enum Out {
    Output(usize, Vec<u8>),      // セッションの出力 (クライアントへ)
    Exit(usize, u32),            // zosh が終わった
    Rejected(usize, String),
}

pub struct Zss {
    pub s: [Sess; NSESS],
    zepoch: Option<u32>,
    pepoch: u32,
    last_zseq: Option<u16>,
    last: Vec<u8>,
    pub log: bool,
    pub seen: bool, // zos.x の 0xA7 の要求が来たことがある
    pub res_max: usize,
}

fn g16(b: &[u8]) -> usize { ((b[0] as usize) << 8) | b[1] as usize }
fn g32(b: &[u8]) -> u32 { ((g16(b) as u32) << 16) | g16(&b[2..]) as u32 }

impl Zss {
    pub fn new() -> Zss {
        let pepoch = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_nanos() as u32).unwrap_or(7) ^ 0xa7a7_a7a7;
        Zss { s: Default::default(), zepoch: None, pepoch, last_zseq: None, last: Vec::new(), log: false, seen: false, res_max: RES_MAX }
    }

    /// 新しいセッションを開く (空きが無ければ None)
    pub fn open(&mut self, cols: u16, rows: u16, term: &str) -> Option<usize> {
        let i = self.s.iter().position(|s| !s.used)?;
        let gen = self.s[i].gen.wrapping_add(1);
        self.s[i] = Sess { used: true, gen, want_open: true, cols, rows, term: term.to_string(), credit_dirty: true, ..Default::default() };
        if self.log { eprintln!("zss: open sess {} gen {} {}x{} {}", i, gen, cols, rows, term); }
        Some(i)
    }

    pub fn input(&mut self, i: usize, d: &[u8]) {
        if self.s[i].used { self.s[i].input.extend(d.iter().copied()); }
    }

    pub fn winch(&mut self, i: usize, cols: u16, rows: u16) {
        if self.s[i].used && (self.s[i].cols, self.s[i].rows) != (cols, rows) {
            self.s[i].cols = cols;
            self.s[i].rows = rows;
            self.s[i].winch = Some((cols, rows));
        }
    }

    pub fn kill(&mut self, i: usize) {
        if self.s[i].used {
            if self.s[i].opened { self.s[i].want_closed = Some("killed".into()); } else { self.s[i].used = false; }
        }
    }

    /// Z に知らせることがあるか (KICK の判断)
    pub fn has_news(&self) -> bool {
        self.s.iter().any(|s| s.used && (s.want_open || s.want_closed.is_some() && s.opened
            || s.accepted && ((!s.input.is_empty() && s.win > 0) || s.winch.is_some() || s.sig.is_some() || s.eof)
            || s.opened && s.credit_dirty))
    }

    pub fn exchange(&mut self, req: &[u8], out: &mut Vec<Out>) -> Vec<u8> {
        if req.len() < HDR || req[0] != MARK {
            return Vec::new();
        }
        self.seen = true;
        let zseq = g16(&req[4..]) as u16;
        let ze = g32(&req[8..]);
        if self.zepoch == Some(ze) && self.last_zseq == Some(zseq) && !self.last.is_empty() {
            return self.last.clone();
        }
        if self.zepoch != Some(ze) {
            // zos.x が始め直した: 開いていたセッションは全部切る
            if self.zepoch.is_some() {
                for i in 0..NSESS {
                    if self.s[i].used && self.s[i].opened {
                        out.push(Out::Output(i, b"\r\n[Z-OS restarted]\r\n".to_vec()));
                        out.push(Out::Exit(i, 255));
                        self.s[i].used = false;
                    }
                }
            }
            self.zepoch = Some(ze);
        }
        let mut p = HDR;
        while p + MHDR <= req.len() {
            let (t, i, gen) = (req[p], req[p + 1] as usize, req[p + 2]);
            let n = g16(&req[p + 4..]);
            if p + MHDR + n > req.len() { break; }
            let b = &req[p + MHDR..p + MHDR + n];
            if i < NSESS && self.s[i].used && self.s[i].gen == gen {
                let s = &mut self.s[i];
                match t {
                    C_ACCEPT => { s.accepted = true; if n >= 4 { s.win = g32(b); } }
                    C_WINDOW => { if n >= 4 { s.win = g32(b); } }
                    C_OUTPUT => {
                        s.scroll.extend(b.iter().copied());
                        while s.scroll.len() > SCROLL { s.scroll.pop_front(); }
                        s.credit_dirty = true;
                        out.push(Out::Output(i, b.to_vec()));
                    }
                    C_REJECT => {
                        let why = if n > 1 { String::from_utf8_lossy(&b[1..1 + (b[0] as usize).min(n - 1)]).to_string() } else { String::new() };
                        out.push(Out::Rejected(i, why));
                        s.used = false;
                    }
                    C_EXIT => {
                        let st = if n >= 4 { g32(b) } else { 0 };
                        if self.log { eprintln!("zss: sess {} exit {}", i, st); }
                        s.exit = Some(st);
                        out.push(Out::Exit(i, st));
                        s.used = false; // Z は EXIT でこの gen を空きにする (CLOSED は送らなくてよい)
                    }
                    _ => {}
                }
            }
            p += MHDR + ((n + 1) & !1);
        }
        let lim = self.res_max.min(RES_MAX);
        let mut r = vec![MARK, 1, 0, 0, (zseq >> 8) as u8, zseq as u8, 0, 0];
        r.extend_from_slice(&self.pepoch.to_be_bytes());
        let mut more = false;
        fn put(r: &mut Vec<u8>, lim: usize, t: u8, i: usize, gen: u8, body: &[u8]) -> bool {
            let sz = MHDR + ((body.len() + 1) & !1);
            if r.len() + sz > lim { return false; }
            r.extend_from_slice(&[t, i as u8, gen, 0, (body.len() >> 8) as u8, body.len() as u8]);
            r.extend_from_slice(body);
            if body.len() & 1 == 1 { r.push(0); }
            true
        }
        for i in 0..NSESS {
            let s = &mut self.s[i];
            if !s.used { continue; }
            if s.want_open {
                let mut b = Vec::new();
                b.extend_from_slice(&s.cols.to_be_bytes());
                b.extend_from_slice(&s.rows.to_be_bytes());
                b.extend_from_slice(&0x7f00_0001u32.to_be_bytes());
                b.extend_from_slice(&0u16.to_be_bytes());
                b.push(4);
                b.extend_from_slice(b"zmux");
                let t = s.term.as_bytes();
                b.push(t.len().min(32) as u8);
                b.extend_from_slice(&t[..t.len().min(32)]);
                b.push(OF_UTF8);
                if !put(&mut r, lim, E_OPEN, i, s.gen, &b) { more = true; continue; }
                s.want_open = false;
                s.opened = true;
            }
            if !s.opened { continue; }
            if let Some(why) = s.want_closed.clone() {
                let mut b = vec![why.len() as u8];
                b.extend_from_slice(why.as_bytes());
                if put(&mut r, lim, E_CLOSED, i, s.gen, &b) { s.used = false; s.opened = false; } else { more = true; }
                continue;
            }
            if s.credit_dirty {
                if put(&mut r, lim, E_CREDIT, i, s.gen, &CREDIT.to_be_bytes()) { s.credit_dirty = false; } else { more = true; }
            }
            if s.accepted && !s.input.is_empty() && s.win > 0 {
                let room = lim.saturating_sub(r.len() + MHDR + 1);
                let n = s.input.len().min(s.win as usize).min(room);
                if n > 0 {
                    let b: Vec<u8> = s.input.drain(..n).collect();
                    put(&mut r, lim, E_INPUT, i, s.gen, &b);
                    s.win -= n as u32;
                }
                if !s.input.is_empty() { more = true; }
            }
            if s.accepted {
                if let Some((c, rw)) = s.winch {
                    let mut b = c.to_be_bytes().to_vec();
                    b.extend_from_slice(&rw.to_be_bytes());
                    if put(&mut r, lim, E_WINCH, i, s.gen, &b) { s.winch = None; }
                }
                if s.input.is_empty() {
                    if let Some(sig) = s.sig.clone() {
                        if put(&mut r, lim, E_SIGNAL, i, s.gen, sig.as_bytes()) { s.sig = None; }
                    }
                    if s.eof && put(&mut r, lim, E_EOF, i, s.gen, &[]) { s.eof = false; }
                }
            }
        }
        if more { r[2] |= PF_MORE; }
        self.last_zseq = Some(zseq);
        self.last = r.clone();
        r
    }
}
