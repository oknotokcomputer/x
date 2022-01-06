# Chromium OS cryptohome

*** note
**Warning: This document is old & has moved.  Please update any links:**<br>
https://chromium.googlesource.com/chromiumos/platform2/+/HEAD/cryptohome/README.md
***

This directory contains source code and documentation for the cryptohome
daemon.

*   [Homedirs]: creation and mounting/unmounting of per-user encrypted home
    directories.
*   [TPM Owner Initialization]
*   [Lockbox]: Tamper-evident, install-time system attribute storage.
*   [D-Bus]: cryptohome provides all functionality via a D-Bus interface.
*   [Challenge Response Key]
*   [LE Credentials]
*   [Firmware Management Parameters]

[Lockbox]: ./docs/lockbox.md
[Homedirs]: ./docs/homedirs.md
[D-Bus]: ./docs/dbus.md
[TPM Owner Initialization]: ./docs/tpm.md
[Challenge Response Key]: ./docs/challenge_response_key.md
[LE Credentials]: ./docs/le_credentials.md
[Firmware Management Parameters]: ./docs/firmware_management_parameters.md
