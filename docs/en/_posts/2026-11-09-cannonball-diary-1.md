---
layout: post
title: "Cannonball for x68 Diary (1) Running it locally, and making it a little faster"
excerpt: "Someone is porting Cannonball, the OutRun engine, to the X68000. On my local emulator the road didn't show up and it ran at 1.2fps. The cause was the emulator reproducing I/O wait states. After that, I made two PRs that cut the drawing preparation by about 10%."
date: 2026-11-09 15:00:00 +0900
---

In October I was trying to port SEGA's OutRun to the X68000 (outrun-x68k). It was based on Cannonball (Chris White's C++ rewrite of the OutRun engine), and the goal was 30fps on a 10MHz 68000. Even after tightening the drawing code in assembly, 5.88fps at 24MHz was the limit.

Recently I learned that someone else is porting the same Cannonball to the X68000: [Cannonball for x68](https://github.com/Mattsuan-Z/CannonballForX68) by Mattsuan-Z, now at v0.2. This one assumes a 68060. On XM6 (an X68000 emulator for Windows), an X68030 with a 68060 at 4x clock, 100MHz, apparently gets about 25fps. I'd like to work on it together, so I started by having the AI (Claude) get it running locally.

## Running it locally

The repository has a build for checking on a PC (`build_host.sh`) and a build for the X68000 (`build_x68k.sh`).

- The PC build went through as is with clang on macOS. Both the demo that needs no ROM (the program draws the road, backgrounds and shapes itself) and the game with ROMs ran
- The X68000 build produced `STAGE1.X` with elf2x68k (gcc for the X68000), which I already had from Homebrew

I had no emulator locally to run the X68000 build. For outrun-x68k I had put XEiJ (an X68000 emulator written in Java) only on the CI runner. I installed XEiJ from its official site and started it in 060turbo mode (a 68060 accelerator board) at 100MHz with 12MB of memory.

It did start. It loaded the ROMs, went to the demo screen, started a race and printed statistics at the end. But there was no road surface. The sky, the sea and the car were there, but where the road should be, it stayed sky blue. Speed: 1.2fps.

## Why the road was missing

This port draws the road with the BG (the X68000's hardware tile layer) and raster interrupts. It raises an interrupt every 2 lines of the screen, and each time it rewrites the BG scroll position and palette. That needs 224 interrupts per frame.

According to the statistics, there were only 2.9 interrupts per frame on average.

XEiJ comes with its source, so I had the worker read the implementation of the CRTC (the chip that controls the display). Rewriting the line number that raises the interrupt (R09) is not checked right away. The comparison happens when the next line starts. If the interrupt handler runs long and the line has already passed before the handler writes the next line number, the interrupts stop there for that frame.

Each interrupt writes dozens of times to BG registers, palettes and sprite memory. By default XEiJ reproduces the wait states for main memory and I/O accesses (`waitcycles=on`). The worker's guess was that these waits keep the handler from finishing within 2 lines.

Restarting with `-waitcycles=off` raised the interrupts to about 215 per frame, and the road appeared. Speed: 23.5fps, close to the 25fps on XM6. From now on I use the local XEiJ with waits off in place of XM6 to compare speed before and after a change.

## On PhantomX

Personally, I'd be happy if it runs on PhantomX (a board that plugs a Raspberry Pi in place of the X68000's CPU and runs a 68000-family CPU in software) in its fast mode. But on PhantomX too, I/O accesses go over the real X68000 bus. Most likely the road would disappear just as it did on XEiJ. However fast the CPU is, the road won't show up unless the I/O per interrupt goes down. In the end, the bus holds it back.

## Where the time goes

The statistics printed at the end of the game don't fit on one screen; each key press shows the next page. I took the Java agent for XEiJ that I made for outrun-x68k (it saves the screen as PNG from inside XEiJ) and added key input and copying the screen text to the clipboard. Now one script runs everything from the build through execution to extracting the statistics.

Here is the time per draw (no waits, 100MHz). table is the whole job of building the drawing data, visuals_build is the part of it that prepares everything except the road, and build_scenery is the part of that which handles roadside objects.

| Section | Time |
|---|---:|
| Building drawing data (table) | 15.6ms |
| of which visuals_build | 13.2ms |
| of which roadside objects (build_scenery) | 11.3ms |
| Game logic | 1.7ms |

Roadside objects (palm trees, signs, other cars) are made in these steps.

1. Draw the objects in software into a 320×224 buffer (capture, 4.6ms)
2. Cut the buffer into 16×16 tiles and convert them into X68000 sprite patterns (tiles, 4.2ms)
3. Assign sprite numbers and patterns so they are reused from the top of the screen down, and build the table of what the raster interrupts rewrite (the rest, 2.5ms)

The X68000 has only 128 sprites, so numbers and patterns that are done near the top get rewritten and used again further down.

## Two PRs

I had the worker look for places that could be made lighter without changing the picture, and tried these two.

1. **Don't read empty tiles**: tiles checked all 20 columns of tiles in the rows where objects are. Now capture works out the horizontal range each sprite may have drawn into, and only tiles that overlap those ranges are converted
2. **Stop the linear search during assignment**: to find a pattern with the same picture, it compared against every pattern in use, every time. In 2000 draws, a match was found only 241 times, so almost every search compared everything. Patterns in use are now split into 64 lists by hash value, and only the matching list is searched. I also changed sorting the tiles from 14 passes into a single bucketing pass

I checked on the PC build that the picture didn't change. The repository already prints a hash of the picture drawn in software. But that hash doesn't include the sprite data handed to the X68000. So I temporarily added code that prints a hash of the sprite patterns, the tables the interrupts rewrite, and the registers of the player's car sprites, and compared it with v0.2. I compared 4000 steps with ROMs and two kinds of the demo without ROMs. Each PR alone, and both together, matched v0.2 exactly.

Time per draw measured on XEiJ (50MHz):

| | v0.2 | PR 1 | PR 2 | Both |
|---|---:|---:|---:|---:|
| visuals_build | 25.3ms | 24.7ms | 24.0ms | 22.8ms |
| vs v0.2 | | -2.4% | -5.3% | -9.8% |

In PR 1, tiles went down by 10.6%, but capture went up by 4.4% for computing the ranges. The overall gain is small.

[PR 1](https://github.com/j3tm0t0/CannonballForX68/pull/1) and [PR 2](https://github.com/j3tm0t0/CannonballForX68/pull/2) went to my own fork first. After checking them over, I'll pass them to Mattsuan-Z.

## A measuring pitfall

Once, every measured time doubled. Not only the drawing: even the time of the game logic, which I hadn't changed, doubled. At that point AI workers were busy with other jobs in other windows on the Mac, and the load was high (load average 4.7). XEiJ apparently couldn't keep up with 100MHz worth of work in real time, and the CPU inside the emulator slowed down.

After that I lowered the clock to 50MHz and only compared runs whose game logic time was about the same. Even so the numbers vary by a few percent, so PR 1's -2.4% is close to the noise.

## Next

- Reduce the I/O per interrupt. To run on PhantomX, this comes before shaving CPU time. XEiJ with `waitcycles=on` might serve as a stand-in for the waits on the real bus
- The statistics show a frame where building the drawing data took up to 128ms. On average it fits within the time for one frame (about 36ms), so I think frames like this are why the fps drops. Find out which part takes the time
