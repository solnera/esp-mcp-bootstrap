#include <unity.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "freertos/queue.h"
#include "freertos/semphr.h"

static void waitUntilQueueHasWaiter(QueueHandle_t queue) {
    const auto waitStart = std::chrono::steady_clock::now();
    while (true) {
        {
            std::lock_guard<std::mutex> lock(queue->mutex);
            if (queue->waiters > 0) {
                return;
            }
        }
        TEST_ASSERT_LESS_THAN(500, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - waitStart).count()));
        std::this_thread::yield();
    }
}

static void waitUntilSemaphoreHasWaiter(SemaphoreHandle_t sem) {
    const auto waitStart = std::chrono::steady_clock::now();
    while (true) {
        {
            std::lock_guard<std::mutex> lock(sem->mutex);
            if (sem->waiters > 0) {
                return;
            }
        }
        TEST_ASSERT_LESS_THAN(500, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - waitStart).count()));
        std::this_thread::yield();
    }
}

void test_vQueueDelete_waits_for_blocked_receiver(void) {
    QueueHandle_t queue = xQueueCreate(1, sizeof(char*));
    TEST_ASSERT_NOT_NULL(queue);

    std::atomic<bool> started(false);
    std::atomic<bool> finished(false);
    BaseType_t receiveResult = pdTRUE;
    char* out = nullptr;

    std::thread waiter([&] {
        started.store(true);
        receiveResult = xQueueReceive(queue, &out, portMAX_DELAY);
        finished.store(true);
    });

    while (!started.load()) std::this_thread::yield();
    waitUntilQueueHasWaiter(queue);

    vQueueDelete(queue);
    waiter.join();

    TEST_ASSERT_TRUE(finished.load());
    TEST_ASSERT_EQUAL(pdFALSE, receiveResult);
    TEST_ASSERT_NULL(out);
}

void test_vSemaphoreDelete_waits_for_blocked_taker(void) {
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(sem);

    std::atomic<bool> started(false);
    std::atomic<bool> finished(false);
    BaseType_t takeResult = pdPASS;

    std::thread waiter([&] {
        started.store(true);
        takeResult = xSemaphoreTake(sem, portMAX_DELAY);
        finished.store(true);
    });

    while (!started.load()) std::this_thread::yield();
    waitUntilSemaphoreHasWaiter(sem);

    vSemaphoreDelete(sem);
    waiter.join();

    TEST_ASSERT_TRUE(finished.load());
    TEST_ASSERT_EQUAL(pdFALSE, takeResult);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    UNITY_BEGIN();
    RUN_TEST(test_vQueueDelete_waits_for_blocked_receiver);
    RUN_TEST(test_vSemaphoreDelete_waits_for_blocked_taker);
    return UNITY_END();
}
