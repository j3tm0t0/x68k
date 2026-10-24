//! zmux -- X68000 の RS-232C 1 本を多重化する Pi 側 (net/zmux/DESIGN.md)
//!
//!   zmux daemon --port /dev/ttyUSB0 [--baud 19200]   線を持つ (zmuxd)。`zmuxd` という名前で起動しても同じ
//!   zmux daemon --tcp 127.0.0.1:54368                 XEiJ の RS-232C (TCP/IP ⇔ AUX) につなぐ
//!   zmux attach [0]                                   チャネル 0 (COMMAND.X のコンソール、タブ 0) の端末
//!   zmux status                                       線の状態
//!
//! attach にはホットキーも前置キーも無い。端末を raw にして、届いたバイトをそのまま送る。
//! 切り離しやタブは herdr / tmux の役目。デーモンとはソケット ($ZMUX_SOCK、無ければ $XDG_RUNTIME_DIR/zmux.sock、
//! それも無ければ /tmp/zmux-<uid>.sock) でつながる。

mod link;
mod netpipe;
mod tsp;

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
    ClientGone(u64),
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
    let mut clients: HashMap<u64, (UnixStream, u8)> = HashMap::new();
    let mut np = netpipe::Netpipe::new();
    np.tsp.log = log;
    let mut npblk: Vec<u8> = Vec::new();      // チャネル 1 で組み立て中の要求のブロック
    let mut last_xchg = Instant::now();
    let mut last_kick = Instant::now();
    loop {
        let m = rx.recv_timeout(Duration::from_millis(10));
        let now = Instant::now();
        let mut out = Vec::new();
        let mut ev = Vec::new();
        match m {
            Ok(Msg::Line(b)) => link.input(&b, &mut out, &mut ev, now),
            Ok(Msg::LineClosed) => die("line closed"),
            Ok(Msg::Client(id, mut s, cmd)) => {
                let words: Vec<&str> = cmd.split_whitespace().collect();
                match words.first().copied() {
                    Some("attach") => {
                        let ch: u8 = words.get(1).and_then(|w| w.parse().ok()).unwrap_or(0);
                        if ch == 0 {
                            let v: Vec<u8> = scroll.iter().copied().collect();
                            let _ = s.write_all(&v);
                        }
                        clients.insert(id, (s, ch));
                    }
                    Some("status") => {
                        let st = &link.stats;
                        let _ = writeln!(s, "line {} rx {} tx {} resent {} bad {} ups {} downs {} pending {} clients {} xchg {} tsp {}",
                                         if link.up { "up" } else { "raw" }, st.rx_frames, st.tx_frames, st.resent,
                                         link.parser.bad, st.ups, st.downs, link.pending(), clients.len(),
                                         np.stats.xchg, np.stats.tsp);
                    }
                    _ => { let _ = writeln!(s, "unknown command"); }
                }
            }
            Ok(Msg::ClientIn(id, d)) => {
                if let Some((_, ch)) = clients.get(&id) {
                    if *ch == 0 && !link.up {
                        out.extend_from_slice(&d); // RAW: そのまま線へ
                    } else {
                        link.send(*ch, &d);
                    }
                }
            }
            Ok(Msg::ClientGone(id)) => { clients.remove(&id); }
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(_) => return,
        }
        let news = np.tsp.poll();
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
            clients.retain(|_, (s, c)| *c != ch || s.write_all(&data).is_ok());
        }
        if !out.is_empty() {
            if let Err(e) = wp.write_all(&out) {
                die(&format!("write: {}", e));
            }
        }
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
    let mut buf = [0u8; 4096];
    loop {
        match r.read(&mut buf) {
            Ok(0) | Err(_) => { let _ = tx.send(Msg::ClientGone(id)); return; }
            Ok(n) => { if tx.send(Msg::ClientIn(id, buf[..n].to_vec())).is_err() { return; } }
        }
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

fn attach(args: &[String]) {
    let ch: u8 = args.first().and_then(|a| a.parse().ok()).unwrap_or(0);
    let mut s = connect();
    s.write_all(format!("attach {}\n", ch).as_bytes()).unwrap();
    let raw = RawMode::new(0);
    let mut r = s.try_clone().unwrap();
    let out = thread::spawn(move || {
        let mut b = [0u8; 4096];
        let mut so = std::io::stdout();
        loop {
            match r.read(&mut b) {
                Ok(0) | Err(_) => return,
                Ok(n) => { if so.write_all(&b[..n]).is_err() { return; } let _ = so.flush(); }
            }
        }
    });
    thread::spawn(move || {
        let mut b = [0u8; 4096];
        let mut si = std::io::stdin();
        loop {
            match si.read(&mut b) {
                Ok(0) | Err(_) => { let _ = s.shutdown(std::net::Shutdown::Write); return; }
                Ok(n) => { if s.write_all(&b[..n]).is_err() { return; } }
            }
        }
    });
    let _ = out.join();
    drop(raw);
}

fn status() {
    let mut s = connect();
    s.write_all(b"status\n").unwrap();
    let mut out = String::new();
    let _ = std::io::BufRead::read_line(&mut std::io::BufReader::new(s), &mut out);
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
        _ => {
            eprintln!("usage: zmux daemon (--port DEV [--baud N] | --tcp HOST:PORT) [-v]\n       zmux attach [0]\n       zmux status");
            std::process::exit(2);
        }
    }
}
