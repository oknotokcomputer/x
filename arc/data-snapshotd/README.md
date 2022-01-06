# Chrome OS ARC data snapshotd daemon

*** note
**Warning: This document is old & has moved.  Please update any links:**<br>
https://chromium.googlesource.com/chromiumos/platform2/+/HEAD/arc/data-snapshotd/README.md
***

This package implements arc-data-snapshotd, a running in minijail daemon that
executes operations with ARC snapshots of /data directory for Managed Guest
Session (MGS) requested by Chrome browser.

The arc-data-snapshotd interface is exposed to Chrome browser through a D-Bus
API.
