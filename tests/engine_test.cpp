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
#include <functional>
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

// --- the other effects --------------------------------------------------------

using Signal = std::function<float(int frame)>;  // the same on both sides

void set(Effect& effect, const char* type, const char* key, float value) {
    const EffectType* description = findEffectType(type);
    for (size_t i = 0; i < description->parameters.size(); i++) {
        if (std::string(description->parameters[i].key) == key) effect.setParameter((int)i, value);
    }
}

// The left side of what comes out, `seconds` long.
std::vector<float> run(Effect& effect, double seconds, const Signal& signal, std::vector<float>* right = nullptr) {
    std::vector<float> left;
    float stereo[kQuantum * kChannels];
    const int cycles = (int)(seconds * kSampleRate / kQuantum);
    for (int cycle = 0; cycle < cycles; cycle++) {
        for (int i = 0; i < kQuantum; i++) stereo[i * 2] = stereo[i * 2 + 1] = signal(cycle * kQuantum + i);
        effect.process(stereo);
        for (int i = 0; i < kQuantum; i++) {
            left.push_back(stereo[i * 2]);
            if (right) right->push_back(stereo[i * 2 + 1]);
        }
    }
    return left;
}

Signal sine(double hz, float amplitude) {
    return [=](int frame) { return amplitude * (float)std::sin(2.0 * kPi * hz * frame / kSampleRate); };
}

// Level in dB of a stretch of samples, from `from` seconds on.
double rmsDb(const std::vector<float>& samples, double from, double to = 1e9) {
    double sum = 0;
    size_t count = 0;
    for (size_t i = (size_t)(from * kSampleRate); i < samples.size() && i < (size_t)(to * kSampleRate); i++, count++) {
        sum += (double)samples[i] * samples[i];
    }
    return 10.0 * std::log10(sum / std::max<size_t>(count, 1) + 1e-30);
}

double peakOf(const std::vector<float>& samples, double from) {
    double peak = 0;
    for (size_t i = (size_t)(from * kSampleRate); i < samples.size(); i++) peak = std::max(peak, (double)std::fabs(samples[i]));
    return peak;
}

// The "rnnoise" node must hand RNNoise the same numbers as calling the library
// directly on 16-bit-scaled audio, each side by itself, and take the work only
// once when the two sides are the same.
void testRnnoiseNode() {
    printf("rnnoise node against the library called directly\n");
    std::mt19937 random(777);
    std::uniform_real_distribution<float> noise(-0.1f, 0.1f);
    auto compare = [&](bool sidesDiffer) {
        std::shared_ptr<Effect> node = createEffect("rnnoise");
        DenoiseState* direct[2] = {rnnoise_create(nullptr), rnnoise_create(nullptr)};
        double largestDifference = 0;
        for (int frame = 0; frame < 300; frame++) {
            float stereo[kQuantum * kChannels], side[2][kQuantum];
            for (int i = 0; i < kQuantum; i++) {
                const float tone = 0.2f * (float)std::sin(2.0 * kPi * 220.0 * (frame * kQuantum + i) / kSampleRate);
                const float left = tone + noise(random), right = sidesDiffer ? tone + noise(random) : left;
                stereo[i * 2] = left;
                stereo[i * 2 + 1] = right;
                side[0][i] = left * 32768.0f;
                side[1][i] = right * 32768.0f;
            }
            node->process(stereo);
            for (int s = 0; s < 2; s++) {
                rnnoise_process_frame(direct[s], side[s], side[s]);
                for (int i = 0; i < kQuantum; i++) {
                    largestDifference = std::max(largestDifference, (double)std::fabs(stereo[i * 2 + s] - side[s][i] / 32768.0f));
                }
            }
        }
        for (DenoiseState* state : direct) rnnoise_destroy(state);
        return largestDifference;
    };
    const double mono = compare(false), stereo = compare(true);
    printf("    largest difference: %.2e with the same on both sides, %.2e with different sides\n", mono, stereo);
    expect(mono < 1e-6 && stereo < 1e-6, "same output as the library, each side cleaned by itself");

    // At 0% the original comes out, one frame late like the cleaned sound.
    std::shared_ptr<Effect> off = createEffect("rnnoise");
    set(*off, "rnnoise", "amount", 0);
    const std::vector<float> out = run(*off, 0.1, sine(300, 0.3f));
    bool late = true;
    for (size_t i = kQuantum; i < out.size(); i++) late &= std::fabs(out[i] - sine(300, 0.3f)((int)(i - kQuantum))) < 1e-6f;
    expect(late, "at 0% the original comes out, one frame late, lined up with the cleaned sound");

    // A microphone's hiss when nobody speaks is taken away; a buzz with
    // harmonics, as a voice has, comes through. (On white hiss alone RNNoise
    // is uneven from one level to another: it learnt from real noises.)
    std::normal_distribution<float> hiss(0.0f, 0.001f);  // -60 dB
    std::shared_ptr<Effect> node = createEffect("rnnoise");
    const std::vector<float> cleaned = run(*node, 3.0, [&](int) { return hiss(random); });
    const double after = rmsDb(cleaned, 1.0);
    const Signal buzz = [&](int frame) {
        double voice = 0;
        for (int h = 1; h <= 12; h++) voice += std::sin(2.0 * kPi * 140.0 * h * frame / kSampleRate + h) / h;
        return 0.1f * (float)voice + hiss(random);
    };
    node = createEffect("rnnoise");
    const double voiceIn = rmsDb(run(*createEffect("gain"), 3.0, buzz), 1.0);
    const double voiceOut = rmsDb(run(*node, 3.0, buzz), 1.0);
    printf("    hiss at -60 dB comes out at %.0f dB; a voice-like buzz goes in at %.1f dB, out at %.1f dB\n", after,
           voiceIn, voiceOut);
    expect(after < -75.0, "a microphone's hiss is taken away");
    expect(std::fabs(voiceOut - voiceIn) < 1.5, "a voice comes through");
}

// The "gain" node multiplies by its gain; a new gain is reached over one
// cycle, without a jump that would click.
void testGainNode() {
    printf("gain node\n");
    std::shared_ptr<Effect> node = createEffect("gain");
    float stereo[kQuantum * kChannels];
    auto fill = [&] {
        for (int i = 0; i < kQuantum * kChannels; i++) stereo[i] = 0.5f;
    };
    fill();
    node->process(stereo);
    expect(stereo[0] == 0.5f && stereo[kQuantum * kChannels - 1] == 0.5f, "at first it changes nothing");

    node->setParameter(0, (float)(20.0 * std::log10(0.25)));  // in dB
    fill();
    node->process(stereo);
    float largestStep = 0.0f;
    for (int i = 1; i < kQuantum; i++) largestStep = std::max(largestStep, std::fabs(stereo[i * 2] - stereo[(i - 1) * 2]));
    expect(stereo[0] < 0.5f && stereo[0] > 0.49f, "a new gain starts from the old one");
    expect(std::fabs(stereo[(kQuantum - 1) * 2] - 0.125f) < 1e-5f && largestStep < 0.001f,
           "and arrives at the end of the cycle, a little at a time");
    expect(stereo[(kQuantum - 1) * 2 + 1] == stereo[(kQuantum - 1) * 2], "on both channels");

    fill();
    node->process(stereo);
    expect(std::fabs(stereo[0] - 0.125f) < 1e-5f && stereo[kQuantum * kChannels - 1] == stereo[0], "then it stays there");
}

// Every effect the editor offers, with its settings all at the bottom, all at
// the top or as they start, stays finite and bounded on loud noise and tone,
// and on the silence after it.
void testEffectsStayBounded() {
    printf("every effect, at the edges of its settings\n");
    std::mt19937 random(42);
    std::uniform_real_distribution<float> noise(-0.3f, 0.3f);
    const Signal loud = [&](int frame) { return 0.7f * (float)std::sin(2.0 * kPi * 440.0 * frame / kSampleRate) + noise(random); };
    const Signal silence = [](int) { return 0.0f; };
    int fine = 0, tried = 0;
    for (const EffectType& type : effectTypes()) {
        if (!type.offered) continue;
        for (int setting = 0; setting < 3; setting++) {
            std::shared_ptr<Effect> effect = createEffect(type.type);
            for (size_t i = 0; i < type.parameters.size(); i++) {
                const EffectParameter& parameter = type.parameters[i];
                if (setting == 1) effect->setParameter((int)i, parameter.min);
                if (setting == 2) effect->setParameter((int)i, parameter.max);
            }
            std::vector<float> out = run(*effect, 2.0, loud);
            const std::vector<float> after = run(*effect, 2.0, silence);
            out.insert(out.end(), after.begin(), after.end());
            bool ok = true;
            for (float sample : out) ok &= std::isfinite(sample) && std::fabs(sample) < 200.0f;  // gain goes up to +40 dB
            if (!ok) printf("    %s, settings %s: out of bounds\n", type.type, setting == 1 ? "at the bottom" : setting == 2 ? "at the top" : "as they start");
            fine += ok ? 1 : 0;
            tried++;
        }
    }
    expect(fine == tried, "finite and bounded");
}

void testEffectsDoTheirJob() {
    printf("what each effect does\n");
    {
        std::shared_ptr<Effect> filter = createEffect("highpass");
        set(*filter, "highpass", "cutoff_hz", 1000);
        const double low = rmsDb(run(*filter, 1.0, sine(50, 0.5f)), 0.5);
        filter = createEffect("highpass");
        set(*filter, "highpass", "cutoff_hz", 1000);
        const double high = rmsDb(run(*filter, 1.0, sine(8000, 0.5f)), 0.5);
        const double reference = rmsDb(run(*createEffect("gain"), 1.0, sine(8000, 0.5f)), 0.5);
        printf("    high-pass at 1 kHz: 50 Hz %.1f dB, 8 kHz %+.2f dB\n", low - reference, high - reference);
        expect(low - reference < -40.0 && std::fabs(high - reference) < 0.5, "high-pass: lows out, highs through");
    }
    {
        std::shared_ptr<Effect> filter = createEffect("lowpass");
        set(*filter, "lowpass", "cutoff_hz", 1000);
        const double high = rmsDb(run(*filter, 1.0, sine(8000, 0.5f)), 0.5);
        filter = createEffect("lowpass");
        set(*filter, "lowpass", "cutoff_hz", 1000);
        const double low = rmsDb(run(*filter, 1.0, sine(100, 0.5f)), 0.5);
        const double reference = rmsDb(run(*createEffect("gain"), 1.0, sine(100, 0.5f)), 0.5);
        printf("    low-pass at 1 kHz: 8 kHz %.1f dB, 100 Hz %+.2f dB\n", high - reference, low - reference);
        expect(high - reference < -30.0 && std::fabs(low - reference) < 0.5, "low-pass: highs out, lows through");
    }
    {
        std::shared_ptr<Effect> eq = createEffect("equalizer");
        set(*eq, "equalizer", "low_db", 12);
        const double boosted = rmsDb(run(*eq, 1.0, sine(40, 0.1f)), 0.5) - rmsDb(run(*createEffect("gain"), 1.0, sine(40, 0.1f)), 0.5);
        printf("    equalizer, lows +12 dB: 40 Hz %+.1f dB\n", boosted);
        expect(boosted > 10.0 && boosted < 12.5, "equalizer: the lows go up");
    }
    {
        std::shared_ptr<Effect> compressor = createEffect("compressor");  // -20 dB, 4:1
        const double peak = 20.0 * std::log10(peakOf(run(*compressor, 2.0, sine(200, 1.0f)), 1.5));
        printf("    compressor, 0 dB in, -20 dB threshold, 4:1: %.1f dB out\n", peak);
        expect(peak > -17.0 && peak < -13.0, "compressor: 20 dB over the threshold comes out 5 dB over it");
    }
    {
        std::shared_ptr<Effect> limiter = createEffect("limiter");
        set(*limiter, "limiter", "ceiling_db", -6);
        const double peak = peakOf(run(*limiter, 1.0, sine(200, 1.0f)), 0.0);
        expect(peak <= std::pow(10.0, -6.0 / 20.0) + 1e-4, "limiter: never over the ceiling, from the first sample");
    }
    {
        std::shared_ptr<Effect> gate = createEffect("gate");  // -50 dB
        const double hiss = rmsDb(run(*gate, 1.0, sine(1000, 0.0003f)), 0.5);
        const double voice = rmsDb(run(*gate, 1.0, sine(1000, 0.1f)), 0.5) - rmsDb(run(*createEffect("gain"), 1.0, sine(1000, 0.1f)), 0.5);
        printf("    noise gate at -50 dB: a -70 dB hiss comes out at %.0f dB, a -20 dB tone %+.2f dB\n", hiss, voice);
        expect(hiss < -120.0 && std::fabs(voice) < 0.1, "noise gate: closed under the threshold, open over it");
    }
    {
        std::shared_ptr<Effect> delay = createEffect("delay");
        set(*delay, "delay", "time_ms", 100);
        set(*delay, "delay", "feedback", 0);
        set(*delay, "delay", "mix", 100);
        const std::vector<float> out = run(*delay, 0.5, [](int frame) { return frame == 0 ? 1.0f : 0.0f; });
        const size_t at = std::max_element(out.begin(), out.end()) - out.begin();
        printf("    delay of 100 ms: the click comes back after %.2f ms\n", at * 1000.0 / kSampleRate);
        expect(at >= 4799 && at <= 4801, "delay: the echo arrives when it should");
    }
    {
        std::shared_ptr<Effect> reverb = createEffect("reverb");
        set(*reverb, "reverb", "mix", 100);
        const std::vector<float> out = run(*reverb, 4.0, [](int frame) { return frame < 48 ? 0.5f : 0.0f; });
        const double early = rmsDb(out, 0.1, 0.6), late = rmsDb(out, 3.0, 4.0);
        printf("    reverb: %.0f dB in the first half second, %.0f dB after three seconds\n", early, late);
        expect(early > -60.0 && late < early - 30.0, "reverb: a tail that dies away");
    }
    {
        std::shared_ptr<Effect> crusher = createEffect("bitcrusher");
        set(*crusher, "bitcrusher", "bits", 2);
        const std::vector<float> out = run(*crusher, 0.5, sine(300, 0.9f));
        bool fewLevels = true;
        int longestRun = 0, runLength = 1, shortestRun = 1000;
        for (size_t i = 0; i < out.size(); i++) {
            fewLevels &= out[i] == 0.0f || std::fabs(std::fabs(out[i]) - 0.5f) < 1e-6f || std::fabs(std::fabs(out[i]) - 1.0f) < 1e-6f;
            if (i > 0 && out[i] == out[i - 1]) runLength++;
            else if (i > 0) shortestRun = std::min(shortestRun, runLength), runLength = 1;
            longestRun = std::max(longestRun, runLength);
        }
        expect(fewLevels, "bitcrusher: 2 bits leave only a few levels");
        expect(shortestRun >= 5, "and at 8000 Hz each value is held for 6 samples");
    }
    {
        std::shared_ptr<Effect> pan = createEffect("pan");
        set(*pan, "pan", "pan", 100);
        std::vector<float> right;
        const std::vector<float> left = run(*pan, 0.2, sine(500, 0.5f), &right);
        expect(peakOf(left, 0.05) == 0.0 && std::fabs(peakOf(right, 0.05) - 0.5) < 0.01, "pan: all the way right, left silent");
    }
    {
        std::shared_ptr<Effect> tremolo = createEffect("tremolo");
        set(*tremolo, "tremolo", "depth", 100);
        const std::vector<float> out = run(*tremolo, 1.0, [](int) { return 0.5f; });
        const auto [low, high] = std::minmax_element(out.begin(), out.end());
        expect(*low < 0.01f && *high > 0.49f, "tremolo: at full depth the volume goes all the way down and back");
    }
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
    testGainNode();
    testEffectsStayBounded();
    testEffectsDoTheirJob();
    printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
