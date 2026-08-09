#ifndef NES_PLUGIN_H
#define NES_PLUGIN_H

/*
 * The plugin boundary.
 *
 * Plain C on purpose. A C++ class laid out by one compiler and called by
 * another is undefined behaviour in practice as well as in theory -- vtable
 * layout, name mangling, exception tables and the standard library's ABI all
 * have to match, and none of them is guaranteed to across a module boundary.
 * Function pointers in a struct have none of those problems.
 *
 * Rules for anything crossing this line:
 *
 *   - Whoever allocates, frees. No pointer handed over here is ever freed by
 *     the other side; a plugin built against a different C runtime has a
 *     different heap, and freeing across that is a crash waiting for a user to
 *     find it.
 *   - No exceptions escape a callback. A throw crossing a C frame is undefined.
 *   - Every api struct starts with its own size, so a field can be added later
 *     without invalidating modules built against the older header: the caller
 *     checks the size before touching anything new.
 *   - The version handshake is not optional. A host that loads a module without
 *     checking nes_plugin_abi_version() is how a plugin ecosystem turns into a
 *     support burden, which is the failure this whole design is trying to avoid.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bumped whenever anything in this header changes meaning.
 *
 * Adding a field to the end of an api struct does not need a bump, because the
 * size field already covers it. Changing what an existing call does, or the
 * order of anything, does.
 */
#define NES_PLUGIN_ABI_VERSION 1u

typedef enum nes_plugin_kind {
	NES_PLUGIN_VIDEO = 1,
	NES_PLUGIN_AUDIO = 2,
	NES_PLUGIN_INPUT = 3
} nes_plugin_kind;

/** Player requests, as opposed to anything the game sees. */
enum {
	NES_COMMAND_NONE       = 0,
	NES_COMMAND_QUIT       = 1 << 0,
	NES_COMMAND_PAUSE      = 1 << 1,   /* toggle */
	NES_COMMAND_STEP_FRAME = 1 << 2,
	NES_COMMAND_MUTE       = 1 << 3,   /* toggle */
	NES_COMMAND_RESET      = 1 << 4,
	NES_COMMAND_SCREENSHOT = 1 << 5
};

/* ------------------------------------------------------------------------- */
/* What the host offers a plugin                                              */
/* ------------------------------------------------------------------------- */

/**
 * Services a plugin can ask of the host.
 *
 * This is what keeps the plugins from needing each other. A light gun has to
 * know how bright the screen is where it is pointed, which is video data
 * arriving in an input plugin -- routing that through the host keeps the
 * dependency graph a star rather than a mesh, and means a controller plugin
 * never has to care which video plugin is loaded.
 *
 * The host owns the window and drains the event queue, for the same reason: on
 * most platforms one queue carries window, keyboard and pad events together, so
 * two modules cannot both own it.
 */
typedef struct nes_host {
	size_t size;
	void* context;

	/**
	 * The frame currently on screen: width * height palette indices, 0-63.
	 * Valid until the next present. Never freed by the caller.
	 */
	const uint8_t* (*get_frame)(void* context, int* width, int* height);

	/** Native window handle -- HWND, X11 Window, NSWindow -- or NULL. */
	void* (*window_handle)(void* context);

	/** For a plugin to say something without owning a console. */
	void (*log)(void* context, const char* message);
} nes_host;

/* ------------------------------------------------------------------------- */
/* Video                                                                      */
/* ------------------------------------------------------------------------- */

typedef struct nes_video_api {
	size_t size;

	void* (*create)(const nes_host* host);
	void (*destroy)(void* self);

	/** @return 1 on success; on failure writes a reason into @p error. */
	int (*open)(void* self, int scale, int fullscreen, const char* title,
			char* error, size_t error_size);
	void (*close)(void* self);

	void (*present)(void* self, const uint8_t* indices, int width, int height);
	void (*set_title)(void* self, const char* title);
	int (*save_screenshot)(void* self, const char* path);

	/** Show this plugin's own settings dialog. May be NULL. */
	void (*configure)(void* self);
} nes_video_api;

/* ------------------------------------------------------------------------- */
/* Audio                                                                      */
/* ------------------------------------------------------------------------- */

typedef struct nes_audio_api {
	size_t size;

	void* (*create)(const nes_host* host);
	void (*destroy)(void* self);

	int (*open)(void* self, int sample_rate, char* error, size_t error_size);
	void (*close)(void* self);
	int (*is_open)(void* self);

	void (*queue)(void* self, const float* samples, size_t count);

	/**
	 * How much audio is still waiting to play.
	 *
	 * The one number that makes the difference between working audio and a
	 * queue that drifts into either crackling or latency: the host steers its
	 * resampling ratio by it, and without it there is nothing to steer by.
	 */
	double (*queued_seconds)(void* self);

	void (*clear)(void* self);

	void (*configure)(void* self);
} nes_audio_api;

/* ------------------------------------------------------------------------- */
/* Input                                                                      */
/* ------------------------------------------------------------------------- */

typedef struct nes_input_state {
	uint8_t buttons[2];
	unsigned commands;
	int turbo;
} nes_input_state;

typedef struct nes_input_api {
	size_t size;

	void* (*create)(const nes_host* host);
	void (*destroy)(void* self);

	int (*open)(void* self, char* error, size_t error_size);
	void (*close)(void* self);

	/** One frame's worth: pad state, plus anything that happened once. */
	void (*poll)(void* self, nes_input_state* out);

	void (*configure)(void* self);
} nes_input_api;

/* ------------------------------------------------------------------------- */
/* Discovery                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct nes_plugin_info {
	size_t size;
	/** Stable, machine-readable, written into the config file. */
	const char* id;
	/** Shown to a person choosing one. */
	const char* name;
	const char* version;
	int kind;                  /* nes_plugin_kind */
} nes_plugin_info;

/*
 * A module exports exactly these three, by name:
 *
 *   uint32_t                nes_plugin_abi_version(void);
 *   const nes_plugin_info*  nes_plugin_describe(void);
 *   const void*             nes_plugin_api(void);
 *
 * nes_plugin_api() returns a pointer to the api struct matching the kind in the
 * descriptor. The host checks the version first and does not call anything else
 * if it does not match -- describe() itself could have changed shape.
 */
typedef uint32_t (*nes_plugin_abi_version_fn)(void);
typedef const nes_plugin_info* (*nes_plugin_describe_fn)(void);
typedef const void* (*nes_plugin_api_fn)(void);

#define NES_PLUGIN_ABI_VERSION_SYMBOL "nes_plugin_abi_version"
#define NES_PLUGIN_DESCRIBE_SYMBOL    "nes_plugin_describe"
#define NES_PLUGIN_API_SYMBOL         "nes_plugin_api"

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NES_PLUGIN_H */
