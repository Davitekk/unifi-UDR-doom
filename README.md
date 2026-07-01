# DOOM on a UniFi Dream Router (UDR) — recovery mode

Real id‑Software Doom, running **on the UDR itself**, with working **video** (the
80×160 front panel), **sound effects**, and **music** (software OPL3 synth).
Built as a single static aarch64 binary on an Arch Linux PC and run from the
device's recovery telnet shell.

```
   ARCH LINUX PC (build/host)                    UDR (recovery, telnet)
 ┌──────────────────────────┐               ┌────────────────────────────────┐
 │ musl cross-gcc           │   HTTP/wget   │  ./doom-udr  (static aarch64)   │
 │ doomgeneric + our backend│ ───────────▶  │   video → mmap(/dev/fb0) 80x160 │
 │      → dist/doom-udr      │   DOOM1.WAD   │   input ← stdin (raw telnet)    │
 │ python3 -m http.server   │               │   audio → pipe → aplay → ALSA   │
 └──────────────────────────┘               └────────────────────────────────┘
```

---

## Table of contents
1. [The device (what the hardware gives us)](#1-the-device)
2. [Quick start (each session)](#2-quick-start-each-session)
3. [Build from scratch (full command trail)](#3-build-from-scratch)
4. [Serving + deploying to the device](#4-serving--deploying)
5. [Controls](#5-controls)
6. [Environment knobs](#6-environment-knobs)
7. [How it works, in detail](#7-how-it-works)
8. [Files in this project](#8-files-in-this-project)
9. [Troubleshooting](#9-troubleshooting)
10. [Init log, annotated](#10-init-log-annotated)

---

## 1. The device

Discovered by probing the recovery shell over telnet:

| Thing | Value | Consequence |
|---|---|---|
| CPU | aarch64, dual Cortex‑A53 (MediaTek MT7622) | cross‑compile for **ARM64** |
| Kernel | 4.4.198 | old; avoid newer loaders → use `-no-pie` |
| libc | glibc 2.24 (recovery) | too old to link against → **static musl** |
| Display | `/dev/fb0`, **80×160, 16bpp RGB565**, stride 160 | standard framebuffer; we `mmap` it |
| Audio | ALSA card 0 = USB C‑Media "MicroII" (internal speaker), `aplay` present | output PCM via `aplay`; **no i2c needed in recovery** |
| Input | telnet only | read keys from **stdin** |
| Writable FS | only tmpfs (`/tmp`, ~960 MB) | stage binary + WAD in `/tmp` each session |

The panel is physically **portrait** (80 w × 160 h); Doom is landscape, so we
render 160×80 and **rotate 90°** onto it.

### Recon command (run in the recovery telnet shell)
```sh
echo "== CPU/KERNEL =="; uname -a; head -40 /proc/cpuinfo; cat /proc/version
echo "== LIBC =="; ls -l /lib/ld-* /lib/libc.* 2>/dev/null; ls /lib/libc.musl* 2>/dev/null
echo "== MEM/DISK =="; (free || head -3 /proc/meminfo); df -h
echo "== DISPLAY =="; ls -l /dev/fb* 2>/dev/null; cat /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/bits_per_pixel /sys/class/graphics/fb0/stride 2>/dev/null; fbset 2>/dev/null
echo "== AUDIO =="; ls -l /dev/snd/ /dev/dsp* 2>/dev/null; cat /proc/asound/cards 2>/dev/null; which aplay
echo "== INPUT =="; ls -l /dev/input/ 2>/dev/null
echo "== I2C =="; ls -l /dev/i2c-* 2>/dev/null; which i2cset i2cget
echo "== TOOLS =="; busybox 2>&1 | head -25; which gcc cc python3 ldd
```

---

## 2. Quick start (each session)

**On the PC** (serve the already‑built files; leave it running):
```sh
cd ~/unifi-doom/dist
python3 -m http.server 8000 --bind 0.0.0.0
```
Find the PC's IP on the UDR's network:
```sh
ip -4 -o addr show scope global | awk '{print $2, $4}'
# here: wlan0  10.24.142.190/24
```

**On the UDR** (recovery telnet):
```sh
cd /tmp
killall doom-udr 2>/dev/null
wget -O doom-udr   http://10.24.142.190:8000/doom-udr
wget -O DOOM1.WAD  http://10.24.142.190:8000/DOOM1.WAD
chmod +x doom-udr
./doom-udr -iwad /tmp/DOOM1.WAD
```
`/tmp` is tmpfs, so files vanish on reboot — re‑`wget` them after a reboot.
Quit with **Ctrl‑C** (the terminal is restored cleanly).

---

## 3. Build from scratch

Everything lives under `~/unifi-doom/`.

### 3.1 Cross toolchain (static musl aarch64)
```sh
mkdir -p ~/unifi-doom && cd ~/unifi-doom
curl -fL --retry 3 -o toolchain.tgz https://musl.cc/aarch64-linux-musl-cross.tgz
tar -xzf toolchain.tgz
# compiler: ~/unifi-doom/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc
```

### 3.2 Engine source
```sh
cd ~/unifi-doom
git clone --depth 1 https://github.com/ozkl/doomgeneric
# the music/OPL code is borrowed from Chocolate Doom:
git clone --depth 1 https://github.com/chocolate-doom/chocolate-doom
```

### 3.3 The shareware IWAD (free to distribute)
```sh
cd ~/unifi-doom
curl -fL -o DOOM1.WAD https://distro.ibiblio.org/slitaz/sources/packages/d/doom1.wad
md5sum DOOM1.WAD     # expect: f0cefca49926d00903cf57551d901abe  (v1.9 shareware)
```

### 3.4 Drop in our files + the imported OPL files
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
(`mus2mid.c` is already in doomgeneric.) The small source patches that make those
compile without SDL on this older tree are listed in
[§8](#8-files-in-this-project).

### 3.5 Build
```sh
cd ~/unifi-doom/doomgeneric/doomgeneric
make -f Makefile.unifi -j$(nproc)
file doom-udr     # ELF 64-bit ARM aarch64, statically linked, stripped (~506 KB)
```

### 3.6 Stage for serving
```sh
mkdir -p ~/unifi-doom/dist
cp ~/unifi-doom/doomgeneric/doomgeneric/doom-udr ~/unifi-doom/dist/
cp ~/unifi-doom/DOOM1.WAD ~/unifi-doom/dist/
chmod +x ~/unifi-doom/dist/doom-udr
```

---

## 4. Serving + deploying

The device's busybox has `wget` but no scp/ssh, so we serve over plain HTTP.

**PC (serve):**
```sh
cd ~/unifi-doom/dist && python3 -m http.server 8000 --bind 0.0.0.0
# verify locally:
curl -fsS -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8000/doom-udr
```

**UDR (pull + run):**
```sh
cd /tmp
wget -O doom-udr  http://10.24.142.190:8000/doom-udr
wget -O DOOM1.WAD http://10.24.142.190:8000/DOOM1.WAD
chmod +x doom-udr
./doom-udr -iwad /tmp/DOOM1.WAD
```

> If `wget` can't connect: the UDR is on a different subnet than `wlan0`. Run
> `ip addr` on the device and serve from a PC interface on the same network
> (and check the PC firewall isn't blocking :8000).

---

## 5. Controls

Typed into the telnet window:

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

Terminals only send key‑**down**, so releases are synthesized after
`DOOM_HOLD_MS` (default 350 ms). Holding relies on your PC's key auto‑repeat.
Lower `DOOM_HOLD_MS` for snappier taps.

---

## 6. Environment knobs

No recompile needed — set them inline, e.g. `DOOM_ROT=270 ./doom-udr -iwad /tmp/DOOM1.WAD`.

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

## 7. How it works

### 7.1 The engine + backend contract
[doomgeneric](https://github.com/ozkl/doomgeneric) is Chocolate Doom reduced to
six platform functions. The engine renders into **`DG_ScreenBuffer`** — a
**640×400** array of `uint32` pixels (`0x00RRGGBB`; Doom's native 320×200 doubled,
hence `Auto-scaling factor: 2`). We implement:

```c
DG_Init / DG_DrawFrame / DG_SleepMs / DG_GetTicksMs / DG_GetKey / DG_SetWindowTitle
```

### 7.2 Two threads
- **Main/game thread:** runs game logic, renders the frame, and in `DG_DrawFrame`
  scales+rotates it to the panel and reads stdin.
- **Audio thread (pthread):** forever mixes SFX + renders OPL music and writes
  PCM to `aplay` (which back‑pressures it to real time).

### 7.3 Video (`doomgeneric_unifi.c`)
1. `mmap(/dev/fb0)`; read geometry/bitfields via `FBIOGET_VSCREENINFO/FSCREENINFO`.
2. **Box‑average downscale** 640×400 → 160×80 (clean 4×/5× blocks → 20 source px averaged per output px).
3. **Pack RGB565** with the panel's reported bit offsets: `(r>>3)<<11 | (g>>2)<<5 | (b>>3)`.
4. **Rotate** onto the 80×160 panel: at 90°, logical `(lx,ly)` → panel `(ly,lx)`,
   stored as a 16‑bit write at `fb + fy*stride + fx*2`.

> Note: the `I_InitGraphics: framebuffer 640x400 RGBA 8888` log line is the
> engine's *internal* buffer; `[doom-udr] fb 80x160` is the *real* panel.

### 7.4 Input (`doomgeneric_unifi.c`)
- `tcsetattr` puts stdin in **raw, non‑blocking** mode (no echo/canonical;
  `ISIG` kept so Ctrl‑C quits); original termios restored on exit.
- Each frame: read bytes, decode arrow escape sequences (`ESC [ A/B/C/D`), map
  ASCII to Doom keys.
- **Synthetic key‑release:** terminals send no key‑up, so each key gets an expiry
  (`now + DOOM_HOLD_MS`); expired keys emit a release. Auto‑repeat extends a held key.

### 7.5 Sound effects (`i_unifisound.c`, replaces `i_sound.c`)
- Parse Doom **DMX** lumps (`03 00` header, rate@[2..3], length@[4..7], 16+16
  pad bytes, unsigned‑8 mono samples), resample 11025 → 44100 and convert to S16,
  cached per sound.
- Mixer thread sums up to **16 channels** with per‑channel L/R gain
  (`left=((254-sep)*vol)/127`), clamps, outputs **S16LE stereo @ 44100**.
- Output by `fork`+`exec` of **`aplay -q -t raw -f S16_LE -c 2 -r 44100 -D plughw:0,0 -`**
  and writing PCM to its stdin pipe (no ALSA lib to static‑link; aplay paces us).

### 7.6 Music — OPL synthesis
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

### 7.7 Locking
SFX uses one small mutex; the OPL driver uses two (callback queue + a lock that
freezes callbacks). Nuked's `OPL3_WriteRegBuffered` is lock‑free by design, so
register writes and sample generation across threads are safe. Lock domains never
nest oppositely → no deadlock.

---

## 8. Files in this project

```
~/unifi-doom/
├── README.md                        ← this file
├── DOOM1.WAD                        ← shareware IWAD (md5 f0cefca4…)
├── toolchain.tgz                    ← musl cross toolchain archive
├── aarch64-linux-musl-cross/        ← extracted toolchain
├── dist/                            ← what the HTTP server serves
│   ├── doom-udr
│   └── DOOM1.WAD
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

### Files written from scratch
| File | Role |
|---|---|
| `doomgeneric_unifi.c` | the six backend functions: fb0 scale/rotate, telnet/termios input, timing |
| `i_unifisound.c` | SFX mixer, `aplay` pipe, music delegation, sound config vars (replaces `i_sound.c`) |
| `opl_unifi.c` | non‑SDL OPL driver: virtual clock, callback queue, `OPL_Unifi_Render` |
| `Makefile.unifi` | static musl cross build; swaps SDL/X11 backends for ours |

### Imported from Chocolate Doom, then patched
| File | Patch | Why |
|---|---|---|
| `opl.c` | `OPL_Delay` SDL→pthread (+2 s timeout); driver table → only `opl_unifi_driver`; drop `SDL.h` | remove SDL; no init hang |
| `midifile.c` | add `SDL_SwapBE16/32` byteswap shims; `M_fopen`→`fopen`; local `I_Realloc` | helpers newer Chocolate assumes |
| `i_oplmusic.c` | `M_remove`→`remove`; `music_opl_module` made non‑`const` | match doomgeneric headers |
| `opl3.c/.h`, `opl_queue.c/.h`, `wf_rom.h`, `midifile.h` | none | self‑contained |

### doomgeneric files modified
| File | Change |
|---|---|
| `doomtype.h` | added `PACKED_STRUCT(...)` / `PACKEDPREFIX` macros |
| `i_sound.h` | added `opl_driver_ver_t` enum + `I_SetOPLDriverVer` declaration |
| `Makefile.unifi` | obj list swaps `i_sound.o`→`i_unifisound.o`, `doomgeneric_xlib.o`→`doomgeneric_unifi.o`, adds the OPL/MIDI objects, `-pthread`, `-static -no-pie` |

Stock doomgeneric already compiles SDL‑free: SDL is gated behind `#ifdef ORIGCODE`
and `FEATURE_SOUND` is undefined, so we only had to provide our own sound module.

---

## 9. Troubleshooting

| Symptom | Fix |
|---|---|
| `wget` can't connect | UDR on a different subnet — `ip addr` on device; serve from a matching PC interface; check firewall on :8000 |
| Picture sideways / mirrored | set `DOOM_ROT=0/90/180/270`, add `DOOM_FLIP=1` |
| No sound at all | try `DOOM_AUDIODEV=default` or `DOOM_AUDIODEV=hw:0,0` |
| Music too loud / clipping | `DOOM_MUSICVOL=180` (or lower); or in‑game Options → Sound Volume |
| Audio crackles/stutters | OPL3 is CPU‑heavy; try `DOOM_DMXOPTION=` (OPL2) or disable music `DOOM_MUSIC=0` |
| `music: OPL init failed` | GENMIDI missing or detection failed; game still runs without music |
| Controls overshoot on tap | lower `DOOM_HOLD_MS` (e.g. 150) |
| Files gone after reboot | tmpfs is volatile — re‑`wget` `doom-udr` and `DOOM1.WAD` into `/tmp` |

---

## 10. Init log, annotated

```
[doom-udr] fb 80x160 16bpp stride=160 logical=160x80 rot=90   ← panel opened, geometry OK
Z_Init … 600000 allocated for zone                            ← Doom's 6 MB pool
Using . for configuration and saves                           ← config dir = cwd (/tmp)
W_Init … adding /tmp/DOOM1.WAD                                 ← IWAD loaded
DOOM Shareware                                                 ← episode 1 only
I_Init: Setting up machine state.                             ← sound+music init begins
[doom-udr] audio: aplay pid=… 44100 Hz stereo -> plughw:0,0   ← SFX mixer + aplay child up
OPL_Init: Using driver 'Unifi'.                               ← our OPL driver bound, chip detected
[doom-udr] music: OPL synth ready (gain=256)                  ← music live
Emulating the behavior of the 'Doom 1.9' executable.          ← gameplay compatibility mode
I_InitGraphics: framebuffer 640x400 RGBA 8888 … factor: 2     ← engine INTERNAL buffer (not the panel)
(shell prompt returns)                                        ← you pressed Ctrl-C; clean exit
```

---

*Doom and the Doom data are © id Software. The shareware `DOOM1.WAD` is freely
distributable. doomgeneric and Chocolate Doom are GPLv2.*
