#!/bin/sh
# Forces a deterministic Bluetooth adapter bind order across suspend/
# resume, working around GNOME 50 favoring whichever adapter the kernel
# most recently registered — which isn't guaranteed to be the same one
# after every resume. See docs/surface-dial/README.md in blind-sight-lib
# for the full explanation.
#
# Install as a systemd sleep hook (the numeric prefix and .sh extension
# match the convention Debian/Ubuntu's own sleep-hook examples use, to
# get a defined run order if other hooks are also present):
#   sudo cp 00-bluetooth-adapter-order.sh /lib/systemd/system-sleep/
#   sudo chmod +x /lib/systemd/system-sleep/00-bluetooth-adapter-order.sh
# Customize the two ID variables below for your own hardware first —
# see README.md for how to find them with `lsusb`.

# --- CUSTOMIZE THESE FOR YOUR OWN MACHINE (see README.md: `lsusb`) ---
# Your primary/everyday adapter's vendor:product ID — this is the one you
# want GNOME to prefer. It gets rebound *last* on every resume. The value
# below (ID 0e8d:0608 MediaTek Inc. Wireless_Device) is one specific
# machine's internal combo Wi-Fi/Bluetooth card, given only as a concrete
# example — internal adapters vary a lot by machine/OEM, so this is not a
# generically-recommended value the way the dongle below is; replace it
# with whatever `lsusb` shows for your own primary adapter.
PRIMARY_DEVICE="0e8d:0608"
# Any secondary/special adapters (space-separated) that should get bound
# *first*, so they end up with lower hci indices and don't win GNOME's
# "most recent" preference. Example: a Surface Dial dongle.
SPECIAL_DEVICES="0b05:17cb"
# ---------------------------------------------------------------------

case "$1" in
    pre)
        /usr/bin/bluetoothctl power off 2>/dev/null
        /usr/bin/modprobe -r btusb
        ;;
    post)
        /usr/bin/modprobe btusb
        sleep 3

        primary_port=""
        special_ports=""

        for d in /sys/bus/usb/devices/*; do
            if [ -f "$d/idVendor" ] && [ -f "$d/idProduct" ]; then
                v=$(cat "$d/idVendor")
                p=$(cat "$d/idProduct")
                id="${v}:${p}"

                if [ "$id" = "$PRIMARY_DEVICE" ]; then
                    primary_port=$(basename "$d")
                else
                    for s_dev in $SPECIAL_DEVICES; do
                        if [ "$id" = "$s_dev" ]; then
                            special_ports="$special_ports $(basename "$d")"
                        fi
                    done
                fi
            fi
        done

        # Force-unbind everything first, to clear the bus.
        [ -n "$primary_port" ] && echo "$primary_port" > /sys/bus/usb/drivers/usb/unbind 2>/dev/null
        for p_path in $special_ports; do
            echo "$p_path" > /sys/bus/usb/drivers/usb/unbind 2>/dev/null
        done
        sleep 2

        # Special adapters bind first — lowest hci indices.
        for p_path in $special_ports; do
            echo "$p_path" > /sys/bus/usb/drivers/usb/bind
            sleep 1
        done

        # Primary adapter binds last — highest hci index, GNOME's favorite.
        if [ -n "$primary_port" ]; then
            echo "$primary_port" > /sys/bus/usb/drivers/usb/bind
            sleep 2
        fi

        /usr/bin/systemctl restart bluetooth
        sleep 2
        /usr/bin/bluetoothctl power on
        ;;
esac
