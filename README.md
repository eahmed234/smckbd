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

`smckbd-keys` is a small root daemon for that. It reads the internal keyboard
read-only (it does not grab it, so the compositor keeps the keys), steps the
backlight by 32 on each press, and toggles on `KEY_KBDILLUMTOGGLE`.

```sh
sudo systemctl start smckbd-keys.service     # installed and enabled by the package
```

Without the package, from a build tree:

```sh
make keys && sudo ./smckbd-keys
```

If pressing F5/F6 still does nothing, check the keys reach userspace:

```sh
sudo evtest /dev/input/event4      # the event node with the Apple keyboard name
```

`KEY_KBDILLUMDOWN` / `KEY_KBDILLUMUP` on a press means the daemon will handle
them; `KEY_F5` / `KEY_F6` means the keyboard is in plain-function mode (try Fn,
or adjust `hid_apple.fnmode`).

## Notes

* Two LED names for one backlight is deliberate: the in-kernel driver owns
  `macsmc-led`, and userspace only knows the applesmc name.
* Unloading the module keeps the current brightness (`LED_RETAIN_AT_SHUTDOWN`).
* On a model without a keyboard backlight the LED still appears, and writes
  fail with `-EOPNOTSUPP` from the SMC.

## License

GPL-2.0-only, see [`LICENSE`](LICENSE).
