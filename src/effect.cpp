#include "effect.h"

#include "rnnoise.h"

namespace wolfe {

namespace {

class MixEffect : public Effect {
public:
    void process(float*) override {}
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
    if (type == "mix") return std::make_shared<MixEffect>();
    if (type == "rnnoise") return std::make_shared<RnnoiseEffect>();
    return nullptr;
}

}  // namespace wolfe
