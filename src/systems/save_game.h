// Save games: a small, versioned, checksummed key/value store a game can persist.
//
// There was no way to save anything: the scene file is authoring data, not player state. This is deliberately not a
// serialiser for arbitrary C++ types — a game's save is a handful of numbers, strings and positions, and a flat store
// keeps the format readable, diffable and safe to load from an untrusted directory. Values are typed so a version 2
// game reading a version 1 file can tell an int from a float instead of guessing.
//
// File layout (little-endian throughout):
//   "UKSV"  u32 version  u32 entryCount  u32 payloadBytes  payload[payloadBytes]  u32 crc32(payload)
//   payload: repeated { u8 type, u16 nameLength, name[], value }
// Loading validates the magic, the version, the entry count, every length, and the checksum before returning success.
// A failed load leaves the store untouched.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace ukiyo {

class SaveGame {
public:
    static constexpr uint32_t kVersion = 1;
    static constexpr uint32_t kMaxEntries = 65535;
    static constexpr uint32_t kMaxNameLength = 255;
    static constexpr uint32_t kMaxStringBytes = 1u << 20;

    enum class Type : uint8_t { Int = 1, Float = 2, Bool = 3, String = 4, Vec3 = 5 };

    void clear();

    void setInt(const std::string& key, int64_t v);
    void setFloat(const std::string& key, double v);
    void setBool(const std::string& key, bool v);
    void setString(const std::string& key, const std::string& v);
    void setVec3(const std::string& key, const glm::vec3& v);

    // Getters return false when the key is missing or holds another type; `out` is untouched on failure.
    bool getInt(const std::string& key, int64_t* out) const;
    bool getFloat(const std::string& key, double* out) const;
    bool getBool(const std::string& key, bool* out) const;
    bool getString(const std::string& key, std::string* out) const;
    bool getVec3(const std::string& key, glm::vec3* out) const;

    bool has(const std::string& key) const;
    bool typeOf(const std::string& key, Type* out) const;
    size_t size() const { return entries_.size(); }
    std::vector<std::string> keys() const;

    // Whether the last load succeeded. Starts true on a fresh store, so a game that never loads still writes.
    bool valid() const { return valid_; }

    std::vector<uint8_t> serialize() const;
    bool deserialize(const uint8_t* data, size_t size);

    bool saveToFile(const std::string& path) const;
    bool loadFromFile(const std::string& path);

    uint32_t crc32(const std::vector<uint8_t>& bytes) const;

private:
    struct Entry {
        Type type = Type::Int;
        int64_t i = 0;
        double f = 0.0;
        bool b = false;
        std::string s;
        glm::vec3 v{0.0f};
    };
    std::map<std::string, Entry> entries_;   // ordered, so serialisation is deterministic for a given content
    bool valid_ = true;
};

// Writes `bytes` and flushes; used by saveToFile. Returns false on any I/O failure.
bool writeFileBytes(const std::string& path, const std::vector<uint8_t>& bytes);
bool readFileBytes(const std::string& path, std::vector<uint8_t>* out);

} // namespace ukiyo
