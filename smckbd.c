// SPDX-License-Identifier: GPL-2.0-only
/*
 * smckbd - expose the Intel-Mac keyboard backlight under the name userspace
 * expects.
 *
 * Linux 7.3 replaced applesmc with the macsmc stack. The keyboard backlight is
 * still driven through SMC key LKSB, but leds-macsmc registers the LED as
 * "macsmc-led". Desktop backlight controls and the F5/F6 keys look for the
 * applesmc name "smc::kbd_backlight", find no such LED and do nothing.
 *
 * macsmc-core's /dev/smc refuses writes to the keys userspace would need, but
 * apple_smc_write() is exported to modules, so this module registers a second
 * LED class device under the conventional name and drives the same key.
 *
 * LKSB is write-only on some models (a read returns -EHWPOISON), which is why
 * leds-macsmc only caches brightness. Reads here fall back to the cached value,
 * so writing always works and reading reflects what was written.
 *
 * Nothing in the macsmc stack is replaced or modified.
 */

#include <linux/leds.h>
#include <linux/mfd/macsmc.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/types.h>

static struct apple_smc *smckbd_smc;

/*
 * The SMC object is the drvdata of the device macsmc-acpi binds to
 * (APP0001:00), which is the parent of the MFD children.
 */
static const char *const smckbd_child_names[] = {
	"macsmc-led",
	"macsmc-hwmon",
};

static struct apple_smc *smckbd_find_smc(void)
{
	struct apple_smc *smc = NULL;
	struct device *dev;
	int i;

	for (i = 0; i < ARRAY_SIZE(smckbd_child_names) && smc == NULL; i++) {
		dev = bus_find_device_by_name(&platform_bus_type, NULL,
					      smckbd_child_names[i]);
		if (dev == NULL)
			continue;

		smc = dev_get_drvdata(dev->parent);
		put_device(dev);
	}

	return smc;
}

static int smckbd_set(struct led_classdev *led_cdev, enum led_brightness value)
{
	u16 raw = value;
	int ret;

	if (smckbd_smc == NULL)
		return -ENODEV;

	/* Same key, size and little-endian u16 that leds-macsmc writes. */
	ret = apple_smc_write(smckbd_smc, SMC_KEY(LKSB), &raw, sizeof(raw));
	return ret < 0 ? ret : 0;
}

static enum led_brightness smckbd_get(struct led_classdev *led_cdev)
{
	u16 raw;
	int ret;

	if (smckbd_smc == NULL)
		return led_cdev->brightness;

	ret = apple_smc_read(smckbd_smc, SMC_KEY(LKSB), &raw, sizeof(raw));
	/* Write-only key: report the value the LED core last set. */
	if (ret != sizeof(raw) || raw > LED_FULL)
		return led_cdev->brightness;

	return raw;
}

static struct led_classdev smckbd_led = {
	.name			= "smc::kbd_backlight",
	.max_brightness		= LED_FULL,
	.brightness_set_blocking = smckbd_set,
	.brightness_get		= smckbd_get,
	/* Do not switch the backlight off when this module is unloaded. */
	.flags			= LED_RETAIN_AT_SHUTDOWN,
};

static int __init smckbd_init(void)
{
	smckbd_smc = smckbd_find_smc();
	if (smckbd_smc == NULL) {
		pr_err("smckbd: no Apple SMC device found; is macsmc-core loaded?\n");
		return -ENODEV;
	}

	return led_classdev_register(NULL, &smckbd_led);
}

static void __exit smckbd_exit(void)
{
	led_classdev_unregister(&smckbd_led);
}

module_init(smckbd_init);
module_exit(smckbd_exit);

MODULE_SOFTDEP("pre: macsmc_core");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Keyboard backlight alias for Apple SMC (macsmc) machines");
MODULE_AUTHOR("Eisa Ahmed");
