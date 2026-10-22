// Loading with a frame budget (Flax 1.11's async scene loading).
//
// Opening a scene is a pile of independent pieces of work — one model file, one texture, one prefab expansion — and the
// naive loop runs all of them before the next frame, so a scene with a dozen models freezes for as long as the slowest
// pile takes. The fix is not a thread: most of this engine's loading is file parsing and buffer creation, and doing it
// on a worker thread means the GPU upload still happens on the main one. The fix is to *stop early*: run pieces until
// the frame's budget is spent, draw a frame, and carry on next frame.
//
// This is that scheduler, small enough to test with a fake clock:
//
//   * a piece of work is a `Step`, run whole or not at all. Nothing is cut off half way, so a loader that runs out of
//     budget never leaves a half-built model behind;
//   * `tick(budgetMs)` starts a step only while less than the budget has been measured since the tick began, so a frame
//     pays the budget (plus at most one step) and no more;
//   * a step that runs longer than the whole budget is not a silent hitch: it is counted in `overruns()`, which is the
//     number a caller reports when it wants to know that the budget is a fiction;
//   * a step that throws is reported to the caller's error handler and does not stop the queue, because one corrupt
//     model should not cost the player the rest of the scene.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace ukiyo {

// What one tick did, and what is left.
struct LoadProgress {
    size_t stepsRun = 0;      // steps this loader has run, ever
    size_t stepsTotal = 0;    // steps queued when the load began plus any queued since
    size_t stepsFailed = 0;   // steps that threw
    size_t overruns = 0;      // steps that ran longer than the whole budget
    size_t pending = 0;       // steps still queued
    double lastTickMs = 0.0;  // the measured cost of the last tick
    bool done = false;        // nothing is left
    bool cancelled = false;
};

class BudgetedLoader {
public:
    // One unit of work. `name` is for the caller's log and for the error handler.
    using Step = std::function<void()>;
    // Called with the step's name and the text of the exception. Optional.
    using ErrorHandler = std::function<void(const std::string& name, const std::string& what)>;

    // A clock in milliseconds, monotonic. Defaults to steady_clock; the tests pass a fake one so a budget is a number
    // rather than a hope.
    using Clock = std::function<double()>;
    BudgetedLoader() = default;
    explicit BudgetedLoader(Clock clock) : clock_(std::move(clock)) {}

    void setErrorHandler(ErrorHandler handler) { onError_ = std::move(handler); }

    // Queues one step. Steps run in the order they were queued, which is the order a scene's instances are written in.
    void push(std::string name, Step step);
    // Queues nothing and forgets everything that was queued. Steps already run stay run — this is for "the scene
    // changed while it was loading", not for undo.
    void cancel();

    // Runs steps until the budget is spent or the queue is empty. Returns `progress()`.
    LoadProgress tick(double budgetMs);
    // Runs everything, ignoring the budget. The synchronous path the engine used before this existed, and what a
    // caller that has already shown a loading screen can use.
    LoadProgress runAll();

    const LoadProgress& progress() const { return progress_; }
    bool empty() const { return next_ >= queue_.size(); }
    // 0 at the start, 1 when the queue is empty. Monotone through a load, even while steps fail.
    double fraction() const;
    // True when the loader ran out of budget with work left, which is what a caller draws a bar for.
    bool partial() const { return !queue_.empty() && progress_.stepsRun > 0; }

private:
    struct Entry {
        std::string name;
        Step step;
    };

    // Runs one queued step, timing it and swallowing its exception into the error handler.
    void runOne(double budgetMs);
    double now() const;

    std::vector<Entry> queue_;
    size_t next_ = 0;   // index into queue_ of the next step to run
    Clock clock_;
    ErrorHandler onError_;
    LoadProgress progress_;
    std::vector<std::string> names_;   // for `current()`, if a caller ever wants it
};

} // namespace ukiyo
