# WiFi Testbed tools

*** note
**Warning: This document is old & has moved.  Please update any links:**<br>
https://chromium.googlesource.com/chromiumos/platform2/+/HEAD/wifi-testbed/README.md
***

`testbed_regulatory` is a replacement for the "CRDA" application for
the ChromeOS WiFi testbed.  This is a sealed environment which is
not exposed to the open air.  As such, this program creates a
permissive rule set to the kernel to allow testing hardware at
various frequencies and power levels.

We also include a few small scripts that help make a testbed system more
friendly.
