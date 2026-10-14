#include "graph.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>
#include <stdexcept>

namespace wolfe {

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
    auto output = std::make_unique<Output>(device);
    if (running_) output->start();
    outputs_[id] = std::move(output);
}

void Graph::removeNode(const std::string& id) {
    if (kindOf(id) == Kind::None) return;
    links_.erase(std::remove_if(links_.begin(), links_.end(),
                                [&](const std::shared_ptr<Link>& link) { return link->from == id || link->to == id; }),
                 links_.end());

    // Taken out of the maps first so that the plans are rebuilt without it;
    // destroying it afterwards stops its thread.
    std::unique_ptr<Input> input;
    std::unique_ptr<Output> output;
    if (auto found = inputs_.find(id); found != inputs_.end()) {
        input = std::move(found->second);
        inputs_.erase(found);
    }
    if (auto found = outputs_.find(id); found != outputs_.end()) {
        output = std::move(found->second);
        outputs_.erase(found);
    }
    effects_.erase(id);
    rebuildPlans();
}

std::string Graph::nodeStatus(const std::string& id) const {
    if (auto found = inputs_.find(id); found != inputs_.end()) return found->second->status();
    if (auto found = outputs_.find(id); found != outputs_.end()) return found->second->status();
    if (effects_.count(id)) return "running";
    return {};
}

float Graph::takeNodePeak(const std::string& id) {
    if (auto found = inputs_.find(id); found != inputs_.end()) return found->second->buffer()->takePeak();
    if (auto found = outputs_.find(id); found != outputs_.end()) return found->second->takePeak();
    return 0.0f;
}

Graph::OutputReport Graph::outputReport(const std::string& id) {
    OutputReport report;
    auto found = outputs_.find(id);
    if (found == outputs_.end()) return report;
    report.output = found->second->takeStats();
    if (std::shared_ptr<Plan> plan = found->second->plan()) {
        for (const Plan::Step& step : plan->steps) {
            if (step.reader) report.sources.push_back({step.nodeId, step.reader->takeStats()});
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
        rebuildPlans();
    } catch (...) {
        links_.pop_back();
        throw;
    }
}

void Graph::removeLink(const std::string& from, const std::string& to) {
    auto link = findLink(from, to);
    if (!link) return;
    links_.erase(std::find(links_.begin(), links_.end(), link));
    rebuildPlans();
}

void Graph::setLinkGain(const std::string& from, const std::string& to, float gain) {
    if (auto link = findLink(from, to)) link->gain = gain;
}

// Collects everything that can reach `outputId` and orders it so that each
// node comes after the nodes feeding it. Readers and effects of the previous
// plan are carried over, so that changing one link does not disturb the audio
// already flowing through the others.
std::shared_ptr<Plan> Graph::buildPlan(const std::string& outputId, const Plan* previous) const {
    auto plan = std::make_shared<Plan>();
    std::map<std::string, int> stepOf;  // node id -> index in plan->steps
    std::set<std::string> visiting;     // nodes on the path being explored

    std::function<int(const std::string&)> visit = [&](const std::string& id) -> int {
        auto done = stepOf.find(id);
        if (done != stepOf.end()) return done->second;
        if (!visiting.insert(id).second) throw std::runtime_error("the links form a loop through \"" + id + "\"");

        Plan::Step step;
        step.nodeId = id;
        step.audio.assign((size_t)kQuantum * kChannels, 0.0f);

        const Plan::Step* old = nullptr;
        if (previous) {
            for (const Plan::Step& candidate : previous->steps) {
                if (candidate.nodeId == id) old = &candidate;
            }
        }

        auto input = inputs_.find(id);
        if (input != inputs_.end()) {
            step.reader = old && old->reader ? old->reader
                                             : std::make_shared<SourceReader>(input->second->buffer(), marginSeconds_);
        } else {
            step.effect = old && old->effect ? old->effect : createEffect(effects_.at(id));
            for (const auto& link : links_) {
                if (link->to != id) continue;
                // A link that is new to this plan fades in from silence.
                float applied = 0.0f;
                if (old) {
                    for (const Plan::Feed& feed : old->feeds) {
                        if (feed.link == link) applied = feed.appliedGain;
                    }
                }
                step.feeds.push_back({visit(link->from), link, applied});
            }
        }

        visiting.erase(id);
        plan->steps.push_back(std::move(step));
        stepOf[id] = (int)plan->steps.size() - 1;
        return stepOf[id];
    };

    for (const auto& link : links_) {
        if (link->to != outputId) continue;
        float applied = 0.0f;
        if (previous) {
            for (const Plan::Feed& feed : previous->outputFeeds) {
                if (feed.link == link) applied = feed.appliedGain;
            }
        }
        plan->outputFeeds.push_back({visit(link->from), link, applied});
    }
    return plan;
}

void Graph::rebuildPlans() {
    // Build all of them first: if one is invalid, none is replaced.
    std::vector<std::pair<Output*, std::shared_ptr<Plan>>> plans;
    for (auto& [id, output] : outputs_) {
        std::shared_ptr<Plan> previous = output->plan();
        plans.emplace_back(output.get(), buildPlan(id, previous.get()));
    }
    for (auto& [output, plan] : plans) output->setPlan(plan);
}

void Graph::start() {
    if (running_) return;
    rebuildPlans();
    for (auto& [id, input] : inputs_) input->start();
    for (auto& [id, output] : outputs_) output->start();
    running_ = true;
}

void Graph::stop() {
    if (!running_) return;
    for (auto& [id, output] : outputs_) output->stop();
    for (auto& [id, input] : inputs_) input->stop();
    running_ = false;
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
    for (auto& [id, output] : outputs_) {
        float peak = output->takePeak();
        Output::Stats outputStats = output->takeStats();
        snprintf(line, sizeof(line),
                 "  out %-12s %-44s peak %6.1f dB  queued %.1f ms (lowest %.1f)  longest wait %.1f ms  underruns %llu\n",
                 id.c_str(), output->status().c_str(), peak > 0 ? 20.0 * log10(peak) : -120.0, outputStats.queuedMs,
                 outputStats.lowestMs, outputStats.longestWaitMs, (unsigned long long)outputStats.underruns);
        text += line;

        std::shared_ptr<Plan> plan = output->plan();
        if (!plan) continue;
        for (const Plan::Step& step : plan->steps) {
            if (!step.reader) continue;
            SourceReader::Stats stats = step.reader->takeStats();
            if (stats.active) {
                snprintf(line, sizeof(line),
                         "        %-12s age %5.1f ms (%+.2f)  rate %+7.1f ppm  spare %5.1f ms  late %llu  jumps %llu\n",
                         step.nodeId.c_str(), stats.targetMs, stats.errorMs, stats.ratioPpm, stats.spareMs,
                         (unsigned long long)stats.lateFrames, (unsigned long long)stats.jumps);
            } else {
                snprintf(line, sizeof(line), "        %-12s idle  late %llu  jumps %llu\n", step.nodeId.c_str(),
                         (unsigned long long)stats.lateFrames, (unsigned long long)stats.jumps);
            }
            text += line;
        }
    }
    return text;
}

}  // namespace wolfe
