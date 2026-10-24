//! TSP (tis.x -p のソケット要求、0xA6) を Pi の普通のソケットで答える (net/tistack/tsp.h、net/tistack/pico/tsp_svc.c と同じ規則)。
//!
//! Z の要求 1 個に応答 1 個。zseq が前回と同じなら中身を適用せず前の応答を返す。zepoch が変わったらソケットを全部捨てる。
//! 流量: Pi → Z は Z の受信リングの空き (WINDOW) まで。Z → Pi は応答のたびに CREDIT (次の要求で送ってよい量、絶対値)。
//! 接続と名前引きは別スレッドで (結果は次の交換で知らせる)。

use std::collections::VecDeque;
use std::io::{ErrorKind, Read, Write};
use std::net::{Ipv4Addr, Shutdown, SocketAddr, SocketAddrV4, TcpListener, TcpStream, ToSocketAddrs, UdpSocket};
use std::sync::mpsc;
use std::time::Duration;

pub const MARK: u8 = 0xa6;
const HDR: usize = 12;
const MHDR: usize = 6;
const NSLOT: usize = 32;
pub const RES_MAX: usize = 4064;
const NQUERY: usize = 4;

const C_CONNECT: u8 = 0x01;
const C_LISTEN: u8 = 0x02;
const C_UDPOPEN: u8 = 0x03;
const C_DATA: u8 = 0x04;
const C_SENDTO: u8 = 0x05;
const C_WINDOW: u8 = 0x06;
const C_SHUT: u8 = 0x07;
const C_CLOSE: u8 = 0x08;
const C_ADOPT: u8 = 0x09;
const C_REJECT: u8 = 0x0a;
const C_RESOLVE: u8 = 0x0b;

const E_CONNECTED: u8 = 0x81;
const E_ACCEPT: u8 = 0x82;
const E_DATA: u8 = 0x83;
const E_DGRAM: u8 = 0x84;
const E_CREDIT: u8 = 0x85;
const E_EOF: u8 = 0x86;
const E_ERROR: u8 = 0x87;
const E_RESOLVED: u8 = 0x88;
const E_BOUND: u8 = 0x89;

const ZF_RESET: u8 = 0x01;
const PF_MORE: u8 = 0x01;
const CF_NAME: u8 = 0x01;
const CF_TLS: u8 = 0x02;
const AF_LISTENER: u8 = 0x01;

const ERR_REFUSED: u8 = 1;
const ERR_TIMEOUT: u8 = 2;
const ERR_UNREACH: u8 = 3;
const ERR_RESET: u8 = 4;
const ERR_NONAME: u8 = 6;
const ERR_NOTSUP: u8 = 7;
const ERR_INUSE: u8 = 8;

const TXCAP: usize = 16384; // Z から受けて、まだネットに書けていない量の上限 (CREDIT の上限)
const RXCAP: usize = 32768; // ネットから受けて、まだ Z に渡していない量の上限 (これを超えたら読まない)
const UDPQ: usize = 4;

#[derive(PartialEq, Clone, Copy, Debug)]
enum St {
    Connecting,
    Est,
    Listen,
    Udp,
    Dead,
}

struct PSock {
    st: St,
    zslot: Option<u8>,
    zgen: u8,
    listener: Option<usize>, // 子: 親の listen (Some の間は引き取られていない)
    announce: bool,
    connected_ev: bool,
    bound_ev: bool,
    eof: bool,
    eof_sent: bool,
    err: Option<u8>,
    closing: bool,
    shut_pending: bool,
    serial: u32,
    tcp: Option<TcpStream>,
    lis: Option<TcpListener>,
    udp: Option<UdpSocket>,
    rxq: VecDeque<u8>,
    txq: VecDeque<u8>,
    uq: VecDeque<(Ipv4Addr, u16, Vec<u8>)>,
    zwin: u32,
    zcred: u32,
    lport: u16,
}

impl PSock {
    fn new(serial: u32) -> PSock {
        PSock {
            st: St::Connecting, zslot: None, zgen: 0, listener: None, announce: false, connected_ev: false,
            bound_ev: false, eof: false, eof_sent: false, err: None, closing: false, shut_pending: false, serial,
            tcp: None, lis: None, udp: None, rxq: VecDeque::new(), txq: VecDeque::new(), uq: VecDeque::new(),
            zwin: 0, zcred: 0, lport: 0,
        }
    }
    fn fail(&mut self, code: u8) {
        self.st = St::Dead;
        self.err = Some(code);
        self.tcp = None;
        self.lis = None;
        self.udp = None;
    }
}

enum Done {
    Connect(usize, u32, std::io::Result<TcpStream>),
    Resolve(usize, u32, Option<Ipv4Addr>),
}

pub struct Tsp {
    ps: Vec<Option<PSock>>,
    z2p: [Option<usize>; NSLOT],
    q: [(u8, u8, u32, u32); NQUERY], // (state 0 空き / 1 引いている / 2 答え待ち, code, addr, serial)
    zepoch: Option<u32>,
    pepoch: u32,
    last_zseq: Option<u16>,
    last: Vec<u8>,
    serial: u32,
    done_tx: mpsc::Sender<Done>,
    done_rx: mpsc::Receiver<Done>,
    pub log: bool,
    pub self_addr: u32, // この番地への接続は Pi 自身 (127.0.0.1) へ
}

fn g16(b: &[u8]) -> u16 { ((b[0] as u16) << 8) | b[1] as u16 }
fn g32(b: &[u8]) -> u32 { ((g16(b) as u32) << 16) | g16(&b[2..]) as u32 }

fn err_code(e: &std::io::Error) -> u8 {
    match e.kind() {
        ErrorKind::ConnectionRefused => ERR_REFUSED,
        ErrorKind::TimedOut | ErrorKind::WouldBlock => ERR_TIMEOUT,
        ErrorKind::ConnectionReset | ErrorKind::ConnectionAborted | ErrorKind::BrokenPipe => ERR_RESET,
        ErrorKind::AddrInUse | ErrorKind::PermissionDenied => ERR_INUSE,
        _ => ERR_UNREACH,
    }
}

fn v4(a: &SocketAddr) -> (u32, u16) {
    match a {
        SocketAddr::V4(s) => (u32::from(*s.ip()), s.port()),
        SocketAddr::V6(s) => (s.ip().to_ipv4_mapped().map(u32::from).unwrap_or(0), s.port()),
    }
}

impl Tsp {
    pub fn new() -> Tsp {
        let (done_tx, done_rx) = mpsc::channel();
        let pepoch = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_nanos() as u32).unwrap_or(1) | 1;
        Tsp {
            ps: Vec::new(), z2p: [None; NSLOT], q: [(0, 0, 0, 0); NQUERY], zepoch: None, pepoch, last_zseq: None,
            last: Vec::new(), serial: 0, done_tx, done_rx, log: false, self_addr: u32::from(Ipv4Addr::new(192, 168, 68, 1)),
        }
    }

    fn alloc(&mut self) -> usize {
        self.serial += 1;
        let s = PSock::new(self.serial);
        if let Some(i) = self.ps.iter().position(|p| p.is_none()) {
            self.ps[i] = Some(s);
            i
        } else {
            self.ps.push(Some(s));
            self.ps.len() - 1
        }
    }

    fn free(&mut self, i: usize) {
        if let Some(p) = self.ps[i].take() {
            if let Some(z) = p.zslot {
                if self.z2p[z as usize] == Some(i) {
                    self.z2p[z as usize] = None;
                }
            }
            if p.st == St::Listen {
                // 引き取られていない子も閉じる
                for k in 0..self.ps.len() {
                    if self.ps[k].as_ref().map(|c| c.listener == Some(i)).unwrap_or(false) {
                        self.ps[k] = None;
                    }
                }
            }
        }
    }

    fn reset(&mut self) {
        self.ps.clear();
        self.z2p = [None; NSLOT];
        self.q = [(0, 0, 0, 0); NQUERY];
    }

    /// Z の番号 slot に新しいソケットを付ける (前のものは捨てる)
    fn attach(&mut self, slot: u8, gen: u8) -> Option<usize> {
        if slot as usize >= NSLOT {
            return None;
        }
        if let Some(old) = self.z2p[slot as usize] {
            self.free(old);
        }
        let i = self.alloc();
        let p = self.ps[i].as_mut().unwrap();
        p.zslot = Some(slot);
        p.zgen = gen;
        self.z2p[slot as usize] = Some(i);
        Some(i)
    }

    fn by_slot(&mut self, slot: u8, gen: u8) -> Option<usize> {
        let i = (*self.z2p.get(slot as usize)?)?;
        if self.ps[i].as_ref()?.zgen == gen { Some(i) } else { None }
    }

    fn start_connect(&mut self, i: usize, addr: SocketAddr) {
        let serial = self.ps[i].as_ref().unwrap().serial;
        let tx = self.done_tx.clone();
        std::thread::spawn(move || {
            let r = TcpStream::connect_timeout(&addr, Duration::from_secs(20));
            let _ = tx.send(Done::Connect(i, serial, r));
        });
    }

    fn apply(&mut self, t: u8, slot: u8, gen: u8, b: &[u8]) {
        if self.log && t != C_DATA && t != C_WINDOW {
            eprintln!("tsp: <- type {:02x} slot {} gen {} len {} {:02x?}", t, slot, gen, b.len(), &b[..b.len().min(16)]);
        }
        match t {
            C_CONNECT => {
                if b.len() < 10 { return; }
                let Some(i) = self.attach(slot, gen) else { return };
                if b[0] != 0 || b[1] & CF_TLS != 0 {
                    self.ps[i].as_mut().unwrap().fail(ERR_NOTSUP);
                    return;
                }
                let rport = g16(&b[4..]);
                let addr = g32(&b[6..]);
                if addr == 0 && b[1] & CF_NAME != 0 && b.len() > 10 {
                    let name = String::from_utf8_lossy(&b[10..]).to_string();
                    let serial = self.ps[i].as_ref().unwrap().serial;
                    let tx = self.done_tx.clone();
                    std::thread::spawn(move || {
                        let r = (name.as_str(), rport).to_socket_addrs().ok().and_then(|mut it| it.find(|a| a.is_ipv4()));
                        let a = r.map(|a| match a { SocketAddr::V4(s) => *s.ip(), _ => Ipv4Addr::UNSPECIFIED });
                        let _ = tx.send(Done::Resolve(i, serial, a));
                    });
                    if self.log { eprintln!("tsp: slot {} connect {}:{}", slot, String::from_utf8_lossy(&b[10..]), rport); }
                } else {
                    // X68000 の 127.0.0.0/8 は tis.x の中のループバックなので、Pi 自身には SELF (既定 192.168.68.1、
                    // X68000 の hosts の raspi) でつなぐ。zmuxd はそれを 127.0.0.1 にする
                    let ip = if addr == self.self_addr { Ipv4Addr::LOCALHOST } else { Ipv4Addr::from(addr) };
                    if self.log { eprintln!("tsp: slot {} connect {}:{}", slot, ip, rport); }
                    self.start_connect(i, SocketAddr::V4(SocketAddrV4::new(ip, rport)));
                }
            }
            C_LISTEN => {
                if b.len() < 8 { return; }
                let Some(i) = self.attach(slot, gen) else { return };
                let lport = g16(&b[2..]);
                let laddr = Ipv4Addr::from(g32(&b[4..]));
                let p = self.ps[i].as_mut().unwrap();
                match TcpListener::bind((laddr, lport)) {
                    Ok(l) => {
                        let _ = l.set_nonblocking(true);
                        p.lport = l.local_addr().map(|a| a.port()).unwrap_or(lport);
                        p.lis = Some(l);
                        p.st = St::Listen;
                        p.bound_ev = true;
                        if self.log { eprintln!("tsp: slot {} listen {}:{}", slot, laddr, p.lport); }
                    }
                    Err(e) => {
                        if self.log { eprintln!("tsp: slot {} listen {}:{}: {}", slot, laddr, lport, e); }
                        p.fail(err_code(&e));
                    }
                }
            }
            C_UDPOPEN => {
                if b.len() < 8 { return; }
                self.udp_open(slot, gen, Ipv4Addr::from(g32(&b[4..])), g16(&b[2..]));
            }
            C_SENDTO => {
                if b.len() < 6 { return; }
                let i = match self.by_slot(slot, gen) { Some(i) => Some(i), None => self.udp_open(slot, gen, Ipv4Addr::UNSPECIFIED, 0) };
                let Some(i) = i else { return };
                if let Some(u) = &self.ps[i].as_ref().unwrap().udp {
                    let _ = u.send_to(&b[6..], (Ipv4Addr::from(g32(b)), g16(&b[4..])));
                }
            }
            C_ADOPT => {
                if b.len() < 8 || slot as usize >= NSLOT { return; }
                let h = b[0] as usize;
                let ok = self.ps.get(h).and_then(|p| p.as_ref()).map(|c| c.listener.is_some() && c.zslot.is_none()).unwrap_or(false);
                if !ok { return; }
                let l = self.ps[h].as_ref().unwrap().listener.unwrap();
                self.ps[h].as_mut().unwrap().listener = None;
                if b[1] & AF_LISTENER != 0 {
                    // backlog 0: listen していた番号がそのまま接続になる。listen はやめる (ほかの子も切る)
                    for k in 0..self.ps.len() {
                        if k != h && self.ps[k].as_ref().map(|c| c.listener == Some(l)).unwrap_or(false) {
                            self.ps[k] = None;
                        }
                    }
                    if let Some(lp) = self.ps[l].as_mut() { lp.zslot = None; }
                    self.ps[l] = None;
                } else if let Some(old) = self.z2p[slot as usize] {
                    self.free(old);
                }
                let c = self.ps[h].as_mut().unwrap();
                c.zslot = Some(slot);
                c.zgen = gen;
                c.zwin = g32(&b[4..]);
                self.z2p[slot as usize] = Some(h);
            }
            C_REJECT => {
                if let Some(h) = b.first().map(|&h| h as usize) {
                    if self.ps.get(h).and_then(|p| p.as_ref()).map(|c| c.listener.is_some()).unwrap_or(false) {
                        self.ps[h] = None;
                    }
                }
            }
            C_RESOLVE => {
                let k = slot as usize;
                if k >= NQUERY { return; }
                self.serial += 1;
                self.q[k] = (1, 0, 0, self.serial);
                let name = String::from_utf8_lossy(b).to_string();
                let (tx, serial) = (self.done_tx.clone(), self.serial);
                std::thread::spawn(move || {
                    let r = (name.as_str(), 0).to_socket_addrs().ok().and_then(|mut it| it.find(|a| a.is_ipv4()));
                    let a = r.map(|a| match a { SocketAddr::V4(s) => *s.ip(), _ => Ipv4Addr::UNSPECIFIED });
                    let _ = tx.send(Done::Resolve(usize::MAX - k, serial, a));
                });
            }
            _ => {
                let Some(i) = self.by_slot(slot, gen) else { return };
                let p = self.ps[i].as_mut().unwrap();
                match t {
                    C_DATA => {
                        p.zcred = p.zcred.saturating_sub(b.len() as u32);
                        if p.st == St::Est {
                            p.txq.extend(b.iter().copied());
                        }
                    }
                    C_WINDOW => {
                        if b.len() >= 4 { p.zwin = g32(b); }
                    }
                    C_SHUT => p.shut_pending = true,
                    C_CLOSE => {
                        // 残りを書いてから閉じる。Z にはもう何も知らせない
                        if let Some(z) = p.zslot.take() {
                            if self.z2p[z as usize] == Some(i) { self.z2p[z as usize] = None; }
                        }
                        p.closing = true;
                        if p.st == St::Listen || p.st == St::Udp || p.txq.is_empty() {
                            self.free(i);
                        }
                    }
                    _ => {}
                }
            }
        }
    }

    fn udp_open(&mut self, slot: u8, gen: u8, laddr: Ipv4Addr, lport: u16) -> Option<usize> {
        let i = self.attach(slot, gen)?;
        let p = self.ps[i].as_mut().unwrap();
        match UdpSocket::bind((laddr, lport)) {
            Ok(u) => {
                let _ = u.set_nonblocking(true);
                p.lport = u.local_addr().map(|a| a.port()).unwrap_or(0);
                p.udp = Some(u);
                p.st = St::Udp;
                p.bound_ev = true;
            }
            Err(e) => p.fail(err_code(&e)),
        }
        Some(i)
    }

    /// ネットの側を進める (交換の外、数 ms ごと)。Z に知らせることがあれば true
    pub fn poll(&mut self) -> bool {
        while let Ok(d) = self.done_rx.try_recv() {
            match d {
                Done::Connect(i, serial, r) => {
                    let Some(Some(p)) = self.ps.get_mut(i) else { continue };
                    if p.serial != serial { continue; }
                    match r {
                        Ok(s) => {
                            let _ = s.set_nonblocking(true);
                            let _ = s.set_nodelay(true);
                            p.tcp = Some(s);
                            p.st = St::Est;
                            p.connected_ev = true;
                        }
                        Err(e) => p.fail(err_code(&e)),
                    }
                }
                Done::Resolve(i, serial, a) if i >= usize::MAX - NQUERY => {
                    let k = usize::MAX - i;
                    if self.q[k].3 == serial {
                        self.q[k] = match a { Some(ip) => (2, 0, u32::from(ip), serial), None => (2, ERR_NONAME, 0, serial) };
                    }
                }
                Done::Resolve(i, serial, a) => {
                    let Some(Some(p)) = self.ps.get(i) else { continue };
                    if p.serial != serial { continue; }
                    match a {
                        Some(ip) => self.pending_connect(i, ip),
                        None => self.ps[i].as_mut().unwrap().fail(ERR_NONAME),
                    }
                }
            }
        }
        let mut news = false;
        for i in 0..self.ps.len() {
            let mut child: Option<TcpStream> = None;
            {
                let Some(p) = self.ps[i].as_mut() else { continue };
                if let Some(l) = &p.lis {
                    if let Ok((s, _)) = l.accept() {
                        child = Some(s);
                    }
                }
                if let Some(u) = &p.udp {
                    let mut b = [0u8; 2048];
                    while p.uq.len() < UDPQ {
                        match u.recv_from(&mut b) {
                            Ok((n, SocketAddr::V4(a))) => { p.uq.push_back((*a.ip(), a.port(), b[..n].to_vec())); news = true; }
                            Ok(_) => {}
                            Err(_) => break,
                        }
                    }
                }
                if let Some(s) = p.tcp.as_mut() {
                    // 書く
                    while !p.txq.is_empty() {
                        let (a, _) = p.txq.as_slices();
                        match s.write(a) {
                            Ok(0) => break,
                            Ok(n) => { p.txq.drain(..n); news = true; }
                            Err(e) if e.kind() == ErrorKind::WouldBlock => break,
                            Err(e) => { let c = err_code(&e); p.fail(c); news = true; break; }
                        }
                    }
                }
                if let Some(s) = p.tcp.as_mut() {
                    if p.shut_pending && p.txq.is_empty() {
                        let _ = s.shutdown(Shutdown::Write);
                        p.shut_pending = false;
                    }
                    // 読む (Z に渡していない分が多ければ待つ)
                    let mut b = [0u8; 4096];
                    while !p.eof && p.rxq.len() < RXCAP {
                        match s.read(&mut b) {
                            Ok(0) => { p.eof = true; news = true; }
                            Ok(n) => { p.rxq.extend(&b[..n]); news = true; }
                            Err(e) if e.kind() == ErrorKind::WouldBlock => break,
                            Err(e) => { p.eof = true; p.err = Some(err_code(&e)); news = true; break; }
                        }
                    }
                }
                if p.closing && p.txq.is_empty() {
                    if let Some(s) = &p.tcp { let _ = s.shutdown(Shutdown::Both); }
                }
            }
            if self.ps[i].as_ref().map(|p| p.closing && (p.txq.is_empty() || p.tcp.is_none())).unwrap_or(false) {
                self.free(i);
                continue;
            }
            if let Some(s) = child {
                let _ = s.set_nonblocking(true);
                let _ = s.set_nodelay(true);
                let c = self.alloc();
                let cp = self.ps[c].as_mut().unwrap();
                cp.tcp = Some(s);
                cp.st = St::Est;
                cp.listener = Some(i);
                cp.announce = true;
                news = true;
                if self.log { eprintln!("tsp: accept on listen {} -> handle {}", i, c); }
            }
        }
        news || self.has_news()
    }

    fn pending_connect(&mut self, i: usize, ip: Ipv4Addr) {
        // 名前で CONNECT したときのポートは、名前を引くスレッドに渡した (ToSocketAddrs の 2 番目)。ここでは控えが無いので
        // PSock.lport に一時的に入れておいたものを使う
        let port = self.ps[i].as_ref().unwrap().lport;
        self.start_connect(i, SocketAddr::V4(SocketAddrV4::new(ip, port)));
    }

    fn has_news(&self) -> bool {
        self.q.iter().any(|q| q.0 == 2)
            || self.ps.iter().flatten().any(|p| {
                p.announce || (p.zslot.is_some() && (p.connected_ev || p.bound_ev || p.err.is_some() && p.rxq.is_empty()
                    || (p.eof && !p.eof_sent && p.rxq.is_empty()) || (p.zwin > 0 && (!p.rxq.is_empty() || !p.uq.is_empty()))
                    || (p.st == St::Est && credit_of(p) != p.zcred)))
            })
    }

    /// Z の TSP 要求 1 個 (MARK から) に答える。戻り値: 応答 (MARK から)
    pub fn exchange(&mut self, req: &[u8]) -> Vec<u8> {
        if req.len() < HDR || req[0] != MARK {
            return Vec::new();
        }
        let zseq = g16(&req[4..]);
        let zepoch = g32(&req[8..]);
        if self.zepoch != Some(zepoch) || req[2] & ZF_RESET != 0 {
            if self.log && self.zepoch.is_some() { eprintln!("tsp: new zepoch, dropping all sockets"); }
            self.reset();
            self.zepoch = Some(zepoch);
            self.last_zseq = None;
        }
        if self.last_zseq == Some(zseq) && !self.last.is_empty() {
            if self.log { eprintln!("tsp: dup zseq {} -> last response ({} bytes)", zseq, self.last.len()); }
            return self.last.clone(); // 送り直し: 適用せずに同じ応答を
        }
        // 名前で CONNECT するときのポートを控える (pending_connect が使う)
        let mut p = HDR;
        while p + MHDR <= req.len() {
            let (t, slot, gen) = (req[p], req[p + 1], req[p + 2]);
            let n = g16(&req[p + 4..]) as usize;
            if p + MHDR + n > req.len() { break; }
            let body = req[p + MHDR..p + MHDR + n].to_vec();
            self.apply(t, slot, gen, &body);
            if t == C_CONNECT && body.len() >= 10 && g32(&body[6..]) == 0 {
                if let Some(i) = self.by_slot(slot, gen) {
                    self.ps[i].as_mut().unwrap().lport = g16(&body[4..]);
                }
            }
            p += MHDR + ((n + 1) & !1);
        }
        self.poll();
        let res = self.build(zseq);
        self.last_zseq = Some(zseq);
        self.last = res.clone();
        res
    }

    fn build(&mut self, zseq: u16) -> Vec<u8> {
        let mut r = vec![MARK, 1, 0, 0, (zseq >> 8) as u8, zseq as u8, 0, 0];
        r.extend_from_slice(&self.pepoch.to_be_bytes());
        let mut more = false;
        fn put(r: &mut Vec<u8>, t: u8, slot: u8, gen: u8, body: &[u8]) -> bool {
            let sz = MHDR + ((body.len() + 1) & !1);
            if r.len() + sz > RES_MAX { return false; }
            r.extend_from_slice(&[t, slot, gen, 0, (body.len() >> 8) as u8, body.len() as u8]);
            r.extend_from_slice(body);
            if body.len() & 1 == 1 { r.push(0); }
            true
        }
        // 1. 状態の知らせ
        for i in 0..self.ps.len() {
            let Some(p) = self.ps[i].as_ref() else { continue };
            if p.announce {
                let l = p.listener.unwrap();
                if let Some((lz, lg)) = self.ps.get(l).and_then(|x| x.as_ref()).and_then(|lp| lp.zslot.map(|z| (z, lp.zgen))) {
                    let s = p.tcp.as_ref().unwrap();
                    let (la, lp) = s.local_addr().map(|a| v4(&a)).unwrap_or((0, 0));
                    let (ra, rp) = s.peer_addr().map(|a| v4(&a)).unwrap_or((0, 0));
                    let mut b = vec![i as u8, 0];
                    b.extend_from_slice(&la.to_be_bytes());
                    b.extend_from_slice(&lp.to_be_bytes());
                    b.extend_from_slice(&ra.to_be_bytes());
                    b.extend_from_slice(&rp.to_be_bytes());
                    if put(&mut r, E_ACCEPT, lz, lg, &b) {
                        self.ps[i].as_mut().unwrap().announce = false;
                    } else {
                        more = true;
                    }
                }
            }
            let Some(p) = self.ps[i].as_mut() else { continue };
            let Some(z) = p.zslot else { continue };
            if p.connected_ev {
                if let Some(s) = &p.tcp {
                    let (la, lp) = s.local_addr().map(|a| v4(&a)).unwrap_or((0, 0));
                    let (ra, rp) = s.peer_addr().map(|a| v4(&a)).unwrap_or((0, 0));
                    let mut b = Vec::new();
                    b.extend_from_slice(&la.to_be_bytes());
                    b.extend_from_slice(&lp.to_be_bytes());
                    b.extend_from_slice(&ra.to_be_bytes());
                    b.extend_from_slice(&rp.to_be_bytes());
                    if put(&mut r, E_CONNECTED, z, p.zgen, &b) { p.connected_ev = false; } else { more = true; }
                }
            }
            if p.bound_ev {
                if put(&mut r, E_BOUND, z, p.zgen, &p.lport.to_be_bytes()) { p.bound_ev = false; } else { more = true; }
            }
        }
        for k in 0..NQUERY {
            if self.q[k].0 == 2 {
                let mut b = vec![self.q[k].1, 0];
                b.extend_from_slice(&self.q[k].2.to_be_bytes());
                if put(&mut r, E_RESOLVED, k as u8, 0, &b) { self.q[k].0 = 0; } else { more = true; }
            }
        }
        // 2. CREDIT
        for p in self.ps.iter_mut().flatten() {
            let Some(z) = p.zslot else { continue };
            if p.st != St::Est { continue; }
            let c = credit_of(p);
            if c != p.zcred {
                if put(&mut r, E_CREDIT, z, p.zgen, &c.to_be_bytes()) { p.zcred = c; } else { more = true; }
            }
        }
        // 3. データ (Z の窓の分)。ソケットで等分してから余りを詰める
        let ready = self.ps.iter().flatten().filter(|p| p.zslot.is_some() && p.zwin > 0 && (!p.rxq.is_empty() || !p.uq.is_empty())).count();
        if ready > 0 {
            let share = ((RES_MAX - r.len()) / ready).max(512);
            for pass in 0..2 {
                for p in self.ps.iter_mut().flatten() {
                    let Some(z) = p.zslot else { continue };
                    if p.zwin == 0 { continue; }
                    while let Some((a, port, d)) = p.uq.front() {
                        if d.len() as u32 + 8 > p.zwin { break; }
                        let mut b = u32::from(*a).to_be_bytes().to_vec();
                        b.extend_from_slice(&port.to_be_bytes());
                        b.extend_from_slice(d);
                        if !put(&mut r, E_DGRAM, z, p.zgen, &b) { more = true; break; }
                        p.zwin -= d.len() as u32 + 8;
                        p.uq.pop_front();
                    }
                    if p.rxq.is_empty() || r.len() + MHDR + 1 >= RES_MAX { continue; }
                    let mut n = p.rxq.len().min(p.zwin as usize).min(RES_MAX - r.len() - MHDR);
                    if pass == 0 { n = n.min(share); }
                    if n > 1 { n &= !1; }
                    if n == 0 { continue; }
                    let b: Vec<u8> = p.rxq.drain(..n).collect();
                    put(&mut r, E_DATA, z, p.zgen, &b);
                    p.zwin -= n as u32;
                }
            }
        }
        // 4. 終わりの知らせ (データを渡し切ってから)
        let mut gone = Vec::new();
        for (i, slot) in self.ps.iter_mut().enumerate() {
            let Some(p) = slot.as_mut() else { continue };
            let Some(z) = p.zslot else { continue };
            if !p.rxq.is_empty() { if p.zwin > 0 { more = true; } continue; }
            if p.eof && !p.eof_sent && p.err.is_none() {
                if put(&mut r, E_EOF, z, p.zgen, &[]) { p.eof_sent = true; } else { more = true; }
            }
            if let Some(code) = p.err {
                if put(&mut r, E_ERROR, z, p.zgen, &[code]) { gone.push(i); } else { more = true; }
            }
        }
        for i in gone { self.free(i); }
        if more { r[2] |= PF_MORE; }
        r
    }
}

fn credit_of(p: &PSock) -> u32 {
    (TXCAP.saturating_sub(p.txq.len())) as u32
}
