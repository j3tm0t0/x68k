//! 線の層: フレームの組み立てと読み取り、Go-Back-N の送り直し (net/zmux/DESIGN.md §1)。
//! X68000 側の net/zmux/x68k/zmux.c と同じ規則。時計は呼ぶ側が渡す (試験しやすいように)。

use std::collections::VecDeque;
use std::time::{Duration, Instant};

pub const DLE: u8 = 0x10;
pub const STX: u8 = 0x02;
pub const ETX: u8 = 0x03;
pub const MAXLEN: usize = 1024;
pub const WIN: u8 = 4;

pub const F_NAK: u8 = 0x01;
pub const F_BUSY: u8 = 0x02;
pub const F_UNSEQ: u8 = 0x04;
pub const F_END: u8 = 0x08; // チャネル 1: 交換のブロックの最後のフレーム

pub const CH_CON: u8 = 0;
pub const CH_NP: u8 = 1;
pub const CH_KEY: u8 = 3;
pub const CH_CTL: u8 = 15;
pub const OP_HELLO: u8 = 1;
pub const OP_HELLO_ACK: u8 = 2;
pub const OP_BYE: u8 = 3;
pub const OP_KICK: u8 = 4;

const T_RESEND: Duration = Duration::from_millis(1000);
const T_ACK: Duration = Duration::from_millis(20);
const T_KEEP: Duration = Duration::from_millis(2000);
const T_DEAD: Duration = Duration::from_millis(10000);

pub fn crc16(mut c: u16, data: &[u8]) -> u16 {
    for &b in data {
        c ^= (b as u16) << 8;
        for _ in 0..8 {
            c = if c & 0x8000 != 0 { (c << 1) ^ 0x1021 } else { c << 1 };
        }
    }
    c
}

/// フレーム 1 つを線のバイト列にする (DLE を重ねる)
pub fn encode(chan: u8, seq: u8, ack: u8, flags: u8, data: &[u8], out: &mut Vec<u8>) {
    let h = [chan, seq, ack, flags, (data.len() >> 8) as u8, data.len() as u8];
    let c = crc16(crc16(0xffff, &h), data);
    out.push(DLE);
    out.push(STX);
    for &b in h.iter().chain(data.iter()).chain([(c >> 8) as u8, c as u8].iter()) {
        if b == DLE {
            out.push(DLE);
        }
        out.push(b);
    }
    out.push(DLE);
    out.push(ETX);
}

#[derive(Debug, Clone, PartialEq)]
pub struct Frame {
    pub chan: u8,
    pub seq: u8,
    pub ack: u8,
    pub flags: u8,
    pub data: Vec<u8>,
}

/// 線から来たバイトの読み取り。フレームの外のバイトは「素の文字」として返す
#[derive(Default)]
pub struct Parser {
    state: u8, // 0 = 外, 1 = 外で DLE, 2 = 中, 3 = 中で DLE
    buf: Vec<u8>,
    pub bad: u64,
}

pub enum Item {
    Raw(u8),
    Frame(Frame),
    Bad,
}

impl Parser {
    pub fn feed(&mut self, b: u8, out: &mut Vec<Item>) {
        match self.state {
            0 => {
                if b == DLE {
                    self.state = 1;
                } else {
                    out.push(Item::Raw(b));
                }
            }
            1 => {
                if b == STX {
                    self.buf.clear();
                    self.state = 2;
                } else {
                    self.state = 0;
                    out.push(Item::Raw(DLE));
                    out.push(Item::Raw(b));
                }
            }
            2 => {
                if b == DLE {
                    self.state = 3;
                } else {
                    self.push(b);
                }
            }
            _ => match b {
                DLE => {
                    self.state = 2;
                    self.push(DLE);
                }
                STX => {
                    self.buf.clear();
                    self.state = 2;
                }
                ETX => {
                    self.state = 0;
                    match self.check() {
                        Some(f) => out.push(Item::Frame(f)),
                        None => {
                            self.bad += 1;
                            out.push(Item::Bad);
                        }
                    }
                }
                _ => {
                    self.state = 0;
                    self.bad += 1;
                    out.push(Item::Bad);
                }
            },
        }
    }

    fn push(&mut self, b: u8) {
        if self.buf.len() < MAXLEN + 8 {
            self.buf.push(b);
        } else {
            self.state = 0;
        }
    }

    fn check(&self) -> Option<Frame> {
        let f = &self.buf;
        if f.len() < 8 {
            return None;
        }
        let len = ((f[4] as usize) << 8) | f[5] as usize;
        if len != f.len() - 8 || len > MAXLEN {
            return None;
        }
        let got = ((f[f.len() - 2] as u16) << 8) | f[f.len() - 1] as u16;
        if crc16(0xffff, &f[..f.len() - 2]) != got {
            return None;
        }
        Some(Frame { chan: f[0] & 15, seq: f[1], ack: f[2], flags: f[3], data: f[6..6 + len].to_vec() })
    }
}

/// 線の状態 (Pi 側)。X68000 の HELLO で UP になり、10 秒フレームが来なければ RAW に戻る
pub struct Link {
    pub up: bool,
    pub parser: Parser,
    tx_seq: u8,
    tx_base: u8,
    rx_expect: u8,
    win: VecDeque<(u8, u8, Vec<u8>)>, // 確認の来ていないフレーム (chan, flags, data)。先頭が tx_base
    queue: [VecDeque<(Vec<u8>, bool)>; 16], // チャネルごとの送り待ち。bool = ブロックの終わり (F_END を付ける)
    need_ack: bool,
    nak: bool,
    peer_busy: bool,
    t_sent: Instant,
    t_last_tx: Instant,
    t_last_rx: Instant,
    t_ack_due: Instant,
    busy_until: Instant, // 線に書いたバイトを送り終える見込みの時刻
    t_done: Instant,     // 確認待ちのデータのフレームを送り終える見込みの時刻 (送り直しの時計はここから数える。KICK などでは動かさない)
    pub byte_time: Duration, // 1 バイトの時間 (10 ビット / 速さ)
    pub stats: Stats,
}

#[derive(Default, Debug, Clone)]
pub struct Stats {
    pub rx_frames: u64,
    pub tx_frames: u64,
    pub resent: u64,
    pub ups: u64,
    pub downs: u64,
}

/// 線から来たものを、上の層に渡す形
pub enum Event {
    Raw(Vec<u8>),          // 素の文字 (RAW のとき)
    Data(u8, Vec<u8>, u8), // チャネルのデータ (UP のとき)、フレームの flags
    Up,
    Down,
}

impl Link {
    pub fn new(now: Instant) -> Link {
        Link {
            up: false,
            parser: Parser::default(),
            tx_seq: 0,
            tx_base: 0,
            rx_expect: 0,
            win: VecDeque::new(),
            queue: Default::default(),
            need_ack: false,
            nak: false,
            peer_busy: false,
            t_sent: now,
            t_last_tx: now,
            t_last_rx: now,
            t_ack_due: now,
            busy_until: now,
            t_done: now,
            byte_time: Duration::from_micros(10_000_000 / 19200),
            stats: Stats::default(),
        }
    }

    fn reset(&mut self, now: Instant) {
        self.tx_seq = 0;
        self.tx_base = 0;
        self.rx_expect = 0;
        // 送りかけのコンソールの文字はチャネルの待ち行列の先頭に戻す (作り直した線で送る)。
        // チャネル 1 の交換は捨てる (X68000 の npd.x は時間切れで送り直す)
        while let Some((ch, fl, d)) = self.win.pop_back() {
            if ch == CH_CON {
                self.queue[ch as usize].push_front((d, fl & F_END != 0));
            }
        }
        self.queue[CH_NP as usize].clear();
        self.need_ack = false;
        self.nak = false;
        self.peer_busy = false;
        self.t_last_rx = now;
        self.t_sent = now;
    }

    fn frame(&mut self, chan: u8, seq: u8, flags: u8, data: &[u8], out: &mut Vec<u8>, now: Instant) {
        let before = out.len();
        encode(chan, seq, self.rx_expect.wrapping_sub(1), flags, data, out);
        let start = if self.busy_until > now { self.busy_until } else { now };
        self.busy_until = start + self.byte_time * (out.len() - before) as u32;
        self.stats.tx_frames += 1;
        self.need_ack = false;
        self.t_last_tx = now;
    }

    fn ctl(&mut self, op: u8, out: &mut Vec<u8>, now: Instant) {
        let d = [op, 1, (MAXLEN >> 8) as u8, MAXLEN as u8];
        let keep = self.t_last_tx;
        self.frame(CH_CTL, 0, F_UNSEQ, &d, out, now);
        if op == OP_KICK {
            self.t_last_tx = keep; // KICK は生存確認の代わりにしない (確認の番号を運ばないので)
        }
    }

    /// チャネルに送るデータを積む (RAW のチャネル 0 は呼ぶ側が直接線に書く)
    pub fn send(&mut self, chan: u8, data: &[u8]) {
        if !data.is_empty() {
            self.queue[chan as usize].push_back((data.to_vec(), false));
        }
    }

    /// ひとまとまりのブロック (チャネル 1 の交換の応答) を積む。最後のフレームに F_END が付く
    pub fn send_block(&mut self, chan: u8, data: &[u8]) {
        self.queue[chan as usize].push_back((data.to_vec(), true));
    }

    /// 番号を使わない制御のフレーム (KICK など) をすぐ線に出す
    pub fn ctl_now(&mut self, op: u8, out: &mut Vec<u8>, now: Instant) {
        self.ctl(op, out, now);
    }

    pub fn debug(&self) -> String {
        let now = Instant::now();
        format!("win {} base {} seq {} expect {} busy {} t_sent {:?} ago, t_done {:?} q {:?}", self.win.len(), self.tx_base, self.tx_seq, self.rx_expect, self.peer_busy,
                now.saturating_duration_since(self.t_sent), self.t_done.saturating_duration_since(now),
                self.queue.iter().map(|q| q.iter().map(|c| c.0.len()).sum::<usize>()).collect::<Vec<_>>())
    }

    pub fn pending(&self) -> usize {
        self.queue.iter().map(|q| q.iter().map(|c| c.0.len()).sum::<usize>()).sum::<usize>() + self.win.len()
    }

    /// 線から来たバイト列を処理する。返事は out に、上の層への知らせは ev に
    pub fn input(&mut self, bytes: &[u8], out: &mut Vec<u8>, ev: &mut Vec<Event>, now: Instant) {
        let mut items = Vec::new();
        for &b in bytes {
            self.parser.feed(b, &mut items);
        }
        let mut raw = Vec::new();
        for it in items {
            match it {
                Item::Raw(b) => {
                    if !self.up {
                        raw.push(b);
                    }
                }
                Item::Bad => {
                    if self.up {
                        self.nak = true;
                    }
                }
                Item::Frame(f) => {
                    if !raw.is_empty() {
                        ev.push(Event::Raw(std::mem::take(&mut raw)));
                    }
                    self.got(f, out, ev, now);
                }
            }
        }
        if !raw.is_empty() {
            ev.push(Event::Raw(raw));
        }
    }

    fn got(&mut self, f: Frame, out: &mut Vec<u8>, ev: &mut Vec<Event>, now: Instant) {
        self.stats.rx_frames += 1;
        if self.up {
            self.t_last_rx = now; // 番号の無いフレームも生きている印
        }
        if f.flags & F_UNSEQ != 0 {
            if f.chan == CH_CTL && !f.data.is_empty() {
                match f.data[0] {
                    OP_HELLO => {
                        // X68000 が始めた (zmux.sys が RAW のまま、または作り直した)
                        self.reset(now);
                        if !self.up {
                            self.up = true;
                            self.stats.ups += 1;
                            ev.push(Event::Up);
                        }
                        self.ctl(OP_HELLO_ACK, out, now);
                    }
                    OP_BYE => self.go_down(ev),
                    _ => {}
                }
            }
            return;
        }
        if !self.up {
            return;
        }
        self.t_last_rx = now;
        let n = f.ack.wrapping_sub(self.tx_base).wrapping_add(1);
        if n as usize <= self.win.len() {
            for _ in 0..n {
                self.win.pop_front();
            }
            self.tx_base = self.tx_base.wrapping_add(n);
            if n > 0 {
                self.t_sent = now;
            }
        }
        self.peer_busy = f.flags & F_BUSY != 0;
        if f.flags & F_NAK != 0 && !self.win.is_empty() {
            self.t_sent = now - T_RESEND;
        }
        if f.data.is_empty() {
            return;
        }
        if f.seq != self.rx_expect {
            self.nak = true;
            return;
        }
        self.rx_expect = self.rx_expect.wrapping_add(1);
        if !self.need_ack {
            self.t_ack_due = now;
        }
        self.need_ack = true;
        ev.push(Event::Data(f.chan, f.data, f.flags));
    }

    fn go_down(&mut self, ev: &mut Vec<Event>) {
        if self.up {
            self.up = false;
            self.stats.downs += 1;
            ev.push(Event::Down);
        }
    }

    /// 時間で動くもの (送り直し、確認、生存確認、新しいデータの送信)
    pub fn tick(&mut self, out: &mut Vec<u8>, ev: &mut Vec<Event>, now: Instant) {
        if !self.up {
            return;
        }
        if now.duration_since(self.t_last_rx) > T_DEAD {
            self.go_down(ev);
            return;
        }
        let since_sent = if self.t_done > self.t_sent { self.t_done } else { self.t_sent };
        if !self.win.is_empty() && now > since_sent && now.duration_since(since_sent) >= T_RESEND {
            let frames: Vec<(u8, u8, Vec<u8>)> = self.win.iter().cloned().collect();
            let mut s = self.tx_base;
            for (ch, fl, d) in frames {
                self.frame(ch, s, fl, &d, out, now);
                self.stats.resent += 1;
                s = s.wrapping_add(1);
            }
            self.t_sent = now;
            self.t_done = self.busy_until;
        }
        if !self.peer_busy {
            for ch in 0..16u8 {
                while (self.win.len() as u8) < WIN {
                    let q = &mut self.queue[ch as usize];
                    // 送るものを 1 フレーム分集める。ブロックの終わりはまたがない
                    let mut d = Vec::new();
                    let mut fl = 0u8;
                    while d.len() < MAXLEN {
                        let Some((c, end)) = q.front_mut() else { break };
                        let take = (MAXLEN - d.len()).min(c.len());
                        d.extend(c.drain(..take));
                        if c.is_empty() {
                            let e = *end;
                            q.pop_front();
                            if e {
                                fl = F_END;
                                break;
                            }
                        }
                    }
                    if d.is_empty() && fl == 0 {
                        break;
                    }
                    if self.win.is_empty() {
                        self.t_sent = now;
                    }
                    let seq = self.tx_seq;
                    self.frame(ch, seq, fl, &d, out, now);
                    self.t_done = self.busy_until;
                    self.win.push_back((ch, fl, d));
                    self.tx_seq = self.tx_seq.wrapping_add(1);
                }
            }
        }
        let ack_due = self.need_ack && now.duration_since(self.t_ack_due) >= T_ACK;
        if ack_due || self.nak || now.duration_since(self.t_last_tx) >= T_KEEP {
            let fl = if self.nak { F_NAK } else { 0 };
            self.frame(CH_CON, 0, fl, &[], out, now);
            self.nak = false;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip_with_dle() {
        let data: Vec<u8> = (0..=255u8).chain([DLE, DLE, STX, ETX].iter().copied()).collect();
        let mut out = Vec::new();
        encode(3, 7, 9, 0, &data, &mut out);
        let mut p = Parser::default();
        let mut items = Vec::new();
        for &b in b"text " {
            p.feed(b, &mut items);
        }
        for &b in &out {
            p.feed(b, &mut items);
        }
        let frames: Vec<Frame> = items.into_iter().filter_map(|i| if let Item::Frame(f) = i { Some(f) } else { None }).collect();
        assert_eq!(frames.len(), 1);
        assert_eq!(frames[0].data, data);
        assert_eq!((frames[0].chan, frames[0].seq, frames[0].ack), (3, 7, 9));
    }

    #[test]
    fn corrupted_frame_is_bad() {
        let mut out = Vec::new();
        encode(0, 0, 0, 0, b"hello", &mut out);
        out[8] ^= 0x40;
        let mut p = Parser::default();
        let mut items = Vec::new();
        for &b in &out {
            p.feed(b, &mut items);
        }
        assert!(items.iter().any(|i| matches!(i, Item::Bad)));
        assert!(!items.iter().any(|i| matches!(i, Item::Frame(_))));
    }

    /// 2 つの Link を 10% のバイトを壊す線でつないで、チャネル 0 のデータが順に全部届くか
    #[test]
    fn two_links_over_lossy_line() {
        let t0 = Instant::now();
        let mut a = Link::new(t0);
        let mut b = Link::new(t0);
        // b は X68000 の代わり: HELLO を送って両方 UP にする
        let mut wire_ab = Vec::new();
        let mut wire_ba = Vec::new();
        encode(CH_CTL, 0, 0xff, F_UNSEQ, &[OP_HELLO, 1, 4, 0], &mut wire_ba);
        let mut ev = Vec::new();
        a.input(&wire_ba, &mut wire_ab, &mut ev, t0);
        assert!(a.up);
        b.up = true; // b は HELLO_ACK を受けたものとする
        wire_ab.clear();
        wire_ba.clear();
        let msg: Vec<u8> = (0..20000u32).map(|i| (i * 7 % 251) as u8).collect();
        a.send(CH_CON, &msg);
        let mut got = Vec::new();
        let mut rng = 12345u32;
        let mut now = t0;
        for _ in 0..20000 {
            now += Duration::from_millis(5);
            let mut ea = Vec::new();
            let mut eb = Vec::new();
            a.tick(&mut wire_ab, &mut ea, now);
            b.tick(&mut wire_ba, &mut eb, now);
            let mut corrupt = |w: &mut Vec<u8>| {
                for x in w.iter_mut() {
                    rng = rng.wrapping_mul(1103515245).wrapping_add(12345);
                    if (rng >> 16) % 2000 == 0 {
                        *x ^= 0x55;
                    }
                }
            };
            corrupt(&mut wire_ab);
            corrupt(&mut wire_ba);
            let (ab, ba) = (std::mem::take(&mut wire_ab), std::mem::take(&mut wire_ba));
            b.input(&ab, &mut wire_ba, &mut eb, now);
            a.input(&ba, &mut wire_ab, &mut ea, now);
            for e in eb {
                if let Event::Data(0, d, _) = e {
                    got.extend(d);
                }
            }
            if got.len() == msg.len() {
                break;
            }
        }
        assert_eq!(got.len(), msg.len());
        assert_eq!(got, msg);
        assert!(a.stats.resent > 0);
    }
}
