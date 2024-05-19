#!/bin/sh
# Install to /usr/lib/systemd/system-sleep/

busctl emit /BatteryStats BatteryStats.Sleep SystemdSleepEvent sss "$1" "$2" "$SYSTEMD_SLEEP_ACTION"
