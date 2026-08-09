A NES emulator in C++17, built on the [emu6502cc](https://github.com/anlohse/emu6502cc)
6502 core.

Super Mario Bros, The Legend of Zelda, Mega Man 3 and Super Mario USA run with picture,
sound and controls.

## What it emulates

- **CPU** — the full 6502 including undocumented opcodes, verified against Klaus
  Dormann's `6502_functional_test`
- **PPU** — background and sprite rendering, scrolling across nametables, 8x8 and 8x16
  sprites, and sprite-zero hit reported at the dot of overlap so mid-frame splits work
- **APU** — two pulses with sweep, triangle, noise and DMC, mixed through the hardware's
  nonlinear curve rather than summed
- **Mappers** — NROM, MMC1, UxROM, CNROM, MMC3 with its scanline IRQ, AxROM, and 87,
  covering roughly 80% of the licensed library
- **Regions** — NTSC and PAL, taken from the cartridge header, measured at 60.0988 and
  50.0070 Hz
- **Controllers** — both ports, with the latch and shift register a game actually reads,
  keyboard or gamepad, rebindable by pressing the key you want
- **The Zapper** — the mouse as a light gun, sensing light from the picture itself
- **Battery saves** — a `.sav` beside the ROM, for cartridges with a save chip

## Not yet

Video and input are still compiled in rather than loadable; only audio ships as a real
plugin so far, and the settings dialogs exist on Windows only. See `ROADMAP.md` for what
comes next and why in that order.
