# Roadmap

Where this goes next, and why in that order.

The emulation core is in good shape: the CPU passes Klaus Dormann's functional test,
seven mapper boards cover roughly 80% of the licensed library, both regions run at
measured-correct speed, and the APU has all five channels. What is missing is almost
entirely the layer between that core and a person holding a controller.

## Before tagging 1.0

Three things a player hits in the first hour. None is large; together they are the
difference between a technically impressive emulator and one somebody can use.

**~~Battery-backed saves.~~** Done. A `.sav` beside the ROM, loaded on start and written
on exit and reset. Verified end to end against Zelda: a sentinel planted in the file
survived being loaded, held through a run, and written back.

**~~Gamepad support.~~** Done, via SDL's game-controller API — discovery, hot-plug and a
single layout across every common pad, with the keyboard still live alongside. The
`Controller` class was not touched, which is the point of it.

**~~Persistent configuration.~~** Done. A portable `nes.cfg` beside the executable,
written with the defaults on first run, covering key and pad bindings for both ports
plus window scale, fullscreen and audio. Command-line options override it.

*With those three done, and the repository housekeeping below, everything this section
called a 1.0 blocker is finished.*

**Repository housekeeping**, because these block a public release rather than a
feature:

- *No licence file in either repository.* Without one the default is "all rights
  reserved" and nobody can legally use, fork or contribute. MIT or Apache-2.0 are the
  usual choices for something meant to be read and learned from. Worth noting in the
  README that Klaus Dormann's GPL-3.0 test suite is *fetched at configure time*, never
  vendored, so its copyleft does not reach this code.
- *`nes` cannot be cloned on its own.* Its CMake hard-fails unless `emu6502` sits in a
  sibling directory, so a fresh `git clone` of just this repository will not configure.
  Fix by adding a `FetchContent` fallback to the core's GitHub URL while still
  honouring `EMU6502_DIR` when a local checkout is present — clone-and-build works for
  strangers, and sibling-checkout development keeps working for you.

## Backends: interfaces, not plugins

The proposal was to make video, audio and input into plugins. That is really two
separate ideas, with very different costs.

**An internal abstraction** — `VideoSink`, `AudioSink`, `InputSource` as abstract
classes, with the current SDL code as one implementation of each — is straightforwardly
worth doing. It costs about a day, makes the Zapper and gamepad work land cleanly,
makes the front-end testable without a window, and lets a headless recorder and an SDL
window be the same program with a different backend selected.

**A plugin ABI with dynamically loaded libraries** is a much larger commitment: a
stable C boundary, versioning, discovery, lifetime and ownership across the boundary,
configuration marshalling, crash isolation, and per-platform loading. The historical
record is discouraging — the late-1990s Nintendo 64 and PlayStation emulators built
exactly this, and it fragmented into incompatible plugin versions that users could not
reason about. Every actively maintained emulator that started there has since moved to
built-in backends behind an internal interface.

The one plugin model that demonstrably works inverts the relationship: in **libretro**,
the emulator is the plugin and the frontend supplies video, audio, input, configuration
UI, save states, rewind, netplay and shaders. A libretro core is a few hundred lines on
top of what already exists here, and it would deliver the entire benefit of a plugin
ecosystem without this project having to define, document and defend an ABI.

So the recommendation is: build the interfaces, skip the ABI, and consider a libretro
core if reach is the goal.

That said — if the point is to *learn how a plugin architecture works*, that is a
perfectly good reason and this whole project began as a learning exercise. In that case
build it deliberately as the exercise it is, and keep the internal interfaces underneath
so the decision stays reversible.

## Input, properly

1. **Backend interface**, as above. Everything below plugs into it.
2. ~~**Gamepad support** via SDL's game-controller API.~~ Done.
3. ~~**Remapping**~~ — done, as a binding table per port in `nes.cfg`.
4. **A configuration dialog.** Editing a file works, but binding a key by pressing it is
   what people expect. SDL alone has no widgets, so this needs either Dear ImGui (small,
   self-contained, no external toolkit) or a native dialog per platform. ImGui is the
   pragmatic choice and would also give a debugger UI later. `Config` already round-trips
   through disk, so a dialog would only need to edit the struct and save it.
5. **The Zapper.** A well-scoped and genuinely fun addition: sample the framebuffer at
   the mouse position for brightness, and report it on `$4017` bit 3 (inverted — the
   bit is *clear* when light is seen) with the trigger on bit 4. Duck Hunt is an NROM
   cartridge, so nothing else has to change. The subtlety is that games strobe the
   screen white for a frame to test each target in turn, so the light sense has to
   reflect what is on screen *now* rather than an average.

## Accuracy, when it starts to matter

Roughly in order of how likely a real game is to notice:

- **Test ROMs as gates.** nestest for the CPU, blargg's APU and MMC3 suites for the
  rest. These would settle how much the known approximations actually cost, rather than
  leaving it to guesswork. The APU suite has a specific job waiting for it: the frame
  interrupt currently powers up inhibited, which is a deviation adopted because a real
  game needs it, and only a test ROM can say what the hardware truly does.
- ~~**Bus conflicts** on UxROM and CNROM~~ — done, driven by the NES 2.0 submapper
  rather than guessed from the mapper number. **MMC1's consecutive-write rule** remains:
  hardware ignores the second write of a read-modify-write pair because it lands on the
  very next cycle, and this does not.
- **The `$2002` read race** — reading exactly as vblank is raised should suppress the
  NMI.
- **Mid-scanline rendering.** Currently each line is drawn from the scroll state at its
  start, which handles per-line raster effects but not mid-line changes.
- **A decimal-mode switch in emu6502**: the 2A03 ignores the `D` flag in `ADC`/`SBC`
  and the core implements full BCD. No commercial game depends on this, which is why it
  is this far down.

## Further out

- **Save states.** The console's state is a handful of plain structs, so this is mostly
  serialisation — and it makes debugging the harder games practical.
- **More mappers.** MMC5, VRC2/4, Namco 163, Sunsoft — diminishing returns after the
  seven already present, but MMC5 is interesting in its own right.
- **Famicom expansion audio**, if a cartridge that uses one ever turns up. VRC6 is the
  usual first. It means letting a mapper contribute to the APU's mix, which is a real
  structural change rather than another board.
- **A debugger front-end** — the `peek()` split throughout the codebase exists
  precisely so this can be built without disturbing what it is inspecting.
