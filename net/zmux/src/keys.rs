//! X68000 のキー: 文字とキーの名前から、_B_KEYINP の値 (スキャンコード << 8 | 文字コード) を作る。
//! 端末に無い X68000 のキー (XF1〜XF5、HELP、UNDO、かな …) は ~/.config/zmux/keys.toml の対応表で、端末から来る並びに割り当てる。
//! 前置キーやホットキーは無い。対応表に無い並びは全部そのまま送る。

/// キーの名前 → スキャンコード (X68000 の JIS 配列)
pub const NAMES: &[(&str, u8)] = &[
    ("ESC", 0x01), ("BS", 0x0f), ("TAB", 0x10), ("RETURN", 0x1d), ("SPACE", 0x35),
    ("HOME", 0x36), ("DEL", 0x37), ("ROLLUP", 0x38), ("ROLLDOWN", 0x39), ("UNDO", 0x3a),
    ("LEFT", 0x3b), ("UP", 0x3c), ("RIGHT", 0x3d), ("DOWN", 0x3e), ("CLR", 0x3f),
    ("TEN/", 0x40), ("TEN*", 0x41), ("TEN-", 0x42), ("TEN7", 0x43), ("TEN8", 0x44), ("TEN9", 0x45), ("TEN+", 0x46),
    ("TEN4", 0x47), ("TEN5", 0x48), ("TEN6", 0x49), ("TEN=", 0x4a), ("TEN1", 0x4b), ("TEN2", 0x4c), ("TEN3", 0x4d),
    ("ENTER", 0x4e), ("TEN0", 0x4f), ("TEN,", 0x50), ("TEN.", 0x51),
    ("KIGOU", 0x52), ("TOUROKU", 0x53), ("HELP", 0x54),
    ("XF1", 0x55), ("XF2", 0x56), ("XF3", 0x57), ("XF4", 0x58), ("XF5", 0x59),
    ("KANA", 0x5a), ("ROMAJI", 0x5b), ("CODE", 0x5c), ("CAPS", 0x5d), ("INS", 0x5e), ("HIRAGANA", 0x5f), ("ZENKAKU", 0x60),
    ("BREAK", 0x61), ("COPY", 0x62),
    ("F1", 0x63), ("F2", 0x64), ("F3", 0x65), ("F4", 0x66), ("F5", 0x67), ("F6", 0x68), ("F7", 0x69), ("F8", 0x6a), ("F9", 0x6b), ("F10", 0x6c),
    ("OPT1", 0x72), ("OPT2", 0x73),
];

pub fn by_name(n: &str) -> Option<u8> {
    let u = n.to_ascii_uppercase();
    NAMES.iter().find(|(k, _)| *k == u).map(|(_, v)| *v)
}

/// 文字 1 つ → _B_KEYINP の値。シフトの要る記号も文字コードはそのまま (スキャンコードはキーの位置)
pub fn char_code(c: u8) -> Option<u16> {
    let row = |s: &[u8], first: u8, c: u8| s.iter().position(|&x| x == c).map(|i| first + i as u8);
    let lc = c.to_ascii_lowercase();
    let scan = match c {
        0x1b => Some(0x01),
        0x08 | 0x7f => Some(0x0f),
        b'\t' => Some(0x10),
        b'\r' | b'\n' => Some(0x1d),
        b' ' => Some(0x35),
        _ => row(b"1234567890-^\\", 0x02, c)
            .or_else(|| row(b"!\"#$%&'()", 0x02, c))
            .or_else(|| match c { b'=' => Some(0x0c), b'~' => Some(0x0d), b'|' => Some(0x0e), _ => None })
            .or_else(|| row(b"qwertyuiop@[", 0x11, lc))
            .or_else(|| match c { b'`' => Some(0x1b), b'{' => Some(0x1c), _ => None })
            .or_else(|| row(b"asdfghjkl;:]", 0x1e, lc))
            .or_else(|| match c { b'+' => Some(0x27), b'*' => Some(0x28), b'}' => Some(0x29), _ => None })
            .or_else(|| row(b"zxcvbnm,./", 0x2a, lc))
            .or_else(|| match c { b'<' => Some(0x31), b'>' => Some(0x32), b'?' => Some(0x33), b'_' => Some(0x34), _ => None })
            .or_else(|| if (1..=26).contains(&c) { char_code(c + 0x60).map(|v| (v >> 8) as u8) } else { None }),
    }?;
    let ascii = if c == b'\n' { b'\r' } else if c == 0x7f { 0x08 } else { c };
    Some(((scan as u16) << 8) | ascii as u16)
}

pub fn text_codes(t: &[u8]) -> Vec<u16> {
    t.iter().filter_map(|&c| char_code(c)).collect()
}

/// 対応表: 端末から来る並び → X68000 のキーの名前
pub struct Map(pub Vec<(Vec<u8>, u16)>);

/// 既定: 普段使われない F13〜F24 (xterm の並び) に、端末に無い X68000 のキーを割り当てる
pub const DEFAULT_TOML: &str = r#"# ~/.config/zmux/keys.toml -- 端末から来る並び = X68000 のキー
# zmux attach / zmux kbd は、この表にある並びだけを X68000 のキー入力の注入 (チャネル 1) に変える。ほかは全部そのまま送る。
# 前置キー・ホットキーは無い。並びは "\e" (ESC) と "\xNN" が書ける。キーの名前は zmux keys で一覧
[keys]
"\e[25~" = "XF1"       # F13
"\e[26~" = "XF2"       # F14
"\e[28~" = "XF3"       # F15
"\e[29~" = "XF4"       # F16
"\e[31~" = "XF5"       # F17
"\e[32~" = "OPT1"      # F18
"\e[33~" = "OPT2"      # F19
"\e[34~" = "BREAK"     # F20
"\e[1;2P" = "HELP"     # Shift+F1 (xterm) / F13 の別の並び
"\e[1;2Q" = "UNDO"     # Shift+F2
"\e[1;2R" = "KANA"     # Shift+F3
"\e[1;2S" = "COPY"     # Shift+F4
"\e[15;2~" = "CLR"     # Shift+F5
"\e[17;2~" = "ROMAJI"  # Shift+F6
"\e[18;2~" = "CODE"    # Shift+F7
"\e[19;2~" = "KIGOU"   # Shift+F8
"\e[20;2~" = "TOUROKU" # Shift+F9
"\eOM" = "ENTER"       # テンキーの ENTER (アプリケーションキーパッドのとき)
"#;

fn unescape(s: &str) -> Vec<u8> {
    let b = s.as_bytes();
    let mut out = Vec::new();
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'\\' && i + 1 < b.len() {
            match b[i + 1] {
                b'e' => { out.push(0x1b); i += 2; continue; }
                b'x' if i + 3 < b.len() => {
                    if let Ok(v) = u8::from_str_radix(std::str::from_utf8(&b[i + 2..i + 4]).unwrap_or(""), 16) {
                        out.push(v);
                        i += 4;
                        continue;
                    }
                }
                b'\\' => { out.push(b'\\'); i += 2; continue; }
                b'"' => { out.push(b'"'); i += 2; continue; }
                _ => {}
            }
        }
        out.push(b[i]);
        i += 1;
    }
    out
}

impl Map {
    /// keys.toml を読む (無ければ既定)。書式は [keys] の下の "並び" = "名前" だけ
    pub fn load() -> Map {
        let path = std::env::var("HOME").map(|h| format!("{}/.config/zmux/keys.toml", h)).unwrap_or_default();
        let text = std::fs::read_to_string(&path).unwrap_or_else(|_| DEFAULT_TOML.to_string());
        Map::parse(&text)
    }

    pub fn parse(text: &str) -> Map {
        let mut v = Vec::new();
        let mut in_keys = false;
        for line in text.lines() {
            let l = line.trim();
            if l.starts_with('[') {
                in_keys = l == "[keys]";
                continue;
            }
            if !in_keys || !l.starts_with('"') {
                continue;
            }
            // "seq" = "NAME"  # comment
            let Some(end) = l[1..].find("\" ").or_else(|| l[1..].find("\"=")).map(|e| e + 1) else { continue };
            let seq = unescape(&l[1..end]);
            let rest = l[end + 1..].trim_start().trim_start_matches('=').trim();
            let name = rest.trim_start_matches('"').split('"').next().unwrap_or("");
            if let Some(sc) = by_name(name) {
                if !seq.is_empty() {
                    v.push((seq, (sc as u16) << 8));
                }
            }
        }
        v.sort_by(|a, b| b.0.len().cmp(&a.0.len())); // 長い並びを先に
        Map(v)
    }

    /// 入力のバイト列を、そのまま送るバイトと注入するキーに分ける (並びの順番は保つ)
    pub fn split(&self, b: &[u8]) -> Vec<Result<Vec<u8>, u16>> {
        let mut out: Vec<Result<Vec<u8>, u16>> = Vec::new();
        let mut i = 0;
        let mut plain = Vec::new();
        'outer: while i < b.len() {
            if b[i] == 0x1b {
                for (seq, code) in &self.0 {
                    if b[i..].starts_with(seq) {
                        if !plain.is_empty() { out.push(Ok(std::mem::take(&mut plain))); }
                        out.push(Err(*code));
                        i += seq.len();
                        continue 'outer;
                    }
                }
            }
            plain.push(b[i]);
            i += 1;
        }
        if !plain.is_empty() { out.push(Ok(plain)); }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn codes() {
        assert_eq!(char_code(b'a'), Some(0x1e61));
        assert_eq!(char_code(b'A'), Some(0x1e41));
        assert_eq!(char_code(b'\r'), Some(0x1d0d));
        assert_eq!(char_code(b'1'), Some(0x0231));
        assert_eq!(char_code(b'!'), Some(0x0221));
        assert_eq!(char_code(b' '), Some(0x3520));
    }
    #[test]
    fn map_split() {
        let m = Map::parse(DEFAULT_TOML);
        let r = m.split(b"ab\x1b[25~c\x1b[A");
        assert_eq!(r.len(), 3);
        assert_eq!(r[0], Ok(b"ab".to_vec()));
        assert_eq!(r[1], Err(0x5500));
        assert_eq!(r[2], Ok(b"c\x1b[A".to_vec()));
    }
}
