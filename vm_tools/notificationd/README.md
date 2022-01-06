# Notification Daemon for Crostini (notificationd)

*** note
**Warning: This document is old & has moved.  Please update any links:**<br>
https://chromium.googlesource.com/chromiumos/platform2/+/HEAD/vm_tools/notificationd/README.md
***

notificationd is a new daemon which catches the notification request from
Crostini apps via D-BUS and forwards it to Chrome OS (host) via Wayland.
