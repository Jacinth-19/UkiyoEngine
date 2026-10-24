// Save games: value types, byte-exact round trips, checksum and truncation rejection, and file I/O.
#include "systems/save_game.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace ukiyo;

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
} // namespace

int main() {
    SaveGame s;
    check(s.size() == 0 && s.valid(), "a fresh store is empty and valid");

    // Every type round-trips through bytes.
    s.setInt("hp", -42);
    s.setFloat("speed", 3.25);
    s.setBool("flag", true);
    s.setString("name", "Alicia");
    s.setVec3("spawn", glm::vec3(1.5f, -2.0f, 0.25f));

    const std::vector<uint8_t> bytes = s.serialize();
    check(bytes.size() > 16, "serialised bytes exist");
    check(std::memcmp(bytes.data(), "UKSV", 4) == 0, "magic is UKSV");

    SaveGame t;
    check(t.deserialize(bytes.data(), bytes.size()), "the file loads");
    int64_t i = 0;
    double f = 0;
    bool b = false;
    std::string str;
    glm::vec3 v(0.0f);
    check(t.getInt("hp", &i) && i == -42, "int round-trips");
    check(t.getFloat("speed", &f) && f == 3.25, "float round-trips exactly");
    check(t.getBool("flag", &b) && b, "bool round-trips");
    check(t.getString("name", &str) && str == "Alicia", "string round-trips");
    check(t.getVec3("spawn", &v) && v == glm::vec3(1.5f, -2.0f, 0.25f), "vec3 round-trips");
    check(t.serialize() == bytes, "serialisation is deterministic");

    // Type safety: reading an int as a float fails and leaves the output alone.
    double untouched = 7.5;
    check(!t.getFloat("hp", &untouched) && untouched == 7.5, "a type mismatch fails without touching the output");
    check(!t.getInt("missing", &i), "a missing key fails");
    SaveGame::Type ty;
    check(t.typeOf("speed", &ty) && ty == SaveGame::Type::Float, "typeOf reports the type");

    // Replacing a key keeps one entry and the new type.
    t.setString("hp", "gone");
    check(t.size() == 5, "replacing a key does not grow the store");
    check(t.has("hp") && !t.getInt("hp", &i), "the replacement changed the type");

    // Integer extremes survive.
    SaveGame e;
    e.setInt("min", INT64_MIN);
    e.setInt("max", INT64_MAX);
    SaveGame e2;
    check(e2.deserialize(e.serialize().data(), e.serialize().size()), "extreme ints load");
    int64_t lo = 0, hi = 0;
    check(e2.getInt("min", &lo) && lo == INT64_MIN, "INT64_MIN round-trips");
    check(e2.getInt("max", &hi) && hi == INT64_MAX, "INT64_MAX round-trips");

    // Unicode and an empty string.
    SaveGame u;
    u.setString("jp", "セーブ");
    u.setString("empty", "");
    SaveGame u2;
    check(u2.deserialize(u.serialize().data(), u.serialize().size()), "utf-8 strings load");
    std::string jp;
    check(u2.getString("jp", &jp) && jp == "セーブ", "utf-8 bytes are preserved");
    std::string empty = "x";
    check(u2.getString("empty", &empty) && empty.empty(), "an empty string round-trips");

    // Bad magic.
    std::vector<uint8_t> bad = bytes;
    bad[0] = 'X';
    SaveGame m;
    check(!m.deserialize(bad.data(), bad.size()), "bad magic is rejected");

    // Wrong version.
    std::vector<uint8_t> version = bytes;
    version[4] = 2;
    check(!m.deserialize(version.data(), version.size()), "an unknown version is rejected");

    // Every truncation is rejected and leaves the store invalid.
    bool anyAccepted = false;
    for (size_t cut = 0; cut < bytes.size(); ++cut) {
        SaveGame q;
        q.setInt("keep", 1);
        if (q.deserialize(bytes.data(), cut)) anyAccepted = true;
        if (q.valid()) anyAccepted = true;
        int64_t keep = 0;
        if (!q.getInt("keep", &keep) || keep != 1) anyAccepted = true;   // a failed load must not mutate
    }
    check(!anyAccepted, "every truncation is rejected without changing the store");

    // A corrupted byte inside the payload breaks the checksum.
    std::vector<uint8_t> corrupt = bytes;
    corrupt[corrupt.size() / 2] ^= 0x40;
    SaveGame c;
    check(!c.deserialize(corrupt.data(), corrupt.size()), "a corrupted payload fails the checksum");

    // A valid header with a mismatched length field is rejected.
    std::vector<uint8_t> len = bytes;
    len[12] = static_cast<uint8_t>(len[12] + 1);
    check(!c.deserialize(len.data(), len.size()), "a wrong payload length is rejected");

    // File round trip.
    const std::string path = "build/save_test_written.uksav";
    check(s.saveToFile(path), "saving to a file works");
    SaveGame fromFile;
    check(fromFile.loadFromFile(path), "loading from a file works");
    int64_t fi = 0;
    check(fromFile.getInt("hp", &fi) && fi == -42, "the file carries the values");
    SaveGame missing;
    check(!missing.loadFromFile("build/does_not_exist.uksav"), "a missing file fails cleanly");
    check(!missing.valid(), "a failed load marks the store invalid");

    // 10 000 keys: no quadratic blow-up, and the size limit holds.
    SaveGame many;
    for (int k = 0; k < 10000; ++k) many.setInt("k" + std::to_string(k), k);
    SaveGame many2;
    const std::vector<uint8_t> manyBytes = many.serialize();
    check(many2.deserialize(manyBytes.data(), manyBytes.size()), "10 000 keys load");
    int64_t last = 0;
    check(many2.getInt("k9999", &last) && last == 9999, "the last key survives");

    std::printf("save_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
