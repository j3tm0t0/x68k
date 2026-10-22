// zterm の通信路 (サーバ -> Z)。多バイトの値はビッグエンディアン (68000 がそのまま読める)。
//
// フレーム: 'Z' (0x5a), 長さ u16, 本体。本体は命令の並び。フレームの頭で Z 側の
// 書き込み位置は (0,0)、属性は (fg=7,bg=0,fl=0) に戻る。0x20-0x7e のバイトはそのまま
// 「その ASCII 字を現在の属性で置き、1 桁進む」。他は命令 (README.md の表)。
// Z は 1 フレームを描き終えるたびに ACK を返し、サーバは ACK の無いフレームを
// 一定数 (既定 1) より多く送らない (遅い Z に古い画面を溜めない。mosh と同じ考え)。
use crate::zcell::{ZCell, F_EMOJI, F_SPECIAL, F_UNDERLINE, F_WIDE, F_WIDECONT};

pub const MAGIC: u8 = b'Z';
pub const VERSION: u8 = 1;

pub const OP_HELLO: u8 = 0x01; // ver cols rows
pub const OP_POS: u8 = 0x02; // row col
pub const OP_ATTR: u8 = 0x03; // fg<<4|bg, fl (F_UNDERLINE|F_SPECIAL)
pub const OP_WIDE: u8 = 0x04; // cp(u16): 全角 (左 F_WIDE + 右 F_WIDECONT)、2 桁進む
pub const OP_EMOJI: u8 = 0x05; // idx(u8): 絵文字 (F_WIDE|F_EMOJI + 右)、2 桁進む
pub const OP_CP16: u8 = 0x06; // cp(u16): 半角の非 ASCII (半角カナ・ブロック要素・¥・‾)
pub const OP_REP: u8 = 0x07; // n ch: 同じ ASCII を n 個
pub const OP_SCROLL: u8 = 0x08; // top bot n(i8): 行 [top,bot] を n 行上へ (負なら下へ)。空いた行は空白 (7,0)
pub const OP_CURSOR: u8 = 0x09; // x y flags(bit0=表示)
pub const OP_MODE: u8 = 0x0a; // flags: bit0 DECCKM, bit1 代替画面, bit2 DECKPAM, bit3 bracketed paste
pub const OP_BELL: u8 = 0x0b;
pub const OP_CLEAR: u8 = 0x0c; // bg: 画面全体を空白 (7,bg) に
pub const OP_BYE: u8 = 0x0d; // code: シェルが終わった
pub const OP_SIZE: u8 = 0x0e; // cols rows: 画面の寸法が変わった (CLEAR 相当)
pub const OP_ECHOACK: u8 = 0x0f; // n(u32): Z から受けたキーの先頭 n バイトは、pty に書いてから ECHO_DELAY 以上経った後の画面 (Z の予測エコーの照合用)

// Z -> サーバ: type(u8) len(u8) 本体
pub const C_KEYS: u8 = b'k';
pub const C_ACK: u8 = b'a';
pub const C_RESIZE: u8 = b'r';
pub const C_REFRESH: u8 = b'f';
pub const C_QUIT: u8 = b'q';
pub const C_ECHO: u8 = b'e'; // 以後のフレームに ECHOACK を入れて (予測エコーを使う)

#[derive(Default, Clone, Copy, PartialEq)]
pub struct Meta {
    pub cx: u8,
    pub cy: u8,
    pub cvis: bool,
    pub mode: u8,
}

pub struct Encoder {
    pub rows: usize,
    pub cols: usize,
    sent: Vec<ZCell>, // Z の論理画面 (と同じになるように保つ)
    meta: Option<Meta>,
    pub scroll_detect: bool,
    pub stats_scrolls: u64,
    pub stats_clears: u64,
}

struct W<'a> {
    o: &'a mut Vec<u8>,
    r: usize,
    c: usize,
    col: u8,
    fl: u8,
}

impl W<'_> {
    fn pos(&mut self, r: usize, c: usize) {
        if self.r != r || self.c != c {
            self.o.extend_from_slice(&[OP_POS, r as u8, c as u8]);
            self.r = r;
            self.c = c;
        }
    }
    fn attr(&mut self, col: u8, fl: u8) {
        let fl = fl & (F_UNDERLINE | F_SPECIAL);
        if self.col != col || self.fl != fl {
            self.o.extend_from_slice(&[OP_ATTR, col, fl]);
            self.col = col;
            self.fl = fl;
        }
    }
}

fn row_eq(a: &[ZCell], b: &[ZCell]) -> bool {
    a.iter().zip(b).all(|(x, y)| x.same_look(y))
}
fn row_blank(a: &[ZCell]) -> bool {
    a.iter().all(|x| x.is_blank() && (x.col & 15) == (a[0].col & 15))
}

impl Encoder {
    pub fn new(cols: usize, rows: usize) -> Self {
        Encoder {
            rows,
            cols,
            sent: vec![ZCell::BLANK; rows * cols],
            meta: None,
            scroll_detect: true,
            stats_scrolls: 0,
            stats_clears: 0,
        }
    }

    /// Z 側の画面が分からなくなった (初回・再描画要求): 次のフレームで全部送る
    pub fn invalidate(&mut self) {
        for c in self.sent.iter_mut() {
            *c = ZCell { cp: 0xffff, col: 0xff, fl: 0xff };
        }
        self.meta = None;
    }

    pub fn resize(&mut self, cols: usize, rows: usize) {
        self.cols = cols;
        self.rows = rows;
        self.sent = vec![ZCell::BLANK; rows * cols];
        self.invalidate();
    }

    fn row<'a>(v: &'a [ZCell], cols: usize, r: usize) -> &'a [ZCell] {
        &v[r * cols..(r + 1) * cols]
    }

    // 送った画面を n 行ずらした形が新しい画面とよく合うなら、その (top, bot, n) を返す
    fn find_scroll(&self, new: &[ZCell]) -> Option<(usize, usize, i32)> {
        let (cols, rows) = (self.cols, self.rows);
        let diff: Vec<bool> = (0..rows)
            .map(|r| !row_eq(Self::row(new, cols, r), Self::row(&self.sent, cols, r)))
            .collect();
        let t = diff.iter().position(|&d| d)?;
        let b = diff.iter().rposition(|&d| d)?;
        if b - t < 2 {
            return None;
        }
        let mut best = (0usize, 0i32);
        for n in 1..=(b - t) {
            // 上へ n: new[r] == sent[r+n] (r = t..=b-n)
            let up = (t..=b - n)
                .filter(|&r| {
                    let nr = Self::row(new, cols, r);
                    !row_blank(nr) && row_eq(nr, Self::row(&self.sent, cols, r + n))
                })
                .count();
            if up > best.0 {
                best = (up, n as i32);
            }
            let down = (t + n..=b)
                .filter(|&r| {
                    let nr = Self::row(new, cols, r);
                    !row_blank(nr) && row_eq(nr, Self::row(&self.sent, cols, r - n))
                })
                .count();
            if down > best.0 {
                best = (down, -(n as i32));
            }
        }
        // ずらして合う行が、ずらさず合う行 (t..=b の中) より 2 行以上多いときだけ
        let same = (t..=b).filter(|&r| !diff[r]).count();
        if best.0 >= 2 && best.0 >= same + 2 {
            Some((t, b, best.1))
        } else {
            None
        }
    }

    fn apply_scroll(&mut self, t: usize, b: usize, n: i32) {
        let cols = self.cols;
        let rows: Vec<Vec<ZCell>> = (t..=b).map(|r| Self::row(&self.sent, cols, r).to_vec()).collect();
        let h = (b - t + 1) as i32;
        for i in 0..h {
            let src = i + n;
            let dst = (t as i32 + i) as usize;
            let line = if src >= 0 && src < h { rows[src as usize].clone() } else { vec![ZCell::BLANK; cols] };
            self.sent[dst * cols..(dst + 1) * cols].copy_from_slice(&line);
        }
    }

    /// 新しい画面との差分を 1 フレームにする。変化が無ければ false (out は触らない)
    pub fn frame(&mut self, new: &[ZCell], meta: Meta, bell: bool, echo: Option<u32>, out: &mut Vec<u8>) -> bool {
        let (cols, rows) = (self.cols, self.rows);
        let mut body = Vec::with_capacity(1024);
        let mut w = W { o: &mut body, r: 0, c: 0, col: 0x70, fl: 0 };
        if bell {
            w.o.push(OP_BELL);
        }
        if let Some(n) = echo {
            // 頭に置く (Z は画面を置いた後で照合するので、位置はどこでもよい)
            w.o.push(OP_ECHOACK);
            w.o.extend_from_slice(&n.to_be_bytes());
        }
        // まずスクロール (流れる文字は「ずらして新しい行だけ」)。それでも大半が変わる (画面消去・切り替え・1 画面以上流れた) なら
        // 全面を空白にしてから差分を取る (帯域の節約。Z の描画量は同じ)
        if self.scroll_detect {
            if let Some((t, b, n)) = self.find_scroll(new) {
                w.o.extend_from_slice(&[OP_SCROLL, t as u8, b as u8, n as i8 as u8]);
                self.apply_scroll(t, b, n);
                self.stats_scrolls += 1;
            }
        }
        let changed = new.iter().zip(&self.sent).filter(|(a, b)| !a.same_look(b)).count();
        if changed > rows * cols / 2 {
            let mut cnt = [0usize; 16];
            for c in new {
                cnt[(c.col & 15) as usize] += 1;
            }
            let bg = (0..16).max_by_key(|&i| cnt[i]).unwrap() as u8;
            w.o.extend_from_slice(&[OP_CLEAR, bg]);
            for c in self.sent.iter_mut() {
                *c = ZCell::blank(bg);
            }
            self.stats_clears += 1;
        }
        for r in 0..rows {
            let nr = &new[r * cols..(r + 1) * cols];
            let sr = &mut self.sent[r * cols..(r + 1) * cols];
            // 変わった桁 (全角は左右の両方を印にする)
            let mut ch = vec![false; cols];
            for c in 0..cols {
                if !nr[c].same_look(&sr[c]) {
                    ch[c] = true;
                }
            }
            for c in 0..cols {
                if ch[c] {
                    if nr[c].fl & F_WIDECONT != 0 && c > 0 {
                        ch[c - 1] = true;
                    }
                    if nr[c].fl & F_WIDE != 0 && c + 1 < cols {
                        ch[c + 1] = true;
                    }
                }
            }
            let mut c = 0;
            while c < cols {
                if !ch[c] {
                    c += 1;
                    continue;
                }
                // 連なりの終わり (間の 3 桁以下の無変化は含めてしまう: POS が 3 バイトなので)
                let mut e = c;
                let mut k = c;
                while k < cols {
                    if ch[k] {
                        e = k;
                        k += 1;
                    } else if k - e <= 3 {
                        k += 1;
                    } else {
                        break;
                    }
                }
                let mut x = c;
                if nr[x].fl & F_WIDECONT != 0 && x > 0 {
                    x -= 1;
                }
                w.pos(r, x);
                while x <= e {
                    let cell = nr[x];
                    if cell.fl & F_WIDECONT != 0 {
                        // 左が全角でない右半分 (起こらないはず): 空白で埋める
                        w.attr(cell.col, 0);
                        w.o.push(b' ');
                        sr[x] = ZCell { cp: 0x20, col: cell.col, fl: 0 };
                        x += 1;
                        w.c = x;
                        continue;
                    }
                    w.attr(cell.col, cell.fl);
                    if cell.fl & F_EMOJI != 0 {
                        w.o.extend_from_slice(&[OP_EMOJI, cell.cp as u8]);
                    } else if cell.fl & F_WIDE != 0 {
                        w.o.extend_from_slice(&[OP_WIDE, (cell.cp >> 8) as u8, cell.cp as u8]);
                    } else if (0x20..0x7f).contains(&cell.cp) {
                        // 同じ字が 4 つ以上続くなら REP
                        let mut n = 1;
                        while x + n <= e && n < 255 && nr[x + n] == cell {
                            n += 1;
                        }
                        if n >= 4 {
                            w.o.extend_from_slice(&[OP_REP, n as u8, cell.cp as u8]);
                            for i in 0..n {
                                sr[x + i] = cell;
                            }
                            x += n;
                            w.c = x;
                            continue;
                        }
                        w.o.push(cell.cp as u8);
                    } else {
                        w.o.extend_from_slice(&[OP_CP16, (cell.cp >> 8) as u8, cell.cp as u8]);
                    }
                    // Z が置くセルと同じ形で控える
                    let ul = w.fl & F_UNDERLINE;
                    if cell.fl & F_WIDE != 0 {
                        sr[x] = ZCell { cp: cell.cp, col: w.col, fl: w.fl | (cell.fl & (F_WIDE | F_EMOJI)) };
                        if x + 1 < cols {
                            sr[x + 1] = ZCell { cp: 0x20, col: w.col, fl: ul | F_WIDECONT };
                        }
                        x += 2;
                    } else {
                        sr[x] = ZCell { cp: cell.cp, col: w.col, fl: w.fl };
                        x += 1;
                    }
                    w.c = x;
                }
                c = x;
            }
        }
        let m = Some(meta);
        if self.meta != m {
            let old = self.meta;
            if old.map(|o| (o.cx, o.cy, o.cvis)) != Some((meta.cx, meta.cy, meta.cvis)) {
                w.o.extend_from_slice(&[OP_CURSOR, meta.cx, meta.cy, meta.cvis as u8]);
            }
            if old.map(|o| o.mode) != Some(meta.mode) {
                w.o.extend_from_slice(&[OP_MODE, meta.mode]);
            }
            self.meta = m;
        }
        if body.is_empty() {
            return false;
        }
        out.push(MAGIC);
        out.extend_from_slice(&(body.len() as u16).to_be_bytes());
        out.extend_from_slice(&body);
        true
    }
}

pub fn frame_raw(body: &[u8], out: &mut Vec<u8>) {
    out.push(MAGIC);
    out.extend_from_slice(&(body.len() as u16).to_be_bytes());
    out.extend_from_slice(body);
}
