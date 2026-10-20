# Surface Dial: a second Bluetooth adapter, and what it breaks on GNOME

The Surface Dial is one of this project's supported augmented-input devices
(see the top-level README and `notes.md`, "Surface Dial input"). On some
machines, the *internal* Bluetooth adapter simply cannot see it — pairing
just times out, with no error explaining why. Its and Windows' own
first-party support both nudge toward the "official" USB dongle for exactly
this reason.

Adding a second Bluetooth adapter to a machine that already has one solves
that problem, but creates a different one: GNOME 50's Bluetooth quick-settings
menu doesn't remember a preferred adapter across a suspend/resume cycle. In
practice, it favors whichever adapter was *most recently registered* by the
kernel — which, after resume, isn't guaranteed to be the same one it was
before. If your everyday Bluetooth devices (a speaker, headphones, a mouse)
are paired against your *other* adapter, they can silently stop
auto-reconnecting after every suspend, because GNOME is now looking at the
Dial's dongle instead.

This directory documents that problem and a fix, independent of the rest of
this project's own code — nothing here is specific to blind-sight-lib, it's
a general GNOME/Bluetooth environment note for anyone using a Surface Dial
(or any other device needing its own dedicated adapter) alongside other
Bluetooth hardware.

## Confirming it's actually this problem

1. List your adapters and see which one GNOME currently considers current:

   ```sh
   bluetoothctl list
   ```

2. Check whether your everyday device is paired against the *other* one —
   compare the adapter MAC address it's connected through (visible in
   `dmesg`/`journalctl` at connection time) against the one `bluetoothctl
   list` marks as current.
3. Suspend and resume once, then re-run `bluetoothctl list` — if the
   "current" adapter changed without you doing anything, this is the
   problem described here.

## The known-working dongle

A Bluetooth 4.0 USB adapter, confirmed to work well with the Surface Dial,
identifies itself as:

```
ID 0b05:17cb ASUSTek Computer, Inc. Broadcom BCM20702A0 Bluetooth
```

(vendor:product `0b05:17cb` — check your own dongle against this with
`lsusb` after plugging it in). A Bluetooth 5.0 version of the same dongle
also exists and is known to work, under a different vendor:product ID —
not documented here in detail since the BT4 one is what this fix was
built and tested against.

## The fix: force a deterministic bind order on every resume

GNOME's "most recently registered adapter" preference can be turned to your
advantage: force the adapter you actually want to be default to *always*
finish registering last, on every resume, regardless of the USB
enumeration order the kernel happens to pick that time.

The fix is a systemd
[sleep hook](https://www.freedesktop.org/software/systemd/man/latest/systemd-suspend.service.html#Sleep%20hooks) —
it unbinds every Bluetooth USB adapter before suspend, then rebinds them
in a fixed, deliberate order after resume: any "special" (secondary)
adapters first, so they get the lowest `hci` index, and your primary
adapter last, so it gets the highest index and becomes GNOME's favorite
again.

**Searching for this problem turns up a lot of older material that
predates systemd** and places similar scripts in the wrong location for
a modern system (e.g. under `pm-utils`'s `/etc/pm/sleep.d/`, which
nothing calls any more). Ubuntu made systemd (replacing Upstart) the
default init system as of **15.04 (Vivid Vervet), March 2015** — but not
as a single clean cutover: Upstart stayed installed and switchable back
to for a while after that (Debian didn't drop the `upstart` package from
its archives until December 2015), and a separate transition for
*session*-level startup (distinct from the system/boot level this fix
cares about) happened later still, around 16.10. Anything written before
roughly this window, or for a still-Upstart-based system, is describing a
mechanism this fix doesn't use. `/lib/systemd/system-sleep/` (below) is
the current, correct location.

The actual script is
[`00-bluetooth-adapter-order.sh`](00-bluetooth-adapter-order.sh) in this
directory. **You must customize its two ID variables before using it** —
they identify *your own* hardware, not a universal constant. Find yours
with:

```sh
lsusb
```

Match the vendor:product pairs (the `xxxx:xxxx` after `ID`) against your
own primary adapter and any secondary/special adapters (like a Surface
Dial dongle), then edit `PRIMARY_DEVICE`/`SPECIAL_DEVICES` at the top of
the script accordingly.

Install it as a systemd sleep hook (the numeric prefix and `.sh`
extension match the naming convention Debian/Ubuntu's own sleep-hook
examples use, to get a defined run order if other hooks are also
present):

```sh
sudo cp 00-bluetooth-adapter-order.sh /lib/systemd/system-sleep/
sudo chmod +x /lib/systemd/system-sleep/00-bluetooth-adapter-order.sh
```

### Also covering a fresh reboot, not just suspend/resume

A systemd sleep hook only ever runs around suspend/resume — it never
fires on a cold boot at all. At reboot, USB enumeration order is
whatever the kernel/BIOS happens to produce, unconstrained by the script
above, so the same GNOME "most recently registered" problem could in
principle show up after a reboot too.

The fix doesn't need a second copy of the logic — boot has no "pre"
phase to prepare for the way sleep does, so
[`bluetooth-adapter-order-boot.service`](bluetooth-adapter-order-boot.service)
is just a plain boot-time systemd service that calls the *same* sleep-hook
script directly with `post`, reusing its bind-order logic outright rather
than duplicating it:

```sh
sudo cp bluetooth-adapter-order-boot.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now bluetooth-adapter-order-boot.service
```

## Known limitations

- Sleep hooks under `/lib/systemd/system-sleep/` run in plain alphabetical
  order of their filename — the `00-` prefix here is meant to make this
  one of the first to run, since it does fairly drastic things (unbinding
  USB devices, restarting `bluetooth.service`) other hooks might implicitly
  depend on happening before or after. If your system already has other
  sleep hooks installed, check their names (`ls /lib/systemd/system-sleep/`)
  and adjust the prefix if something else needs to run earlier or later —
  `00-` only guarantees "first" if nothing else claims an equal or lower
  name.
- This hasn't been tested against a genuine `hibernate` (suspend-to-disk)
  cycle, only regular `suspend` (the `$2` argument systemd passes to sleep
  hooks isn't checked here — the same logic runs for either). Ubuntu's
  default swap-file sizing (much smaller than total RAM since ~17.04/18.04)
  makes real hibernate unreliable or entirely non-functional on many
  systems regardless of this fix, independent of Bluetooth.
- The fixed `sleep` delays are a pragmatic buffer, not a real
  synchronization mechanism — they may need adjusting on a slower machine.
- Not GNOME-version-specific in how it checks anything; it's a description
  of *observed* GNOME 50 behavior, not something confirmed against GNOME's
  own source or release notes.
