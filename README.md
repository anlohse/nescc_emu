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
| Mapper IRQ | MMC3's counter, clocked by rising edges on PPU A12, merged with the APU onto one CPU line |
| Bus conflicts | on the discrete boards, when the NES 2.0 submapper declares them |
| MMC1 serial port | five-bit shift register, with the consecutive-write rule applied |
| CPU bus | RAM + mirroring, PPU/cartridge routing, `peek()` for debuggers |
| Device timing | the PPU and APU advance per bus access, so a register is read at the cycle it is read at |
| CPU | emu6502, which passes Klaus Dormann's functional test |
| PPU timing | dot/scanline/frame counters, vblank, NMI, odd-frame dot skip, the `$2002` race |
| PPU registers | full register file: write toggle, buffered `$2007`, auto-increment |
| PPU memory | VRAM with nametable mirroring, palette mirroring, CHR via the mapper |
| Mirroring | horizontal, vertical, four-screen, both single-screen modes; switchable at runtime |
| Background rendering | tiles, attributes, fine and coarse scroll across nametables |
| Mid-line changes | scroll, mask and pattern-table writes redraw the rest of the line |
| Sprite rendering | 8x8 and 8x16, flipping, priority, 8-per-line limit, overflow |
| Sprite-zero hit | reported at the dot of overlap, so mid-frame splits work |
| OAM DMA (`$4014`) | copies the page and stalls the CPU 513 cycles |
| Controllers (`$4016/$4017`) | both ports, latch and shift register, open bus in the high bits |
| Zapper | the mouse as a light gun on port two: light sensed from the picture, both bits active low |
| Battery saves | `.sav` beside the ROM, loaded on start, written on exit and reset |
| APU channels | two pulses with sweep, triangle, noise, DMC — all five |
| APU frame counter | 4- and 5-step sequences, quarter/half clocks, frame IRQ |
| APU mixing | the hardware's nonlinear curve, high-pass and anti-alias filtering |
| Backends | video, audio, input and clock behind interfaces; SDL is one implementation |
| Plugin ABI | C boundary with a version handshake; the SDL backends already go through it |
| Loadable plugins | `plugins/audio_sdl` is a real shared library; a module shadows the built-in of the same id |
| Plugin chooser | `F1`, or `--settings` with no ROM: pick a plugin per job and the window size, saved to `nes.cfg` |
| Brightness | a gamma curve on the palette, sharing one pass with the CRT lift |
| Controllers | one device per console port, chosen by name — keyboard or a numbered gamepad |
| Rebinding | the controller plugin's own dialog: press Bind, then press the key or pad button |
| Plugin settings | each plugin's own dialog — video's scaling style and pixel shape, audio's device, volume and buffer |
| CRT style | a quadratic stretch, then a mask multiplied over it — a television's slot mask or a monitor's grille, at either of two pitches |
| Menu bar | native, on Windows: Emulation, State, Settings, Help — every item implemented |
| Loading ROMs | from the menu or the command line; the window opens empty without one, and remembers the last eight |
| Save states | eight slots beside the ROM, with the whole machine in them: RAM, both chips, the cartridge's registers, and where the beam is |
| Host services | plugins read the frame, the window handle and their own settings through `nes_host` |
| Window | `nes_gui`: SDL2 video and audio, keyboard, paced to NTSC's 60.0988 Hz |
| Headless runner | `nes_run`: tracing, scripted input, PPM screenshots, WAV capture |

## Layout

```
include/nes/     public headers
src/             Cartridge (iNES), Mapper (seven boards), Ppu, Apu, Controller,
                 NesBus, Nes, main.cpp (headless runner)
src/plugin/      nes_plugin.h -- the C plugin ABI: version, descriptor, api structs
                 PluginHost   -- registry, version handshake, C-to-C++ adapters
                 Module       -- LoadLibrary/dlopen behind RAII, typed creation
                 FieldsDialog -- header-only, so a plugin can put up a dialog
                                 without linking anything of the host's
src/plugins/     audio_sdl -- the first backend to leave the executable
src/frontend/    Backend.h  -- video, audio, input and clock as abstract classes
                 App.cpp    -- the run loop, written against those and nothing else
                 HostServices -- the host answering nes_host: frame, window, settings
                 SdlBackend -- one SDL implementation of each
                 SdlPlugin  -- those, exported through the C ABI
                 gui_main.cpp is now only wiring: parse, init, select, run
test_src/        doctest suite, including the nestest harness
```

The four interfaces in `frontend/Backend.h` are the seam between the emulator and
the machine it runs on. Everything host-shaped -- a window, a sound device, a
person pressing keys, a wall clock -- arrives through one of them, so the run
loop contains no SDL and `nes_gui_test` drives the whole thing with doubles: no
window, no device, and time that only moves when the test says so.

Underneath them is a C plugin ABI in `src/plugin/nes_plugin.h`. Video, audio and
input each reach the run loop through it, and the SDL implementations are already
plugins in every sense except that they are still compiled in rather than loaded:
the same version handshake, the same descriptor, the same struct of function
pointers a shared library will export. Exercising the boundary on every run is what
keeps it from rotting while nothing external uses it yet.

Audio has already left the executable: `plugins/audio_sdl` is a real shared library,
loaded from a folder beside the program, and a module found there shadows the
built-in with the same id. Delete it and the emulator falls back and still makes
sound. Video and input are still compiled in, for a reason worth knowing --

The host owns the window and the event pump. That is not an accident of this
implementation: one platform event queue carries window, keyboard and pad events
together, so two separately loaded modules cannot both own it.

Traffic runs the other way too. `nes_host` is what a plugin may ask of the program:
the frame currently on screen, the window handle to hang a dialog on, and its own
settings. That last one is why a plugin has no configuration file of its own — the
host already owns one, already knows where it belongs on this platform, and is the
only party that can keep the whole thing consistent. A plugin's settings land in a
`[plugin.<id>]` section of `nes.cfg` and survive a load-and-save round trip even on a
machine where that plugin is not installed, because losing another program's settings
is how a config file stops being trusted.

Which settings belong to whom is a real line rather than a tidy one. Window scale and
fullscreen are the *host's*: it reads them and hands them to whichever video plugin is
loaded, so they are edited in the host's own chooser. The filter and the pixel shape
are the *video plugin's*, because nothing else knows how it draws. Two authors for one
setting is how a command line and a dialog end up disagreeing.

The lifetime rule is the other thing to know before writing a plugin. An instance
keeps its library mapped, because every function it calls lives inside that library;
`Module::create<T>()` hands each new object a reference to its own module rather than
relying on something staying in scope. And because a C api struct carries no RTTI, the
kind is checked against the descriptor at run time -- a cast would happily reinterpret
an audio plugin as a video one and crash later.

[ROADMAP.md](ROADMAP.md) has the four stages and the constraints that shaped them.

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

| | Player 1 | Player 2 | Gamepad |
| --- | --- | --- | --- |
| D-pad | arrows | numpad 8 4 5 6 | d-pad or left stick |
| A / B | Z / X | numpad 1 / 2 | A or B / X or Y |
| Start / Select | Enter / Right Shift | numpad Enter / + | Start / Back |

**Each console port reads one device, and you choose which.** Settings > Configure
Controller asks the two questions in the order a player thinks of them: which player, then
what they are holding — the keyboard, or a gamepad picked from a list of what is actually
attached. The choice is saved per port and nothing is inferred from it.

That is deliberately less clever than picking up whichever pad appears first, and the
reason is a real controller. A twin USB adapter presents *one HID collection per socket*,
so it reports two gamepads whether or not two pads are plugged in — and SDL enumerates them
in an order that need not match the labels on the shell. On the hardware this was built
against, the adapter's first socket enumerated second, so "first pad found becomes player
one" quietly gave player one a socket with nothing in it. No auto-detection can fix that,
because nothing distinguishes an empty socket from an idle pad. Choosing does.

It also makes two people playing unambiguous: a port set to a gamepad ignores the keyboard,
so there is no question about who pressed what. Anything SDL recognises works — an Xbox pad,
a DualShock, a generic USB adapter — because SDL's game-controller layer maps them all onto
one layout first. Pads can be connected or removed while a game is running; a port whose
chosen gamepad is absent simply reads nothing, and the dialog still lists it and says so.

Both face-button diagonals are accepted, because the NES has two buttons and a modern pad
has four; binding only one pair makes the other half feel broken.

### Configuration

Everything above can be rebound. A `nes.cfg` is written beside the executable on the
first run, already filled in with the defaults, so there is nothing to look up — open it
and change what you want:

```ini
[video]
scale = 3
fullscreen = false

[keyboard1]
a = Z
b = X
select = Right Shift
start = Return
up = Up

[pad1]
a = a
b = x
start = start
```

It lives beside the program rather than in a user profile directory, so the released zip
stays portable: unpack it anywhere, take the folder with you, nothing is installed and
nothing is left behind.

Names are SDL's own — `Right Shift`, `Keypad 8`, `dpup`, `leftshoulder` — because SDL
converts both directions, so the names it writes are exactly the names it reads back.
A binding it does not recognise is reported and skipped rather than rejecting the file:
one typo costs that binding, not the rest of your setup.

**A ROM is no longer required to start.** With none named the window opens empty and says
so, and one can be loaded from Emulation > Load ROM or from the list of the last eight.
Naming one on the command line still works and still wins -- and a bad name there is
still a failure to start rather than a silent empty window, because somebody who typed a
path wants to hear that it was wrong.

Loading is a cold boot, not a reset: a cartridge going into a slot does not inherit the
RAM of the game that just left. It also re-times the loop, because the region comes with
the cartridge -- loading a PAL game into an emulator that started empty has to move it to
50 Hz, or it runs the console fast with audio to match.

There is a menu bar now, on Windows, so none of the above has to be memorised: **Emulation**
(reset, hard reset, pause, frame advance, mute, screenshot), **State**, **Settings** and
**Help**. Every item on it now does something. The rule that got it there is still enforced by a
test: an item may be listed before it is built, but then it has to be visibly disabled --
a gap somebody can see beats one they have to guess at, and an enabled item that does
nothing is a bug report.

**Help > Keys and Buttons** lists every binding, read from the configuration rather than
from a table written by hand: a page that still says `Z` after somebody has moved A to `Q`
is worse than no page at all.

Two things about it are worth knowing. The window belongs to the *video plugin*, and the
menu is host business, so the host attaches a menu to a window it did not create using
the handle the plugin already exports for dialogs. That is a deliberate exception to the
plugin owning its window: the alternative was an ABI call obliging every video plugin
ever written to host a menu. And the window grows by exactly the menu's height, so the
picture keeps the size that was asked for instead of losing a strip to it.

**Scaling** offers Sharp, Smooth, and **CRT television** and **CRT monitor** each at two
pitches. The CRT styles do two separate things in the order a television did them. First
they stretch the picture soft, because nothing about a television was sharp — the beam was a
spot with soft edges, the signal was bandwidth-limited, and the phosphor spread whatever
light it got. Then they multiply a mask over the result:

```
[R  G  B ]
[R  G  B ]
[r  g  b ]   <- dimmer, where the beam was fading between lines
```

The order is the whole thing. Doing the mask first and stretching afterwards gives a grid
of coloured squares; blurring first is what makes it read as a television. It also costs
nothing — the stretch is what a renderer does anyway, and the mask is one blended draw, so
no pixel is touched per frame and it works at any window size rather than only at 3x.

Where the mask lives matters too. The stripes are a property of the *screen* — they were in
the glass, at a pitch that had nothing to do with what resolution was being displayed — so
the mask is built in output pixels. Scanlines are the other way round: they are in the
*signal*, one per line the console drew, so their spacing follows 240 rather than the
window.

**A television and a monitor were different glass**, which is why both are offered rather
than one being a "better CRT" setting. A monitor — and a Trinitron television — used an
aperture grille: unbroken vertical stripes running the whole height of the tube. Most
televisions used a *slot mask*, where the colour columns run straight down just the same
but each is broken into short slots, and the bridges between one column's slots sit half a
slot from its neighbour's:

```
R  G  B    r  g  b
R  G  B    R  G  B
r  g  b    r  g  b
```

A brick wall stacked sideways — the colours stay in step and the *gaps* alternate. It is
much of why a television never looked like a monitor showing the same picture.

That costs nothing to add, because a slot's height is a scanline's height: the bridge is
where the beam was already fading, so the stagger shifts the beam by half a line and needs
no geometry of its own. The beam integral takes a position rather than an index, so half a
line is no harder to ask for than a whole one. And shifting a periodic profile cannot change
what a whole period of it integrates to, so a staggered column is exactly as bright as an
aligned one — measured on screen, the two masks come out at 112.4 and 112.5 average luma.

It also *reduces* the horizontal banding rather than adding any: alternating gaps break up
the continuous dark rows a grille has, measuring 0.991 row-to-row uniformity against the
grille's 0.968.

**Both come at two pitches**, which is a second and independent choice: how many screen
pixels one red-green-blue triad spans. Three is one pixel per phosphor, the coarse and
obvious mask. Two is a finer glass — the same three phosphors in two pixels — and every
screen pixel then straddles a stripe boundary instead of sitting inside one.

That is exactly where a mask goes wrong. Write the coverage out by hand and the obvious
table gives red and blue a whole stripe each and green two halves, which is a fifth less
green than red; a mask short of one channel does not dim a picture, it *tints* it, and the
tint of missing green is violet. So coverage is integrated rather than tabulated: the
continuous stripes are integrated over each screen pixel, which cannot favour a channel
because the three stripes are the same width. Both pitches then fall out of one expression,
and measured on Mario's sky the four styles agree to a fifth of a level in every channel —
109.1, 102.3, 173.3 at a pitch of three against 109.1, 102.5, 173.1 at two. Changing the
pitch changes how fine the mask looks and nothing else.

**The stretch is quadratic, not linear**, and it costs almost nothing because of what a
B-spline is. Stretching linearly reconstructs the picture with a tent — the order-1
B-spline. A quadratic stretch uses the order-2 one, and B-splines are boxes convolved
together, so `B2 = B1 * B0`: a quadratic stretch *is* a linear stretch of a picture blurred
by one more box. SDL offers nearest and linear and nothing else, so the box is done here, to
the 256×240 frame, and the tent is left to the renderer — three taps each way over 61,440
samples rather than a resample of every pixel in the window.

Doing it at source resolution is also the more faithful place. Softness is bandwidth, a
property of the *signal* like the scanlines and unlike the mask, so it is measured in
console pixels and must not change when the window is resized.

The weight is derived rather than chosen. How much blur a kernel carries is its variance,
and variances add when kernels convolve: a tent's is 1/6 and `B2`'s is 1/4, so the kernel in
front of it must carry 1/12, and a symmetric three-tap `[t, 1-2t, t]` has variance `2t`.
Hence `t = 1/24`, which is what `CRT_SOFTEN` is. Anything larger is a blurrier picture, not
a rounder one. It is a small change on purpose — at these magnifications a linear stretch is
already most of the blur — and measured against the same frame rendered with the weight at
zero it moves 28% of the screen's pixels, by 0.8 levels on average and 33 at the sharpest
edges. Flat colour is untouched, which is the point: the weights sum to one.

Three things about it came out of measuring screenshots rather than from taste, and each
one changed the design:

- **Brightness is paid with a curve, not a multiply.** A mask can only remove light, so
  what it takes has to be paid in beforehand — but most NES colours already have a channel
  at 255, where a multiply has nowhere to put it and clips. With a linear gain the mask took
  42% of Mario's sky and only 16% of its red and green, and the sky turned lavender. A gamma
  lift through (0,0) and (255,255) has room everywhere in between and cannot clip anything.
- **The stripes are weak and the scanlines are strong.** Anything the mask takes evenly is
  just a dimmer television, which is right; taking it unevenly is a different colour rather
  than a darker one. A scanline dims all three channels together so it cannot shift a hue
  however deep it goes, which is why it carries most of the effect.
- **The beam is integrated over each row, not sampled once in it.** An 8:7 picture
  letterboxed into a 720-pixel window is 2.63 rows per console line, so every line meets the
  rows at a different phase. One sample per row put the seams at 98, 109 and 116 against a
  135 line — wide horizontal bands that look like a fault rather than like a television.
  Integrating has no phase to be wrong about, and the closed form costs two cosines.

The picture ends up at about four fifths of its unfiltered brightness, with hues intact.

Reset and Hard Reset are genuinely different. Reset is the button on the front — RAM
survives it, and so do A, X and Y. Hard Reset is the switch at the back, and clears them.

`F1` opens the chooser, which also sets the window size, and every plugin's Settings
button opens that plugin's own dialog: the filter, pixel shape and brightness for video, the
output device, volume and buffer size for audio, the bindings and chosen device for
controllers.

**Brightness is a gamma curve, and it shares its arithmetic with the CRT filter.** Both are
an exponent on each channel, and exponents compose by multiplying — `(x^a)^b` is `x^(ab)` —
so a CRT picture with the brightness raised is *one* pass with `CRT_LIFT / gamma` rather than
two passes and twice the rounding. A curve rather than a multiply because a curve through
(0,0) and (255,255) cannot clip: white stays white, black stays black, and only the midtones
move. Steps rather than a slider, so the value in the file is one a person could have typed
and two machines set to the same brightness really are.

It is also the answer to what the mask costs. Measured on Mario's sky, mean luma:

| | gamma 0.6 | gamma 1.0 | gamma 1.8 |
| --- | --- | --- | --- |
| Sharp | 108.9 | 150.2 | 188.3 |
| CRT television | 86.4 | 112.5 | 135.3 |

The CRT row sits below Sharp at every step, because a mask can only take light away — and
raising the gamma is how to put it back if the picture is darker than you want.

**Those apply the moment you press OK.** A plugin gets an `apply_settings` call after its
dialog is accepted and does as much as it safely can: video rebuilds its renderer state and
palette without touching the window, so the window keeps its position and its focus; audio
reopens the device, because which device and how much latency are properties of an open one;
controllers reload their bindings. Each answers whether everything took effect, and the host
says so when something did not, rather than leaving you to wonder whether OK did anything.

**Choosing a different plugin still needs a restart**, and that one is not a shortcut. It
would mean tearing down the window and the event queue the dialog is itself running on. The
chooser says as much.

Or press `F1` and open the controller plugin's own dialog, where a binding is set by
pressing the key or gamepad button you want it on. Anything else in the same group that
already had it is released. Printable keys go through your keyboard layout, so a
non-US board binds the key you actually pressed rather than the one in that place on a
US one.

Command-line options win over the file. Delete it to get the defaults back.

`P` or `Space` pauses, `N` advances one frame while paused, `M` mutes, holding `Tab`
runs unthrottled, `R` resets, `F1` opens the plugin chooser, `F12` saves a screenshot,
`Esc` quits. `--scale=N` sets
the window size, `--fullscreen` starts borderless, `--no-audio` runs silent; the picture
letterboxes to the NES's aspect at any window size, with nearest-neighbour scaling.

A light-gun game plays with the mouse: aim in the window and click. The cursor is hidden
over the picture — it sits exactly where you are aiming, which is the one place you need
to see — and stays visible on the title bar and in every dialog. Where the picture is on
screen is the video plugin's business, so the host asks it to turn the mouse position
into a console pixel; a controller plugin never has to know what is drawing.

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

The Zapper is scripted the same way, with `--shoot=X,Y@FRAME[:HELD]` in console pixels:

```bash
./build/bin/Release/nes_run duckhunt.nes --press=start@60 --press=start@200 --shoot=237,124@980:12 --frames=1040 --screenshot=hit.ppm
```

Giving any `--shoot` plugs a gun into port two instead of a pad. The trigger has to be
held across several frames, because the game blanks the screen for a frame after it is
pulled and only then looks for light.

## Save states

Eight slots, written beside the ROM as `.st1` to `.st8`, from the State menu. A slot shows
when it was written, read from the file rather than remembered, so a state from an earlier
run is described correctly and a deleted one goes back to reading `(empty)`.

Everything a running console holds goes in: RAM, both chips' registers and counters, the
cartridge's own registers and work RAM, and where the beam is -- a state taken partway
down the picture resumes partway down the picture, half-drawn frame included. What is
deliberately absent is the ROM, because a state is not a copy of the cartridge, and the
APU's sample buffer, which belongs to the run rather than to the machine.

Each class describes its state **once**, in a `serialize()` that runs in both directions.
A save routine and a separate load routine drift apart -- somebody adds a field to one and
not the other, and it surfaces days later as a game that resumes almost correctly.

The format is not portable and does not pretend to be. A state records the version it was
written by, a fingerprint of the ROM, and the size of every structure it contains, so one
from a different build or a different game is **refused** rather than misread -- and
refused with the machine still running, because a half-loaded console is worse than a
rejected file.

### How they are tested

Not by checking that fields come back. By determinism: run a while, save, run on, load,
run the same distance again, and compare every pixel. An omission is the only way a save
state really fails, and an omission is exactly what a field-by-field test cannot see.

That test was itself checked by breaking the code on purpose -- omitting the console's RAM
from the state made three of the five cases fail, which is how you know a passing run
means something.

## Testing

```bash
ctest --test-dir build -C Release --output-on-failure
```

The cartridge and bus tests build synthetic iNES images in-memory, so they need no
external ROMs.

### Test ROMs

`nes_rom_test` runs blargg's suites — the ones written by people with the hardware in
front of them, which is the only kind of test that disagrees with an emulator for
reasons its author did not think of. They report through memory rather than the screen
(`$6000` holds a status byte, `$6004` a message), so the result is read rather than
looked at. Drop them into `roms/blargg/`, in any arrangement of subdirectories; without
them the test reports itself as skipped. They are not redistributed here, and
`roms/` is gitignored.

It is a separate target because it takes minutes where the unit suite takes
milliseconds. **38 of 93 pass**; 30 fail for reasons listed one by one in
`test_src/testBlargg.cpp`, and 25 are older ROMs that only draw their answer on screen.
Listing each known failure with its cause is what keeps the gate readable: a failure
that is *not* on the list is a regression, and one that starts passing asks you to
delete its entry.

Two of them are worth naming, because nothing written here could have caught either:
reset was clearing RAM and the registers, which hardware does not do, and the 2A03's
missing decimal mode accounted for seven failures on its own.

Set `NES_ROM_FILTER` to run one ROM, and `NES_ROM_TRACE` to watch its status byte
change — which is the only thing a ROM that hangs will tell you.

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
- **Bus conflicts are only applied when the header asks for them.** NES 2.0 submapper 2
  on UxROM, CNROM and AxROM means the board has them; 1 means it does not; iNES 1.0 has
  nowhere to say and so is treated as not. Nothing is inferred from the mapper number
  alone, because a wrong guess does not fail loudly — it quietly selects the wrong bank.
- **Saves are written on exit and on reset, not continuously.** Closing the window or
  pressing `R` keeps your game; killing the process loses whatever was written since
  the last of those.
- **No save states** — only the cartridge's own battery RAM persists, the same as on
  hardware.
- **Rebinding means editing a file.** There is no in-emulator settings screen, which
  would need a UI toolkit SDL does not provide.
- **Opposing directions are not filtered.** Real hardware lets Left and Right close
  together and some games glitch when they do; that filtering belongs to whatever
  drives the input, so it is not done in `Controller`.
- **The frame interrupt starts inhibited**, where the documented power-up state of
  `$4017` is `$00`, which enables it. This is a deliberate deviation. A game that wants
  the frame interrupt must write `$4017` regardless — that register selects the four- or
  five-step sequence and is the only way to know the sequencer's phase — so a game that
  never writes it cannot be relying on the interrupt. Powering up with it enabled
  silences *Ikinari Musician* outright: it never writes `$4017` and never reads `$4015`,
  so the interrupt asserts and is never acknowledged, and its handler then runs about
  113 times a frame writing `$4015 = 0`, turning off every channel. Games that ask for
  the interrupt are unaffected; Super Mario Bros produces byte-identical audio either
  way. Worth revisiting against blargg's APU test ROMs.
- **The APU's timing is cycle-driven but not cycle-exact.** The frame sequencer lands on
  the right CPU cycles, but the `$4017` write delay is approximated, and the DMC charges
  a flat 4 cycles per fetch where hardware varies with what the CPU was doing. Music and
  effects are right; a test ROM measuring the sequencer to the cycle would not be.
- **Rendering is not a per-dot pipeline.** A line is drawn from the state at its start
  and redrawn from partway across when a write changes the rest of it, which covers
  mid-line scroll, mask and pattern-table changes. What it does not model is hardware's
  two-tile fetch latency, so a change takes effect at the pixel the write lands on
  rather than a tile or two later.
- **Sprite overflow is set by the real 8-per-line rule**, not by hardware's buggy
  evaluation, which both over- and under-reports on real silicon.
- **MMC3's counter is clocked a few dots off the real fetch.** It counts rising edges on
  PPU A12 now, taken from the fetch schedule — pattern fetches drive the line high, and
  an edge only counts after it has been low for nine dots, which is what throws away the
  every-eight-dots chatter of ordinary background fetches. Four of blargg's six
  `mmc3_test` ROMs pass; `4-scanline_timing` says the edge lands a little early.
- **MMC6, and the difference between MMC3 revisions**, are not implemented. The revisions
  disagree about whether reloading a counter that has just reached zero raises an IRQ.
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

1. The rest of blargg's suites. **71 of 93 pass, and all 93 are now judged** — the older
   suites report by drawing on the screen rather than through `$6000`, and the nametable is
   read back as text to get their verdict, which turned 25 unjudged ROMs into 19 quiet
   passes and 6 failures nobody had seen. The largest clusters left are sprite overflow
   evaluation, the dots around the vblank edge, and interrupt latency inside the CPU core,
   each listed with its cause in `test_src/testBlargg.cpp`. nestest would still be worth
   having as a trace to compare against, whenever that ROM's licensing is clear.
2. Save states — the console's whole state is a handful of plain structs, so this is
   mostly a serialisation exercise, and it makes debugging the harder games practical.
4. Famicom expansion audio, if a cart that uses it ever turns up. VRC6 is the usual
   first one, and it would mean letting a mapper contribute to the APU's mix.
