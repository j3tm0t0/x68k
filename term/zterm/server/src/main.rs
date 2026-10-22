// zterm -- X68000 Z 用リモート端末ヘルパ (リモート側)。
// シェルを pty で動かし、端末の画面を自分で持ち (vt100 crate)、Z が描ける形のセルの差分だけを
// 標準出力に書く。標準入力から Z のキー・ACK・寸法を読む。プロトコルは proto.rs と README.md。
//
//   zterm [--cols C] [--rows R] [--interval MS] [--inflight N] [--term T] [-- cmd args...]
//   zterm --replay FILE [--chunk N] [--cols C --rows R] [--out FILE] [--dump FILE]
//     記録した端末出力を食わせてフレーム列を書く (試験用。--dump は最終画面のセルの文字表)
mod dec;
mod proto;
mod zcell;

use proto::*;
use std::io::Write;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::time::{Duration, Instant};
use zcell::ZCell;

struct Opts {
    cols: usize,
    rows: usize,
    interval: u64,
    inflight: u32,
    term: String,
    cmd: Vec<String>,
    replay: Option<String>,
    chunk: usize,
    out: Option<String>,
    dump: Option<String>,
    noscroll: bool,
    stats: bool,
    refresh: u64,
}

fn usage() -> ! {
    eprintln!(
        "zterm {} (protocol {})\n\
         usage: zterm [--cols C] [--rows R] [--interval MS] [--inflight N] [--term T] [--refresh S] [-- cmd args...]\n\
         \x20      zterm --replay FILE [--chunk N] [--out FILE] [--dump FILE] [--stats]",
        env!("CARGO_PKG_VERSION"),
        VERSION
    );
    std::process::exit(2)
}

fn parse_args() -> Opts {
    let mut o = Opts {
        cols: 96,
        rows: 32,
        interval: 50,
        inflight: 1,
        term: "xterm".into(),
        cmd: vec![],
        replay: None,
        chunk: 1400,
        out: None,
        dump: None,
        noscroll: false,
        stats: false,
        refresh: 0,
    };
    let mut a = std::env::args().skip(1);
    while let Some(s) = a.next() {
        let num = |a: &mut dyn Iterator<Item = String>| -> u64 {
            a.next().and_then(|v| v.parse().ok()).unwrap_or_else(|| usage())
        };
        match s.as_str() {
            "--cols" => o.cols = num(&mut a) as usize,
            "--rows" => o.rows = num(&mut a) as usize,
            "--interval" => o.interval = num(&mut a),
            "--inflight" => o.inflight = num(&mut a) as u32,
            "--chunk" => o.chunk = num(&mut a) as usize,
            "--refresh" => o.refresh = num(&mut a),
            "--term" => o.term = a.next().unwrap_or_else(|| usage()),
            "--replay" => o.replay = a.next(),
            "--out" => o.out = a.next(),
            "--dump" => o.dump = a.next(),
            "--no-scroll" => o.noscroll = true,
            "--stats" => o.stats = true,
            "--version" => {
                println!("zterm {} protocol {}", env!("CARGO_PKG_VERSION"), VERSION);
                std::process::exit(0)
            }
            "--" => {
                o.cmd = a.collect();
                break;
            }
            _ => usage(),
        }
    }
    // 起動スクリプト経由でも試験用の設定を渡せるように (ZTERM_STATS=1 / ZTERM_INTERVAL=ms / ZTERM_NOSCROLL=1)
    if std::env::var_os("ZTERM_STATS").is_some() {
        o.stats = true;
    }
    if let Some(v) = std::env::var("ZTERM_INTERVAL").ok().and_then(|v| v.parse().ok()) {
        o.interval = v;
    }
    if std::env::var_os("ZTERM_NOSCROLL").is_some() {
        o.noscroll = true;
    }
    o.cols = o.cols.clamp(2, 255);
    o.rows = o.rows.clamp(2, 255);
    o
}

// ---- 端末への問い合わせに答える (Z の vt100.c と同じ答え。Z 側の vt_write は使わないのでここで) ----
#[derive(Default)]
struct Cb {
    reply: Vec<u8>,
    bell: bool,
    sync: bool,        // ?2026 (同期更新) の最中
    sync_since: Option<Instant>,
}

impl vt100::Callbacks for Cb {
    fn audible_bell(&mut self, _: &mut vt100::Screen) {
        self.bell = true;
    }
    fn unhandled_csi(&mut self, s: &mut vt100::Screen, i1: Option<u8>, i2: Option<u8>, params: &[&[u16]], c: char) {
        let p0 = params.first().and_then(|p| p.first().copied()).unwrap_or(0);
        match (i1, i2, c) {
            (None, None, 'n') if p0 == 5 => self.reply.extend_from_slice(b"\x1b[0n"),
            (None, None, 'n') if p0 == 6 => {
                let (r, c) = s.cursor_position();
                self.reply.extend_from_slice(format!("\x1b[{};{}R", r + 1, c + 1).as_bytes());
            }
            (None, None, 'c') if p0 == 0 => self.reply.extend_from_slice(b"\x1b[?1;2c"),
            (Some(b'>'), None, 'c') => self.reply.extend_from_slice(b"\x1b[>0;95;0c"),
            (None, None, 't') if p0 == 18 => {
                let (r, c) = s.size();
                self.reply.extend_from_slice(format!("\x1b[8;{};{}t", r, c).as_bytes());
            }
            (Some(b'?'), None, 'h') | (Some(b'?'), None, 'l') => {
                if params.iter().any(|p| p.first() == Some(&2026)) {
                    self.sync = c == 'h';
                    self.sync_since = if self.sync { Some(Instant::now()) } else { None };
                }
            }
            (Some(b'?'), Some(b'$'), 'p') => {
                let st = match p0 {
                    1 => if s.application_cursor() { 1 } else { 2 },
                    25 => if s.hide_cursor() { 2 } else { 1 },
                    47 | 1047 | 1049 => if s.alternate_screen() { 1 } else { 2 },
                    2004 => if s.bracketed_paste() { 1 } else { 2 },
                    2026 => if self.sync { 1 } else { 2 },
                    _ => 0,
                };
                self.reply.extend_from_slice(format!("\x1b[?{};{}$y", p0, st).as_bytes());
            }
            (None, Some(b'$'), 'p') => self.reply.extend_from_slice(format!("\x1b[{};0$y", p0).as_bytes()),
            _ => {}
        }
    }
    fn unhandled_osc(&mut self, _: &mut vt100::Screen, params: &[&[u8]]) {
        if params.len() >= 2 && params[1] == b"?" {
            match params[0] {
                b"10" => self.reply.extend_from_slice(b"\x1b]10;rgb:aaaa/aaaa/aaaa\x1b\\"),
                b"11" => self.reply.extend_from_slice(b"\x1b]11;rgb:0000/0000/0000\x1b\\"),
                _ => {}
            }
        }
    }
}

struct Term {
    parser: vt100::Parser<Cb>,
    dec: dec::DecFilter,
    buf: Vec<u8>,
    grid: Vec<ZCell>,
    cols: usize,
    rows: usize,
}

impl Term {
    fn new(cols: usize, rows: usize) -> Self {
        let nz = |v: usize| std::num::NonZeroU16::new(v as u16).unwrap();
        Term {
            parser: vt100::Parser::new_with_callbacks(nz(rows), nz(cols), 0, Cb::default()),
            dec: dec::DecFilter::new(),
            buf: Vec::new(),
            grid: Vec::new(),
            cols,
            rows,
        }
    }
    fn feed(&mut self, b: &[u8]) {
        self.buf.clear();
        self.dec.feed(b, &mut self.buf);
        self.parser.process(&self.buf);
    }
    fn resize(&mut self, cols: usize, rows: usize) {
        let nz = |v: usize| std::num::NonZeroU16::new(v as u16).unwrap();
        self.parser.set_size(nz(rows), nz(cols));
        self.cols = cols;
        self.rows = rows;
    }
    fn snapshot(&mut self) -> Meta {
        let s = self.parser.screen();
        zcell::snapshot(s, self.rows, self.cols, &mut self.grid);
        let (r, c) = s.cursor_position();
        let mode = (s.application_cursor() as u8)
            | ((s.alternate_screen() as u8) << 1)
            | ((s.application_keypad() as u8) << 2)
            | ((s.bracketed_paste() as u8) << 3);
        Meta {
            cx: c.min(self.cols as u16 - 1) as u8,
            cy: r.min(self.rows as u16 - 1) as u8,
            cvis: !s.hide_cursor(),
            mode,
        }
    }
}

fn hello(cols: usize, rows: usize, out: &mut Vec<u8>) {
    frame_raw(&[OP_HELLO, VERSION, cols as u8, rows as u8], out);
}

// 最終画面を 1 行 1 行の文字 (vtdump と同じ形: 全角の右は出さない、絵文字は元のコードポイント、DEC 罫線は Unicode) で書く
fn dump_text(g: &[ZCell], cols: usize, rows: usize) -> String {
    let special = |b: u16| -> u32 {
        match b {
            0x71 => 0x2500, 0x78 => 0x2502, 0x6c => 0x250c, 0x6b => 0x2510, 0x6d => 0x2514, 0x6a => 0x2518,
            0x74 => 0x251c, 0x75 => 0x2524, 0x77 => 0x252c, 0x76 => 0x2534, 0x6e => 0x253c, 0x61 => 0x2592,
            0x60 => 0x25c6, 0x6f => 0x23ba, 0x73 => 0x23bd, x => x as u32,
        }
    };
    let mut s = String::new();
    for r in 0..rows {
        for c in 0..cols {
            let z = g[r * cols + c];
            if z.fl & zcell::F_WIDECONT != 0 {
                continue;
            }
            let cp = if z.fl & zcell::F_EMOJI != 0 {
                zcell::EMOJI[z.cp as usize]
            } else if z.fl & zcell::F_SPECIAL != 0 {
                special(z.cp)
            } else {
                z.cp as u32
            };
            s.push(char::from_u32(if cp < 0x20 { 0x20 } else { cp }).unwrap_or('?'));
        }
        s.push('\n');
    }
    s
}

fn dump_attr(g: &[ZCell], cols: usize, rows: usize) -> String {
    let mut s = String::new();
    for r in 0..rows {
        for c in 0..cols {
            let z = g[r * cols + c];
            s += &format!("{:x},{:x},{:x} ", z.col >> 4, z.col & 15, z.fl);
        }
        s.push('\n');
    }
    s
}

fn replay(o: &Opts) {
    let path = o.replay.as_ref().unwrap();
    let data = std::fs::read(path).unwrap_or_else(|e| {
        eprintln!("zterm: {path}: {e}");
        std::process::exit(1)
    });
    let mut t = Term::new(o.cols, o.rows);
    let mut enc = Encoder::new(o.cols, o.rows);
    enc.scroll_detect = !o.noscroll;
    let mut out = Vec::new();
    hello(o.cols, o.rows, &mut out);
    let mut frames = 0;
    for ch in data.chunks(o.chunk.max(1)) {
        t.feed(ch);
        t.parser.callbacks_mut().reply.clear();
        if t.parser.callbacks().sync {
            continue;
        }
        let bell = std::mem::take(&mut t.parser.callbacks_mut().bell);
        let m = t.snapshot();
        if enc.frame(&t.grid, m, bell, &mut out) {
            frames += 1;
        }
    }
    let m = t.snapshot();
    if enc.frame(&t.grid, m, false, &mut out) {
        frames += 1;
    }
    frame_raw(&[OP_BYE, 0], &mut out);
    if let Some(f) = &o.out {
        std::fs::write(f, &out).unwrap();
    }
    if let Some(f) = &o.dump {
        std::fs::write(f, dump_text(&t.grid, o.cols, o.rows)).unwrap();
        std::fs::write(format!("{f}.attr"), dump_attr(&t.grid, o.cols, o.rows)).unwrap();
    }
    if o.stats || o.out.is_none() {
        eprintln!(
            "{}: in {} bytes, out {} bytes ({:.1}%), frames {}, scrolls {}, clears {}",
            path,
            data.len(),
            out.len(),
            100.0 * out.len() as f64 / data.len().max(1) as f64,
            frames,
            enc.stats_scrolls,
            enc.stats_clears
        );
    }
}

// ---- pty ----
fn spawn(o: &Opts) -> (OwnedFd, libc::pid_t) {
    unsafe {
        let mut m: RawFd = -1;
        let mut s: RawFd = -1;
        let ws = libc::winsize { ws_row: o.rows as u16, ws_col: o.cols as u16, ws_xpixel: 0, ws_ypixel: 0 };
        if libc::openpty(&mut m, &mut s, std::ptr::null_mut(), std::ptr::null_mut(), &ws as *const _ as *mut _) != 0 {
            eprintln!("zterm: openpty: {}", std::io::Error::last_os_error());
            std::process::exit(1);
        }
        // exec の前に用意しておく (fork 後は確保しない)
        let shell = std::env::var("SHELL").unwrap_or_else(|_| "/bin/sh".into());
        let (prog, argv0, args): (String, String, Vec<String>) = if o.cmd.is_empty() {
            let base = shell.rsplit('/').next().unwrap_or("sh").to_string();
            (shell.clone(), format!("-{base}"), vec![])
        } else {
            (o.cmd[0].clone(), o.cmd[0].clone(), o.cmd[1..].to_vec())
        };
        let cs = |s: &str| std::ffi::CString::new(s).unwrap();
        let cprog = cs(&prog);
        let mut cargs: Vec<std::ffi::CString> = vec![cs(&argv0)];
        cargs.extend(args.iter().map(|a| cs(a)));
        let mut argv: Vec<*const libc::c_char> = cargs.iter().map(|c| c.as_ptr()).collect();
        argv.push(std::ptr::null());
        std::env::set_var("TERM", &o.term);
        std::env::remove_var("COLORTERM");
        if std::env::var_os("LANG").is_none() && std::env::var_os("LC_ALL").is_none() && std::env::var_os("LC_CTYPE").is_none() {
            std::env::set_var("LANG", "C.UTF-8");
        }
        std::env::set_var("ZTERM", env!("CARGO_PKG_VERSION"));
        let pid = libc::fork();
        if pid < 0 {
            eprintln!("zterm: fork: {}", std::io::Error::last_os_error());
            std::process::exit(1);
        }
        if pid == 0 {
            libc::setsid();
            libc::ioctl(s, libc::TIOCSCTTY as _, 0);
            libc::dup2(s, 0);
            libc::dup2(s, 1);
            libc::dup2(s, 2);
            if s > 2 {
                libc::close(s);
            }
            libc::close(m);
            libc::execvp(cprog.as_ptr(), argv.as_ptr());
            let msg = b"zterm: exec failed\r\n";
            libc::write(2, msg.as_ptr() as *const _, msg.len());
            libc::_exit(127);
        }
        libc::close(s);
        (OwnedFd::from_raw_fd(m), pid)
    }
}

fn write_all(fd: RawFd, mut b: &[u8]) -> bool {
    while !b.is_empty() {
        let n = unsafe { libc::write(fd, b.as_ptr() as *const _, b.len()) };
        if n < 0 {
            let e = std::io::Error::last_os_error();
            if e.kind() == std::io::ErrorKind::Interrupted {
                continue;
            }
            if e.kind() == std::io::ErrorKind::WouldBlock {
                let mut p = libc::pollfd { fd, events: libc::POLLOUT, revents: 0 };
                unsafe { libc::poll(&mut p, 1, 1000) };
                continue;
            }
            return false;
        }
        b = &b[n as usize..];
    }
    true
}

fn set_winsize(fd: RawFd, cols: usize, rows: usize) {
    let ws = libc::winsize { ws_row: rows as u16, ws_col: cols as u16, ws_xpixel: 0, ws_ypixel: 0 };
    unsafe { libc::ioctl(fd, libc::TIOCSWINSZ as _, &ws) };
}

// DECCKM の間は矢印キー ESC [ A-D を ESC O A-D に (Z の vt_key は DECCKM を知らずに送ってくる)
fn keys_to_pty(k: &[u8], appcur: bool, out: &mut Vec<u8>) {
    let mut i = 0;
    while i < k.len() {
        if appcur && i + 2 < k.len() && k[i] == 0x1b && k[i + 1] == b'[' && (b'A'..=b'D').contains(&k[i + 2]) {
            out.extend_from_slice(&[0x1b, b'O', k[i + 2]]);
            i += 3;
        } else {
            out.push(k[i]);
            i += 1;
        }
    }
}

fn serve(o: &Opts) -> i32 {
    let (master, pid) = spawn(o);
    let mfd = master.as_raw_fd();
    let stdin_fd = 0;
    let stdout_fd = 1;
    let mut t = Term::new(o.cols, o.rows);
    let mut enc = Encoder::new(o.cols, o.rows);
    enc.scroll_detect = !o.noscroll;
    let mut out = Vec::new();
    hello(o.cols, o.rows, &mut out);
    write_all(stdout_fd, &out);
    out.clear();

    let interval = Duration::from_millis(o.interval);
    let mut last_send = Instant::now() - interval;
    let mut last_full = Instant::now();
    let mut inflight: u32 = 0;
    let mut dirty = true;
    // 書き途中の画面を送らない: pty が QUIET 静かになるか、最初の変化から MAXWAIT 経つまで待つ (mosh と同じ考え)
    const QUIET: Duration = Duration::from_millis(8);
    const MAXWAIT: Duration = Duration::from_millis(100);
    let mut last_read = Instant::now();
    let mut first_dirty = Instant::now();
    let ready_at = |last_send: Instant, last_read: Instant, first_dirty: Instant| -> Duration {
        let a = interval.saturating_sub(last_send.elapsed());
        let b = QUIET.saturating_sub(last_read.elapsed()).min(MAXWAIT.saturating_sub(first_dirty.elapsed()));
        a.max(b)
    };
    let mut child_done = false;
    let mut inbuf: Vec<u8> = Vec::new();
    let mut rbuf = vec![0u8; 65536];
    let mut ptyout: Vec<u8> = Vec::new();
    let (mut st_in, mut st_out) = (0u64, 0u64);
    let t0 = Instant::now();

    loop {
        // いつ起きるか: 送りたい変化があって、ACK 待ちでなければ次の送信時刻まで
        let mut timeout: i32 = -1;
        let sync_hold = t.parser.callbacks().sync
            && t.parser.callbacks().sync_since.map_or(false, |s| s.elapsed() < Duration::from_millis(500));
        if dirty && inflight < o.inflight && !sync_hold {
            let wait = ready_at(last_send, last_read, first_dirty);
            timeout = (wait.as_micros() as i32 + 999) / 1000;
        } else if sync_hold {
            timeout = 20;
        }
        if o.refresh > 0 {
            let w = Duration::from_secs(o.refresh).saturating_sub(last_full.elapsed()).as_millis() as i32;
            timeout = if timeout < 0 { w } else { timeout.min(w) };
        }
        let mut fds = [
            libc::pollfd { fd: stdin_fd, events: libc::POLLIN, revents: 0 },
            libc::pollfd { fd: if child_done { -1 } else { mfd }, events: libc::POLLIN, revents: 0 },
        ];
        if child_done && !dirty {
            break;
        }
        let n = unsafe { libc::poll(fds.as_mut_ptr(), 2, timeout) };
        if n < 0 {
            let e = std::io::Error::last_os_error();
            if e.kind() == std::io::ErrorKind::Interrupted {
                continue;
            }
            break;
        }
        // pty -> 端末
        if fds[1].revents & (libc::POLLIN | libc::POLLHUP | libc::POLLERR) != 0 {
            let r = unsafe { libc::read(mfd, rbuf.as_mut_ptr() as *mut _, rbuf.len()) };
            if r > 0 {
                st_in += r as u64;
                if !dirty {
                    first_dirty = Instant::now();
                }
                last_read = Instant::now();
                t.feed(&rbuf[..r as usize]);
                let rep = std::mem::take(&mut t.parser.callbacks_mut().reply);
                if !rep.is_empty() {
                    write_all(mfd, &rep);
                }
                dirty = true;
            } else {
                child_done = true;
                dirty = true;
            }
        }
        // Z -> サーバ
        if fds[0].revents & (libc::POLLIN | libc::POLLHUP | libc::POLLERR) != 0 {
            let r = unsafe { libc::read(stdin_fd, rbuf.as_mut_ptr() as *mut _, rbuf.len()) };
            if r <= 0 {
                // Z が居なくなった: シェルに SIGHUP
                unsafe { libc::kill(pid, libc::SIGHUP) };
                break;
            }
            inbuf.extend_from_slice(&rbuf[..r as usize]);
            let mut i = 0;
            while i + 2 <= inbuf.len() {
                let (ty, len) = (inbuf[i], inbuf[i + 1] as usize);
                if i + 2 + len > inbuf.len() {
                    break;
                }
                let body = &inbuf[i + 2..i + 2 + len];
                match ty {
                    C_KEYS => {
                        ptyout.clear();
                        keys_to_pty(body, t.parser.screen().application_cursor(), &mut ptyout);
                        write_all(mfd, &ptyout);
                    }
                    C_ACK => inflight = inflight.saturating_sub(1),
                    C_RESIZE if len >= 2 => {
                        let (c, r) = ((body[0] as usize).max(2), (body[1] as usize).max(2));
                        t.resize(c, r);
                        enc.resize(c, r);
                        set_winsize(mfd, c, r);
                        let mut b = Vec::new();
                        frame_raw(&[OP_SIZE, c as u8, r as u8], &mut b);
                        write_all(stdout_fd, &b);
                        st_out += b.len() as u64;
                        dirty = true;
                    }
                    C_REFRESH => {
                        enc.invalidate();
                        dirty = true;
                    }
                    C_QUIT => {
                        unsafe { libc::kill(pid, libc::SIGHUP) };
                    }
                    _ => {}
                }
                i += 2 + len;
            }
            inbuf.drain(..i);
        }
        if o.refresh > 0 && last_full.elapsed() >= Duration::from_secs(o.refresh) {
            enc.invalidate();
            dirty = true;
            last_full = Instant::now();
        }
        let sync_hold = t.parser.callbacks().sync
            && t.parser.callbacks().sync_since.map_or(false, |s| s.elapsed() < Duration::from_millis(500));
        if dirty && inflight < o.inflight && !sync_hold
            && (child_done || ready_at(last_send, last_read, first_dirty).is_zero())
        {
            let bell = std::mem::take(&mut t.parser.callbacks_mut().bell);
            let m = t.snapshot();
            out.clear();
            if enc.frame(&t.grid, m, bell, &mut out) {
                if !write_all(stdout_fd, &out) {
                    break;
                }
                st_out += out.len() as u64;
                inflight += 1;
                last_send = Instant::now();
            }
            dirty = false;
        }
    }
    let mut status: libc::c_int = 0;
    unsafe { libc::waitpid(pid, &mut status, 0) };
    let code = if libc::WIFEXITED(status) { libc::WEXITSTATUS(status) } else { 128 + libc::WTERMSIG(status) };
    out.clear();
    frame_raw(&[OP_BYE, code as u8], &mut out);
    write_all(stdout_fd, &out);
    if o.stats {
        let s = t0.elapsed().as_secs_f64();
        eprintln!(
            "zterm: {:.1}s, pty {} bytes ({:.0} B/s), sent {} bytes ({:.0} B/s), scrolls {}, clears {}",
            s, st_in, st_in as f64 / s, st_out, st_out as f64 / s, enc.stats_scrolls, enc.stats_clears
        );
    }
    let _ = std::io::stdout().flush();
    code
}

fn main() {
    let o = parse_args();
    if o.replay.is_some() {
        replay(&o);
        return;
    }
    std::process::exit(serve(&o));
}
