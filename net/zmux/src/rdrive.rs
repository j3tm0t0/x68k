//! チャネル 2: リモートドライブ (Human68k のリモートドライブの要求を Pi のディレクトリで答える)。
//!
//! 要求と応答の形は yunkya2 氏の x68kserremote (MIT) の `x68kremote.h` と同じ (この repo の x68kzremotedrv の B: も同じ形)。
//! X68000 側の zrmt.sys が 1 つの要求 (cmd_*) をチャネル 2 のブロックで送り、ここが 1 つの応答 (res_*) を返す。
//! 中身は remoteserv.c (service/) を Rust に移したもの。ファイル名は Shift_JIS ⇔ UTF-8 (encoding_rs)。
//!
//! 命令 (先頭の 1 バイト。上の 3 ビットはユニット): 0x40 init, 0x41 chdir, 0x42 mkdir, 0x43 rmdir, 0x44 rename, 0x45 delete,
//! 0x46 chmod, 0x47 files, 0x48 nfiles, 0x49 create, 0x4a open, 0x4b close, 0x4c read, 0x4d write, 0x4f filedate, 0x50 dskfre。
//! read / write のデータは 1 回 DATAMAX バイトまで (zrmt.sys の CONFIG_DATASIZE と合わせる)。

use std::collections::HashMap;
use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::os::unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt};
use std::os::unix::io::AsRawFd;
use std::path::{Path, PathBuf};

pub const CH_RD: u8 = crate::link::CH_RD;
pub const DATAMAX: usize = 4084;      // zrmt.sys の CONFIG_DATASIZE (応答 2 + 4084 = 4 フレーム、要求 11 + 4084 も 4 フレーム)
const NAMEBUF: usize = 88;            // dos_namebuf: flag 1, drive 1, path 65, name1 8, ext 3, name2 10
const FILESINFO: usize = 33;          // dos_filesinfo: dummy 1, atr 1, time 2, date 2, filelen 4, name 23

// Human68k のエラー
const E_NOENT: i32 = -2;
const E_NODIR: i32 = -3;
const E_MFILE: i32 = -4;
const E_ISDIR: i32 = -5;
const E_BADF: i32 = -6;
const E_NOMEM: i32 = -8;
const E_ILGARG: i32 = -12;
const E_ILGFNAME: i32 = -13;
const E_ILGPARM: i32 = -14;
const E_ILGDRV: i32 = -15;
const E_ISCURDIR: i32 = -16;
const E_NOMORE: i32 = -18;
const E_RDONLY: i32 = -19;
const E_EXISTDIR: i32 = -20;
const E_NOTEMPTY: i32 = -21;
const E_CANTREN: i32 = -22;
const E_DISKFULL: i32 = -23;
const E_DIRFULL: i32 = -24;
const E_EXISTFILE: i32 = -80;

#[derive(Default, Debug, Clone)]
pub struct Stats {
    pub requests: u64,
    pub read_bytes: u64,
    pub write_bytes: u64,
    pub errors: u64,
}

struct Fd {
    file: File,
    pos: u64,
    unit: usize,
}

pub struct Drive {
    pub roots: Vec<PathBuf>,
    dirs: HashMap<u32, (Vec<[u8; FILESINFO]>, usize)>,   // FILBUF のアドレス → 一覧と次の位置
    files: HashMap<u32, Fd>,                              // FCB のアドレス → 開いたファイル
    pub stats: Stats,
    pub log: bool,
}

fn g16(b: &[u8]) -> u16 { u16::from_be_bytes([b[0], b[1]]) }
fn g32(b: &[u8]) -> u32 { u32::from_be_bytes([b[0], b[1], b[2], b[3]]) }

fn errno_to_dos(e: &std::io::Error) -> i32 {
    match e.raw_os_error() {
        Some(libc::ENOENT) => E_NOENT,
        Some(libc::ENOTDIR) => E_NODIR,
        Some(libc::EMFILE) => E_MFILE,
        Some(libc::EISDIR) => E_ISDIR,
        Some(libc::EBADF) => E_BADF,
        Some(libc::ENOMEM) => E_NOMEM,
        Some(libc::ENAMETOOLONG) => E_ILGFNAME,
        Some(libc::EINVAL) => E_ILGPARM,
        Some(libc::EXDEV) => E_ILGDRV,
        Some(libc::EACCES) | Some(libc::EPERM) | Some(libc::EROFS) => E_RDONLY,
        Some(libc::ENOTEMPTY) => E_NOTEMPTY,
        Some(libc::ENOSPC) => E_DISKFULL,
        Some(libc::EEXIST) => E_EXISTFILE,
        _ => E_ILGPARM,
    }
}

/// Shift_JIS → UTF-8 (変換できない並びは None)
pub fn sjis_to_utf8(b: &[u8]) -> Option<String> {
    let (s, _, bad) = encoding_rs::SHIFT_JIS.decode(b);
    if bad { None } else { Some(s.into_owned()) }
}

/// UTF-8 → Shift_JIS (変換できない字は None)
pub fn utf8_to_sjis(s: &str) -> Option<Vec<u8>> {
    let (b, _, bad) = encoding_rs::SHIFT_JIS.encode(s);
    if bad { None } else { Some(b.into_owned()) }
}

fn is_sjis_lead(c: u8) -> bool { (0x81..=0x9f).contains(&c) || (0xe0..=0xef).contains(&c) }

/// dos_namebuf → ホストのパス (remoteserv.c の conv_namebuf、元は Makoto Kamada 氏の HFS.java)。
/// full: 主ファイル名と拡張子も付ける (ディレクトリだけなら false)
fn conv_namebuf(root: &Path, ns: &[u8], full: bool) -> Option<PathBuf> {
    if ns.len() < NAMEBUF { return None; }
    let path = &ns[2..67];
    let name1 = &ns[67..75];
    let ext = &ns[75..78];
    let name2 = &ns[78..88];
    let mut bb: Vec<u8> = Vec::new();
    let mut i = 0;
    while i < 65 {
        while i < 65 && path[i] == 0x09 { i += 1; }
        if i >= 65 || path[i] == 0 { break; }
        bb.push(b'/');
        while i < 65 && path[i] != 0 && path[i] != 0x09 { bb.push(path[i]); i += 1; }
    }
    if full {
        bb.push(b'/');
        bb.extend_from_slice(name1);
        bb.extend_from_slice(name2);
        while bb.last() == Some(&0) { bb.pop(); }
        while bb.last() == Some(&b' ') { bb.pop(); }
        bb.push(b'.');
        bb.extend_from_slice(ext);
        while bb.last() == Some(&b' ') { bb.pop(); }
        while bb.last() == Some(&b'.') { bb.pop(); }
    }
    let rel = sjis_to_utf8(&bb)?;
    let rel = rel.trim_start_matches('/');
    Some(if rel.is_empty() { root.to_path_buf() } else { root.join(rel) })
}

fn dos_time_date(mtime: i64) -> (u16, u16) {
    unsafe {
        let t: libc::time_t = mtime as libc::time_t;
        let mut tm: libc::tm = std::mem::zeroed();
        if libc::localtime_r(&t, &mut tm).is_null() { return (0, 0); }
        let year = (tm.tm_year - 80).clamp(0, 127) as u16;
        let time = ((tm.tm_hour as u16) << 11) | ((tm.tm_min as u16) << 5) | ((tm.tm_sec as u16) >> 1);
        let date = (year << 9) | (((tm.tm_mon + 1) as u16) << 5) | tm.tm_mday as u16;
        (time, date)
    }
}

fn attr_of(md: &std::fs::Metadata) -> u8 {
    let mut a = if md.is_file() { 0x20 } else { 0 };
    if md.is_dir() { a |= 0x10; }
    if md.permissions().mode() & 0o200 == 0 { a |= 0x01; }
    a
}

/// dos_filesinfo (33 バイト) を作る。name は Shift_JIS、22 バイトまで
fn filesinfo(attr: u8, time: u16, date: u16, len: u32, name: &[u8]) -> [u8; FILESINFO] {
    let mut f = [0u8; FILESINFO];
    f[1] = attr;
    f[2..4].copy_from_slice(&time.to_be_bytes());
    f[4..6].copy_from_slice(&date.to_be_bytes());
    f[6..10].copy_from_slice(&len.to_be_bytes());
    let n = name.len().min(22);
    f[10..10 + n].copy_from_slice(&name[..n]);
    f
}

/// 検索の名前 (name1 8 + name2 10 + ext 3 の 21 バイト、小文字、'?' は何でも)。HFS.java の規則
fn search_pattern(ns: &[u8]) -> [u8; 21] {
    let (name1, ext, name2) = (&ns[67..75], &ns[75..78], &ns[78..88]);
    let mut w = [0u8; 21];
    w[..8].copy_from_slice(name1);
    if name1[7] == b'?' && name2[0] == 0 { w[8..18].fill(b'?'); } else { w[8..18].copy_from_slice(name2); }
    let mut i = 17;
    loop {
        if w[i] == 0 || w[i] == b' ' { w[i] = 0; } else { break; }
        if i == 0 { break; }
        i -= 1;
    }
    w[18..21].copy_from_slice(ext);
    for i in (18..21).rev() { if w[i] == b' ' { w[i] = 0; } else { break; } }
    let mut i = 0;
    while i < 21 {
        if is_sjis_lead(w[i]) { i += 2; continue; }
        w[i] = w[i].to_ascii_lowercase();
        i += 1;
    }
    w
}

/// ホストのファイル名 (Shift_JIS) を Human68k の 18 + 3 に分ける。使えない名前は None
fn split_name(b: &[u8]) -> Option<[u8; 21]> {
    let mut i = 0;
    while i < b.len() {
        let c = b[i];
        if is_sjis_lead(c) { i += 2; continue; }
        if c <= 0x1f || (c == b'-' && i == 0) || b"/\\,;<=>[]|".contains(&c) { return None; }
        i += 1;
    }
    let k = b.len();
    if k == 0 { return None; }
    let m = if b[k - 1] == b'.' { k }
        else if k >= 3 && b[k - 2] == b'.' { k - 2 }
        else if k >= 4 && b[k - 3] == b'.' { k - 3 }
        else if k >= 5 && b[k - 4] == b'.' { k - 4 }
        else { k };
    if m > 18 { return None; }
    let mut w = [0u8; 21];
    w[..m].copy_from_slice(&b[..m]);
    if m < k && b[m] == b'.' {
        let e = &b[m + 1..];
        let n = e.len().min(3);
        w[18..18 + n].copy_from_slice(&e[..n]);
    }
    Some(w)
}

/// 名前 w2 が検索の名前 w に合うか (大文字は小文字にして比べる。Shift_JIS の 2 バイト目はそのまま)
fn name_matches(w2: &[u8; 21], w: &[u8; 21]) -> bool {
    let mut f = 0x20u8;
    for i in 0..21 {
        let c = w2[i];
        let d = w[i];
        let cc = if (b'A'..=b'Z').contains(&c) { c | f } else { c };
        if d != b'?' && cc != d { return false; }
        f = if f != 0 && is_sjis_lead(c) { 0 } else { 0x20 };
    }
    true
}

impl Drive {
    pub fn new(roots: Vec<PathBuf>) -> Drive {
        Drive { roots, dirs: HashMap::new(), files: HashMap::new(), stats: Stats::default(), log: false }
    }

    fn root(&self, unit: usize) -> Option<&Path> { self.roots.get(unit).map(|p| p.as_path()) }

    fn res1(v: i32) -> Vec<u8> { vec![v as i8 as u8] }

    /// 要求 1 つに答える。知らない命令や短すぎる要求は空
    pub fn exchange(&mut self, cmd: &[u8]) -> Vec<u8> {
        if cmd.is_empty() { return Vec::new(); }
        self.stats.requests += 1;
        let unit = (cmd[0] >> 5) as usize;
        let op = (cmd[0] & 0x1f) | 0x40;
        let r = match op {
            0x40 => self.op_init(unit),
            0x41 => self.op_chdir(unit, cmd),
            0x42 => self.op_mkdir(unit, cmd),
            0x43 => self.op_rmdir(unit, cmd),
            0x44 => self.op_rename(unit, cmd),
            0x45 => self.op_delete(unit, cmd),
            0x46 => self.op_chmod(unit, cmd),
            0x47 => self.op_files(unit, cmd),
            0x48 => self.op_nfiles(cmd),
            0x49 => self.op_create(unit, cmd),
            0x4a => self.op_open(unit, cmd),
            0x4b => self.op_close(cmd),
            0x4c => self.op_read(cmd),
            0x4d => self.op_write(cmd),
            0x4f => self.op_filedate(cmd),
            0x50 => self.op_dskfre(unit),
            _ => Vec::new(),
        };
        if self.log {
            eprintln!("rd: op {:02x} unit {} req {} -> res {} {:?}", op, unit, cmd.len(), r.len(), &r[..r.len().min(8)]);
        }
        r
    }

    fn op_init(&mut self, unit: usize) -> Vec<u8> {
        self.dirs.clear();
        self.files.retain(|_, f| f.unit != unit);
        Self::res1(0)
    }

    fn path_of(&self, unit: usize, ns: &[u8], full: bool) -> Option<PathBuf> {
        conv_namebuf(self.root(unit)?, ns, full)
    }

    fn op_chdir(&mut self, unit: usize, cmd: &[u8]) -> Vec<u8> {
        if cmd.len() < 1 + NAMEBUF { return Self::res1(E_NODIR); }
        let Some(p) = self.path_of(unit, &cmd[1..], false) else { return Self::res1(E_NODIR) };
        Self::res1(match std::fs::metadata(&p) { Ok(m) if m.is_dir() => 0, _ => E_NODIR })
    }

    fn op_mkdir(&mut self, unit: usize, cmd: &[u8]) -> Vec<u8> {
        if cmd.len() < 1 + NAMEBUF { return Self::res1(E_NODIR); }
        let Some(p) = self.path_of(unit, &cmd[1..], true) else { return Self::res1(E_NODIR) };
        Self::res1(match std::fs::create_dir(&p) {
            Ok(()) => 0,
            Err(e) if e.raw_os_error() == Some(libc::EEXIST) => E_EXISTDIR,
            Err(e) => { self.stats.errors += 1; errno_to_dos(&e) }
        })
    }

    fn op_rmdir(&mut self, unit: usize, cmd: &[u8]) -> Vec<u8> {
        if cmd.len() < 1 + NAMEBUF { return Self::res1(E_NODIR); }
        let Some(p) = self.path_of(unit, &cmd[1..], true) else { return Self::res1(E_NODIR) };
        Self::res1(match std::fs::remove_dir(&p) {
            Ok(()) => 0,
            Err(e) if e.raw_os_error() == Some(libc::EINVAL) => E_ISCURDIR,
            Err(e) => { self.stats.errors += 1; errno_to_dos(&e) }
        })
    }

    fn op_rename(&mut self, unit: usize, cmd: &[u8]) -> Vec<u8> {
        if cmd.len() < 1 + 2 * NAMEBUF { return Self::res1(E_NODIR); }
        let Some(a) = self.path_of(unit, &cmd[1..], true) else { return Self::res1(E_NODIR) };
        let Some(b) = self.path_of(unit, &cmd[1 + NAMEBUF..], true) else { return Self::res1(E_NODIR) };
        Self::res1(match std::fs::rename(&a, &b) {
            Ok(()) => 0,
            Err(e) if e.raw_os_error() == Some(libc::ENOTEMPTY) => E_CANTREN,
            Err(e) => { self.stats.errors += 1; errno_to_dos(&e) }
        })
    }

    fn op_delete(&mut self, unit: usize, cmd: &[u8]) -> Vec<u8> {
        if cmd.len() < 1 + NAMEBUF { return Self::res1(E_NODIR); }
        let Some(p) = self.path_of(unit, &cmd[1..], true) else { return Self::res1(E_NODIR) };
        Self::res1(match std::fs::remove_file(&p) { Ok(()) => 0, Err(e) => { self.stats.errors += 1; errno_to_dos(&e) } })
    }

    fn op_chmod(&mut self, unit: usize, cmd: &[u8]) -> Vec<u8> {
        if cmd.len() < 2 + NAMEBUF { return Self::res1(E_NODIR); }
        let attr = cmd[1];
        let Some(p) = self.path_of(unit, &cmd[2..], true) else { return Self::res1(E_NODIR) };
        let md = match std::fs::metadata(&p) { Ok(m) => m, Err(e) => return Self::res1(errno_to_dos(&e)) };
        if attr == 0xff { return Self::res1(attr_of(&md) as i32); }
        let mut mode = md.permissions().mode();
        if attr & 1 != 0 { mode &= !0o222; } else { mode |= 0o222; }
        Self::res1(match std::fs::set_permissions(&p, std::fs::Permissions::from_mode(mode)) { Ok(()) => 0, Err(e) => errno_to_dos(&e) })
    }

    /// files: 一覧を作って最初の 1 つを返す (応答: res 1 + dos_filesinfo 33)
    fn op_files(&mut self, unit: usize, cmd: &[u8]) -> Vec<u8> {
        let mut res = vec![E_NOMORE as i8 as u8];
        res.resize(1 + FILESINFO, 0);
        if cmd.len() < 6 + NAMEBUF { res[0] = E_NODIR as i8 as u8; return res; }
        let attr = cmd[1];
        let filep = g32(&cmd[2..]);
        let ns = &cmd[6..6 + NAMEBUF];
        self.dirs.remove(&filep);
        let Some(dir) = self.path_of(unit, ns, false) else { res[0] = E_NODIR as i8 as u8; return res };
        let isroot = ns[2] == 0x09 && ns[3] == 0;
        let w = search_pattern(ns);
        let rd = match std::fs::read_dir(&dir) {
            Ok(r) => r,
            Err(e) => {
                res[0] = (if e.raw_os_error() == Some(libc::ENOENT) { E_NODIR } else { errno_to_dos(&e) }) as i8 as u8;
                return res;
            }
        };
        let mut list: Vec<[u8; FILESINFO]> = Vec::new();
        if isroot && attr & 0x08 != 0 && w[0] == b'?' && w[18] == b'?' {
            // ボリューム名 (ディレクトリの名前)
            let vol = dir.file_name().map(|s| s.to_string_lossy().to_string()).unwrap_or_default();
            let vb = utf8_to_sjis(&vol).unwrap_or_default();
            list.push(filesinfo(0x08, 0, 0, 0, &vb[..vb.len().min(21)]));
        }
        let mut names: Vec<(Vec<u8>, std::fs::Metadata)> = Vec::new();
        for ent in rd.flatten() {
            let name = ent.file_name().to_string_lossy().to_string();
            if name == "." || name == ".." { continue; }
            let Some(sj) = utf8_to_sjis(&name) else { continue };
            if sj.len() > 22 { continue; }
            let Some(w2) = split_name(&sj) else { continue };
            if !name_matches(&w2, &w) { continue; }
            let Ok(md) = std::fs::metadata(ent.path()) else { continue };
            if md.len() > 0xffff_ffff { continue; }
            if attr_of(&md) & attr == 0 { continue; }
            names.push((sj, md));
        }
        names.sort_by(|a, b| a.0.cmp(&b.0));
        for (sj, md) in names {
            let (t, d) = dos_time_date(md.mtime());
            list.push(filesinfo(attr_of(&md), t, d, md.len() as u32, &sj));
        }
        if !list.is_empty() {
            res[0] = 0;
            res[1..].copy_from_slice(&list[0]);
            if list.len() > 1 { self.dirs.insert(filep, (list, 1)); }
        }
        res
    }

    fn op_nfiles(&mut self, cmd: &[u8]) -> Vec<u8> {
        let mut res = vec![E_NOMORE as i8 as u8];
        res.resize(1 + FILESINFO, 0);
        if cmd.len() < 5 { return res; }
        let filep = g32(&cmd[1..]);
        if let Some((list, idx)) = self.dirs.get_mut(&filep) {
            res[0] = 0;
            res[1..].copy_from_slice(&list[*idx]);
            *idx += 1;
            if *idx >= list.len() { self.dirs.remove(&filep); }
        }
        res
    }

    fn op_create(&mut self, unit: usize, cmd: &[u8]) -> Vec<u8> {
        if cmd.len() < 7 + NAMEBUF { return Self::res1(E_NODIR); }
        let mode = cmd[2];
        let fcb = g32(&cmd[3..]);
        let Some(p) = self.path_of(unit, &cmd[7..], true) else { return Self::res1(E_NODIR) };
        let mut o = OpenOptions::new();
        o.read(true).write(true).truncate(true).mode(0o666);
        if mode == 0 { o.create_new(true); } else { o.create(true); }
        match o.open(&p) {
            Ok(f) => { self.files.insert(fcb, Fd { file: f, pos: 0, unit }); Self::res1(0) }
            Err(e) if e.raw_os_error() == Some(libc::ENOSPC) => Self::res1(E_DIRFULL),
            Err(e) => { self.stats.errors += 1; Self::res1(errno_to_dos(&e)) }
        }
    }

    fn op_open(&mut self, unit: usize, cmd: &[u8]) -> Vec<u8> {
        let mut res = vec![0u8; 5];
        if cmd.len() < 6 + NAMEBUF { res[0] = E_NODIR as i8 as u8; return res; }
        let mode = cmd[1];
        let fcb = g32(&cmd[2..]);
        let Some(p) = self.path_of(unit, &cmd[6..], true) else { res[0] = E_NODIR as i8 as u8; return res };
        let mut o = OpenOptions::new();
        match mode { 0 => { o.read(true); } 1 => { o.write(true); } 2 => { o.read(true).write(true); } _ => { res[0] = E_ILGARG as i8 as u8; return res; } }
        match o.open(&p) {
            Ok(f) => {
                let len = f.metadata().map(|m| m.len()).unwrap_or(0) as u32;
                res[1..5].copy_from_slice(&len.to_be_bytes());
                self.files.insert(fcb, Fd { file: f, pos: 0, unit });
            }
            Err(e) if e.raw_os_error() == Some(libc::EINVAL) => res[0] = E_ILGARG as i8 as u8,
            Err(e) => { self.stats.errors += 1; res[0] = errno_to_dos(&e) as i8 as u8; }
        }
        res
    }

    fn op_close(&mut self, cmd: &[u8]) -> Vec<u8> {
        if cmd.len() < 5 { return Self::res1(E_BADF); }
        let fcb = g32(&cmd[1..]);
        match self.files.remove(&fcb) { Some(_) => Self::res1(0), None => Self::res1(E_BADF) }
    }

    fn op_read(&mut self, cmd: &[u8]) -> Vec<u8> {
        if cmd.len() < 11 { return (E_BADF as i16).to_be_bytes().to_vec(); }
        let fcb = g32(&cmd[1..]);
        let pos = g32(&cmd[5..]) as u64;
        let len = (g16(&cmd[9..]) as usize).min(DATAMAX);
        let Some(fd) = self.files.get_mut(&fcb) else { return (E_BADF as i16).to_be_bytes().to_vec() };
        if fd.pos != pos {
            if let Err(e) = fd.file.seek(SeekFrom::Start(pos)) { return (errno_to_dos(&e) as i16).to_be_bytes().to_vec(); }
            fd.pos = pos;
        }
        let mut res = vec![0u8; 2 + len];
        let mut got = 0;
        while got < len {
            match fd.file.read(&mut res[2 + got..]) {
                Ok(0) => break,
                Ok(n) => got += n,
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
                Err(e) => { if got == 0 { return (errno_to_dos(&e) as i16).to_be_bytes().to_vec(); } break; }
            }
        }
        fd.pos += got as u64;
        self.stats.read_bytes += got as u64;
        res.truncate(2 + got);
        res[..2].copy_from_slice(&(got as i16).to_be_bytes());
        res
    }

    fn op_write(&mut self, cmd: &[u8]) -> Vec<u8> {
        if cmd.len() < 11 { return (E_BADF as i16).to_be_bytes().to_vec(); }
        let fcb = g32(&cmd[1..]);
        let pos = g32(&cmd[5..]) as u64;
        let len = (g16(&cmd[9..]) as usize).min(cmd.len() - 11);
        let Some(fd) = self.files.get_mut(&fcb) else { return (E_BADF as i16).to_be_bytes().to_vec() };
        if len == 0 {
            // 0 バイトの書き込みはファイルをその位置で切る
            return match fd.file.set_len(pos) { Ok(()) => 0i16, Err(e) => errno_to_dos(&e) as i16 }.to_be_bytes().to_vec();
        }
        if fd.pos != pos {
            if let Err(e) = fd.file.seek(SeekFrom::Start(pos)) { return (errno_to_dos(&e) as i16).to_be_bytes().to_vec(); }
            fd.pos = pos;
        }
        match fd.file.write_all(&cmd[11..11 + len]) {
            Ok(()) => { fd.pos += len as u64; self.stats.write_bytes += len as u64; (len as i16).to_be_bytes().to_vec() }
            Err(e) => { self.stats.errors += 1; (errno_to_dos(&e) as i16).to_be_bytes().to_vec() }
        }
    }

    fn op_filedate(&mut self, cmd: &[u8]) -> Vec<u8> {
        if cmd.len() < 9 { return [(E_BADF as i16).to_be_bytes(), 0xffffu16.to_be_bytes()].concat(); }
        let fcb = g32(&cmd[1..]);
        let time = g16(&cmd[5..]);
        let date = g16(&cmd[7..]);
        let Some(fd) = self.files.get(&fcb) else { return [(E_BADF as i16).to_be_bytes(), 0xffffu16.to_be_bytes()].concat() };
        if time == 0 && date == 0 {
            match fd.file.metadata() {
                Ok(md) => { let (t, d) = dos_time_date(md.mtime()); [t.to_be_bytes(), d.to_be_bytes()].concat() }
                Err(e) => [(errno_to_dos(&e) as i16).to_be_bytes(), 0xffffu16.to_be_bytes()].concat(),
            }
        } else {
            unsafe {
                let mut tm: libc::tm = std::mem::zeroed();
                tm.tm_sec = ((time << 1) & 0x3f) as i32;
                tm.tm_min = ((time >> 5) & 0x3f) as i32;
                tm.tm_hour = ((time >> 11) & 0x1f) as i32;
                tm.tm_mday = (date & 0x1f) as i32;
                tm.tm_mon = (((date >> 5) & 0xf) as i32 - 1).max(0);
                tm.tm_year = ((date >> 9) & 0x7f) as i32 + 80;
                tm.tm_isdst = -1;
                let tt = libc::mktime(&mut tm);
                let ts = [libc::timespec { tv_sec: tt, tv_nsec: 0 }, libc::timespec { tv_sec: tt, tv_nsec: 0 }];
                libc::futimens(fd.file.as_raw_fd(), ts.as_ptr());
            }
            vec![0, 0, 0, 0]
        }
    }

    fn op_dskfre(&mut self, unit: usize) -> Vec<u8> {
        let mut res = vec![0u8; 12];
        let Some(root) = self.root(unit) else { return res };
        let (total, free) = unsafe {
            let c = std::ffi::CString::new(root.to_string_lossy().as_bytes()).unwrap_or_default();
            let mut sf: libc::statvfs = std::mem::zeroed();
            if libc::statvfs(c.as_ptr(), &mut sf) == 0 {
                (sf.f_blocks as u64 * sf.f_frsize as u64, sf.f_bavail as u64 * sf.f_frsize as u64)
            } else { (0, 0) }
        };
        let total = total.min(0x7fff_ffff);
        let free = free.min(0x7fff_ffff);
        res[0..4].copy_from_slice(&(free as u32).to_be_bytes());
        res[4..6].copy_from_slice(&((free / 32768) as u16).to_be_bytes());
        res[6..8].copy_from_slice(&((total / 32768) as u16).to_be_bytes());
        res[8..10].copy_from_slice(&128u16.to_be_bytes());
        res[10..12].copy_from_slice(&1024u16.to_be_bytes());
        res
    }
}

#[cfg(test)]
pub mod tests {
    use super::*;

    /// dos_namebuf を作る: dir は "\\SUB\\DIR" の形 (区切りは 0x09 にする)、name は "FILE.EXT" (18 + 3 まで)
    pub fn namebuf(dir: &str, name: &str) -> Vec<u8> {
        let mut b = vec![0u8; NAMEBUF];
        let mut p = Vec::new();
        p.push(0x09);
        for seg in dir.split('\\').filter(|s| !s.is_empty()) {
            p.extend_from_slice(seg.as_bytes());
            p.push(0x09);
        }
        b[2..2 + p.len()].copy_from_slice(&p);
        let (n, e) = name.rsplit_once('.').unwrap_or((name, ""));
        let nb = n.as_bytes();
        let n1 = &nb[..nb.len().min(8)];
        b[67..67 + n1.len()].copy_from_slice(n1);
        for i in n1.len()..8 { b[67 + i] = b' '; }
        if nb.len() > 8 { let n2 = &nb[8..nb.len().min(18)]; b[78..78 + n2.len()].copy_from_slice(n2); }
        let eb = e.as_bytes();
        b[75..75 + eb.len().min(3)].copy_from_slice(&eb[..eb.len().min(3)]);
        for i in eb.len().min(3)..3 { b[75 + i] = b' '; }
        b
    }

    pub fn cmd_dirop(op: u8, nb: &[u8]) -> Vec<u8> { let mut c = vec![op]; c.extend_from_slice(nb); c }
    pub fn cmd_files(attr: u8, filep: u32, nb: &[u8]) -> Vec<u8> {
        let mut c = vec![0x07, attr]; c.extend_from_slice(&filep.to_be_bytes()); c.extend_from_slice(nb); c
    }
    pub fn cmd_create(fcb: u32, mode: u8, nb: &[u8]) -> Vec<u8> {
        let mut c = vec![0x09, 0x20, mode]; c.extend_from_slice(&fcb.to_be_bytes()); c.extend_from_slice(nb); c
    }
    pub fn cmd_open(fcb: u32, mode: u8, nb: &[u8]) -> Vec<u8> {
        let mut c = vec![0x0a, mode]; c.extend_from_slice(&fcb.to_be_bytes()); c.extend_from_slice(nb); c
    }
    pub fn cmd_read(fcb: u32, pos: u32, len: u16) -> Vec<u8> {
        let mut c = vec![0x0c]; c.extend_from_slice(&fcb.to_be_bytes()); c.extend_from_slice(&pos.to_be_bytes()); c.extend_from_slice(&len.to_be_bytes()); c
    }
    pub fn cmd_write(fcb: u32, pos: u32, d: &[u8]) -> Vec<u8> {
        let mut c = vec![0x0d]; c.extend_from_slice(&fcb.to_be_bytes()); c.extend_from_slice(&pos.to_be_bytes());
        c.extend_from_slice(&(d.len() as u16).to_be_bytes()); c.extend_from_slice(d); c
    }
    pub fn cmd_close(fcb: u32) -> Vec<u8> { let mut c = vec![0x0b]; c.extend_from_slice(&fcb.to_be_bytes()); c }

    fn tmpdir(tag: &str) -> PathBuf {
        let p = std::env::temp_dir().join(format!("zmux-rd-{}-{}", tag, std::process::id()));
        let _ = std::fs::remove_dir_all(&p);
        std::fs::create_dir_all(&p).unwrap();
        p
    }

    fn name_of(info: &[u8]) -> String {
        let n = &info[10..];
        let end = n.iter().position(|&c| c == 0).unwrap_or(n.len());
        String::from_utf8_lossy(&n[..end]).to_string()
    }

    #[test]
    fn namebuf_to_path() {
        let root = Path::new("/r");
        assert_eq!(conv_namebuf(root, &namebuf("", "AUTOEXEC.BAT"), true).unwrap(), PathBuf::from("/r/AUTOEXEC.BAT"));
        assert_eq!(conv_namebuf(root, &namebuf("\\SYS", "RSDRV.SYS"), true).unwrap(), PathBuf::from("/r/SYS/RSDRV.SYS"));
        assert_eq!(conv_namebuf(root, &namebuf("\\SYS", ""), false).unwrap(), PathBuf::from("/r/SYS"));
        assert_eq!(conv_namebuf(root, &namebuf("", ""), false).unwrap(), PathBuf::from("/r"));
        assert_eq!(conv_namebuf(root, &namebuf("", "LONGFILENAME18CHARS.TXT"), true).unwrap(), PathBuf::from("/r/LONGFILENAME18CHAR.TXT"));
    }

    #[test]
    fn wildcard() {
        let w = search_pattern(&namebuf("", "????????.???"));
        assert!(name_matches(&split_name(b"README.MD").unwrap(), &w));
        assert!(name_matches(&split_name(b"a").unwrap(), &w));
        let w = search_pattern(&namebuf("", "*.TXT").iter().map(|&c| if c == b'*' { b'?' } else { c }).collect::<Vec<u8>>());
        assert!(!name_matches(&split_name(b"README.MD").unwrap(), &w));
        let w = search_pattern(&namebuf("", "ZMUX????.SYS"));
        assert!(name_matches(&split_name(b"zmux.sys").unwrap(), &w));
        assert!(name_matches(&split_name(b"ZMUX.SYS").unwrap(), &w));
        assert!(!name_matches(&split_name(b"ZMUX.X").unwrap(), &w));
        assert!(split_name(b"a,b").is_none());
        assert!(split_name(b"-x").is_none());
        assert!(split_name(b"averyveryverylongname.txt").is_none());
    }

    #[test]
    fn sjis_roundtrip() {
        let s = "日本語.TXT";
        let b = utf8_to_sjis(s).unwrap();
        assert_eq!(b.len(), 10);
        assert_eq!(sjis_to_utf8(&b).unwrap(), s);
    }

    #[test]
    fn file_ops() {
        let root = tmpdir("ops");
        let mut d = Drive::new(vec![root.clone()]);
        assert_eq!(d.exchange(&[0x00]), vec![0]);
        // mkdir / chdir
        assert_eq!(d.exchange(&cmd_dirop(0x02, &namebuf("", "SUB"))), vec![0]);
        assert!(root.join("SUB").is_dir());
        assert_eq!(d.exchange(&cmd_dirop(0x02, &namebuf("", "SUB"))), vec![E_EXISTDIR as i8 as u8]);
        assert_eq!(d.exchange(&cmd_dirop(0x01, &namebuf("\\SUB", ""))), vec![0]);
        assert_eq!(d.exchange(&cmd_dirop(0x01, &namebuf("\\NOPE", ""))), vec![E_NODIR as i8 as u8]);
        // create + write + close
        let data: Vec<u8> = (0..10000u32).map(|i| (i * 13 % 251) as u8).collect();
        assert_eq!(d.exchange(&cmd_create(0x1000, 0, &namebuf("\\SUB", "DATA.BIN"))), vec![0]);
        let mut pos = 0;
        while pos < data.len() {
            let n = (data.len() - pos).min(DATAMAX);
            let r = d.exchange(&cmd_write(0x1000, pos as u32, &data[pos..pos + n]));
            assert_eq!(i16::from_be_bytes([r[0], r[1]]) as usize, n);
            pos += n;
        }
        assert_eq!(d.exchange(&cmd_close(0x1000)), vec![0]);
        assert_eq!(std::fs::read(root.join("SUB/DATA.BIN")).unwrap(), data);
        assert_eq!(d.exchange(&cmd_create(0x1000, 0, &namebuf("\\SUB", "DATA.BIN"))), vec![E_EXISTFILE as i8 as u8]);
        // open + read back (in 1000-byte pieces, then one big one)
        let r = d.exchange(&cmd_open(0x2000, 0, &namebuf("\\SUB", "DATA.BIN")));
        assert_eq!(r[0], 0);
        assert_eq!(g32(&r[1..]), 10000);
        let mut got = Vec::new();
        while got.len() < 10000 {
            let r = d.exchange(&cmd_read(0x2000, got.len() as u32, 1000));
            let n = i16::from_be_bytes([r[0], r[1]]) as usize;
            assert_eq!(r.len(), 2 + n);
            got.extend_from_slice(&r[2..]);
        }
        assert_eq!(got, data);
        let r = d.exchange(&cmd_read(0x2000, 10000, 1000));
        assert_eq!(r, vec![0, 0]);
        let r = d.exchange(&cmd_read(0x2000, 0, DATAMAX as u16));
        assert_eq!(r.len(), 2 + DATAMAX);
        assert_eq!(&r[2..], &data[..DATAMAX]);
        assert_eq!(d.exchange(&cmd_close(0x2000)), vec![0]);
        assert_eq!(d.exchange(&cmd_read(0x2000, 0, 10)), (E_BADF as i16).to_be_bytes().to_vec());
        // files / nfiles over SUB (*.*), volume label on the root
        std::fs::write(root.join("SUB/B.TXT"), b"x").unwrap();
        std::fs::write(root.join("SUB/ignored,name"), b"x").unwrap();
        let r = d.exchange(&cmd_files(0x37, 0x5000, &namebuf("\\SUB", "????????.???")));
        assert_eq!(r[0], 0);
        assert_eq!(name_of(&r[1..]), "B.TXT");
        assert_eq!(r[2], 0x20);
        assert_eq!(g32(&r[7..]), 1);
        let r = d.exchange(&[0x08, 0, 0, 0x50, 0]);
        assert_eq!(r[0], 0);
        assert_eq!(name_of(&r[1..]), "DATA.BIN");
        assert_eq!(g32(&r[7..]), 10000);
        let r = d.exchange(&[0x08, 0, 0, 0x50, 0]);
        assert_eq!(r[0], E_NOMORE as i8 as u8);
        let r = d.exchange(&cmd_files(0x3f, 0x5001, &namebuf("", "????????.???")));
        assert_eq!(r[0], 0);
        assert_eq!(r[2], 0x08, "volume label first");
        let r = d.exchange(&[0x08, 0, 0, 0x50, 1]);
        assert_eq!(name_of(&r[1..]), "SUB");
        assert_eq!(r[2], 0x10);
        // only directories
        let r = d.exchange(&cmd_files(0x10, 0x5002, &namebuf("\\SUB", "????????.???")));
        assert_eq!(r[0], E_NOMORE as i8 as u8);
        // rename / delete / rmdir
        assert_eq!(d.exchange(&{ let mut c = vec![0x04]; c.extend_from_slice(&namebuf("\\SUB", "B.TXT")); c.extend_from_slice(&namebuf("\\SUB", "C.TXT")); c }), vec![0]);
        assert!(root.join("SUB/C.TXT").is_file());
        assert_eq!(d.exchange(&cmd_dirop(0x05, &namebuf("\\SUB", "C.TXT"))), vec![0]);
        assert_eq!(d.exchange(&cmd_dirop(0x05, &namebuf("\\SUB", "C.TXT"))), vec![E_NOENT as i8 as u8]);
        assert_eq!(d.exchange(&cmd_dirop(0x03, &namebuf("", "SUB"))), vec![E_NOTEMPTY as i8 as u8]);
        std::fs::remove_file(root.join("SUB/DATA.BIN")).unwrap();
        std::fs::remove_file(root.join("SUB/ignored,name")).unwrap();
        assert_eq!(d.exchange(&cmd_dirop(0x03, &namebuf("", "SUB"))), vec![0]);
        // chmod: read-only flag
        std::fs::write(root.join("RO.TXT"), b"x").unwrap();
        let mut c = vec![0x06, 0x01]; c.extend_from_slice(&namebuf("", "RO.TXT"));
        assert_eq!(d.exchange(&c), vec![0]);
        let mut c = vec![0x06, 0xff]; c.extend_from_slice(&namebuf("", "RO.TXT"));
        assert_eq!(d.exchange(&c), vec![0x21]);
        // dskfre
        let r = d.exchange(&[0x10]);
        assert_eq!(r.len(), 12);
        assert_eq!(g16(&r[8..]), 128);
        assert_eq!(g16(&r[10..]), 1024);
        // truncate by a 0-byte write, filedate
        assert_eq!(d.exchange(&cmd_create(0x3000, 1, &namebuf("", "T.BIN"))), vec![0]);
        assert_eq!(i16::from_be_bytes([d.exchange(&cmd_write(0x3000, 0, &[1u8; 100]))[0], 0]) >= 0, true);
        assert_eq!(d.exchange(&cmd_write(0x3000, 40, &[])), vec![0, 0]);
        assert_eq!(std::fs::metadata(root.join("T.BIN")).unwrap().len(), 40);
        let mut c = vec![0x0f]; c.extend_from_slice(&0x3000u32.to_be_bytes()); c.extend_from_slice(&0x6000u16.to_be_bytes()); c.extend_from_slice(&0x5d59u16.to_be_bytes()); // 12:00:00 2026-10-25
        assert_eq!(d.exchange(&c), vec![0, 0, 0, 0]);
        let mut c = vec![0x0f]; c.extend_from_slice(&0x3000u32.to_be_bytes()); c.extend_from_slice(&[0, 0, 0, 0]);
        let r = d.exchange(&c);
        assert_eq!((g16(&r[0..]), g16(&r[2..])), (0x6000, 0x5d59));
        assert_eq!(d.exchange(&cmd_close(0x3000)), vec![0]);
        assert_eq!(d.exchange(&[0x7f]), Vec::<u8>::new());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn japanese_names() {
        let root = tmpdir("jp");
        let mut d = Drive::new(vec![root.clone()]);
        std::fs::write(root.join("日本語.TXT"), b"abc").unwrap();
        let mut nb = namebuf("", "");
        let sj = utf8_to_sjis("日本語").unwrap();
        nb[67..67 + sj.len()].copy_from_slice(&sj);
        for i in sj.len()..8 { nb[67 + i] = b' '; }
        nb[75..78].copy_from_slice(b"TXT");
        let r = d.exchange(&cmd_open(0x10, 0, &nb));
        assert_eq!(r[0], 0);
        assert_eq!(g32(&r[1..]), 3);
        let r = d.exchange(&cmd_files(0x37, 0x50, &namebuf("", "????????.???")));
        assert_eq!(r[0], 0);
        assert_eq!(sjis_to_utf8(&r[11..11 + sj.len() + 4]).unwrap(), "日本語.TXT");
        let _ = std::fs::remove_dir_all(&root);
    }
}
