# rfnm-calib-fetch — factory-calibration distributor (aarch64 static, ddnsd's cross setup).
# make          build build/rfnm-calib-fetch (board binary)
# make host     build build/rfnm-calib-fetch-host (x86, for live end-to-end tests on the host)
# make test     run --selftest under qemu-aarch64
CROSS ?= aarch64-linux-gnu-
CXXFLAGS ?= -O2 -Wall -Wextra

build/rfnm-calib-fetch: calibfetch.cpp
	@mkdir -p build
	$(CROSS)g++ $(CXXFLAGS) -static calibfetch.cpp -o build/rfnm-calib-fetch

host: calibfetch.cpp
	@mkdir -p build
	g++ $(CXXFLAGS) calibfetch.cpp -o build/rfnm-calib-fetch-host

test: build/rfnm-calib-fetch
	qemu-aarch64 ./build/rfnm-calib-fetch --selftest

clean:
	rm -rf build
