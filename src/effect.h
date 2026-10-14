#pragma once

#include "common.h"

#include <memory>
#include <string>

namespace wolfe {

// A node in the middle of the graph: it receives the sum of its inputs and
// changes it. Each output that the node feeds gets its own instance, because
// every output runs on its own thread and clock.
class Effect {
public:
    virtual ~Effect() = default;

    // One quantum of interleaved stereo (kQuantum frames), processed in place.
    virtual void process(float* stereo) = 0;
};

// Known types:
//   "mix"      passes the sum of its inputs through unchanged (a junction)
//   "rnnoise"  noise suppression for voice; folds its input to mono
// Returns nullptr for anything else.
std::shared_ptr<Effect> createEffect(const std::string& type);

}  // namespace wolfe
