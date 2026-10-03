# Third-party notices

The files in `bin/` are built by `build.sh` and contain the following third-party code, linked statically. The source of each one is downloaded by `build.sh` at the version given below.

| Component | Used in | License | Source |
|---|---|---|---|
| libusb 1.0.27 | all four files | LGPL-2.1-or-later | https://github.com/libusb/libusb |
| libairspyhf (commit `24fe8ff`) | `AirspyHfIntf.dll`, `AirspyHfIntfConfig.exe` | BSD 3-Clause | https://github.com/airspy/airspyhf |
| rtl-sdr, RTL-SDR Blog fork (commit `aed0ea1`) | `RtlSdrIntf.dll`, `RtlSdrIntfConfig.exe` | GPL-2.0-or-later | https://github.com/rtlsdrblog/rtl-sdr-blog |
| winpthreads (MinGW-w64 runtime) | `AirspyHfIntf.dll`, `AirspyHfIntfConfig.exe` | MIT / BSD 3-Clause | https://www.mingw-w64.org |

Because the RTL-SDR driver is licensed under the GPL, `RtlSdrIntf.dll` and `RtlSdrIntfConfig.exe` as distributed here are covered by the GPL, version 2 or later. The complete source needed to rebuild every file is in this repository together with the sources that `build.sh` downloads.

CW Skimmer, Skimmer Server and RTTY Skimmer Server are products of Afreet Software, Inc. and are not part of this project.
