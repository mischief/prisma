#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <wchar.h>
#include <signal.h>
#include <sys/types.h>
#include <hidapi/hidapi.h>
#include <unistd.h>
#include <time.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <sys/stat.h>

#include "image.h"
#include "default_config.h"

extern char **environ;

#define ELGATO_VENDOR_ID	0x0fd9
#define ELGATO_MK2_PRODUCT_ID	0x0080

#define BUTTON_COUNT		15

typedef struct {
	uint8_t header[4];
	uint8_t buttons[BUTTON_COUNT];
	uint8_t unknown[1024 - 4 - BUTTON_COUNT];
} ButtonReport;

typedef struct {
	int key;
	char *path;
} ImageUpload;

typedef struct {
	double interval;   /* seconds between fires */
	double next_fire;  /* CLOCK_MONOTONIC seconds */
	int func_ref;       /* LUA_REGISTRYINDEX ref to the callback */
} TimerEntry;

int debug = 0;
static int image_only = 0;
static int wait_seconds = 1;  /* Wait time after image upload */
static int brightness = -1;   /* -1 = leave as-is; 0-100 = set on startup */
static uint8_t prev_buttons[BUTTON_COUNT] = {0};
static ImageUpload *uploads = NULL;
static int num_uploads = 0;

static hid_device *dev = NULL;
static lua_State *L = NULL;

/* LUA_NOREF if the key has no on_press() handler registered. */
static int button_refs[BUTTON_COUNT];
/* Set once image()/blank() has been called for a key from the config
 * script, so the unconfigured-key blanking pass at startup leaves it
 * alone. */
static int key_configured[BUTTON_COUNT];

static TimerEntry *timers = NULL;
static int num_timers = 0;

static double
now_seconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int
send_image_chunks(hid_device *d, int key, const unsigned char *image_data, int total_size)
{
	unsigned char buf[1024];
	int offset, r;

	for (offset = 0; offset < total_size; offset += 1016) {
		int chunk_size = (total_size - offset > 1016) ? 1016 : (total_size - offset);
		int is_last = (offset + chunk_size >= total_size);
		int page_num = offset / 1016;

		memset(buf, 0, sizeof(buf));

		buf[0] = 0x02;
		buf[1] = 0x07;
		buf[2] = key;
		buf[3] = is_last ? 1 : 0;
		buf[4] = chunk_size & 0xFF;
		buf[5] = (chunk_size >> 8) & 0xFF;
		buf[6] = page_num & 0xFF;
		buf[7] = (page_num >> 8) & 0xFF;

		memcpy(buf + 8, image_data + offset, chunk_size);

		if (debug)
			fprintf(stderr, "sending chunk %d: key=%d, is_last=%d, size=%d, offset=%d\n",
			        page_num, key, is_last, chunk_size, offset);

		/* buf[0] is the report ID (0x02), exactly as the MagickWand
		 * V2 protocol expects -- hid_write() sends it as a single
		 * output report, first byte as report ID. */
		r = hid_write(d, buf, sizeof(buf));

		if (r < 0) {
			fprintf(stderr, "image upload failed: %ls (chunk %d)\n",
			        hid_error(d), page_num);
			return -1;
		}

		if (debug)
			fprintf(stderr, "chunk %d sent successfully (%d bytes transferred)\n", page_num, r);
	}

	if (debug)
		fprintf(stderr, "image upload complete\n");

	return 0;
}

/*
 * tint is NULL for the normal path (convert_image()); non-NULL routes
 * through convert_image_tinted() instead, for SVG "symbolic" icon sets
 * whose embedded fill color is meant to be overridden, not shown as-is.
 */
static int
upload_image(hid_device *d, int key, const char *path, const unsigned char *tint)
{
	int rc;
	ImageBlob blob;

	if (!path || !path[0]) {
		fprintf(stderr, "No image path specified\n");
		return -1;
	}

	/* Every input format (svg/png/jpg/jpeg) goes through convert_image()
	 * uniformly, so a pre-made JPEG that happens to be the wrong size or
	 * orientation still comes out correct instead of being uploaded
	 * byte-for-byte as-is. */
	if (debug)
		fprintf(stderr, "Converting image: %s\n", path);
	blob = tint ? convert_image_tinted(path, tint[0], tint[1], tint[2]) : convert_image(path);
	if (!blob.data) {
		fprintf(stderr, "Image conversion failed\n");
		return -1;
	}

	fprintf(stderr, "uploading image %s to key %d\n", path, key + 1);

	rc = send_image_chunks(d, key, blob.data, (int)blob.size);
	free(blob.data);

	return rc;
}

/*
 * Uploads a solid black image to a key that has no image= configured, so
 * it doesn't keep showing whatever was left on it by a previous run (or
 * the official Elgato software). Only used for the full multi-button
 * flow (-c / no args), never for single-key -i updates -- those exist
 * specifically to touch one button without disturbing the others.
 */
static int
upload_blank(hid_device *d, int key)
{
	ImageBlob blob = blank_image();
	int rc;

	if (!blob.data) {
		fprintf(stderr, "Failed to generate blank image for key %d\n", key + 1);
		return -1;
	}

	fprintf(stderr, "uploading blank image to key %d (%zu bytes)\n", key + 1, blob.size);

	rc = send_image_chunks(d, key, blob.data, (int)blob.size);
	free(blob.data);
	return rc;
}

/*
 * MK.2 brightness is set via a HID feature report (not a regular OUT
 * report like images/button state): report ID 0x03, subcommand 0x08,
 * then the percent as a single byte, matching python-elgato-streamdeck's
 * StreamDeckOriginalV2.set_brightness().
 */
static int
set_brightness(hid_device *d, int percent)
{
	unsigned char payload[33] = {0};
	int r;

	if (percent < 0)
		percent = 0;
	if (percent > 100)
		percent = 100;

	payload[0] = 0x03;
	payload[1] = 0x08;
	payload[2] = (unsigned char)percent;

	r = hid_send_feature_report(d, payload, sizeof(payload));
	if (r < 0) {
		fprintf(stderr, "set_brightness failed: %ls\n", hid_error(d));
		return -1;
	}

	fprintf(stderr, "brightness set to %d%%\n", percent);
	return 0;
}

/*
 * We tried getting single-threaded operation by hand (raw libusb async
 * transfers driven by our own poll(2) loop over libusb_get_pollfds(),
 * plus LIBUSB_OPTION_NO_DEVICE_DISCOVERY + a manual sysfs walk to open
 * the device without triggering libusb's internal udev hotplug-monitor
 * thread). It worked, but needed a Linux-specific hand-rolled device
 * finder since NO_DEVICE_DISCOVERY makes libusb_get_device_list() always
 * return empty.
 *
 * The hidapi-hidraw backend gets the same result for free and more
 * simply: hid_enumerate()/hid_open() do portable device discovery
 * internally (their own sysfs walk, not ours), and -- critically -- the
 * hidraw backend has no persistent background thread at all. Unlike the
 * libusb backend (which needs a thread continuously resubmitting async
 * transfers to avoid missing reports), hidraw reads are just poll()+
 * read() on a plain character device fd, and the kernel itself queues
 * incoming HID reports internally regardless of whether we're actively
 * reading -- exactly the "always listening" property we were trying to
 * build by hand, provided by the kernel for free. Net result: one
 * thread, no libusb, standard hidapi API.
 */
static void
execute_command(const char *cmd)
{
	pid_t pid;

	if (debug)
		fprintf(stderr, "exec: %s\n", cmd);

	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "fork failed: %s\n", strerror(errno));
		return;
	}
	if (pid == 0) {
		/* Child: run the command through a real shell so that
		 * exec() strings can freely use &&, ||, pipes, $VAR
		 * expansion, etc, exactly as if typed at a prompt. Lua
		 * variables/tables replace the old [vars] macro system, so
		 * there's no custom var substitution here anymore -- just
		 * our real inherited environment. */
		char *argv[] = {"/bin/sh", "-c", (char *)cmd, NULL};
		execve("/bin/sh", argv, environ);
		_exit(127);
	}
	/* Parent: fire-and-forget. SIGCHLD is SIG_IGN (set in main()), so
	 * the kernel reaps the child automatically -- no zombies, no
	 * waitpid() needed, and we never block the button-read loop
	 * waiting on a command to finish. */
}

/*
 * Runs cmd through /bin/sh -c via popen() and returns its captured stdout
 * as a Lua string (trailing newline stripped). Unlike exec(), this
 * blocks until the command finishes -- intended for short synchronous
 * queries from inside an on_press() handler (e.g. "wpctl get-volume ..."
 * to decide which icon to show next), not for long-running commands.
 */
static int
l_capture(lua_State *lua)
{
	const char *cmd = luaL_checkstring(lua, 1);
	FILE *f;
	char buf[4096];
	size_t total = 0;
	size_t n;

	if (debug)
		fprintf(stderr, "capture: %s\n", cmd);

	f = popen(cmd, "r");
	if (!f) {
		fprintf(stderr, "popen failed for '%s': %s\n", cmd, strerror(errno));
		lua_pushstring(lua, "");
		return 1;
	}

	n = fread(buf, 1, sizeof(buf) - 1, f);
	total = n;
	buf[total] = '\0';
	pclose(f);

	while (total > 0 && (buf[total - 1] == '\n' || buf[total - 1] == '\r'))
		buf[--total] = '\0';

	lua_pushstring(lua, buf);
	return 1;
}

static int
l_exec(lua_State *lua)
{
	const char *cmd = luaL_checkstring(lua, 1);
	execute_command(cmd);
	return 0;
}

/*
 * All Lua-facing key numbers are 1-15 (matching the physical 3x5 grid
 * printed on the device and how a person points at a button), while
 * everything internal -- hardware button-state indices, array indices,
 * the wire protocol's key byte -- stays 0-14. This is the one place that
 * translates between the two; every l_* function below should read key
 * numbers through here rather than using luaL_checkinteger() directly.
 */
static int
check_key_arg(lua_State *lua, int arg_index)
{
	int key = (int)luaL_checkinteger(lua, arg_index);

	if (key < 1 || key > BUTTON_COUNT)
		return luaL_error(lua, "key %d out of range (1-%d)", key, BUTTON_COUNT);

	return key - 1;
}

/*
 * prisma.image(key, path) uploads path as-is. prisma.image(key, path, r,
 * g, b) additionally tints SVG input -- the source SVG's own fill colors
 * are ignored and replaced with (r, g, b), using its alpha channel as a
 * stencil mask. Meant for "symbolic" icon sets (GNOME/Adwaita-style
 * monochrome status icons hardcoded to a dark fill like #222222 for
 * theme-engine recoloring) that would otherwise render as nearly
 * invisible dark-on-black.
 */
static int
l_image(lua_State *lua)
{
	int key = check_key_arg(lua, 1);
	const char *path = luaL_checkstring(lua, 2);
	int nargs = lua_gettop(lua);
	unsigned char tint[3];
	const unsigned char *tint_ptr = NULL;

	if (nargs >= 5) {
		int r = (int)luaL_checkinteger(lua, 3);
		int g = (int)luaL_checkinteger(lua, 4);
		int b = (int)luaL_checkinteger(lua, 5);
		if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255)
			return luaL_error(lua, "color components must be 0-255");
		tint[0] = (unsigned char)r;
		tint[1] = (unsigned char)g;
		tint[2] = (unsigned char)b;
		tint_ptr = tint;
	}

	if (dev) {
		if (upload_image(dev, key, path, tint_ptr) < 0)
			fprintf(stderr, "warning: image upload failed for key %d\n", key + 1);
	}
	key_configured[key] = 1;
	return 0;
}

static int
l_color(lua_State *lua)
{
	int key = check_key_arg(lua, 1);
	int r = (int)luaL_checkinteger(lua, 2);
	int g = (int)luaL_checkinteger(lua, 3);
	int b = (int)luaL_checkinteger(lua, 4);

	if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255)
		return luaL_error(lua, "color components must be 0-255");

	if (dev) {
		ImageBlob blob = solid_color_image((unsigned char)r, (unsigned char)g, (unsigned char)b);
		if (!blob.data) {
			fprintf(stderr, "warning: color image synthesis failed for key %d\n", key + 1);
		} else {
			fprintf(stderr, "uploading color #%02x%02x%02x to key %d\n", r, g, b, key + 1);
			if (send_image_chunks(dev, key, blob.data, (int)blob.size) < 0)
				fprintf(stderr, "warning: color upload failed for key %d\n", key + 1);
			free(blob.data);
		}
	}
	key_configured[key] = 1;
	return 0;
}

static int
l_blank(lua_State *lua)
{
	int key = check_key_arg(lua, 1);

	if (dev)
		upload_blank(dev, key);
	key_configured[key] = 1;
	return 0;
}

static int
l_brightness(lua_State *lua)
{
	int percent = (int)luaL_checkinteger(lua, 1);

	if (percent < 0 || percent > 100)
		return luaL_error(lua, "brightness must be 0-100");

	if (dev)
		set_brightness(dev, percent);
	return 0;
}

static int
l_on_press(lua_State *lua)
{
	int key = check_key_arg(lua, 1);

	luaL_checktype(lua, 2, LUA_TFUNCTION);

	if (button_refs[key] != LUA_NOREF)
		luaL_unref(lua, LUA_REGISTRYINDEX, button_refs[key]);

	lua_pushvalue(lua, 2);
	button_refs[key] = luaL_ref(lua, LUA_REGISTRYINDEX);
	return 0;
}

/*
 * prisma.every(seconds, function) registers a repeating timer, fired from
 * the main event loop. seconds may be fractional (e.g. prisma.every(0.1,
 * ...)) -- main()'s hid_read_timeout() poll deadline is shortened to the
 * next timer's deadline via next_timeout_ms() below, so sub-second
 * intervals actually fire close to on time rather than being quantized
 * to whole seconds. Precision is still bounded by how long a single
 * on_press()/timer handler takes to run, since everything shares the one
 * event-loop thread.
 */
static int
l_every(lua_State *lua)
{
	double interval = luaL_checknumber(lua, 1);
	TimerEntry *t;

	luaL_checktype(lua, 2, LUA_TFUNCTION);

	if (interval <= 0)
		return luaL_error(lua, "interval must be > 0");

	t = realloc(timers, (size_t)(num_timers + 1) * sizeof(TimerEntry));
	if (!t)
		return luaL_error(lua, "malloc failed");
	timers = t;

	lua_pushvalue(lua, 2);
	timers[num_timers].func_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
	timers[num_timers].interval = interval;
	timers[num_timers].next_fire = now_seconds() + interval;
	num_timers++;

	return 0;
}

static const luaL_Reg prisma_funcs[] = {
	{"image",      l_image},
	{"blank",      l_blank},
	{"color",      l_color},
	{"brightness", l_brightness},
	{"exec",       l_exec},
	{"capture",    l_capture},
	{"on_press",   l_on_press},
	{"every",      l_every},
	{NULL, NULL}
};

/*
 * Resolves the default config path: $XDG_CONFIG_HOME/prisma/prisma.lua,
 * or ~/.config/prisma/prisma.lua if XDG_CONFIG_HOME isn't set. Returns a
 * malloc'd string, or NULL if neither HOME nor XDG_CONFIG_HOME is set.
 */
static char *
default_config_path(void)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	const char *base;
	const char *suffix;
	char *path;
	size_t len;

	if (xdg && xdg[0]) {
		base = xdg;
		suffix = "/prisma/prisma.lua";
	} else if (home && home[0]) {
		base = home;
		suffix = "/.config/prisma/prisma.lua";
	} else {
		return NULL;
	}

	len = strlen(base) + strlen(suffix) + 1;
	path = malloc(len);
	if (!path)
		return NULL;

	snprintf(path, len, "%s%s", base, suffix);
	return path;
}

/*
 * If path doesn't exist yet, creates its parent directory (mkdir -p one
 * level deep, which is all that's needed for .../prisma/prisma.lua) and
 * writes out the config embedded at build time from prisma.lua (see
 * default_config.h, generated by meson.build via xxd -i) -- so a fresh
 * install has a working, commented example config instead of erroring
 * out with "no such file".
 */
static void
ensure_default_config(const char *path)
{
	char *dir, *slash;
	FILE *f;

	if (access(path, F_OK) == 0)
		return;

	dir = strdup(path);
	if (!dir)
		return;
	slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
			/* Try creating the parent too (e.g. ~/.config didn't
			 * exist yet); best-effort, mkdir below will fail with
			 * a clear error if this still doesn't work. */
			char *parent_slash = strrchr(dir, '/');
			if (parent_slash) {
				*parent_slash = '\0';
				mkdir(dir, 0755);
				*parent_slash = '/';
				mkdir(dir, 0755);
			}
		}
	}
	free(dir);

	f = fopen(path, "wbe");
	if (!f) {
		fprintf(stderr, "warning: could not create default config %s: %s\n",
		        path, strerror(errno));
		return;
	}
	fwrite(default_config_lua, 1, default_config_lua_len, f);
	fclose(f);

	fprintf(stderr, "wrote default config to %s\n", path);
}

static int
load_config(const char *path)
{
	int i;

	L = luaL_newstate();
	if (!L) {
		fprintf(stderr, "failed to create Lua state\n");
		return -1;
	}
	luaL_openlibs(L);

	for (i = 0; i < BUTTON_COUNT; i++)
		button_refs[i] = LUA_NOREF;

	/* Register our C functions under a "prisma" module table so a config
	 * script reads like ordinary Lua using a real module: prisma.image(3,
	 * "..."), prisma.on_press(3, function() ... end), etc, instead of
	 * polluting the global namespace. */
	luaL_newlib(L, prisma_funcs);
	lua_setglobal(L, "prisma");

	if (luaL_dofile(L, path) != LUA_OK) {
		fprintf(stderr, "lua config error: %s\n", lua_tostring(L, -1));
		lua_pop(L, 1);
		return -1;
	}

	return 0;
}

static void
dispatch_button(int key)
{
	if (button_refs[key] == LUA_NOREF)
		return;

	lua_rawgeti(L, LUA_REGISTRYINDEX, button_refs[key]);
	if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
		fprintf(stderr, "on_press(%d) error: %s\n", key + 1, lua_tostring(L, -1));
		lua_pop(L, 1);
	}
}

/*
 * Milliseconds until the earliest prisma.every() deadline. main()'s
 * hid_read_timeout() uses this as its poll timeout instead of a fixed
 * 1000ms, so sub-second intervals (prisma.every(0.1, ...)) actually fire
 * at close to their configured rate rather than being quantized to
 * whole seconds -- we just wake up sooner when a timer is due sooner.
 * With no timers registered there's nothing to wake up early for, so
 * this returns -1, hidapi's "block indefinitely until a report
 * arrives" value -- no need to keep polling every second just in case a
 * timer might exist.
 */
static int
next_timeout_ms(void)
{
	double now, earliest, remaining_ms;
	int i;

	if (num_timers == 0)
		return -1;

	earliest = timers[0].next_fire;
	for (i = 1; i < num_timers; i++) {
		if (timers[i].next_fire < earliest)
			earliest = timers[i].next_fire;
	}

	now = now_seconds();
	remaining_ms = (earliest - now) * 1000.0;
	if (remaining_ms < 0)
		remaining_ms = 0;

	return (int)remaining_ms;
}

/*
 * Fires any prisma.every() timer whose deadline has passed. Rescheduled
 * from "now" rather than the missed deadline, so a slow handler (or a
 * long gap between main-loop wakeups) doesn't cause a burst of
 * catch-up calls -- it just resumes ticking at the configured interval
 * from whenever it actually ran.
 */
static void
run_timers(void)
{
	int i;
	double now = now_seconds();

	for (i = 0; i < num_timers; i++) {
		if (now < timers[i].next_fire)
			continue;

		lua_rawgeti(L, LUA_REGISTRYINDEX, timers[i].func_ref);
		if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
			fprintf(stderr, "prisma.every() handler error: %s\n", lua_tostring(L, -1));
			lua_pop(L, 1);
		}
		timers[i].next_fire = now_seconds() + timers[i].interval;
	}
}

static void
print_buttons(ButtonReport *state)
{
	int i;
	char buf[256] = {0};
	char *p = buf;

	for (i = 0; i < BUTTON_COUNT; i++) {
		int pressed = state->buttons[i] != 0;
		int was_pressed = prev_buttons[i] != 0;

		if (pressed && !was_pressed) {
			p += sprintf(p, "btn%d ", i);
			if (L)
				dispatch_button(i);
		}
		prev_buttons[i] = state->buttons[i];
	}

	if (buf[0] && debug)
		printf("pressed: %s\n", buf);
}

static void
usage(char *argv0)
{
	fprintf(stderr, "usage: %s [-d] [-c config.lua] [-w seconds] [-b percent] [-i key image] ...\n", argv0);
	fprintf(stderr, "  -d              debug output\n");
	fprintf(stderr, "  -c config.lua   load Lua config script (images, brightness, on_press handlers)\n");
	fprintf(stderr, "  -w seconds      wait time after image upload (default: 1 second)\n");
	fprintf(stderr, "  -b percent      set screen brightness 0-100 on startup\n");
	fprintf(stderr, "  -i key image    upload JPEG image to key 1-15 (one-shot)\n");
	exit(1);
}

int
main(int argc, char *argv[])
{
	int r = 0;
	int opt;
	const char *config_path = NULL;

	while ((opt = getopt(argc, argv, "dc:w:b:i:")) != -1) {
		switch (opt) {
		case 'd':
			debug++;
			break;
		case 'c':
			config_path = optarg;
			break;
		case 'w':
			wait_seconds = atoi(optarg);
			break;
		case 'b':
			brightness = atoi(optarg);
			if (brightness < 0 || brightness > 100) {
				fprintf(stderr, "brightness must be 0-100\n");
				return 1;
			}
			break;
		case 'i': {
			int key;
			char *path;
			ImageUpload *u;

			if (optind >= argc)
				usage(argv[0]);

			key = atoi(optarg);
			path = argv[optind++];

			if (key < 1 || key > BUTTON_COUNT) {
				fprintf(stderr, "key %d out of range (1-%d)\n", key, BUTTON_COUNT);
				return 1;
			}

			u = realloc(uploads, (num_uploads + 1) * sizeof(ImageUpload));
			if (!u) {
				fprintf(stderr, "malloc failed\n");
				return 1;
			}
			uploads = u;
			uploads[num_uploads].key = key - 1;
			uploads[num_uploads].path = strdup(path);
			if (!uploads[num_uploads].path) {
				fprintf(stderr, "strdup failed\n");
				return 1;
			}
			num_uploads++;
			image_only = 1;
			break;
		}
		default:
			usage(argv[0]);
		}
	}

	/* Button commands are fired via fork()+execve() and never
	 * waitpid()'d (see execute_command()) -- ignoring SIGCHLD makes
	 * the kernel reap them automatically instead of leaving zombies. */
	signal(SIGCHLD, SIG_IGN);

	if (hid_init() != 0) {
		fprintf(stderr, "hid_init failed\n");
		return 1;
	}

	dev = hid_open(ELGATO_VENDOR_ID, ELGATO_MK2_PRODUCT_ID, NULL);
	if (!dev) {
		fprintf(stderr, "Stream Deck MK.2 not found: %ls\n", hid_error(NULL));
		hid_exit();
		return 1;
	}
	if (debug)
		fprintf(stderr, "Found Stream Deck MK.2\n");

	if (brightness >= 0)
		set_brightness(dev, brightness);

	/* Read button states first (streamdeck-ui does this before uploading) */
	{
		ButtonReport state;
		if (debug)
			fprintf(stderr, "reading button states before upload...\n");
		r = hid_read_timeout(dev, (unsigned char *)&state, sizeof(state), 1000);
		if (r == 0) {
			if (debug)
				fprintf(stderr, "button read timeout (expected)\n");
		} else if (r < 0) {
			fprintf(stderr, "button read failed: %ls\n", hid_error(dev));
		} else if (debug) {
			fprintf(stderr, "button read: %d bytes\n", r);
		}
	}

	/* Send blank key report to clear display before uploading images */
	{
		unsigned char blank_report[1024] = {0};
		blank_report[0] = 0x02;  /* Command */
		if (debug)
			fprintf(stderr, "sending blank key report...\n");
		r = hid_write(dev, blank_report, sizeof(blank_report));
		if (r < 0) {
			fprintf(stderr, "blank key report failed: %ls\n", hid_error(dev));
		} else if (debug) {
			fprintf(stderr, "blank key report sent: %d bytes\n", r);
		}
	}

	/* -i one-shot uploads bypass Lua entirely. */
	for (int i = 0; i < num_uploads; i++) {
		if (debug)
			fprintf(stderr, "uploading: key %d, path %s\n", uploads[i].key + 1, uploads[i].path);
		if (upload_image(dev, uploads[i].key, uploads[i].path, NULL) < 0) {
			fprintf(stderr, "warning: image upload failed\n");
		}
	}

	{
		char *resolved_path = NULL;

		/* No -c given and not a one-shot -i upload: fall back to
		 * ~/.config/prisma/prisma.lua (or $XDG_CONFIG_HOME
		 * equivalent), auto-populating it from the embedded example
		 * config on first run so there's something to load instead
		 * of erroring out. */
		if (!config_path && !image_only) {
			resolved_path = default_config_path();
			if (resolved_path) {
				ensure_default_config(resolved_path);
				config_path = resolved_path;
			} else {
				fprintf(stderr, "warning: HOME/XDG_CONFIG_HOME not set, "
				                "no default config path available\n");
			}
		}

		if (config_path) {
			/* The config script's image()/blank() calls upload
			 * directly as they execute (dev is already open), and
			 * on_press() registers handlers for the event loop
			 * below. */
			if (load_config(config_path) < 0) {
				free(resolved_path);
				hid_close(dev);
				hid_exit();
				return 1;
			}
		}
		free(resolved_path);
	}

	if (!image_only) {
		/* Blank out any key that wasn't touched by image()/blank()
		 * in the config script, so it doesn't keep showing stale
		 * content from a previous run. Single-key -i updates skip
		 * this entirely -- that mode exists specifically to touch
		 * one button without disturbing the rest of the prisma. */
		for (int key = 0; key < BUTTON_COUNT; key++) {
			if (!key_configured[key])
				upload_blank(dev, key);
		}
	}

	if (image_only) {
		/* No button events to service; just hold the device open
		 * long enough for the uploaded image(s) to render. */
		if (debug)
			fprintf(stderr, "image-only mode, keeping device open for %d second(s)...\n", wait_seconds);
		sleep(wait_seconds);

		hid_close(dev);
		hid_exit();
		return 0;
	}

	printf("listening for button events...\n");
	fflush(stdout);

	/* hidraw's hid_read_timeout() is a plain poll()+read() on the
	 * /dev/hidrawN character device fd -- no background thread, since
	 * the kernel itself queues incoming HID reports internally
	 * regardless of whether we're actively reading. */
	for (;;) {
		ButtonReport state;
		int poll_ms = L ? next_timeout_ms() : 1000;

		r = hid_read_timeout(dev, (unsigned char *)&state, sizeof(state), poll_ms);
		if (r == 0) {
			if (L)
				run_timers();
			continue; /* timeout, nothing new */
		}
		if (r < 0) {
			fprintf(stderr, "hid_read_timeout failed: %ls\n", hid_error(dev));
			break;
		}

		if (r < (int)sizeof(state) && debug)
			fprintf(stderr, "transferred %d bytes, expected %zu\n",
			        r, sizeof(state));

		if (debug) {
			int bi;
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			fprintf(stderr, "[%ld.%03ld] read %d bytes: hdr %02x %02x %02x %02x buttons",
			        (long)ts.tv_sec, ts.tv_nsec / 1000000,
			        r, state.header[0], state.header[1],
			        state.header[2], state.header[3]);
			for (bi = 0; bi < BUTTON_COUNT; bi++)
				fprintf(stderr, " %02x", state.buttons[bi]);
			fprintf(stderr, "\n");
		}

		print_buttons(&state);
		if (L)
			run_timers();
	}

	if (L)
		lua_close(L);
	hid_close(dev);
	hid_exit();
	return 0;
}
