#!/bin/sh
# Builds the 32-bit interface DLLs and their settings programs into bin/, each as a
# single file with the radio driver and libusb linked in:
#   AirspyHfIntf.dll + AirspyHfIntfConfig.exe   (Airspy HF+ / HF+ Discovery)
#   RtlSdrIntf.dll   + RtlSdrIntfConfig.exe     (RTL-SDR, including Blog V3 and V4)
# Needs: i686-w64-mingw32-gcc, git, curl and 7z (or python3 with py7zr).
set -e
AIRSPYHF_COMMIT=24fe8ffcb00b14f827268bbad89ae1392de055e5
RTLSDR_COMMIT=aed0ea19f3a273370a13c9009b96313c75d54c7b    # RTL-SDR Blog fork (V4 support)
LIBUSB_VER=1.0.27
TP=third_party
mkdir -p bin

if [ ! -d $TP/airspyhf ]; then
    git clone https://github.com/airspy/airspyhf $TP/airspyhf
    git -c safe.directory='*' -C $TP/airspyhf checkout -q $AIRSPYHF_COMMIT
fi
if [ ! -d $TP/rtl-sdr-blog ]; then
    git clone https://github.com/rtlsdrblog/rtl-sdr-blog $TP/rtl-sdr-blog
    git -c safe.directory='*' -C $TP/rtl-sdr-blog checkout -q $RTLSDR_COMMIT
fi
if [ ! -f $TP/libusb/MinGW32/static/libusb-1.0.a ]; then
    mkdir -p $TP/libusb
    curl -sSL -o $TP/libusb.7z https://github.com/libusb/libusb/releases/download/v$LIBUSB_VER/libusb-$LIBUSB_VER.7z
    7z x -y -o$TP/libusb $TP/libusb.7z >/dev/null 2>&1 || python3 -m py7zr x $TP/libusb.7z $TP/libusb
fi

SRC=$TP/airspyhf/libairspyhf/src
LIBS="$SRC/airspyhf.c $SRC/iqbalancer.c $TP/libusb/MinGW32/static/libusb-1.0.a -lpthread"
INC="-DSTATIC_AIRSPYHFPLUS -I$TP/libusb/include -I$SRC"

DLG="-Iconfig -Icommon config/settings_dialog.c common/level_meter.c -lcomctl32 -lgdi32"

i686-w64-mingw32-windres -I config config/settings.rc -O coff -o $TP/dll_res.o
i686-w64-mingw32-gcc -std=gnu17 -O2 -s -msse2 -shared -static -o bin/AirspyHfIntf.dll \
    -DSTATIC_AIRSPYHF $INC AirspyHfIntf.c $TP/dll_res.o $LIBS $DLG \
    -Wl,--kill-at -Wl,--exclude-all-symbols
echo "bin/AirspyHfIntf.dll built"

i686-w64-mingw32-windres -DWITH_MANIFEST -I config config/settings.rc -O coff -o $TP/exe_res.o
i686-w64-mingw32-gcc -std=gnu17 -O2 -s -mwindows -static -o bin/AirspyHfIntfConfig.exe \
    $INC config/AirspyHfIntfConfig.c $TP/exe_res.o $LIBS $DLG -lshell32
echo "bin/AirspyHfIntfConfig.exe built"

# ---- RTL-SDR ----
RSRC=$TP/rtl-sdr-blog/src
RINC="-Drtlsdr_STATIC -I$TP/rtl-sdr-blog/include -I$TP/libusb/include"
RLIBS="$RSRC/librtlsdr.c $RSRC/tuner_e4k.c $RSRC/tuner_fc0012.c $RSRC/tuner_fc0013.c $RSRC/tuner_fc2580.c $RSRC/tuner_r82xx.c $TP/libusb/MinGW32/static/libusb-1.0.a"
RDLG="-Irtlsdr -Icommon rtlsdr/rtl_settings_dialog.c common/level_meter.c -lcomctl32 -lgdi32"

i686-w64-mingw32-windres -I rtlsdr rtlsdr/settings.rc -O coff -o $TP/rtl_dll_res.o
i686-w64-mingw32-gcc -std=gnu17 -O2 -s -msse2 -shared -static -o bin/RtlSdrIntf.dll \
    $RINC RtlSdrIntf.c $TP/rtl_dll_res.o $RLIBS $RDLG \
    -Wl,--kill-at -Wl,--exclude-all-symbols
echo "bin/RtlSdrIntf.dll built"

i686-w64-mingw32-windres -DWITH_MANIFEST -I rtlsdr rtlsdr/settings.rc -O coff -o $TP/rtl_exe_res.o
i686-w64-mingw32-gcc -std=gnu17 -O2 -s -mwindows -static -o bin/RtlSdrIntfConfig.exe \
    $RINC rtlsdr/RtlSdrIntfConfig.c $TP/rtl_exe_res.o $RLIBS $RDLG -lshell32
echo "bin/RtlSdrIntfConfig.exe built"

# default settings files next to the binaries
cp AirspyHfIntf.ini RtlSdrIntf.ini bin/
