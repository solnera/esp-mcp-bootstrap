#include <unity.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "BLEMCPServer.h"
#include "mcp_transport.h"

constexpr uint8_t EXPECT_CONTROL_HEADER = 0x3F;
constexpr uint8_t EXPECT_CONTROL_ERROR = 0x01;
constexpr uint8_t EXPECT_ERR_BAD_SEQUENCE = 2;
constexpr uint8_t EXPECT_ERR_BUSY = 6;

class BlockingHandler : public ToolHandler {
public:
    BlockingHandler(std::atomic<bool>& started, std::atomic<bool>& release)
        : started_(started), release_(release) {}

    JsonDocument call(JsonVariantConst params) override {
        (void)params;
        started_.store(true);
        while (!release_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        JsonDocument result;
        result["ok"] = true;
        return result;
    }

private:
    std::atomic<bool>& started_;
    std::atomic<bool>& release_;
};

class SlowBlockingHandler : public ToolHandler {
public:
    SlowBlockingHandler(std::atomic<bool>& started, std::atomic<bool>& finished, std::atomic<bool>& release)
        : started_(started), finished_(finished), release_(release) {}

    JsonDocument call(JsonVariantConst params) override {
        (void)params;
        started_.store(true);
        while (!release_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        JsonDocument result;
        result["ok"] = true;
        finished_.store(true);
        return result;
    }

private:
    std::atomic<bool>& started_;
    std::atomic<bool>& finished_;
    std::atomic<bool>& release_;
};

static void sendSingleMessage(const char* message) {
    size_t len = strlen(message);
    std::vector<uint8_t> packet(len + 1);
    packet[0] = 0x00;
    memcpy(packet.data() + 1, message, len);
    mcp_transport_receive(packet.data(), packet.size());
}

class PingHandler : public ToolHandler {
public:
    JsonDocument call(JsonVariantConst params) override {
        (void)params;
        JsonDocument result;
        result["ok"] = true;
        return result;
    }
};

void test_concurrent_sends_are_serialized(void) {
    BLEMCPServer server("test-ble", "1.0.0");
    server.begin();

    auto& ble = McpBle::getInstance();
    ble._onConnect(nullptr);
    ble._onMtuChange(247);  // larger MTU → each message is a few packets

    mock_ble::resetNotifyTracking();

    /* Two threads push messages through the same send path begin() wired up.
     * The transport must serialize them so notifications never interleave. */
    std::string big(600, 'A');
    auto sender = [&big]() {
        for (int i = 0; i < 10; i++) {
            mcp_transport_send_message(big.c_str());
        }
    };
    std::thread t1(sender);
    std::thread t2(sender);
    t1.join();
    t2.join();

    int maxConcurrent = mock_ble::notifyMaxConcurrent().load();

    ble._onDisconnect(nullptr);
    server.end();

    TEST_ASSERT_EQUAL_INT(1, maxConcurrent);
}

void test_second_connection_is_rejected_without_dropping_active_session(void) {
    BLEMCPServer server("test-ble", "1.0.0");
    server.begin();

    auto& ble = McpBle::getInstance();
    auto* nimServer = NimBLEDevice::createServer();
    NimBLEConnInfo first(7);
    NimBLEConnInfo second(9);

    mock_ble::resetConnectionTracking();
    bool firstAccepted = ble._onConnect(nimServer, first);
    bool secondAccepted = ble._onConnect(nimServer, second);

    TEST_ASSERT_TRUE(firstAccepted);
    TEST_ASSERT_FALSE(secondAccepted);
    TEST_ASSERT_TRUE(ble.isConnected());
    TEST_ASSERT_EQUAL_UINT16(7, ble.activeConnHandle());
    TEST_ASSERT_EQUAL_INT(1, mock_ble::disconnectCount().load());
    TEST_ASSERT_EQUAL_INT(9, mock_ble::lastDisconnectConnHandle().load());

    ble._onDisconnect(nimServer, second, 0);
    TEST_ASSERT_TRUE(ble.isConnected());
    TEST_ASSERT_EQUAL_UINT16(7, ble.activeConnHandle());

    ble._onDisconnect(nimServer, first, 0);
    TEST_ASSERT_FALSE(ble.isConnected());

    server.end();
}

void test_non_active_client_writes_are_ignored(void) {
    BLEMCPServer server("test-ble", "1.0.0");
    server.begin();

    auto& ble = McpBle::getInstance();
    NimBLEConnInfo active(7);
    NimBLEConnInfo other(9);
    ble._onConnect(NimBLEDevice::createServer(), active);

    int callbackCount = 0;
    ble.setRxCallback([&callbackCount](const uint8_t* data, size_t len) {
        (void)data;
        (void)len;
        callbackCount++;
    });

    NimBLECharacteristic characteristic;
    characteristic.setValue(std::vector<uint8_t>{'{', '}'});

    ble._onWrite(&characteristic, other);
    TEST_ASSERT_EQUAL_INT(0, callbackCount);

    ble._onWrite(&characteristic, active);
    TEST_ASSERT_EQUAL_INT(1, callbackCount);

    ble._onDisconnect(NimBLEDevice::createServer(), active, 0);
    server.end();
}

void test_notifications_target_active_connection(void) {
    BLEMCPServer server("test-ble", "1.0.0");
    server.begin();

    auto& ble = McpBle::getInstance();
    NimBLEConnInfo active(7);
    ble._onConnect(NimBLEDevice::createServer(), active);

    const uint8_t payload[] = {'o', 'k'};
    mock_ble::resetNotifyTracking();
    bool ok = ble.sendNotification(payload, sizeof(payload));

    ble._onDisconnect(NimBLEDevice::createServer(), active, 0);
    server.end();

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(7, mock_ble::lastNotifyConnHandle().load());
}

void test_disconnect_discards_partial_message(void) {
    BLEMCPServer server("test-ble", "1.0.0");

    Tool tool;
    tool.name = "ping";
    tool.description = "Returns ok";
    tool.inputSchema.type = "object";
    tool.handler = std::make_shared<PingHandler>();
    server.RegisterTool(tool);

    server.begin();
    auto& ble = McpBle::getInstance();
    ble._onConnect(nullptr);

    /* A valid request split across two fragments; only the START arrives on
     * the first connection, leaving reassembly in progress. */
    const std::string req =
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"ping","arguments":{}}})";
    const size_t total = req.size();
    const size_t firstChunk = 40;

    std::vector<uint8_t> start(5 + firstChunk);
    start[0] = 0x40;  // TYPE_START, seq 0
    start[1] = (uint8_t)((total >> 24) & 0xFF);
    start[2] = (uint8_t)((total >> 16) & 0xFF);
    start[3] = (uint8_t)((total >> 8) & 0xFF);
    start[4] = (uint8_t)(total & 0xFF);
    memcpy(start.data() + 5, req.data(), firstChunk);
    mcp_transport_receive(start.data(), start.size());

    /* Connection drops and a new one comes up. */
    ble._onDisconnect(nullptr);
    mock_ble::resetNotifyTracking();
    ble._onConnect(nullptr);

    /* The leftover END from the previous connection must NOT complete a message;
     * it should fail fast with a transport BAD_SEQUENCE frame. */
    const size_t rest = total - firstChunk;
    std::vector<uint8_t> end(1 + rest);
    end[0] = 0xC0 | 1;  // TYPE_END, seq 1
    memcpy(end.data() + 1, req.data() + firstChunk, rest);
    mcp_transport_receive(end.data(), end.size());

    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // let the worker run
    int notifications = mock_ble::notifyCount().load();
    std::vector<uint8_t> lastNotify = mock_ble::lastNotifyPayload();

    ble._onDisconnect(nullptr);
    server.end();

    TEST_ASSERT_EQUAL_INT(1, notifications);
    TEST_ASSERT_GREATER_OR_EQUAL(3, static_cast<int>(lastNotify.size()));
    TEST_ASSERT_EQUAL_UINT8(EXPECT_CONTROL_HEADER, lastNotify[0]);
    TEST_ASSERT_EQUAL_UINT8(EXPECT_CONTROL_ERROR, lastNotify[1]);
    TEST_ASSERT_EQUAL_UINT8(EXPECT_ERR_BAD_SEQUENCE, lastNotify[2]);
}

void test_end_waits_until_worker_task_exits(void) {
    std::atomic<bool> handlerStarted(false);
    std::atomic<bool> releaseHandler(false);

    BLEMCPServer server("test-ble", "1.0.0");

    Tool tool;
    tool.name = "block";
    tool.description = "Blocks until released";
    tool.inputSchema.type = "object";
    tool.handler = std::make_shared<BlockingHandler>(handlerStarted, releaseHandler);
    server.RegisterTool(tool);

    server.begin();
    sendSingleMessage(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"block","arguments":{}}})");

    const auto waitStart = std::chrono::steady_clock::now();
    while (!handlerStarted.load()) {
        TEST_ASSERT_LESS_THAN(500, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - waitStart).count()));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::thread releaser([&releaseHandler] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        releaseHandler.store(true);
    });

    const auto shutdownStart = std::chrono::steady_clock::now();
    server.end();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - shutdownStart)
                               .count();

    releaser.join();
    TEST_ASSERT_GREATER_OR_EQUAL(180, static_cast<int>(elapsedMs));
}

void test_end_waits_past_five_second_timeout_until_handler_finishes(void) {
    std::atomic<bool> handlerStarted(false);
    std::atomic<bool> handlerFinished(false);
    std::atomic<bool> releaseHandler(false);

    BLEMCPServer server("test-ble", "1.0.0");

    Tool tool;
    tool.name = "slow_block";
    tool.description = "Blocks past the shutdown timeout";
    tool.inputSchema.type = "object";
    tool.handler = std::make_shared<SlowBlockingHandler>(handlerStarted, handlerFinished, releaseHandler);
    server.RegisterTool(tool);

    server.begin();
    sendSingleMessage(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"slow_block","arguments":{}}})");

    const auto waitStart = std::chrono::steady_clock::now();
    while (!handlerStarted.load()) {
        TEST_ASSERT_LESS_THAN(500, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - waitStart).count()));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::thread releaser([&releaseHandler] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5200));
        releaseHandler.store(true);
    });

    const auto shutdownStart = std::chrono::steady_clock::now();
    server.end();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - shutdownStart)
                               .count();
    const bool finishedAtReturn = handlerFinished.load();

    releaser.join();
    TEST_ASSERT_TRUE(finishedAtReturn);
    TEST_ASSERT_GREATER_OR_EQUAL(5100, static_cast<int>(elapsedMs));
}

void test_rx_queue_overflow_is_counted_not_silent(void) {
    std::atomic<bool> started(false);
    std::atomic<bool> release(false);

    BLEMCPServer server("test-ble", "1.0.0");

    Tool tool;
    tool.name = "block";
    tool.description = "Occupies the worker task";
    tool.inputSchema.type = "object";
    tool.handler = std::make_shared<BlockingHandler>(started, release);
    server.RegisterTool(tool);

    server.begin();
    McpBle::getInstance()._onConnect(nullptr);

    /* First request is picked up by the worker, which then blocks — so nothing
     * drains the queue while we overrun it. */
    sendSingleMessage(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"block","arguments":{}}})");
    const auto waitStart = std::chrono::steady_clock::now();
    while (!started.load()) {
        TEST_ASSERT_LESS_THAN(500, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - waitStart).count()));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    /* Fill the queue to capacity and then push one more than it can hold. */
    mock_ble::resetNotifyTracking();
    for (int i = 0; i < MCP_BLE_RX_QUEUE_DEPTH + 4; i++) {
        sendSingleMessage("{}");
    }

    uint32_t dropped = server.droppedMessageCount();
    std::vector<uint8_t> lastNotify = mock_ble::lastNotifyPayload();

    release.store(true);
    McpBle::getInstance()._onDisconnect(nullptr);
    server.end();

    /* The overflow must be observable, not dropped silently. */
    TEST_ASSERT_GREATER_OR_EQUAL(1, static_cast<int>(dropped));
    TEST_ASSERT_GREATER_OR_EQUAL(1, mock_ble::notifyCount().load());
    TEST_ASSERT_GREATER_OR_EQUAL(3, static_cast<int>(lastNotify.size()));
    TEST_ASSERT_EQUAL_UINT8(EXPECT_CONTROL_HEADER, lastNotify[0]);
    TEST_ASSERT_EQUAL_UINT8(EXPECT_CONTROL_ERROR, lastNotify[1]);
    TEST_ASSERT_EQUAL_UINT8(EXPECT_ERR_BUSY, lastNotify[2]);
}

void test_rx_queue_enforces_aggregate_byte_budget(void) {
    std::atomic<bool> started(false);
    std::atomic<bool> release(false);

    BLEMCPServer server("test-ble", "1.0.0");
    Tool tool;
    tool.name = "block";
    tool.inputSchema.type = "object";
    tool.handler = std::make_shared<BlockingHandler>(started, release);
    server.RegisterTool(tool);
    server.begin();
    McpBle::getInstance()._onConnect(nullptr);

    sendSingleMessage(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"block","arguments":{}}})");
    const auto waitStart = std::chrono::steady_clock::now();
    while (!started.load()) {
        TEST_ASSERT_LESS_THAN(500, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - waitStart).count()));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::string large =
        R"({"jsonrpc":"2.0","id":2,"method":"ping","params":{"padding":")";
    large.append(7000, 'x');
    large += R"("}})";

    mock_ble::resetNotifyTracking();
    sendSingleMessage(large.c_str());
    sendSingleMessage(large.c_str());
    sendSingleMessage(large.c_str());

    TEST_ASSERT_LESS_OR_EQUAL(MCP_BLE_RX_QUEUE_MAX_BYTES,
                              static_cast<int>(server.queuedMessageBytes()));
    TEST_ASSERT_GREATER_OR_EQUAL(1, static_cast<int>(server.droppedMessageCount()));
    std::vector<uint8_t> lastNotify = mock_ble::lastNotifyPayload();
    TEST_ASSERT_GREATER_OR_EQUAL(3, static_cast<int>(lastNotify.size()));
    TEST_ASSERT_EQUAL_UINT8(EXPECT_ERR_BUSY, lastNotify[2]);

    release.store(true);
    McpBle::getInstance()._onDisconnect(nullptr);
    server.end();
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(server.queuedMessageBytes()));
}

void test_loop_does_not_consume_queue(void) {
    std::atomic<bool> started(false);
    std::atomic<bool> release(false);

    BLEMCPServer server("test-ble", "1.0.0");

    Tool blockTool;
    blockTool.name = "block";
    blockTool.description = "Occupies the worker task";
    blockTool.inputSchema.type = "object";
    blockTool.handler = std::make_shared<BlockingHandler>(started, release);
    server.RegisterTool(blockTool);

    Tool pingTool;
    pingTool.name = "ping";
    pingTool.description = "Returns ok";
    pingTool.inputSchema.type = "object";
    pingTool.handler = std::make_shared<PingHandler>();
    server.RegisterTool(pingTool);

    server.begin();
    McpBle::getInstance()._onConnect(nullptr);

    /* Park the worker inside the blocking handler so it cannot drain the queue. */
    sendSingleMessage(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"block","arguments":{}}})");
    const auto waitStart = std::chrono::steady_clock::now();
    while (!started.load()) {
        TEST_ASSERT_LESS_THAN(500, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - waitStart).count()));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    /* Queue a request that, if processed, would produce a response (notify). */
    mock_ble::resetNotifyTracking();
    sendSingleMessage(
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"ping","arguments":{}}})");

    /* loop() must NOT process the queued message — that belongs to the worker
     * task; draining it here would race the worker on shared transport state. */
    server.loop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int responses = mock_ble::notifyCount().load();

    release.store(true);
    McpBle::getInstance()._onDisconnect(nullptr);
    server.end();

    TEST_ASSERT_EQUAL_INT(0, responses);
}

void test_failed_notify_is_reported(void) {
    BLEMCPServer server("test-ble", "1.0.0");
    server.begin();
    auto& ble = McpBle::getInstance();
    ble._onConnect(nullptr);

    const uint8_t payload[] = {'h', 'i'};

    mock_ble::resetNotifyTracking();
    mock_ble::failNextNotify(1);
    bool ok = ble.sendNotification(payload, sizeof(payload));

    mock_ble::resetNotifyTracking();
    ble._onDisconnect(nullptr);
    server.end();

    /* A failed BLE notification must be reported, not masked as success. */
    TEST_ASSERT_FALSE(ok);
}

void test_transport_retries_failed_notify(void) {
    BLEMCPServer server("test-ble", "1.0.0");
    server.begin();
    auto& ble = McpBle::getInstance();
    ble._onConnect(nullptr);

    /* "hi" fits in a single packet at the default MTU → exactly one logical send.
     * Fail the first two notify attempts; the transport's retry (3 attempts,
     * configured in begin()) must resend until the third succeeds. */
    mock_ble::resetNotifyTracking();
    mock_ble::failNextNotify(2);
    mcp_transport_send_message("hi");
    int calls = mock_ble::notifyCount().load();

    mock_ble::resetNotifyTracking();
    ble._onDisconnect(nullptr);
    server.end();

    TEST_ASSERT_EQUAL_INT(3, calls);
}

void test_disconnect_resets_transport_mtu(void) {
    BLEMCPServer server("test-ble", "1.0.0");
    server.begin();

    auto& ble = McpBle::getInstance();
    ble._onConnect(nullptr);
    ble._onMtuChange(247);

    /* Disconnect, then reconnect WITHOUT an MTU exchange: the transport must be
     * back at the ATT default (23 → 20-byte payloads), not still framing for
     * the previous connection's 247. */
    ble._onDisconnect(nullptr);
    ble._onConnect(nullptr);

    mock_ble::resetNotifyTracking();
    std::string msg(100, 'B');
    mcp_transport_send_message(msg.c_str());

    int packets = mock_ble::notifyCount().load();
    size_t lastPacketSize = mock_ble::lastNotifyPayload().size();

    ble._onDisconnect(nullptr);
    server.end();

    TEST_ASSERT_GREATER_THAN(1, packets);
    TEST_ASSERT_LESS_OR_EQUAL(20, static_cast<int>(lastPacketSize));
}

void test_end_during_inflight_writes_is_safe(void) {
    BLEMCPServer server("test-ble", "1.0.0");
    server.begin();

    auto& ble = McpBle::getInstance();
    auto* nimServer = NimBLEDevice::createServer();
    NimBLEConnInfo conn(7);
    ble._onConnect(nimServer, conn);

    std::atomic<bool> stopWriting(false);
    std::atomic<int> writesDone(0);
    NimBLECharacteristic characteristic;
    /* Header 0x00 (SINGLE) + "{}": parses, has no method → the worker sends a
     * parse-error response, so teardown races both the RX and TX paths. */
    characteristic.setValue(std::vector<uint8_t>{0x00, '{', '}'});

    std::thread writer([&] {
        while (!stopWriting.load()) {
            ble._onWrite(&characteristic, conn);
            writesDone.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    const auto waitStart = std::chrono::steady_clock::now();
    while (writesDone.load() < 5) {
        TEST_ASSERT_LESS_THAN(1000, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - waitStart).count()));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    /* Tear down while the host-task thread is still hammering writes. The
     * callback setters must block until any in-flight write returns, so no
     * write can touch freed transport state. */
    server.end();

    stopWriting.store(true);
    writer.join();

    /* After end() the write path must be inert: no callback, no response. */
    mock_ble::resetNotifyTracking();
    ble._onWrite(&characteristic, conn);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    TEST_ASSERT_EQUAL_INT(0, mock_ble::notifyCount().load());

    /* Simulate the disconnect event that stop()'s disconnect() eventually
     * produces, so the singleton is clean for the next test. */
    ble._onDisconnect(nimServer, conn, 0);
    TEST_ASSERT_FALSE(ble.isConnected());
}

void test_end_stops_advertising_and_disconnects_client(void) {
    BLEMCPServer server("test-ble", "1.0.0");
    server.begin();

    auto& ble = McpBle::getInstance();
    auto* nimServer = NimBLEDevice::createServer();
    NimBLEConnInfo conn(7);
    ble._onConnect(nimServer, conn);

    mock_ble::resetConnectionTracking();
    server.end();

    int stops = mock_ble::stopAdvertisingCount().load();
    int disconnects = mock_ble::disconnectCount().load();
    int disconnectedHandle = mock_ble::lastDisconnectConnHandle().load();

    /* The disconnect completes asynchronously on the host task; when the event
     * lands after end(), advertising must NOT be restarted. */
    mock_ble::resetConnectionTracking();
    ble._onDisconnect(nimServer, conn, 0);
    int restartsAfterEnd = mock_ble::startAdvertisingCount().load();

    TEST_ASSERT_GREATER_OR_EQUAL(1, stops);
    TEST_ASSERT_GREATER_OR_EQUAL(1, disconnects);
    TEST_ASSERT_EQUAL_INT(7, disconnectedHandle);
    TEST_ASSERT_EQUAL_INT(0, restartsAfterEnd);
    TEST_ASSERT_FALSE(ble.isConnected());
}

void test_begin_after_end_restarts_advertising(void) {
    BLEMCPServer server("test-ble", "1.0.0");
    server.begin();

    auto& ble = McpBle::getInstance();
    auto* nimServer = NimBLEDevice::createServer();
    NimBLEConnInfo conn(7);
    ble._onConnect(nimServer, conn);

    server.end();

    /* Deferred disconnect after end(): must NOT re-advertise. */
    mock_ble::resetConnectionTracking();
    ble._onDisconnect(nimServer, conn, 0);
    TEST_ASSERT_EQUAL_INT(0, mock_ble::startAdvertisingCount().load());

    /* A central whose connection completes while stopped must be dropped. */
    NimBLEConnInfo late(9);
    bool accepted = ble._onConnect(nimServer, late);
    TEST_ASSERT_FALSE(accepted);
    TEST_ASSERT_GREATER_OR_EQUAL(1, mock_ble::disconnectCount().load());
    TEST_ASSERT_EQUAL_INT(9, mock_ble::lastDisconnectConnHandle().load());

    /* begin() again must resume advertising (deleting the _stopped reset would
     * leave the device permanently undiscoverable)... */
    mock_ble::resetConnectionTracking();
    server.begin();
    TEST_ASSERT_GREATER_OR_EQUAL(1, mock_ble::startAdvertisingCount().load());

    /* ...and the normal disconnect → re-advertise cycle must work again. */
    NimBLEConnInfo conn2(11);
    TEST_ASSERT_TRUE(ble._onConnect(nimServer, conn2));
    mock_ble::resetConnectionTracking();
    ble._onDisconnect(nimServer, conn2, 0);
    TEST_ASSERT_GREATER_OR_EQUAL(1, mock_ble::startAdvertisingCount().load());

    server.end();
    ble._onDisconnect(nimServer, conn2, 0);
}

void test_device_name_is_advertised(void) {
    /* NimBLE 2.x no longer advertises the device name automatically from
     * NimBLEDevice::init(); it must be set on the advertisement explicitly. */
    BLEMCPServer server("test-ble", "1.0.0");
    server.begin();
    std::string advName = mock_ble::advertisedName();
    server.end();

    TEST_ASSERT_FALSE(advName.empty());
    TEST_ASSERT_EQUAL_STRING("MCP_Server_BLE", advName.c_str());
}

void test_preferred_mtu_is_offered_during_init(void) {
    /* Fragment count is driven by the negotiated MTU, and NimBLE only offers
     * more than the 23-byte default if asked. McpBle is a process-wide
     * singleton whose init() runs once, so this asserts the value that first
     * begin() published rather than resetting and re-initialising. */
    BLEMCPServer server("test-ble", "1.0.0");
    server.begin();
    server.end();

    TEST_ASSERT_EQUAL_INT(517, mock_ble::lastPreferredMtu().load());
}

void test_default_tx_gap_is_zero(void) {
    /* The per-fragment delay used to be a hardcoded 1 tick, which on a 1 kHz
     * tick cost ~430 ms for an 8 KiB message at the default MTU. Backpressure
     * is handled by the send-retry path instead. */
    TEST_ASSERT_EQUAL_UINT32(0, McpBle::getInstance().getConfig().txGapTicks);
}

void test_tx_gap_is_configurable(void) {
    auto& ble = McpBle::getInstance();
    BleServerConfig restore = ble.getConfig();

    BleServerConfig cfg = restore;
    cfg.txGapTicks = 3;
    ble.setConfig(cfg);
    TEST_ASSERT_EQUAL_UINT32(3, ble.getConfig().txGapTicks);

    ble.setConfig(restore);
    TEST_ASSERT_EQUAL_UINT32(0, ble.getConfig().txGapTicks);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    UNITY_BEGIN();
    RUN_TEST(test_preferred_mtu_is_offered_during_init);
    RUN_TEST(test_default_tx_gap_is_zero);
    RUN_TEST(test_tx_gap_is_configurable);
    RUN_TEST(test_end_waits_until_worker_task_exits);
    RUN_TEST(test_end_waits_past_five_second_timeout_until_handler_finishes);
    RUN_TEST(test_concurrent_sends_are_serialized);
    RUN_TEST(test_second_connection_is_rejected_without_dropping_active_session);
    RUN_TEST(test_non_active_client_writes_are_ignored);
    RUN_TEST(test_notifications_target_active_connection);
    RUN_TEST(test_disconnect_discards_partial_message);
    RUN_TEST(test_rx_queue_overflow_is_counted_not_silent);
    RUN_TEST(test_rx_queue_enforces_aggregate_byte_budget);
    RUN_TEST(test_loop_does_not_consume_queue);
    RUN_TEST(test_failed_notify_is_reported);
    RUN_TEST(test_transport_retries_failed_notify);
    RUN_TEST(test_disconnect_resets_transport_mtu);
    RUN_TEST(test_end_during_inflight_writes_is_safe);
    RUN_TEST(test_end_stops_advertising_and_disconnects_client);
    RUN_TEST(test_begin_after_end_restarts_advertising);
    RUN_TEST(test_device_name_is_advertised);
    return UNITY_END();
}
