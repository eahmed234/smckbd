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
 * This daemon reads the internal keyboard (read-only, it does not grab the
 * device, so the compositor keeps receiving the keys) and adjusts the LED.
 * It needs root: /dev/input/event* and the LED brightness file are root-only.
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

static void write_led(int value)
{
	char buf[16];
	int fd, len, max;

	max = read_int(LED_MAX_PATH);
	if (max <= 0)
		max = 255;

	if (value < 0)
		value = 0;
	if (value > max)
		value = max;

	fd = open(LED_BRIGHTNESS, O_WRONLY);
	if (fd < 0)
		return;

	len = snprintf(buf, sizeof(buf), "%d\n", value);
	if (write(fd, buf, len) != len)
		perror("smckbd-keys: write brightness");
	close(fd);
}

/*
 * Find the event node of the internal keyboard by name. The keyboard is a USB
 * HID device whose input node carries both the keys and the trackpad, so the
 * name is the reliable part.
 */
static const char *keyboard_event(void)
{
	static char path[320];
	DIR *input, *ev;
	struct dirent *de, *evde;

	input = opendir("/sys/class/input");
	if (input == NULL)
		return NULL;

	while ((de = readdir(input)) != NULL) {
		char name_path[320], name[256];
		int fd;
		ssize_t n;

		if (strncmp(de->d_name, "input", 5) != 0)
			continue;

		snprintf(name_path, sizeof(name_path),
			 "/sys/class/input/%s/name", de->d_name);
		fd = open(name_path, O_RDONLY);
		if (fd < 0)
			continue;
		n = read(fd, name, sizeof(name) - 1);
		close(fd);
		if (n <= 0)
			continue;
		name[n] = '\0';

		if (strstr(name, KEYBOARD_NAME) == NULL)
			continue;

		snprintf(name_path, sizeof(name_path),
			 "/sys/class/input/%s", de->d_name);
		ev = opendir(name_path);
		if (ev == NULL)
			continue;

		while ((evde = readdir(ev)) != NULL) {
			if (strncmp(evde->d_name, "event", 5) != 0)
				continue;

			snprintf(path, sizeof(path), "/dev/input/%s",
				 evde->d_name);
			closedir(ev);
			closedir(input);
			return path;
		}

		closedir(ev);
	}

	closedir(input);
	return NULL;
}

int main(void)
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
				perror("smckbd-keys: open keyboard");
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

		switch (ev.code) {
		case KEY_KBDILLUMUP: {
			int cur = read_int(LED_BRIGHTNESS);

			if (cur > 0)
				last = cur;
			write_led((cur < 0 ? 0 : cur) + STEP);
			break;
		}
		case KEY_KBDILLUMDOWN: {
			int cur = read_int(LED_BRIGHTNESS);

			if (cur > 0)
				last = cur;
			write_led((cur < 0 ? 0 : cur) - STEP);
			break;
		}
		case KEY_KBDILLUMTOGGLE: {
			int cur = read_int(LED_BRIGHTNESS);

			if (cur > 0) {
				last = cur;
				write_led(0);
			} else {
				write_led(last > 0 ? last : 64);
			}
			break;
		}
		default:
			break;
		}
	}
}
