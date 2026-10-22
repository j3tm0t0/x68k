// DEC 特殊グリフ (ESC ( 0 / ESC ) 0 + SO/SI) の前処理。vt100 crate は文字セットの指定を扱わないので、
// 端末に渡す前に「文字として表示されるバイト」だけを見分け、0x60-0x7e を私用領域 U+F760-F77E の UTF-8 に置き換える。
// zcell::convert がそれを F_SPECIAL のセル (Z の自前罫線グリフ) にする。指定シーケンス自体と SO/SI は捨てる。
use crate::zcell::DEC_PUA;

#[derive(Clone, Copy, PartialEq)]
enum St { Ground, Esc, Desig(u8), EscInter, Csi, Str, StrEsc }

pub struct DecFilter {
    st: St,
    g: [bool; 2], // G0/G1 が DEC 特殊グリフか
    gl: usize,
}

impl DecFilter {
    pub fn new() -> Self { DecFilter { st: St::Ground, g: [false; 2], gl: 0 } }

    fn esc(&mut self, b: u8, out: &mut Vec<u8>) {
        match b {
            b'(' | b')' => {
                // ESC はもう出してあるので取り消す (指定シーケンスは端末に渡さない)
                out.pop();
                self.st = St::Desig(if b == b'(' { 0 } else { 1 });
            }
            b'[' => { self.st = St::Csi; out.push(b); }
            b']' | b'P' | b'_' | b'^' | b'X' => { self.st = St::Str; out.push(b); }
            0x20..=0x2f => { self.st = St::EscInter; out.push(b); }
            b'c' => { self.g = [false; 2]; self.gl = 0; self.st = St::Ground; out.push(b); }
            0x1b => { self.st = St::Esc; out.push(b); }
            _ => { self.st = St::Ground; out.push(b); }
        }
    }

    pub fn feed(&mut self, inp: &[u8], out: &mut Vec<u8>) {
        for &b in inp {
            match self.st {
                St::Ground => match b {
                    0x1b => { self.st = St::Esc; out.push(b); }
                    0x0e => self.gl = 1,
                    0x0f => self.gl = 0,
                    0x60..=0x7e if self.g[self.gl] => {
                        let c = char::from_u32(DEC_PUA + b as u32).unwrap();
                        let mut t = [0u8; 4];
                        out.extend_from_slice(c.encode_utf8(&mut t).as_bytes());
                    }
                    _ => out.push(b),
                },
                St::Esc => self.esc(b, out),
                St::Desig(w) => {
                    self.g[w as usize] = b == b'0';
                    self.st = St::Ground;
                }
                St::EscInter => {
                    out.push(b);
                    if !(0x20..=0x2f).contains(&b) { self.st = St::Ground; }
                }
                St::Csi => {
                    out.push(b);
                    if (0x40..=0x7e).contains(&b) || b == 0x18 || b == 0x1a { self.st = St::Ground; }
                    else if b == 0x1b { self.st = St::Esc; }
                }
                St::Str => {
                    out.push(b);
                    if b == 0x07 || b == 0x18 || b == 0x1a { self.st = St::Ground; }
                    else if b == 0x1b { self.st = St::StrEsc; }
                }
                St::StrEsc => {
                    // ESC \ (ST) で終わり。他の字なら文字列は打ち切られ、新しいエスケープの始まり
                    if b == b'\\' { out.push(b); self.st = St::Ground; } else { self.esc(b, out); }
                }
            }
        }
    }
}
