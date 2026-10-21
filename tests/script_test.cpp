// Tests for the Lua scripting system: the sandbox, require(), error reporting, the per-entity runtime, and the files a
// project needs for an external editor to understand it.
//
// No device and no window: everything here is the engine's CPU code plus Lua 5.4.

#include "engine/script_project.h"
#include "systems/script.h"
#include "systems/script_runtime.h"
#include "systems/world.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <csignal>
#include <windows.h>
#endif

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

using namespace ukiyo;

namespace fs = std::filesystem;

static int failures = 0;
static int checks = 0;

// Every check's name as it runs, off by default. A crash leaves no summary, and a CI runner writes a redirected
// stdout in blocks, so the trace is flushed on every line: the last line it wrote is the check that killed it.
static bool trace = false;
static std::ofstream traceLog;   // UKIYO_TEST_LOG: the same trace in a file, flushed line by line
static const char* lastSection = "start";   // named in the crash line below

#ifdef _WIN32
// A Windows test that dies leaves an exit code and an empty log: the pipe loses what the process wrote, and a checked
// stack frame or an abort reports itself as 0xC0000409 with no other word. These handlers write the reason into the
// trace file with plain Win32 calls -- no C++ runtime, no buffers -- so the file says "CRASH abort" or "CRASH exception
// 0xc0000005 at script_test.exe+0x1f3a" instead of a number to guess at. Nothing here runs on Linux, where a crash
// already leaves a signal and a core.
static void crashLine(const char* what) {
    char line[512];
    std::snprintf(line, sizeof line, "CRASH %s in section '%s'\n", what, lastSection);
    if (const char* path = std::getenv("UKIYO_TEST_LOG")) {
        HANDLE file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
            FlushFileBuffers(file);
            CloseHandle(file);
        }
    }
    std::fprintf(stderr, "%s", line);
    std::fflush(stderr);
}

// "script_test.exe+0x1234": a module and an offset, which a linker map turns back into a function name.
static void describeAddress(char* out, size_t size, void* address) {
    char module[MAX_PATH] = "?";
    HMODULE owner = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &owner))
        GetModuleFileNameA(owner, module, MAX_PATH);
    const char* name = std::strrchr(module, '\\');
    std::snprintf(out, size, "%s+0x%llx", name ? name + 1 : module,
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                                  reinterpret_cast<uintptr_t>(owner)));
}

static void crashAccessViolation(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    char where[320];
    describeAddress(where, sizeof where, record->ExceptionAddress);
    char what[512];
    std::snprintf(what, sizeof what, "exception 0x%08lx at %s", static_cast<unsigned long>(record->ExceptionCode), where);
    crashLine(what);
#if defined(_M_X64)
    // Return addresses off the faulting stack, each read only after the page it lives on has said it is readable, and
    // resolved to module+offset: a map file names the functions.
    const uintptr_t rsp = static_cast<uintptr_t>(info->ContextRecord->Rsp);
    for (int i = 0, shown = 0; i < 512 && shown < 16; ++i) {
        const uintptr_t slot = rsp + static_cast<uintptr_t>(i) * 8;
        MEMORY_BASIC_INFORMATION page{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(slot), &page, sizeof page) != sizeof page) break;
        if (page.State != MEM_COMMIT || (page.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) break;
        const uintptr_t candidate = *reinterpret_cast<const uintptr_t*>(slot);
        MEMORY_BASIC_INFORMATION code{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(candidate), &code, sizeof code) != sizeof code) continue;
        if (code.State != MEM_COMMIT ||
            (code.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) == 0)
            continue;
        char frame[384];
        char line[420];
        describeAddress(frame, sizeof frame, reinterpret_cast<void*>(candidate));
        std::snprintf(line, sizeof line, "frame %d: %s", i, frame);
        crashLine(line);
        ++shown;
    }
#endif
}

static LONG WINAPI crashFilter(EXCEPTION_POINTERS* info) {
    crashAccessViolation(info);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void invalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    crashLine("invalid CRT parameter");
    std::_Exit(70);
}

static void terminateHandler() {
    crashLine("unhandled C++ exception (std::terminate)");
    std::_Exit(71);
}

static void abortHandler(int) {
    crashLine("abort (Lua's panic, a failed assert, or a CRT report)");
    std::_Exit(72);
}

// Handlers first, then the state: a failure after this point is written down even when the log pipe is lost.
static void installCrashReporters() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crashing process never flushes a block buffer
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);   // abort() raises SIGABRT instead of fast-failing
    std::signal(SIGABRT, abortHandler);
    _set_invalid_parameter_handler(invalidParameterHandler);
    std::set_terminate(terminateHandler);
    AddVectoredExceptionHandler(1, crashFilter);
}
#endif

// Thrown by --max, to end a run where CI asked it to: unwinding runs the destructors, which close the Lua states and
// let the heap be checked, instead of _Exit, which would leave the corruption undiscovered.
struct StopEarly {};

static int maxChecks = 0;   // --max: stop after this many checks

// --- a checked heap for this test --------------------------------------------------------------------------------
// The scripting test corrupted the Windows heap once, somewhere, and the heap said so a section later: a wild write into
// a live block is the one thing an address sanitiser cannot see, because the address it lands on is valid. Every block
// this process asks for -- through C++ new, or through the allocator Lua is handed -- gets a canary on both sides and an
// entry in a fixed table, and the test checks every live block after every check. The canary that is broken names the
// block and the check, instead of leaving a heap-corruption exit code from three sections away.
namespace checked {

constexpr size_t kGuard = 16;
constexpr size_t kMarkSize = 8;
constexpr int kSlots = 1 << 17;   // a hashed table, never resized: it allocates nothing itself
constexpr unsigned char kMark[kMarkSize] = {0xA5, 0x5A, 0xC3, 0x3C, 0x0F, 0xF0, 0x96, 0x69};

struct Entry {
    void* ptr = nullptr;
    size_t size = 0;
};

Entry table[kSlots];
long long allocated = 0;
long long reported = 0;
// The last few pointers given back: enough to tell a double free from a free of something this heap never handed out,
// which are the two ways a block leaves here without a broken canary to explain it.
void* recentlyFreed[64] = {};
int recentlyFreedNext = 0;

unsigned slotOf(void* ptr) { return static_cast<unsigned>((reinterpret_cast<uintptr_t>(ptr) >> 4) & (kSlots - 1)); }

Entry* findEntry(void* ptr) {
    const unsigned start = slotOf(ptr);
    for (unsigned i = 0; i < 64; ++i) {
        Entry& e = table[(start + i) & (kSlots - 1)];
        if (e.ptr == ptr) return &e;
        if (!e.ptr) return nullptr;
    }
    return nullptr;
}

bool intact(void* ptr, size_t size) {
    const unsigned char* raw = static_cast<const unsigned char*>(ptr);
    return std::memcmp(raw - kGuard, kMark, kMarkSize) == 0 &&
           std::memcmp(raw + size, kMark, kMarkSize) == 0;
}

void insert(void* ptr, size_t size) {
    const unsigned start = slotOf(ptr);
    for (unsigned i = 0; i < 64; ++i) {
        Entry& e = table[(start + i) & (kSlots - 1)];
        if (!e.ptr) {
            e.ptr = ptr;
            e.size = size;
            return;
        }
    }
}

void erase(void* ptr) {
    Entry* e = findEntry(ptr);
    if (e) e->ptr = nullptr;
}

void* allocate(size_t size) {
    unsigned char* raw = static_cast<unsigned char*>(std::malloc(size + 2 * kGuard));
    if (!raw) throw std::bad_alloc();
    std::memcpy(raw, kMark, kMarkSize);
    std::memcpy(raw + kGuard + size, kMark, kMarkSize);
    insert(raw + kGuard, size);
    ++allocated;
    return raw + kGuard;
}

void note(const char* what, size_t size, const void* where) {
    if (reported >= 8) return;
    char line[288];
    std::snprintf(line, sizeof line, "CRASH %s at check %d, section '%s': block of %zu bytes at %p", what, checks,
                  lastSection, size, where);
    std::printf("%s\n", line);
    if (traceLog) {
        traceLog << line << "\n";
        traceLog.flush();
    }
    ++reported;
}

void release(void* ptr) {
    if (!ptr) return;
    Entry* e = findEntry(ptr);
    if (!e) {
        // Either a library freed something it allocated inside its own DLL, or this heap is being handed a pointer it
        // never handed out -- a double free, or a pointer that was never a block.
        bool seen = false;
        for (void* freed : recentlyFreed) seen = seen || freed == ptr;
        note(seen ? "double free" : "free of an unknown pointer", 0, ptr);
        if (seen) return;   // do not hand a double free to the CRT: it would report corruption somewhere else
        std::free(ptr);
        return;
    }
    if (!intact(ptr, e->size)) {
        // A write past the end of this block happened. Say which block and how big, once per block.
        note("heap canary broken", e->size, ptr);
    }
    erase(ptr);
    recentlyFreed[recentlyFreedNext] = ptr;
    recentlyFreedNext = (recentlyFreedNext + 1) % 64;
    std::free(static_cast<unsigned char*>(ptr) - kGuard);
}

// Every live block, after every check: the first broken canary is the write that damaged the heap.
void checkAll() {
    if (reported >= 8) return;
    for (int i = 0; i < kSlots; ++i) {
        Entry& e = table[i];
        if (!e.ptr || intact(e.ptr, e.size)) continue;
        note("heap canary broken", e.size, e.ptr);
        return;
    }
}

// Lua's allocator, with the same canaries: a realloc is a copy, so every block stays checked.
void* luaAllocator(void*, void* ptr, size_t osize, size_t nsize) {
    if (nsize == 0) {
        release(ptr);
        return nullptr;
    }
    void* fresh = allocate(nsize);
    if (ptr) {
        std::memcpy(fresh, ptr, osize < nsize ? osize : nsize);
        release(ptr);
    }
    return fresh;
}

} // namespace checked

void* operator new(std::size_t size) { return checked::allocate(size); }
void* operator new[](std::size_t size) { return checked::allocate(size); }
void operator delete(void* ptr) noexcept { checked::release(ptr); }
void operator delete[](void* ptr) noexcept { checked::release(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { checked::release(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { checked::release(ptr); }

static void check(bool ok, const std::string& what, const std::string& detail = {}) {
    ++checks;
    if (maxChecks > 0 && checks >= maxChecks) {
        if (!ok) {
            ++failures;
            std::printf("FAIL  %s\n", what.c_str());
        }
        std::fflush(stdout);
        checked::checkAll();
        if (traceLog) {
            traceLog << "MAX " << checks << "\n";
            traceLog.flush();
        }
        throw StopEarly{};
    }
    if (ok) {
        if (trace) {
            std::printf("PASS  #%d %s\n", checks, what.c_str());
            std::fflush(stdout);
            if (traceLog) {
                traceLog << "PASS  #" << checks << " " << what << "\n";
                traceLog.flush();
            }
        }
        return;
    }
    ++failures;
    std::printf("FAIL  %s", what.c_str());
    if (!detail.empty()) std::printf("  -- %s", detail.c_str());
    std::printf("\n");
    std::fflush(stdout);
}

// Printed before each section and flushed, so a crash in CI still says where it happened: a redirected stdout is
// block-buffered, and a crashing process never flushes it.
static void section(const char* name) {
    lastSection = name;
    std::printf("== %s\n", name);
    std::fflush(stdout);
    if (traceLog) {
        traceLog << "== " << name << "\n";
        traceLog.flush();
    }
}


static fs::path testRoot() {
    static const fs::path root = fs::temp_directory_path() / "ukiyo_script_test";
    return root;
}

static std::string pathOf(const std::string& rel) { return (testRoot() / rel).string(); }

static void makeTree() {
    std::error_code ec;
    fs::remove_all(testRoot(), ec);
    fs::create_directories(testRoot() / "mods" / "sub", ec);
}

static bool writeFile(const std::string& rel, const std::string& text) {
    const fs::path p = testRoot() / rel;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    return static_cast<bool>(out);
}

// --- the sandbox --------------------------------------------------------------------------------------------

static void testSandbox() {
    const char* probe =
        "assert(io == nil, 'io is open')\n"
        "assert(os == nil, 'os is open')\n"
        "assert(package == nil, 'package is open')\n"
        "assert(debug == nil, 'debug is open')\n"
        "assert(dofile == nil and loadfile == nil and load == nil, 'a file loader is open')\n";
    {
        ScriptHost h;
        std::string err;
        check(h.loadString(probe, "@probe.lua", &err), "sandbox: no io, os, package, debug or file loaders", err);
    }
    const char* safe =
        "assert(type(string) == 'table' and type(table) == 'table' and type(math) == 'table')\n"
        "assert(type(utf8) == 'table' and type(coroutine) == 'table')\n"
        "assert(type(require) == 'function', 'require is not installed')\n";
    ScriptHost h;
    std::string err;
    check(h.loadString(safe, "@safe.lua", &err), "sandbox: the pure libraries and require() are there", err);
}

// --- errors -------------------------------------------------------------------------------------------------

static void testErrors() {
    ScriptHost h;
    h.setEntityName("hero");
    std::string err;
    check(h.loadString("function onUpdate(dt)\n  local t = nil\n  return t.x\nend\n", "@hero.lua", &err),
          "errors: a script with a runtime fault loads", err);
    h.callUpdate(0.016f);
    check(h.error().file == "hero.lua", "errors: the file is the chunk's", h.error().file);
    check(h.error().line == 3, "errors: the line is the line that failed", std::to_string(h.error().line));
    check(h.error().message.find("nil value") != std::string::npos, "errors: the message says what went wrong",
          h.error().message);
    check(h.error().traceback.find("stack traceback") != std::string::npos ||
              h.error().traceback.find("onUpdate") != std::string::npos,
          "errors: a traceback comes with it", h.error().traceback);
    check(h.lastError().find("hero.lua:3:") == 0, "errors: lastError reads file:line", h.lastError());

    // The budget: three failures in a row stop the script, and a success in between resets the count.
    section("errors: budget");
    ScriptHost once;
    section("errors: budget, second host created");
    std::string e2;
    // Compiled and run as two steps on purpose: the test says which half a crash is in.
    const bool doomedCompiles = once.compileString("function onUpdate(dt) error('boom') end", "@boom.lua", &e2);
    section("errors: budget, doomed script compiled");
    check(doomedCompiles, "errors: the doomed script compiles", e2);
    const bool doomedLoaded = once.loadString("function onUpdate(dt) error('boom') end", "@boom.lua", &e2);
    section("errors: budget, doomed script loaded");
    check(doomedLoaded, "errors: the doomed script loads", e2);
    for (int i = 0; i < ScriptHost::kMaxErrors; ++i) {
        once.callUpdate(0.016f);
        section("errors: budget, one call");
    }
    check(once.hasFailed(), "errors: three failures in a row stop the script");
    check(once.errorCount() == ScriptHost::kMaxErrors, "errors: and it counted them",
          std::to_string(once.errorCount()));
    once.callUpdate(0.016f);
    check(once.errorCount() == ScriptHost::kMaxErrors, "errors: a stopped script is not called again");

    section("errors: alternating");
    ScriptHost alternating;
    std::string e3;
    check(alternating.loadString("n = 0\nfunction onUpdate(dt) n = n + 1 if n % 2 == 1 then error('odd') end end",
                                 "@odd.lua", &e3),
          "errors: the alternating script loads", e3);
    for (int i = 0; i < 5; ++i) alternating.callUpdate(0.016f);
    check(!alternating.hasFailed() && alternating.errorCount() == 3,
          "errors: a call that succeeds resets the budget", std::to_string(alternating.errorCount()));

    // A compile error is a check, not a run: it names the file and line and executes nothing.
    section("errors: compile");
    ScriptHost compile;
    std::string bad = "local = = 1\n";
    check(!compile.compileString(bad, "@bad.lua", &err), "errors: a syntax error is reported");
    check(err.find("bad.lua:1:") != std::string::npos, "errors: the syntax error names the file and line", err);
    check(!compile.loaded(), "errors: a script that did not compile is not loaded");
}

// --- require ------------------------------------------------------------------------------------------------

static void testRequire() {
    makeTree();
    check(writeFile("mods/counter.lua", "COUNT = (COUNT or 0) + 1\nreturn { value = 42, count = COUNT }\n"),
          "require: the test module is written");
    check(writeFile("mods/sub/leaf.lua", "return { name = 'leaf' }\n"), "require: the nested module is written");
    check(writeFile("mods/cycle_a.lua", "local b = require('cycle_b')\nreturn { name = 'a' }\n"),
          "require: the cycle's first half is written");
    check(writeFile("mods/cycle_b.lua", "local a = require('cycle_a')\nreturn { name = 'b' }\n"),
          "require: the cycle's second half is written");

    ScriptHost h;
    h.setSearchPaths({(testRoot() / "mods").string()});
    std::string err;
    const char* code =
        "local a = require('counter')\n"
        "local b = require('counter')\n"
        "assert(a == b, 'a module is loaded twice')\n"
        "assert(COUNT == 1, 'the module body ran twice')\n"
        "local leaf = require('sub.leaf')\n"
        "assert(leaf.name == 'leaf', 'a dotted name did not become a path')\n";
    check(h.loadString(code, "@uses_modules.lua", &err), "require: a module loads once and a dotted name resolves", err);

    ScriptHost cycle;
    cycle.setSearchPaths({(testRoot() / "mods").string()});
    std::string e2;
    check(cycle.loadString("local a = require('cycle_a')\nassert(a.name == 'a')\n", "@cycle.lua", &e2),
          "require: a cycle is answered with the half-built module, not a hang", e2);

    ScriptHost escapes;
    escapes.setSearchPaths({(testRoot() / "mods").string()});
    std::string e3;
    check(escapes.loadString("local ok = pcall(require, '../outside')\nassert(not ok, 'a traversal was allowed')\n"
                             "local ok2 = pcall(require, '/etc/passwd')\nassert(not ok2, 'an absolute path was allowed')\n",
                             "@escape.lua", &e3),
          "require: .. and an absolute path are refused", e3);

    ScriptHost missing;
    missing.setSearchPaths({(testRoot() / "mods").string()});
    missing.loadString("function onStart() require('nope') end", "@missing.lua", &e3);
    missing.callStart();
    check(missing.error().message.find("nope") != std::string::npos,
          "require: a missing module says which one", missing.error().message);

    ScriptHost noroots;
    std::string e4;
    noroots.loadString("function onStart() require('counter') end", "@noroots.lua", &e4);
    noroots.callStart();
    check(!noroots.error().empty() && noroots.error().message.find("no script folders") != std::string::npos,
          "require: a host with no roots says so", noroots.error().message);
}

// --- the runtime ----------------------------------------------------------------------------------------------

static std::vector<std::string> gCalls;

static int l_record(lua_State* L) {
    gCalls.push_back(luaL_checkstring(L, 1));
    return 0;
}

static int l_self(lua_State* L) {
    lua_getglobal(L, "self");
    gCalls.push_back(std::string("self:") + (lua_isstring(L, -1) ? lua_tostring(L, -1) : "?"));
    return 0;
}

static void testRuntime() {
    makeTree();
    writeFile("hero.lua",
              "function onStart() ukiyo.record('start') ukiyo.self_name() end\n"
              "function onUpdate(dt) ukiyo.record('update') end\n"
              "function onContact(c) ukiyo.record('contact:' .. c.kind) end\n"
              "function onDestroy() ukiyo.record('destroy') end\n");
    writeFile("enemy.lua", "function onStart() ukiyo.record('start') end\nfunction onUpdate(dt) ukiyo.record('update') end\n");
    gCalls.clear();

    ScriptRuntime rt;
    rt.onHostCreate([](ScriptHost& h) {
        h.registerFunction("record", l_record, nullptr);
        h.registerFunction("self_name", l_self, nullptr);
    });
    std::string err;
    check(rt.add("hero", pathOf("hero.lua"), &err), "runtime: the hero's script loads", err);
    check(rt.add("enemy", pathOf("enemy.lua"), &err), "runtime: the enemy's script loads too", err);
    check(rt.size() == 2, "runtime: two scripts are running", std::to_string(rt.size()));

    rt.callStart();
    check(gCalls.size() == 3 && gCalls[0] == "start" && gCalls[1] == "self:hero" && gCalls[2] == "start",
          "runtime: onStart runs for each script, in the order they were added, and self is the entity",
          gCalls.size() > 1 ? gCalls[1] : std::string());
    gCalls.clear();
    rt.callUpdate(0.016f);
    check(gCalls == std::vector<std::string>{"update", "update"}, "runtime: onUpdate runs for each script");

    const std::vector<std::string> contacts = rt.entitiesWith("onContact");
    check(contacts.size() == 1 && contacts[0] == "hero", "runtime: only the script that defines onContact is listed");

    const std::vector<ScriptInfo>& info = rt.scripts();
    check(info.size() == 2 && info[0].entity == "hero" && info[0].loaded && !info[0].stopped &&
              info[0].path == pathOf("hero.lua"),
          "runtime: the list says what is loaded, from where, and whether it stopped");
    check(rt.lastError().empty(), "runtime: a healthy set has no error");

    gCalls.clear();
    check(rt.reload("hero", &err), "runtime: a script reloads", err);
    rt.callStart();
    check(gCalls.size() == 3 && gCalls[0] == "start" && gCalls[1] == "self:hero",
          "runtime: the reloaded script runs its onStart again, and so does the other one",
          std::to_string(gCalls.size()));

    rt.callDestroy();
    check(std::find(gCalls.begin(), gCalls.end(), "destroy") != gCalls.end(), "runtime: onDestroy reaches the script");

    // A script that does not compile is reported and left out, without taking the others down.
    writeFile("broken.lua", "function onUpdate(dt)\n  local = 1\nend\n");
    std::string badErr;
    check(!rt.add("broken", pathOf("broken.lua"), &badErr), "runtime: a script that does not compile is refused");
    check(badErr.find("broken.lua:2:") != std::string::npos, "runtime: and the error names the line", badErr);
    check(rt.size() == 2, "runtime: the other scripts are untouched", std::to_string(rt.size()));

    rt.remove("enemy");
    check(rt.size() == 1 && rt.find("enemy") == nullptr, "runtime: removing a script leaves the rest");
    rt.clear();
    check(rt.size() == 0 && rt.scripts().empty(), "runtime: clear removes every script");
}

// --- the editor support files ------------------------------------------------------------------------------------

static void testProjectFiles() {
    makeTree();
    const std::string root = testRoot().string();
    std::vector<std::string> written;
    std::string err;
    check(script_project::writeSupportFiles(root, &written, &err), "project: the support files are written", err);
    for (const std::string& rel : script_project::supportFiles()) {
        check(fs::exists(fs::path(root) / rel), "project: " + rel + " exists");
    }
    check(written.size() == script_project::supportFiles().size(), "project: every file was written once",
          std::to_string(written.size()));

    std::ifstream luarc(fs::path(root) / ".luarc.json");
    const std::string text((std::istreambuf_iterator<char>(luarc)), std::istreambuf_iterator<char>());
    check(text.find("Scripts/.ukiyo/lua") != std::string::npos && text.find("Lua 5.4") != std::string::npos,
          "project: .luarc.json points the language server at the stub, for Lua 5.4");

    // The editor's own files are not overwritten, the stub and .luarc.json always are.
    writeFile("stylua.toml", "-- mine\n");
    std::vector<std::string> second;
    check(script_project::writeSupportFiles(root, &second, &err), "project: writing again succeeds", err);
    check(second.size() == 2, "project: only the stub and .luarc.json are rewritten", std::to_string(second.size()));
    std::ifstream stylua(fs::path(root) / "stylua.toml");
    const std::string kept((std::istreambuf_iterator<char>(stylua)), std::istreambuf_iterator<char>());
    check(kept == "-- mine\n", "project: a formatter config the project changed is kept");

    // The recommendations: the editor shows this list, and .vscode/extensions.json is generated from it, so the two
    // cannot disagree about what a project wants installed.
    const auto& tools = script_project::toolRecommendations();
    check(tools.size() >= 5, "tools: there is a recommendation for a language server, a formatter, a linter and more",
          std::to_string(tools.size()));
    bool complete = true;
    std::string missing;
    std::set<std::string> kinds;
    for (const script_project::ToolRecommendation& tool : tools) {
        if (std::string(tool.name).empty() || std::string(tool.kind).empty() || std::string(tool.url).empty() ||
            std::string(tool.why).empty()) {
            complete = false;
            missing += std::string(tool.name[0] == '\0' ? "?" : tool.name) + " ";
        }
        kinds.insert(tool.kind);
    }
    check(complete, "tools: every recommendation names itself, its kind, its page and what it is for", missing);
    for (const char* kind : {"Language server", "Formatter", "Linter", "Debugger"})
        check(kinds.count(kind) == 1, std::string("tools: there is a ") + kind + " in the list");

    std::string extensions;
    {
        std::ifstream file(fs::path(root) / ".vscode/extensions.json");
        extensions.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }
    int vscodeIds = 0;
    std::set<std::string> seen;
    for (const script_project::ToolRecommendation& tool : tools) {
        const std::string id = tool.vscodeId;
        if (id.empty()) continue;
        ++vscodeIds;
        check(seen.insert(id).second, "tools: " + id + " is recommended once");
        check(extensions.find("\"" + id + "\"") != std::string::npos,
              "tools: .vscode/extensions.json recommends " + id);
        check(script_project::installCommand(tool) == "code --install-extension " + id,
              "tools: " + id + " has a command that installs it");
    }
    check(vscodeIds >= 4, "tools: most recommendations are VS Code extensions", std::to_string(vscodeIds));
    check(extensions.find("slevesque.shader") != std::string::npos,
          "tools: the shader language is recommended too, not only Lua");

    // Completion in the built-in editor comes from the same table as the stub.
    const std::vector<std::string> words = script_project::completionWords();
    for (const char* want : {"ukiyo.raycast", "ukiyo.setPosition", "ukiyo.Entity", "onUpdate", "self", "require"})
        check(std::find(words.begin(), words.end(), std::string(want)) != words.end(),
              std::string("tools: the editor completes ") + want);

    // The stub is Lua the engine can compile, and it documents every function the engine binds.
    ScriptHost h;
    std::string stubErr;
    const std::string stub = script_project::apiStub();
    check(h.compileString(stub, "@ukiyo.lua", &stubErr), "project: the stub is valid Lua", stubErr);
    for (const ScriptApiBinding& b : World::apiBindings()) {
        const std::string decl = std::string("function ukiyo.") + b.name;
        check(stub.find(decl) != std::string::npos, std::string("project: the stub declares ") + b.name);
    }
    bool everyDocIsBound = true;
    std::string unbound;
    for (const script_project::ApiDoc& d : script_project::apiDocs()) {
        const std::string name = d.name;
        if (name == "Vector3" || name == "Quaternion") continue;   // classes, in installMath
        const bool bound = std::any_of(World::apiBindings().begin(), World::apiBindings().end(),
                                       [&](const ScriptApiBinding& b) { return name == b.name; });
        if (!bound && name != "Entity") {
            everyDocIsBound = false;
            unbound += name + " ";
        }
    }
    check(everyDocIsBound, "project: every documented function is one the engine binds", unbound);
}

int main(int argc, char** argv) {
#ifdef _WIN32
    installCrashReporters();
#endif
    checked::checkAll();
    ScriptHost::setAllocator(checked::luaAllocator, nullptr);
    std::string only;   // --only <section>: one section, for CI to bisect a crash by
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--trace") trace = true;
        else if (arg == "--only" && i + 1 < argc) only = argv[++i];
        else if (arg == "--max" && i + 1 < argc) maxChecks = std::atoi(argv[++i]);
    }
    if (const char* env = std::getenv("UKIYO_TEST_TRACE")) trace = trace || std::string(env) != "0";
    if (const char* logPath = std::getenv("UKIYO_TEST_LOG")) {
        // A file, flushed on every line: the OS keeps what was written even when the process is killed, which a pipe
        // does not always do. CI reads it back when a test dies.
        traceLog.open(logPath, std::ios::binary | std::ios::trunc);
        trace = trace || static_cast<bool>(traceLog);
    }
    bool completed = false;
    try {
        if (only.empty() || only == "sandbox") {
            section("sandbox");
            testSandbox();
        }
        if (only.empty() || only == "errors") {
            section("errors");
            testErrors();
        }
        if (only.empty() || only == "require") {
            section("require");
            testRequire();
        }
        if (only.empty() || only == "runtime") {
            section("runtime");
            testRuntime();
        }
        if (only.empty() || only == "project files") {
            section("project files");
            testProjectFiles();
        }
        completed = true;
    } catch (const StopEarly&) {
        // Asked to stop: the run's own destructors have already closed every Lua state and given the heap its chance to
        // complain about what that section did.
    } catch (const std::exception& e) {
        // An exception in the test is a failure of the test, not a crash the next reader has to decode from an exit
        // code: say which one and exit cleanly.
        std::printf("FAIL  the test threw: %s\n", e.what());
        std::fflush(stdout);
        ++failures;
    }
    std::error_code ec;
    fs::remove_all(testRoot(), ec);
    std::printf("script: %d checks, %d failure(s)\n", checks, failures);
    std::fflush(stdout);
    // A run that was asked to stop early says so in the trace too, where CI can read it when stdout is lost.
    if (traceLog) {
        traceLog << (completed ? "END " : "STOPPED ") << checks << "\n";
        traceLog.flush();
    }
    return failures == 0 ? 0 : 1;
}
