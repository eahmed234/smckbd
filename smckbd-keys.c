// SPDX-License-Identifier: GPL-2.0-only
/*
 * smckbd-keys - drive the keyboard backlight from the Apple keyboard's
 * illumination keys.
 *
 * On Linux 7.3+ the backlight is an LED (smc::kbd_backlight, provided by
 * smckbd), but the key events still need something to act on them. Desktop
 * environments only do that on hardware they recognise; a bare compositor does
 * nothing, so F5/F6 are dead even though the keyboard emits KEY_KBDILLUMDOWN
 * and KEY_KBDILLUMUP.
 *
 * Two modes:
 *
 *   smckbd-keys                 daemon: watch the keyboard and act on every
 *                               press. Runs as root (system service).
 *   smckbd-keys up|down|toggle  one shot: apply a single step and exit, for
 *                               compositor key bindings (XF86KbdBrightnessUp /
 *                               Down). Works as a normal user once the udev rule
 *                               gives the video group write access.
 *
 * The daemon reads the keyboard read-only and does not grab it, so the
 * compositor keeps receiving every key.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LED_DIR		"/sys/class/leds/smc::kbd_backlight"
#define LED_BRIGHTNESS	LED_DIR "/brightness"
#define LED_MAX_PATH	LED_DIR "/max_brightness"
#define KEYBOARD_NAME	"Apple Internal Keyboard"
#define STEP		32

static int read_int(const char *path)
{
	char buf[32];
	int fd, value;
	ssize_t n;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;

	buf[n] = '\0';
	value = atoi(buf);
	return value;
}

static int led_max(void)
{
	int max = read_int(LED_MAX_PATH);

	return max > 0 ? max : 255;
}

static int led_read(void)
{
	return read_int(LED_BRIGHTNESS);
}

/* Returns the value written, or -1 if the LED could not be written. */
static int led_write(int value)
{
	char buf[16];
	int fd, len;

	if (value < 0)
		value = 0;
	if (value > led_max())
		value = led_max();

	fd = open(LED_BRIGHTNESS, O_WRONLY);
	if (fd < 0) {
		fprintf(stderr,
			"smckbd-keys: cannot write " LED_BRIGHTNESS " (%s)\n",
			strerror(errno));
		return -1;
	}

	len = snprintf(buf, sizeof(buf), "%d\n", value);
	if (write(fd, buf, len) != len) {
		fprintf(stderr, "smckbd-keys: write %s failed (%s)\n",
			LED_BRIGHTNESS, strerror(errno));
		close(fd);
		return -1;
	}

	close(fd);
	return value;
}

/*
 * Test one bit in a sysfs input key bitmap. The file holds 64-bit words in hex,
 * most significant word first, space separated, so the last word holds bits
 * 0..63. Bit 229 (KEY_KBDILLUMDOWN) therefore lives in the fourth word from the
 * end on a keyboard that has it.
 */
static int bitmap_has(const char *text, unsigned int bit)
{
	const char *tok[32];
	char buf[1024];
	unsigned int word, ntok = 0;
	unsigned long long v;
	char *p;
	ssize_t n;
	int fd;

	fd = open(text, O_RDONLY);
	if (fd < 0)
		return 0;

	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = '\0';

	p = buf;
	while (*p != '\0' && ntok < 32) {
		while (*p == ' ' || *p == '\n')
			p++;
		if (*p == '\0')
			break;
		tok[ntok++] = p;
		while (*p != '\0' && *p != ' ' && *p != '\n')
			p++;
		if (*p != '\0')
			*p++ = '\0';
	}

	word = bit / 64;
	if (word >= ntok)
		return 0;

	v = strtoull(tok[ntok - 1 - word], NULL, 16);
	return (int)((v >> (bit % 64)) & 1);
}

static const char *file_contains(const char *path, const char *needle,
				 char *buf, size_t buflen)
{
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return NULL;

	n = read(fd, buf, buflen - 1);
	close(fd);
	if (n <= 0)
		return NULL;

	buf[n] = '\0';
	return strstr(buf, needle) != NULL ? buf : NULL;
}

/*
 * Find the event node that carries the illumination keys. Capabilities are the
 * reliable part: name the keyboard, and trackpad/audio nodes never have these
 * bits. Falls back to the device name for kernels or models where the bitmap
 * is not what we expect.
 */
static const char *keyboard_event(void)
{
	static char path[320], name_path[320], caps_path[320], name[256];
	DIR *dir;
	struct dirent *de;
	const char *fallback = NULL;
	static char fallback_path[320];

	dir = opendir("/sys/class/input");
	if (dir == NULL)
		return NULL;

	fallback_path[0] = '\0';

	while ((de = readdir(dir)) != NULL) {
		if (strncmp(de->d_name, "event", 5) != 0)
			continue;

		snprintf(caps_path, sizeof(caps_path),
			 "/sys/class/input/%s/device/capabilities/key",
			 de->d_name);

		if (bitmap_has(caps_path, KEY_KBDILLUMUP) ||
		    bitmap_has(caps_path, KEY_KBDILLUMDOWN)) {
			snprintf(path, sizeof(path), "/dev/input/%s",
				 de->d_name);
			closedir(dir);
			return path;
		}

		if (fallback != NULL)
			continue;

		snprintf(name_path, sizeof(name_path),
			 "/sys/class/input/%s/device/name", de->d_name);
		if (file_contains(name_path, KEYBOARD_NAME, name,
				  sizeof(name)) == NULL)
			continue;

		snprintf(fallback_path, sizeof(fallback_path), "/dev/input/%s",
			 de->d_name);
		fallback = fallback_path;
	}

	closedir(dir);

	if (fallback != NULL) {
		snprintf(path, sizeof(path), "%s", fallback);
		return path;
	}

	return NULL;
}

/*
 * Apply one key. Keeps the last non-zero level so TOGGLE can restore it; in
 * one-shot mode that state starts empty, so the first toggle turns the light on
 * at a modest level.
 */
static int apply(int code, int *last)
{
	int cur = led_read(), target;

	if (cur < 0) {
		fprintf(stderr,
			"smckbd-keys: cannot read " LED_BRIGHTNESS
			" (%s) - is the smckbd module loaded?\n",
			strerror(errno));
		return -1;
	}

	switch (code) {
	case KEY_KBDILLUMUP:
		if (cur > 0)
			*last = cur;
		target = cur + STEP;
		break;
	case KEY_KBDILLUMDOWN:
		if (cur > 0)
			*last = cur;
		target = cur - STEP;
		break;
	case KEY_KBDILLUMTOGGLE:
		if (cur > 0) {
			*last = cur;
			target = 0;
		} else {
			target = *last > 0 ? *last : 64;
		}
		break;
	default:
		return 0;
	}

	target = led_write(target);
	if (target >= 0)
		fprintf(stderr, "smckbd-keys: %d -> %d\n", cur, target);

	return target;
}

/* Not "daemon": that name is taken by unistd.h's daemon(3). */
static void watch_keys(void)
{
	int fd = -1, last = -1;

	for (;;) {
		struct input_event ev;

		if (fd < 0) {
			const char *path = keyboard_event();

			if (path == NULL) {
				sleep(2);
				continue;
			}

			fd = open(path, O_RDONLY);
			if (fd < 0) {
				fprintf(stderr,
					"smckbd-keys: open %s failed (%s)\n",
					path, strerror(errno));
				sleep(2);
				continue;
			}
			fprintf(stderr, "smckbd-keys: watching %s\n", path);
		}

		if (read(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
			close(fd);
			fd = -1;
			sleep(1);
			continue;
		}

		if (ev.type != EV_KEY || ev.value != 1)
			continue;

		apply(ev.code, &last);
	}
}

int main(int argc, char **argv)
{
	int last = -1;

	if (argc > 1) {
		static const struct {
			const char *name;
			int code;
		} modes[] = {
			{ "up", KEY_KBDILLUMUP },
			{ "down", KEY_KBDILLUMDOWN },
			{ "toggle", KEY_KBDILLUMTOGGLE },
		};
		size_t i;

		if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
			goto usage;

		for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
			if (strcmp(argv[1], modes[i].name) == 0)
				return apply(modes[i].code, &last) < 0 ? 1 : 0;
		}

usage:
		fprintf(stderr,
			"usage: smckbd-keys [up|down|toggle]\n"
			"       with no argument: stay in the foreground and watch the keyboard\n");
		return argc > 1 && strcmp(argv[1], "--help") != 0 &&
		       strcmp(argv[1], "-h") != 0 ? 2 : 0;
	}

	watch_keys();
	return 0;
}
