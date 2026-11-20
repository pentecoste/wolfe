#include "effect.h"

#include "rnnoise.h"

#include <atomic>

namespace wolfe {

namespace {

class MixEffect : public Effect {
public:
    void process(float*) override {}
};

// A change of gain is spread over one cycle, so that it does not click.
class GainEffect : public Effect {
public:
    void process(float* stereo) override {
        const float target = target_.load(std::memory_order_relaxed);
        if (target == current_) {
            if (target != 1.0f) {
                for (int i = 0; i < kQuantum * 2; i++) stereo[i] *= target;
            }
            return;
        }
        const float step = (target - current_) / kQuantum;
        for (int i = 0; i < kQuantum; i++) {
            const float gain = current_ + step * (i + 1);
            stereo[i * 2] *= gain;
            stereo[i * 2 + 1] *= gain;
        }
        current_ = target;
    }
    void setGain(float gain) override { target_.store(gain, std::memory_order_relaxed); }

private:
    std::atomic<float> target_{1.0f};
    float current_ = 1.0f;  // used by the audio thread only
};

class RnnoiseEffect : public Effect {
public:
    RnnoiseEffect() : state_(rnnoise_create(nullptr)) {}
    ~RnnoiseEffect() override { rnnoise_destroy(state_); }

    void process(float* stereo) override {
        // RNNoise works on mono frames of exactly kQuantum samples, scaled like
        // 16-bit audio.
        float mono[kQuantum];
        for (int i = 0; i < kQuantum; i++) mono[i] = (stereo[i * 2] + stereo[i * 2 + 1]) * 0.5f * 32768.0f;
        rnnoise_process_frame(state_, mono, mono);
        for (int i = 0; i < kQuantum; i++) stereo[i * 2] = stereo[i * 2 + 1] = mono[i] / 32768.0f;
    }

private:
    DenoiseState* state_;
};

}  // namespace

std::shared_ptr<Effect> createEffect(const std::string& type) {
    if (type == "gain") return std::make_shared<GainEffect>();
    if (type == "mix") return std::make_shared<MixEffect>();
    if (type == "rnnoise") return std::make_shared<RnnoiseEffect>();
    return nullptr;
}

}  // namespace wolfe
