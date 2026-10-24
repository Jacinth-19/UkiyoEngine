// The figure in docs/ASYNC_LOADING.md: what a frame costs while a scene loads, with and without a budget.
//
// The model is the shipping scheduler (src/engine/async_load.cpp) driven with a fake clock and a step cost drawn from a
// fixed table, so the picture is deterministic and the numbers in its labels are the ones the run produced. Each frame
// is one bar: its height is how many milliseconds of loading that frame paid. Without a budget the first frame pays for
// the whole scene and every frame after it pays nothing; with one, every frame pays about the same and the scene arrives
// a few frames later. That trade is the entire feature.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/async_load.h"
#include "figure_kit.h"

namespace {

// The steps of a scene: one model each, in the order a scene file lists them, with the milliseconds that model takes.
const double kStepMs[] = {12.0, 3.0, 7.0, 21.0, 2.0, 5.0, 9.0, 4.0, 15.0, 6.0, 3.0, 8.0};
constexpr int kSteps = int(sizeof(kStepMs) / sizeof(kStepMs[0]));

struct Run {
    std::vector<double> frameMs;   // what each frame paid
    double totalMs = 0.0;
    size_t overruns = 0;
};

// Runs the loader to completion, one tick per frame, recording what each frame paid. A budget of 0 asks for the
// synchronous path (runAll), which is what the engine did before this feature: one frame pays for everything.
Run run(double budgetMs) {
    double clock = 0.0;
    ukiyo::BudgetedLoader loader([&clock] { return clock; });
    Run out;
    for (int i = 0; i < kSteps; ++i)
        loader.push("model " + std::to_string(i), [&clock, i] { clock += kStepMs[i]; });
    if (budgetMs <= 0.0) {
        const double before = clock;
        const ukiyo::LoadProgress p = loader.runAll();
        out.frameMs.push_back(clock - before);
        out.overruns = p.overruns;
        out.totalMs = clock;
        return out;
    }
    int guard = 0;
    while (!loader.empty() && guard++ < 1000) {
        const double before = clock;
        const ukiyo::LoadProgress p = loader.tick(budgetMs);
        out.frameMs.push_back(clock - before);
        out.overruns = p.overruns;
    }
    out.totalMs = clock;
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::string out = "docs/async-loading.png";
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--out") out = argv[i + 1];

    const Run immediate = run(0.0);   // runAll: the whole scene before the first frame
    const Run budgeted = run(4.0);    // 4 ms a frame

    const int W = 1600, H = 900;
    figure::Image sheet;
    sheet.reset(W, H, glm::vec3(0.045f, 0.05f, 0.06f));
    figure::panelHeader(sheet, 30, 18, "NO BUDGET", "RUN-ALL: THE WHOLE SCENE, ONE FRAME");
    figure::panelHeader(sheet, 820, 18, "4 MS A FRAME", "TICKED: ONE STEP AT A TIME");

    const int graphX = 90, graphY = 130, graphW = 620, graphH = 520;
    const double maxMs = std::max(immediate.frameMs.front(), 8.0);
    auto drawRun = [&](const Run& r, int x0, glm::vec3 tone) {
        // Axes.
        figure::fillRect(sheet, x0, graphY, graphW, 1, glm::vec3(0.22f, 0.25f, 0.30f), 1.0f);
        figure::fillRect(sheet, x0, graphY + graphH, graphW, 1, glm::vec3(0.22f, 0.25f, 0.30f), 1.0f);
        figure::fillRect(sheet, x0, graphY, 1, graphH, glm::vec3(0.22f, 0.25f, 0.30f), 1.0f);
        const int n = int(r.frameMs.size());
        const float barW = std::min(46.0f, float(graphW) / std::max(n, 1) * 0.7f);
        const float gap = float(graphW) / float(std::max(n, 1));
        for (int i = 0; i < n; ++i) {
            const double ms = r.frameMs[size_t(i)];
            const float h = float(graphH * ms / maxMs);
            const float bx = float(x0) + gap * float(i) + (gap - barW) * 0.5f;
            const float by = float(graphY + graphH) - std::min(std::max(h, 1.0f), float(graphH));
            const int barH = std::min(int(std::max(h, 1.0f)), graphH);
            figure::fillRect(sheet, int(bx), graphY + graphH - barH, int(barW), barH, tone, 1.0f);
            // The frame's own number, above the bar, when there is room for it.
            if (h > 18.0f) {
                char label[32];
                std::snprintf(label, sizeof label, "%.0f", ms);
                figure::drawText(sheet, int(bx) + 3, int(by) - 14, label, 1, glm::vec3(0.75f, 0.80f, 0.87f));
            }
        }
        char label[96];
        std::snprintf(label, sizeof label, "%d FRAME%s, %.0f MS OF LOADING", n, n == 1 ? "" : "S", r.totalMs);
        figure::drawText(sheet, x0, graphY + graphH + 18, label, 2, glm::vec3(0.62f, 0.68f, 0.76f));
    };
    drawRun(immediate, graphX, glm::vec3(0.78f, 0.36f, 0.30f));
    drawRun(budgeted, 840, glm::vec3(0.35f, 0.72f, 0.55f));

    // The vertical axis, in milliseconds, on the left of each graph.
    for (int i = 0; i <= 4; ++i) {
        const double v = maxMs * double(i) / 4.0;
        const int y = graphY + graphH - int(double(graphH) * double(i) / 4.0);
        char label[32];
        std::snprintf(label, sizeof label, "%.0f", v);
        figure::drawText(sheet, graphX - 34, y - 4, label, 1, glm::vec3(0.42f, 0.47f, 0.54f));
    }
    figure::drawText(sheet, 30, graphY + 12, "MS", 1, glm::vec3(0.42f, 0.47f, 0.54f));
    figure::drawText(sheet, 780, graphY + 12, "MS", 1, glm::vec3(0.42f, 0.47f, 0.54f));

    // The numbers, spelled out.
    const double worstBudgeted = *std::max_element(budgeted.frameMs.begin(), budgeted.frameMs.end());
    const double firstImmediate = immediate.frameMs.front();
    char line[192];
    std::snprintf(line, sizeof line, "WORST FRAME %.0f MS WITHOUT A BUDGET, %.0f MS WITH ONE", firstImmediate, worstBudgeted);
    figure::drawText(sheet, 90, 790, line, 2, glm::vec3(0.80f, 0.85f, 0.92f));
    std::snprintf(line, sizeof line, "THE SCENE ARRIVES IN %d FRAMES RATHER THAN 1. A FRAME PAYS ONE MODEL AND STOPS.",
                  int(budgeted.frameMs.size()));
    figure::drawText(sheet, 90, 816, line, 2, glm::vec3(0.55f, 0.61f, 0.69f));
    std::snprintf(line, sizeof line, "%zu OF %d STEPS COST MORE THAN THE BUDGET - COUNTED, NOT HIDDEN",
                  budgeted.overruns, kSteps);
    figure::drawText(sheet, 90, 842, line, 2, glm::vec3(0.55f, 0.61f, 0.69f));

    if (!figure::savePng(sheet, out)) {
        std::fprintf(stderr, "async_load_preview: could not write %s\n", out.c_str());
        return 1;
    }
    std::printf("async_load_preview: immediate %.0f ms in %d frame(s), budgeted %.0f ms over %d frames, worst %.0f ms\n",
                firstImmediate, int(immediate.frameMs.size()), budgeted.totalMs, int(budgeted.frameMs.size()), worstBudgeted);
    return 0;
}
