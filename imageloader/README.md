# imageloader

*** note
**Warning: This document is old & has moved.  Please update any links:**<br>
https://chromium.googlesource.com/chromiumos/platform2/+/HEAD/imageloader/README.md
***

This aims to provide a generic utility to verify and load (mount) signed disk
images through DBUS IPC.

# Binaries

* `imageloader`

`imageloader` handles the mounting of disk images.
When `imageloader` is not running, DBus will automatically invoke it. After 20
seconds of inactivity, the service exits.
