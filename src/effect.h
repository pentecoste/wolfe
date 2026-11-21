#pragma once

#include "common.h"

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace wolfe {

// One setting of an effect, as the editor shows it and the file keeps it.
struct EffectParameter {
    const char* key;     // name in the file ("cutoff_hz")
    const char* format;  // how the editor shows it, label included ("Cutoff %.0f Hz")
    float min, max, initial;
    bool logarithmic;    // dragged on a logarithmic scale (frequencies, times)
    float dragSpeed;     // how much one pixel of dragging changes it
    const char* atMin = nullptr;  // shown in place of the value at the minimum, where it means "none" ("silent")
};

// A kind of effect.
struct EffectType {
    const char* type;   // name in the file ("lowpass")
    const char* name;   // name in the editor ("Low-pass")
    const char* hint;   // a few words about what it does
    const char* words;  // more words a search finds it by
    bool offered;       // the editor offers it (the others are for the console program)
    std::vector<EffectParameter> parameters;
    // What it is for and what each setting does, a few short lines for the
    // box's "?"; a good place to start, if there is one.
    const char* help = "";
};

// Every kind of effect, those the editor offers in the order it lists them.
const std::vector<EffectType>& effectTypes();
const EffectType* findEffectType(const std::string& type);

// A node in the middle of the graph: it receives the sum of its inputs and
// changes it. There is one instance per node, run once per graph cycle
// whatever the number of outputs it feeds.
class Effect {
public:
    static constexpr int kMaxParameters = 8;

    virtual ~Effect() = default;

    // One quantum of interleaved stereo (kQuantum frames), processed in place.
    virtual void process(float* stereo) = 0;

    // A setting, by its index in the type's parameters. May be called from
    // any thread while audio runs; the effect picks it up at its next cycle.
    void setParameter(int index, float value) {
        if (index >= 0 && index < kMaxParameters) values_[index].store(value, std::memory_order_relaxed);
    }

protected:
    float parameter(int index) const { return values_[index].load(std::memory_order_relaxed); }

private:
    std::array<std::atomic<float>, kMaxParameters> values_{};
};

// An effect with every parameter at its initial value; nullptr for an unknown type.
std::shared_ptr<Effect> createEffect(const std::string& type);

}  // namespace wolfe
