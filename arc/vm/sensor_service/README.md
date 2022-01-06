# ARC sensor service

*** note
**Warning: This document is old & has moved.  Please update any links:**<br>
https://chromium.googlesource.com/chromiumos/platform2/+/HEAD/arc/vm/sensor_service/README.md
***

`arc-sensor-service` is a D-Bus service which provides sensor-related mojo interfaces for ARC to implement [Android Sensors API].
Chrome uses D-Bus to establish a mojo connection with this service.

[Android Sensors API]: https://developer.android.com/guide/topics/sensors/sensors_overview
