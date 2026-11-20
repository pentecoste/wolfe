#include "graph.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>
#include <stdexcept>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace wolfe {

namespace {

constexpr double kQuantumSeconds = (double)kQuantum / kSampleRate;
constexpr size_t kQuantumSamples = (size_t)kQuantum * kChannels;
// A cycle this late means the computer was asleep or stopped in a debugger:
// the graph jumps to the present instead of catching up.
constexpr double kResyncSeconds = 0.2;

// Adds every feed, scaled by its link's gain, into `out`. A gain that changed
// since the previous cycle is faded over the cycle to avoid a click.
void mixFeeds(Plan& plan, std::vector<Plan::Feed>& feeds, float* out) {
    for (Plan::Feed& feed : feeds) {
        const float* in = plan.steps[feed.step].audio.data();
        const float wanted = feed.link->gain.load(std::memory_order_relaxed);
        if (wanted == feed.appliedGain) {
            if (wanted == 0.0f) continue;
            for (size_t i = 0; i < kQuantumSamples; i++) out[i] += in[i] * wanted;
        } else {
            float gain = feed.appliedGain;
            const float step = (wanted - gain) / kQuantum;
            for (int frame = 0; frame < kQuantum; frame++) {
                gain += step;
                out[frame * 2] += in[frame * 2] * gain;
                out[frame * 2 + 1] += in[frame * 2 + 1] * gain;
            }
            feed.appliedGain = wanted;
        }
    }
}

}  // namespace

Graph::Graph() = default;
Graph::~Graph() { stop(); }

Graph::Kind Graph::kindOf(const std::string& id) const {
    if (inputs_.count(id)) return Kind::Input;
    if (effects_.count(id)) return Kind::Effect;
    if (outputs_.count(id)) return Kind::Output;
    return Kind::None;
}

void Graph::requireNewId(const std::string& id) const {
    if (id.empty()) throw std::runtime_error("a node needs an id");
    if (kindOf(id) != Kind::None) throw std::runtime_error("duplicate node id \"" + id + "\"");
}

void Graph::addInput(const std::string& id, InputSpec spec) {
    requireNewId(id);
    auto input = std::make_unique<Input>(std::move(spec));
    if (running_) input->start();
    inputs_[id] = std::move(input);
}

void Graph::addEffect(const std::string& id, const std::string& type) {
    requireNewId(id);
    if (!createEffect(type)) throw std::runtime_error("unknown effect type \"" + type + "\"");
    effects_[id] = type;
}

void Graph::addOutput(const std::string& id, const std::string& device) {
    requireNewId(id);
    OutputNode node;
    // The graph delivers exactly one quantum per cycle.
    node.feed = std::make_shared<SourceBuffer>();
    node.feed->setPacketFrames(kQuantum);
    node.output = std::make_unique<Output>(device, node.feed, marginSeconds_);
    if (running_) node.output->start();
    outputs_[id] = std::move(node);
}

void Graph::removeNode(const std::string& id) {
    if (kindOf(id) == Kind::None) return;
    links_.erase(std::remove_if(links_.begin(), links_.end(),
                                [&](const std::shared_ptr<Link>& link) { return link->from == id || link->to == id; }),
                 links_.end());

    // Taken out of the maps first so that the plan is rebuilt without it;
    // destroying it afterwards stops its thread.
    std::unique_ptr<Input> input;
    OutputNode output;
    if (auto found = inputs_.find(id); found != inputs_.end()) {
        input = std::move(found->second);
        inputs_.erase(found);
    }
    if (auto found = outputs_.find(id); found != outputs_.end()) {
        output = std::move(found->second);
        outputs_.erase(found);
    }
    effects_.erase(id);
    rebuildPlan();
}

std::string Graph::nodeStatus(const std::string& id) const {
    if (auto found = inputs_.find(id); found != inputs_.end()) return found->second->status();
    if (auto found = outputs_.find(id); found != outputs_.end()) return found->second.output->status();
    if (effects_.count(id)) return "running";
    return {};
}

float Graph::takeNodePeak(const std::string& id) {
    if (auto found = inputs_.find(id); found != inputs_.end()) return found->second->buffer()->takePeak();
    if (auto found = outputs_.find(id); found != outputs_.end()) return found->second.output->takePeak();
    return 0.0f;
}

Graph::OutputReport Graph::outputReport(const std::string& id) {
    OutputReport report;
    auto found = outputs_.find(id);
    if (found == outputs_.end()) return report;
    report.output = found->second.output->takeStats();
    report.feed = found->second.output->takeFeedStats();
    if (std::shared_ptr<Plan> current = plan()) {
        for (const Plan::Sink& sink : current->sinks) {
            if (sink.nodeId != id) continue;
            for (int step : sink.inputs) {
                const Plan::Step& input = current->steps[step];
                report.sources.push_back({input.nodeId, input.reader->takeStats()});
            }
        }
    }
    return report;
}

std::shared_ptr<Link> Graph::findLink(const std::string& from, const std::string& to) const {
    for (const auto& link : links_) {
        if (link->from == from && link->to == to) return link;
    }
    return nullptr;
}

void Graph::addLink(const std::string& from, const std::string& to, float gain) {
    const Kind fromKind = kindOf(from), toKind = kindOf(to);
    if (fromKind == Kind::None) throw std::runtime_error("link from unknown node \"" + from + "\"");
    if (toKind == Kind::None) throw std::runtime_error("link to unknown node \"" + to + "\"");
    if (fromKind == Kind::Output) throw std::runtime_error("\"" + from + "\" is an output: nothing can come out of it");
    if (toKind == Kind::Input) throw std::runtime_error("\"" + to + "\" is an input: nothing can go into it");
    if (findLink(from, to)) throw std::runtime_error("\"" + from + "\" is already linked to \"" + to + "\"");

    // A loop would have no first step to compute. It is one if `from` can
    // already be reached by following links onward from `to`.
    std::set<std::string> seen;
    std::function<bool(const std::string&)> leadsBack = [&](const std::string& node) {
        if (node == from) return true;
        if (!seen.insert(node).second) return false;
        for (const auto& link : links_) {
            if (link->from == node && leadsBack(link->to)) return true;
        }
        return false;
    };
    if (leadsBack(to)) throw std::runtime_error("that link would make a loop");

    auto link = std::make_shared<Link>();
    link->from = from;
    link->to = to;
    link->gain = gain;
    links_.push_back(link);
    try {
        rebuildPlan();
    } catch (...) {
        links_.pop_back();
        throw;
    }
}

void Graph::removeLink(const std::string& from, const std::string& to) {
    auto link = findLink(from, to);
    if (!link) return;
    links_.erase(std::find(links_.begin(), links_.end(), link));
    rebuildPlan();
}

void Graph::setLinkGain(const std::string& from, const std::string& to, float gain) {
    if (auto link = findLink(from, to)) link->gain = gain;
}

// Collects everything that can reach an output and orders it so that each
// node comes after the nodes feeding it. Readers, effects and gains of the
// previous plan are carried over, so that changing one link does not disturb
// the audio already flowing through the others.
std::shared_ptr<Plan> Graph::buildPlan(const Plan* previous) const {
    auto plan = std::make_shared<Plan>();
    std::map<std::string, int> stepOf;  // node id -> index in plan->steps
    std::set<std::string> visiting;     // nodes on the path being explored

    auto oldStep = [&](const std::string& id) -> const Plan::Step* {
        if (!previous) return nullptr;
        for (const Plan::Step& candidate : previous->steps) {
            if (candidate.nodeId == id) return &candidate;
        }
        return nullptr;
    };
    // A link that is new to the plan fades in from silence.
    auto appliedGain = [](const std::vector<Plan::Feed>* oldFeeds, const std::shared_ptr<Link>& link) {
        if (oldFeeds) {
            for (const Plan::Feed& feed : *oldFeeds) {
                if (feed.link == link) return feed.appliedGain;
            }
        }
        return 0.0f;
    };

    std::function<int(const std::string&)> visit = [&](const std::string& id) -> int {
        auto done = stepOf.find(id);
        if (done != stepOf.end()) return done->second;
        if (!visiting.insert(id).second) throw std::runtime_error("the links form a loop through \"" + id + "\"");

        Plan::Step step;
        step.nodeId = id;
        step.audio.assign(kQuantumSamples, 0.0f);
        const Plan::Step* old = oldStep(id);

        auto input = inputs_.find(id);
        if (input != inputs_.end()) {
            step.reader = old && old->reader ? old->reader
                                             : std::make_shared<SourceReader>(input->second->buffer(), marginSeconds_);
        } else {
            step.effect = old && old->effect ? old->effect : createEffect(effects_.at(id));
            for (const auto& link : links_) {
                if (link->to != id) continue;
                const int from = visit(link->from);
                step.feeds.push_back({from, link, appliedGain(old ? &old->feeds : nullptr, link)});
            }
        }

        visiting.erase(id);
        plan->steps.push_back(std::move(step));
        stepOf[id] = (int)plan->steps.size() - 1;
        return stepOf[id];
    };

    for (const auto& [id, output] : outputs_) {
        const std::vector<Plan::Feed>* oldFeeds = nullptr;
        if (previous) {
            for (const Plan::Sink& sink : previous->sinks) {
                if (sink.nodeId == id) oldFeeds = &sink.feeds;
            }
        }
        Plan::Sink sink;
        sink.nodeId = id;
        sink.feed = output.feed;
        for (const auto& link : links_) {
            if (link->to != id) continue;
            const int from = visit(link->from);
            sink.feeds.push_back({from, link, appliedGain(oldFeeds, link)});
        }
        plan->sinks.push_back(std::move(sink));
    }

    // Which inputs reach each output, by walking its feeds back.
    for (Plan::Sink& sink : plan->sinks) {
        std::set<int> seen;
        std::function<void(int)> walk = [&](int index) {
            if (!seen.insert(index).second) return;
            const Plan::Step& step = plan->steps[index];
            if (step.reader) sink.inputs.push_back(index);
            for (const Plan::Feed& feed : step.feeds) walk(feed.step);
        };
        for (const Plan::Feed& feed : sink.feeds) walk(feed.step);
    }
    return plan;
}

void Graph::rebuildPlan() {
    // Under the lock: the cycle changes the gains being carried over. Throws,
    // leaving the old plan in place, if the new one is invalid.
    std::lock_guard<std::mutex> lock(planMutex_);
    std::shared_ptr<Plan> next = buildPlan(plan_.get());
    plan_.swap(next);
}

std::shared_ptr<Plan> Graph::plan() const {
    std::lock_guard<std::mutex> lock(planMutex_);
    return plan_;
}

void Graph::start() {
    if (running_) return;
    rebuildPlan();
    for (auto& [id, input] : inputs_) input->start();
    for (auto& [id, output] : outputs_) output.output->start();
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    cycleThread_ = std::thread([this] { cycleMain(); });
    running_ = true;
}

void Graph::stop() {
    if (!running_) return;
    SetEvent(stopEvent_);
    cycleThread_.join();
    CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
    for (auto& [id, output] : outputs_) output.output->stop();
    for (auto& [id, input] : inputs_) input->stop();
    running_ = false;
}

// Cycle n computes the graph's frames [n, n + 1) quanta on the performance
// counter, and runs when they are all in the past.
void Graph::cycleMain() {
    AudioThreadPriority priority;
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    HANDLE handles[2] = {stopEvent_, timer};

    int64_t cycle = (int64_t)std::floor(nowSeconds() / kQuantumSeconds);
    for (;;) {
        const double end = (double)(cycle + 1) * kQuantumSeconds;
        double now = nowSeconds();
        if (now < end) {
            LARGE_INTEGER due;
            due.QuadPart = -std::max<LONGLONG>(1, (LONGLONG)((end - now) * 1e7));
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0) break;
            continue;
        }
        if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) break;
        if (now - end > kResyncSeconds) {
            const int64_t present = (int64_t)std::floor(now / kQuantumSeconds) - 1;
            skipped_.fetch_add((uint64_t)(present - cycle), std::memory_order_relaxed);
            cycle = present;
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(planMutex_);
            if (plan_) runCycle(*plan_, end, end);
        }
        cycle++;
    }
    CloseHandle(timer);
}

void Graph::runCycle(Plan& plan, double cycleTime, double endTime) {
    const double now = nowSeconds();
    for (Plan::Step& step : plan.steps) {
        float* audio = step.audio.data();
        if (step.reader) {
            step.reader->read(cycleTime, now, audio);
        } else {
            std::memset(audio, 0, kQuantumSamples * sizeof(float));
            mixFeeds(plan, step.feeds, audio);
            step.effect->process(audio);
        }
    }
    float out[kQuantumSamples];
    for (Plan::Sink& sink : plan.sinks) {
        std::memset(out, 0, sizeof(out));
        mixFeeds(plan, sink.feeds, out);
        // On the graph's clock these frames end exactly now.
        sink.feed->push(endTime, out, kQuantum);
    }
}

std::string Graph::report() {
    std::string text;
    char line[400];

    for (auto& [id, input] : inputs_) {
        float peak = input->buffer()->takePeak();
        snprintf(line, sizeof(line), "  in  %-12s %-44s peak %6.1f dB\n", id.c_str(), input->status().c_str(),
                 peak > 0 ? 20.0 * log10(peak) : -120.0);
        text += line;
    }
    auto readerLine = [&](const std::string& name, const SourceReader::Stats& stats) {
        if (stats.active) {
            snprintf(line, sizeof(line),
                     "        %-12s age %5.1f ms (%+.2f)  rate %+7.1f ppm  spare %5.1f ms  late %llu  jumps %llu\n",
                     name.c_str(), stats.targetMs, stats.errorMs, stats.ratioPpm, stats.spareMs,
                     (unsigned long long)stats.lateFrames, (unsigned long long)stats.jumps);
        } else {
            snprintf(line, sizeof(line), "        %-12s idle  late %llu  jumps %llu\n", name.c_str(),
                     (unsigned long long)stats.lateFrames, (unsigned long long)stats.jumps);
        }
        text += line;
    };

    // Every input reaching the graph, once.
    if (std::shared_ptr<Plan> current = plan()) {
        text += "  into the graph:\n";
        for (const Plan::Step& step : current->steps) {
            if (step.reader) readerLine(step.nodeId, step.reader->takeStats());
        }
    }
    for (auto& [id, node] : outputs_) {
        float peak = node.output->takePeak();
        Output::Stats outputStats = node.output->takeStats();
        snprintf(line, sizeof(line),
                 "  out %-12s %-44s peak %6.1f dB  queued %.1f ms (lowest %.1f)  longest wait %.1f ms  underruns %llu\n",
                 id.c_str(), node.output->status().c_str(), peak > 0 ? 20.0 * log10(peak) : -120.0,
                 outputStats.queuedMs, outputStats.lowestMs, outputStats.longestWaitMs,
                 (unsigned long long)outputStats.underruns);
        text += line;
        readerLine("from graph", node.output->takeFeedStats());
    }
    const uint64_t skipped = skippedCycles();
    if (skipped) {
        snprintf(line, sizeof(line), "  graph cycles skipped: %llu\n", (unsigned long long)skipped);
        text += line;
    }
    return text;
}

}  // namespace wolfe
