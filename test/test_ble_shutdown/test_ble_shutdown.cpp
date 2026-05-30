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

    /* The leftover END from the previous connection must NOT complete a message. */
    const size_t rest = total - firstChunk;
    std::vector<uint8_t> end(1 + rest);
    end[0] = 0xC0 | 1;  // TYPE_END, seq 1
    memcpy(end.data() + 1, req.data() + firstChunk, rest);
    mcp_transport_receive(end.data(), end.size());

    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // let the worker run
    int responses = mock_ble::notifyCount().load();

    ble._onDisconnect(nullptr);
    server.end();

    TEST_ASSERT_EQUAL_INT(0, responses);
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
    for (int i = 0; i < MCP_BLE_RX_QUEUE_DEPTH + 4; i++) {
        sendSingleMessage("{}");
    }

    uint32_t dropped = server.droppedMessageCount();

    release.store(true);
    server.end();

    /* The overflow must be observable, not dropped silently. */
    TEST_ASSERT_GREATER_OR_EQUAL(1, static_cast<int>(dropped));
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

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    UNITY_BEGIN();
    RUN_TEST(test_end_waits_until_worker_task_exits);
    RUN_TEST(test_end_waits_past_five_second_timeout_until_handler_finishes);
    RUN_TEST(test_concurrent_sends_are_serialized);
    RUN_TEST(test_disconnect_discards_partial_message);
    RUN_TEST(test_rx_queue_overflow_is_counted_not_silent);
    RUN_TEST(test_loop_does_not_consume_queue);
    RUN_TEST(test_failed_notify_is_reported);
    RUN_TEST(test_transport_retries_failed_notify);
    RUN_TEST(test_device_name_is_advertised);
    return UNITY_END();
}
