#include "effect.h"

#include "rnnoise.h"

#include <algorithm>
#include <cmath>

namespace wolfe {

namespace {

constexpr double kRate = kSampleRate;
constexpr double kTwoPi = 6.283185307179586;

float factorOf(float db) { return std::pow(10.0f, db / 20.0f); }

// The per-sample coefficient of a one-pole smoother that covers about 63% of
// the way in `ms` milliseconds.
float timeCoefficient(float ms) { return (float)std::exp(-1.0 / (std::max(ms, 0.01f) * 0.001 * kRate)); }

// Values small enough to be denormal cost a lot of time in feedback loops.
inline float quiet(float value) { return std::fabs(value) < 1e-15f ? 0.0f : value; }

// One channel pair of a second-order filter (the "Audio EQ Cookbook" designs),
// transposed direct form II in double precision.
class Biquad {
public:
    enum Shape { LowPass, HighPass, Peak, LowShelf, HighShelf };

    void design(Shape shape, double hz, double q, double gainDb) {
        hz = std::min(std::max(hz, 5.0), kRate * 0.45);
        const double w = kTwoPi * hz / kRate, c = std::cos(w), s = std::sin(w);
        const double a = std::pow(10.0, gainDb / 40.0), sa = std::sqrt(a);
        double alpha = s / (2.0 * q), b0, b1, b2, a0, a1, a2;
        switch (shape) {
            case LowPass:
                b0 = (1 - c) / 2, b1 = 1 - c, b2 = (1 - c) / 2, a0 = 1 + alpha, a1 = -2 * c, a2 = 1 - alpha;
                break;
            case HighPass:
                b0 = (1 + c) / 2, b1 = -(1 + c), b2 = (1 + c) / 2, a0 = 1 + alpha, a1 = -2 * c, a2 = 1 - alpha;
                break;
            case Peak:
                b0 = 1 + alpha * a, b1 = -2 * c, b2 = 1 - alpha * a, a0 = 1 + alpha / a, a1 = -2 * c, a2 = 1 - alpha / a;
                break;
            case LowShelf:
                alpha = s / 2 * std::sqrt(2.0);  // slope 1
                b0 = a * ((a + 1) - (a - 1) * c + 2 * sa * alpha);
                b1 = 2 * a * ((a - 1) - (a + 1) * c);
                b2 = a * ((a + 1) - (a - 1) * c - 2 * sa * alpha);
                a0 = (a + 1) + (a - 1) * c + 2 * sa * alpha;
                a1 = -2 * ((a - 1) + (a + 1) * c);
                a2 = (a + 1) + (a - 1) * c - 2 * sa * alpha;
                break;
            default:  // HighShelf
                alpha = s / 2 * std::sqrt(2.0);
                b0 = a * ((a + 1) + (a - 1) * c + 2 * sa * alpha);
                b1 = -2 * a * ((a - 1) + (a + 1) * c);
                b2 = a * ((a + 1) + (a - 1) * c - 2 * sa * alpha);
                a0 = (a + 1) - (a - 1) * c + 2 * sa * alpha;
                a1 = 2 * ((a - 1) - (a + 1) * c);
                a2 = (a + 1) - (a - 1) * c - 2 * sa * alpha;
                break;
        }
        b0_ = b0 / a0, b1_ = b1 / a0, b2_ = b2 / a0, a1_ = a1 / a0, a2_ = a2 / a0;
    }

    void process(float* stereo) {
        for (int i = 0; i < kQuantum; i++) {
            for (int channel = 0; channel < 2; channel++) {
                const double x = stereo[i * 2 + channel];
                const double y = b0_ * x + z1_[channel];
                z1_[channel] = b1_ * x - a1_ * y + z2_[channel];
                z2_[channel] = b2_ * x - a2_ * y;
                stereo[i * 2 + channel] = (float)y;
            }
        }
    }

private:
    double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    double z1_[2] = {0, 0}, z2_[2] = {0, 0};
};

// A line of past samples, read at any distance, also between two samples.
class DelayLine {
public:
    explicit DelayLine(int frames) : buffer_((size_t)frames * 2, 0.0f), frames_(frames) {}

    void write(float left, float right) {
        buffer_[position_ * 2] = quiet(left);
        buffer_[position_ * 2 + 1] = quiet(right);
        position_ = (position_ + 1) % frames_;
    }
    // `delay` frames before the last one written (1 = the last one).
    float read(int channel, float delay) const {
        delay = std::min(std::max(delay, 1.0f), (float)(frames_ - 2));
        const int whole = (int)delay;
        const float fraction = delay - whole;
        const int a = (position_ - whole + frames_) % frames_, b = (a - 1 + frames_) % frames_;
        return buffer_[a * 2 + channel] * (1.0f - fraction) + buffer_[b * 2 + channel] * fraction;
    }

private:
    std::vector<float> buffer_;
    int frames_;
    int position_ = 0;
};

// Dry and processed sound in proportion: 0 all dry, 1 all processed.
inline float blend(float dry, float wet, float mix) { return dry + (wet - dry) * mix; }

// --- level ------------------------------------------------------------------

class MixEffect : public Effect {
public:
    void process(float*) override {}
};

// A change of gain is spread over one cycle, so that it does not click.
class GainEffect : public Effect {
public:
    void process(float* stereo) override {
        const float target = factorOf(parameter(0));
        const float step = (target - current_) / kQuantum;
        for (int i = 0; i < kQuantum; i++) {
            const float gain = current_ + step * (i + 1);
            stereo[i * 2] *= gain;
            stereo[i * 2 + 1] *= gain;
        }
        current_ = target;
    }

private:
    float current_ = 1.0f;
};

// Balance: the side it moves away from gets quieter, the other stays as it is.
class PanEffect : public Effect {
public:
    void process(float* stereo) override {
        const float pan = parameter(0) / 100.0f;
        const float left = std::min(1.0f, 1.0f - pan), right = std::min(1.0f, 1.0f + pan);
        for (int i = 0; i < kQuantum; i++) {
            const float t = (i + 1.0f) / kQuantum;
            stereo[i * 2] *= left_ + (left - left_) * t;
            stereo[i * 2 + 1] *= right_ + (right - right_) * t;
        }
        left_ = left, right_ = right;
    }

private:
    float left_ = 1.0f, right_ = 1.0f;
};

// Above the threshold, the level rises only 1/ratio as much as it would.
class CompressorEffect : public Effect {
public:
    void process(float* stereo) override {
        const float threshold = parameter(0), ratio = std::max(1.0f, parameter(1));
        const float attack = timeCoefficient(parameter(2)), release = timeCoefficient(parameter(3));
        const float makeup = parameter(4);
        for (int i = 0; i < kQuantum; i++) {
            const float level = std::max(std::fabs(stereo[i * 2]), std::fabs(stereo[i * 2 + 1]));
            const float coefficient = level > envelope_ ? attack : release;
            envelope_ = quiet(level + coefficient * (envelope_ - level));
            const float over = 20.0f * std::log10(envelope_ + 1e-9f) - threshold;
            const float gain = factorOf(makeup - (over > 0.0f ? over * (1.0f - 1.0f / ratio) : 0.0f));
            stereo[i * 2] *= gain;
            stereo[i * 2 + 1] *= gain;
        }
    }

private:
    float envelope_ = 0.0f;
};

// Brings down at once whatever would go over the ceiling, and lets go slowly.
class LimiterEffect : public Effect {
public:
    void process(float* stereo) override {
        const float ceiling = factorOf(parameter(0)), release = timeCoefficient(parameter(1));
        for (int i = 0; i < kQuantum; i++) {
            const float level = std::max(std::fabs(stereo[i * 2]), std::fabs(stereo[i * 2 + 1]));
            envelope_ = level > envelope_ ? level : quiet(level + release * (envelope_ - level));
            if (envelope_ > ceiling) {
                const float gain = ceiling / envelope_;
                stereo[i * 2] *= gain;
                stereo[i * 2 + 1] *= gain;
            }
        }
    }

private:
    float envelope_ = 0.0f;
};

// Silent while the sound stays under the threshold; opens at once, stays open
// a moment after the sound drops, then fades out.
class GateEffect : public Effect {
public:
    void process(float* stereo) override {
        const float threshold = factorOf(parameter(0)), release = timeCoefficient(parameter(1));
        const float follow = timeCoefficient(5.0f), open = timeCoefficient(1.0f);
        const int hold = (int)(0.05 * kRate);
        for (int i = 0; i < kQuantum; i++) {
            const float level = std::max(std::fabs(stereo[i * 2]), std::fabs(stereo[i * 2 + 1]));
            envelope_ = level > envelope_ ? level : quiet(level + follow * (envelope_ - level));
            if (envelope_ > threshold) held_ = hold;
            else if (held_ > 0) held_--;
            const float target = held_ > 0 ? 1.0f : 0.0f;
            gain_ = quiet(target + (target > gain_ ? open : release) * (gain_ - target));
            stereo[i * 2] *= gain_;
            stereo[i * 2 + 1] *= gain_;
        }
    }

private:
    float envelope_ = 0.0f, gain_ = 0.0f;
    int held_ = 0;
};

// --- tone -------------------------------------------------------------------

class FilterEffect : public Effect {
public:
    explicit FilterEffect(Biquad::Shape shape) : shape_(shape) {}

    void process(float* stereo) override {
        const float hz = parameter(0), q = parameter(1);
        if (hz != hz_ || q != q_) {
            hz_ = hz, q_ = q;
            filter_.design(shape_, hz, std::max(0.1f, q), 0.0);
        }
        filter_.process(stereo);
    }

private:
    Biquad::Shape shape_;
    Biquad filter_;
    float hz_ = -1, q_ = -1;
};

// Lows below 150 Hz, mids around 1 kHz, highs above 5 kHz.
class EqualizerEffect : public Effect {
public:
    void process(float* stereo) override {
        static const Biquad::Shape shapes[3] = {Biquad::LowShelf, Biquad::Peak, Biquad::HighShelf};
        static const double frequencies[3] = {150.0, 1000.0, 5000.0};
        for (int band = 0; band < 3; band++) {
            const float db = parameter(band);
            if (db != db_[band]) {
                db_[band] = db;
                bands_[band].design(shapes[band], frequencies[band], 0.7, db);
            }
            if (db != 0.0f) bands_[band].process(stereo);
        }
    }

private:
    Biquad bands_[3];
    float db_[3] = {-1000, -1000, -1000};
};

// --- space and time ---------------------------------------------------------

// Echoes: what comes out of the line goes back into it, a little quieter each time.
class DelayEffect : public Effect {
public:
    DelayEffect() : line_((int)(2.1 * kRate)) {}

    void process(float* stereo) override {
        const float target = parameter(0) * 0.001f * (float)kRate;
        const float feedback = parameter(1) / 100.0f, mix = parameter(2) / 100.0f;
        if (delay_ < 0) delay_ = target;
        // A new time is reached gradually, so that it does not click.
        const float glide = 1.0f - timeCoefficient(50.0f);
        for (int i = 0; i < kQuantum; i++) {
            delay_ += (target - delay_) * glide;
            const float left = line_.read(0, delay_), right = line_.read(1, delay_);
            line_.write(stereo[i * 2] + left * feedback, stereo[i * 2 + 1] + right * feedback);
            stereo[i * 2] = blend(stereo[i * 2], left, mix);
            stereo[i * 2 + 1] = blend(stereo[i * 2 + 1], right, mix);
        }
    }

private:
    DelayLine line_;
    float delay_ = -1.0f;
};

// Freeverb (Jezar at Dreampoint, public domain): eight feedback combs in
// parallel, then four allpasses in series, for each side.
class ReverbEffect : public Effect {
public:
    ReverbEffect() {
        static const int combs[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int allpasses[4] = {556, 441, 341, 225};
        const double scale = kRate / 44100.0;
        for (int side = 0; side < 2; side++) {
            const int spread = side * 23;  // the right side slightly different, for width
            for (int i = 0; i < 8; i++) combs_[side][i].buffer.assign((size_t)((combs[i] + spread) * scale), 0.0f);
            for (int i = 0; i < 4; i++) allpasses_[side][i].buffer.assign((size_t)((allpasses[i] + spread) * scale), 0.0f);
        }
    }

    void process(float* stereo) override {
        const float feedback = parameter(0) / 100.0f * 0.28f + 0.7f;
        const float damping = parameter(1) / 100.0f * 0.4f;
        const float mix = parameter(2) / 100.0f;
        for (int i = 0; i < kQuantum; i++) {
            const float input = (stereo[i * 2] + stereo[i * 2 + 1]) * 0.015f;
            for (int side = 0; side < 2; side++) {
                float wet = 0.0f;
                for (Comb& comb : combs_[side]) wet += comb.process(input, feedback, damping);
                for (Allpass& allpass : allpasses_[side]) wet = allpass.process(wet);
                stereo[i * 2 + side] = blend(stereo[i * 2 + side], wet, mix);
            }
        }
    }

private:
    struct Comb {
        std::vector<float> buffer;
        size_t position = 0;
        float filtered = 0.0f;
        float process(float input, float feedback, float damping) {
            const float output = buffer[position];
            filtered = quiet(output * (1.0f - damping) + filtered * damping);
            buffer[position] = quiet(input + filtered * feedback);
            if (++position == buffer.size()) position = 0;
            return output;
        }
    };
    struct Allpass {
        std::vector<float> buffer;
        size_t position = 0;
        float process(float input) {
            const float stored = buffer[position];
            buffer[position] = quiet(input + stored * 0.5f);
            if (++position == buffer.size()) position = 0;
            return stored - input;
        }
    };
    Comb combs_[2][8];
    Allpass allpasses_[2][4];
};

// A copy of the sound slightly late by an amount that swings slowly, the two
// sides out of step.
class ChorusEffect : public Effect {
public:
    ChorusEffect() : line_((int)(0.05 * kRate)) {}

    void process(float* stereo) override {
        const double step = kTwoPi * parameter(0) / kRate;
        const float depth = parameter(1) * 0.001f * (float)kRate, mix = parameter(2) / 100.0f;
        const float base = 0.012f * (float)kRate;
        for (int i = 0; i < kQuantum; i++) {
            line_.write(stereo[i * 2], stereo[i * 2 + 1]);
            for (int side = 0; side < 2; side++) {
                const float swing = 0.5f + 0.5f * (float)std::sin(phase_ + side * kTwoPi / 4.0);
                const float wet = line_.read(side, base + depth * swing);
                stereo[i * 2 + side] = blend(stereo[i * 2 + side], wet, mix);
            }
            phase_ = std::fmod(phase_ + step, kTwoPi);
        }
    }

private:
    DelayLine line_;
    double phase_ = 0.0;
};

// The volume going down and back up, over and over.
class TremoloEffect : public Effect {
public:
    void process(float* stereo) override {
        const double step = kTwoPi * parameter(0) / kRate;
        const float depth = parameter(1) / 100.0f;
        for (int i = 0; i < kQuantum; i++) {
            const float gain = 1.0f - depth * (0.5f - 0.5f * (float)std::cos(phase_));
            stereo[i * 2] *= gain;
            stereo[i * 2 + 1] *= gain;
            phase_ = std::fmod(phase_ + step, kTwoPi);
        }
    }

private:
    double phase_ = 0.0;
};

// --- character --------------------------------------------------------------

// Pushes the sound into a curve that rounds off its peaks. The louder the
// drive, the more it rounds; the result is turned down part of the way back.
class DistortionEffect : public Effect {
public:
    void process(float* stereo) override {
        const float drive = factorOf(parameter(0)), mix = parameter(1) / 100.0f;
        const float back = 1.0f / std::sqrt(drive);
        for (int i = 0; i < kQuantum * 2; i++) stereo[i] = blend(stereo[i], std::tanh(stereo[i] * drive) * back, mix);
    }
};

// Fewer levels (bits) and fewer samples a second: each sample is held until
// the next one at the lower rate.
class BitcrusherEffect : public Effect {
public:
    void process(float* stereo) override {
        const float steps = std::pow(2.0f, std::max(1.0f, parameter(0)) - 1.0f);
        const double step = std::min(1.0, parameter(1) / kRate);
        for (int i = 0; i < kQuantum; i++) {
            phase_ += step;
            if (phase_ >= 1.0) {
                phase_ -= 1.0;
                for (int side = 0; side < 2; side++) held_[side] = std::round(stereo[i * 2 + side] * steps) / steps;
            }
            stereo[i * 2] = held_[0];
            stereo[i * 2 + 1] = held_[1];
        }
    }

private:
    double phase_ = 1.0;
    float held_[2] = {0, 0};
};

// RNNoise (Jean-Marc Valin, Xiph.Org), the noise suppression Mumble uses: a
// small neural network that tells voice from noise, band by band, and keeps
// the voice. It works on frames of exactly kQuantum samples, scaled like
// 16-bit audio, and hands each one back one frame late.
//
// The two sides are cleaned separately; when they are the same (a microphone
// is nearly always mono) the work is done once. Below 100%, the original is
// mixed back in one frame late too, so that the two line up.
class RnnoiseEffect : public Effect {
public:
    RnnoiseEffect() : states_{rnnoise_create(nullptr), rnnoise_create(nullptr)} {}
    ~RnnoiseEffect() override {
        for (DenoiseState* state : states_) rnnoise_destroy(state);
    }

    void process(float* stereo) override {
        const float amount = parameter(0) / 100.0f;
        bool same = true;
        for (int i = 0; i < kQuantum && same; i++) same = stereo[i * 2] == stereo[i * 2 + 1];

        float clean[2][kQuantum];
        for (int side = 0; side < (same ? 1 : 2); side++) {
            float frame[kQuantum];
            for (int i = 0; i < kQuantum; i++) frame[i] = stereo[i * 2 + side] * 32768.0f;
            rnnoise_process_frame(states_[side], clean[side], frame);
        }
        if (same) std::copy(clean[0], clean[0] + kQuantum, clean[1]);

        for (int i = 0; i < kQuantum; i++) {
            for (int side = 0; side < 2; side++) {
                const float original = stereo[i * 2 + side];
                stereo[i * 2 + side] = blend(previous_[i * 2 + side], clean[side][i] / 32768.0f, amount);
                previous_[i * 2 + side] = original;
            }
        }
    }

private:
    DenoiseState* states_[2];
    float previous_[kQuantum * 2] = {};  // the last frame as it came in
};

}  // namespace

const std::vector<EffectType>& effectTypes() {
    // Linear settings say how much a pixel of dragging changes them; the
    // editor drags logarithmic ones across their whole range in a few
    // hundred pixels.
    static const std::vector<EffectType> types = {
        {"gain", "Gain", "louder or quieter", "volume level louder quieter", true,
         {{"gain_db", "%+.1f dB", -100, 40, 0, false, 0.1f}}},
        {"pan", "Pan", "more to the left or to the right", "balance stereo left right", true,
         {{"pan", "Pan %+.0f", -100, 100, 0, false, 0.5f}}},
        {"compressor", "Compressor", "evens out loud and quiet", "dynamics", true,
         {{"threshold_db", "Threshold %.0f dB", -60, 0, -20, false, 0.2f},
          {"ratio", "Ratio %.1f:1", 1, 20, 4, true, 0},
          {"attack_ms", "Attack %.1f ms", 0.1f, 100, 5, true, 0},
          {"release_ms", "Release %.0f ms", 10, 2000, 150, true, 0},
          {"makeup_db", "Makeup %+.1f dB", 0, 30, 0, false, 0.1f}}},
        {"limiter", "Limiter", "never louder than its ceiling", "clip protect peak", true,
         {{"ceiling_db", "Ceiling %.1f dB", -30, 0, -1, false, 0.1f},
          {"release_ms", "Release %.0f ms", 5, 1000, 50, true, 0}}},
        {"gate", "Noise gate", "silent below a level", "noise gate expander mute", true,
         {{"threshold_db", "Threshold %.0f dB", -90, 0, -50, false, 0.2f},
          {"release_ms", "Release %.0f ms", 10, 2000, 200, true, 0}}},
        {"rnnoise", "Noise suppression", "keeps the voice, removes the rest (RNNoise)",
         "noise suppression denoise rnnoise background voice", true,
         {{"amount", "Amount %.0f%%", 0, 100, 100, false, 0.3f}}},
        {"highpass", "High-pass", "cuts the lows: rumble, hum", "highpass hpf filter low cut bass", true,
         {{"cutoff_hz", "Cutoff %.0f Hz", 20, 5000, 100, true, 0},
          {"q", "Resonance %.2f", 0.3f, 10, 0.71f, true, 0}}},
        {"lowpass", "Low-pass", "cuts the highs: hiss, harshness", "lowpass lpf filter high cut treble", true,
         {{"cutoff_hz", "Cutoff %.0f Hz", 200, 20000, 8000, true, 0},
          {"q", "Resonance %.2f", 0.3f, 10, 0.71f, true, 0}}},
        {"equalizer", "Equalizer", "lows, mids and highs", "eq bass treble tone", true,
         {{"low_db", "Low %+.1f dB", -24, 24, 0, false, 0.1f},
          {"mid_db", "Mid %+.1f dB", -24, 24, 0, false, 0.1f},
          {"high_db", "High %+.1f dB", -24, 24, 0, false, 0.1f}}},
        {"delay", "Delay", "echoes", "echo repeat", true,
         {{"time_ms", "Time %.0f ms", 1, 2000, 300, true, 0},
          {"feedback", "Feedback %.0f%%", 0, 95, 35, false, 0.3f},
          {"mix", "Mix %.0f%%", 0, 100, 30, false, 0.3f}}},
        {"reverb", "Reverb", "a room around the sound", "room hall space", true,
         {{"size", "Size %.0f%%", 0, 100, 60, false, 0.3f},
          {"damping", "Damping %.0f%%", 0, 100, 50, false, 0.3f},
          {"mix", "Mix %.0f%%", 0, 100, 25, false, 0.3f}}},
        {"chorus", "Chorus", "thicker, shimmering", "modulation ensemble", true,
         {{"rate_hz", "Rate %.2f Hz", 0.05f, 5, 0.8f, true, 0},
          {"depth_ms", "Depth %.1f ms", 0, 10, 3, false, 0.03f},
          {"mix", "Mix %.0f%%", 0, 100, 50, false, 0.3f}}},
        {"tremolo", "Tremolo", "volume that pulses", "modulation pulse", true,
         {{"rate_hz", "Rate %.1f Hz", 0.1f, 20, 5, true, 0},
          {"depth", "Depth %.0f%%", 0, 100, 50, false, 0.3f}}},
        {"distortion", "Distortion", "drive, saturation", "overdrive fuzz saturation", true,
         {{"drive_db", "Drive %.0f dB", 0, 40, 15, false, 0.1f},
          {"mix", "Mix %.0f%%", 0, 100, 100, false, 0.3f}}},
        {"bitcrusher", "Bitcrusher", "lo-fi: fewer bits, fewer samples", "lofi bit crush downsample", true,
         {{"bits", "Bits %.0f", 1, 16, 8, false, 0.03f},
          {"rate_hz", "Rate %.0f Hz", 500, 48000, 8000, true, 0}}},
        {"mix", "Mix", "a junction", "", false, {}},
    };
    return types;
}

const EffectType* findEffectType(const std::string& type) {
    for (const EffectType& candidate : effectTypes()) {
        if (type == candidate.type) return &candidate;
    }
    return nullptr;
}

std::shared_ptr<Effect> createEffect(const std::string& type) {
    std::shared_ptr<Effect> effect;
    if (type == "gain") effect = std::make_shared<GainEffect>();
    else if (type == "pan") effect = std::make_shared<PanEffect>();
    else if (type == "compressor") effect = std::make_shared<CompressorEffect>();
    else if (type == "limiter") effect = std::make_shared<LimiterEffect>();
    else if (type == "gate") effect = std::make_shared<GateEffect>();
    else if (type == "highpass") effect = std::make_shared<FilterEffect>(Biquad::HighPass);
    else if (type == "lowpass") effect = std::make_shared<FilterEffect>(Biquad::LowPass);
    else if (type == "equalizer") effect = std::make_shared<EqualizerEffect>();
    else if (type == "delay") effect = std::make_shared<DelayEffect>();
    else if (type == "reverb") effect = std::make_shared<ReverbEffect>();
    else if (type == "chorus") effect = std::make_shared<ChorusEffect>();
    else if (type == "tremolo") effect = std::make_shared<TremoloEffect>();
    else if (type == "distortion") effect = std::make_shared<DistortionEffect>();
    else if (type == "bitcrusher") effect = std::make_shared<BitcrusherEffect>();
    else if (type == "mix") effect = std::make_shared<MixEffect>();
    else if (type == "rnnoise") effect = std::make_shared<RnnoiseEffect>();
    else return nullptr;
    const EffectType* description = findEffectType(type);
    for (size_t i = 0; i < description->parameters.size(); i++) effect->setParameter((int)i, description->parameters[i].initial);
    return effect;
}

}  // namespace wolfe
