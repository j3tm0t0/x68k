//! zmux screen : X68000 のテキスト VRAM を写して PNG か文字にする。
//!
//! X68000 (zmux.sys) にチャネル 1 の IN_SCREEN で頼み、チャネル 4 で PackBits に詰めた行が返ってくる (形は zmux.sys の scr_start)。
//! テキスト VRAM を読むので、ssh.x や Z-OS が直接描いた画面やファンクションキーの行も写る。グラフィック画面は写らない。
//! 文字にするとき (--text) は CGROM の 8x16 の半角と 16x16 の全角 (1〜8 区、16〜47 区) と照らし合わせる。
//! CGROM は初回だけ X68000 から取り (約 145KB、78125bps で 20 秒ほど)、~/.cache/zmux に置く。

use std::io::{BufRead, BufReader, Read, Write};

pub const CH_SCR: u8 = 4;
pub const IN_SCREEN: u8 = 3;

/// チャネル 1 のメッセージ: IN_SCREEN 種類 a b c d (2 バイトずつ、上位が先)
pub fn request_msg(kind: u8, a: u16, b: u16, c: u16, d: u16) -> Vec<u8> {
    let mut m = vec![IN_SCREEN, kind];
    for v in [a, b, c, d] {
        m.extend_from_slice(&v.to_be_bytes());
    }
    m
}

/// PackBits を n バイト分ほどく。足りなければ None
fn unpack(src: &[u8], pos: &mut usize, n: usize) -> Option<Vec<u8>> {
    let mut out = Vec::with_capacity(n);
    while out.len() < n {
        let c = *src.get(*pos)? as usize;
        *pos += 1;
        if c < 128 {
            let s = src.get(*pos..*pos + c + 1)?;
            out.extend_from_slice(s);
            *pos += c + 1;
        } else {
            let b = *src.get(*pos)?;
            *pos += 1;
            out.extend(std::iter::repeat(b).take(c - 126));
        }
    }
    if out.len() != n {
        return None;
    }
    Some(out)
}

pub struct Vram {
    pub mode: u8,
    pub planes: u8,
    pub w: usize,    // バイト
    pub rows: usize, // ラスタ
    pub pal: [u16; 16],
    pub plane: [Option<Vec<u8>>; 4], // w * rows バイト
}

pub fn parse_vram(b: &[u8]) -> Result<Vram, String> {
    if b.len() < 2 || b[0] == b'E' {
        return Err("X68000 が要求を断った (範囲の誤り)".into());
    }
    if b.len() < 40 || b[0] != b'V' {
        return Err(format!("応答の形が違う ({} バイト)", b.len()));
    }
    let w = b[5] as usize;
    let rows = ((b[6] as usize) << 8) | b[7] as usize;
    let mut pal = [0u16; 16];
    for (i, p) in pal.iter_mut().enumerate() {
        *p = ((b[8 + 2 * i] as u16) << 8) | b[9 + 2 * i] as u16;
    }
    let mut v = Vram { mode: b[2], planes: b[3], w, rows, pal, plane: Default::default() };
    let mut pos = 40;
    for k in 0..4 {
        if v.planes & (1 << k) == 0 {
            continue;
        }
        let mut pl = Vec::with_capacity(w * rows);
        for _ in 0..rows {
            pl.extend(unpack(b, &mut pos, w).ok_or("応答が途中で切れている")?);
        }
        v.plane[k] = Some(pl);
    }
    Ok(v)
}

pub fn parse_cgrom(b: &[u8]) -> Result<Vec<u8>, String> {
    if b.len() < 4 || b[0] != b'G' {
        return Err("CGROM の応答の形が違う".into());
    }
    let n = (((b[2] as usize) << 8) | b[3] as usize) * 256;
    let mut pos = 4;
    let mut out = Vec::with_capacity(n);
    while out.len() < n {
        out.extend(unpack(b, &mut pos, 128).ok_or("CGROM の応答が途中で切れている")?);
    }
    Ok(out)
}

impl Vram {
    fn pixel(&self, x: usize, y: usize) -> u8 {
        let mut v = 0;
        for k in 0..4 {
            if let Some(p) = &self.plane[k] {
                if p[y * self.w + x / 8] & (0x80 >> (x % 8)) != 0 {
                    v |= 1 << k;
                }
            }
        }
        v
    }

    /// 4 ビットのパレット付き PNG。色はテキストのパレット (G5 R5 B5 I1)
    pub fn png(&self) -> Vec<u8> {
        let (wpx, h) = (self.w * 8, self.rows);
        let mut raw = Vec::with_capacity((wpx / 2 + 1) * h);
        for y in 0..h {
            raw.push(0);
            for x in (0..wpx).step_by(2) {
                raw.push((self.pixel(x, y) << 4) | self.pixel(x + 1, y));
            }
        }
        let mut plte = Vec::new();
        for (i, &c) in self.pal.iter().enumerate() {
            let g = ((c >> 11) & 31) as u32;
            let r = ((c >> 6) & 31) as u32;
            let b = ((c >> 1) & 31) as u32;
            let x = |v: u32| (v * 255 / 31) as u8;
            if i == 0 { plte.extend_from_slice(&[0, 0, 0]); } else { plte.extend_from_slice(&[x(r), x(g), x(b)]); }
        }
        let mut ihdr = Vec::new();
        ihdr.extend_from_slice(&(wpx as u32).to_be_bytes());
        ihdr.extend_from_slice(&(h as u32).to_be_bytes());
        ihdr.extend_from_slice(&[4, 3, 0, 0, 0]);
        let mut out = b"\x89PNG\r\n\x1a\n".to_vec();
        chunk(&mut out, b"IHDR", &ihdr);
        chunk(&mut out, b"PLTE", &plte);
        chunk(&mut out, b"IDAT", &zlib_stored(&raw));
        chunk(&mut out, b"IEND", &[]);
        out
    }

    fn cell(&self, row: usize, col: usize, mask: u8) -> [u8; 16] {
        let mut g = [0u8; 16];
        for (s, gs) in g.iter_mut().enumerate() {
            let y = row * 16 + s;
            for k in 0..4 {
                if mask & (1 << k) != 0 {
                    if let Some(p) = &self.plane[k] {
                        *gs |= p[y * self.w + col];
                    }
                }
            }
        }
        g
    }

    /// CGROM と照らし合わせて文字にする (Shift_JIS ではなく UTF-8 の文字列)。分からない字は '?'
    pub fn text(&self, ank: &[u8], kanji: &[u8]) -> String {
        use std::collections::HashMap;
        let mut glyph: HashMap<[u8; 16], char> = HashMap::new();
        for c in (0x20u32..0x7f).chain(0xa1..0xe0) {
            let mut g = [0u8; 16];
            g.copy_from_slice(&ank[c as usize * 16..c as usize * 16 + 16]);
            let ch = if c < 0x80 { char::from_u32(c).unwrap() } else { char::from_u32(0xff61 + c - 0xa1).unwrap() };
            glyph.entry(g).or_insert(ch);
        }
        // 全角: ((区-1)*94 + (点-1))*32、16 区以降は区から 7 を引く (CGROM は 9〜15 区を飛ばして詰めてある)。左右 8 ドットずつに分けて引く
        let mut kg: HashMap<([u8; 16], [u8; 16]), char> = HashMap::new();
        for ku in (1..9).chain(16..48) {
            for ten in 1..95usize {
                let o = (((if ku < 16 { ku } else { ku - 7 }) - 1) * 94 + (ten - 1)) * 32;
                if o + 32 > kanji.len() {
                    continue;
                }
                let g = &kanji[o..o + 32];
                if g.iter().all(|&b| b == 0) {
                    continue;
                }
                let (mut l, mut r) = ([0u8; 16], [0u8; 16]);
                for s in 0..16 {
                    l[s] = g[2 * s];
                    r[s] = g[2 * s + 1];
                }
                let euc = [(ku + 0xa0) as u8, (ten + 0xa0) as u8];
                let (s, _, bad) = encoding_rs::EUC_JP.decode(&euc);
                if !bad {
                    if let Some(ch) = s.chars().next() {
                        kg.entry((l, r)).or_insert(ch);
                    }
                }
            }
        }
        let all = self.planes;
        let tries: Vec<u8> = std::iter::once(all).chain((0..4).map(|k| 1u8 << k).filter(|m| all & m != 0)).collect();
        let mut out = String::new();
        let cols = self.w;
        for row in 0..self.rows / 16 {
            let mut line = String::new();
            let mut c = 0;
            while c < cols {
                if c + 1 < cols {
                    let hit = tries.iter().find_map(|&m| kg.get(&(self.cell(row, c, m), self.cell(row, c + 1, m))));
                    if let Some(&ch) = hit {
                        line.push(ch);
                        c += 2;
                        continue;
                    }
                }
                let mut ch = ' ';
                for &m in &tries {
                    let g = self.cell(row, c, m);
                    if g.iter().all(|&b| b == 0) {
                        continue;
                    }
                    let inv = g.map(|b| !b);
                    ch = *glyph.get(&g).or_else(|| glyph.get(&inv)).unwrap_or(&'?');
                    if ch != '?' {
                        break;
                    }
                }
                line.push(ch);
                c += 1;
            }
            out.push_str(line.trim_end());
            out.push('\n');
        }
        while out.ends_with("\n\n") {
            out.pop();
        }
        out
    }
}

fn crc32(d: &[u8]) -> u32 {
    let mut c = 0xffff_ffffu32;
    for &b in d {
        c ^= b as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { (c >> 1) ^ 0xedb8_8320 } else { c >> 1 };
        }
    }
    !c
}

fn chunk(out: &mut Vec<u8>, t: &[u8; 4], d: &[u8]) {
    out.extend_from_slice(&(d.len() as u32).to_be_bytes());
    let mut c = t.to_vec();
    c.extend_from_slice(d);
    out.extend_from_slice(&c);
    out.extend_from_slice(&crc32(&c).to_be_bytes());
}

/// 圧縮しない zlib (deflate の stored ブロック)
fn zlib_stored(d: &[u8]) -> Vec<u8> {
    let mut o = vec![0x78, 0x01];
    let mut chunks = d.chunks(65535).peekable();
    if chunks.peek().is_none() {
        o.extend_from_slice(&[1, 0, 0, 0xff, 0xff]);
    }
    while let Some(c) = chunks.next() {
        o.push(if chunks.peek().is_none() { 1 } else { 0 });
        o.extend_from_slice(&(c.len() as u16).to_le_bytes());
        o.extend_from_slice(&(!(c.len() as u16)).to_le_bytes());
        o.extend_from_slice(c);
    }
    let (mut a, mut b) = (1u32, 0u32);
    for &x in d {
        a = (a + x as u32) % 65521;
        b = (b + a) % 65521;
    }
    o.extend_from_slice(&((b << 16) | a).to_be_bytes());
    o
}

/// デーモンに頼んで応答のブロックを取る ("ok N\n" + N バイト、または "err ...\n")
fn fetch(kind: u8, a: u16, b: u16, c: u16, d: u16) -> Result<Vec<u8>, String> {
    let mut s = crate::connect();
    s.write_all(format!("screen {} {} {} {} {}\n", kind, a, b, c, d).as_bytes()).map_err(|e| e.to_string())?;
    let mut r = BufReader::new(s);
    let mut line = String::new();
    r.read_line(&mut line).map_err(|e| e.to_string())?;
    let n: usize = match line.trim().strip_prefix("ok ") {
        Some(n) => n.parse().map_err(|_| line.clone())?,
        None => return Err(line.trim().to_string()),
    };
    let mut buf = vec![0u8; n];
    r.read_exact(&mut buf).map_err(|e| e.to_string())?;
    Ok(buf)
}

fn cache_dir() -> std::path::PathBuf {
    let base = std::env::var("XDG_CACHE_HOME").map(std::path::PathBuf::from)
        .unwrap_or_else(|_| std::path::PathBuf::from(std::env::var("HOME").unwrap_or_else(|_| "/tmp".into())).join(".cache"));
    base.join("zmux")
}

/// CGROM の一部 (0xF00000 + off*256 から len*256 バイト)。~/.cache/zmux に置いてあればそれを使う
fn cgrom(name: &str, off: u16, len: u16) -> Result<Vec<u8>, String> {
    let p = cache_dir().join(name);
    if let Ok(v) = std::fs::read(&p) {
        if v.len() == len as usize * 256 && v.iter().any(|&b| b != 0) {
            return Ok(v);
        }
    }
    eprintln!("zmux screen: X68000 から CGROM を取る ({} KB、初回だけ)", len as usize / 4);
    let v = parse_cgrom(&fetch(1, off, len, 0, 0)?)?;
    let _ = std::fs::create_dir_all(cache_dir());
    let _ = std::fs::write(&p, &v);
    Ok(v)
}

/// zmux screen [-o FILE.png] [-t | --text] [-p PLANES] [-y Y0] [-n ROWS]
pub fn run(args: &[String]) {
    let mut out: Option<String> = None;
    let mut rawout: Option<String> = None;
    let mut text = false;
    let mut planes = 0x3u16;
    let (mut y0, mut rows) = (0u16, 0u16);
    let mut i = 0;
    while i < args.len() {
        let v = args.get(i + 1).cloned();
        match args[i].as_str() {
            "-o" => { out = v; i += 1; }
            "-t" | "--text" => text = true,
            "-r" => { rawout = v; i += 1; }   // 試験用: ほどいたプレーンをそのまま (プレーンの順に w * rows バイトずつ)
            "-p" => {
                planes = v.as_deref().unwrap_or("").bytes().filter(|b| (b'0'..=b'3').contains(b)).fold(0, |m, b| m | 1 << (b - b'0'));
                i += 1;
            }
            "-y" => { y0 = v.and_then(|s| s.parse().ok()).unwrap_or(0); i += 1; }
            "-n" => { rows = v.and_then(|s| s.parse().ok()).unwrap_or(0); i += 1; }
            a => crate::die(&format!("screen: unknown option {} (zmux screen [-o FILE.png] [-t] [-p 0123] [-y Y0] [-n ROWS])", a)),
        }
        i += 1;
    }
    if planes == 0 {
        crate::die("screen: -p にプレーンの番号 (0〜3) が要る");
    }
    let t0 = std::time::Instant::now();
    let raw = fetch(0, planes, 0, y0, rows).unwrap_or_else(|e| crate::die(&format!("screen: {}", e)));
    let v = parse_vram(&raw).unwrap_or_else(|e| crate::die(&format!("screen: {}", e)));
    eprintln!("zmux screen: {}x{} (CRTMOD {}) {} バイトで {:.1} 秒", v.w * 8, v.rows, v.mode, raw.len(), t0.elapsed().as_secs_f64());
    if text {
        let ank = cgrom("cgrom-ank.bin", 0x3a8, 16).unwrap_or_else(|e| crate::die(&format!("screen: {}", e)));
        let kanji = cgrom("cgrom-kanji.bin", 0, 553).unwrap_or_else(|e| crate::die(&format!("screen: {}", e)));
        print!("{}", v.text(&ank, &kanji));
    }
    if let Some(f) = &rawout {
        let d: Vec<u8> = v.plane.iter().flatten().flatten().copied().collect();
        std::fs::write(f, d).unwrap_or_else(|e| crate::die(&format!("{}: {}", f, e)));
    }
    if out.is_some() || (!text && rawout.is_none()) {
        let f = out.unwrap_or_else(|| "screen.png".into());
        std::fs::write(&f, v.png()).unwrap_or_else(|e| crate::die(&format!("{}: {}", f, e)));
        eprintln!("zmux screen: {}", f);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pack(p: &[u8]) -> Vec<u8> {
        // zmux.c の packbits と同じ詰め方
        let (mut i, mut o) = (0, Vec::new());
        while i < p.len() {
            let mut r = 1;
            while i + r < p.len() && r < 129 && p[i + r] == p[i] { r += 1; }
            if r >= 2 {
                o.push((r + 126) as u8);
                o.push(p[i]);
                i += r;
            } else {
                let s = i;
                let mut l = 0;
                while i < p.len() && l < 128 && !(i + 1 < p.len() && p[i + 1] == p[i]) { i += 1; l += 1; }
                if l == 0 { i += 1; l = 1; }
                o.push((l - 1) as u8);
                o.extend_from_slice(&p[s..s + l]);
            }
        }
        o
    }

    #[test]
    fn packbits_roundtrip() {
        let rows: Vec<Vec<u8>> = vec![vec![0; 96], (0..96).map(|i| i as u8).collect(), vec![1, 2, 2, 3, 3, 3, 0, 0, 9], (0..128).map(|i| (i % 3) as u8).collect()];
        for r in rows {
            let p = pack(&r);
            let mut pos = 0;
            assert_eq!(unpack(&p, &mut pos, r.len()).unwrap(), r);
            assert_eq!(pos, p.len());
        }
    }

    #[test]
    fn png_has_signature_and_iend() {
        let mut b = vec![b'V', 0, 16, 1, 0, 2, 0, 16];
        b.extend_from_slice(&[0; 32]);
        for _ in 0..16 { b.extend_from_slice(&pack(&[0xff, 0x00])); }
        let v = parse_vram(&b).unwrap();
        let p = v.png();
        assert_eq!(&p[..8], b"\x89PNG\r\n\x1a\n");
        assert_eq!(&p[p.len() - 8..p.len() - 4], b"IEND");
    }
}
