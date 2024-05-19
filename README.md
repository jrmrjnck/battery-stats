A simple program that calculates basic battery statistics to help understand how
much life a laptop battery provides provides during usage or sleep.

Supports linux only.

Does the following:
1. Tracks battery energy levels by watching for D-Bus signals from UPower.
2. Tracks system sleep state by installing a hook to be run by systemd, which
   provides the sleep state to `battery-stats` via D-Bus signal. (You must
   install the script at /usr/lib/systemd/system-sleep/ manually.)
3. With this data, print statistics to the console:
    * Instantaneous power based on last two samples, both in Watts and %/hr.
    * Average power since the current charge or discharge cycle began, both in
    Watts and %/hr.
    * After resuming from sleep, the average power during the sleep cycle, both
    in Watts and %/day.
