# Downloadable Content (DLC) Service Daemon Utility

*** note
**Warning: This document is old & has moved.  Please update any links:**<br>
https://chromium.googlesource.com/chromiumos/platform2/+/HEAD/dlcservice/tools/README.md
***

## dlcservice-util

This is a wrapper utility for `dlcservice`. It can be used to install and
uninstall DLC modules, as well as print a list of installed modules.

## Usage

To install DLC modules, set the `--install` flag and set `--dlc_ids` to a colon
separated list of DLC IDs.

`dlcservice_util --install --dlc_ids="foo:bar"`

To uninstall DLC modules, set the `--uninstall` flag and set `--dlc_ids` to a
colon separated list of DLC IDs.

`dlcservice_util --uninstall --dlc_ids="foo:bar"`

To list installed modules, set the `--list` flag.

`dlcservice_util --list`
