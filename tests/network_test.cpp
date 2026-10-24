// Headless checks for the networking layer: serialisation, addresses, UDP sockets, the client/server
// handshake, reliable ordering, retransmission through a lossy relay, and session timeouts.
// Needs no window, GPU, or external services.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "systems/network.h"

using namespace ukiyo;

static int passed = 0;
static int failed = 0;

static void check(bool ok, const std::string& name, const std::string& detail = "") {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(),
                detail.empty() ? "" : "  -- ", detail.c_str());
    std::fflush(stdout);
    ok ? ++passed : ++failed;
}

static std::vector<uint8_t> indexedMessage(uint32_t index, size_t payloadBytes) {
    ByteWriter writer;
    writer.u32(index);
    for (size_t i = 0; i < payloadBytes; ++i) {
        writer.u8(static_cast<uint8_t>((index * 31 + i) & 0xff));
    }
    return writer.data();
}

// ---- Serialisation --------------------------------------------------------------------------

static void testSerialisation() {
    ByteWriter writer;
    writer.u8(0x12);
    writer.u16(0x3456);
    writer.u32(0x89abcdefu);
    writer.u64(0x0123456789abcdefull);
    writer.i32(-2);
    writer.f32(1.5f);
    writer.string("ukiyo");
    const uint8_t raw[3] = {1, 2, 3};
    writer.bytes(raw, sizeof raw);

    const std::vector<uint8_t>& bytes = writer.data();
    check(bytes.size() == 33, "serialisation: every value appends its expected size",
          std::to_string(bytes.size()));
    check(bytes[0] == 0x12 && bytes[1] == 0x56 && bytes[2] == 0x34 && bytes[3] == 0xef &&
              bytes[4] == 0xcd,
          "serialisation: numbers are written little endian");

    ByteReader reader(bytes);
    uint8_t u8 = 0;
    uint16_t u16 = 0;
    uint32_t u32 = 0;
    uint64_t u64 = 0;
    int32_t i32 = 0;
    float f32 = 0.0f;
    std::string text;
    uint8_t rawBack[3] = {};
    const bool readAll = reader.u8(&u8) && reader.u16(&u16) && reader.u32(&u32) &&
                         reader.u64(&u64) && reader.i32(&i32) && reader.f32(&f32) &&
                         reader.string(&text) && reader.bytes(rawBack, sizeof rawBack);
    check(readAll && reader.remaining() == 0 && reader.ok(),
          "serialisation: every value reads back and the buffer is consumed");
    check(u8 == 0x12 && u16 == 0x3456 && u32 == 0x89abcdefu && u64 == 0x0123456789abcdefull,
          "serialisation: unsigned values round trip");
    check(i32 == -2 && f32 == 1.5f, "serialisation: signed and floating point values round trip");
    check(text == "ukiyo" && std::memcmp(rawBack, raw, sizeof raw) == 0,
          "serialisation: strings and raw bytes round trip");

    const uint8_t shortBuffer[2] = {0, 0};
    ByteReader shortReader(shortBuffer, sizeof shortBuffer);
    uint32_t missing = 0;
    check(!shortReader.u32(&missing), "serialisation: a read past the end fails");
    check(!shortReader.ok(), "serialisation: the reader stays failed after a short read");
    uint16_t alsoMissing = 0;
    check(!shortReader.u16(&alsoMissing), "serialisation: later reads keep failing");

    const uint8_t badString[3] = {5, 0, 'a'};
    ByteReader stringReader(badString, sizeof badString);
    std::string badText;
    check(!stringReader.string(&badText), "serialisation: a string longer than the buffer fails");

    ByteWriter empty;
    empty.string("");
    ByteReader emptyReader(empty.data());
    std::string emptyText = "not empty";
    check(emptyReader.string(&emptyText) && emptyText.empty(),
          "serialisation: an empty string round trips");
}

// ---- Addresses ------------------------------------------------------------------------------

static void testAddresses() {
    NetAddress address;
    std::string error;
    check(NetAddress::parse("127.0.0.1", 27015, &address, &error) &&
              address.valid() && address.port == 27015,
          "address: a dotted quad resolves", error);
    check(address.toString() == "127.0.0.1:27015", "address: it prints as host:port",
          address.toString());

    NetAddress local;
    check(NetAddress::parse("localhost", 1234, &local, &error) && local.valid(),
          "address: a host name resolves", error);

    NetAddress bad;
    error.clear();
    check(!NetAddress::parse("999.999.999.999", 1, &bad, &error) && !error.empty(),
          "address: an impossible host is refused with an error", error);
}

// ---- Sockets --------------------------------------------------------------------------------

static void testSockets() {
    UdpSocket sender;
    UdpSocket receiver;
    std::string error;
    const bool opened = sender.open(0, &error) && receiver.open(0, &error);
    check(opened && sender.localPort() != 0 && receiver.localPort() != 0 &&
              sender.localPort() != receiver.localPort(),
          "sockets: two sockets bind to different ephemeral ports", error);

    UdpSocket conflicting;
    error.clear();
    check(!conflicting.open(receiver.localPort(), &error) && !error.empty(),
          "sockets: a port that is already in use is refused", error);

    NetAddress destination;
    NetAddress::parse("127.0.0.1", receiver.localPort(), &destination);
    const char message[] = "hello";
    check(sender.sendTo(destination, message, sizeof message) ==
              static_cast<int>(sizeof message),
          "sockets: a datagram is sent");

    NetAddress from;
    char buffer[32] = {};
    const int received = receiver.recvFrom(&from, buffer, sizeof buffer);
    check(received == static_cast<int>(sizeof message) &&
              std::memcmp(buffer, message, sizeof message) == 0,
          "sockets: the receiver gets the bytes");
    NetAddress senderAddress;
    NetAddress::parse("127.0.0.1", sender.localPort(), &senderAddress);
    check(from == senderAddress, "sockets: the source address is reported", from.toString());

    check(receiver.recvFrom(&from, buffer, sizeof buffer) == 0,
          "sockets: an empty socket reports no datagram rather than blocking");
}

// ---- A relay that loses and duplicates datagrams -------------------------------------------

class LossyRelay {
public:
    bool open(std::string* error) { return socket_.open(0, error); }
    uint16_t port() const { return socket_.localPort(); }
    void setServer(const NetAddress& address) { server_ = address; }
    int dropped() const { return dropped_; }

    void pump() {
        for (int i = 0; i < 512; ++i) {
            NetAddress from;
            uint8_t buffer[2048] = {};
            const int received = socket_.recvFrom(&from, buffer, sizeof buffer);
            if (received <= 0) break;
            if (from == server_) {
                if (!haveClient_) continue;
                forward(buffer, received, client_);
            } else {
                client_ = from;
                haveClient_ = true;
                forward(buffer, received, server_);
            }
        }
    }

private:
    void forward(const uint8_t* data, int size, const NetAddress& destination) {
        const int ticket = ++packets_;
        if (ticket % 3 == 0) {
            ++dropped_;
            return;
        }
        socket_.sendTo(destination, data, static_cast<size_t>(size));
        if (ticket % 7 == 0) {
            // A duplicate datagram: the session layer must ignore it.
            socket_.sendTo(destination, data, static_cast<size_t>(size));
        }
    }

    UdpSocket socket_;
    NetAddress server_;
    NetAddress client_;
    bool haveClient_ = false;
    int packets_ = 0;
    int dropped_ = 0;
};

// ---- A client and server driven in lockstep -------------------------------------------------

struct Session {
    Session(const NetConfig& serverConfig = NetConfig(), const NetConfig& clientConfig = NetConfig())
        : server(serverConfig), client(clientConfig) {}

    NetServer server;
    NetClient client;
    LossyRelay* relay = nullptr;
    std::vector<NetEvent> clientEvents;
    std::vector<NetEvent> serverEvents;

    void step(float dt = 0.01f) {
        client.tick(dt);
        if (relay) relay->pump();
        server.tick(dt);
        if (relay) relay->pump();
        drain();
    }

    void drain() {
        for (const NetEvent& event : client.pollEvents()) clientEvents.push_back(event);
        for (const NetEvent& event : server.pollEvents()) serverEvents.push_back(event);
    }

    bool waitFor(const std::function<bool()>& done, int maxSteps = 2000, float dt = 0.01f) {
        if (done()) return true;
        for (int i = 0; i < maxSteps; ++i) {
            step(dt);
            if (done()) return true;
        }
        return done();
    }

    size_t clientMessages() const {
        size_t count = 0;
        for (const NetEvent& event : clientEvents) {
            if (event.type == NetEvent::Type::Message) ++count;
        }
        return count;
    }

    size_t serverMessages() const {
        size_t count = 0;
        for (const NetEvent& event : serverEvents) {
            if (event.type == NetEvent::Type::Message) ++count;
        }
        return count;
    }

    std::vector<uint32_t> serverMessageIndices() const {
        std::vector<uint32_t> indices;
        for (const NetEvent& event : serverEvents) {
            if (event.type != NetEvent::Type::Message) continue;
            ByteReader reader(event.message);
            uint32_t index = 0;
            if (reader.u32(&index)) indices.push_back(index);
        }
        return indices;
    }

    std::vector<uint32_t> clientMessageIndices() const {
        std::vector<uint32_t> indices;
        for (const NetEvent& event : clientEvents) {
            if (event.type != NetEvent::Type::Message) continue;
            ByteReader reader(event.message);
            uint32_t index = 0;
            if (reader.u32(&index)) indices.push_back(index);
        }
        return indices;
    }
};

static bool strictlyIncreasingFromZero(const std::vector<uint32_t>& values, uint32_t count) {
    if (values.size() != count) return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (values[i] != i) return false;
    }
    return true;
}

// ---- Handshake and messages -----------------------------------------------------------------

static void testSession() {
    Session session;
    std::string error;
    check(session.server.start(0, &error), "session: the server starts", error);
    check(session.client.connect("127.0.0.1", session.server.port(), &error),
          "session: connect begins", error);

    bool clientConnected = false;
    bool serverConnected = false;
    int clientId = -1;
    const bool handshake = session.waitFor([&] {
        for (const NetEvent& event : session.clientEvents) {
            if (event.type == NetEvent::Type::Connected) clientConnected = true;
        }
        for (const NetEvent& event : session.serverEvents) {
            if (event.type == NetEvent::Type::Connected) {
                serverConnected = true;
                clientId = event.client;
            }
        }
        return clientConnected && serverConnected;
    });
    check(handshake && session.client.isConnected(),
          "session: the handshake completes on both sides");
    check(session.server.clients().size() == 1 && clientId >= 0,
          "session: the server accepts one client");
    check(session.client.sessionId() == static_cast<uint32_t>(clientId),
          "session: both sides agree on the client id",
          std::to_string(session.client.sessionId()) + " vs " + std::to_string(clientId));

    ByteWriter ping;
    ping.u32(7);
    ping.string("ping");
    check(session.client.send(ping.data(), true), "session: the client queues a reliable message");
    check(session.waitFor([&] { return session.serverMessages() == 1; }),
          "session: the server receives it");
    check(!session.serverEvents.empty() &&
              session.serverEvents.back().type == NetEvent::Type::Message &&
              session.serverEvents.back().client == clientId &&
              session.serverEvents.back().reliable,
          "session: the event names the client and marks the message reliable");
    if (!session.serverEvents.empty()) {
        ByteReader reader(session.serverEvents.back().message);
        uint32_t number = 0;
        std::string text;
        check(reader.u32(&number) && reader.string(&text) && number == 7 && text == "ping",
              "session: the payload arrives unchanged");
    }

    ByteWriter pong;
    pong.u32(9);
    pong.string("pong");
    check(session.server.send(clientId, pong.data(), true),
          "session: the server sends a reliable message");
    check(session.waitFor([&] { return session.clientMessages() == 1; }),
          "session: the client receives it");
    if (!session.clientEvents.empty()) {
        ByteReader reader(session.clientEvents.back().message);
        uint32_t number = 0;
        std::string text;
        check(reader.u32(&number) && reader.string(&text) && number == 9 && text == "pong",
              "session: the server payload arrives unchanged");
    }

    session.client.disconnect();
    check(session.waitFor([&] {
              for (const NetEvent& event : session.serverEvents) {
                  if (event.type == NetEvent::Type::Disconnected && event.client == clientId) {
                      return true;
                  }
              }
              return session.server.clients().empty();
          }, 200),
          "session: disconnecting removes the client on the server");
    check(!session.client.isConnected(), "session: the client reports that it is closed");
}

// ---- Reliable ordering ----------------------------------------------------------------------

static void testReliableOrder() {
    Session session;
    std::string error;
    session.server.start(0, &error);
    session.client.connect("127.0.0.1", session.server.port(), &error);
    check(session.waitFor([&] { return session.client.isConnected(); }, 500),
          "ordering: the session connects", error);

    const uint32_t count = 40;
    bool queued = true;
    for (uint32_t i = 0; i < count; ++i) {
        queued = session.client.send(indexedMessage(i, 60), true) && queued;
    }
    check(queued, "ordering: all reliable messages are accepted");
    const bool allArrived = session.waitFor([&] { return session.serverMessages() == count; }, 1000);
    check(allArrived, "ordering: every reliable message arrives",
          std::to_string(session.serverMessages()) + " of " + std::to_string(count));
    const std::vector<uint32_t> indices = session.serverMessageIndices();
    check(strictlyIncreasingFromZero(indices, count),
          "ordering: reliable messages arrive exactly once, in order",
          std::to_string(indices.size()) + " messages");

    for (uint32_t i = 0; i < 8; ++i) {
        session.client.send(indexedMessage(i, 8), false);
    }
    session.waitFor([&] { return session.serverMessages() >= count + 8; }, 300);
    size_t unreliable = 0;
    for (const NetEvent& event : session.serverEvents) {
        if (event.type == NetEvent::Type::Message && !event.reliable) ++unreliable;
    }
    check(unreliable >= 6, "ordering: unreliable messages are delivered when they arrive",
          std::to_string(unreliable) + " of 8");
}

// ---- Loss, duplication, and retransmission --------------------------------------------------

static void testLossySession() {
    Session session;
    LossyRelay relay;
    std::string error;
    check(relay.open(&error), "loss: the relay socket opens", error);
    NetAddress serverAddress;
    NetAddress::parse("127.0.0.1", 0, &serverAddress);
    check(session.server.start(0, &error), "loss: the server starts", error);
    NetAddress::parse("127.0.0.1", session.server.port(), &serverAddress);
    relay.setServer(serverAddress);
    session.relay = &relay;

    check(session.client.connect("127.0.0.1", relay.port(), &error),
          "loss: the client connects through the relay", error);
    check(session.waitFor([&] { return session.client.isConnected(); }, 2000),
          "loss: the handshake survives dropped datagrams");
    const std::vector<int> clients = session.server.clients();
    check(clients.size() == 1, "loss: the server sees the client through the relay");
    if (clients.empty()) return;
    const int clientId = clients.front();

    const uint32_t count = 30;
    for (uint32_t i = 0; i < count; ++i) {
        session.client.send(indexedMessage(i, 200), true);
    }
    const bool allRequested = session.waitFor([&] { return session.serverMessages() == count; }, 5000);
    check(allRequested, "loss: every reliable client message is retransmitted until it arrives",
          std::to_string(session.serverMessages()) + " of " + std::to_string(count));
    check(strictlyIncreasingFromZero(session.serverMessageIndices(), count),
          "loss: duplicates do not disturb reliable ordering");
    check(relay.dropped() > 0, "loss: the relay actually dropped datagrams");
    check(session.client.stats().retransmits > 0,
          "loss: the client retransmitted at least one packet",
          std::to_string(session.client.stats().retransmits));

    const uint32_t replies = 20;
    for (uint32_t i = 0; i < replies; ++i) {
        session.server.send(clientId, indexedMessage(i, 200), true);
    }
    const bool allReplied = session.waitFor([&] { return session.clientMessages() == replies; }, 5000);
    check(allReplied, "loss: every reliable server message arrives",
          std::to_string(session.clientMessages()) + " of " + std::to_string(replies));
    check(strictlyIncreasingFromZero(session.clientMessageIndices(), replies),
          "loss: server messages also arrive in order");
    check(session.server.stats(clientId).retransmits > 0,
          "loss: the server retransmitted at least one packet",
          std::to_string(session.server.stats(clientId).retransmits));
}

// ---- Timeouts -------------------------------------------------------------------------------

static void testTimeout() {
    NetConfig config;
    config.disconnectTimeout = 0.5f;
    config.heartbeatInterval = 0.2f;
    NetServer server(config);
    NetClient client(config);
    std::string error;
    check(server.start(0, &error), "timeout: the server starts", error);
    check(client.connect("127.0.0.1", server.port(), &error), "timeout: the client connects", error);

    bool connected = false;
    for (int i = 0; i < 500 && !connected; ++i) {
        client.tick(0.01f);
        server.tick(0.01f);
        for (const NetEvent& event : client.pollEvents()) {
            if (event.type == NetEvent::Type::Connected) connected = true;
        }
        server.pollEvents();
    }
    check(connected, "timeout: the session connects first");

    std::vector<NetEvent> events;
    for (int i = 0; i < 200; ++i) {
        server.tick(0.01f);
        for (const NetEvent& event : server.pollEvents()) events.push_back(event);
    }
    bool timedOut = false;
    for (const NetEvent& event : events) {
        if (event.type == NetEvent::Type::Disconnected && event.reason == DisconnectReason::Timeout) {
            timedOut = true;
        }
    }
    check(timedOut, "timeout: a silent client is dropped with a timeout");
    check(server.clients().empty(), "timeout: the dropped client is gone");
}

static void testConnectTimeout() {
    NetConfig config;
    config.connectTimeout = 0.2f;
    config.connectRetryInterval = 0.05f;
    NetClient client(config);
    std::string error;
    check(client.connect("127.0.0.1", 9, &error),
          "timeout: a client can try a server that is not there", error);
    bool timedOut = false;
    for (int i = 0; i < 50 && !timedOut; ++i) {
        client.tick(0.01f);
        for (const NetEvent& event : client.pollEvents()) {
            if (event.type == NetEvent::Type::Disconnected &&
                event.reason == DisconnectReason::Timeout) {
                timedOut = true;
            }
        }
    }
    check(timedOut && !client.isConnecting() && !client.isConnected(),
          "timeout: a connect attempt with no answer times out");
}

// ---- Limits and capacity --------------------------------------------------------------------

static void testLimits() {
    NetConfig config;
    config.maxClients = 1;
    config.connectTimeout = 0.6f;
    NetServer server(config);
    NetClient first;
    NetClient second;
    std::string error;
    check(server.start(0, &error), "limits: the server starts", error);
    first.connect("127.0.0.1", server.port(), &error);
    second.connect("127.0.0.1", server.port(), &error);

    std::vector<NetEvent> firstEvents;
    std::vector<NetEvent> secondEvents;
    for (int i = 0; i < 300; ++i) {
        first.tick(0.01f);
        second.tick(0.01f);
        server.tick(0.01f);
        for (const NetEvent& event : first.pollEvents()) firstEvents.push_back(event);
        for (const NetEvent& event : second.pollEvents()) secondEvents.push_back(event);
        server.pollEvents();
    }

    NetClient* connected = first.isConnected() ? &first : (second.isConnected() ? &second : nullptr);
    NetClient* refusedClient = connected == &first ? &second : &first;
    check(server.clients().size() == 1 && connected != nullptr,
          "limits: exactly one client connects when maxClients is one");
    bool refused = false;
    for (const NetEvent& event : firstEvents) {
        if (event.type == NetEvent::Type::Disconnected && event.reason == DisconnectReason::ServerFull) {
            refused = true;
        }
    }
    for (const NetEvent& event : secondEvents) {
        if (event.type == NetEvent::Type::Disconnected && event.reason == DisconnectReason::ServerFull) {
            refused = true;
        }
    }
    check(refused && !refusedClient->isConnected() && !refusedClient->isConnecting(),
          "limits: the extra client is refused with a reason");

    const std::vector<int> clients = server.clients();
    if (clients.empty() || !connected) return;
    const int clientId = clients.front();
    check(!server.send(clientId + 100, "x", 1, true), "limits: sending to an unknown client fails");
    check(!server.send(clientId, "x", static_cast<size_t>(config.maxMessageSize) + 1, true),
          "limits: an oversized message is refused");
    check(server.send(clientId, "x", 1, true), "limits: a message at the default limit is accepted");
    check(!server.send(clientId, nullptr, 1, true),
          "limits: a null payload with a non-zero size is refused");
    check(connected->send(nullptr, 0, true), "limits: an empty reliable message is allowed");
}

int main() {
    testSerialisation();
    testAddresses();
    testSockets();
    testSession();
    testReliableOrder();
    testLossySession();
    testTimeout();
    testConnectTimeout();
    testLimits();
    std::printf("network: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
