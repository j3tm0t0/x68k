// term/vt100 の表 (kanjitab.h / emojitab.h) を読んで Rust の表を作る。
// Z 側の描画 (vt100.c) と同じ「描ける字」の集合をサーバが持つので、表がずれない。
use std::{env, fs, path::Path};

fn hexes(s: &str, pat: &str) -> Vec<u32> {
    // "{0x00a2,0x2171}" / "{0x2705,{" の先頭の 16 進だけ拾う
    let mut v = Vec::new();
    for part in s.split(pat).skip(1) {
        let h: String = part.chars().take_while(|c| c.is_ascii_hexdigit()).collect();
        if !h.is_empty() {
            v.push(u32::from_str_radix(&h, 16).unwrap());
        }
    }
    v
}

fn main() {
    let vt = env::var("ZTERM_VT100_DIR").unwrap_or_else(|_| "../../vt100".into());
    let kt = fs::read_to_string(Path::new(&vt).join("kanjitab.h")).expect("kanjitab.h");
    let et = fs::read_to_string(Path::new(&vt).join("emojitab.h")).expect("emojitab.h");
    println!("cargo:rerun-if-changed={vt}/kanjitab.h");
    println!("cargo:rerun-if-changed={vt}/emojitab.h");
    // kanjitab: 表の本体 ("static const kanji_ent kanjitab[] = {" 以降) の "{0x" の直後が ucs
    let body = &kt[kt.find("kanjitab[] = {").expect("kanjitab body")..];
    let body = &body[..body.find("};").unwrap()];
    let kanji = hexes(body, "{0x");
    let ebody = &et[et.find("emojitab[] = {").expect("emojitab body")..];
    let ebody = &ebody[..ebody.find("\n};").unwrap()];
    let emoji: Vec<u32> = ebody.lines().filter(|l| l.starts_with("{0x")).map(|l| hexes(l, "{0x")[0]).collect();
    let mut o = String::new();
    o += &format!("pub static KANJI: [u16; {}] = {:?};\n", kanji.len(), kanji);
    o += &format!("pub static EMOJI: [u32; {}] = {:?};\n", emoji.len(), emoji);
    fs::write(Path::new(&env::var("OUT_DIR").unwrap()).join("tables.rs"), o).unwrap();
}
