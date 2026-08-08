# nes

A NES emulator built on the [emu6502](../emu6502) core.

## Status

**Super Mario Bros is playable, in a window, at NTSC speed, with sound.** Press Start,
the game begins; hold Right and Mario runs; press A and he jumps over the Goomba, and
the overworld theme plays while he does. Scrolling, sprites, and the sprite-zero split
that keeps the status bar fixed while the level moves beneath it all work.

| Component | State |
| --- | --- |
| iNES container parsing | 1.0 with trainers; NES 2.0 read as 1.0 plus its timing field |
| Mappers | 0 NROM, 1 MMC1, 2 UxROM, 3 CNROM, 4 MMC3, 7 AxROM, 87 |
| Region | NTSC and PAL, read from the header and applied to every clock |
| Mapper IRQ | MMC3's scanline counter, merged with the APU onto one CPU line |
| CPU bus | RAM + mirroring, PPU/cartridge routing, `peek()` for debuggers |
| CPU | emu6502, which passes Klaus Dormann's functional test |
| PPU timing | dot/scanline/frame counters, vblank, NMI, odd-frame dot skip |
| PPU registers | full register file: write toggle, buffered `$2007`, auto-increment |
| PPU memory | VRAM with nametable mirroring, palette mirroring, CHR via the mapper |
| Mirroring | horizontal, vertical, four-screen, both single-screen modes; switchable at runtime |
| Background rendering | tiles, attributes, fine and coarse scroll across nametables |
| Sprite rendering | 8x8 and 8x16, flipping, priority, 8-per-line limit, overflow |
| Sprite-zero hit | reported at the dot of overlap, so mid-frame splits work |
| OAM DMA (`$4014`) | copies the page and stalls the CPU 513 cycles |
| Controllers (`$4016/$4017`) | both ports, latch and shift register, open bus in the high bits |
| Battery saves | `.sav` beside the ROM, loaded on start, written on exit and reset |
| APU channels | two pulses with sweep, triangle, noise, DMC — all five |
| APU frame counter | 4- and 5-step sequences, quarter/half clocks, frame IRQ |
| APU mixing | the hardware's nonlinear curve, high-pass and anti-alias filtering |
| Window | `nes_gui`: SDL2 video and audio, keyboard, paced to NTSC's 60.0988 Hz |
| Headless runner | `nes_run`: tracing, scripted input, PPM screenshots, WAV capture |

## Layout

```
include/nes/     public headers
src/             Cartridge (iNES), Mapper (seven boards), Ppu, Apu, Controller,
                 NesBus, Nes, main.cpp (headless runner), gui_main.cpp (SDL2 window)
test_src/        doctest suite, including the nestest harness
```

## Building

```bash
cmake -S . -B build && cmake --build build --config Release
```

That is all it takes from a fresh clone. The 6502 core is used from a sibling checkout
when one exists, and fetched from git when it does not — so cloning only this repository
works, and developing against a local core alongside it also works, with the local copy
winning. To point at a checkout somewhere else:

```bash
cmake -S . -B build -DEMU6502_DIR=/path/to/emu6502
```

Its own tests and debugger stay off when consumed this way — build those from that
project directly. Everything lands in `build/bin/<config>/` so `emu6502_lib.dll` and
`SDL2.dll` sit beside the executables on Windows.

SDL2 is used by the window front-end only. An installed one is preferred; failing that
CMake fetches and builds it, which is what makes a fresh checkout work with nothing but
a compiler and git — at the cost of a slower first configure. To skip it entirely and
build only the headless runner and the tests:

```bash
cmake -S . -B build -DNES_BUILD_GUI=OFF
```

## Playing

```bash
./build/bin/Release/nes_gui rom.nes --scale=3
```

| | Player 1 | Player 2 |
| --- | --- | --- |
| D-pad | arrows | numpad 8 4 5 6 |
| A / B | Z / X | numpad 1 / 2 |
| Start / Select | Enter / Right Shift | numpad Enter / + |

`P` or `Space` pauses, `N` advances one frame while paused, `M` mutes, holding `Tab`
runs unthrottled, `R` resets, `F12` saves a screenshot, `Esc` quits. `--scale=N` sets
the window size, `--fullscreen` starts borderless, `--no-audio` runs silent; the picture
letterboxes to the NES's aspect at any window size, with nearest-neighbour scaling.

Audio is 44.1 kHz mono, queued rather than driven from a callback thread — the emulator
produces samples in frame-sized bursts on the main thread, and the device's own buffer
smooths them out. Sound and video are paced by two clocks nobody synchronised, so the
resampling ratio is nudged by a few parts in a thousand depending on how much audio is
queued: too little starves the device into crackling, too much turns into latency. The
correction is far below the threshold of hearing and holds the two together
indefinitely.

A cartridge with a save chip keeps its data in a `.sav` file beside the ROM, written
when you close the window or press `R` and read back the next time you load it. Nothing
asks: a game that saves, saves.

The title bar shows the real frame rate, which should read `60.1 fps` — NTSC is
60.0988 Hz, not 60. The pacing is an absolute deadline advanced by exactly one frame
each time, so per-frame overshoot cannot accumulate into drift, and vsync is
deliberately off: on a 144 Hz display it would run the console more than twice too fast.

## Running headless

```bash
./build/bin/Release/nes_run rom.nes --trace --stop-on-trap
```

```
C000  A9 42     LDA $42        A:00 X:00 Y:00 P:24 SP:FD CYC:7
C002  8D 00 02  STA $0200      A:42 X:00 Y:00 P:24 SP:FD CYC:9
```

Options: `--trace`, `--start-pc=HEX`, `--max=N`, `--stop-on-trap`. Disassembly goes
through a read-only `PeekBus`, so tracing never disturbs device state.

On exit it summarises PPU state, which is how you check a game is alive without opening
a window:

```
PPU: frame 303, scanline 58 dot 89, ctrl=10 mask=1E, rendering on
     3948/4096 nametable bytes written, 67/256 OAM bytes set
     palette: 22 29 1A 0F 0F 36 17 0F 0F 30 21 0F 0F 27 17 0F
```

A full nametable, sprites staged in OAM, and `$22` (sky blue) at the top of the palette
is a game that has drawn a screen.

To actually see it, grab a frame:

```bash
./build/bin/Release/nes_run rom.nes --frames=900 --screenshot=frame.ppm
```

`--frames=N` runs N PPU frames and stops; the PPM is a plain binary P6 that any image
viewer opens. Super Mario Bros reaches its title screen by frame ~120 and starts the
attract-mode demo around frame ~900.

Sound comes out the same way:

```bash
./build/bin/Release/nes_run rom.nes --press=start@200 --frames=900 --audio=out.wav
```

`--audio` writes 44.1 kHz mono 16-bit PCM. Unlike the window, this uses a fixed
resampling ratio and no drift correction — there is no sound card to stay in step with,
so the same ROM and the same inputs produce the same WAV every time. That is what makes
audio testable rather than merely audible.

### Scripted input

Headless runs take their buttons from `--press`, which is `BUTTON@FRAME[:HELD][/PORT]`
and can be repeated. Deterministic, so it reproduces exactly — which is what makes it
worth keeping now that there is a keyboard:

```bash
./build/bin/Release/nes_run smb.nes --press=start@200 --press=right@430:200 --press=a@600:20 --frames=615 --screenshot=jump.ppm
```

That starts the game, runs Mario right for 200 frames, and jumps him over the first
Goomba. `HELD` defaults to 10 frames — a press has to survive at least a frame or two
to be seen, because a game samples the pad once per frame in its NMI handler. `PORT` is
1 or 2 and defaults to 1.

Buttons are `a`, `b`, `select`, `start`, `up`, `down`, `left`, `right`. Overlapping
presses combine, so `--press=right@100:60 --press=b@100:60` is a run.

## Testing

```bash
ctest --test-dir build -C Release --output-on-failure
```

The cartridge and bus tests build synthetic iNES images in-memory, so they need no
external ROMs.

### nestest

The CPU conformance test needs `nestest.nes` and its reference log, which are **not**
included — the licensing of that ROM is not clearly stated by its author, so it is not
redistributed here. To enable it:

1. Get `nestest.nes` and `nestest.log` (see [the nesdev wiki](https://www.nesdev.org/wiki/Emulator_tests)).
2. Drop both into `nes/roms/`, or configure with `-DNES_TEST_ROM_DIR=<dir>`.
3. Re-run CMake. Without them the test reports itself as skipped.

The harness enters nestest's automated mode at `$C000` and compares `PC`, `A`, `X`,
`Y`, `P`, `SP` and the cumulative cycle count against every line of the log, stopping at
the first divergence. It does not compare the disassembly text: the log's operand
annotations (`= 00`, `@ 80 = 12`) describe resolved addresses that emu6502's
disassembler does not emit. Those columns are for humans; the register columns are what
validate the CPU.

## Design notes

**`Bus` is the seam.** `NesBus` overrides emu6502's virtual `read`/`write` to decode the
address space. The base class's `Memory` pointer is unused.

**`peek()` is separate from `read()` on purpose.** PPU registers change state when read —
`$2002` clears the vblank flag, `$2007` advances the VRAM address — so a memory viewer
using `read()` would corrupt what it is displaying. Everything that inspects memory goes
through `peek()`, and `PeekBus` adapts it for emu6502's disassembler. This costs nothing
now and would be painful to retrofit after a debugger exists.

**No `I6502Emulator`.** That class resets through a `Memory` object; an NES takes its
reset vector from the cartridge via the bus. `Nes` drives `Processor` directly, which is
also the shape a multi-chip system needs — the master clock has to advance the PPU and
APU alongside the CPU rather than letting the CPU pace itself.

**The clock is free-running; the window paces.** `default_clock` counts cycles and never
sleeps, so the CPU runs as fast as it can and `Nes` reports how many cycles went by.
Real time is imposed one level up, by `nes_gui`'s frame deadline. That split is why the
same core can run 30 million instructions in a test harness at full speed and 60.0988
frames a second in a window, with no switch between the two.

**Input is polled once per frame, not accumulated from events.** A game latches the pad
once per frame and sees exactly what was held at that instant, so polling matches the
hardware and avoids inventing key-repeat and event-ordering that the pad does not have.
The one addition is that a key whose press *and* release both arrive inside a single
frame is still reported for that frame — otherwise a tap faster than 16 ms vanishes.

**Mappers answer two questions, and `BankedMapper` does the rest.** Every board here
owns the same three memories and differs only in which byte of PRG a CPU address lands
on and which byte of CHR a PPU address lands on. Those are the two virtuals; the address
decoding, work RAM, CHR RAM and bounds checking live in the base once. Bank indices are
signed, so `-1` is the last bank and `-2` the one before it — which is how boards
describe their fixed windows, and it saves every mapper from counting banks itself.

Moving NROM onto that base was verified by comparing a rendered frame of Super Mario
Bros before and after: byte-identical.

**The PPU asks the cartridge for mirroring on every nametable access** rather than
caching it at load time. MMC1, MMC3 and AxROM all change mirroring at runtime, and some
games change it mid-frame.

**The region comes from the cartridge and reaches every clock.** PAL is not a display
setting — it is a different machine: a slower CPU (1.662607 MHz), 312 scanlines instead
of 262, no odd-frame dot skip, and a PPU that runs at 16 dots per 5 CPU cycles rather
than a clean 3 per 1. That ratio is why `Nes::step` carries a fractional remainder
between steps instead of rounding; rounding would drift a whole scanline every few
hundred instructions. The APU's frame sequencer, noise periods and DMC rates are all
retuned to match, and both front-ends take their frame rate and resampling ratio from
the console rather than from a constant.

Region means PAL versus NTSC, not Japan versus America — Japan and North America both
ran NTSC, so a Famicom cartridge and its NES counterpart are timing-identical.

**The APU mixes nonlinearly and filters its own output.** The hardware sums its five
channels through a resistor ladder, so a channel gets quieter as the others get louder;
a linear sum is audibly wrong, harsh and too loud once more than two channels play. The
two output filters belong in the APU rather than the front-end for concrete reasons: the
high-pass removes the DC offset the positive-only mixer would otherwise carry, which is
a click every time audio starts, and the low-pass is anti-aliasing — samples come out at
1.79 MHz and get decimated about 40:1, so without it everything above the device's
Nyquist folds back down as noise.

## Known gaps

- **CNROM and AxROM have never run a commercial game.** NROM, MMC1, UxROM, MMC3 and 87
  have; those two are covered by synthetic-ROM tests only.
- **No Famicom expansion audio.** The Famicom cartridge connector carries an audio-in
  pin that the NES connector does not, so Japanese carts could add sound chips — VRC6,
  VRC7, Namco 163, Sunsoft 5B, MMC5, FDS. None are supported, and a game that uses one
  will play with those channels silent rather than refusing to run.
- **No Famicom microphone.** The hardwired second Famicom pad has a microphone where
  the NES has Start and Select, read on `$4016` bit 2. A few Japanese games use it;
  here those bits read as zero, which is correct for an NES and wrong for a Famicom.
- **No FDS support** — Famicom Disk System images are a different container entirely.
- **No bus conflicts.** On UxROM and CNROM the value written to ROM is ANDed with the
  byte already there, and a few games depend on it. Every board here takes the written
  value as-is.
- **MMC1 accepts consecutive writes.** Hardware ignores the second write of a
  read-modify-write pair, because it arrives on the very next cycle; this does not, so a
  game using `INC $8000` on the register would behave differently.
- **Saves are written on exit and on reset, not continuously.** Closing the window or
  pressing `R` keeps your game; killing the process loses whatever was written since
  the last of those.
- **No save states** — only the cartridge's own battery RAM persists, the same as on
  hardware.
- **No gamepad support** — keyboard only. SDL's game-controller API would be a small
  addition on top of `Controller`.
- **Opposing directions are not filtered.** Real hardware lets Left and Right close
  together and some games glitch when they do; that filtering belongs to whatever
  drives the input, so it is not done in `Controller`.
- **The APU's timing is cycle-driven but not cycle-exact.** The frame sequencer lands on
  the right CPU cycles, but the `$4017` write delay is approximated, and the DMC charges
  a flat 4 cycles per fetch where hardware varies with what the CPU was doing. Music and
  effects are right; a test ROM measuring the sequencer to the cycle would not be.
- **Scanline-granular rendering.** Each line is drawn from the scroll state at its
  start, so per-line raster effects work but mid-line scroll changes do not. That is
  enough for most games and not enough for a few.
- **Sprite overflow is set by the real 8-per-line rule**, not by hardware's buggy
  evaluation, which both over- and under-reports on real silicon.
- **MMC3's counter is clocked once per scanline**, at dot 260, where the sprite fetches
  begin. Hardware counts rising edges on PPU address line A12, which during rendering
  works out to the same thing — but a game that toggles `$2000`'s pattern-table bits
  mid-line can produce extra edges this will miss.
- **The `$2002` read race is not modelled** — reading exactly as vblank is raised should
  suppress the NMI.
- **OAM DMA always charges 513 cycles**; hardware charges 514 when the write lands on
  an odd CPU cycle, which nothing tracks yet.

## Licence

MIT — see [LICENSE](LICENSE).

No ROMs are included and none are downloaded. `roms/` is ignored, and the two test
suites that need external images (nestest, and Klaus Dormann's 6502 functional test)
report themselves as skipped when the files are absent.

The functional test is GPL-3.0-or-later. CMake **fetches** it at configure time and
never vendors it, so nothing in this repository is a derivative work of it and its
copyleft does not reach this code.

## Next

The short list below is the immediate work; [ROADMAP.md](ROADMAP.md) has the longer
view, including the backend-interface design and what would be needed for a 1.0.

1. **Gamepad support**, then remappable bindings in a config file. Keyboard-only is the
   roughest remaining edge for anyone who just wants to play.
2. nestest, whenever the ROM is available — still the only thing that would validate
   the undocumented opcodes and exact cycle counts against a reference. The `blargg`
   APU and MMC3 test ROMs are the equivalent gates for sound and for the scanline
   counter, and would settle how much the timing approximations above actually matter.
3. A decimal-mode switch in emu6502: the 2A03 ignores the `D` flag in `ADC`/`SBC`, and
   the core currently implements full BCD.
4. Save states — the console's whole state is a handful of plain structs, so this is
   mostly a serialisation exercise, and it makes debugging the harder games practical.
5. Bus conflicts on UxROM and CNROM, and MMC1's consecutive-write rule — the accuracy
   gaps above that a real game is most likely to notice.
6. Famicom expansion audio, if a cart that uses it ever turns up. VRC6 is the usual
   first one, and it would mean letting a mapper contribute to the APU's mix.
