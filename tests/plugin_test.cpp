// The plugin host, against a real shared library.
//
// A plugin API that is only "tested" with a mock is a plugin API that has never been dlopen'd, and the interesting
// failures - the header that does not compile as C, the symbol that does not export, the string that is freed before
// the plugin reads it - all live exactly there. So this test loads two libraries built by the C compiler:
//
//   * plugin_sample, which registers two commands and one importer, and writes a file when they run;
//   * plugin_refuse, which registers a command and then fails, so the host has to take the command back.
//
// CMake builds both and passes their paths in on the command line.
#include "engine/plugin_host.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int checks = 0;
int failures = 0;

void check(bool ok, const char* what, const std::string& detail = std::string()) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s%s%s\n", what, detail.empty() ? "" : " -- ", detail.c_str());
    }
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

int main(int argc, char** argv) {
    // The paths come from CMake, so CI can run the executable by name the way it runs every other test. An argument
    // overrides them, which is what makes it possible to point the test at a plugin built by hand.
    const char* sampleArg = argc > 1 ? argv[1] : UKIYO_PLUGIN_SAMPLE;
    const char* refuseArg = argc > 2 ? argv[2] : UKIYO_PLUGIN_REFUSE;
    const fs::path sample = sampleArg;
    const fs::path refuse = refuseArg;
    if (!fs::exists(sample) || !fs::exists(refuse)) {
        std::printf("plugin_test: a plugin library is missing\n");
        return 2;
    }

    const fs::path out = fs::temp_directory_path() / "ukiyo_plugin_test_out.txt";
    std::error_code ec;
    fs::remove(out, ec);
    // The sample reads this; it is how the test gets a path into a library that has no other way to be told one.
#if defined(_WIN32)
    _putenv_s("UKIYO_PLUGIN_SAMPLE_OUT", out.string().c_str());
#else
    setenv("UKIYO_PLUGIN_SAMPLE_OUT", out.string().c_str(), 1);
#endif

    std::vector<std::string> logs;
    std::string statusSeen;
    ukiyo::PluginHostCallbacks cb;
    cb.log = [&](const std::string& line) { logs.push_back(line); };
    cb.status = [&](const std::string& line, int) { statusSeen = line; };
    cb.projectRoot = [] { return std::string("/tmp/a-project"); };
    cb.scenePath = [] { return std::string("Scenes/Main.ukscene"); };

    ukiyo::PluginHost host(cb);
    check(host.loadedCount() == 0 && host.commands().empty() && host.importers().empty(),
          "a fresh host has no plugins, commands or importers");

    // --- A library that loads --------------------------------------------------------------------------
    check(host.loadFile(sample.string()), "the sample plugin loads");
    check(host.loadedCount() == 1, "and is counted once");
    const std::vector<ukiyo::PluginReport>& reports = host.reports();
    check(reports.size() == 1 && reports[0].ok, "the report says it loaded");
    check(reports[0].name == "sample" && reports[0].version == "1.2.3",
          "the report carries the name and version the plugin gave", reports[0].name + " " + reports[0].version);

    check(host.commands().size() == 2, "both of its commands are registered",
          std::to_string(host.commands().size()));
    if (host.commands().size() == 2) {
        check(host.commands()[0].id == sample.string() + "/hello",
              "a command id is the plugin and the command, so two plugins can both have a 'hello'",
              host.commands()[0].id);
        check(host.commands()[0].label == "Sample: say hello", "the label is the plugin's own wording",
              host.commands()[0].label);
        check(host.commands()[1].id != host.commands()[0].id, "two commands have two ids");
    }
    check(host.importers().size() == 1 && host.importers()[0].extension == "txt",
          "its importer claims .txt, lower cased from the TXT it asked for",
          host.importers().empty() ? "none" : host.importers()[0].extension);

    // --- Running what it registered --------------------------------------------------------------------
    check(!host.runCommand("nobody/nothing"), "running an unknown command fails rather than doing nothing quietly");
    check(host.runCommand(sample.string() + "/hello"), "the command runs");
    const std::string first = readAll(out);
    check(first.find("command ran") != std::string::npos, "and reached the plugin's own code", first);
    check(first.find("root=/tmp/a-project") != std::string::npos,
          "the project root reached the plugin while the command ran", first);
    check(!logs.empty() && logs.back().find("sample plugin: hello") != std::string::npos,
          "a message the plugin logged arrived at the editor", logs.empty() ? "no logs" : logs.back());

    check(host.importFile("/tmp/a-project/assets/thing.txt"), "a .txt file is handed to the plugin that claimed it");
    const std::string second = readAll(out);
    check(second.find("import=/tmp/a-project/assets/thing.txt") != std::string::npos,
          "with the path it was given, unchanged", second);
    check(!host.importFile("/tmp/a-project/assets/thing.glb"),
          "a file no plugin claims is not imported by whoever happens to be first");
    check(!host.importFile("/tmp/a-project/assets/noextension"),
          "and neither is a file with no extension at all");

    // --- A library that fails --------------------------------------------------------------------------
    check(!host.loadFile(refuse.string()), "a plugin that fails to load is refused");
    check(host.loadedCount() == 1, "and is not counted");
    check(host.reports().size() == 2 && !host.reports()[1].ok && !host.reports()[1].error.empty(),
          "the failure is reported with a reason");
    bool leaked = false;
    for (const ukiyo::PluginCommand& c : host.commands()) leaked = leaked || c.label == "Should never appear";
    check(!leaked, "the command it registered before failing is taken back");

    // --- The same file twice, and unloading -------------------------------------------------------------
    check(host.loadFile(sample.string()), "loading the same library again succeeds");
    check(host.commands().size() == 4, "and registers its commands again", std::to_string(host.commands().size()));
    host.unloadAll();
    check(host.loadedCount() == 0 && host.commands().empty() && host.importers().empty(),
          "unloading drops every command and importer, so nothing points into a closed library");
    check(!host.runCommand(sample.string() + "/hello"), "and a command cannot be run after its library is gone");

    // A directory with both libraries loads the good one and reports the bad one.
    const fs::path dir = fs::temp_directory_path() / "ukiyo_plugin_test_dir";
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    fs::copy_file(sample, dir / ("a" + ukiyo::PluginHost::pluginExtension()), ec);
    fs::copy_file(refuse, dir / ("b" + ukiyo::PluginHost::pluginExtension()), ec);
    fs::copy_file(sample, dir / "notes.txt", ec);   // not a plugin, and must be ignored
    const int loaded = host.loadDirectory(dir.string());
    check(loaded == 1, "a directory load takes the plugin and leaves the failure behind",
          std::to_string(loaded));
    check(host.reports().size() == 2, "both files are reported", std::to_string(host.reports().size()));
    // Sorted by name: a... before b..., so the good one is the good one.
    check(host.reports().size() == 2 && host.reports()[0].ok && !host.reports()[1].ok,
          "in name order, so a plugin's commands come in a predictable order");
    host.unloadAll();
    check(host.commands().empty() && host.importers().empty(), "and a directory unload is clean");

    fs::remove_all(dir, ec);
    fs::remove(out, ec);

    std::printf("plugin_test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
