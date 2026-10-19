#include "systems/network.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ukiyo {
namespace {

// ---- Wire format ----------------------------------------------------------------------------

constexpr uint8_t kMagic0 = 'U';
constexpr uint8_t kMagic1 = 'K';
constexpr uint8_t kProtocolVersion = 1;
constexpr uint8_t kTypeConnect = 1;
constexpr uint8_t kTypeAccept = 2;
constexpr uint8_t kTypeDisconnect = 3;
constexpr uint8_t kTypeData = 4;
constexpr size_t kHeaderSize = 12;
constexpr size_t kMaxDatagram = 1200;
constexpr size_t kMaxPendingMessages = 512;
constexpr double kInitialRto = 0.2;
constexpr double kPacketHistorySeconds = 10.0;

#ifdef _WIN32
using SocketLength = int;
#else
using SocketLength = socklen_t;
#endif

bool ensureNetwork(std::string* error) {
#ifdef _WIN32
    static const int startup = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data);
    }();
    if (startup != 0) {
        if (error) *error = "WSAStartup failed with code " + std::to_string(startup);
        return false;
    }
#else
    (void)error;
#endif
    return true;
}

bool wouldBlock() {
#ifdef _WIN32
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

std::string socketErrorText() {
#ifdef _WIN32
    return "socket error " + std::to_string(WSAGetLastError());
#else
    return std::string(std::strerror(errno));
#endif
}

std::string resolveErrorText(int code) {
#ifdef _WIN32
    (void)code;
    return "host not found";
#else
    const char* text = gai_strerror(code);
    return text ? std::string(text) : ("resolver error " + std::to_string(code));
#endif
}

void closeHandle(std::intptr_t handle) {
    if (handle == -1) return;
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(handle));
#else
    ::close(static_cast<int>(handle));
#endif
}

sockaddr_in toSockAddr(const NetAddress& address) {
    sockaddr_in result{};
    result.sin_family = AF_INET;
    result.sin_addr.s_addr = address.ip;
    result.sin_port = htons(address.port);
    return result;
}

NetAddress fromSockAddr(const sockaddr_in& address) {
    NetAddress result;
    result.ip = address.sin_addr.s_addr;
    result.port = ntohs(address.sin_port);
    return result;
}

void putU16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void putU32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xff));
}

struct Header {
    uint8_t type = 0;
    uint16_t sequence = 0;
    uint16_t ack = 0;
    uint32_t ackBits = 0;
};

void writeHeader(std::vector<uint8_t>& out, uint8_t type, uint16_t sequence, uint16_t ack,
                 uint32_t ackBits) {
    out.push_back(kMagic0);
    out.push_back(kMagic1);
    out.push_back(kProtocolVersion);
    out.push_back(type);
    putU16(out, sequence);
    putU16(out, ack);
    putU32(out, ackBits);
}

bool parseHeader(const uint8_t* data, size_t size, Header* header) {
    if (!data || size < kHeaderSize) return false;
    if (data[0] != kMagic0 || data[1] != kMagic1 || data[2] != kProtocolVersion) return false;
    header->type = data[3];
    header->sequence = static_cast<uint16_t>(data[4] | (data[5] << 8));
    header->ack = static_cast<uint16_t>(data[6] | (data[7] << 8));
    header->ackBits = uint32_t(data[8]) | (uint32_t(data[9]) << 8) | (uint32_t(data[10]) << 16) |
                      (uint32_t(data[11]) << 24);
    return header->type >= kTypeConnect && header->type <= kTypeData;
}

// True when `a` comes before `b` in the wrapping 16-bit sequence space.
bool serialLess(uint16_t a, uint16_t b) {
    return static_cast<int16_t>(static_cast<uint16_t>(a - b)) < 0;
}

// Tracks which packet sequence numbers have arrived. `bits` holds the 32 packets below `highest`:
// bit 0 is highest - 1, bit 1 is highest - 2, and so on.
struct SequenceWindow {
    bool started = false;
    uint16_t highest = 0;
    uint32_t bits = 0;

    uint16_t ackValue() const { return started ? highest : 0; }

    bool add(uint16_t sequence) {
        if (!started) {
            started = true;
            highest = sequence;
            bits = 0;
            return true;
        }
        if (sequence == highest) return false;
        if (serialLess(highest, sequence)) {
            const unsigned shift = static_cast<unsigned>(static_cast<uint16_t>(sequence - highest));
            bits = shift >= 32 ? 0u : (bits << shift);
            if (shift >= 1 && shift <= 32) bits |= 1u << (shift - 1);
            highest = sequence;
            return true;
        }
        const unsigned behind = static_cast<unsigned>(static_cast<uint16_t>(highest - sequence));
        if (behind >= 1 && behind <= 32) {
            const uint32_t mask = 1u << (behind - 1);
            if (bits & mask) return false;
            bits |= mask;
            return true;
        }
        return false;   // too old to tell apart: drop it rather than deliver stale data
    }
};

bool packetAcked(uint16_t ack, uint32_t ackBits, uint16_t sequence) {
    if (ack == 0) return false;
    if (sequence == ack) return true;
    const unsigned behind = static_cast<unsigned>(static_cast<uint16_t>(ack - sequence));
    return behind >= 1 && behind <= 32 && (ackBits & (1u << (behind - 1))) != 0;
}

// ---- Peers ----------------------------------------------------------------------------------

struct PendingMessage {
    uint16_t id = 0;
    bool reliable = true;
    std::vector<uint8_t> data;
    double lastSent = 0.0;
    double firstSent = 0.0;
    uint16_t lastSequence = 0;
    int attempts = 0;
};

struct Peer {
    NetAddress address;
    bool connected = false;
    uint32_t nonce = 0;       // server: nonce the client asked with
    uint32_t sessionId = 0;   // server: id sent back in the accept
    NetStats stats;

    std::vector<PendingMessage> outbox;
    uint16_t nextMessageId = 1;
    uint16_t nextSequence = 1;

    uint16_t nextExpectedMessage = 1;
    std::map<uint16_t, std::vector<uint8_t>> pendingIn;
    SequenceWindow received;
    bool ackPending = false;

    double lastSend = 0.0;
    double lastReceive = 0.0;
    double smoothedRtt = 0.0;
    bool haveRtt = false;
    std::deque<std::pair<uint16_t, double>> sentPackets;
};

double retransmitTimeout(const Peer& peer, const NetConfig& config) {
    const double minimum = std::max(0.001, static_cast<double>(config.rtoMin));
    const double maximum = std::max(minimum, static_cast<double>(config.rtoMax));
    const double timeout = peer.haveRtt ? peer.smoothedRtt * 2.0 : kInitialRto;
    return std::min(maximum, std::max(minimum, timeout));
}

bool queueMessage(Peer& peer, const NetConfig& config, const void* data, size_t size, bool reliable) {
    if (size > config.maxMessageSize) return false;
    const size_t overhead = 1 + (reliable ? 2 : 0) + 2;
    if (overhead + size > kMaxDatagram - kHeaderSize - 2) return false;
    if (size > 0 && !data) return false;

    PendingMessage message;
    message.reliable = reliable;
    if (size > 0) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        message.data.assign(bytes, bytes + size);
    }
    if (reliable) message.id = peer.nextMessageId++;
    peer.outbox.push_back(std::move(message));
    return true;
}

void processData(Peer& peer, const uint8_t* payload, size_t size, int client,
                 std::vector<NetEvent>& events) {
    ByteReader reader(payload, size);
    uint16_t count = 0;
    if (!reader.u16(&count)) {
        ++peer.stats.rejected;
        return;
    }
    for (uint16_t i = 0; i < count; ++i) {
        uint8_t flags = 0;
        if (!reader.u8(&flags)) {
            ++peer.stats.rejected;
            return;
        }
        const bool reliable = (flags & 1u) != 0;
        uint16_t id = 0;
        uint16_t length = 0;
        if (reliable && !reader.u16(&id)) {
            ++peer.stats.rejected;
            return;
        }
        if (!reader.u16(&length) || length > reader.remaining()) {
            ++peer.stats.rejected;
            return;
        }
        std::vector<uint8_t> data(length);
        if (length > 0 && !reader.bytes(data.data(), length)) {
            ++peer.stats.rejected;
            return;
        }

        if (!reliable) {
            NetEvent event;
            event.type = NetEvent::Type::Message;
            event.client = client;
            event.message = std::move(data);
            event.reliable = false;
            events.push_back(std::move(event));
            ++peer.stats.messagesReceived;
            continue;
        }

        // Reliable messages are delivered in id order exactly once. A message that arrives early waits
        // in pendingIn until every message before it has been delivered.
        if (serialLess(id, peer.nextExpectedMessage)) {
            ++peer.stats.duplicates;
            continue;
        }
        if (peer.pendingIn.find(id) == peer.pendingIn.end() &&
            peer.pendingIn.size() >= kMaxPendingMessages) {
            ++peer.stats.rejected;
            continue;
        }
        peer.pendingIn.emplace(id, std::move(data));
        for (;;) {
            auto next = peer.pendingIn.find(peer.nextExpectedMessage);
            if (next == peer.pendingIn.end()) break;
            NetEvent event;
            event.type = NetEvent::Type::Message;
            event.client = client;
            event.message = std::move(next->second);
            event.reliable = true;
            events.push_back(std::move(event));
            ++peer.stats.messagesReceived;
            peer.pendingIn.erase(next);
            peer.nextExpectedMessage = static_cast<uint16_t>(peer.nextExpectedMessage + 1);
        }
    }
    // Trailing bytes are ignored so later protocol additions can append fields.
}

void processAcks(Peer& peer, uint16_t ack, uint32_t ackBits, double now) {
    if (ack == 0) return;

    for (const auto& sent : peer.sentPackets) {
        if (sent.first != ack) continue;
        const double sample = now - sent.second;
        if (sample >= 0.0 && sample < kPacketHistorySeconds) {
            if (!peer.haveRtt) {
                peer.smoothedRtt = sample;
                peer.haveRtt = true;
            } else {
                peer.smoothedRtt = peer.smoothedRtt * 0.875 + sample * 0.125;
            }
            peer.stats.rtt = static_cast<float>(peer.smoothedRtt);
        }
        break;
    }
    while (!peer.sentPackets.empty() &&
           (peer.sentPackets.size() > 128 ||
            now - peer.sentPackets.front().second > kPacketHistorySeconds)) {
        peer.sentPackets.pop_front();
    }

    peer.outbox.erase(
        std::remove_if(peer.outbox.begin(), peer.outbox.end(),
                       [ack, ackBits](const PendingMessage& message) {
                           return message.reliable && message.attempts > 0 &&
                                  packetAcked(ack, ackBits, message.lastSequence);
                       }),
        peer.outbox.end());
}

// Writes queued messages and retransmissions, plus pending acknowledgements and heartbeats. Returns
// true when at least one datagram left the socket.
bool flushPeer(Peer& peer, UdpSocket& socket, const NetConfig& config, double now) {
    if (!peer.connected) return false;

    bool sentAny = false;
    for (int batch = 0; batch < 8; ++batch) {
        std::vector<size_t> due;
        size_t budget = kMaxDatagram - kHeaderSize - 2;
        const double rto = retransmitTimeout(peer, config);
        for (size_t i = 0; i < peer.outbox.size(); ++i) {
            const PendingMessage& message = peer.outbox[i];
            const bool ready = message.attempts == 0 ||
                               (message.reliable && now - message.lastSent >= rto);
            if (!ready) continue;
            const size_t need = 1 + (message.reliable ? 2 : 0) + 2 + message.data.size();
            if (need > budget) {
                if (due.empty()) {
                    // send() refuses messages that do not fit a whole datagram, so this only happens
                    // if a message was queued by a path that bypassed that check. Skip it.
                    ++peer.stats.rejected;
                    peer.outbox.erase(peer.outbox.begin() + static_cast<std::ptrdiff_t>(i));
                    --i;
                    continue;
                }
                break;
            }
            due.push_back(i);
            budget -= need;
        }

        const bool acknowledges = peer.ackPending;
        const bool heartbeat = now - peer.lastSend >= static_cast<double>(config.heartbeatInterval);
        if (due.empty() && !acknowledges && !heartbeat) break;

        std::vector<uint8_t> packet;
        packet.reserve(kMaxDatagram);
        const uint16_t sequence = peer.nextSequence++;
        writeHeader(packet, kTypeData, sequence, peer.received.ackValue(), peer.received.bits);
        const size_t countPosition = packet.size();
        packet.push_back(0);
        packet.push_back(0);
        uint16_t count = 0;
        for (size_t index : due) {
            const PendingMessage& message = peer.outbox[index];
            packet.push_back(message.reliable ? 1 : 0);
            if (message.reliable) putU16(packet, message.id);
            putU16(packet, static_cast<uint16_t>(message.data.size()));
            packet.insert(packet.end(), message.data.begin(), message.data.end());
            ++count;
        }
        packet[countPosition] = static_cast<uint8_t>(count & 0xff);
        packet[countPosition + 1] = static_cast<uint8_t>(count >> 8);

        const int written = socket.sendTo(peer.address, packet.data(), packet.size());
        if (written != static_cast<int>(packet.size())) break;

        sentAny = true;
        peer.lastSend = now;
        ++peer.stats.packetsSent;
        peer.stats.bytesSent += packet.size();
        peer.ackPending = false;
        peer.sentPackets.emplace_back(sequence, now);
        while (peer.sentPackets.size() > 128) peer.sentPackets.pop_front();

        for (size_t index : due) {
            PendingMessage& message = peer.outbox[index];
            if (message.attempts == 0) {
                message.firstSent = now;
                ++peer.stats.messagesSent;
            } else {
                ++peer.stats.retransmits;
            }
            ++message.attempts;
            message.lastSent = now;
            message.lastSequence = sequence;
        }
        // Unreliable messages are never retransmitted, so they leave the queue after their first send.
        peer.outbox.erase(
            std::remove_if(peer.outbox.begin(), peer.outbox.end(),
                           [](const PendingMessage& message) {
                               return !message.reliable && message.attempts > 0;
                           }),
            peer.outbox.end());

        if (due.empty()) break;   // that packet only carried an acknowledgement or heartbeat
    }
    return sentAny;
}

uint32_t nextNonce() {
    static uint32_t counter = 0;
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return static_cast<uint32_t>(ticks) ^ (++counter * 0x9e3779b9u);
}

const NetStats kEmptyStats;

} // namespace

// ---- Serialisation --------------------------------------------------------------------------

void ByteWriter::u8(uint8_t value) { data_.push_back(value); }

void ByteWriter::u16(uint16_t value) { putU16(data_, value); }

void ByteWriter::u32(uint32_t value) { putU32(data_, value); }

void ByteWriter::u64(uint64_t value) {
    for (int i = 0; i < 8; ++i) data_.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xff));
}

void ByteWriter::f32(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    u32(bits);
}

void ByteWriter::bytes(const void* data, size_t size) {
    if (!data || size == 0) return;
    const auto* bytes = static_cast<const uint8_t*>(data);
    data_.insert(data_.end(), bytes, bytes + size);
}

void ByteWriter::string(const std::string& value) {
    const size_t length = std::min<size_t>(value.size(), 65535);
    u16(static_cast<uint16_t>(length));
    bytes(value.data(), length);
}

ByteReader::ByteReader(const void* data, size_t size)
    : data_(static_cast<const uint8_t*>(data)), size_(size) {}

ByteReader::ByteReader(const std::vector<uint8_t>& data) : data_(data.data()), size_(data.size()) {}

bool ByteReader::take(void* destination, size_t size) {
    if (!ok_ || remaining() < size) {
        ok_ = false;
        return false;
    }
    if (size > 0) std::memcpy(destination, data_ + position_, size);
    position_ += size;
    return true;
}

bool ByteReader::u8(uint8_t* value) { return value && take(value, sizeof *value); }

bool ByteReader::u16(uint16_t* value) {
    if (!value) return false;
    uint8_t raw[2] = {};
    if (!take(raw, sizeof raw)) return false;
    *value = static_cast<uint16_t>(raw[0] | (raw[1] << 8));
    return true;
}

bool ByteReader::u32(uint32_t* value) {
    if (!value) return false;
    uint8_t raw[4] = {};
    if (!take(raw, sizeof raw)) return false;
    *value = uint32_t(raw[0]) | (uint32_t(raw[1]) << 8) | (uint32_t(raw[2]) << 16) |
             (uint32_t(raw[3]) << 24);
    return true;
}

bool ByteReader::u64(uint64_t* value) {
    if (!value) return false;
    uint8_t raw[8] = {};
    if (!take(raw, sizeof raw)) return false;
    uint64_t result = 0;
    for (int i = 0; i < 8; ++i) result |= uint64_t(raw[i]) << (8 * i);
    *value = result;
    return true;
}

bool ByteReader::i32(int32_t* value) {
    uint32_t raw = 0;
    if (!u32(&raw)) return false;
    *value = static_cast<int32_t>(raw);
    return true;
}

bool ByteReader::f32(float* value) {
    uint32_t bits = 0;
    if (!u32(&bits)) return false;
    std::memcpy(value, &bits, sizeof bits);
    return true;
}

bool ByteReader::bytes(void* destination, size_t size) {
    if (size > 0 && !destination) return false;
    return take(destination, size);
}

bool ByteReader::string(std::string* value) {
    if (!value) return false;
    uint16_t length = 0;
    if (!u16(&length) || remaining() < length) {
        ok_ = false;
        return false;
    }
    if (length == 0) {
        value->clear();
    } else {
        value->assign(reinterpret_cast<const char*>(data_ + position_), length);
    }
    position_ += length;
    return true;
}

// ---- Addresses ------------------------------------------------------------------------------

std::string NetAddress::toString() const {
    char text[INET_ADDRSTRLEN] = {};
    in_addr address{};
    address.s_addr = ip;
    if (!inet_ntop(AF_INET, &address, text, sizeof text)) return "<invalid>";
    return std::string(text) + ":" + std::to_string(port);
}

bool NetAddress::parse(const std::string& host, uint16_t port, NetAddress* address,
                       std::string* error) {
    if (!address) {
        if (error) *error = "no address to fill";
        return false;
    }
    if (!ensureNetwork(error)) return false;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* found = nullptr;
    const int result = getaddrinfo(host.c_str(), nullptr, &hints, &found);
    if (result != 0 || !found) {
        if (error) *error = "cannot resolve \"" + host + "\": " + resolveErrorText(result);
        if (found) freeaddrinfo(found);
        return false;
    }
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(found->ai_addr);
    address->ip = ipv4->sin_addr.s_addr;
    address->port = port;
    freeaddrinfo(found);
    return true;
}

// ---- Sockets --------------------------------------------------------------------------------

UdpSocket::~UdpSocket() { close(); }

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : handle_(other.handle_), localPort_(other.localPort_), error_(std::move(other.error_)) {
    other.handle_ = kClosed;
    other.localPort_ = 0;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this == &other) return *this;
    close();
    handle_ = other.handle_;
    localPort_ = other.localPort_;
    error_ = std::move(other.error_);
    other.handle_ = kClosed;
    other.localPort_ = 0;
    return *this;
}

bool UdpSocket::open(uint16_t port, std::string* error) {
    close();
    error_.clear();
    if (!ensureNetwork(&error_)) {
        if (error) *error = error_;
        return false;
    }

#ifdef _WIN32
    SOCKET handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const bool failed = handle == INVALID_SOCKET;
#else
    int handle = ::socket(AF_INET, SOCK_DGRAM, 0);
    const bool failed = handle < 0;
#endif
    if (failed) {
        error_ = socketErrorText();
        if (error) *error = error_;
        return false;
    }

    // No SO_REUSEADDR: two servers must not be able to bind the same UDP port and split the
    // incoming datagrams between them. A UDP socket has no TIME_WAIT, so a restart is immediate.
    const sockaddr_in local = toSockAddr(NetAddress{htonl(INADDR_ANY), port});
    if (::bind(handle, reinterpret_cast<const sockaddr*>(&local), sizeof local) != 0) {
        error_ = "cannot bind UDP port " + std::to_string(port) + ": " + socketErrorText();
        closeHandle(static_cast<std::intptr_t>(handle));
        if (error) *error = error_;
        return false;
    }

#ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(handle, FIONBIO, &mode) != 0) {
        error_ = "cannot make the socket non-blocking: " + socketErrorText();
        closeHandle(static_cast<std::intptr_t>(handle));
        if (error) *error = error_;
        return false;
    }
#else
    const int flags = fcntl(handle, F_GETFL, 0);
    if (flags < 0 || fcntl(handle, F_SETFL, flags | O_NONBLOCK) < 0) {
        error_ = "cannot make the socket non-blocking: " + socketErrorText();
        closeHandle(static_cast<std::intptr_t>(handle));
        if (error) *error = error_;
        return false;
    }
#endif

    sockaddr_in bound{};
    SocketLength length = sizeof bound;
    if (getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
        localPort_ = ntohs(bound.sin_port);
    } else {
        localPort_ = port;
    }
    handle_ = static_cast<std::intptr_t>(handle);
    return true;
}

void UdpSocket::close() {
    if (handle_ == kClosed) return;
    closeHandle(handle_);
    handle_ = kClosed;
    localPort_ = 0;
}

int UdpSocket::sendTo(const NetAddress& destination, const void* data, size_t size) {
    if (!isOpen()) {
        error_ = "socket is not open";
        return -1;
    }
    if (size > 65507) {
        error_ = "datagram is larger than the UDP limit";
        return -1;
    }
    if (!destination.valid()) {
        error_ = "destination address is invalid";
        return -1;
    }
    const sockaddr_in target = toSockAddr(destination);
    const int written = static_cast<int>(
        ::sendto(handle_, static_cast<const char*>(data), static_cast<int>(size), 0,
                 reinterpret_cast<const sockaddr*>(&target), sizeof target));
    if (written < 0) {
        if (wouldBlock()) return 0;
        error_ = socketErrorText();
        return -1;
    }
    return written;
}

int UdpSocket::recvFrom(NetAddress* from, void* data, size_t capacity) {
    if (!isOpen()) {
        error_ = "socket is not open";
        return -1;
    }
    sockaddr_in source{};
    SocketLength length = sizeof source;
    const int received = static_cast<int>(
        ::recvfrom(handle_, static_cast<char*>(data),
                   static_cast<int>(std::min<size_t>(capacity, 65507)), 0,
                   reinterpret_cast<sockaddr*>(&source), &length));
    if (received < 0) {
        if (wouldBlock()) return 0;
        error_ = socketErrorText();
        return -1;
    }
    if (from) *from = fromSockAddr(source);
    return received;
}

// ---- Client ---------------------------------------------------------------------------------

struct NetClient::Impl {
    explicit Impl(const NetConfig& settings) : config(settings) {}

    enum class State { Idle, Connecting, Connected };

    NetConfig config;
    UdpSocket socket;
    NetAddress server;
    Peer peer;
    State state = State::Idle;
    uint32_t nonce = 0;
    uint32_t session = 0;
    double now = 0.0;
    float connectElapsed = 0.0f;
    float retryElapsed = 0.0f;
    std::vector<NetEvent> events;

    void sendConnect();
    void sendDisconnectNotice(DisconnectReason reason);
    void handleDatagram(const uint8_t* data, size_t size);
    void fail(DisconnectReason reason);
};

void NetClient::Impl::sendConnect() {
    std::vector<uint8_t> packet;
    writeHeader(packet, kTypeConnect, 0, 0, 0);
    putU32(packet, nonce);
    if (socket.sendTo(server, packet.data(), packet.size()) == static_cast<int>(packet.size())) {
        ++peer.stats.packetsSent;
        peer.stats.bytesSent += packet.size();
    }
}

void NetClient::Impl::sendDisconnectNotice(DisconnectReason reason) {
    std::vector<uint8_t> packet;
    writeHeader(packet, kTypeDisconnect, 0, 0, 0);
    packet.push_back(static_cast<uint8_t>(reason));
    socket.sendTo(server, packet.data(), packet.size());
}

void NetClient::Impl::fail(DisconnectReason reason) {
    if (state == State::Idle) return;
    state = State::Idle;
    peer.connected = false;
    socket.close();
    NetEvent event;
    event.type = NetEvent::Type::Disconnected;
    event.client = 0;
    event.reason = reason;
    events.push_back(event);
}

void NetClient::Impl::handleDatagram(const uint8_t* data, size_t size) {
    Header header;
    if (!parseHeader(data, size, &header)) {
        ++peer.stats.rejected;
        return;
    }
    const uint8_t* payload = data + kHeaderSize;
    const size_t payloadSize = size - kHeaderSize;

    if (header.type == kTypeAccept) {
        if (state != State::Connecting) return;
        ByteReader reader(payload, payloadSize);
        uint32_t acceptedNonce = 0;
        uint32_t sessionId = 0;
        if (!reader.u32(&acceptedNonce) || !reader.u32(&sessionId)) {
            ++peer.stats.rejected;
            return;
        }
        if (acceptedNonce != nonce) {
            ++peer.stats.rejected;
            return;
        }
        state = State::Connected;
        session = sessionId;
        peer.connected = true;
        peer.lastReceive = now;
        NetEvent event;
        event.type = NetEvent::Type::Connected;
        event.client = 0;
        events.push_back(event);
        return;
    }

    if (header.type == kTypeDisconnect) {
        if (state == State::Idle) return;
        DisconnectReason reason = DisconnectReason::Normal;
        if (payloadSize >= 1 && payload[0] <= static_cast<uint8_t>(DisconnectReason::Shutdown)) {
            reason = static_cast<DisconnectReason>(payload[0]);
        }
        fail(reason);
        return;
    }

    if (header.type != kTypeData || state != State::Connected) return;

    const bool fresh = peer.received.add(header.sequence);
    peer.ackPending = true;
    processAcks(peer, header.ack, header.ackBits, now);
    if (fresh) {
        processData(peer, payload, payloadSize, 0, events);
    } else {
        ++peer.stats.duplicates;
    }
}

NetClient::NetClient(const NetConfig& config) : impl_(new Impl(config)) {}

NetClient::~NetClient() = default;

bool NetClient::connect(const std::string& host, uint16_t port, std::string* error) {
    Impl& client = *impl_;
    client.socket.close();
    client.state = Impl::State::Idle;
    client.peer = Peer{};
    client.session = 0;
    client.now = 0.0;
    client.connectElapsed = 0.0f;
    client.retryElapsed = 0.0f;

    if (!NetAddress::parse(host, port, &client.server, error)) return false;
    if (!client.socket.open(0, error)) return false;

    client.nonce = nextNonce();
    client.state = Impl::State::Connecting;
    client.peer.address = client.server;
    client.sendConnect();
    return true;
}

void NetClient::disconnect() {
    Impl& client = *impl_;
    if (client.state == Impl::State::Connected) {
        // Three notices so one dropped datagram does not leave the server waiting for the timeout.
        for (int i = 0; i < 3; ++i) client.sendDisconnectNotice(DisconnectReason::Normal);
    }
    client.socket.close();
    client.state = Impl::State::Idle;
    client.peer.connected = false;
}

bool NetClient::isConnecting() const { return impl_->state == Impl::State::Connecting; }
bool NetClient::isConnected() const { return impl_->state == Impl::State::Connected; }

uint32_t NetClient::sessionId() const { return impl_->session; }

const NetAddress& NetClient::serverAddress() const { return impl_->server; }

bool NetClient::send(const void* data, size_t size, bool reliable) {
    Impl& client = *impl_;
    if (client.state != Impl::State::Connected) return false;
    // Queued here and written by tick(), so messages sent in one frame share datagrams.
    return queueMessage(client.peer, client.config, data, size, reliable);
}

void NetClient::tick(float dt) {
    Impl& client = *impl_;
    if (!client.socket.isOpen()) return;
    const float step = dt > 0.0f ? dt : 0.0f;
    client.now += static_cast<double>(step);

    if (client.state == Impl::State::Connecting) {
        client.connectElapsed += step;
        client.retryElapsed += step;
        if (client.retryElapsed >= client.config.connectRetryInterval) {
            client.retryElapsed = 0.0f;
            client.sendConnect();
        }
    }

    std::array<uint8_t, 2048> buffer{};
    for (int i = 0; i < 256; ++i) {
        NetAddress from;
        const int received = client.socket.recvFrom(&from, buffer.data(), buffer.size());
        if (received <= 0) break;
        if (from != client.server) continue;
        ++client.peer.stats.packetsReceived;
        client.peer.stats.bytesReceived += static_cast<uint64_t>(received);
        client.handleDatagram(buffer.data(), static_cast<size_t>(received));
    }

    if (client.state == Impl::State::Connecting &&
        client.connectElapsed >= client.config.connectTimeout) {
        client.fail(DisconnectReason::Timeout);
        return;
    }
    if (client.state != Impl::State::Connected) return;
    if (client.now - client.peer.lastReceive > static_cast<double>(client.config.disconnectTimeout)) {
        client.fail(DisconnectReason::Timeout);
        return;
    }
    flushPeer(client.peer, client.socket, client.config, client.now);
}

std::vector<NetEvent> NetClient::pollEvents() {
    std::vector<NetEvent> events;
    events.swap(impl_->events);
    return events;
}

const NetStats& NetClient::stats() const { return impl_->peer.stats; }

NetConfig NetClient::config() const { return impl_->config; }

// ---- Server ---------------------------------------------------------------------------------

struct NetServer::Impl {
    explicit Impl(const NetConfig& settings) : config(settings) {}

    NetConfig config;
    UdpSocket socket;
    std::map<int, Peer> clients;
    int nextClientId = 1;
    double now = 0.0;
    std::vector<NetEvent> events;

    void sendAccept(Peer& peer, uint32_t nonce);
    void sendDisconnectTo(const NetAddress& destination, DisconnectReason reason);
    void handleDatagram(const NetAddress& from, const uint8_t* data, size_t size);
};

void NetServer::Impl::sendAccept(Peer& peer, uint32_t nonce) {
    std::vector<uint8_t> packet;
    writeHeader(packet, kTypeAccept, 0, 0, 0);
    putU32(packet, nonce);
    putU32(packet, peer.sessionId);
    if (socket.sendTo(peer.address, packet.data(), packet.size()) ==
        static_cast<int>(packet.size())) {
        ++peer.stats.packetsSent;
        peer.stats.bytesSent += packet.size();
    }
}

void NetServer::Impl::sendDisconnectTo(const NetAddress& destination, DisconnectReason reason) {
    std::vector<uint8_t> packet;
    writeHeader(packet, kTypeDisconnect, 0, 0, 0);
    packet.push_back(static_cast<uint8_t>(reason));
    socket.sendTo(destination, packet.data(), packet.size());
}

void NetServer::Impl::handleDatagram(const NetAddress& from, const uint8_t* data, size_t size) {
    Header header;
    if (!parseHeader(data, size, &header)) return;
    const uint8_t* payload = data + kHeaderSize;
    const size_t payloadSize = size - kHeaderSize;

    if (header.type == kTypeConnect) {
        ByteReader reader(payload, payloadSize);
        uint32_t nonce = 0;
        if (!reader.u32(&nonce)) return;
        for (auto it = clients.begin(); it != clients.end(); ++it) {
            if (it->second.address != from) continue;
            if (it->second.nonce == nonce) {
                // A repeated connect means the accept was lost. Send it again instead of resetting
                // the session, so messages in flight are not dropped.
                it->second.lastReceive = now;
                sendAccept(it->second, nonce);
                return;
            }
            // The address is the same but the nonce is new: the client started over before the
            // server timed it out. Replace the session rather than rejecting its message ids.
            NetEvent closed;
            closed.type = NetEvent::Type::Disconnected;
            closed.client = it->first;
            closed.reason = DisconnectReason::Normal;
            events.push_back(closed);
            clients.erase(it);
            break;
        }
        if (static_cast<int>(clients.size()) >= config.maxClients) {
            sendDisconnectTo(from, DisconnectReason::ServerFull);
            return;
        }
        const int id = nextClientId++;
        Peer peer;
        peer.address = from;
        peer.connected = true;
        peer.nonce = nonce;
        peer.sessionId = static_cast<uint32_t>(id);
        peer.lastReceive = now;
        auto inserted = clients.emplace(id, std::move(peer));
        sendAccept(inserted.first->second, nonce);
        NetEvent event;
        event.type = NetEvent::Type::Connected;
        event.client = id;
        events.push_back(event);
        return;
    }

    auto found = clients.end();
    for (auto it = clients.begin(); it != clients.end(); ++it) {
        if (it->second.address == from) {
            found = it;
            break;
        }
    }
    if (found == clients.end()) {
        if (header.type == kTypeData) sendDisconnectTo(from, DisconnectReason::NotConnected);
        return;
    }

    const int id = found->first;
    Peer& peer = found->second;
    peer.lastReceive = now;
    ++peer.stats.packetsReceived;
    peer.stats.bytesReceived += static_cast<uint64_t>(size);

    if (header.type == kTypeData) {
        const bool fresh = peer.received.add(header.sequence);
        peer.ackPending = true;
        processAcks(peer, header.ack, header.ackBits, now);
        if (fresh) {
            processData(peer, payload, payloadSize, id, events);
        } else {
            ++peer.stats.duplicates;
        }
    } else if (header.type == kTypeDisconnect) {
        DisconnectReason reason = DisconnectReason::Normal;
        if (payloadSize >= 1 && payload[0] <= static_cast<uint8_t>(DisconnectReason::Shutdown)) {
            reason = static_cast<DisconnectReason>(payload[0]);
        }
        NetEvent event;
        event.type = NetEvent::Type::Disconnected;
        event.client = id;
        event.reason = reason;
        events.push_back(event);
        clients.erase(found);
    }
}

NetServer::NetServer(const NetConfig& config) : impl_(new Impl(config)) {}

NetServer::~NetServer() = default;

bool NetServer::start(uint16_t port, std::string* error) {
    Impl& server = *impl_;
    server.socket.close();
    server.clients.clear();
    server.events.clear();
    server.nextClientId = 1;
    server.now = 0.0;
    return server.socket.open(port, error);
}

void NetServer::stop() {
    Impl& server = *impl_;
    server.socket.close();
    server.clients.clear();
}

bool NetServer::isRunning() const { return impl_->socket.isOpen(); }

uint16_t NetServer::port() const { return impl_->socket.localPort(); }

void NetServer::tick(float dt) {
    Impl& server = *impl_;
    if (!server.socket.isOpen()) return;
    const float step = dt > 0.0f ? dt : 0.0f;
    server.now += static_cast<double>(step);

    std::array<uint8_t, 2048> buffer{};
    for (int i = 0; i < 256; ++i) {
        NetAddress from;
        const int received = server.socket.recvFrom(&from, buffer.data(), buffer.size());
        if (received <= 0) break;
        server.handleDatagram(from, buffer.data(), static_cast<size_t>(received));
    }

    std::vector<int> timedOut;
    for (const auto& entry : server.clients) {
        if (server.now - entry.second.lastReceive >
            static_cast<double>(server.config.disconnectTimeout)) {
            timedOut.push_back(entry.first);
        }
    }
    for (int id : timedOut) {
        auto it = server.clients.find(id);
        if (it == server.clients.end()) continue;
        NetEvent event;
        event.type = NetEvent::Type::Disconnected;
        event.client = id;
        event.reason = DisconnectReason::Timeout;
        server.events.push_back(event);
        server.clients.erase(it);
    }

    for (auto& entry : server.clients) {
        flushPeer(entry.second, server.socket, server.config, server.now);
    }
}

std::vector<NetEvent> NetServer::pollEvents() {
    std::vector<NetEvent> events;
    events.swap(impl_->events);
    return events;
}

bool NetServer::send(int clientId, const void* data, size_t size, bool reliable) {
    Impl& server = *impl_;
    auto it = server.clients.find(clientId);
    if (it == server.clients.end()) return false;
    // Queued here and written by tick(), so messages sent in one frame share datagrams.
    return queueMessage(it->second, server.config, data, size, reliable);
}

int NetServer::broadcast(const void* data, size_t size, bool reliable) {
    Impl& server = *impl_;
    int sent = 0;
    for (auto& entry : server.clients) {
        if (queueMessage(entry.second, server.config, data, size, reliable)) ++sent;
    }
    return sent;
}

std::vector<int> NetServer::clients() const {
    std::vector<int> ids;
    ids.reserve(impl_->clients.size());
    for (const auto& entry : impl_->clients) ids.push_back(entry.first);
    return ids;
}

bool NetServer::clientAddress(int clientId, NetAddress* address) const {
    auto it = impl_->clients.find(clientId);
    if (it == impl_->clients.end()) return false;
    if (address) *address = it->second.address;
    return true;
}

const NetStats& NetServer::stats(int clientId) const {
    auto it = impl_->clients.find(clientId);
    return it == impl_->clients.end() ? kEmptyStats : it->second.stats;
}

NetConfig NetServer::config() const { return impl_->config; }

} // namespace ukiyo
