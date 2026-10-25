//! zmux -- X68000 の RS-232C 1 本を多重化する Pi 側 (net/zmux/DESIGN.md)
//!
//!   zmux daemon --port /dev/ttyUSB0 [--baud 19200]   線を持つ (zmuxd)。`zmuxd` という名前で起動しても同じ
//!   zmux daemon --tcp 127.0.0.1:54368                 XEiJ の RS-232C (TCP/IP ⇔ AUX) につなぐ
//!   zmux attach 0                                     チャネル 0 (COMMAND.X のコンソール、タブ 0) の端末
//!   zmux attach                                       新しい zosh のセッション (X68000 で zos.x が動いているとき)
//!   zmux attach N                                     セッション N (1〜4) につなぎ直す
//!   zmux kill N                                       セッション N を閉じる
//!   zmux status                                       線の状態
//!
//! attach にはホットキーも前置キーも無い。端末を raw にして、届いたバイトをそのまま送る。
//! 切り離しやタブは herdr / tmux の役目。デーモンとはソケット ($ZMUX_SOCK、無ければ $XDG_RUNTIME_DIR/zmux.sock、
//! それも無ければ /tmp/zmux-<uid>.sock) でつながる。

mod link;
mod netpipe;
mod tsp;
mod zss;

use std::collections::{HashMap, VecDeque};
use std::io::{Read, Write};
use std::net::TcpStream;
use std::os::unix::io::{FromRawFd, RawFd};
use std::os::unix::net::{UnixListener, UnixStream};
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, Instant};

const SCROLLBACK: usize = 65536;

fn sock_path() -> String {
    if let Ok(p) = std::env::var("ZMUX_SOCK") {
        return p;
    }
    if let Ok(d) = std::env::var("XDG_RUNTIME_DIR") {
        return format!("{}/zmux.sock", d);
    }
    format!("/tmp/zmux-{}.sock", unsafe { libc::getuid() })
}

fn die(msg: &str) -> ! {
    eprintln!("zmux: {}", msg);
    std::process::exit(1)
}

// ---------------------------------------------------------------- 線 (シリアルか TCP)

enum Port {
    Serial(std::fs::File),
    Tcp(TcpStream),
}

impl Port {
    fn try_clone(&self) -> std::io::Result<Port> {
        Ok(match self {
            Port::Serial(f) => Port::Serial(f.try_clone()?),
            Port::Tcp(s) => Port::Tcp(s.try_clone()?),
        })
    }
    fn read(&mut self, b: &mut [u8]) -> std::io::Result<usize> {
        match self {
            Port::Serial(f) => f.read(b),
            Port::Tcp(s) => s.read(b),
        }
    }
    fn write_all(&mut self, b: &[u8]) -> std::io::Result<()> {
        match self {
            Port::Serial(f) => f.write_all(b),
            Port::Tcp(s) => s.write_all(b),
        }
    }
}

fn open_serial(path: &str, baud: u32) -> std::fs::File {
    let c = std::ffi::CString::new(path).unwrap();
    let fd = unsafe { libc::open(c.as_ptr(), libc::O_RDWR | libc::O_NOCTTY) };
    if fd < 0 {
        die(&format!("{}: {}", path, std::io::Error::last_os_error()));
    }
    let speed = match baud {
        9600 => libc::B9600,
        19200 => libc::B19200,
        38400 => libc::B38400,
        57600 => libc::B57600,
        115200 => libc::B115200,
        _ => die("--baud: 9600 / 19200 / 38400 / 57600 / 115200"),
    };
    unsafe {
        let mut t: libc::termios = std::mem::zeroed();
        libc::tcgetattr(fd, &mut t);
        libc::cfmakeraw(&mut t);
        t.c_cflag |= libc::CLOCAL | libc::CREAD;
        t.c_cflag &= !libc::CRTSCTS;
        t.c_iflag &= !(libc::IXON | libc::IXOFF | libc::IXANY);
        t.c_cc[libc::VMIN] = 1;
        t.c_cc[libc::VTIME] = 0;
        libc::cfsetispeed(&mut t, speed);
        libc::cfsetospeed(&mut t, speed);
        libc::tcsetattr(fd, libc::TCSANOW, &t);
        libc::tcflush(fd, libc::TCIOFLUSH);
        std::fs::File::from_raw_fd(fd)
    }
}

// ---------------------------------------------------------------- デーモン

enum Msg {
    Line(Vec<u8>),
    LineClosed,
    Client(u64, UnixStream, String),
    ClientIn(u64, Vec<u8>),
    ClientWinch(u64, u16, u16),
    ClientGone(u64),
}

/// クライアントがつながっている先
#[derive(Clone, Copy, PartialEq)]
enum Att {
    Console,      // チャネル 0 (タブ 0、COMMAND.X の CTTY AUX)
    Sess(usize),  // zosh のセッション (0xA7)。zmux attach では 1〜4 と数える
}

fn daemon(args: &[String]) {
    let mut port_path = None;
    let mut tcp = None;
    let mut baud = 19200u32;
    let mut log = false;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--port" => { port_path = args.get(i + 1).cloned(); i += 1; }
            "--tcp" => { tcp = args.get(i + 1).cloned(); i += 1; }
            "--baud" => { baud = args.get(i + 1).and_then(|s| s.parse().ok()).unwrap_or_else(|| die("--baud")); i += 1; }
            "-v" => log = true,
            a => die(&format!("unknown option {}", a)),
        }
        i += 1;
    }
    let port = if let Some(t) = tcp {
        let mut s = None;
        for _ in 0..60 {
            if let Ok(c) = TcpStream::connect(&t) {
                s = Some(c);
                break;
            }
            thread::sleep(Duration::from_secs(1));
        }
        let s = s.unwrap_or_else(|| die(&format!("cannot connect to {}", t)));
        s.set_nodelay(true).ok();
        Port::Tcp(s)
    } else {
        let p = port_path.unwrap_or_else(|| std::env::var("X68_DEV").unwrap_or_else(|_| die("--port か --tcp が要る")));
        Port::Serial(open_serial(&p, baud))
    };
    let (tx, rx) = mpsc::channel::<Msg>();
    // 線を読むスレッド
    {
        let mut rp = port.try_clone().unwrap_or_else(|e| die(&e.to_string()));
        let tx = tx.clone();
        thread::spawn(move || {
            let mut b = [0u8; 4096];
            loop {
                match rp.read(&mut b) {
                    Ok(0) | Err(_) => {
                        let _ = tx.send(Msg::LineClosed);
                        return;
                    }
                    Ok(n) => {
                        if tx.send(Msg::Line(b[..n].to_vec())).is_err() {
                            return;
                        }
                    }
                }
            }
        });
    }
    // ソケットで待つスレッド
    let sp = sock_path();
    let _ = std::fs::remove_file(&sp);
    let listener = UnixListener::bind(&sp).unwrap_or_else(|e| die(&format!("{}: {}", sp, e)));
    {
        let tx = tx.clone();
        thread::spawn(move || {
            let mut id = 0u64;
            for s in listener.incoming().flatten() {
                id += 1;
                let tx = tx.clone();
                let my = id;
                thread::spawn(move || client_reader(my, s, tx));
            }
        });
    }
    eprintln!("zmuxd: socket {}", sp);
    let mut wp = port;
    let mut link = link::Link::new(Instant::now());
    link.byte_time = Duration::from_micros(10_000_000 / baud as u64);
    let mut scroll: VecDeque<u8> = VecDeque::new();
    let mut clients: HashMap<u64, (UnixStream, Att)> = HashMap::new();
    let mut np = netpipe::Netpipe::new();
    np.tsp.log = log;
    np.zss.log = log;
    let mut npblk: Vec<u8> = Vec::new();      // チャネル 1 で組み立て中の要求のブロック
    let mut last_xchg = Instant::now();
    let mut last_kick = Instant::now();
    let mut loop_t = Instant::now();
    let mut linelog = std::env::var("ZMUX_LINELOG").ok().and_then(|p| std::fs::File::create(p).ok());   // 試験用: 線に書いたバイト
    let mut loop_what = "";
    loop {
        let m = rx.recv_timeout(Duration::from_millis(10));
        let now = Instant::now();
        if log && now.duration_since(loop_t) > Duration::from_millis(500) {
            eprintln!("zmuxd: main loop stalled {:?} (last: {})", now.duration_since(loop_t), loop_what);
        }
        let mut out = Vec::new();
        let mut ev = Vec::new();
        match m {
            Ok(Msg::Line(b)) => link.input(&b, &mut out, &mut ev, now),
            Ok(Msg::LineClosed) => die("line closed"),
            Ok(Msg::Client(id, mut s, cmd)) => {
                let words: Vec<&str> = cmd.split_whitespace().collect();
                let num = |k: usize, d: u16| -> u16 { words.get(k).and_then(|w| w.parse().ok()).unwrap_or(d) };
                match words.first().copied() {
                    Some("attach") => {
                        // attach 0 / attach new COLS ROWS TERM / attach N COLS ROWS (N = 1..4)
                        match words.get(1).copied().unwrap_or("0") {
                            "0" => {
                                let v: Vec<u8> = scroll.iter().copied().collect();
                                let _ = s.write_all(&v);
                                clients.insert(id, (s, Att::Console));
                            }
                            "new" => {
                                if !np.zss.seen {
                                    let _ = s.write_all(b"zmux: Z-OS (zos.x) is not running on the X68000 (no 0xA7 records yet)\r\n");
                                } else if let Some(i) = np.zss.open(num(2, 80), num(3, 24), words.get(4).copied().unwrap_or("xterm-256color")) {
                                    let _ = s.write_all(format!("\x1b]2;zmux {}\x07", i + 1).as_bytes());
                                    clients.insert(id, (s, Att::Sess(i)));
                                } else {
                                    let _ = s.write_all(b"zmux: no free session (4 in use; zmux kill N)\r\n");
                                }
                            }
                            n => match n.parse::<usize>() {
                                Ok(k) if (1..=zss::NSESS).contains(&k) && np.zss.s[k - 1].used => {
                                    let v: Vec<u8> = np.zss.s[k - 1].scroll.iter().copied().collect();
                                    let _ = s.write_all(&v);
                                    np.zss.winch(k - 1, num(2, 80), num(3, 24));
                                    clients.insert(id, (s, Att::Sess(k - 1)));
                                }
                                _ => { let _ = s.write_all(format!("zmux: no session {}\r\n", n).as_bytes()); }
                            },
                        }
                    }
                    Some("kill") => {
                        if let Some(k) = words.get(1).and_then(|w| w.parse::<usize>().ok()).filter(|k| (1..=zss::NSESS).contains(k)) {
                            np.zss.kill(k - 1);
                        }
                        let _ = writeln!(s, "ok");
                    }
                    Some("status") => {
                        let st = &link.stats;
                        let sess: Vec<String> = (0..zss::NSESS).filter(|&i| np.zss.s[i].used)
                            .map(|i| format!("{}:{}x{}{}", i + 1, np.zss.s[i].cols, np.zss.s[i].rows, if np.zss.s[i].accepted { "" } else { "(opening)" })).collect();
                        let _ = writeln!(s, "line {} rx {} tx {} resent {} bad {} ups {} downs {} pending {} clients {} xchg {} tsp {} zos {} sessions [{}]",
                                         if link.up { "up" } else { "raw" }, st.rx_frames, st.tx_frames, st.resent,
                                         link.parser.bad, st.ups, st.downs, link.pending(), clients.len(),
                                         np.stats.xchg, np.stats.tsp, if np.zss.seen { "yes" } else { "no" }, sess.join(" "));
                        if words.get(1) == Some(&"-v") { let _ = writeln!(s, "{}", link.debug()); }
                    }
                    _ => { let _ = writeln!(s, "unknown command"); }
                }
            }
            Ok(Msg::ClientIn(id, d)) => {
                match clients.get(&id).map(|c| c.1) {
                    Some(Att::Console) => {
                        if !link.up {
                            out.extend_from_slice(&d); // RAW: そのまま線へ
                        } else {
                            link.send(link::CH_CON, &d);
                        }
                    }
                    Some(Att::Sess(i)) => np.zss.input(i, &d),
                    None => {}
                }
            }
            Ok(Msg::ClientWinch(id, c, r)) => {
                if let Some((_, Att::Sess(i))) = clients.get(&id) {
                    np.zss.winch(*i, c, r);
                }
            }
            Ok(Msg::ClientGone(id)) => { clients.remove(&id); }
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(_) => return,
        }
        let news = np.tsp.poll() | np.zss.has_news();
        // Pi に X68000 へ渡すものがあって、しばらく交換が無ければ KICK で npd.x に交換を促す
        if link.up && news && now.duration_since(last_xchg) > Duration::from_millis(20)
            && now.duration_since(last_kick) > Duration::from_millis(100) {
            link.ctl_now(link::OP_KICK, &mut out, now);
            last_kick = now;
        }
        link.tick(&mut out, &mut ev, now);
        for e in ev {
            let (ch, data) = match e {
                link::Event::Raw(d) => (0u8, d),
                link::Event::Data(link::CH_NP, d, fl) => {
                    npblk.extend_from_slice(&d);
                    if fl & link::F_END != 0 {
                        if let Some(res) = np.exchange(&npblk) {
                            link.send_block(link::CH_NP, &res);
                        }
                        npblk.clear();
                        last_xchg = now;
                        // セッションの出力・終わりをクライアントへ
                        for o in std::mem::take(&mut np.zout) {
                            match o {
                                zss::Out::Output(i, d) => clients.retain(|_, (s, a)| *a != Att::Sess(i) || s.write_all(&d).is_ok()),
                                zss::Out::Exit(i, st) => {
                                    let msg = format!("\r\n[zmux: session {} exited ({})]\r\n", i + 1, st);
                                    clients.retain(|_, (s, a)| {
                                        if *a == Att::Sess(i) {
                                            let _ = s.write_all(msg.as_bytes());
                                            let _ = s.shutdown(std::net::Shutdown::Both);
                                            false
                                        } else { true }
                                    });
                                }
                                zss::Out::Rejected(i, why) => {
                                    let msg = format!("zmux: Z-OS rejected the session: {}\r\n", why);
                                    clients.retain(|_, (s, a)| {
                                        if *a == Att::Sess(i) {
                                            let _ = s.write_all(msg.as_bytes());
                                            let _ = s.shutdown(std::net::Shutdown::Both);
                                            false
                                        } else { true }
                                    });
                                }
                            }
                        }
                    }
                    continue;
                }
                link::Event::Data(ch, d, _) => (ch, d),
                link::Event::Up => { if log { eprintln!("zmuxd: line up"); } continue; }
                link::Event::Down => { if log { eprintln!("zmuxd: line down (raw)"); } npblk.clear(); continue; }
            };
            if ch == 0 {
                scroll.extend(data.iter().copied());
                while scroll.len() > SCROLLBACK {
                    scroll.pop_front();
                }
            }
            if ch == 0 {
                clients.retain(|_, (s, a)| *a != Att::Console || s.write_all(&data).is_ok());
            }
        }
        loop_what = "write line";
        let tw = Instant::now();
        if !out.is_empty() {
            if let Some(f) = linelog.as_mut() { let _ = f.write_all(&out); }
            if let Err(e) = wp.write_all(&out) {
                die(&format!("write: {}", e));
            }
        }
        if log && tw.elapsed() > Duration::from_millis(300) {
            eprintln!("zmuxd: writing {} bytes to the line took {:?}", out.len(), tw.elapsed());
        }
        loop_t = Instant::now();
        loop_what = "idle";
    }
}

fn client_reader(id: u64, s: UnixStream, tx: mpsc::Sender<Msg>) {
    let mut r = match s.try_clone() { Ok(r) => r, Err(_) => return };
    // 最初の 1 行が命令 ("attach 0" / "status")
    let mut line = Vec::new();
    let mut b = [0u8; 1];
    while let Ok(1) = r.read(&mut b) {
        if b[0] == b'\n' { break; }
        line.push(b[0]);
        if line.len() > 200 { return; }
    }
    let cmd = String::from_utf8_lossy(&line).to_string();
    if tx.send(Msg::Client(id, s, cmd)).is_err() { return; }
    // 以後は [種類 1][長さ 2][中身]。種類 0 = 打ったバイト、1 = 端末の大きさ [cols 2][rows 2]
    loop {
        let mut h = [0u8; 3];
        if r.read_exact(&mut h).is_err() { let _ = tx.send(Msg::ClientGone(id)); return; }
        let n = ((h[1] as usize) << 8) | h[2] as usize;
        let mut d = vec![0u8; n];
        if r.read_exact(&mut d).is_err() { let _ = tx.send(Msg::ClientGone(id)); return; }
        let m = match h[0] {
            0 => Msg::ClientIn(id, d),
            1 if n >= 4 => Msg::ClientWinch(id, ((d[0] as u16) << 8) | d[1] as u16, ((d[2] as u16) << 8) | d[3] as u16),
            _ => continue,
        };
        if tx.send(m).is_err() { return; }
    }
}

// ---------------------------------------------------------------- クライアント

struct RawMode { fd: RawFd, old: libc::termios }

impl RawMode {
    fn new(fd: RawFd) -> Option<RawMode> {
        unsafe {
            if libc::isatty(fd) == 0 { return None; }
            let mut old: libc::termios = std::mem::zeroed();
            libc::tcgetattr(fd, &mut old);
            let mut t = old;
            libc::cfmakeraw(&mut t);
            libc::tcsetattr(fd, libc::TCSANOW, &t);
            Some(RawMode { fd, old })
        }
    }
}

impl Drop for RawMode {
    fn drop(&mut self) {
        unsafe { libc::tcsetattr(self.fd, libc::TCSANOW, &self.old); }
    }
}

fn connect() -> UnixStream {
    let sp = sock_path();
    UnixStream::connect(&sp).unwrap_or_else(|e| die(&format!("{}: {} (zmuxd は動いているか)", sp, e)))
}

fn winsize() -> (u16, u16) {
    unsafe {
        let mut w: libc::winsize = std::mem::zeroed();
        if libc::ioctl(1, libc::TIOCGWINSZ, &mut w) == 0 && w.ws_col > 0 {
            return (w.ws_col, w.ws_row);
        }
    }
    (80, 24)
}

static WINCH: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
extern "C" fn on_winch(_: libc::c_int) { WINCH.store(true, std::sync::atomic::Ordering::Relaxed); }

fn send_frame(s: &mut UnixStream, t: u8, d: &[u8]) -> std::io::Result<()> {
    let mut b = vec![t, (d.len() >> 8) as u8, d.len() as u8];
    b.extend_from_slice(d);
    s.write_all(&b)
}

/// zmux attach [0 | N] : 端末をそのまま X68000 のチャネル 0 か zosh のセッションにつなぐ。番号なしは新しいセッション。
/// ホットキーは無い。終わるのは、セッションの zosh が終わったときか、herdr / tmux でペインを閉じたとき
fn attach(args: &[String]) {
    let (c, r) = winsize();
    let term = std::env::var("TERM").unwrap_or_else(|_| "xterm-256color".into());
    let what = args.first().cloned().unwrap_or_else(|| "new".into());
    let mut s = connect();
    let cmd = if what == "0" { "attach 0\n".to_string() } else { format!("attach {} {} {} {}\n", what, c, r, term) };
    s.write_all(cmd.as_bytes()).unwrap();
    let raw = RawMode::new(0);
    unsafe { libc::signal(libc::SIGWINCH, on_winch as *const () as usize); }
    let mut rd = s.try_clone().unwrap();
    let out = thread::spawn(move || {
        let mut b = [0u8; 4096];
        let mut so = std::io::stdout();
        loop {
            match rd.read(&mut b) {
                Ok(0) | Err(_) => return,
                Ok(n) => { if so.write_all(&b[..n]).is_err() { return; } let _ = so.flush(); }
            }
        }
    });
    {
        let mut w = s.try_clone().unwrap();
        thread::spawn(move || loop {
            thread::sleep(Duration::from_millis(200));
            if WINCH.swap(false, std::sync::atomic::Ordering::Relaxed) {
                let (c, r) = winsize();
                let mut d = c.to_be_bytes().to_vec();
                d.extend_from_slice(&r.to_be_bytes());
                if send_frame(&mut w, 1, &d).is_err() { return; }
            }
        });
    }
    thread::spawn(move || {
        let mut b = [0u8; 4096];
        let mut si = std::io::stdin();
        loop {
            match si.read(&mut b) {
                Ok(0) | Err(_) => { let _ = s.shutdown(std::net::Shutdown::Write); return; }
                Ok(n) => { if send_frame(&mut s, 0, &b[..n]).is_err() { return; } }
            }
        }
    });
    let _ = out.join();
    drop(raw);
}

fn kill(args: &[String]) {
    let mut s = connect();
    s.write_all(format!("kill {}\n", args.first().map(|s| s.as_str()).unwrap_or("")).as_bytes()).unwrap();
    let mut out = String::new();
    let _ = std::io::BufRead::read_line(&mut std::io::BufReader::new(s), &mut out);
}

fn status() {
    let mut s = connect();
    s.write_all(if std::env::args().any(|a| a == "-v") { b"status -v\n" as &[u8] } else { b"status\n" }).unwrap();
    let mut out = String::new();
    let mut r = std::io::BufReader::new(s);
    let _ = std::io::BufRead::read_line(&mut r, &mut out);
    if std::env::args().any(|a| a == "-v") { let _ = std::io::BufRead::read_line(&mut r, &mut out); }
    print!("{}", out);
}

fn main() {
    let argv: Vec<String> = std::env::args().collect();
    let prog = std::path::Path::new(&argv[0]).file_name().map(|s| s.to_string_lossy().to_string()).unwrap_or_default();
    if prog == "zmuxd" {
        return daemon(&argv[1..]);
    }
    match argv.get(1).map(|s| s.as_str()) {
        Some("daemon") => daemon(&argv[2..]),
        Some("attach") => attach(&argv[2..]),
        Some("status") => status(),
        Some("kill") => kill(&argv[2..]),
        _ => {
            eprintln!("usage: zmux daemon (--port DEV [--baud N] | --tcp HOST:PORT) [-v]\n       zmux attach [0 | N]   (0 = X68000 のコンソール、番号なし = 新しい zosh のセッション、N = セッション N につなぎ直す)\n       zmux kill N\n       zmux status");
            std::process::exit(2);
        }
    }
}
