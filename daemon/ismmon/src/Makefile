# ismmon DSP module.
# Offline selftests (no hardware):
#   host x86 scalar:  make test
#   aarch64 (qemu):   make test CXX=aarch64-linux-gnu-g++ ARCHFLAGS="-mcpu=cortex-a53 -static"
#                     then run each ismmon_* under qemu-aarch64-static
# (-fcx-limited-range removes the libgcc __mulsc3 call on every complex multiply.)
# NOTE: gfsk_rx.cpp references ble_coded.cpp (LE Coded PHY), so every BLE target links ble_coded.cpp;
# the ismmond board build must add ismmon/ble_coded.cpp too.
CXX       ?= g++
CXXFLAGS  ?= -O3 -std=c++17 -Wall -Wextra -fcx-limited-range
ARCHFLAGS ?=
LDFLAGS   ?= -lm

SELFTEST_SRC = ble.cpp ble_coded.cpp gfsk_rx.cpp selftest.cpp
WB_SRC = ble.cpp ble_coded.cpp gfsk_rx.cpp wb_selftest.cpp
REACH_SRC = ble.cpp ble_coded.cpp gfsk_rx.cpp selftest_reach.cpp
WIFI_SRC = wifi.cpp wifi_selftest.cpp
DSSS_SRC = wifi.cpp wifi_dsss.cpp dsss_selftest.cpp
OFDM_SRC = wifi.cpp wifi_ofdm_selftest.cpp
DETCHECK_SRC = detector_check.cpp
HDRS = ble.h ble_coded.h gfsk_rx.h channelizer.h detector.h wifi.h wifi_dsss.h wifi_ht.h dsp.h

selftest: $(SELFTEST_SRC) $(HDRS)
	$(CXX) $(CXXFLAGS) $(ARCHFLAGS) $(SELFTEST_SRC) -o ismmon_selftest $(LDFLAGS)

wb: $(WB_SRC) $(HDRS)
	$(CXX) $(CXXFLAGS) $(ARCHFLAGS) $(WB_SRC) -o ismmon_wb $(LDFLAGS)

reach: $(REACH_SRC) $(HDRS)
	$(CXX) $(CXXFLAGS) $(ARCHFLAGS) $(REACH_SRC) -o ismmon_reach $(LDFLAGS)

wifi: $(WIFI_SRC) $(HDRS)
	$(CXX) $(CXXFLAGS) $(ARCHFLAGS) $(WIFI_SRC) -o ismmon_wifi $(LDFLAGS)

dsss: $(DSSS_SRC) $(HDRS)
	$(CXX) $(CXXFLAGS) $(ARCHFLAGS) $(DSSS_SRC) -o ismmon_dsss $(LDFLAGS)

ofdm: $(OFDM_SRC) $(HDRS)
	$(CXX) $(CXXFLAGS) $(ARCHFLAGS) $(OFDM_SRC) -o ismmon_ofdm $(LDFLAGS)

detcheck: $(DETCHECK_SRC) detector.h
	$(CXX) $(CXXFLAGS) $(ARCHFLAGS) $(DETCHECK_SRC) -o ismmon_detcheck $(LDFLAGS)

test: selftest wb reach wifi dsss ofdm detcheck
	./ismmon_selftest && ./ismmon_wb && ./ismmon_reach && ./ismmon_wifi && ./ismmon_dsss && ./ismmon_ofdm && ./ismmon_detcheck

clean:
	rm -f ismmon_selftest ismmon_wb ismmon_reach ismmon_wifi ismmon_dsss ismmon_ofdm ismmon_detcheck *.o

.PHONY: selftest wb reach wifi dsss ofdm detcheck test clean
