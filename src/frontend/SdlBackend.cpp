#include "SdlBackend.h"

#include "BindingsDialog.h"
#include "../plugin/FieldsDialog.h"
#include "CrtFilter.h"

#include <SDL_syswm.h>

#include "nes/Controller.h"
#include "nes/Ppu.h"

namespace nesfe {

namespace {

// The order every binding table uses, here and in the config file.
const std::uint8_t BUTTON_BITS[8] = {
	nes::Controller::BUTTON_A,
	nes::Controller::BUTTON_B,
	nes::Controller::BUTTON_SELECT,
	nes::Controller::BUTTON_START,
	nes::Controller::BUTTON_UP,
	nes::Controller::BUTTON_DOWN,
	nes::Controller::BUTTON_LEFT,
	nes::Controller::BUTTON_RIGHT
};

std::uint8_t readKeys(const Uint8* keys, const SDL_Scancode (&map)[8]) {
	std::uint8_t buttons = 0;
	for (int i = 0; i < 8; i++)
		if (map[i] != SDL_SCANCODE_UNKNOWN && keys[map[i]])
			buttons |= BUTTON_BITS[i];
	return buttons;
}

std::uint8_t buttonForKey(SDL_Scancode code, const SDL_Scancode (&map)[8]) {
	std::uint8_t buttons = 0;
	for (int i = 0; i < 8; i++)
		if (map[i] == code)
			buttons |= BUTTON_BITS[i];
	return buttons;
}

bool isBound(const SDL_GameControllerButton (&map)[8], SDL_GameControllerButton button) {
	for (int i = 0; i < 8; i++)
		if (map[i] == button)
			return true;
	return false;
}

SDL_JoystickID instanceId(SDL_GameController* pad) {
	return SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad));
}

} // namespace

/* ------------------------------------------------------------------------- */
/* Video                                                                      */
/* ------------------------------------------------------------------------- */

SdlVideo::SdlVideo(const nes_host* host) :
		m_host(host), m_window(nullptr), m_renderer(nullptr), m_texture(nullptr),
		m_width(0), m_height(0), m_logicalWidth(0),
		m_textureWidth(0), m_textureHeight(0), m_crt(false) {
	// The console's fixed palette, pre-expanded with an opaque alpha.
	const std::uint32_t* rgb = nes::Ppu::nesPaletteRgb();
	for (int i = 0; i < 64; i++)
		m_argbPalette[i] = 0xFF000000u | rgb[i];
}

SdlVideo::~SdlVideo() {
	close();
}

bool SdlVideo::open(const VideoOptions& options, Error* error) {
	m_width = nes::Ppu::SCREEN_WIDTH;
	m_height = nes::Ppu::SCREEN_HEIGHT;
	m_pixels.assign(static_cast<std::size_t>(m_width) * m_height
			* CRT_SCALE * CRT_SCALE, 0);

	m_window = SDL_CreateWindow(options.title,
			SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
			m_width * options.scale, m_height * options.scale,
			SDL_WINDOW_RESIZABLE
					| (options.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
	if (!m_window) {
		if (error) *error = std::string("SDL_CreateWindow: ") + SDL_GetError();
		return false;
	}

	// No PRESENTVSYNC: the frame clock is the pacing authority, and a 144 Hz
	// display would otherwise run the console two and a half times too fast.
	// Letting both throttle produces beat-frequency stutter.
	m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_ACCELERATED);
	if (!m_renderer)
		m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_SOFTWARE);
	if (!m_renderer) {
		if (error) *error = std::string("SDL_CreateRenderer: ") + SDL_GetError();
		close();
		return false;
	}

	// Letterbox at whatever size the window is dragged to. Which aspect that is
	// is a real choice: the console's pixels were not square on a television,
	// so a circle drawn in a game is an ellipse at 256x240 and a circle at
	// 292x240. Sharpness is a choice too -- these are 8x8 tiles, not
	// photographs, and most people want to see them.
	const std::string filter = setting("filter", "sharp");
	m_crt = (filter == "crt");

	// The CRT style draws a pattern inside every console pixel, so its texture is
	// three times the size and the renderer is told that *is* the picture. The
	// logical size follows, which keeps the letterboxing and the aspect choice
	// working exactly as they did.
	const int scale = m_crt ? CRT_SCALE : 1;
	m_textureWidth = m_width * scale;
	m_textureHeight = m_height * scale;
	m_logicalWidth = ((setting("aspect", "square") == "tv")
			? WIDE_WIDTH : m_width) * scale;
	SDL_RenderSetLogicalSize(m_renderer, m_logicalWidth, m_height * scale);

	// Linear for the CRT style as well: at exactly 3x the texture lands 1:1 and
	// nothing is resampled, and at any other size a smooth reduction of the
	// stripes looks far more like a television than a jagged one.
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,
			(filter == "smooth" || m_crt) ? "linear" : "nearest");

	m_texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ARGB8888,
			SDL_TEXTUREACCESS_STREAMING, m_textureWidth, m_textureHeight);
	if (!m_texture) {
		if (error) *error = std::string("SDL_CreateTexture: ") + SDL_GetError();
		close();
		return false;
	}

	// The arrow sits exactly where a light gun is being aimed, which is the one
	// place a player needs to see. Hiding it costs nothing when there is no gun:
	// this is a television, and there is nothing in the picture to point at.
	//
	// SDL answers WM_SETCURSOR (and its equivalents) only for HTCLIENT on its own
	// windows, so this hides the arrow over the picture and nowhere else -- the
	// title bar, the borders and every dialog we put up keep their own cursor.
	SDL_ShowCursor(SDL_DISABLE);
	return true;
}

void SdlVideo::close() {
	// Whoever opens a window next gets the cursor back in the state they would
	// expect to find it, rather than one this instance left behind.
	SDL_ShowCursor(SDL_ENABLE);
	if (m_texture) { SDL_DestroyTexture(m_texture); m_texture = nullptr; }
	if (m_renderer) { SDL_DestroyRenderer(m_renderer); m_renderer = nullptr; }
	if (m_window) { SDL_DestroyWindow(m_window); m_window = nullptr; }
}

void SdlVideo::present(const std::uint8_t* indices, int width, int height) {
	if (!m_texture || width != m_width || height != m_height)
		return;

	if (m_crt) {
		crtExpand(indices, m_width, m_height, m_argbPalette, m_pixels.data());
	} else {
		const std::size_t count = static_cast<std::size_t>(m_width) * m_height;
		for (std::size_t i = 0; i < count; i++)
			m_pixels[i] = m_argbPalette[indices[i] & 0x3F];
	}

	SDL_UpdateTexture(m_texture, nullptr, m_pixels.data(),
			m_textureWidth * static_cast<int>(sizeof(std::uint32_t)));
	SDL_RenderClear(m_renderer);
	SDL_RenderCopy(m_renderer, m_texture, nullptr, nullptr);
	SDL_RenderPresent(m_renderer);
}

void* SdlVideo::nativeWindow() const {
	if (!m_window)
		return nullptr;
	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);
	if (!SDL_GetWindowWMInfo(m_window, &info))
		return nullptr;
#if defined(_WIN32)
	return info.info.win.window;
#else
	// Deliberately nothing elsewhere: a handle is only useful to a dialog that
	// knows what to do with it, and the only dialog so far is Win32's.
	return nullptr;
#endif
}

void SdlVideo::windowToFrame(int windowX, int windowY, int* frameX, int* frameY) const {
	*frameX = -1;
	*frameY = -1;
	if (!m_renderer)
		return;

	float logicalX = 0.0f;
	float logicalY = 0.0f;
	SDL_RenderWindowToLogical(m_renderer, windowX, windowY, &logicalX, &logicalY);

	// Logical units are not console pixels whenever the renderer is working in a
	// bigger space than the framebuffer: stretched to the television's aspect it
	// is 292 wide against 256, and with the CRT style it is three times both. A
	// light gun asking where it is pointed wants the pixel, so undo whatever the
	// scale is rather than reporting a row or column that does not exist.
	//
	// Both axes. Y needed no correction until the CRT style made the logical
	// height 720 against a 240-line picture, at which point every shot would
	// have read as off the screen.
	const int logicalHeight = m_crt ? m_height * CRT_SCALE : m_height;
	const int x = (m_logicalWidth > 0 && m_logicalWidth != m_width)
			? static_cast<int>(logicalX) * m_width / m_logicalWidth
			: static_cast<int>(logicalX);
	const int y = (logicalHeight != m_height)
			? static_cast<int>(logicalY) * m_height / logicalHeight
			: static_cast<int>(logicalY);
	// Outside the picture is a real answer here, not a failure: the player is
	// pointing the gun at the letterbox, or off the television entirely.
	if (x < 0 || y < 0 || x >= m_width || y >= m_height)
		return;
	*frameX = x;
	*frameY = y;
}

std::string SdlVideo::setting(const char* key, const char* fallback) const {
	if (!NES_HOST_PROVIDES(m_host, get_setting))
		return fallback;
	char value[64] = { 0 };
	const std::size_t length = m_host->get_setting(m_host->context, "sdl-video",
			key, value, sizeof(value));
	if (length == 0 || length >= sizeof(value))
		return fallback;   // never written, or too long to be one of ours
	return value;
}

void SdlVideo::putSetting(const char* key, const char* value) {
	if (NES_HOST_PROVIDES(m_host, set_setting))
		m_host->set_setting(m_host->context, "sdl-video", key, value);
}

void SdlVideo::configure() {
	if (!nesdlg::fieldsDialogAvailable()) {
		SDL_Log("no video settings dialog on this platform yet");
		return;
	}

	std::vector<nesdlg::Field> fields(2);
	fields[0].label = "Scaling";
	fields[0].options.push_back("Sharp  (nearest neighbour)");
	fields[0].options.push_back("Smooth  (linear)");
	fields[0].options.push_back("CRT  (RGB stripes and scanlines)");
	const std::string filter = setting("filter", "sharp");
	fields[0].selected = (filter == "smooth") ? 1 : (filter == "crt" ? 2 : 0);

	fields[1].label = "Pixel shape";
	fields[1].options.push_back("Square  (256 x 240)");
	fields[1].options.push_back("As a television showed it  (8:7)");
	fields[1].selected = (setting("aspect", "square") == "tv") ? 1 : 0;

	// Parented to the emulator's window when there is one. This dialog is
	// usually opened from a throwaway instance that has no window of its own,
	// so the handle has to come from the host rather than from m_window.
	void* parent = nullptr;
	if (NES_HOST_PROVIDES(m_host, window_handle))
		parent = m_host->window_handle(m_host->context);

	if (!nesdlg::showFieldsDialog("SDL2 video", parent, &fields,
			"Takes effect the next time the emulator starts."))
		return;

	putSetting("filter", fields[0].selected == 2 ? "crt"
			: (fields[0].selected == 1 ? "smooth" : "sharp"));
	putSetting("aspect", fields[1].selected == 1 ? "tv" : "square");
}

void SdlVideo::setTitle(const char* title) {
	if (m_window)
		SDL_SetWindowTitle(m_window, title);
}

bool SdlVideo::saveScreenshot(const char* path) {
	if (m_pixels.empty())
		return false;
	// Whatever is in the texture, including the CRT pattern if that is on: a
	// screenshot of a television should look like the television.
	SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
			m_pixels.data(), m_textureWidth, m_textureHeight, 32,
			m_textureWidth * static_cast<int>(sizeof(std::uint32_t)),
			SDL_PIXELFORMAT_ARGB8888);
	if (!surface)
		return false;
	const bool ok = SDL_SaveBMP(surface, path) == 0;
	SDL_FreeSurface(surface);
	if (ok)
		SDL_Log("wrote %s", path);
	return ok;
}

/* ------------------------------------------------------------------------- */
/* Audio                                                                      */
/* ------------------------------------------------------------------------- */

SdlAudio::SdlAudio() : m_device(0), m_sampleRate(0) { }

SdlAudio::~SdlAudio() {
	close();
}

bool SdlAudio::open(int sampleRate, Error* error) {
	SDL_AudioSpec want;
	SDL_zero(want);
	want.freq = sampleRate;
	want.format = AUDIO_F32SYS;
	want.channels = 1;         // the NES mixes to one signal
	want.samples = 1024;
	want.callback = nullptr;   // queued, not driven from a callback thread

	SDL_AudioSpec got;
	m_device = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
	if (m_device == 0) {
		if (error) *error = std::string("no audio: ") + SDL_GetError();
		return false;
	}
	m_sampleRate = got.freq ? got.freq : sampleRate;
	SDL_PauseAudioDevice(m_device, 0);
	return true;
}

void SdlAudio::close() {
	if (m_device) {
		SDL_CloseAudioDevice(m_device);
		m_device = 0;
	}
}

void SdlAudio::queue(const float* samples, std::size_t count) {
	if (m_device)
		SDL_QueueAudio(m_device, samples,
				static_cast<Uint32>(count * sizeof(float)));
}

double SdlAudio::queuedSeconds() const {
	if (!m_device || m_sampleRate <= 0)
		return 0.0;
	return static_cast<double>(SDL_GetQueuedAudioSize(m_device))
			/ sizeof(float) / m_sampleRate;
}

void SdlAudio::clear() {
	if (m_device)
		SDL_ClearQueuedAudio(m_device);
}

/* ------------------------------------------------------------------------- */
/* Input                                                                      */
/* ------------------------------------------------------------------------- */

SdlInput::SdlInput() :
		m_owned(nesgui::Config::defaults()), m_config(m_owned), m_loadOwn(true) {
	m_pads[0] = nullptr;
	m_pads[1] = nullptr;
}

SdlInput::SdlInput(const nesgui::Config& config) :
		m_owned(nesgui::Config::defaults()), m_config(config), m_loadOwn(false) {
	m_pads[0] = nullptr;
	m_pads[1] = nullptr;
}

SdlInput::~SdlInput() {
	close();
}

bool SdlInput::open(Error* /*error*/) {
	// A bad binding is reported by load() and skipped; it is not a reason to
	// refuse to start, so the warnings are dropped here rather than failing.
	if (m_loadOwn)
		m_owned.load(nesgui::Config::path());

	// Adopt anything already plugged in when we started. A machine with no pad
	// is not an error -- the keyboard is always there.
	for (int i = 0; i < SDL_NumJoysticks(); i++)
		if (SDL_IsGameController(i))
			addPad(i);
	return true;
}

void SdlInput::close() {
	for (int port = 0; port < 2; port++)
		if (m_pads[port]) {
			SDL_GameControllerClose(m_pads[port]);
			m_pads[port] = nullptr;
		}
}

void SdlInput::addPad(int deviceIndex) {
	if (!SDL_IsGameController(deviceIndex))
		return;
	for (int port = 0; port < 2; port++) {
		if (m_pads[port])
			continue;
		m_pads[port] = SDL_GameControllerOpen(deviceIndex);
		if (m_pads[port])
			SDL_Log("gamepad on port %d: %s", port + 1,
					SDL_GameControllerName(m_pads[port]));
		return;
	}
	// More than two pads: the console only has two ports.
}

void SdlInput::removePad(SDL_JoystickID id) {
	for (int port = 0; port < 2; port++) {
		if (!m_pads[port] || instanceId(m_pads[port]) != id)
			continue;
		SDL_GameControllerClose(m_pads[port]);
		m_pads[port] = nullptr;
		SDL_Log("gamepad removed from port %d", port + 1);
	}
}

int SdlInput::portOf(SDL_JoystickID id) const {
	for (int port = 0; port < 2; port++)
		if (m_pads[port] && instanceId(m_pads[port]) == id)
			return port;
	return -1;
}

void SdlInput::pollZapper(ZapperState* out) {
	int x = 0;
	int y = 0;
	const Uint32 buttons = SDL_GetMouseState(&x, &y);

	out->connected = true;
	out->port = 1;              // the Zapper's port on every game that uses one
	out->windowX = x;
	out->windowY = y;
	out->trigger = (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
}

void SdlInput::configure() {
	if (!bindingsDialogAvailable()) {
		SDL_Log("no bindings dialog on this platform yet -- edit %s",
				nesgui::Config::path().c_str());
		return;
	}

	// A fresh copy: whatever else has been changed in the file since this
	// program started is not this dialog's to undo, and saving a stale copy
	// would do exactly that.
	nesgui::Config onDisk = nesgui::Config::defaults();
	const std::string path = nesgui::Config::path();
	onDisk.load(path);

	if (!showBindingsDialog(&onDisk, nullptr))
		return;

	if (onDisk.save(path))
		SDL_Log("saved controller bindings to %s", path.c_str());
	else
		SDL_Log("could not write %s", path.c_str());

	// If this instance owns its bindings, take the new ones now so a live
	// emulator responds without being restarted.
	if (m_loadOwn)
		m_owned = onDisk;
}

int SdlInput::padCount() const {
	return (m_pads[0] ? 1 : 0) + (m_pads[1] ? 1 : 0);
}

std::uint8_t SdlInput::readPad(int port) const {
	SDL_GameController* pad = m_pads[port];
	if (!pad)
		return 0;
	const SDL_GameControllerButton (&map)[8] = m_config.padButtons[port];

	std::uint8_t buttons = 0;
	for (int i = 0; i < 8; i++)
		if (map[i] != SDL_CONTROLLER_BUTTON_INVALID
				&& SDL_GameControllerGetButton(pad, map[i]))
			buttons |= BUTTON_BITS[i];

	// The other face-button diagonal, for whichever of the four the
	// configuration does not already use. The NES has two buttons and a modern
	// pad has four; leaving half of them dead feels broken. An explicit binding
	// always wins, so rebinding cannot collide with this.
	if (!isBound(map, SDL_CONTROLLER_BUTTON_B)
			&& SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B))
		buttons |= nes::Controller::BUTTON_A;
	if (!isBound(map, SDL_CONTROLLER_BUTTON_Y)
			&& SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y))
		buttons |= nes::Controller::BUTTON_B;

	// The left stick drives the d-pad as well. Deadzone is generous: this is a
	// digital pad underneath, so precision buys nothing and drift on a worn
	// stick would be read as a held direction.
	const int DEADZONE = 12000;
	const int x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
	const int y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
	if (x < -DEADZONE) buttons |= nes::Controller::BUTTON_LEFT;
	if (x >  DEADZONE) buttons |= nes::Controller::BUTTON_RIGHT;
	if (y < -DEADZONE) buttons |= nes::Controller::BUTTON_UP;
	if (y >  DEADZONE) buttons |= nes::Controller::BUTTON_DOWN;
	return buttons;
}

std::uint8_t SdlInput::padButtonFor(Uint8 sdlButton, int port) const {
	const SDL_GameControllerButton (&map)[8] = m_config.padButtons[port];
	const SDL_GameControllerButton pressed =
			static_cast<SDL_GameControllerButton>(sdlButton);
	std::uint8_t buttons = 0;
	for (int i = 0; i < 8; i++)
		if (map[i] == pressed)
			buttons |= BUTTON_BITS[i];
	if (!isBound(map, pressed)) {
		if (pressed == SDL_CONTROLLER_BUTTON_B) buttons |= nes::Controller::BUTTON_A;
		if (pressed == SDL_CONTROLLER_BUTTON_Y) buttons |= nes::Controller::BUTTON_B;
	}
	return buttons;
}

void SdlInput::poll(InputState* out) {
	// Buttons whose key went down during this frame. Polling alone would miss a
	// press and release that both arrive between two frames -- rare from a
	// human, routine from anything automating the window -- so a tap is held
	// for the frame it landed in and no shorter.
	std::uint8_t tapped[2] = { 0, 0 };

	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		if (event.type == SDL_QUIT) {
			out->commands |= COMMAND_QUIT;
		} else if (event.type == SDL_CONTROLLERDEVICEADDED) {
			addPad(event.cdevice.which);
		} else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
			removePad(event.cdevice.which);
		} else if (event.type == SDL_CONTROLLERBUTTONDOWN) {
			const int port = portOf(event.cbutton.which);
			if (port >= 0)
				tapped[port] |= padButtonFor(event.cbutton.button, port);
		} else if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
			tapped[0] |= buttonForKey(event.key.keysym.scancode, m_config.keys[0]);
			tapped[1] |= buttonForKey(event.key.keysym.scancode, m_config.keys[1]);
			switch (event.key.keysym.scancode) {
			case SDL_SCANCODE_ESCAPE:  out->commands |= COMMAND_QUIT; break;
			case SDL_SCANCODE_P:
			case SDL_SCANCODE_SPACE:   out->commands |= COMMAND_PAUSE; break;
			case SDL_SCANCODE_N:       out->commands |= COMMAND_STEP_FRAME; break;
			case SDL_SCANCODE_M:       out->commands |= COMMAND_MUTE; break;
			case SDL_SCANCODE_R:       out->commands |= COMMAND_RESET; break;
			case SDL_SCANCODE_F12:     out->commands |= COMMAND_SCREENSHOT; break;
			case SDL_SCANCODE_F1:      out->commands |= COMMAND_SETTINGS; break;
			default: break;
			}
		}
	}

	// Keyboard and pad both drive the same port, so a pad can be picked up
	// mid-game without the keyboard going dead.
	const Uint8* keys = SDL_GetKeyboardState(nullptr);
	for (int port = 0; port < 2; port++)
		out->buttons[port] = static_cast<std::uint8_t>(
				readKeys(keys, m_config.keys[port]) | readPad(port) | tapped[port]);

	// Held, not tapped: fast-forward lasts as long as the key is down.
	out->turbo = keys[SDL_SCANCODE_TAB] != 0;
}

/* ------------------------------------------------------------------------- */
/* Clock                                                                      */
/* ------------------------------------------------------------------------- */

SdlClock::SdlClock() :
		m_period(1.0 / static_cast<double>(SDL_GetPerformanceFrequency())) { }

double SdlClock::now() const {
	return static_cast<double>(SDL_GetPerformanceCounter()) * m_period;
}

void SdlClock::sleep(double seconds) {
	if (seconds <= 0.0)
		return;
	const double target = now() + seconds;
	// Sleep the bulk and spin the last millisecond. SDL_Delay's resolution is a
	// whole scheduler tick, which is most of a frame -- accurate enough to stop
	// burning a core, nowhere near accurate enough to pace on its own.
	if (seconds > 0.002)
		SDL_Delay(static_cast<Uint32>((seconds - 0.001) * 1000.0));
	while (now() < target) { }
}

} // namespace nesfe
