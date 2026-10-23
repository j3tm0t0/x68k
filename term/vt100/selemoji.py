#!/usr/bin/env python3
"""収録する絵文字を選んで emojilist.txt を作る (genemoji.py の LIST)。
元の順位は Unicode の「Emoji Frequency」(https://home.unicode.org/emoji/emoji-frequency/、
Ranked Order by Median Frequency) の並び。それに既存の表・手で足す字 (EXTRA) を先に入れ、順位の上から N 件まで埋める。
入れられるのは次をすべて満たす字だけ:
  - 1 コードポイント (U+FE0F は除いて数える)。国旗・ZWJ 合成・肌色は入らない
  - wcwidth が 2 (widthtab.h の全角)。テキスト表示が既定の字 (❤ ⚠ ☀ ➡ ✔ …) は wcwidth 1 で、
    相手のアプリ・pyte・zterm の vt100 crate が 1 桁と数えるので 2 桁の絵文字にすると桁がずれる
  - JIS X 0208 に無い (kanjitab.h にある字は CGROM の漢字として描くので絵文字表を引かない)
  - Twemoji 15.1.0 の 72x72 PNG がある
usage: selemoji.py FREQ.html PNGDIR [N] > emojilist.txt
"""
import html, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ORIG = "🌟 🎂 🎉 👀 👋 👍 👎 👏 💖 💩 💪 💯 🔥 😀 😂 😊 😍 😎 😡 😢 😭 🙏 🚀 🤔 ✅ ✨ ❌ ⭐".split()   # 以前の 28 字
EXTRA = ("🍣 🍺 🍻 🍶 🍜 🍙 🍱 🍕 🍔 🍰 ☕ 🍵 🍷 🍸 🍾 🍎 🍓 🍌 "
         "❤️ 💔 😅 😆 😉 😘 😱 😴 🥺 😇 🙂 🙃 😐 🤣 🥳 😤 🤯 🫠 "
         "⚠️ ⚡ 💡 📝 📌 📎 🔑 🔒 🔧 🔨 ⚙️ 💻 🖥️ ⌨️ 📱 📷 📦 📁 📂 🗂️ 🗑️ ⏰ ⏳ ⌛ 🔔 🎯 🏁 🐛 🐞 🎮 🕹️ 🎵 🎶 "
         "✏️ 📊 📈 📉 🔍 ➡️ ⬅️ ⬆️ ⬇️ ✔️ ☑️ ➕ ➖ ❓ ❗ 🆗 🆕 🟢 🟡 🔴 🔵 ⚪ ⚫ 🟥 🟩 🟦 🟨 "
         "🐱 🐶 🐟 🐙 🐧 🦀 ☀️ ☁️ 🌧️ ⛄ 🌈 🌸 🍀 🌙 🇯🇵").split()   # ⏰ は Claude Code が出す

freq, pngdir = sys.argv[1], sys.argv[2]
N = int(sys.argv[3]) if len(sys.argv) > 3 else 300

wt = open(os.path.join(HERE, "widthtab.h")).read()
wt = wt[wt.find("widetab"):wt.find("zerotab")]
wide = [(int(a, 16), int(b, 16)) for a, b in re.findall(r"\{0x([0-9a-f]+),0x([0-9a-f]+)\}", wt)]
kanji = {int(a, 16) for a in re.findall(r"\{0x([0-9a-f]+),0x", open(os.path.join(HERE, "kanjitab.h")).read())}

t = open(freq, encoding="utf-8").read()
i = t.find("Ranked Order by Median Frequency")
ranked = [w.rstrip("…") for w in html.unescape(re.sub("<[^>]+>", " ", t[i:t.find("</table>", i)])).split()
          if any(ord(c) > 0x2000 for c in w)]


def why_not(s):
    c = [ord(ch) for ch in s if ord(ch) != 0xfe0f]
    if len(c) != 1: return "複数コードポイント"
    c = c[0]
    if c in kanji: return "JIS にある (CGROM の漢字で描く)"
    if not any(a <= c <= b for a, b in wide): return "wcwidth 1 (テキスト表示が既定)"
    if not os.path.exists(os.path.join(pngdir, "%x.png" % c)): return "Twemoji に無い"
    return None


src = {}
for s, how in [(s, "orig") for s in ORIG] + [(s, "extra") for s in EXTRA] + [(s, "rank") for s in ranked]:
    if len(src) >= N and how == "rank": break
    r = why_not(s)
    if r:
        if how != "rank": sys.stderr.write("skip %s (%s): %s\n" % (s, how, r))
        continue
    src.setdefault(ord(s.replace("\ufe0f", "")), how)
print("# selemoji.py が生成。hex コードポイント 字 出所 (orig = 以前の 28 字 / extra = 手で足した字 / rank = Unicode Emoji Frequency の順位)")
for c in sorted(src):
    print("%x %s %s" % (c, chr(c), src[c]))
sys.stderr.write("%d entries\n" % len(src))
