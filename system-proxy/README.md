# System-proxy

*** note
**Warning: This document is old & has moved.  Please update any links:**<br>
https://chromium.googlesource.com/chromiumos/platform2/+/HEAD/system-proxy/README.md
***

This directory contains the System-proxy service which runs as a HTTP proxy on
Chrome OS and acts as a proxy authenticator at the OS level. Proxy aware system
services and ARC++ apps can connect to System-proxy which will perform the
authentication challenge to the remote proxy server and the connection setup
on behalf of the client.
