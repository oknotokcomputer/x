# SMB FUSE filesystem

*** note
**Warning: This document is old & has moved.  Please update any links:**<br>
https://chromium.googlesource.com/chromiumos/platform2/+/HEAD/smbfs/README.md
***

This directory contains a FUSE filesystem for accessing SMB file shares.

It uses libsmbclient from the Samba project to handle the SMB protocol,
and Mojo to communicate with Chrome.
