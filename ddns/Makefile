# ddnsd — RFNM dynamic-DNS client daemon (aarch64 static, same cross setup as the vsg repo).
# make          build build/ddnsd
# make test     run --selftest under qemu-aarch64
CROSS ?= aarch64-linux-gnu-
CXXFLAGS ?= -O2 -Wall -Wextra

build/ddnsd: ddnsd.cpp
	@mkdir -p build
	$(CROSS)g++ $(CXXFLAGS) -static -pthread ddnsd.cpp -Wl,--whole-archive -lpthread -Wl,--no-whole-archive -o build/ddnsd

test: build/ddnsd
	qemu-aarch64 ./build/ddnsd --selftest

clean:
	rm -rf build
