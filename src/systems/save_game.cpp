#include "systems/save_game.h"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace ukiyo {

namespace {

constexpr size_t kHeaderBytes = 4 + 4 + 4 + 4;
constexpr char kMagic[4] = {'U', 'K', 'S', 'V'};

void putU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}

void putU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
}

void putU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
}

uint16_t getU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t getU32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}

uint64_t getU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

} // namespace

void SaveGame::clear() {
    entries_.clear();
    valid_ = true;
}

void SaveGame::setInt(const std::string& key, int64_t v) {
    Entry e;
    e.type = Type::Int;
    e.i = v;
    entries_[key] = e;
}

void SaveGame::setFloat(const std::string& key, double v) {
    Entry e;
    e.type = Type::Float;
    e.f = v;
    entries_[key] = e;
}

void SaveGame::setBool(const std::string& key, bool v) {
    Entry e;
    e.type = Type::Bool;
    e.b = v;
    entries_[key] = e;
}

void SaveGame::setString(const std::string& key, const std::string& v) {
    Entry e;
    e.type = Type::String;
    e.s = v;
    entries_[key] = e;
}

void SaveGame::setVec3(const std::string& key, const glm::vec3& v) {
    Entry e;
    e.type = Type::Vec3;
    e.v = v;
    entries_[key] = e;
}

bool SaveGame::getInt(const std::string& key, int64_t* out) const {
    const auto it = entries_.find(key);
    if (it == entries_.end() || it->second.type != Type::Int) return false;
    if (out) *out = it->second.i;
    return true;
}

bool SaveGame::getFloat(const std::string& key, double* out) const {
    const auto it = entries_.find(key);
    if (it == entries_.end() || it->second.type != Type::Float) return false;
    if (out) *out = it->second.f;
    return true;
}

bool SaveGame::getBool(const std::string& key, bool* out) const {
    const auto it = entries_.find(key);
    if (it == entries_.end() || it->second.type != Type::Bool) return false;
    if (out) *out = it->second.b;
    return true;
}

bool SaveGame::getString(const std::string& key, std::string* out) const {
    const auto it = entries_.find(key);
    if (it == entries_.end() || it->second.type != Type::String) return false;
    if (out) *out = it->second.s;
    return true;
}

bool SaveGame::getVec3(const std::string& key, glm::vec3* out) const {
    const auto it = entries_.find(key);
    if (it == entries_.end() || it->second.type != Type::Vec3) return false;
    if (out) *out = it->second.v;
    return true;
}

bool SaveGame::has(const std::string& key) const { return entries_.find(key) != entries_.end(); }

bool SaveGame::typeOf(const std::string& key, Type* out) const {
    const auto it = entries_.find(key);
    if (it == entries_.end()) return false;
    if (out) *out = it->second.type;
    return true;
}

std::vector<std::string> SaveGame::keys() const {
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const auto& kv : entries_) out.push_back(kv.first);
    return out;
}

std::vector<uint8_t> SaveGame::serialize() const {
    std::vector<uint8_t> payload;
    for (const auto& kv : entries_) {
        const std::string& name = kv.first;
        const Entry& e = kv.second;
        payload.push_back(static_cast<uint8_t>(e.type));
        putU16(payload, static_cast<uint16_t>(std::min<size_t>(name.size(), kMaxNameLength)));
        payload.insert(payload.end(), name.begin(), name.begin() + std::min<size_t>(name.size(), kMaxNameLength));
        switch (e.type) {
        case Type::Int: putU64(payload, static_cast<uint64_t>(e.i)); break;
        case Type::Float: {
            // Bit-cast rather than textual: saves must round-trip exactly, including denormals and infinities.
            uint64_t bits = 0;
            std::memcpy(&bits, &e.f, sizeof(bits));
            putU64(payload, bits);
            break;
        }
        case Type::Bool: payload.push_back(e.b ? 1 : 0); break;
        case Type::String: {
            const size_t n = std::min<size_t>(e.s.size(), kMaxStringBytes);
            putU32(payload, static_cast<uint32_t>(n));
            payload.insert(payload.end(), e.s.begin(), e.s.begin() + static_cast<long>(n));
            break;
        }
        case Type::Vec3: {
            float v[3] = {e.v.x, e.v.y, e.v.z};
            uint8_t raw[12];
            std::memcpy(raw, v, sizeof(raw));
            payload.insert(payload.end(), raw, raw + sizeof(raw));
            break;
        }
        }
    }
    std::vector<uint8_t> out;
    out.reserve(kHeaderBytes + payload.size() + 4);
    out.insert(out.end(), kMagic, kMagic + 4);
    putU32(out, kVersion);
    putU32(out, static_cast<uint32_t>(entries_.size()));
    putU32(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    putU32(out, crc32(payload));
    return out;
}

bool SaveGame::deserialize(const uint8_t* data, size_t size) {
    valid_ = false;
    if (!data || size < kHeaderBytes + 4) return false;
    if (std::memcmp(data, kMagic, 4) != 0) return false;
    const uint32_t version = getU32(data + 4);
    if (version != kVersion) return false;
    const uint32_t count = getU32(data + 8);
    const uint32_t payloadBytes = getU32(data + 12);
    if (count > kMaxEntries) return false;
    if (static_cast<uint64_t>(payloadBytes) + kHeaderBytes + 4 != size) return false;
    const uint8_t* payload = data + kHeaderBytes;
    const uint32_t stored = getU32(data + kHeaderBytes + payloadBytes);
    std::vector<uint8_t> body(payload, payload + payloadBytes);
    if (crc32(body) != stored) return false;

    std::map<std::string, Entry> parsed;
    size_t at = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (at + 3 > payloadBytes) return false;
        const Type type = static_cast<Type>(payload[at]);
        const uint16_t nameLen = getU16(payload + at + 1);
        at += 3;
        if (type != Type::Int && type != Type::Float && type != Type::Bool && type != Type::String &&
            type != Type::Vec3) return false;
        if (at + nameLen > payloadBytes) return false;
        std::string name(reinterpret_cast<const char*>(payload + at), nameLen);
        at += nameLen;
        Entry e;
        e.type = type;
        switch (type) {
        case Type::Int: {
            if (at + 8 > payloadBytes) return false;
            e.i = static_cast<int64_t>(getU64(payload + at));
            at += 8;
            break;
        }
        case Type::Float: {
            if (at + 8 > payloadBytes) return false;
            const uint64_t bits = getU64(payload + at);
            std::memcpy(&e.f, &bits, sizeof(bits));
            at += 8;
            break;
        }
        case Type::Bool: {
            if (at + 1 > payloadBytes) return false;
            e.b = payload[at] != 0;
            at += 1;
            break;
        }
        case Type::String: {
            if (at + 4 > payloadBytes) return false;
            const uint32_t n = getU32(payload + at);
            at += 4;
            if (n > kMaxStringBytes || at + n > payloadBytes) return false;
            e.s.assign(reinterpret_cast<const char*>(payload + at), n);
            at += n;
            break;
        }
        case Type::Vec3: {
            if (at + 12 > payloadBytes) return false;
            float v[3];
            std::memcpy(v, payload + at, sizeof(v));
            e.v = glm::vec3(v[0], v[1], v[2]);
            at += 12;
            break;
        }
        }
        parsed[name] = e;
    }
    if (at != payloadBytes) return false;   // trailing bytes mean the file is not what it claims to be
    entries_ = std::move(parsed);
    valid_ = true;
    return true;
}

uint32_t SaveGame::crc32(const std::vector<uint8_t>& bytes) const {
    // Standard reflected CRC-32 (IEEE 802.3), table built once. No dependency, no platform endianness assumptions.
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (uint8_t b : bytes) c = table[(c ^ b) & 0xff] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool SaveGame::saveToFile(const std::string& path) const { return writeFileBytes(path, serialize()); }

bool SaveGame::loadFromFile(const std::string& path) {
    std::vector<uint8_t> bytes;
    if (!readFileBytes(path, &bytes)) {
        valid_ = false;
        return false;
    }
    return deserialize(bytes.data(), bytes.size());
}

bool writeFileBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (!bytes.empty()) f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    f.flush();
    return static_cast<bool>(f);
}

bool readFileBytes(const std::string& path, std::vector<uint8_t>* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n < 0) return false;
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(n));
    if (n > 0) f.read(reinterpret_cast<char*>(bytes.data()), n);
    if (!f && !f.eof()) return false;
    if (out) *out = std::move(bytes);
    return true;
}

} // namespace ukiyo
