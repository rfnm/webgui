// lrx_probe: the stack acceptance gate. A plain librfnm LOCAL-transport RX client must
// receive buffers; if this fails, the stack is broken for every client regardless of
// what spectrumd shows. Exit 0 iff >0 buffers arrive within 8 s.
#include <librfnm/device.h>
#include <librfnm/rx_stream.h>
#include <cstdio>
#include <vector>
#include <librfnm/constants.h>
int main() {
	try {
		rfnm::device dev(rfnm::TRANSPORT_LOCAL);
		size_t bsz; uint8_t bpe;
		dev.set_stream_format(rfnm::STREAM_FORMAT_CS16, &bsz, &bpe);
		if (auto r = dev.set_samp_rate(61440000)) { printf("lrx_probe: samp_rate fail %d\n", r); return 2; }
		auto ch = dev.rx_ch_helper(0);
		// reset first: a previous client (or crash) may have left the channel RF_ON,
		// and rx_stream::start() refuses busy channels. Then configure only -
		// the stream wrapper owns the enable + apply.
		dev.set_rx_channel_status(ch.id, RFNM_CH_RF_OFF, RFNM_CH_STREAM_OFF, false);
		dev.apply(ch.apply, true, 1000000);
		dev.set_rx_channel_freq(ch.id, RFNM_MHZ_TO_HZ(2450));
		dev.set_rx_channel_gain(ch.id, 30);
		rfnm::rx_stream s(dev, 0x1);
		if (auto r = s.start()) { printf("lrx_probe: stream start fail %d\n", r); return 2; }
		std::vector<uint8_t> buf(bsz * 64);
		void *ptrs[1] = { buf.data() };
		size_t got = 0, total = 0; uint64_t t0 = 0; double dt;
		int rounds = 0, ok = 0;
		for (rounds = 0; rounds < 16; rounds++) {
			auto r = s.read(ptrs, bsz * 16 / 4, got, t0, 500000);
			if (r == RFNM_API_OK && got > 0) { total += got; ok++; }
		}
		printf("lrx_probe: %d/%d reads ok, %zu samples\n", ok, rounds, total);
		s.stop();
		return total > 0 ? 0 : 1;
	} catch (const std::exception &e) { printf("lrx_probe: EXC %s\n", e.what()); return 2; }
}
