# CUPS Proxy Daemon

*** note
**Warning: This document is old & has moved.  Please update any links:**<br>
https://chromium.googlesource.com/chromiumos/platform2/+/HEAD/cups_proxy/README.md
***

[//]: # TODO(pihsun): Complete Document.

## Summary

The CUPS Proxy Daemon is responsible for converting HTTP requests from the VM’s
file socket and pass that to the CupsProxyService in Chrome via Mojo.

## Design docs

* Overall design: [go/cups-proxy-daemon]


[go/cups-proxy-daemon]: http://go/cups-proxy-daemon
