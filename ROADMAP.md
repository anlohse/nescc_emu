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

**~~An internal abstraction~~** — done. `VideoSink`, `AudioSink`, `InputSource` and
`Clock` are abstract classes in `src/frontend/Backend.h`, the run loop lives in
`App.cpp` and contains no SDL, and the SDL code is one implementation of each.

The fourth interface was not in the original plan, and the reason it exists is worth
recording: pacing needs a clock, and a `waitForNextFrame()` on the backend would have
hidden the deadline arithmetic — the part that actually keeps the emulator from
drifting — inside the untestable half. Splitting it into `now()` and `sleep()` puts
that arithmetic in the loop where it can be read and tested, and leaves the backend
only the genuinely host-specific job of sleeping accurately.

Writing the tests found a bug in the first attempt, which is the return on the whole
exercise arriving early: the loop sleeps the bulk of a wait and then spins on `now()`
for the last millisecond. Against a fake clock that only moves when asked, that spin
never terminates. The spin belongs in the backend, and the interface now says so.

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

So the recommendation was: build the interfaces, skip the ABI, and consider a libretro
core if reach is the goal. **The decision taken was to build the ABI anyway**, as the
learning exercise this project has been from the start — with the internal interfaces
underneath, so it stays reversible.

### The ABI, in four stages

Two things found while writing the SDL backends shaped the design, and both are worth
knowing before reading the header:

- **Video and input cannot be independent modules.** `SDL_PollEvent` drains one queue
  carrying window, keyboard and pad events alike, so two separately loaded libraries
  would have to agree about who owns it. The host therefore owns the window and the
  event pump and forwards what it drains — which is also what kills the "which video
  plugin works with which input plugin" combinatorics that made the N64 plugin scene
  miserable.
- **The Zapper needs video data inside an input plugin.** Light sensing samples screen
  brightness where the gun is pointed. Routing that through host services
  (`nes_host::get_frame`) rather than plugin-to-plugin keeps the dependency graph a star
  and means a controller plugin never has to know which video plugin is loaded. This is
  the reason to settle the ABI *before* the Zapper rather than after.

1. ~~**The C boundary, with the backends still compiled in.**~~ Done.
   `src/plugin/nes_plugin.h` is the ABI; `PluginHost` is the registry, the version
   handshake and the adapters that present a C api as the C++ interface `App` already
   speaks. The SDL backends reach the run loop through it on every run, so the boundary
   cannot rot unnoticed — a mistake in its shape is a compile error today rather than a
   crash in a stranger's library later.
2. ~~**Real shared libraries**, audio first.~~ Done for audio. `plugins/audio_sdl.dll`
   is loaded from a folder beside the executable, and a module found there shadows the
   built-in of the same id -- which is the point of being able to drop one in. Nothing
   in the api structs changed, exactly as predicted; what got added is `Module`
   (LoadLibrary/dlopen behind RAII), directory scanning, and the typed creation below.

   Two things worth recording. **An instance keeps its library mapped**: every function
   it calls lives in the library's address space, so `Module::create<T>()` hands the new
   object a `shared_ptr` to its own module rather than trusting a vector somewhere to
   stay in scope. Getting that wrong crashes at shutdown, on someone else's machine, in
   a stack trace naming nothing, so the types enforce it instead of a comment.
   **The kind check cannot be a cast**: the api is a C struct with no RTTI, and a
   `static_cast` from `void*` would reinterpret an audio api as a video one and crash on
   the third call. `PluginTraits<T>` says what is expected at compile time and the
   descriptor is checked at run time.

   Video and input remain built in. They are harder for a real reason -- the host owns
   the window and the event pump -- and are worth doing after the dialogs, when there is
   something to configure.
3. **Dialogs.** The host's chooser is ~~done~~: `F1` while playing, or `--settings` with
   no ROM at all, lists what is installed for each job, says whether each came from a
   file or is built in, and writes the choice to `nes.cfg`. It applies on the next
   launch — swapping a video or input plugin under a running emulator would mean tearing
   down the window and the event queue the dialog is itself running on.

   What it decides lives in `PluginSettings` and is tested with no window: that a choice
   persists, that Cancel does not apply, that a config naming a deleted plugin shows the
   fallback rather than a name nothing matches. Only the presentation is per-platform,
   and only Win32 exists so far; elsewhere it says so instead of opening nothing.

   The controller plugin now has its own dialog, reached from that chooser: pick a
   group, pick a button, press Bind, then press the key or gamepad button you want.
   Anything else in the same group that already had it is released, because one key
   driving two NES buttons is never what someone meant and finding out while playing is
   worse than watching the old binding go.

   The key mapping is the part worth knowing about. Printable keys are resolved through
   the keyboard layout and SDL's own keycode table, so a French or German keyboard binds
   the key that was pressed rather than the one in that position on a US board; only the
   keys producing no character need a table. A key SDL cannot name is refused out loud
   rather than guessed at.

   ~~Still to come: `configure()` for video and audio.~~ Done, and it needed something
   built first. The audio plugin people actually run is `audio_sdl.dll`, which links
   against SDL and nothing else — no `nes_lib`, no `GuiConfig` — so it cannot read or
   write `nes.cfg`, and a dialog on the built-in copy would have been a dialog on the
   copy that gets shadowed. A plugin needed somewhere to keep its settings.

   The answer is that the host keeps them. `nes_host` grew `get_setting` and
   `set_setting`, namespaced by plugin id, persisted into `[plugin.<id>]` sections of
   the one configuration file the program already owns. A plugin opening a file beside
   itself instead is how a program ends up with four configuration files in three
   formats. Settings belonging to a plugin that is not installed on this machine
   survive the round trip, because losing them silently is how a config file stops
   being trusted. This also made `nes_host` real: it was being passed as null
   everywhere until something needed it.

   **Who owns a setting turned out to be the whole design question.** Window scale and
   fullscreen look like video settings and are not: the host reads them and passes them
   to whichever video plugin is loaded, so the host's chooser is where they are edited
   and the chooser grew a Window row for them. What the video plugin owns is what only
   it knows — the scaling filter, and whether a console pixel is square or the 8:7 a
   television actually showed. Audio owns its device, its volume and its buffer size.
   Two authors for one setting is how a command line and a dialog end up disagreeing.

   The dialogs are three combo boxes and an OK, twice, which would have been the third
   and fourth copy of the same eighty lines of Win32 — so those live in
   `plugin/FieldsDialog.h`, header-only, because a plugin links nothing of the host's
   and anything shared with it has to be shareable without a library. It registers its
   window class per module rather than per process: two modules using the same header
   must not be handed each other's window procedure, which is visible in the running
   program as two class names, `nesFieldsDialog<host>` and `nesFieldsDialog<dll>`.
4. ~~**The Zapper**, as a second controller plugin. The proof the architecture holds.~~
   Done, and the architecture held: what the ABI grew is two functions on the end of
   two existing structs — `poll_zapper` on the input api, `window_to_frame` on the
   video one — plus a `nes_zapper_state` carrying its own size, set by the host, so a
   newer plugin cannot write past an older host's buffer. Neither plugin can do this
   alone. The input plugin knows where the mouse is in *window* pixels; the video
   plugin is the only thing that knows how the picture sits inside the window. The host
   joins them, which is what keeps the graph a star.

   Two things were wrong in the references and had to be settled against the game.
   **Bit 4 is active low too.** With the bit set while the trigger is held, Duck Hunt
   consumes a bullet and never starts its shot sequence at all; with it clear, the
   screen blanks four frames later exactly as it should. Both bits being active low is
   the more sensible piece of hardware anyway — a closed switch and a conducting
   phototransistor each pull their line down. **The brightness threshold matters more
   than it looks.** Duck Hunt's foliage green has a luma of 172, so a threshold of 160
   made scenery count as light, and the game's own "screen blanked, so I should see
   nothing" check then discarded *every* shot. It is 200 now; the strobe is pure white.

   Verified end to end against the multicart: `--shoot=237,124@980:12` scores
   `001000` and marks the duck hit, `--shoot=120,40@980:12` scores nothing and the duck
   flies on. The aim point came from instrumenting the emulator to report the brightest
   thing on screen each frame, which found the strobe as a 32x32 white box at
   (222-253, 109-140) — 190 pixels from where reading a screenshot had suggested.

## Input, properly

1. ~~**Backend interface**~~ — done. Everything below plugs into it.
2. ~~**Gamepad support** via SDL's game-controller API.~~ Done.
3. ~~**Remapping**~~ — done, as a binding table per port in `nes.cfg`.
4. ~~**A configuration dialog.**~~ Done, as a native dialog rather than Dear ImGui. The
   choice went that way because the dialog has to be reachable while the emulator is
   running and must not fight SDL for the event queue: a modal native window runs its
   own loop and hands control back when it closes. `Config` already round-tripped
   through disk, so the dialog only edits the struct and saves it. Only Win32 exists so
   far; elsewhere it says so rather than opening nothing.
5. ~~**The Zapper.**~~ Done — see the plugin section above for what it cost and what
   the hardware references got wrong. The subtlety anticipated here was the real one:
   a game strobes the screen white for a frame to test each target in turn, so the
   light sense reads the framebuffer as it stands rather than an average, and the aim
   point is applied before the frame runs so it is already in place when the game reads
   the gun partway down the picture it is drawing.

## Accuracy, when it starts to matter

Roughly in order of how likely a real game is to notice:

- ~~**Test ROMs as gates.**~~ Done, and worth every minute. `nes_rom_test` runs 93 of
  blargg's ROMs through the `$6000` status protocol — a result read rather than looked
  at — in its own target, because it takes minutes where the unit suite takes
  milliseconds. **38 pass, 30 fail for reasons now written down, 25 are older ROMs that
  only draw their answer on screen.** Every failure is listed in `testBlargg.cpp` with
  its cause, so a *new* failure is a regression and a fixed one tells you to delete its
  entry.

  Two things fell out of the first run, both invisible to every test written here:

  **Reset was clearing the machine.** `Nes::reset()` wiped the 2 KB of RAM and zeroed
  A, X and Y. Hardware does neither: reset decrements the stack pointer three times,
  sets `I`, and jumps through the vector. A game can tell a warm boot from a cold one by
  what survived, and pressing `R` in the emulator was destroying it. Found because
  `ram_after_reset` did not fail — it *hung*, restarting forever on a machine that kept
  forgetting.

  **The 2A03 has no decimal mode**, which was the last bullet in this section on the
  grounds that no commercial game depends on it. It was the single largest cluster of
  failures: seven of `instr_test-v5`'s sixteen ROMs, every failing opcode an `ADC`,
  `SBC`, `RRA` or `ISC`. Fixed in the core as a switch, and the switch is phrased as a
  *disable* so that the `memset` several places use to reset a register file cannot
  silently flip it — the first version did exactly that and broke Klaus Dormann's
  decimal tests.

  ~~The largest clusters left are the MMC3's A12 clocking (6 ROMs), the dots around the
  vblank edge (7), and interrupt latency inside the CPU core (5).~~ The MMC3 cluster is
  mostly closed: **42 of 93 pass now**.

  The counter on that board is not a scanline counter, whatever everyone calls it. It
  counts rising edges on PPU address line A12, and the PPU now produces that waveform
  from the fetch schedule rather than pretending: two dots of nametable, two of
  attribute, four of pattern data, repeating, with A12 high only while a pattern fetch
  is reading the upper table. An edge counts only after the line has been low for nine
  dots, which is what discards the every-eight-dots chatter a background in the upper
  table produces and leaves the once-a-line crossing between background and sprite
  fetches — the edge a game actually times against.

  The payoff is that clocking now works the way the hardware's does, including from
  `$2006` writes with the screen off, which nothing here handled before. Four of the six
  `mmc3_test` ROMs pass; `4-scanline_timing` says the edge is still a few dots early,
  and MMC6 is not implemented at all.

  Two of the old unit tests had to change, and the change is the interesting part: they
  enabled rendering with backgrounds *and* sprites in `$0000` and expected IRQs. On
  hardware A12 never rises in that configuration and the counter never runs, so a game
  wanting this interrupt has no choice about where it puts its tiles. The tests said
  what the approximation did; they now say what the board does.

  All ten commercial ROMs render byte-identical frames across the change, which is the
  expected result: they all use one table for backgrounds and the other for sprites,
  where an A12 model and a per-scanline count agree exactly.

  The vblank cluster gave up one more: an NMI raised by enabling it in `$2000` while
  the flag is already set is taken after the *next* instruction, not this one. The write
  lands in its instruction's final cycle, past the point where the CPU samples /NMI. A
  unit test here had asserted the opposite in as many words -- "fires immediately" -- so
  it was wrong in the same direction as the code, which is the failure mode these ROMs
  exist to catch. **43 of 93 pass.**

  Three more of that cluster fell once I stopped guessing and **read blargg's own
  readme**, which prints the expected table for every one of these ROMs. Two hypotheses
  died first, both cheaply, and both worth recording because each would have been a
  plausible thing to "fix" blind:

  - *The CPU/PPU phase.* A real console fixes it at power-on and the PPU can start 0, 1
    or 2 dots into a CPU cycle. Sweeping all three changed nothing -- not the counts,
    not even `05-nmi_timing`'s checksum. These ROMs synchronise to vblank by polling
    `$2002` before measuring anything, so a constant offset is absorbed by the sync.
    They are built to be immune to exactly that.
  - *Accesses dated three dots late.* The core charges an instruction's cycles in one
    lump at the end, so the PPU's position within an instruction comes entirely from the
    bus. For a four-cycle, four-access instruction like the `LDA $2002` these ROMs use,
    accesses and cycles line up exactly.

  What the tables actually asked for was a redistribution, not a shift. **The flag has a
  one-dot window at each end where a read wins**, and they are different mechanisms: at
  the top of vblank a read on the set dot cancels the flag outright, and the interrupt
  is lost for *three* dots rather than two; at the bottom the flag is genuinely down but
  a read landing on that dot still returns it set. Moving the clear itself instead of
  the readback made `03-vbl_clear_time` pass and broke `07-nmi_on_timing`, which is what
  told them apart -- enabling NMI on that dot must raise nothing.

  A unit test here had encoded the old window, including a read one dot *before* the
  flag being suppressed, which blargg says is an entirely ordinary read.

  One dead end: `10-even_odd_timing` reports the odd-frame skip as decided too late, and
  latching the rendering flag a dot earlier changed nothing it measures. Reverted rather
  than kept -- an unproven behavioural change that does not fix its test is worse than
  none.

  **46 of 93 pass.** What is left of this cluster is `05-nmi_timing` and
  `08-nmi_off_timing`, which are about which CPU cycle the interrupt is taken on rather
  than which dot the flag moves, and the odd-frame skip.

  The other large cluster is interrupt latency inside the CPU core (5 ROMs).
- ~~**Bus conflicts** on UxROM and CNROM~~ — done, driven by the NES 2.0 submapper
  rather than guessed from the mapper number.
- ~~**MMC1's consecutive-write rule.**~~ Done, and it needed fixing in the CPU first: a
  read-modify-write instruction writes the unmodified byte back before the result, one
  cycle apart, and the core was only ever emitting the second. Now it emits both, and
  MMC1 keeps the first and drops the second — so `DEC $8000` shifts in a bit of what was
  already at that address, which is what the games expecting it were written against.
- ~~**Advance the PPU inside the bus access.**~~ Done. `Nes::step()` used to run a whole
  instruction and only then tick the PPU, dating every device access to the instruction
  boundary before it — up to 21 dots early, measured on Super Mario Bros' own vblank
  loop, which reads `$2002` every seven CPU cycles. `NesBus` now advances the PPU and
  APU one cycle before serving each access, and settles the difference when the
  instruction ends. A register is read at the cycle its access happens, to the dot.
  Bus accesses are not quite cycles — a taken branch, a page-crossing read and the
  internal cycles of a stack operation each cost one without touching the bus — so the
  distribution is still an approximation, just a far finer one. All nine test ROMs
  render byte-identical frames, which says the totals did not move.
- ~~**The `$2002` read race.**~~ Done. Three dots at the top of vblank decide it, and the
  CPU loses all three: reading one dot early cancels the flag before it comes up,
  reading on the dot returns it clear, and reading one dot late returns it set but still
  loses the interrupt, because /NMI went down and came back up inside a single dot. What
  varies across the three is only what the flag reads back as; the interrupt is gone
  either way. Measured across the ROMs here it fires about once every three thousand
  frames, and `Ppu::vblankRaces()` counts it so the number is never a guess. Still worth
  checking against blargg's `ppu_vbl_nmi`, which tests exactly this.
- ~~**Mid-scanline rendering.**~~ Done. A line is still drawn from the state at its
  start, and is now redrawn from partway across whenever a write changes something the
  rest of it depends on: `$2001`, the background pattern-table bit of `$2000`, fine X,
  or `v` itself. A line nothing is written during is untouched, which is why every ROM
  renders identically.

  Worth recording what the measurement found, because it decides how much this is worth:
  of the ten ROMs here, **only Super Mario Bros writes mid-line at all** — the fine X of
  its sprite-zero split, 355 times in a thousand frames of play. Not one of those
  redraws changed a single pixel, because the split lands on a uniform band of sky where
  a few pixels of horizontal shift look exactly the same. The behaviour is real and now
  correct; nothing available demonstrates it, so it is pinned by unit tests instead.
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
