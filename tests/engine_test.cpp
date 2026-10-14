// Checks the part of the engine that needs no audio device: a simulated source
// feeds a SourceBuffer, a simulated output reads it through a SourceReader, and
// the result is compared with what should have come out.
//
// Time is simulated, so a minute of audio takes a moment to run.
#include "effect.h"
#include "rnnoise.h"
#include "source_buffer.h"
#include "source_reader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace wolfe;

namespace {

const double kPi = 3.14159265358979323846;
int g_failures = 0;

void expect(bool ok, const char* what) {
    printf("    %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_failures++;
}

struct Scenario {
    double sourcePpm = 0;        // how fast the source's clock runs compared to the output's
    double toneHz = 1000;
    double seconds = 60;
    double jitterMs = 1.0;       // random lateness of each block
    int blockFrames = 480;
    // Sound comes in bursts when burstOn > 0: on for burstOn seconds, then silent for burstOff.
    double burstOn = 0, burstOff = 0;
    // Every lateEvery seconds one block arrives lateMs late.
    double lateEvery = 0, lateMs = 0;
};

struct Result {
    std::vector<float> output;   // left channel
    SourceReader::Stats stats;
    double firstTargetMs = 0;
    int64_t sourceFramesSent = 0;
};

Result simulate(const Scenario& scenario) {
    auto buffer = std::make_shared<SourceBuffer>();
    buffer->setPacketFrames(scenario.blockFrames);
    SourceReader reader(buffer, 0.004);

    const double sourceRate = kSampleRate * (1.0 + scenario.sourcePpm * 1e-6);
    const double blockSeconds = scenario.blockFrames / sourceRate;
    const double cycleSeconds = (double)kQuantum / kSampleRate;
    std::mt19937 random(12345);
    std::uniform_real_distribution<double> jitter(0.0, scenario.jitterMs / 1000.0);

    Result result;
    std::vector<float> block((size_t)scenario.blockFrames * 2);
    float cycle[kQuantum * kChannels];

    int64_t blockIndex = 0;     // blocks of source time elapsed
    int64_t sourceFrame = 0;    // frames of tone generated
    double nextLate = scenario.lateEvery;
    double nextPush = 0.2;      // the source starts a little after the output
    double blockDue = nextPush;
    int64_t cycleIndex = 0;

    auto schedulePush = [&] {
        blockDue += blockSeconds;
        nextPush = blockDue + jitter(random);
        if (scenario.lateEvery > 0 && blockDue >= nextLate) {
            nextPush = blockDue + scenario.lateMs / 1000.0;
            nextLate += scenario.lateEvery;
        }
    };

    for (;;) {
        const double nextCycle = cycleIndex * cycleSeconds;
        if (std::min(nextCycle, nextPush) > scenario.seconds) break;

        if (nextPush <= nextCycle) {
            bool sounding = true;
            if (scenario.burstOn > 0) {
                double phase = std::fmod(blockIndex * blockSeconds, scenario.burstOn + scenario.burstOff);
                sounding = phase < scenario.burstOn;
            }
            if (sounding) {
                for (int i = 0; i < scenario.blockFrames; i++, sourceFrame++) {
                    float sample = 0.5f * (float)std::sin(2.0 * kPi * scenario.toneHz * (double)sourceFrame / kSampleRate);
                    block[(size_t)i * 2] = block[(size_t)i * 2 + 1] = sample;
                }
                // An application that is silent delivers nothing at all.
                buffer->push(nextPush, block.data(), scenario.blockFrames);
                result.sourceFramesSent += scenario.blockFrames;
            }
            blockIndex++;
            schedulePush();
        } else {
            reader.read(nextCycle, nextCycle, cycle);
            for (int i = 0; i < kQuantum; i++) result.output.push_back(cycle[i * 2]);
            if (cycleIndex == 200) result.firstTargetMs = reader.takeStats().targetMs;
            cycleIndex++;
        }
    }
    result.stats = reader.takeStats();
    return result;
}

// Solves the 4x4 system a * x = b in place (Gaussian elimination with pivoting).
void solve4(double a[4][4], double b[4], double x[4]) {
    for (int column = 0; column < 4; column++) {
        int pivot = column;
        for (int row = column + 1; row < 4; row++) {
            if (std::fabs(a[row][column]) > std::fabs(a[pivot][column])) pivot = row;
        }
        for (int k = 0; k < 4; k++) std::swap(a[column][k], a[pivot][k]);
        std::swap(b[column], b[pivot]);
        for (int row = column + 1; row < 4; row++) {
            double factor = a[row][column] / a[column][column];
            for (int k = column; k < 4; k++) a[row][k] -= factor * a[column][k];
            b[row] -= factor * b[column];
        }
    }
    for (int row = 3; row >= 0; row--) {
        double sum = b[row];
        for (int k = row + 1; k < 4; k++) sum -= a[row][k] * x[k];
        x[row] = sum / a[row][row];
    }
}

// How much louder the tone is than everything else in `samples`, in dB, by
// fitting a sine of free amplitude, phase, offset and frequency (IEEE 1057).
double purityDb(const float* samples, int count, double roughHz) {
    double omega = 2.0 * kPi * roughHz / kSampleRate;
    double cosAmp = 0, sinAmp = 0, offset = 0;
    for (int iteration = 0; iteration < 10; iteration++) {
        double a[4][4] = {}, b[4] = {}, x[4];
        for (int n = 0; n < count; n++) {
            double c = std::cos(omega * n), s = std::sin(omega * n);
            double basis[4] = {c, s, 1.0, -cosAmp * n * s + sinAmp * n * c};
            for (int i = 0; i < 4; i++) {
                b[i] += basis[i] * samples[n];
                for (int j = 0; j < 4; j++) a[i][j] += basis[i] * basis[j];
            }
        }
        if (iteration == 0) {
            // No amplitude estimate yet: the frequency column is all zeros.
            a[3][3] = 1.0;
        }
        solve4(a, b, x);
        cosAmp = x[0];
        sinAmp = x[1];
        offset = x[2];
        if (iteration > 0) omega += x[3];
    }
    double residual = 0;
    for (int n = 0; n < count; n++) {
        double fitted = cosAmp * std::cos(omega * n) + sinAmp * std::sin(omega * n) + offset;
        residual += (samples[n] - fitted) * (samples[n] - fitted);
    }
    double tone = (cosAmp * cosAmp + sinAmp * sinAmp) / 2.0;
    return 10.0 * std::log10(tone / std::max(residual / count, 1e-30));
}

struct Purity {
    double median, worst;
};

Purity purityOver(const std::vector<float>& output, double fromSeconds, double toneHz) {
    const int window = 4096;
    std::vector<double> values;
    for (size_t start = (size_t)(fromSeconds * kSampleRate); start + window <= output.size(); start += window) {
        values.push_back(purityDb(output.data() + start, window, toneHz));
    }
    std::sort(values.begin(), values.end());
    return {values[values.size() / 2], values.front()};
}

void report(const Result& result) {
    printf("    target %.1f ms, error %+.3f ms, rate %+.1f ppm, late frames %llu, jumps %llu\n", result.stats.targetMs,
           result.stats.errorMs, result.stats.ratioPpm, (unsigned long long)result.stats.lateFrames,
           (unsigned long long)result.stats.jumps);
}

void testDrift(double ppm, double toneHz, double jitterMs, double wantedPurity) {
    printf("source %+.0f ppm, %.0f Hz tone, blocks up to %.1f ms late\n", ppm, toneHz, jitterMs);
    Scenario scenario;
    scenario.sourcePpm = ppm;
    scenario.toneHz = toneHz;
    scenario.jitterMs = jitterMs;
    Result result = simulate(scenario);
    report(result);
    // The tone leaves at the source's own pitch.
    Purity purity = purityOver(result.output, 25.0, toneHz * (1.0 + ppm * 1e-6));
    printf("    purity after settling: median %.1f dB, worst window %.1f dB\n", purity.median, purity.worst);
    expect(result.stats.lateFrames == 0, "no audio arrived too late");
    expect(result.stats.jumps == 0, "the position never jumped");
    expect(std::fabs(result.stats.ratioPpm - ppm) < 30.0, "the rate settled on the source's");
    expect(std::fabs(result.stats.errorMs) < 0.3, "the audio is kept at the target age");
    expect(purity.median > wantedPurity, "the tone comes out clean");
}

void testBursts() {
    printf("an application that plays 0.5 s of sound every 1.5 s\n");
    Scenario scenario;
    scenario.sourcePpm = 120;
    scenario.burstOn = 0.5;
    scenario.burstOff = 1.0;
    scenario.seconds = 30;
    Result result = simulate(scenario);
    report(result);
    double energy = 0;
    for (float sample : result.output) energy += (double)sample * sample;
    // Each frame sent carries 0.5^2 / 2 on average; the last burst may be cut by the end of the test.
    double expected = result.sourceFramesSent * 0.125;
    printf("    energy out / energy in: %.4f\n", energy / expected);
    expect(std::fabs(energy / expected - 1.0) < 0.02, "every burst comes out whole");
    expect(result.stats.lateFrames == 0, "the end of a burst is not mistaken for late audio");
    expect(std::fabs(result.stats.targetMs - result.firstTargetMs) < 0.01, "the target age did not move");
}

void testLateBlocks() {
    printf("a source where one block every 5 s arrives 15 ms late\n");
    Scenario scenario;
    scenario.lateEvery = 5;
    scenario.lateMs = 15;
    scenario.seconds = 28;
    Result result = simulate(scenario);
    report(result);
    Result longer = simulate([&] {
        Scenario twice = scenario;
        twice.seconds = 56;
        return twice;
    }());
    printf("    late frames after 28 s: %llu, after 56 s: %llu; target went from %.1f to %.1f ms\n",
           (unsigned long long)result.stats.lateFrames, (unsigned long long)longer.stats.lateFrames,
           result.firstTargetMs, result.stats.targetMs);
    expect(result.stats.lateFrames > 0, "the first late block is noticed");
    expect(result.stats.targetMs > result.firstTargetMs + 5.0, "the target age grew to cover it");
    expect(longer.stats.lateFrames == result.stats.lateFrames, "after that, late blocks no longer cost audio");
}

// The "rnnoise" node must hand RNNoise the same numbers as calling the library
// directly on 16-bit-scaled mono, and return its answer on both channels.
void testRnnoiseNode() {
    printf("rnnoise node against the library called directly\n");
    std::shared_ptr<Effect> node = createEffect("rnnoise");
    DenoiseState* direct = rnnoise_create(nullptr);
    std::mt19937 random(777);
    std::uniform_real_distribution<float> noise(-0.1f, 0.1f);

    double largestDifference = 0, energyIn = 0, energyOut = 0;
    for (int frame = 0; frame < 300; frame++) {
        float stereo[kQuantum * kChannels], mono[kQuantum];
        for (int i = 0; i < kQuantum; i++) {
            // A tone under noise, different on the two sides, so that folding to mono matters.
            float tone = 0.2f * (float)std::sin(2.0 * kPi * 220.0 * (frame * kQuantum + i) / kSampleRate);
            float left = tone + noise(random), right = tone + noise(random);
            stereo[i * 2] = left;
            stereo[i * 2 + 1] = right;
            mono[i] = (left + right) * 0.5f * 32768.0f;
            energyIn += (double)mono[i] * mono[i];
        }
        node->process(stereo);
        rnnoise_process_frame(direct, mono, mono);
        for (int i = 0; i < kQuantum; i++) {
            energyOut += (double)mono[i] * mono[i];
            largestDifference = std::max(largestDifference, (double)std::fabs(stereo[i * 2] - mono[i] / 32768.0f));
            largestDifference = std::max(largestDifference, (double)std::fabs(stereo[i * 2 + 1] - mono[i] / 32768.0f));
        }
    }
    rnnoise_destroy(direct);
    printf("    largest difference %.2e, level change %.1f dB\n", largestDifference,
           10.0 * std::log10(energyOut / energyIn));
    expect(largestDifference < 1e-6, "same output as the library");
    expect(energyOut < energyIn, "it does remove something");
}

}  // namespace

int main() {
    // With perfectly regular blocks what is left is the interpolation filter itself.
    testDrift(+150, 1000, 0.0, 100);
    testDrift(+150, 15000, 0.0, 85);
    // Irregular arrival makes the estimate of the source's clock wobble a
    // little, which shows up as a slow pitch wobble, larger at high frequencies.
    testDrift(+150, 1000, 1.0, 88);
    testDrift(-300, 1000, 1.0, 88);
    testDrift(+150, 15000, 1.0, 65);
    testBursts();
    testLateBlocks();
    testRnnoiseNode();
    printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
