# Downloadable Content (DLC) Service Daemon

*** note
**Warning: This document is old & has moved.  Please update any links:**<br>
https://chromium.googlesource.com/chromiumos/platform2/+/HEAD/dlcservice/README.md
***

Read [Developer Doc] instead on how to use DLC framework.

## dlcservice
This D-Bus daemon manages life-cycles of DLC Modules and provides APIs to
install/uninstall DLC modules.

## dlcservice-client
This is a generated D-Bus client library for dlcservice. Other system services
that intend to interact with dlcservice are supposed to use this library.

[Developer Doc]: docs/developer.md
