#pragma once

#include "common.h"

#include <memory>
#include <string>

namespace wolfe {

// A node in the middle of the graph: it receives the sum of its inputs and
// changes it. There is one instance per node, run once per graph cycle
// whatever the number of outputs it feeds.
class Effect {
public:
    virtual ~Effect() = default;

    // One quantum of interleaved stereo (kQuantum frames), processed in place.
    virtual void process(float* stereo) = 0;

    // Effects with a gain: the new one, as a factor. May be called from any
    // thread while audio runs. Others ignore it.
    virtual void setGain(float) {}
};

// Known types:
//   "gain"     passes the sum of its inputs through, louder or quieter (see setGain)
//   "mix"      passes the sum of its inputs through unchanged (a junction)
//   "rnnoise"  noise suppression for voice; folds its input to mono
// Returns nullptr for anything else.
std::shared_ptr<Effect> createEffect(const std::string& type);

}  // namespace wolfe
