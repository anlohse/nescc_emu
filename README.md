# nes

A NES emulator built on the [emu6502](../emu6502) core.

## Status

**Super Mario Bros is playable.** Press Start, the game begins; hold Right and Mario
runs; press A and he jumps over the Goomba. Scrolling, sprites, and the sprite-zero
split that keeps the status bar fixed while the level moves beneath it all work.

| Component | State |
| --- | --- |
| iNES container parsing | 1.0, with trainer support; NES 2.0 read as 1.0 |
| Mappers | 0 (NROM) only |
| CPU bus | RAM + mirroring, PPU/cartridge routing, `peek()` for debuggers |
| CPU | emu6502, which passes Klaus Dormann's functional test |
| PPU timing | dot/scanline/frame counters, vblank, NMI, odd-frame dot skip |
| PPU registers | full register file: write toggle, buffered `$2007`, auto-increment |
| PPU memory | VRAM with nametable mirroring, palette mirroring, CHR via the mapper |
| Background rendering | tiles, attributes, fine and coarse scroll across nametables |
| Sprite rendering | 8x8 and 8x16, flipping, priority, 8-per-line limit, overflow |
| Sprite-zero hit | reported at the dot of overlap, so mid-frame splits work |
| OAM DMA (`$4014`) | copies the page and stalls the CPU 513 cycles |
| Controllers (`$4016/$4017`) | both ports, latch and shift register, open bus in the high bits |
| Input source | scripted only — `--press`; no keyboard yet |
| Output | headless — `--screenshot` writes a PPM; no window yet |
| APU `$4000-$4015` | **stubbed** — reads return 0, writes counted but inert |

## Layout

```
include/nes/     public headers
src/             Cartridge (iNES), Mapper (NROM), Ppu, Controller, NesBus, Nes, the CLI
test_src/        doctest suite, including the nestest harness
```

## Building

```bash
cmake -S . -B build && cmake --build build --config Release
```

The 6502 core is pulled in from a sibling checkout with `add_subdirectory`. If it lives
elsewhere:

```bash
cmake -S . -B build -DEMU6502_DIR=/path/to/emu6502
```

Its own tests and debugger stay off when consumed this way — build those from that
project directly. Everything lands in `build/bin/<config>/` so `emu6502_lib.dll` sits
beside the executables on Windows.

## Running

```bash
./build/bin/Release/nes_run rom.nes --trace --stop-on-trap
```

```
C000  A9 42     LDA $42        A:00 X:00 Y:00 P:24 SP:FD CYC:7
C002  8D 00 02  STA $0200      A:42 X:00 Y:00 P:24 SP:FD CYC:9
```

Options: `--trace`, `--start-pc=HEX`, `--max=N`, `--stop-on-trap`. Disassembly goes
through a read-only `PeekBus`, so tracing never disturbs device state.

On exit it summarises PPU state, which is how you check a game is alive while there is
still nothing to look at:

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

### Input

There is no window yet, so buttons are scripted on the command line with `--press`,
which takes `BUTTON@FRAME[:HELD][/PORT]` and can be repeated:

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

**The clock is free-running.** `default_clock` counts cycles and never sleeps. Pacing
belongs one level up, once there is a frame to pace against.

## Known gaps

- **No window and no live input** — buttons are scripted with `--press` and frames only
  come out through `--screenshot`.
- **Opposing directions are not filtered.** Real hardware lets Left and Right close
  together and some games glitch when they do; that filtering belongs to whatever
  drives the input, so it is not done in `Controller`.
- **No APU**, so no sound, and no DMC IRQ.
- **Scanline-granular rendering.** Each line is drawn from the scroll state at its
  start, so per-line raster effects work but mid-line scroll changes do not. That is
  enough for most games and not enough for a few.
- **Sprite overflow is set by the real 8-per-line rule**, not by hardware's buggy
  evaluation, which both over- and under-reports on real silicon.
- **The `$2002` read race is not modelled** — reading exactly as vblank is raised should
  suppress the NMI.
- **OAM DMA always charges 513 cycles**; hardware charges 514 when the write lands on
  an odd CPU cycle, which nothing tracks yet.

## Next

1. A window and a frame-paced main loop, with the keyboard wired to `Controller` — the
   point where `emu6502`'s clock abstraction moves up to the system level, and the point
   at which this stops being a batch job.
2. A decimal-mode switch in emu6502: the 2A03 ignores the `D` flag in `ADC`/`SBC`, and
   the core currently implements full BCD.
3. nestest, whenever the ROM is available — still the only thing that would validate
   the undocumented opcodes and exact cycle counts against a reference.
4. More mappers — MMC1, UNROM, MMC3 (which needs a scanline-counter IRQ).
5. APU.
