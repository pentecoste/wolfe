#include "source_reader.h"

#include <cmath>
#include <cstring>

namespace wolfe {

namespace {

// --- interpolation filter ---------------------------------------------------
// Reading "between" two samples is done with a windowed-sinc filter: kTaps
// neighbouring samples are weighted according to how far the wanted position
// is from each. The weights are precomputed for kPhases fractional positions
// and blended linearly for positions in between.
constexpr int kTaps = 32;
constexpr int kPhases = 256;
constexpr double kKaiserBeta = 9.0;

double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 40; k++) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

struct FilterTable {
    float weights[kPhases + 1][kTaps];

    FilterTable() {
        const double pi = 3.14159265358979323846;
        const double half = kTaps / 2.0;
        for (int phase = 0; phase <= kPhases; phase++) {
            double fraction = (double)phase / kPhases;
            double row[kTaps];
            double sum = 0.0;
            for (int tap = 0; tap < kTaps; tap++) {
                // Distance from the wanted position to the sample under this tap.
                double t = tap - (half - 1) - fraction;
                double sinc = t == 0.0 ? 1.0 : std::sin(pi * t) / (pi * t);
                double window = std::fabs(t) >= half
                                    ? 0.0
                                    : besselI0(kKaiserBeta * std::sqrt(1.0 - (t / half) * (t / half))) /
                                          besselI0(kKaiserBeta);
                row[tap] = sinc * window;
                sum += row[tap];
            }
            // Unity gain at every phase, so a steady signal does not ripple.
            for (int tap = 0; tap < kTaps; tap++) weights[phase][tap] = (float)(row[tap] / sum);
        }
    }
};

const FilterTable& filterTable() {
    static const FilterTable table;
    return table;
}

// --- delay control ----------------------------------------------------------
// A PI controller: the ratio follows the age error directly (kProportional)
// and also accumulates it slowly (kIntegral) to learn the steady clock
// difference. Tuned to settle in about 15 seconds without audible pitch changes.
constexpr double kProportional = 0.5;    // per second
constexpr double kIntegral = 0.0625;     // per second squared
constexpr double kMaxDeviation = 0.005;  // ratio stays within 1 +/- 0.5 %
constexpr double kJumpSeconds = 0.015;   // beyond this error, jump instead of gliding

// --- target adaptation ------------------------------------------------------
constexpr double kSafetySeconds = 0.002;  // room wanted beyond what was needed so far
constexpr double kMaxExtra = 0.2;
constexpr double kRelaxSeconds = 30.0;    // how long room must have been to spare before giving some back
constexpr double kRelaxStep = 0.003;      // at most this much at a time (it is glided, not jumped)

constexpr double kQuantumSeconds = (double)kQuantum / kSampleRate;

double clampTo(double value, double limit) { return std::max(-limit, std::min(limit, value)); }

}  // namespace

SourceReader::SourceReader(std::shared_ptr<SourceBuffer> buffer, double marginSeconds)
    : buffer_(std::move(buffer)), margin_(marginSeconds) {}

void SourceReader::seek(double position) {
    double whole = std::floor(position);
    positionWhole_ = (int64_t)whole;
    positionFraction_ = position - whole;
}

void SourceReader::read(double cycleTime, double now, float* stereo) {
    if (!buffer_->clockValid()) {
        std::memset(stereo, 0, sizeof(float) * kQuantum * kChannels);
        started_ = false;
        statActive_.store(false, std::memory_order_relaxed);
        return;
    }

    // A whole quantum is read at once and the source delivers whole packets,
    // so the audio must be at least that old to be there when it is needed.
    const double target = (double)(kQuantum + buffer_->packetFrames()) / kSampleRate + margin_ + extra_;
    const double targetPosition = (cycleTime - target - buffer_->clockOffset()) * kSampleRate;

    if (!started_) {
        seek(targetPosition);
        started_ = true;
    }

    double error = (targetPosition - ((double)positionWhole_ + positionFraction_)) / kSampleRate;
    if (std::fabs(error) > kJumpSeconds) {  // positive: the audio is older than wanted
        seek(targetPosition);
        error = 0.0;
        statJumps_.fetch_add(1, std::memory_order_relaxed);
    }

    integral_ = clampTo(integral_ + kIntegral * error * kQuantumSeconds, kMaxDeviation);
    const double ratio = 1.0 + clampTo(kProportional * error + integral_, kMaxDeviation);

    // Source frames needed: from kTaps/2 - 1 before the first position to
    // kTaps/2 after the last one, plus one spare because the loop below adds
    // the ratio up step by step.
    float window[(kQuantum + kQuantum / 8 + kTaps + 2) * kChannels];
    const int64_t first = positionWhole_ - (kTaps / 2 - 1);
    const int span = (int)std::floor(positionFraction_ + (kQuantum - 1) * ratio) + kTaps + 1;
    const int missing = buffer_->read(first, span, window);

    const FilterTable& table = filterTable();
    double fraction = positionFraction_;
    int base = 0;
    for (int i = 0; i < kQuantum; i++) {
        double scaled = fraction * kPhases;
        int phase = (int)scaled;
        float blend = (float)(scaled - phase);
        const float* low = table.weights[phase];
        const float* high = table.weights[phase + 1];
        const float* in = window + base * kChannels;
        float left = 0.0f, right = 0.0f;
        for (int tap = 0; tap < kTaps; tap++) {
            float weight = low[tap] + blend * (high[tap] - low[tap]);
            left += weight * in[tap * 2];
            right += weight * in[tap * 2 + 1];
        }
        stereo[i * 2] = left;
        stereo[i * 2 + 1] = right;

        fraction += ratio;
        int step = (int)fraction;
        fraction -= step;
        base += step;
    }
    positionWhole_ += base;
    positionFraction_ = fraction;

    const double spare = (double)(buffer_->framesWritten() - (first + span)) / kSampleRate;
    adapt(now, missing, spare);

    statTarget_.store(target, std::memory_order_relaxed);
    statError_.store(error, std::memory_order_relaxed);
    statRatio_.store(ratio, std::memory_order_relaxed);
}

// Decides whether the target age has to change, from how the last read went.
void SourceReader::adapt(double now, int missing, double spare) {
    const bool active = now - buffer_->lastPushTime() < SourceBuffer::kGapSeconds;
    statActive_.store(active, std::memory_order_relaxed);

    // Frames that were not there can mean two things: the source is late, or it
    // has simply stopped (a sound that ended). Which one is only known later.
    if (missing > 0) {
        if (pendingMissing_ == 0) {
            pendingGaps_ = buffer_->gapCount();
            pendingPushTime_ = buffer_->lastPushTime();
        }
        pendingMissing_ += missing;
    }
    if (pendingMissing_ > 0) {
        if (!active || buffer_->gapCount() != pendingGaps_) {
            pendingMissing_ = 0;  // it had stopped: nothing was lost
        } else if (buffer_->lastPushTime() != pendingPushTime_) {
            // It carried on without a pause: that audio was late and has been
            // skipped. Keep further behind, and step back to the new position.
            statLate_.fetch_add((uint64_t)pendingMissing_, std::memory_order_relaxed);
            extra_ = std::min(extra_ + (double)pendingMissing_ / kSampleRate + kSafetySeconds, kMaxExtra);
            pendingMissing_ = 0;
            started_ = false;
            relaxStart_ = now;
            relaxLowest_ = 1e9;
        }
        return;
    }
    if (!active) return;

    if (spare < statSpare_.load(std::memory_order_relaxed)) statSpare_.store(spare, std::memory_order_relaxed);
    relaxLowest_ = std::min(relaxLowest_, spare);
    if (now - relaxStart_ > kRelaxSeconds) {
        const double excess = relaxLowest_ - 2.0 * kSafetySeconds;
        if (extra_ > 0.0 && excess > 0.0) extra_ -= std::min({excess / 2.0, kRelaxStep, extra_});
        relaxStart_ = now;
        relaxLowest_ = 1e9;
    }
}

SourceReader::Stats SourceReader::takeStats() {
    Stats stats;
    stats.targetMs = statTarget_.load(std::memory_order_relaxed) * 1000.0;
    stats.errorMs = statError_.load(std::memory_order_relaxed) * 1000.0;
    stats.ratioPpm = (statRatio_.load(std::memory_order_relaxed) - 1.0) * 1e6;
    double spare = statSpare_.exchange(1e9, std::memory_order_relaxed);
    stats.spareMs = spare > 1e8 ? 0.0 : spare * 1000.0;
    stats.lateFrames = statLate_.load(std::memory_order_relaxed);
    stats.jumps = statJumps_.load(std::memory_order_relaxed);
    stats.active = statActive_.load(std::memory_order_relaxed);
    return stats;
}

}  // namespace wolfe
