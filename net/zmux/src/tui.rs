//! `zmux` (引数なし): tmux のような画面 (net/zmux/DESIGN.md §5)。
//!
//! zmuxd (サーバー) がシリアル回線とセッションを持ち、この画面はクライアント。ペイン 1 つ = zmuxd へのソケット 1 本
//! (`attach 0` か `open` / `attach N`)。ssh が切れても zmuxd のセッションは残り、`zmux` を打てばつなぎ直す
//! (タブとペインの配置は zmuxd に置いておく: `layout set` / `layout get`)。
//!
//! キー: 前置キー Ctrl+X のあと
//!   |  左右に割る (新しい zosh)    -  上下に割る    c  新しいタブ (新しい zosh)    n / p / 0〜9  タブの切り替え
//!   k  ペインを閉じる (zosh を kill)    d  切り離す    ?  説明    Ctrl+X  Ctrl+X そのものを X68000 へ
//! ほかのホットキーは無い。前置キーの組とマウスの並び (SGR 1006) 以外のバイトは全部そのまま X68000 へ送る
//! (CSI u などの拡張キーの並びも素通し)。X68000 にしか無いキーは keys.toml の対応表で、タブ 0 のときだけキー入力の注入に変える。
//! マウス: クリックでペインとタブを選ぶ、ホイールでそのペインのスクロールバック。マウスの並びは X68000 へは送らない。
//!
//! 描画: ペインごとに vt100 のパーサ (vt100 クレート)、ratatui + crossterm。タブ 0 (COMMAND.X の CTTY AUX) は Shift_JIS なので
//! UTF-8 に変えて表示し、打った字は Shift_JIS にして送る。zosh のセッションは UTF-8 のまま (zos.x が変える)。
//! 下の 1 行は状態: タブの一覧、線 (up / raw)、速さ、受けた・送ったフレームと送り直しの数。

use std::collections::HashMap;
use std::io::{BufRead, Read, Write};
use std::os::unix::net::UnixStream;
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, Instant};

use ratatui::backend::CrosstermBackend;
use ratatui::buffer::Buffer;
use ratatui::layout::{Constraint, Direction, Layout, Position, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::widgets::{Block, Clear, Paragraph, Widget};
use ratatui::Terminal;

const PREFIX: u8 = 0x18; // Ctrl+X
const MSG_TIME: Duration = Duration::from_secs(4);

// ---------------------------------------------------------------- タブとペインの配置 (木)

#[derive(Debug, Clone, PartialEq)]
pub enum Node {
    Leaf(usize),                              // ペインの番号 (App の panes の鍵)
    Split(bool, Box<Node>, Box<Node>),        // true = 左右 (|)、false = 上下 (-)
}

impl Node {
    pub fn leaves(&self, out: &mut Vec<usize>) {
        match self {
            Node::Leaf(p) => out.push(*p),
            Node::Split(_, a, b) => { a.leaves(out); b.leaves(out); }
        }
    }

    /// ペイン p を割って、新しいペイン q を隣 (右か下) に置く
    pub fn split(&mut self, p: usize, q: usize, vertical: bool) -> bool {
        match self {
            Node::Leaf(x) if *x == p => {
                *self = Node::Split(vertical, Box::new(Node::Leaf(p)), Box::new(Node::Leaf(q)));
                true
            }
            Node::Leaf(_) => false,
            Node::Split(_, a, b) => a.split(p, q, vertical) || b.split(p, q, vertical),
        }
    }

    /// ペイン p を外す。木が空になったら None
    pub fn remove(self, p: usize) -> Option<Node> {
        match self {
            Node::Leaf(x) => if x == p { None } else { Some(Node::Leaf(x)) },
            Node::Split(v, a, b) => match (a.remove(p), b.remove(p)) {
                (Some(a), Some(b)) => Some(Node::Split(v, Box::new(a), Box::new(b))),
                (Some(a), None) => Some(a),
                (None, Some(b)) => Some(b),
                (None, None) => None,
            },
        }
    }

    /// 区画を割り当てる (半分ずつ)
    pub fn areas(&self, r: Rect, out: &mut Vec<(usize, Rect)>) {
        match self {
            Node::Leaf(p) => out.push((*p, r)),
            Node::Split(v, a, b) => {
                let d = if *v { Direction::Horizontal } else { Direction::Vertical };
                let c = Layout::default().direction(d).constraints([Constraint::Percentage(50), Constraint::Percentage(50)]).split(r);
                a.areas(c[0], out);
                b.areas(c[1], out);
            }
        }
    }

    /// 置いておく形: 葉はセッションの番号 (0 = コンソール)、"(v A B)" / "(h A B)"
    pub fn serialize(&self, sess_of: &dyn Fn(usize) -> usize) -> String {
        match self {
            Node::Leaf(p) => sess_of(*p).to_string(),
            Node::Split(v, a, b) => format!("({} {} {})", if *v { 'v' } else { 'h' }, a.serialize(sess_of), b.serialize(sess_of)),
        }
    }

    /// serialize の逆。葉の値はセッションの番号のまま返す (Leaf(sess))
    pub fn parse(s: &str) -> Option<Node> {
        let toks: Vec<String> = s.replace('(', " ( ").replace(')', " ) ").split_whitespace().map(|t| t.to_string()).collect();
        let mut i = 0;
        let n = Self::parse_at(&toks, &mut i)?;
        if i == toks.len() { Some(n) } else { None }
    }

    fn parse_at(t: &[String], i: &mut usize) -> Option<Node> {
        let tok = t.get(*i)?.clone();
        *i += 1;
        if tok == "(" {
            let v = match t.get(*i)?.as_str() { "v" => true, "h" => false, _ => return None };
            *i += 1;
            let a = Self::parse_at(t, i)?;
            let b = Self::parse_at(t, i)?;
            if t.get(*i)? != ")" { return None; }
            *i += 1;
            Some(Node::Split(v, Box::new(a), Box::new(b)))
        } else {
            tok.parse::<usize>().ok().map(Node::Leaf)
        }
    }
}

// ---------------------------------------------------------------- 打ったバイトの読み取り

#[derive(Debug, Clone, PartialEq)]
pub enum Action {
    Forward(Vec<u8>),               // そのまま今のペインへ
    Inject(u16),                    // keys.toml の並び → キー入力の注入 (タブ 0 のとき)
    Cmd(u8),                        // 前置キーのあとの 1 文字
    Mouse { btn: u16, x: u16, y: u16, press: bool },
}

/// 前置キーとマウスの並びを拾い、ほかは全部そのまま通す。並びが途中なら持っておく (次のバイトか時間切れで流す)
pub struct Input {
    pub prefix: bool,
    pending: Vec<u8>,
    pub map: crate::keys::Map,
    pub use_map: bool,
    since: Option<Instant>,
}

impl Input {
    pub fn new(map: crate::keys::Map) -> Input {
        Input { prefix: false, pending: Vec::new(), map, use_map: false, since: None }
    }

    pub fn feed(&mut self, b: &[u8], now: Instant) -> Vec<Action> {
        self.pending.extend_from_slice(b);
        self.since = Some(now);
        self.drain(false)
    }

    /// 持っているものを時間切れで流す (20ms 以上たっていたら)
    pub fn flush(&mut self, now: Instant) -> Vec<Action> {
        match self.since {
            Some(t) if !self.pending.is_empty() && now.duration_since(t) >= Duration::from_millis(20) => self.drain(true),
            _ => Vec::new(),
        }
    }

    fn drain(&mut self, force: bool) -> Vec<Action> {
        let mut out = Vec::new();
        let mut plain = Vec::new();
        let b = std::mem::take(&mut self.pending);
        let mut i = 0;
        let mut hold_from = None;
        while i < b.len() {
            let c = b[i];
            if self.prefix {
                self.prefix = false;
                if c == PREFIX { plain.push(PREFIX); } else {
                    if !plain.is_empty() { out.push(Action::Forward(std::mem::take(&mut plain))); }
                    out.push(Action::Cmd(c));
                }
                i += 1;
                continue;
            }
            if c == PREFIX {
                self.prefix = true;
                i += 1;
                continue;
            }
            if c == 0x1b {
                // マウス: ESC [ < b ; x ; y M/m
                if b[i..].starts_with(b"\x1b[<") {
                    if let Some(end) = b[i..].iter().position(|&x| x == b'M' || x == b'm') {
                        if let Some(m) = parse_mouse(&b[i + 3..i + end], b[i + end] == b'M') {
                            if !plain.is_empty() { out.push(Action::Forward(std::mem::take(&mut plain))); }
                            out.push(m);
                        }
                        i += end + 1;
                        continue;
                    }
                    if !force && b.len() - i < 24 { hold_from = Some(i); break; }
                }
                if self.use_map {
                    let rest = &b[i..];
                    if let Some((seq, code)) = self.map.0.iter().find(|(seq, _)| rest.starts_with(seq)) {
                        if !plain.is_empty() { out.push(Action::Forward(std::mem::take(&mut plain))); }
                        out.push(Action::Inject(*code));
                        i += seq.len();
                        continue;
                    }
                    if !force && self.map.0.iter().any(|(seq, _)| seq.starts_with(rest)) { hold_from = Some(i); break; }
                }
                if !force && b.len() - i < 3 && b.len() - i >= 1 && (b.len() - i == 1 || b[i + 1] == b'[') {
                    hold_from = Some(i); // ESC だけ / ESC [ だけ: 続きがマウスの並びかもしれない
                    break;
                }
            }
            plain.push(c);
            i += 1;
        }
        if let Some(h) = hold_from { self.pending = b[h..].to_vec(); }
        if !plain.is_empty() { out.push(Action::Forward(plain)); }
        out
    }
}

fn parse_mouse(body: &[u8], press: bool) -> Option<Action> {
    let s = std::str::from_utf8(body).ok()?;
    let mut it = s.split(';').map(|v| v.parse::<u16>().ok());
    let (btn, x, y) = (it.next()??, it.next()??, it.next()??);
    Some(Action::Mouse { btn, x, y, press })
}

// ---------------------------------------------------------------- 文字コード (タブ 0 だけ)

/// UTF-8 → Shift_JIS。末尾の途中の UTF-8 は hold に残す。表せない字は '?'、UTF-8 として壊れたバイトはそのまま
pub fn utf8_to_sjis_stream(input: &[u8], hold: &mut Vec<u8>) -> Vec<u8> {
    let mut b = std::mem::take(hold);
    b.extend_from_slice(input);
    let mut out = Vec::new();
    let mut i = 0;
    while i < b.len() {
        match std::str::from_utf8(&b[i..]) {
            Ok(s) => { encode_sjis(s, &mut out); i = b.len(); }
            Err(e) => {
                let ok = e.valid_up_to();
                encode_sjis(std::str::from_utf8(&b[i..i + ok]).unwrap_or(""), &mut out);
                i += ok;
                match e.error_len() {
                    Some(n) => { out.extend_from_slice(&b[i..i + n]); i += n; }
                    None => { *hold = b[i..].to_vec(); return out; }
                }
            }
        }
    }
    out
}

fn encode_sjis(s: &str, out: &mut Vec<u8>) {
    let mut enc = encoding_rs::SHIFT_JIS.new_encoder();
    let mut rest = s;
    let mut buf = vec![0u8; s.len() * 2 + 16];
    loop {
        let (r, read, written) = enc.encode_from_utf8_without_replacement(rest, &mut buf, true);
        out.extend_from_slice(&buf[..written]);
        rest = &rest[read..];
        match r {
            encoding_rs::EncoderResult::InputEmpty => return,
            encoding_rs::EncoderResult::OutputFull => continue,
            encoding_rs::EncoderResult::Unmappable(_) => out.push(b'?'),
        }
    }
}

// ---------------------------------------------------------------- 状態の行

/// 端末のタイトル (OSC 0): 線が上がっていれば "XVI · zmux 19200 · [0] COMMAND.X"、そうでなければ "XVI · 切断中"
pub fn title_text(up: bool, ok: bool, baud: &str, tab: usize, name: &str) -> String {
    if !ok || !up { return "XVI · 切断中".to_string(); }
    format!("XVI · zmux {} · [{}] {}", baud, tab, name)
}

#[derive(Default, Clone)]
pub struct LinkStatus {
    pub up: bool,
    pub baud: String,
    pub rx: String,
    pub tx: String,
    pub resent: String,
    pub ok: bool,
}

pub fn parse_status(line: &str) -> LinkStatus {
    let w: Vec<&str> = line.split_whitespace().collect();
    let mut st = LinkStatus { ok: w.first() == Some(&"line"), ..Default::default() };
    st.up = w.get(1) == Some(&"up");
    // "sessions [1:80x24 2:80x24]" のように値に空白が入るものがあるので、鍵を 1 つずつ探す
    for i in 2..w.len().saturating_sub(1) {
        match w[i] {
            "rx" => st.rx = w[i + 1].to_string(),
            "tx" => st.tx = w[i + 1].to_string(),
            "resent" => st.resent = w[i + 1].to_string(),
            "baud" => st.baud = w[i + 1].to_string(),
            _ => {}
        }
    }
    st
}

// ---------------------------------------------------------------- 画面

struct Pane {
    sess: usize,                       // 0 = コンソール (タブ 0)、1〜4 = zosh のセッション
    sock: UnixStream,
    parser: vt100::Parser,
    dec: Option<encoding_rs::Decoder>, // コンソールだけ Shift_JIS → UTF-8
    enc_hold: Vec<u8>,
    size: (u16, u16),                  // (cols, rows) 今パーサに与えている大きさ
    closed: bool,
}

struct Tab {
    root: Node,
    active: usize,
}

enum Ev {
    Stdin(Vec<u8>),
    StdinClosed,
    Data(usize, Vec<u8>),
    Closed(usize),
    Status(String),
}

struct App {
    tabs: Vec<Tab>,
    cur: usize,
    panes: HashMap<usize, Pane>,
    next: usize,
    input: Input,
    help: bool,
    msg: Option<(String, Instant)>,
    status: LinkStatus,
    areas: Vec<(usize, Rect)>,         // 直前の描画でのペインの区画 (マウス用)
    tab_x: Vec<(u16, u16, usize)>,     // 状態の行のタブの文字の範囲
    tx: mpsc::Sender<Ev>,
    term: String,
    quit: bool,
}

fn query(cmd: &str) -> std::io::Result<String> {
    let mut s = UnixStream::connect(crate::sock_path())?;
    s.set_read_timeout(Some(Duration::from_secs(3)))?;
    s.write_all(format!("{}\n", cmd).as_bytes())?;
    let mut line = String::new();
    std::io::BufReader::new(s).read_line(&mut line)?;
    Ok(line.trim_end().to_string())
}

fn send_frame(s: &mut UnixStream, t: u8, d: &[u8]) -> std::io::Result<()> {
    let mut b = vec![t, (d.len() >> 8) as u8, d.len() as u8];
    b.extend_from_slice(d);
    s.write_all(&b)
}

fn read_line_from(s: &mut UnixStream) -> std::io::Result<String> {
    let mut line = Vec::new();
    let mut b = [0u8; 1];
    loop {
        if s.read(&mut b)? == 0 { break; }
        if b[0] == b'\n' { break; }
        line.push(b[0]);
        if line.len() > 300 { break; }
    }
    Ok(String::from_utf8_lossy(&line).to_string())
}

impl App {
    fn new(tx: mpsc::Sender<Ev>) -> App {
        App {
            tabs: Vec::new(), cur: 0, panes: HashMap::new(), next: 1,
            input: Input::new(crate::keys::Map::load()), help: false, msg: None, status: LinkStatus::default(),
            areas: Vec::new(), tab_x: Vec::new(), tx,
            term: std::env::var("TERM").unwrap_or_else(|_| "xterm-256color".into()), quit: false,
        }
    }

    /// タブ i の名前 (下の行のタブ一覧と同じ)
    fn tab_name(&self, i: usize) -> String {
        let mut l = Vec::new();
        self.tabs[i].root.leaves(&mut l);
        match self.panes.get(&l[0]).map(|p| p.sess).unwrap_or(0) {
            0 => "COMMAND.X".to_string(),
            n => if l.len() > 1 { format!("zosh {}+{}", n, l.len() - 1) } else { format!("zosh {}", n) },
        }
    }

    fn title(&self) -> String {
        let name = if self.cur < self.tabs.len() { self.tab_name(self.cur) } else { String::new() };
        title_text(self.status.up, self.status.ok, &self.status.baud, self.cur, &name)
    }

    fn say(&mut self, m: impl Into<String>) { self.msg = Some((m.into(), Instant::now())); }

    fn spawn_reader(&self, id: usize, s: &UnixStream) {
        let mut rd = match s.try_clone() { Ok(r) => r, Err(_) => return };
        let tx = self.tx.clone();
        thread::spawn(move || {
            let mut b = [0u8; 8192];
            loop {
                match rd.read(&mut b) {
                    Ok(0) | Err(_) => { let _ = tx.send(Ev::Closed(id)); return; }
                    Ok(n) => { if tx.send(Ev::Data(id, b[..n].to_vec())).is_err() { return; } }
                }
            }
        });
    }

    fn add_pane(&mut self, sess: usize, sock: UnixStream) -> usize {
        let id = self.next;
        self.next += 1;
        self.spawn_reader(id, &sock);
        let dec = if sess == 0 { Some(encoding_rs::SHIFT_JIS.new_decoder_without_bom_handling()) } else { None };
        self.panes.insert(id, Pane { sess, sock, parser: vt100::Parser::new(24, 80, 2000), dec, enc_hold: Vec::new(), size: (80, 24), closed: false });
        id
    }

    /// コンソール (タブ 0) につなぐ
    fn attach_console(&mut self) -> std::io::Result<usize> {
        let mut s = UnixStream::connect(crate::sock_path())?;
        s.write_all(b"attach 0\n")?;
        Ok(self.add_pane(0, s))
    }

    /// 既にあるセッション n につなぎ直す
    fn attach_sess(&mut self, n: usize) -> std::io::Result<usize> {
        let mut s = UnixStream::connect(crate::sock_path())?;
        s.write_all(format!("attach {} 80 24\n", n).as_bytes())?;
        Ok(self.add_pane(n, s))
    }

    /// 新しい zosh のセッションを開く ("open" は最初に "ok N" か "err 理由" を 1 行返す)
    fn open_sess(&mut self) -> Result<usize, String> {
        let mut s = UnixStream::connect(crate::sock_path()).map_err(|e| e.to_string())?;
        s.write_all(format!("open 80 24 {}\n", self.term).as_bytes()).map_err(|e| e.to_string())?;
        let line = read_line_from(&mut s).map_err(|e| e.to_string())?;
        match line.strip_prefix("ok ").and_then(|n| n.trim().parse::<usize>().ok()) {
            Some(n) => Ok(self.add_pane(n, s)),
            None => Err(line.strip_prefix("err ").unwrap_or(&line).to_string()),
        }
    }

    fn active_pane(&self) -> Option<usize> { self.tabs.get(self.cur).map(|t| t.active) }

    fn sess_of(&self, p: usize) -> usize { self.panes.get(&p).map(|x| x.sess).unwrap_or(0) }

    fn save_layout(&self) {
        let panes = &self.panes;
        let f = |p: usize| panes.get(&p).map(|x| x.sess).unwrap_or(0);
        let text: Vec<String> = self.tabs.iter().map(|t| t.root.serialize(&f)).collect();
        let _ = query(&format!("layout set {}", text.join(";")));
    }

    /// 起動: zmuxd に置いてある配置を取り、残っているセッションにつなぎ直す。無ければタブ 0 だけ
    fn restore(&mut self) -> std::io::Result<()> {
        let alive: Vec<usize> = query("sessions").unwrap_or_default().split_whitespace().filter_map(|w| w.parse().ok()).collect();
        let layout = query("layout get").unwrap_or_default();
        let mut used: Vec<usize> = Vec::new();
        let mut console_seen = false;
        for part in layout.split(';').filter(|s| !s.trim().is_empty()) {
            let Some(node) = Node::parse(part.trim()) else { continue };
            // 葉のセッション番号 → ペインの番号
            let mut leaves = Vec::new();
            node.leaves(&mut leaves);
            let mut root: Option<Node> = Some(node);
            let mut map: HashMap<usize, usize> = HashMap::new(); // sess → pane id
            for s in leaves {
                let ok = if s == 0 {
                    if console_seen { false } else { console_seen = true; self.attach_console().map(|id| { map.insert(0, id); }).is_ok() }
                } else if alive.contains(&s) && !used.contains(&s) {
                    used.push(s);
                    self.attach_sess(s).map(|id| { map.insert(s, id); }).is_ok()
                } else { false };
                if !ok { root = root.and_then(|r| r.remove(s)); }
            }
            if let Some(r) = root {
                let r = relabel(r, &map);
                let mut l = Vec::new();
                r.leaves(&mut l);
                let active = l[0];
                self.tabs.push(Tab { root: r, active });
            }
        }
        if !console_seen {
            let id = self.attach_console()?;
            self.tabs.insert(0, Tab { root: Node::Leaf(id), active: id });
        }
        // 配置に無いセッションが残っていればタブにして見せる
        for s in alive {
            if !used.contains(&s) {
                if let Ok(id) = self.attach_sess(s) { self.tabs.push(Tab { root: Node::Leaf(id), active: id }); }
            }
        }
        self.cur = 0;
        Ok(())
    }

    fn remove_pane(&mut self, p: usize, kill: bool) {
        if let Some(pane) = self.panes.remove(&p) {
            let _ = pane.sock.shutdown(std::net::Shutdown::Both);
            if kill && pane.sess > 0 { let _ = query(&format!("kill {}", pane.sess)); }
        }
        let mut i = 0;
        while i < self.tabs.len() {
            let t = std::mem::replace(&mut self.tabs[i].root, Node::Leaf(usize::MAX));
            match t.remove(p) {
                Some(r) => {
                    if self.tabs[i].active == p {
                        let mut l = Vec::new();
                        r.leaves(&mut l);
                        self.tabs[i].active = l[0];
                    }
                    self.tabs[i].root = r;
                    i += 1;
                }
                None => { self.tabs.remove(i); }
            }
        }
        if self.tabs.is_empty() {
            // コンソールのペインは消えないはずだが、消えたら作り直す
            if let Ok(id) = self.attach_console() { self.tabs.push(Tab { root: Node::Leaf(id), active: id }); } else { self.quit = true; }
        }
        if self.cur >= self.tabs.len() { self.cur = self.tabs.len() - 1; }
        self.save_layout();
    }

    fn cmd(&mut self, c: u8) {
        match c {
            b'|' | b'-' => {
                let Some(p) = self.active_pane() else { return };
                match self.open_sess() {
                    Ok(q) => {
                        self.tabs[self.cur].root.split(p, q, c == b'|');
                        self.tabs[self.cur].active = q;
                        self.save_layout();
                    }
                    Err(e) => self.say(format!("zosh を開けない: {}", e)),
                }
            }
            b'c' => match self.open_sess() {
                Ok(q) => { self.tabs.push(Tab { root: Node::Leaf(q), active: q }); self.cur = self.tabs.len() - 1; self.save_layout(); }
                Err(e) => self.say(format!("zosh を開けない: {}", e)),
            },
            b'n' => self.cur = (self.cur + 1) % self.tabs.len(),
            b'p' => self.cur = (self.cur + self.tabs.len() - 1) % self.tabs.len(),
            b'0'..=b'9' => {
                let n = (c - b'0') as usize;
                if n < self.tabs.len() { self.cur = n; } else { self.say(format!("タブ {} は無い", n)); }
            }
            b'k' => {
                let Some(p) = self.active_pane() else { return };
                if self.sess_of(p) == 0 { self.say("タブ 0 (COMMAND.X) は閉じられない"); } else { self.remove_pane(p, true); }
            }
            b'd' => { self.save_layout(); self.quit = true; }
            b'?' => self.help = true,
            _ => self.say(format!("Ctrl+X {}: zmux のキーではない (? で一覧)", printable(c))),
        }
    }

    fn mouse(&mut self, btn: u16, x: u16, y: u16, press: bool) {
        let (x, y) = (x.saturating_sub(1), y.saturating_sub(1));
        let wheel = btn & 64 != 0;
        if wheel {
            if let Some((p, _)) = self.areas.iter().find(|(_, r)| r.contains(Position { x, y })) {
                if let Some(pane) = self.panes.get_mut(p) {
                    let cur = pane.parser.screen().scrollback();
                    let n = if btn & 1 == 0 { cur + 3 } else { cur.saturating_sub(3) };
                    pane.parser.set_scrollback(n);
                }
            }
            return;
        }
        if !press || btn & 3 != 0 { return; }
        if let Some((_, _, t)) = self.tab_x.iter().find(|(a, b, _)| x >= *a && x < *b) {
            if *t < self.tabs.len() { self.cur = *t; }
            return;
        }
        if let Some((p, _)) = self.areas.iter().find(|(_, r)| r.contains(Position { x, y })) {
            self.tabs[self.cur].active = *p;
        }
    }

    fn forward(&mut self, d: Vec<u8>) {
        let Some(p) = self.active_pane() else { return };
        let Some(pane) = self.panes.get_mut(&p) else { return };
        if pane.parser.screen().scrollback() > 0 { pane.parser.set_scrollback(0); }
        let bytes = if pane.sess == 0 { utf8_to_sjis_stream(&d, &mut pane.enc_hold) } else { d };
        if !bytes.is_empty() && send_frame(&mut pane.sock, 0, &bytes).is_err() { pane.closed = true; }
    }

    fn inject(&mut self, code: u16) {
        let Some(p) = self.active_pane() else { return };
        if let Some(pane) = self.panes.get_mut(&p) {
            let _ = send_frame(&mut pane.sock, 2, &code.to_be_bytes());
        }
    }

    fn on_actions(&mut self, acts: Vec<Action>) {
        for a in acts {
            if self.help {
                // 説明を見ている間の最初のキーは閉じるだけ (マウスは通す)
                if let Action::Mouse { .. } = a {} else { self.help = false; continue; }
            }
            match a {
                Action::Forward(d) => self.forward(d),
                Action::Inject(c) => self.inject(c),
                Action::Cmd(c) => self.cmd(c),
                Action::Mouse { btn, x, y, press } => self.mouse(btn, x, y, press),
            }
        }
        self.input.use_map = self.active_pane().map(|p| self.sess_of(p) == 0).unwrap_or(false);
    }

    fn on_data(&mut self, id: usize, d: Vec<u8>) {
        let Some(pane) = self.panes.get_mut(&id) else { return };
        match pane.dec.as_mut() {
            Some(dec) => {
                let mut s = String::with_capacity(d.len() * 3);
                let _ = dec.decode_to_string(&d, &mut s, false);
                pane.parser.process(s.as_bytes());
            }
            None => pane.parser.process(&d),
        }
    }

    fn on_closed(&mut self, id: usize) {
        let Some(pane) = self.panes.get(&id) else { return };
        let last = last_line(pane.parser.screen());
        let sess = pane.sess;
        if sess == 0 {
            self.say("zmuxd との接続が切れた (zmuxd は動いているか)");
            self.quit = true;
            return;
        }
        self.say(format!("zosh {} が終わった: {}", sess, last));
        self.remove_pane(id, false);
    }

    /// 描く。ペインの大きさが変わっていればパーサと zmuxd (WINCH) に伝える
    fn draw(&mut self, f: &mut ratatui::Frame) {
        let area = f.area();
        if area.height < 2 || self.tabs.is_empty() { return; }
        let body = Rect { x: area.x, y: area.y, width: area.width, height: area.height - 1 };
        let status_y = area.y + area.height - 1;
        let tab = &self.tabs[self.cur];
        let mut areas = Vec::new();
        tab.root.areas(body, &mut areas);
        let many = areas.len() > 1;
        let active = tab.active;
        let buf = f.buffer_mut();
        let mut cursor = None;
        let mut resized = Vec::new();
        for (p, r) in &areas {
            let inner = if many {
                let title = match self.sess_of(*p) { 0 => " 0 COMMAND.X ".to_string(), n => format!(" zosh {} ", n) };
                let style = if *p == active { Style::default().fg(Color::Green) } else { Style::default().fg(Color::DarkGray) };
                let b = Block::bordered().title(title).border_style(style);
                let inner = b.inner(*r);
                b.render(*r, buf);
                inner
            } else { *r };
            if inner.width == 0 || inner.height == 0 { continue; }
            let Some(pane) = self.panes.get_mut(p) else { continue };
            if pane.size != (inner.width, inner.height) {
                pane.size = (inner.width, inner.height);
                pane.parser.set_size(inner.height, inner.width);
                resized.push(*p);
            }
            draw_screen(pane.parser.screen(), inner, buf);
            if *p == active && !pane.parser.screen().hide_cursor() && pane.parser.screen().scrollback() == 0 {
                let (cy, cx) = pane.parser.screen().cursor_position();
                cursor = Some(Position { x: inner.x + cx.min(inner.width - 1), y: inner.y + cy.min(inner.height - 1) });
            }
        }
        self.areas = areas.iter().map(|(p, r)| (*p, if many { Block::bordered().inner(*r) } else { *r })).collect();
        // 状態の行
        let mut x = area.x;
        self.tab_x.clear();
        let names: Vec<String> = (0..self.tabs.len()).map(|i| self.tab_name(i)).collect();
        for (i, _t) in self.tabs.iter().enumerate() {
            let name = names[i].clone();
            let label = format!(" {}:{} ", i, name);
            let st = if i == self.cur { Style::default().fg(Color::Black).bg(Color::Green) } else { Style::default().fg(Color::White).bg(Color::DarkGray) };
            let w = label.chars().count() as u16;
            if x + w > area.x + area.width { break; }
            buf.set_string(x, status_y, &label, st);
            self.tab_x.push((x, x + w, i));
            x += w;
        }
        let right = match &self.msg {
            Some((m, t)) if t.elapsed() < MSG_TIME => format!(" {} ", m),
            _ => {
                if !self.status.ok { " zmuxd? ".to_string() }
                else { format!(" {} {}bps rx {} tx {} resent {} | C-x ? ", if self.status.up { "up" } else { "raw" }, self.status.baud, self.status.rx, self.status.tx, self.status.resent) }
            }
        };
        let rw = right.chars().count() as u16;
        let rx = (area.x + area.width).saturating_sub(rw).max(x);
        buf.set_string(rx, status_y, &right, Style::default().fg(Color::White).bg(Color::DarkGray));
        for cx in x..rx { buf[(cx, status_y)].set_style(Style::default().bg(Color::DarkGray)); }
        if self.help {
            let text = "zmux: 前置キー Ctrl+X のあとに\n\
                        |  左右に割る (新しい zosh)        -  上下に割る\n\
                        c  新しいタブ (新しい zosh)       n / p / 0-9  タブの切り替え\n\
                        k  ペインを閉じる (zosh を kill)  d  切り離す (セッションは残る)\n\
                        Ctrl+X  Ctrl+X そのものを X68000 へ   ?  この説明\n\
                        マウス: クリックでペインとタブ、ホイールでスクロールバック\n\
                        ほかのキーは全部そのまま X68000 へ。何かキーを押すと閉じる";
            let w = 66u16.min(area.width);
            let h = 9u16.min(area.height);
            let r = Rect { x: area.x + (area.width - w) / 2, y: area.y + (area.height - h) / 2, width: w, height: h };
            Clear.render(r, buf);
            Paragraph::new(text).block(Block::bordered().title(" zmux ")).render(r, buf);
            cursor = None;
        }
        if let Some(c) = cursor { f.set_cursor_position(c); }
        for p in resized {
            if let Some(pane) = self.panes.get_mut(&p) {
                if pane.sess > 0 {
                    let mut d = pane.size.0.to_be_bytes().to_vec();
                    d.extend_from_slice(&pane.size.1.to_be_bytes());
                    let _ = send_frame(&mut pane.sock, 1, &d);
                }
            }
        }
    }
}

/// 葉の値 (セッション番号) をペインの番号に置き換える
fn relabel(n: Node, map: &HashMap<usize, usize>) -> Node {
    match n {
        Node::Leaf(s) => Node::Leaf(*map.get(&s).unwrap_or(&s)),
        Node::Split(v, a, b) => Node::Split(v, Box::new(relabel(*a, map)), Box::new(relabel(*b, map))),
    }
}

fn printable(c: u8) -> String {
    if c < 0x20 { format!("Ctrl+{}", (c + 0x40) as char) } else if c < 0x7f { (c as char).to_string() } else { format!("0x{:02x}", c) }
}

fn last_line(s: &vt100::Screen) -> String {
    let (rows, cols) = s.size();
    for r in (0..rows).rev() {
        let line: String = s.rows(0, cols).nth(r as usize).unwrap_or_default();
        let t = line.trim();
        if !t.is_empty() { return t.to_string(); }
    }
    String::new()
}

fn conv_color(c: vt100::Color) -> Color {
    match c {
        vt100::Color::Default => Color::Reset,
        vt100::Color::Idx(i) => Color::Indexed(i),
        vt100::Color::Rgb(r, g, b) => Color::Rgb(r, g, b),
    }
}

/// vt100 の画面を区画に写す
fn draw_screen(s: &vt100::Screen, r: Rect, buf: &mut Buffer) {
    let (rows, cols) = s.size();
    for y in 0..rows.min(r.height) {
        let mut x = 0u16;
        while x < cols.min(r.width) {
            let Some(c) = s.cell(y, x) else { x += 1; continue };
            if c.is_wide_continuation() { x += 1; continue; }
            let mut st = Style::default().fg(conv_color(c.fgcolor())).bg(conv_color(c.bgcolor()));
            if c.bold() { st = st.add_modifier(Modifier::BOLD); }
            if c.italic() { st = st.add_modifier(Modifier::ITALIC); }
            if c.underline() { st = st.add_modifier(Modifier::UNDERLINED); }
            if c.inverse() { st = st.add_modifier(Modifier::REVERSED); }
            let txt = c.contents();
            let txt = if txt.is_empty() { " ".to_string() } else { txt };
            let w = if c.is_wide() { 2 } else { 1 };
            if x + w > r.width { break; }
            buf.set_stringn(r.x + x, r.y + y, &txt, w as usize, st);
            x += w;
        }
    }
}

/// `zmux` (引数なし)
pub fn run() {
    if unsafe { libc::isatty(0) } == 0 || unsafe { libc::isatty(1) } == 0 {
        crate::die("端末で実行する (zmux attach 0 なら端末でなくてもよい)");
    }
    if let Err(e) = query("status") {
        crate::die(&format!("{}: {} (zmuxd は動いているか)", crate::sock_path(), e));
    }
    let (tx, rx) = mpsc::channel::<Ev>();
    let mut app = App::new(tx.clone());
    if let Err(e) = app.restore() {
        crate::die(&format!("zmuxd につなげない: {}", e));
    }
    {
        let tx = tx.clone();
        thread::spawn(move || {
            let mut b = [0u8; 4096];
            let mut si = std::io::stdin();
            loop {
                match si.read(&mut b) {
                    Ok(0) | Err(_) => { let _ = tx.send(Ev::StdinClosed); return; }
                    Ok(n) => { if tx.send(Ev::Stdin(b[..n].to_vec())).is_err() { return; } }
                }
            }
        });
    }
    {
        let tx = tx.clone();
        thread::spawn(move || loop {
            let line = query("status").unwrap_or_default();
            if tx.send(Ev::Status(line)).is_err() { return; }
            thread::sleep(Duration::from_secs(1));
        });
    }
    unsafe { libc::signal(libc::SIGWINCH, on_winch as *const () as usize); }
    let _ = crossterm::terminal::enable_raw_mode();
    let mut so = std::io::stdout();
    let _ = crossterm::execute!(so, crossterm::terminal::EnterAlternateScreen);
    let _ = so.write_all(b"\x1b[22;0t"); // 今の端末のタイトルを保存 (終わりに CSI 23;0t で戻す)
    let _ = so.write_all(b"\x1b[?1000h\x1b[?1006h"); // マウス: ボタンだけ、SGR の並び
    let _ = so.flush();
    let mut term = Terminal::new(CrosstermBackend::new(std::io::stdout())).unwrap_or_else(|e| crate::die(&e.to_string()));
    let _ = term.clear();
    let mut last_title = String::new();
    let mut dirty = true;
    let mut last_draw = Instant::now();
    while !app.quit {
        let m = rx.recv_timeout(Duration::from_millis(20));
        let now = Instant::now();
        match m {
            Ok(Ev::Stdin(b)) => { let acts = app.input.feed(&b, now); app.on_actions(acts); dirty = true; }
            Ok(Ev::StdinClosed) => { app.save_layout(); break; }
            Ok(Ev::Data(id, d)) => { app.on_data(id, d); dirty = true; }
            Ok(Ev::Closed(id)) => { app.on_closed(id); dirty = true; }
            Ok(Ev::Status(l)) => { app.status = parse_status(&l); dirty = true; }
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(_) => break,
        }
        let acts = app.input.flush(now);
        if !acts.is_empty() { app.on_actions(acts); dirty = true; }
        if WINCH.swap(false, std::sync::atomic::Ordering::Relaxed) { let _ = term.autoresize(); dirty = true; }
        if app.msg.as_ref().map(|(_, t)| t.elapsed() < MSG_TIME + Duration::from_millis(100) && t.elapsed() > MSG_TIME).unwrap_or(false) { dirty = true; }
        // 描くのは 20ms に 1 回まで (zosh の出力が続くとき)。待ちは recv_timeout の 20ms なので遅れてもその程度
        if dirty && now.duration_since(last_draw) >= Duration::from_millis(20) {
            let _ = term.draw(|f| app.draw(f));
            last_draw = Instant::now();
            dirty = false;
            // 端末のタイトル (OSC 0)。変わったときだけ出す。tmux の中でも set-titles が on なら通る
            let t = app.title();
            if t != last_title {
                let _ = so.write_all(format!("\x1b]0;{}\x07", t).as_bytes());
                let _ = so.flush();
                last_title = t;
            }
        }
    }
    let _ = so.write_all(b"\x1b[?1006l\x1b[?1000l");
    let _ = so.write_all(b"\x1b]0;\x07\x1b[23;0t"); // タイトルを空にしてから、保存したタイトルに戻す (戻せない端末は空のまま)
    let _ = crossterm::execute!(so, crossterm::terminal::LeaveAlternateScreen);
    let _ = crossterm::terminal::disable_raw_mode();
    eprintln!("[zmux: 切り離した。zmux でつなぎ直せる]");
}

static WINCH: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
extern "C" fn on_winch(_: libc::c_int) { WINCH.store(true, std::sync::atomic::Ordering::Relaxed); }

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tree_split_remove_serialize() {
        let mut n = Node::Leaf(1);
        assert!(n.split(1, 2, true));
        assert!(n.split(2, 3, false));
        assert!(!n.split(9, 4, true));
        let mut l = Vec::new();
        n.leaves(&mut l);
        assert_eq!(l, vec![1, 2, 3]);
        let sess = |p: usize| p * 10;
        assert_eq!(n.serialize(&sess), "(v 10 (h 20 30))");
        let back = Node::parse("(v 10 (h 20 30))").unwrap();
        assert_eq!(back, Node::Split(true, Box::new(Node::Leaf(10)), Box::new(Node::Split(false, Box::new(Node::Leaf(20)), Box::new(Node::Leaf(30))))));
        assert_eq!(Node::parse("0"), Some(Node::Leaf(0)));
        assert_eq!(Node::parse("(v 1"), None);
        assert_eq!(Node::parse("(x 1 2)"), None);
        let n = n.remove(2).unwrap();
        assert_eq!(n, Node::Split(true, Box::new(Node::Leaf(1)), Box::new(Node::Leaf(3))));
        let n = n.remove(1).unwrap();
        assert_eq!(n, Node::Leaf(3));
        assert_eq!(n.remove(3), None);
        let mut areas = Vec::new();
        Node::parse("(v 1 (h 2 3))").unwrap().areas(Rect::new(0, 0, 80, 24), &mut areas);
        assert_eq!(areas.len(), 3);
        assert_eq!(areas[0].1, Rect::new(0, 0, 40, 24));
        assert_eq!(areas[1].1.height + areas[2].1.height, 24);
        assert_eq!(areas[1].1.x, 40);
    }

    #[test]
    fn input_prefix_and_passthrough() {
        let mut inp = Input::new(crate::keys::Map(Vec::new()));
        let t = Instant::now();
        // 普通の字と CSI u の並びはそのまま
        assert_eq!(inp.feed(b"ab\x1b[97;5u", t), vec![Action::Forward(b"ab\x1b[97;5u".to_vec())]);
        // 前置キー + 命令。前置キー 2 回は Ctrl+X そのもの
        assert_eq!(inp.feed(b"\x18|x\x18\x18y", t), vec![Action::Cmd(b'|'), Action::Forward(b"x\x18y".to_vec())]);
        // 前置キーが読みの終わりに来たら次の読みで続く
        assert_eq!(inp.feed(b"\x18", t), vec![]);
        assert_eq!(inp.feed(b"d", t), vec![Action::Cmd(b'd')]);
        // マウス (SGR): 押す / 離す / ホイール
        assert_eq!(inp.feed(b"\x1b[<0;10;5M\x1b[<0;10;5mq\x1b[<64;3;3M", t),
                   vec![Action::Mouse { btn: 0, x: 10, y: 5, press: true }, Action::Mouse { btn: 0, x: 10, y: 5, press: false },
                        Action::Forward(b"q".to_vec()), Action::Mouse { btn: 64, x: 3, y: 3, press: true }]);
        // 途中で切れたマウスの並びは持っておく
        assert_eq!(inp.feed(b"z\x1b[<0;1", t), vec![Action::Forward(b"z".to_vec())]);
        assert_eq!(inp.feed(b"2;3M", t), vec![Action::Mouse { btn: 0, x: 12, y: 3, press: true }]);
        // ESC だけは持っておき、時間切れでそのまま流す (ESC キー)
        assert_eq!(inp.feed(b"\x1b", t), vec![]);
        assert_eq!(inp.flush(t + Duration::from_millis(5)), vec![]);
        assert_eq!(inp.flush(t + Duration::from_millis(30)), vec![Action::Forward(b"\x1b".to_vec())]);
        // カーソルキーはそのまま (ESC [ A は 3 バイトそろっている)
        assert_eq!(inp.feed(b"\x1b[A", t), vec![Action::Forward(b"\x1b[A".to_vec())]);
    }

    #[test]
    fn input_keys_toml_on_console() {
        let mut inp = Input::new(crate::keys::Map::parse(crate::keys::DEFAULT_TOML));
        let t = Instant::now();
        inp.use_map = false;
        assert_eq!(inp.feed(b"\x1b[25~", t), vec![Action::Forward(b"\x1b[25~".to_vec())]);
        inp.use_map = true;
        assert_eq!(inp.feed(b"a\x1b[25~b", t), vec![Action::Forward(b"a".to_vec()), Action::Inject(0x5500), Action::Forward(b"b".to_vec())]);
        assert_eq!(inp.feed(b"\x1b[2", t), vec![]);
        assert_eq!(inp.feed(b"6~", t), vec![Action::Inject(0x5600)]);
    }

    #[test]
    fn sjis_stream() {
        let mut hold = Vec::new();
        let s = "dir 日本語\r".as_bytes();
        let out = utf8_to_sjis_stream(&s[..6], &mut hold);
        assert_eq!(out, b"dir ");
        assert_eq!(hold.len(), 2);
        let out2 = utf8_to_sjis_stream(&s[6..], &mut hold);
        assert_eq!(hold.len(), 0);
        let mut all = out.clone();
        all.extend(out2);
        assert_eq!(all, b"dir \x93\xfa\x96\x7b\x8c\xea\r".to_vec());
        assert_eq!(utf8_to_sjis_stream("😀".as_bytes(), &mut hold), b"?".to_vec());
        assert_eq!(utf8_to_sjis_stream(b"\xff\x03", &mut hold), b"\xff\x03".to_vec());
    }

    #[test]
    fn title_string() {
        assert_eq!(title_text(true, true, "19200", 0, "COMMAND.X"), "XVI · zmux 19200 · [0] COMMAND.X");
        assert_eq!(title_text(true, true, "78125", 2, "zosh 3"), "XVI · zmux 78125 · [2] zosh 3");
        assert_eq!(title_text(false, true, "19200", 0, "COMMAND.X"), "XVI · 切断中");
        assert_eq!(title_text(true, false, "19200", 0, "COMMAND.X"), "XVI · 切断中");
    }

    #[test]
    fn status_line() {
        let st = parse_status("line up rx 10 tx 20 resent 3 bad 0 ups 1 downs 0 pending 0 clients 1 xchg 0 tsp 0 accel 0 zos yes sessions [1:80x24 2:80x24] baud 19200 drive 1 rdreq 0 rdrd 0 rdwr 0");
        assert!(st.ok && st.up);
        assert_eq!((st.rx.as_str(), st.tx.as_str(), st.resent.as_str(), st.baud.as_str()), ("10", "20", "3", "19200"));
        assert!(!parse_status("").ok);
    }
}
