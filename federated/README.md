# Chrome OS Federated Computation Service

*** note
**Warning: This document is old & has moved.  Please update any links:**<br>
https://chromium.googlesource.com/chromiumos/platform2/+/HEAD/federated/README.md
***

## Summary

The federated computation service provides a common runtime for federated
analytics (F.A.) and federated learning (F.L.). The service wraps the [federated
computation client] which communicates with the federated computation server,
receives and manages examples from its clients (usually in Chromium) and
schedules the learning/analytics plan. See [go/cros-federated-design] for a
design overview.

[federated computation client]: http://go/fcp
[go/cros-federated-design]: http://go/cros-federated-design
