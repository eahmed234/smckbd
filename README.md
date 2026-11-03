# Keyboard backlight under the name Linux expects

Linux 7.3 replaced `applesmc` with the `macsmc` stack. The keyboard backlight
still works — `leds-macsmc` drives it through SMC key `LKSB` — but it registers
the LED as **`macsmc-led`**. Desktop backlight controls, `brightnessctl` and
the F5/F6 keys look for the applesmc name **`smc::kbd_backlight`**, find nothing,
and do nothing.

`smckbd` registers a second LED class device under that conventional name and
drives the very same `LKSB` key through the exported `apple_smc_write()`. No
piece of the macsmc stack is replaced or modified.

## What it does

| | |
| --- | --- |
| LED name | `smc::kbd_backlight` |
| Range | 0–255, same as the in-kernel LED |
| Write | little-endian `u16` to `LKSB`, identical to `leds-macsmc` |
| Read | `LKSB` where readable; on models where it is write-only (MacBookPro11,2 returns `-EHWPOISON`), the value the LED core last set |

The in-kernel `macsmc-led` LED stays in place, so both names control the same
hardware. Unlike that driver, this one has no cached brightness to go stale:
writes and reads go to the same key.

## Requirements

* An Intel Mac whose SMC is bound by `macsmc` (kernel 7.3+). On CachyOS that is
  the Intel-Mac port of 7.3; older kernels still have applesmc, which already
  exposes `smc::kbd_backlight`, and DKMS skips them.
* `dkms` and the headers of the kernel you run.

## Install (Arch / CachyOS)

```sh
cd packaging/aur/smckbd-dkms-git && makepkg -si
sudo systemctl start smckbd.service     # DKMS may still be building at install time
```

Or from source:

```sh
make
sudo insmod smckbd.ko
```

## Autoloading

`smckbd.service` is a oneshot unit that runs `modprobe smckbd` at boot. Its
`ConditionPathExists=/sys/class/leds/macsmc-led` skips machines without the
macsmc backlight, so it does not fail on older kernels. The package enables the
unit but does not start it at install time.

## Verify

```sh
ls -l /sys/class/leds/ | grep kbd_backlight
cat /sys/class/leds/smc::kbd_backlight/max_brightness     # 255
echo 120 | sudo tee /sys/class/leds/smc::kbd_backlight/brightness
cat /sys/class/leds/smc::kbd_backlight/brightness          # 120
```

With `brightnessctl`: `sudo brightnessctl -d smc::kbd_backlight set 50%`.

## F5/F6 keys

Having the LED is not enough: something has to translate `KEY_KBDILLUMDOWN` and
`KEY_KBDILLUMUP` (what the Apple keyboard emits) into LED writes. Desktop
environments do that for hardware they know; a bare compositor does not, and
then the keys do nothing.

The keysym your compositor sees is `XF86KbdBrightnessDown` /
`XF86KbdBrightnessUp`. `smckbd-keys` handles them in two ways.

### One shot, from the session config

Use the **keyboard** keysyms. `XF86MonBrightnessUp`/`Down` are the *display*
backlight and must keep their screen-brightness action; binding them to
`smckbd-keys` leaves F5/F6 dead and makes the screen keys drive the keyboard.

Umbriel, which is what the Noctalia shell runs on, binding all four:

```toml
[keybinds]
"XF86MonBrightnessUp"   = { action = "spawn noctalia msg brightness-up",   allow_when_locked = true }
"XF86MonBrightnessDown" = { action = "spawn noctalia msg brightness-down", allow_when_locked = true }
"XF86KbdBrightnessUp"   = { action = "spawn smckbd-keys up",   allow_when_locked = true }
"XF86KbdBrightnessDown" = { action = "spawn smckbd-keys down", allow_when_locked = true }
```

Other compositors use the same keysym names (`XF86KbdBrightnessUp` /
`XF86KbdBrightnessDown`, from the `inet` section of the XKB rules, which applies
to every layout). Check the config with `umbriel config validate`: an unknown
keysym name is reported as a diagnostic.

This needs no root: `80-smckbd-led.rules` gives the `video` group write access
to the backlight. Check your user is in it (`id -nG`), and that the rule is in
place (`/usr/lib/udev/rules.d/80-smckbd-led.rules`, from the package). Applied
by hand, the rule needs one reload:

```sh
sudo udevadm control --reload && sudo udevadm trigger --subsystem-match=leds
ls -l /sys/class/leds/smc::kbd_backlight/brightness    # -rw-rw-r-- root video
```

The rule uses `RUN+=` with `chgrp`/`chmod` rather than `GROUP=`/`MODE=`: an LED
class device has no `/dev` node, and udev only applies those keys to device
nodes, never to sysfs attributes. Same reason the classic backlight recipes do.

### Daemon, for sessions with no key binding

`smckbd-keys` with no argument watches the keyboard and applies every press,
for compositors that never route the keysyms. It reads the keyboard read-only
and does not grab it, so the compositor keeps every key. It runs as root, so it
works with or without the udev rule.

The package installs the unit but leaves it disabled, because the two modes are
alternatives: with key bindings in place a running daemon would step the
backlight a second time for the same press. Enable it only if you have no
bindings:

```sh
sudo systemctl enable --now smckbd-keys.service
```

Without the package, from a build tree:

```sh
make keys && sudo ./smckbd-keys       # prints each change: 150 -> 182
```

Each press steps by 32 of 255; `KEY_KBDILLUMTOGGLE` turns the light off and
back to the previous level.

### If nothing happens

Check the keys reach userspace at all:

```sh
sudo evtest /dev/input/event6      # the node whose name has "Apple Internal Keyboard"
```

`KEY_KBDILLUMDOWN` (229) / `KEY_KBDILLUMUP` (230) on a press means the helper
can act on them; `KEY_F5` / `KEY_F6` means the keyboard is in plain-function
mode (try Fn, or adjust `hid_apple.fnmode`).

## Notes

* Two LED names for one backlight is deliberate: the in-kernel driver owns
  `macsmc-led`, and userspace only knows the applesmc name.
* Unloading the module keeps the current brightness (`LED_RETAIN_AT_SHUTDOWN`).
* On a model without a keyboard backlight the LED still appears, and writes
  fail with `-EOPNOTSUPP` from the SMC.

## License

GPL-2.0-only, see [`LICENSE`](LICENSE).
