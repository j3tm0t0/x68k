// Z (term/vt100) が描ける形のセル。vt100.c の cp_kind / fallback / map256 / nearest16 をそのまま移した。
// Z は受け取った cp と色・フラグを論理画面に置くだけで、Unicode の判定は何もしない。
//   cp: 0x20-0x7e は ANK、F_SPECIAL なら DEC 罫線グリフ (0x60-0x7e)、F_EMOJI なら絵文字表の番号、
//       F_WIDE なら漢字 (Unicode BMP。Z の blit が CGROM の JIS に引く)、他は半角カナ・ブロック要素・¥・‾。
include!(concat!(env!("OUT_DIR"), "/tables.rs"));

pub const F_UNDERLINE: u8 = 0x01;
pub const F_SPECIAL: u8 = 0x02;
pub const F_WIDE: u8 = 0x04;
pub const F_WIDECONT: u8 = 0x08;
pub const F_EMOJI: u8 = 0x10;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ZCell {
    pub cp: u16,
    pub col: u8, // fg<<4 | bg (0..15 のパレット番号、bold / reverse 解決済み)
    pub fl: u8,
}

impl ZCell {
    pub const BLANK: ZCell = ZCell { cp: 0x20, col: 0x70, fl: 0 };
    pub fn blank(bg: u8) -> ZCell { ZCell { cp: 0x20, col: 0x70 | bg, fl: 0 } }
    pub fn is_blank(&self) -> bool { self.cp == 0x20 && self.fl == 0 }
    // 見た目が同じか (vt100.c の same_look): 空白 (下線なし) どうしなら前景色は見えない
    pub fn same_look(&self, o: &ZCell) -> bool {
        if self == o { return true; }
        self.is_blank() && o.is_blank() && (self.col & 15) == (o.col & 15)
    }
}

pub fn is_kanji(cp: u32) -> bool {
    cp <= 0xffff && KANJI.binary_search(&(cp as u16)).is_ok()
}
pub fn emoji_index(cp: u32) -> Option<u16> {
    EMOJI.binary_search(&cp).ok().map(|i| i as u16)
}

// 字形の無い文字の代用 (vt100.c fallback)。(ASCII, DEC 特殊グリフ) のどちらか
fn fallback(cp: u32) -> Option<(u8, u8)> {
    const LATIN1: &[u8; 64] = b"AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (0xc0..=0xff).contains(&cp) { return Some((LATIN1[(cp - 0xc0) as usize], 0)); }
    let a = |c: u8| Some((c, 0));
    match cp {
        0x23bf => Some((0, 0x6d)),
        0x00b7 | 0x2022 | 0x2219 | 0x30fb => Some((0, 0x7e)),
        0x00b0 => Some((0, 0x66)),
        0x00b1 => a(b'+'),
        0x2264 => a(b'<'),
        0x2265 => a(b'>'),
        0x23fa | 0x25cf | 0x25cb | 0x25c9 | 0x25ef | 0x26aa | 0x26ab => a(b'o'),
        0x273b | 0x273d | 0x2736 | 0x2733 | 0x2722 | 0x2217 | 0x2731 | 0x2728 => a(b'*'),
        0x276f | 0x203a | 0x25b8 | 0x25b6 | 0x23f5 | 0x2192 | 0x21d2 | 0x23e9 => a(b'>'),
        0x2039 | 0x25c2 | 0x25c0 | 0x2190 | 0x23ea => a(b'<'),
        0x2191 | 0x25b4 | 0x25b2 => a(b'^'),
        0x2193 | 0x25be | 0x25bc => a(b'v'),
        0x2713 | 0x2714 | 0x2705 | 0x2611 => a(b'v'),
        0x2717 | 0x2718 | 0x00d7 | 0x2715 | 0x274c | 0x274e => a(b'x'),
        0x2026 => a(b'.'),
        0x2013 | 0x2014 | 0x2015 | 0x2212 => a(b'-'),
        0x2018 | 0x2019 => a(b'\''),
        0x201c | 0x201d => a(b'"'),
        0x26a0 | 0x2757 | 0x2755 => a(b'!'),
        0x2753 | 0x2754 => a(b'?'),
        0x26a1 => a(b'z'),
        0x23f0..=0x23f3 | 0x231a | 0x231b => a(b'@'),
        0x2b50 | 0x2605 | 0x2606 => a(b'*'),
        0x2795 => a(b'+'),
        0x2796 => a(b'-'),
        0x00a0 => a(b' '),
        0x00a9 => a(b'c'),
        0x00ae => a(b'r'),
        _ => None,
    }
}

fn box_dec(cp: u32) -> u8 {
    match cp {
        0x2500 | 0x2501 | 0x2504 | 0x2505 | 0x2508 | 0x2509 | 0x254c | 0x254d | 0x2550 => 0x71,
        0x2502 | 0x2503 | 0x2506 | 0x2507 | 0x250a | 0x250b | 0x254e | 0x254f | 0x2551 => 0x78,
        0x250c | 0x250f | 0x256d | 0x2554 => 0x6c,
        0x2510 | 0x2513 | 0x256e | 0x2557 => 0x6b,
        0x2514 | 0x2517 | 0x2570 | 0x255a => 0x6d,
        0x2518 | 0x251b | 0x256f | 0x255d => 0x6a,
        0x251c | 0x2523 | 0x2560 => 0x74,
        0x2524 | 0x252b | 0x2563 => 0x75,
        0x252c | 0x2533 | 0x2566 => 0x77,
        0x2534 | 0x253b | 0x2569 => 0x76,
        0x253c | 0x254b | 0x256c => 0x6e,
        _ => 0,
    }
}

// DEC 特殊グリフ (ESC ( 0 中の 0x60-0x7e) は前処理で私用領域 U+F760-F77E に置き換えてある (dec.rs)
pub const DEC_PUA: u32 = 0xf700;

/// 1 字を置き方に変える。wide は端末 (vt100 crate) がその字を 2 桁と数えたか。
/// 戻り値は 1 桁目のセル (cp, fl) と、2 桁目の扱い: None = 1 桁 / Some(None) = 全角の右半分 / Some(Some(c)) = 半角の代用セル
pub fn convert(ch: char, wide: bool) -> ((u16, u8), Option<Option<(u16, u8)>>) {
    let cp = ch as u32;
    let qq = ((b'?' as u16, 0), Some(Some((b'?' as u16, 0))));
    if (0x20..0x7f).contains(&cp) {
        return ((cp as u16, 0), if wide { Some(Some((0x20, 0))) } else { None });
    }
    if (DEC_PUA + 0x60..=DEC_PUA + 0x7e).contains(&cp) {
        return (((cp - DEC_PUA) as u16, F_SPECIAL), None);
    }
    if wide {
        if is_kanji(cp) { return ((cp as u16, F_WIDE), Some(None)); }
        if let Some(i) = emoji_index(cp) { return ((i, F_WIDE | F_EMOJI), Some(None)); }
        if let Some((a, m)) = fallback(cp) {
            let c = if m != 0 { (m as u16, F_SPECIAL) } else { (a as u16, 0) };
            return (c, Some(Some((0x20, 0))));
        }
        return qq;
    }
    let one = |c: (u16, u8)| (c, None);
    if cp < 0xa0 { return one((b'?' as u16, 0)); }
    if (0x2500..=0x257f).contains(&cp) {
        let m = box_dec(cp);
        return one(if m != 0 { (m as u16, F_SPECIAL) } else { (b'+' as u16, 0) });
    }
    if (0x2580..=0x259f).contains(&cp) || (0xff61..=0xff9f).contains(&cp) || cp == 0xa5 || cp == 0x203e {
        return one((cp as u16, 0));
    }
    match fallback(cp) {
        Some((a, m)) => one(if m != 0 { (m as u16, F_SPECIAL) } else { (a as u16, 0) }),
        None => one((b'?' as u16, 0)),
    }
}

// ---- 色 (vt100.c nearest16_calc / map256 と同じ) ----
pub fn nearest16(r: i32, g: i32, b: i32) -> u8 {
    let mx = r.max(g).max(b);
    let mn = r.min(g).min(b);
    let c = mx - mn;
    if c < 64 {
        let l = (r * 3 + g * 6 + b) / 10;
        return if l < 48 { 0 } else if l < 128 { 8 } else if l < 214 { 7 } else { 15 };
    }
    let h = if mx == r {
        let h = 60 * (g - b) / c;
        if h < 0 { h + 360 } else { h }
    } else if mx == g {
        120 + 60 * (b - r) / c
    } else {
        240 + 60 * (r - g) / c
    };
    const HUE6: [u8; 6] = [1, 3, 2, 6, 4, 5];
    let idx = HUE6[(((h + 30) % 360) / 60) as usize];
    if mx >= 200 { idx + 8 } else { idx }
}

pub fn map256(n: u8) -> u8 {
    const LV: [i32; 6] = [0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff];
    if n < 16 { return n; }
    if n >= 232 {
        let v = 8 + (n as i32 - 232) * 10;
        return nearest16(v, v, v);
    }
    let n = (n - 16) as usize;
    nearest16(LV[n / 36], LV[(n / 6) % 6], LV[n % 6])
}

fn color(c: vt100::Color, def: u8) -> u8 {
    match c {
        vt100::Color::Default => def,
        vt100::Color::Idx(n) => map256(n),
        vt100::Color::Rgb(r, g, b) => nearest16(r as i32, g as i32, b as i32),
    }
}

/// vt100 crate のセルの属性 -> (fg<<4|bg, 下線フラグ)。vt100.c の attr_update と同じ (bold は暗い 8 色を明るく、reverse は入れ替え)
pub fn attrs(c: &vt100::Cell) -> (u8, u8) {
    let mut fg = color(c.fgcolor(), 7);
    let mut bg = color(c.bgcolor(), 0);
    if c.bold() && fg < 8 { fg += 8; }
    if c.inverse() { std::mem::swap(&mut fg, &mut bg); }
    ((fg << 4) | bg, if c.underline() { F_UNDERLINE } else { 0 })
}

/// 端末の画面を Z のセルの格子に写す
pub fn snapshot(screen: &vt100::Screen, rows: usize, cols: usize, out: &mut Vec<ZCell>) {
    out.clear();
    out.resize(rows * cols, ZCell::BLANK);
    for r in 0..rows {
        let mut c = 0;
        while c < cols {
            let Some(cell) = screen.cell(r as u16, c as u16) else { c += 1; continue };
            let (col, ul) = attrs(cell);
            if cell.is_wide_continuation() {
                // 左が全角でなかった (右端の取り残しなど): 空白
                out[r * cols + c] = ZCell { cp: 0x20, col, fl: 0 };
                c += 1;
                continue;
            }
            let ch = cell.contents().chars().next().unwrap_or(' ');
            let wide = cell.is_wide() && c + 1 < cols;
            let ((cp, fl), second) = convert(ch, wide);
            out[r * cols + c] = ZCell { cp, col, fl: fl | ul };
            match second {
                None => c += 1,
                Some(None) => {
                    out[r * cols + c + 1] = ZCell { cp: 0x20, col, fl: F_WIDECONT };
                    c += 2;
                }
                Some(Some((cp2, fl2))) => {
                    out[r * cols + c + 1] = ZCell { cp: cp2, col, fl: fl2 | ul };
                    c += 2;
                }
            }
        }
    }
}
