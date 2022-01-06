# D-Bus API

*** note
**Warning: This document is old & has moved.  Please update any links:**<br>
https://chromium.googlesource.com/chromiumos/platform2/+/HEAD/cryptohome/docs/dbus.md
***

[TOC]

## Overview

Other programs communicate with `cryptohomed` using its D-Bus API. This API
exposes both synchronous and asynchronous methods for mounting, unmounting,
removing, and changing passwords. It also gives status information about the
TPM. See cryptohome.xml for a brief listing of the APIs available. Note that the
asynchronous APIs complete using a signal (AsyncCallStatus). The completion
signals are guaranteed to be in-order.

BootLockbox functionality is provided by `bootlockboxd` daemon using D-Bus API.
This API exposes both synchronous and asynchronous methods for signing,
verifying and finalizing. See
`dbus_adaptors/org.chromium.BootLockboxInterface.xml` for a brief listing of the
APIs available.
