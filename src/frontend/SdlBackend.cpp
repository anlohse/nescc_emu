#include "SdlBackend.h"

#include "BindingsDialog.h"
#include "../plugin/FieldsDialog.h"
#include "CrtFilter.h"
#include <cstring>

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

CrtMaskKind parseCrtMaskKind(const char* name) {
	if (std::strcmp(name, "crt-monitor") == 0)
		return CRT_APERTURE_GRILLE;
	if (std::strcmp(name, "crt-monitor-2") == 0)
		return CRT_APERTURE_GRILLE_2;
	if (std::strcmp(name, "crt-tv") == 0)
		return CRT_SLOT_MASK;
	if (std::strcmp(name, "crt-tv-2") == 0)
		return CRT_SLOT_MASK_2;
	return CRT_SLOT_MASK;   // the default
}

/**
 * The scaling choices, in the order the dialog offers them.
 *
 * One table read both ways, which is the point of having it here rather than
 * inside configure(): a list that is written by index and read back by a
 * hand-written chain of comparisons is a list that forgets settings it has no
 * branch for, and this one had grown two of those.
 */
const char* const FILTERS[] = {
	"sharp", "smooth", "crt-tv", "crt-monitor", "crt-tv-2", "crt-monitor-2"
};
const int FILTER_COUNT = static_cast<int>(sizeof(FILTERS) / sizeof(FILTERS[0]));

/**
 * The brightness steps, as the text that goes in the file.
 *
 * Text rather than numbers so the dialog's entries and the file's values are the
 * same strings, and so a hand-edited file matching a step selects it in the
 * dialog instead of quietly reading as something else.
 */
const char* const GAMMA_STEPS[] = {
	"0.6", "0.7", "0.8", "0.9", "1.0", "1.1", "1.2", "1.4", "1.6", "1.8", "2.2"
};
const int GAMMA_STEP_COUNT =
		static_cast<int>(sizeof(GAMMA_STEPS) / sizeof(GAMMA_STEPS[0]));

/** A gamma as the step text it came from, so the dialog can find it again. */
std::string gammaText(float gamma) {
	char buffer[16];
	std::snprintf(buffer, sizeof buffer, "%.1f", gamma);
	return buffer;
}

SDL_JoystickID instanceId(SDL_GameController* pad) {
	return SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad));
}

} // namespace

/* ------------------------------------------------------------------------- */
/* Video                                                                      */
/* ------------------------------------------------------------------------- */

// Wide enough for a dim laptop panel at one end and a bright television at the
// other, and bounded because this comes from a text file: zero would divide, and
// a huge exponent would post a black or a white screen with no way back except
// finding the file again.
const float SdlVideo::GAMMA_MIN = 0.4f;
const float SdlVideo::GAMMA_MAX = 3.0f;

SdlVideo::SdlVideo(const nes_host* host) :
		m_host(host), m_window(nullptr), m_renderer(nullptr), m_texture(nullptr),
		m_width(0), m_height(0), m_logicalWidth(0), m_crt(false),
		m_maskKind(CRT_SLOT_MASK),
		m_mask(nullptr), m_maskWidth(0), m_maskHeight(0) {
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
	m_pixels.assign(static_cast<std::size_t>(m_width) * m_height, 0);
	// Sized once here rather than per frame. The softening pass is separable, so
	// it needs somewhere to put the horizontal half before the vertical one reads
	// it back, and a frame is not the place to be allocating.
	m_scratch.assign(m_pixels.size(), 0);
	m_soft.assign(m_pixels.size(), 0);

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

	if (!applyPictureSettings()) {
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
	if (m_mask) { SDL_DestroyTexture(m_mask); m_mask = nullptr; }
	m_maskWidth = 0;
	m_maskHeight = 0;
	if (m_texture) { SDL_DestroyTexture(m_texture); m_texture = nullptr; }
	if (m_renderer) { SDL_DestroyRenderer(m_renderer); m_renderer = nullptr; }
	if (m_window) { SDL_DestroyWindow(m_window); m_window = nullptr; }
}

float SdlVideo::pictureGamma() const {
	// Stored as the text a person would write, so the file stays editable by hand
	// and the dialog's entries and the file's values are the same strings.
	const float gamma = static_cast<float>(
			std::atof(setting("gamma", "1.0").c_str()));
	// A nonsense value from a hand-edited file must not black the screen out or
	// divide by zero, and silently ignoring it is kinder than refusing to start.
	if (!(gamma >= GAMMA_MIN && gamma <= GAMMA_MAX))
		return 1.0f;
	return gamma;
}

bool SdlVideo::applyPictureSettings() {
	// Everything about how the picture is drawn, in one place so that it can be
	// done again. Pressing OK in the settings dialog used to change a file and
	// nothing else, which is a poor answer: somebody who has just chosen a
	// different picture wants to see a different picture.
	//
	// The window is deliberately untouched. Only the renderer's state and the
	// texture depend on these settings, so there is no reason to destroy a window
	// -- which would lose its position, flash, and on some platforms lose the
	// keyboard focus with it.
	//
	// Letterbox at whatever size the window is dragged to. Which aspect that is
	// is a real choice: the console's pixels were not square on a television, so a
	// circle drawn in a game is an ellipse at 256x240 and a circle at 292x240.
	// Sharpness is a choice too -- these are 8x8 tiles, not photographs, and most
	// people want to see them.
	const std::string filter = setting("filter", "sharp");
	m_crt = (filter.rfind("crt", 0) == 0);
	// A plain "crt" from an older configuration means the television, which is
	// both the default and what an NES was actually plugged into.
	m_maskKind = parseCrtMaskKind(filter.c_str());
	m_logicalWidth = (setting("aspect", "square") == "tv") ? WIDE_WIDTH : m_width;

	// The CRT style letterboxes for itself, because its mask has to land on whole
	// screen pixels: under a logical size the mask would be scaled along with the
	// picture, and one-pixel stripes stretched by 3.4 turn into moire.
	//
	// Zero turns a logical size off again, which matters when switching *away*
	// from CRT: leaving the old one set would letterbox twice.
	SDL_RenderSetLogicalSize(m_renderer, m_crt ? 0 : m_logicalWidth,
			m_crt ? 0 : m_height);

	// A television was never sharp -- soft beam, bandwidth-limited signal,
	// phosphor spreading whatever light it got -- so the CRT style stretches soft
	// and multiplies the mask over the result. Blurring first is what stops it
	// looking like a grid of coloured squares.
	//
	// Linear here even for the CRT, which wants a quadratic stretch, because a
	// quadratic one is built out of this: SDL offers nearest and linear and
	// nothing else, and a quadratic B-spline is a tent convolved with one more
	// box. So the box is done to the frame in softenPicture and the tent is left
	// to the renderer, which was going to run anyway.
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,
			(filter == "smooth" || m_crt) ? "linear" : "nearest");

	// The scale-quality hint is read when a texture is created, so the texture has
	// to be made again for a changed filter to mean anything.
	if (m_texture)
		SDL_DestroyTexture(m_texture);
	m_texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ARGB8888,
			SDL_TEXTUREACCESS_STREAMING, m_width, m_height);
	if (!m_texture)
		return false;

	// The mask belongs to the old settings, and is rebuilt on the next frame.
	if (m_mask) {
		SDL_DestroyTexture(m_mask);
		m_mask = nullptr;
	}
	m_maskWidth = 0;
	m_maskHeight = 0;

	// From the console's palette every time rather than from whatever this
	// currently holds. Lifting an already-lifted palette would brighten it again
	// on each visit to the dialog, and the drift would look like a bug in the
	// filter rather than in the bookkeeping.
	// One curve for both jobs. The CRT mask has to be paid for in advance, because
	// a multiply can only take light away; brightness is whatever somebody wants
	// on their own screen. Both are an exponent per channel, and exponents compose
	// by multiplying, so this is one pass with CRT_LIFT / gamma rather than two
	// passes and twice the rounding.
	//
	// A gamma above 1.0 brightens, which is the direction every other program
	// means by the word -- so it is the reciprocal of the exponent.
	const float gamma = pictureGamma();
	const float exponent = (m_crt ? CRT_LIFT : 1.0f) / gamma;
	gammaPalette(nes::Ppu::nesPaletteRgb(), exponent, m_argbPalette);
	return true;
}

SDL_Rect SdlVideo::pictureRect() const {
	// Letterboxed by hand, which is what SDL_RenderSetLogicalSize would do -- but
	// the CRT style needs the destination in whole screen pixels so its mask can
	// line up with them.
	int windowWidth = 0;
	int windowHeight = 0;
	SDL_GetRendererOutputSize(m_renderer, &windowWidth, &windowHeight);

	SDL_Rect into;
	const int byWidth = windowWidth * m_height / m_logicalWidth;
	if (byWidth <= windowHeight) {
		into.w = windowWidth;
		into.h = byWidth;
	} else {
		into.h = windowHeight;
		into.w = windowHeight * m_logicalWidth / m_height;
	}
	into.x = (windowWidth - into.w) / 2;
	into.y = (windowHeight - into.h) / 2;
	return into;
}

void SdlVideo::ensureMask(const SDL_Rect& into) {
	if (m_mask && m_maskWidth == into.w && m_maskHeight == into.h)
		return;                        // the window has not changed size

	if (m_mask)
		SDL_DestroyTexture(m_mask);
	m_maskWidth = into.w;
	m_maskHeight = into.h;
	m_mask = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ARGB8888,
			SDL_TEXTUREACCESS_STATIC, m_maskWidth, m_maskHeight);
	if (!m_mask)
		return;

	std::vector<std::uint32_t> pattern(
			static_cast<std::size_t>(m_maskWidth) * m_maskHeight, 0);
	buildCrtMask(m_maskWidth, m_maskHeight, m_height, m_maskKind,
			pattern.data());
	SDL_UpdateTexture(m_mask, nullptr, pattern.data(),
			m_maskWidth * static_cast<int>(sizeof(std::uint32_t)));
	// Modulate: every channel of the picture is multiplied by the mask's. One
	// blended pass over pixels the GPU was going to touch anyway.
	SDL_SetTextureBlendMode(m_mask, SDL_BLENDMODE_MOD);
	// Nearest, so a stripe stays one screen pixel wide instead of being smoothed
	// into a grey wash -- the blur belongs to the picture, not to the glass.
	SDL_SetTextureScaleMode(m_mask, SDL_ScaleModeNearest);
}

void SdlVideo::present(const std::uint8_t* indices, int width, int height) {
	if (!m_texture || width != m_width || height != m_height)
		return;

	const std::size_t count = static_cast<std::size_t>(m_width) * m_height;
	for (std::size_t i = 0; i < count; i++)
		m_pixels[i] = m_argbPalette[indices[i] & 0x3F];

	// The first half of a quadratic stretch, and the only part of it that costs
	// anything: three taps each way over 256x240 samples, after which the
	// renderer's linear filter finishes the job for free. It goes here, before
	// the mask, because it belongs to the signal rather than to the glass -- so
	// it is the same blur however large the window is.
	const std::uint32_t* frame = m_pixels.data();
	if (m_crt) {
		softenPicture(m_pixels.data(), m_width, m_height, m_scratch.data(),
				m_soft.data());
		frame = m_soft.data();
	}

	SDL_UpdateTexture(m_texture, nullptr, frame,
			m_width * static_cast<int>(sizeof(std::uint32_t)));
	SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
	SDL_RenderClear(m_renderer);

	if (!m_crt) {
		SDL_RenderCopy(m_renderer, m_texture, nullptr, nullptr);
		SDL_RenderPresent(m_renderer);
		return;
	}

	// The two stages, in the order a television did them: stretch it soft, then
	// multiply by the mask.
	const SDL_Rect into = pictureRect();
	SDL_RenderCopy(m_renderer, m_texture, nullptr, &into);
	ensureMask(into);
	if (m_mask)
		SDL_RenderCopy(m_renderer, m_mask, nullptr, &into);
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

	int x = 0;
	int y = 0;
	if (m_crt) {
		// No logical size to ask about in this mode, because the mask needs whole
		// screen pixels; the same rectangle that was drawn into is what maps back.
		const SDL_Rect into = pictureRect();
		if (into.w <= 0 || into.h <= 0)
			return;
		x = (windowX - into.x) * m_width / into.w;
		y = (windowY - into.y) * m_height / into.h;
	} else {
		float logicalX = 0.0f;
		float logicalY = 0.0f;
		SDL_RenderWindowToLogical(m_renderer, windowX, windowY,
				&logicalX, &logicalY);

		// Logical units are not console pixels when the renderer works in a wider
		// space than the framebuffer: stretched to the television's aspect it is
		// 292 across against 256. A light gun wants the pixel, so undo that
		// rather than reporting a column which does not exist.
		x = (m_logicalWidth > 0 && m_logicalWidth != m_width)
				? static_cast<int>(logicalX) * m_width / m_logicalWidth
				: static_cast<int>(logicalX);
		y = static_cast<int>(logicalY);
	}

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

	std::vector<nesdlg::Field> fields(3);
	fields[0].label = "Scaling";
	fields[0].options.push_back("Sharp  (nearest neighbour)");
	fields[0].options.push_back("Smooth  (linear)");
	// Two CRTs, because they were two pieces of hardware. A television's slot mask
	// broke each colour column into slots and put the bridges between them half a
	// line from the neighbouring column's -- a brick wall on its side. A monitor's
	// aperture grille ran its stripes unbroken down the tube.
	fields[0].options.push_back("CRT television  (staggered slots)");
	fields[0].options.push_back("CRT monitor  (unbroken stripes)");
	// And the same two at a finer pitch: three phosphors to two screen pixels
	// rather than three, which is what a smaller dot pitch looked like.
	fields[0].options.push_back("CRT television  (fine slots, 2 pixels)");
	fields[0].options.push_back("CRT monitor  (fine stripes, 2 pixels)");
	const std::string filter = setting("filter", "sharp");
	// A plain "crt" from an older configuration means the television, which is
	// both the default and what an NES was actually plugged into.
	fields[0].selected = (filter == "crt") ? 2 : 0;
	for (int i = 0; i < FILTER_COUNT; i++)
		if (filter == FILTERS[i])
			fields[0].selected = i;

	fields[1].label = "Pixel shape";
	fields[1].options.push_back("Square  (256 x 240)");
	fields[1].options.push_back("As a television showed it  (8:7)");
	fields[1].selected = (setting("aspect", "square") == "tv") ? 1 : 0;

	// Steps rather than a slider, because this dialog is rows of choices and is
	// deliberately not a widget toolkit. Steps also mean the value in the file is
	// one somebody could have typed, and that two machines set to the same
	// brightness really are.
	fields[2].label = "Brightness";
	const std::string current = gammaText(pictureGamma());
	for (int i = 0; i < GAMMA_STEP_COUNT; i++) {
		const std::string text = GAMMA_STEPS[i];
		std::string label = text;
		if (text == "1.0")
			label += "  (unchanged)";
		fields[2].options.push_back(label);
		if (text == current)
			fields[2].selected = i;
	}

	// Parented to the emulator's window when there is one. This dialog is
	// usually opened from a throwaway instance that has no window of its own,
	// so the handle has to come from the host rather than from m_window.
	void* parent = nullptr;
	if (NES_HOST_PROVIDES(m_host, window_handle))
		parent = m_host->window_handle(m_host->context);

	if (!nesdlg::showFieldsDialog("SDL2 video", parent, &fields,
			"Applied as soon as you press OK."))
		return;

	// Bounds-checked rather than masked. A mask happened to work while there were
	// four entries, because three is every bit of three; with six it silently
	// mapped the two television entries onto "sharp" and "smooth".
	if (fields[0].selected >= 0 && fields[0].selected < FILTER_COUNT)
		putSetting("filter", FILTERS[fields[0].selected]);
	putSetting("aspect", fields[1].selected == 1 ? "tv" : "square");
	if (fields[2].selected >= 0 && fields[2].selected < GAMMA_STEP_COUNT)
		putSetting("gamma", GAMMA_STEPS[fields[2].selected]);
}

void SdlVideo::setTitle(const char* title) {
	if (m_window)
		SDL_SetWindowTitle(m_window, title);
}

bool SdlVideo::saveScreenshot(const char* path) {
	if (m_pixels.empty())
		return false;
	// The picture as the console drew it, 256x240. The CRT look lives in the
	// stretch and the mask, both of which happen on the GPU after this -- so a
	// screenshot is the signal rather than a photograph of the television.
	SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
			m_pixels.data(), m_width, m_height, 32,
			m_width * static_cast<int>(sizeof(std::uint32_t)),
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
	for (int i = 0; i < nesgui::MAX_GAMEPADS; i++)
		m_pads[i] = nullptr;
}

SdlInput::SdlInput(const nesgui::Config& config) :
		m_owned(nesgui::Config::defaults()), m_config(config), m_loadOwn(false) {
	for (int i = 0; i < nesgui::MAX_GAMEPADS; i++)
		m_pads[i] = nullptr;
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
	for (int i = 0; i < nesgui::MAX_GAMEPADS; i++)
		if (m_pads[i]) {
			SDL_GameControllerClose(m_pads[i]);
			m_pads[i] = nullptr;
		}
}

void SdlInput::addPad(int deviceIndex) {
	if (!SDL_IsGameController(deviceIndex))
		return;
	// Into the first free slot, and that slot number is what a person chooses
	// between: "Gamepad 1" is this list's first entry, not console port one.
	// Opening a pad no longer decides anything about who is playing, which is the
	// point -- an adapter presenting a socket it has nothing plugged into can no
	// longer take a player's controls with it.
	for (int i = 0; i < nesgui::MAX_GAMEPADS; i++) {
		if (m_pads[i])
			continue;
		m_pads[i] = SDL_GameControllerOpen(deviceIndex);
		if (m_pads[i])
			SDL_Log("gamepad %d: %s", i + 1, SDL_GameControllerName(m_pads[i]));
		return;
	}
}

void SdlInput::removePad(SDL_JoystickID id) {
	for (int i = 0; i < nesgui::MAX_GAMEPADS; i++) {
		if (!m_pads[i] || instanceId(m_pads[i]) != id)
			continue;
		SDL_GameControllerClose(m_pads[i]);
		m_pads[i] = nullptr;
		SDL_Log("gamepad %d removed", i + 1);
	}
}

int SdlInput::portOf(SDL_JoystickID id) const {
	// Which console port, if any, is set to read this device. A pad nobody
	// selected drives nothing, however hard it is pressed -- which is what makes
	// two players unambiguous.
	for (int port = 0; port < 2; port++) {
		if (m_config.device[port] != nesgui::PORT_GAMEPAD)
			continue;
		const int which = m_config.gamepad[port];
		if (which >= 0 && which < nesgui::MAX_GAMEPADS && m_pads[which]
				&& instanceId(m_pads[which]) == id)
			return port;
	}
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

bool SdlInput::applySettings() {
	// The dialog writes the file rather than editing this instance, on purpose:
	// the instance showing it is usually a throwaway. So reloading the file is how
	// a new binding arrives -- and only an instance that owns its configuration
	// can do that. One reading somebody else's says no, and the host reloads its
	// own copy instead, which every reference then sees.
	if (!m_loadOwn)
		return false;
	m_owned.load(nesgui::Config::path());
	return true;
}

int SdlInput::padCount() const {
	int count = 0;
	for (int i = 0; i < nesgui::MAX_GAMEPADS; i++)
		if (m_pads[i])
			count++;
	return count;
}

std::uint8_t SdlInput::readPad(int port) const {
	// The pad this port was told to read, not whichever happened to be found
	// first. A port set to the keyboard reads no pad at all.
	if (m_config.device[port] != nesgui::PORT_GAMEPAD)
		return 0;
	const int which = m_config.gamepad[port];
	if (which < 0 || which >= nesgui::MAX_GAMEPADS)
		return 0;
	SDL_GameController* pad = m_pads[which];
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
			// Only for a port actually set to the keyboard, or a tap would reach a
			// port being driven by a pad.
			for (int port = 0; port < 2; port++)
				if (m_config.device[port] == nesgui::PORT_KEYBOARD)
					tapped[port] |= buttonForKey(event.key.keysym.scancode,
							m_config.keys[port]);
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

	// One device per port, chosen rather than merged. Reading both used to be a
	// kindness -- plug a pad in and the keyboard kept working -- but it makes two
	// players ambiguous, and it hid the bug where a port was reading the wrong
	// pad: the keyboard still worked, so the port looked alive.
	const Uint8* keys = SDL_GetKeyboardState(nullptr);
	for (int port = 0; port < 2; port++) {
		const std::uint8_t held = (m_config.device[port] == nesgui::PORT_GAMEPAD)
				? readPad(port)
				: readKeys(keys, m_config.keys[port]);
		out->buttons[port] = static_cast<std::uint8_t>(held | tapped[port]);
	}

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
