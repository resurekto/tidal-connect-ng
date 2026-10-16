#include "media/output/VolumeCurve.hpp"

#include <algorithm>
#include <cmath>

namespace tcng::media {

namespace {
double clamp01(double v) {
    return std::clamp(v, 0.0, 1.0);
}
}  // namespace

// alsamixer: min_norm = exp10((min - max) / 6000); v = v * (1 - min_norm) + min_norm;
// value = 6000 * log10(v) + max (in 1/100 dB).
double sliderToDb(double slider, double minDb, double maxDb) {
    const double minNorm = std::pow(10.0, (minDb - maxDb) / 60.0);
    const double v = clamp01(slider) * (1.0 - minNorm) + minNorm;
    return std::max(minDb, 60.0 * std::log10(v) + maxDb);
}

double dbToSlider(double db, double minDb, double maxDb) {
    if (maxDb <= minDb)
        return 1.0;
    const double minNorm = std::pow(10.0, (minDb - maxDb) / 60.0);
    const double v = std::pow(10.0, (std::clamp(db, minDb, maxDb) - maxDb) / 60.0);
    return clamp01((v - minNorm) / (1.0 - minNorm));
}

double sliderToGain(double slider) {
    if (slider <= 0.0)
        return 0.0;
    return std::pow(10.0, sliderToDb(slider, -kSoftwareRangeDb, 0.0) / 20.0);
}

}  // namespace tcng::media
