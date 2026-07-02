# DOOM on a UniFi Dream Router
*Little disclaimer here: Practically all of this was made with Claude AI and for fun, I really suck at C coding so i needed help someway or another to make this.* 🥲
*I'm using all of this also to self-learn programming :)*

---
## Table of contents
1. [How this works](#how-this-works)
2. [How to do this](#how-to-do-this)
3. [Why using telnet?](#why-using-telnet)
4. [Deploying to the device](#deploying-doom)
5. [Bulding from scratch](#bulding-from-scratch)
6. [Controls](#controls)
7. [Environment knobs](#environment-knobs)
8. [How it works, in detail](#how-all-of-this-works-in-detail)
9. [Files in this project](#files-in-this-project)
---

## How this works

It's nothing special, it's just compiling doom for arm64 using chocolate-doom and doom generic and writing some custom code to make audio and screen working

## How to do this

After compiling DOOM you will get a binary file that you can run on the device with the DOOM WAD file.
We will need to use telnet and netcat or an http server to send the files. Then, it's just a command to start the game!

## Why using telnet?

This UniFi device has a very special thing, a speaker! The thing is, you have probably heard the speaker once (if you had this device), the speaker is used only in the device setup for a welcome jingle as far as I know, then, the audio amplifier chip, gets disabled. Probably for energy saving.
I saw on [this](https://elmoret.substack.com/i/150590991/audio) very detailed teardown of the device (go check it out if you're interested) that with a modprobe command you can activate back the speaker amplifier but had no success on doing it, still don't really know why or for what reason. The only thing that seems to make the speaker universally work is using the recovery mode.

## Deploying DOOM

Before starting, you need to boot the device in recovery mode: With your device disconnected from the power, connect the device to the power while holding down the reset button under the device. Release it when the device does a sound and says "Recovery Mode" On the screen.

I connected the device via a LAN cable to port 1 directly to my PC and set a static IP address on 192.168.1.10, you can telnet to the device on ```192.168.1.30``` and using ```ubnt``` as username and password.

You can transfer the bynary in two ways:

Using netcat:

On the computer, inside the folder containing the doom-udr binary and DOOM1.WAD:
```
tar -cvf - * | nc 192.168.1.30
```

On the UniFi:
```
cd /tmp
nc -l -p 9999 | tar -xvf -
```

Using HTTP:

Serve the files on the PC Using python HTTP Server:
```
python3 -m http.server 8000 --bind 0.0.0.0
```

On the UniFi:
```
cd /tmp
wget -O doom-udr  http://192.168.1.10:8000/doom-udr
wget -O DOOM1.WAD http://192.168.1.10:8000/DOOM1.WAD
```

After the files are transferred:
```
chmod +x doom-udr
./doom-udr -iwad /tmp/DOOM1.WAD
```



## Bulding from scratch

Everything lives under `~/unifi-doom/`.

### Cross toolchain (static musl aarch64)
```sh
mkdir -p ~/unifi-doom && cd ~/unifi-doom
curl -fL --retry 3 -o toolchain.tgz https://musl.cc/aarch64-linux-musl-cross.tgz
tar -xzf toolchain.tgz
```

### Engine source
```sh
cd ~/unifi-doom
git clone --depth 1 https://github.com/ozkl/doomgeneric
# the music/OPL code is borrowed from Chocolate Doom:
git clone --depth 1 https://github.com/chocolate-doom/chocolate-doom
```

### The shareware IWAD
```sh
cd ~/unifi-doom
curl -fL -o DOOM1.WAD https://distro.ibiblio.org/slitaz/sources/packages/d/doom1.wad
```

### Drop in our files + the imported OPL files
Our hand‑written files and `Makefile.unifi` live in
`doomgeneric/doomgeneric/`. The OPL/music files are copied from Chocolate Doom:
```sh
cd ~/unifi-doom
DST=doomgeneric/doomgeneric
cp chocolate-doom/opl/opl3.c chocolate-doom/opl/opl3.h \
   chocolate-doom/opl/opl.c chocolate-doom/opl/opl.h chocolate-doom/opl/opl_internal.h \
   chocolate-doom/opl/opl_queue.c chocolate-doom/opl/opl_queue.h chocolate-doom/opl/wf_rom.h \
   chocolate-doom/src/i_oplmusic.c chocolate-doom/src/midifile.c chocolate-doom/src/midifile.h \
   "$DST"/
```

### Build
```sh
cd ~/unifi-doom/doomgeneric/doomgeneric
make -f Makefile.unifi -j$(nproc)
file doom-udr
```

---

## Controls

All the controls you can use to navigate Doom on telnet:

| Action | Keys |
|---|---|
| Move forward / back | `W` / `S`  or  `↑` / `↓` |
| Turn left / right | `←` / `→`  or  `Q` / `E` |
| Strafe | `A` / `D` |
| Fire | `F` |
| Use / open door | `Space` |
| Run (hold) | `R` |
| Weapons | `1`–`7` |
| Automap | `Tab` |
| Menu / back | `Esc` |
| Confirm / select | `Enter` |
| Quit | `Ctrl‑C` |

Terminals only send key‑**down**, so, releases are synthesized after
`DOOM_HOLD_MS` (default 350 ms). Holding relies on your PC's key auto‑repeat.
Lower `DOOM_HOLD_MS` for snappier taps.

---

## Environment knobs

Set them in line, e.g. `DOOM_ROT=270 ./doom-udr -iwad /tmp/DOOM1.WAD`.

| Var | Default | Effect |
|---|---|---|
| `DOOM_FB` | `/dev/fb0` | framebuffer device |
| `DOOM_ROT` | `90` | rotation: `0/90/180/270` |
| `DOOM_FLIP` | `0` | mirror image horizontally |
| `DOOM_LETTERBOX` | `0` | `1` keeps 4:3 aspect (black bars) vs fill |
| `DOOM_HOLD_MS` | `350` | key auto‑release window (ms) |
| `DOOM_SND` | `1` | `0` disables sound effects |
| `DOOM_AUDIODEV` | `plughw:0,0` | ALSA device passed to `aplay -D` |
| `DOOM_MUSIC` | `1` | `0` disables music |
| `DOOM_MUSICVOL` | `256` | music level vs SFX (0–512; try 160–200 if harsh) |
| `DOOM_DMXOPTION` | `-opl3` | OPL3 (rich) vs `""` for OPL2 |

---

## How all of this works, in detail

### The engine + backend contract
[doomgeneric](https://github.com/ozkl/doomgeneric) is Chocolate Doom reduced to
six platform functions. The engine renders into **`DG_ScreenBuffer`** — a
**640×400** array of `uint32` pixels (`0x00RRGGBB`; Doom's native 320×200 doubled,
hence `Auto-scaling factor: 2`). We implement:

```c
DG_Init / DG_DrawFrame / DG_SleepMs / DG_GetTicksMs / DG_GetKey / DG_SetWindowTitle
```

### Two threads
- **Main/game thread:** runs game logic, renders the frame, and in `DG_DrawFrame`
  scales+rotates it to the panel and reads stdin.
- **Audio thread (pthread):** forever mixes SFX + renders OPL music and writes
  PCM to `aplay` (which back‑pressures it to real time).

### Video (`doomgeneric_unifi.c`)
1. `mmap(/dev/fb0)`; read geometry/bitfields via `FBIOGET_VSCREENINFO/FSCREENINFO`.
2. **Box‑average downscale** 640×400 → 160×80 (clean 4×/5× blocks → 20 source px averaged per output px).
3. **Pack RGB565** with the panel's reported bit offsets: `(r>>3)<<11 | (g>>2)<<5 | (b>>3)`.
4. **Rotate** onto the 80×160 panel: at 90°, logical `(lx,ly)` → panel `(ly,lx)`,
   stored as a 16‑bit write at `fb + fy*stride + fx*2`.

> Note: the `I_InitGraphics: framebuffer 640x400 RGBA 8888` log line is the
> engine's *internal* buffer; `[doom-udr] fb 80x160` is the *real* panel.

### Input (`doomgeneric_unifi.c`)
- `tcsetattr` puts stdin in **raw, non‑blocking** mode (no echo/canonical;
  `ISIG` kept so Ctrl‑C quits); original termios restored on exit.
- Each frame: read bytes, decode arrow escape sequences (`ESC [ A/B/C/D`), map
  ASCII to Doom keys.
- **Synthetic key‑release:** terminals send no key‑up, so each key gets an expiry
  (`now + DOOM_HOLD_MS`); expired keys emit a release. Auto‑repeat extends a held key.

### Sound effects (`i_unifisound.c`, replaces `i_sound.c`)
- Parse Doom **DMX** lumps (`03 00` header, rate@[2..3], length@[4..7], 16+16
  pad bytes, unsigned‑8 mono samples), resample 11025 → 44100 and convert to S16,
  cached per sound.
- Mixer thread sums up to **16 channels** with per‑channel L/R gain
  (`left=((254-sep)*vol)/127`), clamps, outputs **S16LE stereo @ 44100**.
- Output by `fork`+`exec` of **`aplay -q -t raw -f S16_LE -c 2 -r 44100 -D plughw:0,0 -`**
  and writing PCM to its stdin pipe (no ALSA lib to static‑link; aplay paces us).

### Music — OPL synthesis
Doom music = **MUS** sequence + **GENMIDI** instrument table in the WAD, played on
an emulated **OPL3** FM chip:

```
MUS ──mus2mid──▶ MIDI(/tmp/doom.mid) ──midifile.c──▶ events
                          GENMIDI patches ──▶ i_oplmusic.c programs the chip
                          opl3.c (Nuked OPL3) ──▶ PCM
```
- `i_oplmusic.c` (reused from Chocolate Doom) turns each note into OPL frequency
  (F‑number + octave) and operator settings from GENMIDI.
- `opl3.c` is the **Nuked OPL3** emulator.
- **`opl_unifi.c`** is our custom, non‑SDL "driver": it keeps a virtual µs clock
  and a queue of scheduled music events. Our audio thread calls
  `OPL_Unifi_Render(buf, 1024)`, which generates samples up to the next event,
  fires it (writes registers + schedules the next), and advances time — so
  rendering audio *is* what advances musical time, perfectly synced with SFX.
- `opl.c`'s `OPL_Delay` (used during chip detection, originally SDL cond‑var) was
  ported to **pthreads with a 2 s timeout** so init can't hang.

### Locking
SFX uses one small mutex; the OPL driver uses two (callback queue + a lock that
freezes callbacks). Nuked's `OPL3_WriteRegBuffered` is lock‑free by design, so
register writes and sample generation across threads are safe. Lock domains never
nest oppositely → no deadlock.

---

## Files in this project

```
~/unifi-doom/
├── README.md                        ← this file
├── DOOM1.WAD                        ← shareware IWAD
├── aarch64-linux-musl-cross/        ← extracted toolchain
├── dist/                            ← what the HTTP server serves
│   ├── doom-udr                     ← Compiled binary
│   └── DOOM1.WAD                    ← DOOM Wad
├── chocolate-doom/                  ← upstream, source of the OPL/music files
└── doomgeneric/doomgeneric/         ← build dir
    ├── Makefile.unifi               ← (ours) cross-build recipe
    ├── doomgeneric_unifi.c          ← (ours) video + input + timing backend
    ├── i_unifisound.c               ← (ours) SFX mixer + aplay + music glue
    ├── opl_unifi.c                  ← (ours) non-SDL OPL driver + pull-renderer
    ├── opl3.{c,h} opl.{c,h} opl_internal.h opl_queue.{c,h} wf_rom.h   ← imported
    ├── i_oplmusic.c midifile.{c,h}  ← imported
    └── …stock doomgeneric/Doom sources…
```

---

*Doom and the Doom data are © id Software. The shareware `DOOM1.WAD` is freely
distributable. doomgeneric and Chocolate Doom are GPLv2.*
