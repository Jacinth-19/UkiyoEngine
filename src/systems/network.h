// UDP networking: sockets, byte serialisation, and client/server sessions with a small reliable
// message layer. The layer is single-threaded: call tick() once a frame and drain pollEvents().
//
// Wire format (little endian):
//   header  magic "UK" (2 bytes), version (u8), type (u8), sequence (u16), ack (u16), ack bits (u32)
//   connect nonce (u32)
//   accept  nonce (u32), client id (u32)
//   data    message count (u16), then per message: flags (u8), id (u16, reliable only), size (u16), bytes
//
// Reliable messages are retransmitted until acknowledged and delivered once, in order. Unreliable
// messages are delivered once at most and may be reordered or lost. The session layer adds a
// handshake, heartbeats, and timeouts on top. Datagrams are limited to 1200 bytes, so a message is at
// most maxMessageSize bytes and is never fragmented across datagrams.
//
// The protocol is meant for trusted networks (LAN, local development, a game server that validates
// input). It is not encrypted and does not authenticate peers.
//
// Typical use, driven once a frame:
//
//   ukiyo::NetServer server;
//   server.start(7777);
//   ukiyo::NetClient client;
//   client.connect("127.0.0.1", 7777);
//   ...
//   server.tick(dt);
//   client.tick(dt);
//   for (const ukiyo::NetEvent& event : server.pollEvents()) {
//       if (event.type == ukiyo::NetEvent::Type::Message) {
//           server.send(event.client, event.message.data(), event.message.size(), true);
//       }
//   }
//   for (const ukiyo::NetEvent& event : client.pollEvents()) { ... }
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ukiyo {

// ---- Byte serialisation ---------------------------------------------------------------------

// Appends little-endian values to a byte buffer. Strings are a u16 length followed by the bytes.
class ByteWriter {
public:
    void u8(uint8_t value);
    void u16(uint16_t value);
    void u32(uint32_t value);
    void u64(uint64_t value);
    void i32(int32_t value) { u32(static_cast<uint32_t>(value)); }
    void f32(float value);
    // Raw bytes, with no length prefix. Use string() for a length-prefixed blob.
    void bytes(const void* data, size_t size);
    // Length-prefixed blob. Longer strings keep their first 65535 bytes.
    void string(const std::string& value);

    const std::vector<uint8_t>& data() const { return data_; }
    size_t size() const { return data_.size(); }
    void clear() { data_.clear(); }

private:
    std::vector<uint8_t> data_;
};

// Reads values written by ByteWriter. A short read fails and leaves the reader marked bad; later
// reads keep failing so callers can check once after reading a whole packet.
class ByteReader {
public:
    ByteReader(const void* data, size_t size);
    explicit ByteReader(const std::vector<uint8_t>& data);

    bool u8(uint8_t* value);
    bool u16(uint16_t* value);
    bool u32(uint32_t* value);
    bool u64(uint64_t* value);
    bool i32(int32_t* value);
    bool f32(float* value);
    bool bytes(void* destination, size_t size);
    bool string(std::string* value);

    size_t remaining() const { return size_ - position_; }
    size_t position() const { return position_; }
    bool ok() const { return ok_; }
    const uint8_t* cursor() const { return data_ + position_; }

private:
    bool take(void* destination, size_t size);
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t position_ = 0;
    bool ok_ = true;
};

// ---- Addresses and sockets ------------------------------------------------------------------

// An IPv4 endpoint. `ip` is in network byte order (as stored in sockaddr_in), `port` is in host
// byte order.
struct NetAddress {
    uint32_t ip = 0;
    uint16_t port = 0;

    bool valid() const { return ip != 0; }
    std::string toString() const;
    bool operator==(const NetAddress& other) const { return ip == other.ip && port == other.port; }
    bool operator!=(const NetAddress& other) const { return !(*this == other); }

    // Resolves a dotted quad or a host name (for example "127.0.0.1" or "localhost").
    static bool parse(const std::string& host, uint16_t port, NetAddress* address,
                      std::string* error = nullptr);
};

// A non-blocking UDP socket bound to INADDR_ANY. Move-only; not thread-safe.
class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Binds to `port`, or to an ephemeral port when `port` is 0. Returns false and fills `error` on
    // failure. Opening an already open socket closes it first.
    bool open(uint16_t port, std::string* error = nullptr);
    void close();
    bool isOpen() const { return handle_ != kClosed; }
    uint16_t localPort() const { return localPort_; }
    const std::string& lastError() const { return error_; }

    // Returns the number of bytes sent, 0 when the send would block, or -1 on error.
    int sendTo(const NetAddress& destination, const void* data, size_t size);
    // Returns the number of bytes received, 0 when no datagram is waiting, or -1 on error.
    int recvFrom(NetAddress* from, void* data, size_t capacity);

private:
    static constexpr std::intptr_t kClosed = -1;
    std::intptr_t handle_ = kClosed;
    uint16_t localPort_ = 0;
    std::string error_;
};

// ---- Sessions -------------------------------------------------------------------------------

struct NetConfig {
    int maxClients = 32;
    // Seconds a client may sit in the connecting state before the attempt fails.
    float connectTimeout = 5.0f;
    // Seconds without a packet from the peer before the session is dropped.
    float disconnectTimeout = 5.0f;
    // A session with nothing to send emits an empty packet this often, so the peer stays alive.
    float heartbeatInterval = 1.0f;
    // Seconds between connect requests while waiting for an accept.
    float connectRetryInterval = 0.25f;
    // Retransmission timeout bounds, in seconds. The measured round trip sets the timeout between them.
    float rtoMin = 0.05f;
    float rtoMax = 0.5f;
    // Largest accepted message in bytes. Must leave room for the headers inside a 1200-byte datagram.
    size_t maxMessageSize = 1024;
};

struct NetStats {
    uint64_t packetsSent = 0;
    uint64_t packetsReceived = 0;
    uint64_t bytesSent = 0;
    uint64_t bytesReceived = 0;
    uint64_t messagesSent = 0;       // reliable messages count once, when first sent
    uint64_t messagesReceived = 0;   // messages delivered to the application
    uint64_t retransmits = 0;
    uint64_t duplicates = 0;         // duplicate packets or reliable messages ignored
    uint64_t rejected = 0;           // datagrams that failed validation
    float rtt = 0.0f;                // smoothed round-trip time in seconds; 0 until measured
};

enum class DisconnectReason : uint8_t {
    Normal = 0,
    Timeout = 1,
    ServerFull = 2,
    NotConnected = 3,
    ProtocolError = 4,
    Shutdown = 5,
};

struct NetEvent {
    enum class Type { Connected, Disconnected, Message };

    Type type = Type::Message;
    // Server events carry the client id. Client events use 0.
    int client = 0;
    // Message payload for Type::Message.
    std::vector<uint8_t> message;
    bool reliable = false;
    DisconnectReason reason = DisconnectReason::Normal;
};

// A client session. connect() starts a non-blocking handshake; tick() retries it until the server
// accepts or connectTimeout passes. Sends are queued and written by tick(), so every message sent in
// one frame can share a datagram. Call tick() even on frames with nothing to send: it also drives
// retransmissions, acknowledgements, and heartbeats.
class NetClient {
public:
    explicit NetClient(const NetConfig& config = NetConfig());
    ~NetClient();
    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    bool connect(const std::string& host, uint16_t port, std::string* error = nullptr);
    // Sends a disconnect notice and closes the socket. Does not raise a Disconnected event.
    void disconnect();

    bool isConnecting() const;
    bool isConnected() const;
    uint32_t sessionId() const;
    const NetAddress& serverAddress() const;

    bool send(const void* data, size_t size, bool reliable = true);
    bool send(const std::vector<uint8_t>& message, bool reliable = true) {
        return send(message.data(), message.size(), reliable);
    }

    // Advances handshake retries, retransmissions, heartbeats, and the receive queue. Call every frame.
    void tick(float dt);
    std::vector<NetEvent> pollEvents();
    const NetStats& stats() const;
    NetConfig config() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A server that accepts connections from clients and exchanges messages with each of them.
class NetServer {
public:
    explicit NetServer(const NetConfig& config = NetConfig());
    ~NetServer();
    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;

    // Binds to `port`, or to an ephemeral port when `port` is 0 (read it back with port()).
    bool start(uint16_t port, std::string* error = nullptr);
    // Closes the socket and drops every client without raising events.
    void stop();
    bool isRunning() const;
    uint16_t port() const;

    void tick(float dt);
    std::vector<NetEvent> pollEvents();

    bool send(int clientId, const void* data, size_t size, bool reliable = true);
    bool send(int clientId, const std::vector<uint8_t>& message, bool reliable = true) {
        return send(clientId, message.data(), message.size(), reliable);
    }
    // Sends to every connected client. Returns the number that accepted the message.
    int broadcast(const void* data, size_t size, bool reliable = true);
    int broadcast(const std::vector<uint8_t>& message, bool reliable = true) {
        return broadcast(message.data(), message.size(), reliable);
    }

    std::vector<int> clients() const;
    bool clientAddress(int clientId, NetAddress* address) const;
    // Stats for a client, or an empty set when the id is unknown.
    const NetStats& stats(int clientId) const;
    NetConfig config() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ukiyo
