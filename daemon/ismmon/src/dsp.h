// ismmon: the one shared numeric type - complex float baseband sample. The channelizer and
// GFSK receiver pass these around. (Detection FFT lives in the reused spectrum engine, not here.)
#pragma once

#include <complex>

namespace ismmon {

typedef std::complex<float> cf;

} // namespace ismmon
