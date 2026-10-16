#pragma once
// TCNG: the app's volume slider (0..1) <-> mixer values, perceptually, as alsamixer does
// (alsa-utils alsamixer/volume_mapping.c): with a dB range wider than kMaxLinearDbScale the
// slider maps to dB with a cubic curve (slider^3 ~ amplitude); otherwise linearly to the raw
// range. A linear mapping onto the raw range would crowd the audible range into the
// bottom of the slider.

namespace tcng::media {

constexpr double kMaxLinearDbScale = 24.0;  // dB
// Floor of the software curve: the slider's lowest non-zero step is about -60 dB.
constexpr double kSoftwareRangeDb = 60.0;

// Slider -> dB within [minDb, maxDb] (cubic, alsamixer's set_normalized_volume). 0 -> minDb.
double sliderToDb(double slider, double minDb, double maxDb);
// dB -> slider, the inverse (alsamixer's get_normalized_volume).
double dbToSlider(double db, double minDb, double maxDb);
// Linear amplitude for software gain: 0 -> 0 (silence), 1 -> 1, cubic over kSoftwareRangeDb.
double sliderToGain(double slider);

}  // namespace tcng::media
