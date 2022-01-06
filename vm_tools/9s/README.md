# 9s - [9p] server program

*** note
**Warning: This document is old & has moved.  Please update any links:**<br>
https://chromium.googlesource.com/chromiumos/platform2/+/HEAD/vm_tools/9s/README.md
***

This is a very thin wrapper around the [p9] library crate.  It takes care of
parsing command-line options, accepting incoming client connections, and then
handing things off to the [p9] crate.

[9p]: http://man.cat-v.org/plan_9/5/intro
[p9]: ../p9
