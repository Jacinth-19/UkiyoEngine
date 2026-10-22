#include "engine/async_load.h"

#include <chrono>
#include <exception>

namespace ukiyo {
namespace {

double steadyMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

void BudgetedLoader::push(std::string name, Step step) {
    if (!step) return;
    queue_.push_back(Entry{std::move(name), std::move(step)});
    progress_.stepsTotal = queue_.size();
    progress_.pending = queue_.size() - next_;
    progress_.done = next_ >= queue_.size();
    progress_.cancelled = false;
}

void BudgetedLoader::cancel() {
    queue_.clear();
    next_ = 0;
    progress_.pending = 0;
    progress_.done = true;
    progress_.cancelled = true;
}

double BudgetedLoader::now() const {
    if (clock_) return clock_();
    static const double start = steadyMs();
    return steadyMs() - start;
}

double BudgetedLoader::fraction() const {
    if (progress_.stepsTotal == 0) return 1.0;
    const double done = double(progress_.stepsRun + progress_.stepsFailed);
    return done >= double(progress_.stepsTotal) ? 1.0 : done / double(progress_.stepsTotal);
}

void BudgetedLoader::runOne(double budgetMs) {
    const Entry& e = queue_[next_];
    const double before = now();
    try {
        e.step();
    } catch (const std::exception& ex) {
        ++progress_.stepsFailed;
        if (onError_) onError_(e.name, ex.what());
    } catch (...) {
        ++progress_.stepsFailed;
        if (onError_) onError_(e.name, "unknown error");
    }
    ++next_;
    ++progress_.stepsRun;
    const double spent = now() - before;
    if (budgetMs > 0.0 && spent > budgetMs) ++progress_.overruns;
    progress_.pending = queue_.size() - next_;
    progress_.done = next_ >= queue_.size();
    progress_.lastTickMs = spent;
}

LoadProgress BudgetedLoader::tick(double budgetMs) {
    const double start = now();
    progress_.lastTickMs = 0.0;
    progress_.done = next_ >= queue_.size();
    while (next_ < queue_.size()) {
        // The budget is measured, not estimated: a step starts only if this tick has not already spent it. A caller
        // that passes 0 gets one step per tick, which is the "keep the frame short no matter what" setting.
        if (budgetMs > 0.0 && now() - start >= budgetMs) break;
        runOne(budgetMs);
        if (budgetMs <= 0.0) break;
    }
    progress_.lastTickMs = now() - start;
    progress_.pending = queue_.size() - next_;
    progress_.done = next_ >= queue_.size();
    return progress_;
}

LoadProgress BudgetedLoader::runAll() {
    const double start = now();
    while (next_ < queue_.size()) runOne(0.0);
    progress_.lastTickMs = now() - start;
    progress_.pending = 0;
    progress_.done = true;
    return progress_;
}

} // namespace ukiyo
