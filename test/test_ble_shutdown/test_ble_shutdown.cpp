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

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    UNITY_BEGIN();
    RUN_TEST(test_end_waits_until_worker_task_exits);
    RUN_TEST(test_end_waits_past_five_second_timeout_until_handler_finishes);
    return UNITY_END();
}
