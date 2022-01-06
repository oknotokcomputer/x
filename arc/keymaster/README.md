# Chrome OS arc-keymasterd

*** note
**Warning: This document is old & has moved.  Please update any links:**<br>
https://chromium.googlesource.com/chromiumos/platform2/+/HEAD/arc/keymaster/README.md
***

This package implements arc-keymasterd, a daemon that executes crypto operations
requested by ARC on Chrome OS, by running the android keymaster in a minijail.

The android keymaster interface is exposed to ARC through a mojo connection via
Chrome.

For more information about the Android security stack, see the [keystore docs].

[keystore docs]: https://developer.android.com/training/articles/keystore
