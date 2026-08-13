# webgui on-board daemons — cross-compiled for i.MX8MP (aarch64/Cortex-A53), NEVER on-board.
#
#   make            all three daemons (spectrumd, ismmond, appd)
#   make check      arch sanity on every produced binary
#   RFNM_SYSROOT=<board rootfs or pulled sysroot> make
#
# spectrumd/ismmond link librfnm's build-imx8mp cross build (spectrumd STATIC -> rebuild on
# any librfnm change; deploy-together rule). appd is dependency-free static C++.

# Local overrides (untracked): e.g. RFNM_SYSROOT for the cross builds.
-include Makefile.local

CROSS   ?= aarch64-linux-gnu-
SYSROOT ?= $(RFNM_SYSROOT)
ifeq ($(SYSROOT),)
$(error set RFNM_SYSROOT to the board sysroot (e.g. the SDK's sysroot/current))
endif
LIBRFNM ?= $(abspath $(CURDIR)/../../librfnm)

CXX_X    = $(CROSS)g++ --sysroot=$(SYSROOT) -isystem $(SYSROOT)/usr/local/include
CC_X     = $(CROSS)gcc --sysroot=$(SYSROOT)
ARCH     = -mcpu=cortex-a53
LD_X     = -L$(SYSROOT)/usr/local/lib -L$(SYSROOT)/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,$(SYSROOT)/usr/lib/aarch64-linux-gnu:$(SYSROOT)/lib/aarch64-linux-gnu:$(SYSROOT)/usr/local/lib

ISM_SRCS = ismmon/src/ble.cpp ismmon/src/ble_coded.cpp ismmon/src/gfsk_rx.cpp ismmon/src/wifi.cpp \
	ismmon/src/wifi_dsss.cpp ismmon/src/capture_rfnm.cpp ismmon/src/httpd.cpp ismmon/src/ismmond.cpp

all: spectrumd-imx8mp ismmond-imx8mp appd-imx8mp lrx_probe-imx8mp

build-imx8mp:
	mkdir -p build-imx8mp

build-imx8mp/pffft.o: third_party/pffft.c | build-imx8mp
	$(CC_X) -O3 $(ARCH) -c $< -o $@

# rfnm_spectrum_protocol.h lives at the webgui repo root — hence -I..
spectrumd-imx8mp: spectrumd.cpp pacer.h fft_engine.h sigmf.h sweep_comp.h build-imx8mp/pffft.o
	$(CXX_X) -O3 -std=c++17 $(ARCH) -DCL_TARGET_OPENCL_VERSION=120 -I. -I.. -Ithird_party \
		-I$(LIBRFNM)/include spectrumd.cpp build-imx8mp/pffft.o $(LIBRFNM)/build-imx8mp/librfnm.a \
		$(LD_X) -lOpenCL -lusb-1.0 -lspdlog -lfmt -lz -lpthread -lm -o $@

# WITHOUT -DWITH_RFNM=1 it builds fine and refuses at runtime — silent-gate trap
ismmond-imx8mp: $(ISM_SRCS)
	$(CXX_X) -O3 -std=c++17 -Wall -Wextra -fcx-limited-range $(ARCH) -DWITH_RFNM=1 -I. \
		-I$(LIBRFNM)/include $(ISM_SRCS) -o $@ \
		-L$(LIBRFNM)/build-imx8mp $(LD_X) -lrfnm -lOpenCL -lm -pthread

# lrx_probe: the local-transport stack acceptance gate (spectrumd's sibling; canonical
# home HERE since 2026-07-10 - the board /rfnm/spectrum/src copies are provenance mirrors).
# Static librfnm like spectrumd: rebuild on any librfnm change (deploy-together).
lrx_probe-imx8mp: lrx_probe.cpp
	$(CXX_X) -O2 -std=c++17 $(ARCH) -I$(LIBRFNM)/include lrx_probe.cpp \
		$(LIBRFNM)/build-imx8mp/librfnm.a $(LD_X) -lusb-1.0 -lspdlog -lfmt -lpthread -lm -o $@

appd-imx8mp: appd.cpp
	$(CROSS)g++ -O2 -static -std=c++17 -Wall -Wextra $(ARCH) -pthread appd.cpp -o $@

# host-side (x86) unit test for the sweep composition/flatness math in sweep_comp.h:
# a synthetic per-hop passband dome must compose flat, signals must survive
build-host/sweep_eq_test: tests/sweep_eq_test.cpp sweep_comp.h third_party/pffft.c
	mkdir -p build-host
	g++ -O2 -std=c++17 -Wall -Wextra -I. -Ithird_party tests/sweep_eq_test.cpp third_party/pffft.c -lm -o $@

check: all build-host/sweep_eq_test
	./build-host/sweep_eq_test
	@for b in spectrumd-imx8mp ismmond-imx8mp appd-imx8mp lrx_probe-imx8mp; do \
		file $$b | grep -q 'ARM aarch64' && echo "OK  $$b" || { echo "BAD ARCH $$b"; exit 1; }; \
	done

clean:
	rm -rf build-imx8mp build-host spectrumd-imx8mp ismmond-imx8mp appd-imx8mp lrx_probe-imx8mp

.PHONY: all check clean
