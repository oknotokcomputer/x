# ARC OBB mounter

*** note
**Warning: This document is old & has moved.  Please update any links:**<br>
https://chromium.googlesource.com/chromiumos/platform2/+/HEAD/arc/container/obb-mounter/README.md
***

`arc-obb-mounter` is a D-Bus service which provides two methods: MountObb and
UnmountObb. When MountObb is called, `arc-obb-mounter` launches a new
`mount-obb` process to mount the specified [OBB file] on the specified location.
When UnmountObb is called, `arc-obb-mounter` unmounts the mounted file system
from the specified location and kills the corresponding `mount-obb` process.

[OBB file]: https://developer.android.com/google/play/expansion-files.html
