---
layout: post
title: "Cannonball for x68 Diary (2) The road, the sound, and measuring again closer to the real machine"
excerpt: "I merged 27 PRs in two days. The road no longer disappears when a raster interrupt runs late, and sound now plays through the FM chip and ADPCM. When I turned on the emulator's I/O wait states and measured again, it was far slower than the earlier numbers. I got the game back to running in real time under that condition too. It has not run on a real machine yet."
date: 2026-11-14 01:30:00 +0900
---

At the end of the [previous post]({{ site.baseurl }}{% post_url en/2026-11-09-cannonball-diary-1 %}) I wrote down two things to do next: reduce the I/O per raster interrupt, and find out what makes some frames heavy. Over two days, November 12 and 13, I had the AI (Claude) continue, and merged 27 PRs ([#3](https://github.com/j3tm0t0/CannonballForX68/pull/3) to [#29](https://github.com/j3tm0t0/CannonballForX68/pull/29)) into my own fork. The target is v0.2 of [Cannonball for x68](https://github.com/Mattsuan-Z/CannonballForX68) by Mattsuan-Z (an X68000 port of Cannonball, the OutRun engine). The goal I set: make it run at a playable speed on a real X68030 with a 060turbo (a 68060 accelerator board).

The conclusion first: it has not run on a real machine yet. Every number in this post was measured on an emulator.

## How to read the numbers

This post uses three counts: steps, draws and screen updates.

- **Step**: advancing the game's time by 1/60 second. A measurement runs 4,000 steps (about 67 seconds of game time) on autopilot
- **Draw**: the picture is rebuilt once every 2 steps, so 4,000 steps means at most 2,000 draws
- **Screen update**: counted once each time the X68000 finishes displaying a screen (the number of vertical blanks). It is a different number from draws. If 4,000 steps finish in 4,000 screen updates, the game is running in real time. If it takes more, the game is running late

The emulator is XEiJ in 060turbo mode, as before. The clock is 16MHz in some measurements and 50MHz in others, so each table says which.

## The road disappears partway

Last time, the road appeared when XEiJ ran at 100MHz with no I/O waits. But at lower clocks, the road stopped partway down the screen, or didn't appear at all in some frames. This port draws the road with a raster interrupt every 2 lines of the screen. There should be 224 raster interrupts per frame. Counting them gave this (no waits, 1,200 steps).

| Clock | All lines | Stopped partway | None at all |
|---|---:|---:|---:|
| 16MHz | 24% | 0% | 76% |
| 33MHz | 55% | 45% | 0% |

There were two causes ([#5](https://github.com/j3tm0t0/CannonballForX68/pull/5)).

- The vertical blank interrupt set up the first raster interrupt only after it had finished writing the new frame's palettes and sprite registers. If those writes don't finish within the vertical blank period, that frame gets no raster interrupts at all
- While a raster interrupt was writing roadside object data, the next line's interrupt was not accepted. If the writes take longer than one line (63.5µs), setting the line for the next raster interrupt comes too late, and the rest of that frame's interrupts never happen

In both places, the next interrupt is now set up first, and the slow writes run with interrupts accepted. If the next line's raster interrupt arrives in the middle of the writes, that interrupt writes only the road registers and returns. At 16MHz over 4,000 steps, every line of every frame now gets its interrupt.

## Fewer writes for roadside objects

Roadside objects (palm trees, signs, other cars) are drawn in software, cut into 16×16 pixel tiles, and shown with X68000 sprites. The sprite patterns are written to the PCG (the memory that holds sprite patterns). Patterns that are done near the top of the screen get used again further down, so the PCG is rewritten inside raster interrupts while the screen is being displayed.

On XEiJ, a write to the PCG takes about 1.1µs per long (4 bytes), and raising the clock doesn't change that. v0.2 was set to write up to 96 longs per line, and writing 96 longs takes about 105µs. That doesn't fit in the 63.5µs of one line, so sometimes a pattern wasn't fully written by the time its picture started to display. I changed it as follows.

- When the same picture is displayed again, only the places that were rewritten are put back. The amount written per screen update went from 3,060 longs on average to 583 ([#6](https://github.com/j3tm0t0/CannonballForX68/pull/6))
- When a pattern is rewritten, only the longs that differ from the old contents are written (#6)
- The limit per line is decided by measuring the PCG write time at startup ([#7](https://github.com/j3tm0t0/CannonballForX68/pull/7))
- One write may run over into the next line, and the limit is "the amount that can be written in one line's time on average" ([#20](https://github.com/j3tm0t0/CannonballForX68/pull/20))

Lowering the limit makes the writes finish in time, but more objects can't be shown. So when not everything fits, tiles with the least drawn in them are dropped first ([#15](https://github.com/j3tm0t0/CannonballForX68/pull/15)). Before that, tiles on the right side of the screen were dropped regardless of their contents, which left a rectangular hole on the right just after the start. The number of pixels that couldn't be shown went down by 76% (counted on the build that runs on a PC, the PC build from here on: 330,019 → 78,657).

## What made frames heavy

Last time I wrote that building the drawing data took up to 128ms in some frames. The heavy frames were in the scene before the start. That scene has about 330 sprites, and almost the only thing moving is the person waving the flag.

- Once every 8 frames, there is a pass that records which colors each palette actually used. In that frame alone, every tile was converted again even if nothing on screen had changed, and it took about 200ms. The other frames took about 18ms (16MHz). Now, if the colors have already been recorded, the previous frame's conversion result is used ([#4](https://github.com/j3tm0t0/CannonballForX68/pull/4))
- If even one sprite changed, all of the roughly 330 were redrawn (115 to 130ms per frame at 50MHz). Now only the rows (in units of 16 lines) that the changed sprites overlap are redrawn ([#11](https://github.com/j3tm0t0/CannonballForX68/pull/11), [#14](https://github.com/j3tm0t0/CannonballForX68/pull/14), [#16](https://github.com/j3tm0t0/CannonballForX68/pull/16))
- If everything is the same as the previous frame, the sprite assignment result is used as is. Those frames went from 25ms to 6ms (50MHz, [#9](https://github.com/j3tm0t0/CannonballForX68/pull/9))

Redrawing fewer rows means more draws fit into the scene before the start. Draws in the first 400 steps went from 39 to 57–65 with #14. Draws in the first 500 steps went from 62–77 to 87–90 with #16 (both at 50MHz).

I also made changes that cut CPU time. The tile conversion was rewritten in assembly ([#3](https://github.com/j3tm0t0/CannonballForX68/pull/3); at 16MHz, the tile conversion went from 20.4ms to 10.2ms per draw). The text display, such as the score, skips rows that haven't changed ([#8](https://github.com/j3tm0t0/CannonballForX68/pull/8)). The compiler optimization option went from `-O2` to `-O3` ([#18](https://github.com/j3tm0t0/CannonballForX68/pull/18); CPU time went down by about 5%).

## Skipping draws to keep the game at speed

In v0.2, when drawing can't keep up, the game itself slows down. At 16MHz it ran at about 1/3 of real time. So now, when the game falls 2 or more screen updates behind, the next draw is skipped. The game's calculation is never skipped (#9).

At 50MHz with no waits, 4,000 steps finished in 3,999 screen updates, with 1,610 draws out of 2,000 (as of #9).

## Playing sound

v0.2 was silent.

The FM chip on the OutRun arcade board is a YM2151, the same chip as the X68000's built-in FM sound. The clock is the same 4MHz too. Cannonball includes a C++ port of the board's original sound program. The register writes that program makes are now sent as they are to the X68000's YM2151 ([#10](https://github.com/j3tm0t0/CannonballForX68/pull/10)). The sound routine is called from the vertical blank interrupt so that it runs 125 times a second. Called from the game loop, the tempo of the music would waver whenever drawing took long.

Drums, voices and the engine sound come from a chip called Sega PCM on the board (it plays 8-bit waveforms from ROM on 16 channels). The X68000 has no such chip. The 16 channels are now mixed in software into mono (15,625Hz) and played through the X68000's ADPCM ([#13](https://github.com/j3tm0t0/CannonballForX68/pull/13)). The playback buffer is linked so that it goes back to the beginning after the end, and at every vertical blank it is filled up to about 57ms ahead of the playback position.

The trouble is that the AI can't listen, so it can't tell whether the sound is right. Instead it checked two things.

- It made a test that records, in order, the register values the X68000 build writes to the FM chip, feeds the same input to the PC build's sound routine, and compares the writes ([#12](https://github.com/j3tm0t0/CannonballForX68/pull/12)). Over 3,000 steps (54 seconds), all 104,770 writes matched
- It recorded XEiJ's audio output and measured the level (RMS). FM was 1,897 and PCM was 1,074, so PCM was about half. Playing the same section through the sound chip emulators included in Cannonball gives almost the same level for both. The PCM volume was doubled to match ([#17](https://github.com/j3tm0t0/CannonballForX68/pull/17))

What this shows is only that the registers get the same values in the same order as the PC build, and that it isn't silent. Nobody has yet checked by ear whether it sounds right as music.

## Measuring again with waits on

Up to here I had been measuring with XEiJ's waits off (`-waitcycles=off`), as last time. To see whether the speed is playable on a real machine, it has to be measured with waits on. This XEiJ setting reproduces the time I/O accesses take, and the wait when reading memory that isn't in the 68060's cache (8KB).

I started measuring with 060turbo at 50MHz, waits on, and 060turbo.sys (the driver for the 060turbo) installed, but 600 steps didn't finish even after 800 seconds. On XEiJ's 060turbo, the CPU cache was off at startup. The program now turns the cache on when it starts ([#19](https://github.com/j3tm0t0/CannonballForX68/pull/19)). Even so, 4,000 steps took 7,986 screen updates, with 380 draws. The game was running at only half of real time.

From there I added changes one after another. The table shows the screen updates that 4,000 steps took. The closer to 4,000, the closer the game is to real time.

| Change | Screen updates | Draws |
|---|---:|---:|
| Cache turned on, nothing else | 7,986 | 380 |
| Raster interrupts write only registers whose value differs from the previous line (#19) | 5,873 | 382 |
| Rework the limit on roadside object writes (#20) | 5,751 | |
| Swap the working memory for road and sprites by swapping pointers instead of copying ([#21](https://github.com/j3tm0t0/CannonballForX68/pull/21)) | 5,515 | |
| Don't read screen memory (GVRAM) when writing the background ([#22](https://github.com/j3tm0t0/CannonballForX68/pull/22)) | 5,210 | 442 |
| Raise the limit on consecutive skipped draws from 5 to 27 ([#23](https://github.com/j3tm0t0/CannonballForX68/pull/23)) | 4,222 | 376 |
| Write new background columns row by row, horizontally, instead of vertically ([#24](https://github.com/j3tm0t0/CannonballForX68/pull/24)) | 4,233 | 412 |
| Fewer cache misses, and a light mode ([#25](https://github.com/j3tm0t0/CannonballForX68/pull/25)) | 4,003 | 465 |

Where the draws column is empty, the PR doesn't give the number.

v0.2 wrote 16 registers at every raster interrupt (26 word writes). The road's color changes only at the edges of its stripes, so most lines don't need those writes. This is the change I listed under "Next" last time: reducing the I/O per interrupt.

The other thing I learned is that under this condition, cache misses add more time than I/O does. The 68060's data cache is only 8KB. At every game step there was code that swapped a 4KB area and a 2KB area by copying, and each time most of the cache contents were replaced (#21). #24 also reduces cache misses. Writing the background one column at a time, vertically, reads and writes a different row of memory for every pixel, and each one is a cache miss. Changing it to write horizontally, row by row, took the background and text writes from 19.8ms to 10.4ms per frame.

The light mode reduces the number of roadside tiles when writes to the PCG are slow (1.4µs or more per long, measured at startup). Tiles with the most drawn in them are kept. On XEiJ it is 1.9µs with waits on, so light mode is used, and 1.0µs with waits off, so it is not. In light mode, 14.6% of the pixels of roadside objects aren't shown (counted on the PC build).

## Running in the 060turbo's local memory

The 060turbo has its own memory on the board (local memory). When reading data that isn't in the cache, local memory is about 3 times faster than the main memory on the computer itself (this is the value XEiJ reproduces; I haven't measured it on a real machine). Neither the code nor the data of this program fits in the 8KB cache, so the speed depends on which memory it is in.

060turbo.sys has a compatible mode and an extended mode (the `-xm` option). Only the extended mode can load a program into local memory. But just adding `-xm` still left the program loaded in main memory. So right after starting, the program now uses a 060turbo.sys function to run itself again in local memory ([#27](https://github.com/j3tm0t0/CannonballForX68/pull/27)). There were two problems on the way.

- With 060turbo.sys 0.61, the call that reports the mode did not return the documented 1 in extended mode (it returned 518). Anything other than 0 and -1 is now treated as extended mode
- The DMA controller used for ADPCM playback (the circuit that sends memory contents to the chip automatically) can't read local memory. Only the playback buffer is allocated from main memory

Along with this, I fixed two things about skipping draws. In a step whose draw is skipped, the work that only builds data for drawing is no longer run ([#26](https://github.com/j3tm0t0/CannonballForX68/pull/26); the time for the game's calculation went from 27.2 to 20.9 seconds). The other is how the wait at the end of a step is calculated. The decision to skip a draw and the wait used different deadlines. Because of that, about 20% of the total time went into waiting even though draws were being skipped ([#28](https://github.com/j3tm0t0/CannonballForX68/pull/28)).

Last, I made the Sega PCM mixing faster ([#29](https://github.com/j3tm0t0/CannonballForX68/pull/29)). Comparing draws with and without sound showed that sound alone was costing about 15% of the draws (in local memory: 1,412 without sound, 1,196 with).

Draws in 4,000 steps (XEiJ, 060turbo, 50MHz. In the measurements for #27 and #28, every condition took 4,001 to 4,006 screen updates, so the game runs in real time):

| Condition | #27 | #28 | #29 |
|---|---:|---:|---:|
| Waits on, main memory | 536 | 589 | 635 |
| Waits on, local memory | 947–975 | 1,196 | 1,241 |
| Waits off | 1,763 | 1,839 | 1,852 |

Each PR was measured on its own, so the last value in the previous section's table (465) and the first value in this one (536) don't connect. Also, which steps skip their draw changes a little from build to build, so a difference of a few draws is noise (measuring the same program again stays within 1,196 to 1,198). Converting the #29 values to draws per second gives about 9.5 in main memory, about 18.6 in local memory, and about 27.8 with waits off.

## A separate machine for measuring

Last time I wrote that the time inside XEiJ drifts when the Mac is under load. It happened again. The same program at 50MHz took 4,540 screen updates one time and 7,293 another. Partway through, I set up a separate Linux machine that does nothing but run XEiJ, and moved the 50MHz measurements there. Two runs under the same condition now differ by 0.3 to 0.5% in draws.

## It also ran on a Raspberry Pi 5

In another project, I'm making an X68000 emulator that runs on a Raspberry Pi 5 without an OS (pix68k). I have an AI do the development there too, in a session separate from the Cannonball one. The program so far had been checked only on XEiJ. To see whether it runs on a different implementation, I had the pix68k AI run it.

That turned up one bug. In the IOCS call (a system call in the X68000's ROM) that reads the cache state, the argument number was wrong. Because of it, the program turned the cache off on exit even if it had been on at startup. I hadn't noticed while measuring on XEiJ (fixed in #25).

Results of 2,000 steps on the Pi 5 (060turbo configuration, 060turbo.sys 0.61):

| | No sound | FM only | FM and PCM |
|---|---:|---:|---:|
| Draws in compatible mode | 813 | 789 | 712 |
| Draws in extended mode (local memory) | 808 | 790 | 710 |

Under every condition, the raster interrupts ran to the last line in every frame except the first frame of the measurement, the vertical blank interrupt never failed to finish before the next vertical blank, and the PCM playback position never had to be resynchronized. In extended mode, Human68k (the X68000's OS) didn't boot at first because of a bug in pix68k. The runs were done after that was fixed.

These numbers don't indicate the speed of a real machine. pix68k's 68060 uses approximate times per instruction and has no I/O waits. I think draws don't go up in extended mode because it doesn't reproduce the speed difference between main memory and local memory. What this does show is that on an implementation other than XEiJ, the raster interrupts that draw the road and the sound playback keep running to the end of the measurement.

## Next

- Run it on a real machine with a 060turbo. The numbers so far are what XEiJ reproduces, and I don't know whether they match the real machine. The statistics printed at exit now include I/O write times and where things are in memory, so I'll compare them with the results on the real machine
- Listen to the sound. It has only been recorded and measured for level. Nobody has listened to the FM or the PCM yet
- All 27 PRs were merged into my own fork. How to hand them to Mattsuan-Z, the author, is yet to be decided
- I haven't tried PhantomX (a board that plugs a Raspberry Pi in place of the X68000's CPU), which I wrote about last time
