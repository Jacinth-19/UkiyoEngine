// Budgeted loading (Flax 1.11's async scene loading): the scheduler that decides how much of a scene's load one frame
// pays for. The clock is fake, so a budget is a number this test can hold the loader to rather than a stopwatch reading.
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/async_load.h"

using ukiyo::BudgetedLoader;
using ukiyo::LoadProgress;

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what, const std::string& detail = "") {
    ++g_checks;
    std::printf("  %s %s%s%s\n", ok ? "ok  " : "FAIL", what, detail.empty() ? "" : "  -- ", detail.c_str());
    if (!ok) ++g_failures;
}

// A clock that only moves when a step says it did, so a step's cost is exactly the number written beside it and the
// loader's decisions are reproducible rather than measured.
struct FakeClock {
    double t = 0.0;
    double operator()() const { return t; }
    void advance(double ms) { t += ms; }
};

} // namespace

int main() {
    std::printf("1. An empty loader is done, and says so\n");
    {
        FakeClock clock;
        BudgetedLoader loader([&] { return clock(); });
        check(loader.empty() && loader.fraction() == 1.0, "1a: nothing queued, nothing to do");
        const LoadProgress p = loader.tick(4.0);
        check(p.done && p.stepsRun == 0 && p.stepsTotal == 0, "1b: a tick on an empty queue is a no-op");
        check(loader.fraction() == 1.0, "1c: and it reads as complete");
    }

    std::printf("2. A tick spends its budget and no more\n");
    {
        FakeClock clock;
        BudgetedLoader loader([&] { return clock(); });
        std::vector<int> ran;
        // Each step says it took 1 ms. A 4 ms budget therefore buys four of them, and the fifth waits for the next frame.
        for (int i = 0; i < 10; ++i)
            loader.push("step " + std::to_string(i), [&ran, &clock, i] {
                clock.advance(1.0);
                ran.push_back(i);
            });
        check(loader.progress().stepsTotal == 10, "2a: ten steps queued");

        const LoadProgress first = loader.tick(4.0);
        check(!first.done && first.pending == 6, "2b: the first tick leaves six for the next frame",
              std::to_string(first.pending) + " pending");
        check(first.stepsRun == 4, "2c: and runs exactly the four the budget pays for", std::to_string(first.stepsRun));
        check(!ran.empty() && ran.back() == 3, "2d: in queue order, from the front");
        check(first.lastTickMs >= 4.0 && first.lastTickMs < 5.0, "2e: the tick is measured, not estimated",
              std::to_string(first.lastTickMs) + " ms");
    }

    std::printf("3. The frame after it carries on, and the load finishes\n");
    {
        FakeClock clock;
        BudgetedLoader loader([&] { return clock(); });
        std::vector<int> ran;
        for (int i = 0; i < 12; ++i)
            loader.push("s" + std::to_string(i), [&ran, &clock, i] {
                clock.advance(1.0);
                ran.push_back(i);
            });
        int ticks = 0;
        bool partialSeen = false;
        while (!loader.empty() && ticks < 50) {
            const LoadProgress p = loader.tick(5.0);
            ++ticks;
            if (p.pending > 0) partialSeen = true;
        }
        check(loader.empty(), "3a: the queue drains", "after " + std::to_string(ticks) + " ticks");
        check(ticks == 3, "3b: twelve steps at five a frame take three frames", std::to_string(ticks));
        check(partialSeen, "3c: and at least one tick reported work left over");
        check(ran.size() == 12, "3d: every step ran exactly once", std::to_string(ran.size()));
        bool ordered = true;
        for (size_t i = 0; i < ran.size(); ++i) ordered = ordered && ran[i] == int(i);
        check(ordered, "3e: in the order they were queued");
        check(loader.fraction() == 1.0 && loader.progress().done, "3f: progress ends at 1 and done");
    }

    std::printf("4. A step that throws is reported and does not stop the load\n");
    {
        FakeClock clock;
        BudgetedLoader loader([&] { return clock(); });
        std::vector<std::string> errors;
        loader.setErrorHandler([&](const std::string& name, const std::string& what) {
            errors.push_back(name + ": " + what);
        });
        std::vector<int> ran;
        loader.push("good", [&] { ran.push_back(0); });
        loader.push("bad", [] { throw std::runtime_error("corrupt model"); });
        loader.push("also good", [&] { ran.push_back(2); });
        loader.runAll();
        check(ran.size() == 2, "4a: the steps around the failure ran", std::to_string(ran.size()));
        check(errors.size() == 1 && errors[0] == "bad: corrupt model", "4b: the failure reached the error handler",
              errors.empty() ? "" : errors[0]);
        const LoadProgress p = loader.progress();
        check(p.stepsFailed == 1 && p.stepsRun == 3, "4c: it is counted as failed, and the queue moved past it");
        check(loader.fraction() == 1.0, "4d: a failed step still counts towards progress");
    }

    std::printf("5. A step longer than the whole budget is an overrun, not a silent hitch\n");
    {
        // One step that takes 50 ms inside a 4 ms budget: the model nobody could split, which the caller needs to know
        // about rather than discover as a stutter.
        FakeClock clock;
        BudgetedLoader loader([&] { return clock(); });
        loader.push("slow", [&clock] { clock.advance(50.0); });
        loader.push("next", [] {});
        const LoadProgress p = loader.tick(4.0);
        check(p.overruns == 1, "5a: the overrun is counted", std::to_string(p.overruns));
        check(p.stepsRun == 1, "5b: and the tick stops after it rather than starting another",
              std::to_string(p.stepsRun));
    }

    std::printf("6. Cancelling abandons the rest\n");
    {
        FakeClock clock;
        BudgetedLoader loader([&] { return clock(); });
        int ran = 0;
        for (int i = 0; i < 5; ++i) loader.push("s" + std::to_string(i), [&ran] { ++ran; });
        loader.tick(1.0);
        const int before = ran;
        loader.cancel();
        check(loader.empty() && loader.progress().cancelled, "6a: the queue is empty and marked cancelled");
        loader.tick(100.0);
        check(ran == before, "6b: nothing else ran", std::to_string(ran));
        check(loader.progress().done, "6c: a cancelled load is done, not pending forever");
    }

    std::printf("7. runAll is the synchronous path, unchanged\n");
    {
        FakeClock clock;
        BudgetedLoader loader([&] { return clock(); });
        int ran = 0;
        for (int i = 0; i < 6; ++i) loader.push("s" + std::to_string(i), [&ran] { ++ran; });
        const LoadProgress p = loader.runAll();
        check(ran == 6 && p.done && p.overruns == 0, "7a: every step ran in one call, and none was called an overrun");
        check(loader.fraction() == 1.0, "7b: progress reads complete");
    }

    std::printf("8. Pushing after a tick continues the same load\n");
    {
        FakeClock clock;
        BudgetedLoader loader([&] { return clock(); });
        int ran = 0;
        loader.push("a", [&ran] { ++ran; });
        loader.tick(100.0);
        loader.push("b", [&ran] { ++ran; });
        check(!loader.empty() && loader.progress().stepsTotal == 2, "8a: a later push is part of the same total");
        loader.tick(100.0);
        check(ran == 2 && loader.progress().done, "8b: and runs on the next tick");
    }

    std::printf("\nasync_load_test: %d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
