#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include "mcp_transport.h"

#define TAG "MCP_TRANS"

#ifndef MCP_TRANSPORT_MAX_MESSAGE_SIZE
#define MCP_TRANSPORT_MAX_MESSAGE_SIZE 8192
#endif

#define MAX_MESSAGE_SIZE MCP_TRANSPORT_MAX_MESSAGE_SIZE
#define DEFAULT_MTU 23
#define MAX_MTU 517
#define MAX_GATT_VALUE_LEN 512

/* RX reassembly buffer is allocated on demand and grown to fit each message,
 * then shrunk back to this baseline so a single large message does not keep
 * MAX_MESSAGE_SIZE bytes resident on the heap. */
#ifndef MCP_TRANSPORT_RX_BASELINE_CAP
#define MCP_TRANSPORT_RX_BASELINE_CAP 256
#endif
#define RX_BASELINE_CAP MCP_TRANSPORT_RX_BASELINE_CAP

/* Protocol Definitions */
#define HEADER_TYPE_MASK 0xC0
#define HEADER_SEQ_MASK  0x3F

#define TYPE_SINGLE 0x00
#define TYPE_START  0x40
#define TYPE_CONT   0x80
#define TYPE_END    0xC0

static uint8_t *rx_buffer = NULL;
static size_t rx_buffer_cap = 0;
static uint8_t *tx_buffer = NULL;
static size_t rx_received_len = 0;
static size_t rx_total_len = 0;
static uint8_t rx_expect_seq_id = 0;
static bool rx_in_progress = false;

static mcp_transport_send_fn_t s_send_fn = NULL;
static void *s_send_ctx = NULL;
static mcp_transport_message_cb_t s_message_cb = NULL;
static void *s_message_ctx = NULL;
static mcp_transport_sleep_fn_t s_sleep_fn = NULL;
static void *s_sleep_ctx = NULL;
static mcp_transport_log_fn_t s_log_fn = NULL;
static void *s_log_ctx = NULL;
static mcp_transport_lock_fn_t s_lock_fn = NULL;
static void *s_lock_ctx = NULL;
static uint16_t s_mtu = DEFAULT_MTU;
static uint32_t s_tx_gap_ticks = 0;
static uint8_t s_send_max_retries = 3;
static uint32_t s_send_retry_delay_ticks = 1;
static bool s_initialized = false;

#ifdef MCP_TRANSPORT_TEST_HOOKS
static int s_fail_next_alloc = 0;
void mcp_transport_test_fail_next_alloc(int n) {
    s_fail_next_alloc = n;
}
#endif

/* realloc wrapper for the RX buffer; honors the test-only failure injection. */
static void *rx_realloc(void *ptr, size_t size) {
#ifdef MCP_TRANSPORT_TEST_HOOKS
    if (s_fail_next_alloc > 0) {
        s_fail_next_alloc--;
        return NULL;
    }
#endif
    return realloc(ptr, size);
}

/* Ensure the RX buffer can hold at least `need` bytes. Grows only; never shrinks.
 * On allocation failure the existing buffer is left intact and false is returned. */
static bool rx_ensure_capacity(size_t need) {
    if (rx_buffer && rx_buffer_cap >= need) {
        return true;
    }
    uint8_t *grown = (uint8_t *)rx_realloc(rx_buffer, need);
    if (!grown) {
        return false;
    }
    rx_buffer = grown;
    rx_buffer_cap = need;
    return true;
}

/* Release any growth beyond the baseline once a message has been delivered, so a
 * single large message does not pin a large allocation for the connection's life. */
static void rx_shrink_to_baseline(void) {
    if (rx_buffer_cap <= RX_BASELINE_CAP) {
        return;
    }
    uint8_t *shrunk = (uint8_t *)realloc(rx_buffer, RX_BASELINE_CAP);
    if (shrunk) {
        rx_buffer = shrunk;
        rx_buffer_cap = RX_BASELINE_CAP;
    }
    /* If shrinking fails we simply keep the larger buffer — harmless. */
}

static void rx_reset_state(void) {
    rx_received_len = 0;
    rx_total_len = 0;
    rx_in_progress = false;
}

static void mcp_transport_logf(int level, const char *fmt, ...) {
    if (!s_log_fn) {
        return;
    }
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s_log_fn(level, TAG, buf, s_log_ctx);
}

static size_t mcp_transport_max_packet_len(void) {
    uint16_t mtu = s_mtu;
    if (mtu < 4) {
        return 20;
    }
    size_t max_len = (size_t)mtu - 3;
    if (max_len > MAX_GATT_VALUE_LEN) {
        max_len = MAX_GATT_VALUE_LEN;
    }
    if (max_len < 2) {
        max_len = 2;
    }
    return max_len;
}

static bool mcp_transport_send_packet(const uint8_t *data, size_t len) {
    if (!s_send_fn) {
        return false;
    }

    for (uint8_t attempt = 0; attempt <= s_send_max_retries; attempt++) {
        int rc = s_send_fn(data, len, s_send_ctx);
        if (rc == 0) {
            return true;
        }
        if (attempt < s_send_max_retries && s_send_retry_delay_ticks > 0 && s_sleep_fn) {
            s_sleep_fn(s_send_retry_delay_ticks, s_sleep_ctx);
        }
    }
    return false;
}

void mcp_transport_init(void) {
    if (s_initialized) {
        return;
    }

    rx_buffer = (uint8_t *)malloc(RX_BASELINE_CAP);
    if (!rx_buffer) {
        mcp_transport_logf(MCP_TRANSPORT_LOG_ERROR, "Failed to allocate RX buffer");
        return;
    }
    rx_buffer_cap = RX_BASELINE_CAP;
    tx_buffer = (uint8_t *)malloc(MAX_MTU);
    if (!tx_buffer) {
        free(rx_buffer);
        rx_buffer = NULL;
        mcp_transport_logf(MCP_TRANSPORT_LOG_ERROR, "Failed to allocate TX buffer");
        return;
    }
    mcp_transport_logf(MCP_TRANSPORT_LOG_INFO, "Initialized");
    s_initialized = true;
}

void mcp_transport_deinit(void) {
    if (tx_buffer) {
        free(tx_buffer);
        tx_buffer = NULL;
    }
    if (rx_buffer) {
        free(rx_buffer);
        rx_buffer = NULL;
    }
    rx_buffer_cap = 0;

    rx_reset_state();
    rx_expect_seq_id = 0;

    s_initialized = false;
}

void mcp_transport_set_send_fn(mcp_transport_send_fn_t fn, void *ctx) {
    s_send_fn = fn;
    s_send_ctx = ctx;
}

void mcp_transport_set_message_cb(mcp_transport_message_cb_t cb, void *ctx) {
    s_message_cb = cb;
    s_message_ctx = ctx;
}

void mcp_transport_set_sleep_fn(mcp_transport_sleep_fn_t fn, void *ctx) {
    s_sleep_fn = fn;
    s_sleep_ctx = ctx;
}

void mcp_transport_set_log_fn(mcp_transport_log_fn_t fn, void *ctx) {
    s_log_fn = fn;
    s_log_ctx = ctx;
}

void mcp_transport_set_lock_fn(mcp_transport_lock_fn_t fn, void *ctx) {
    s_lock_fn = fn;
    s_lock_ctx = ctx;
}

void mcp_transport_set_mtu(uint16_t mtu) {
    s_mtu = mtu ? mtu : DEFAULT_MTU;
}

void mcp_transport_set_tx_gap_ticks(uint32_t gap_ticks) {
    s_tx_gap_ticks = gap_ticks;
}

void mcp_transport_set_send_retry(uint8_t max_retries, uint32_t retry_delay_ticks) {
    s_send_max_retries = max_retries;
    s_send_retry_delay_ticks = retry_delay_ticks;
}

void mcp_transport_reset_rx(void) {
    rx_reset_state();
    rx_shrink_to_baseline();
}

void mcp_transport_receive(const uint8_t *data, size_t len) {
    if (!rx_buffer) return;
    if (!data) return;
    if (len < 1) return;

    uint8_t header = data[0];
    uint8_t type = header & HEADER_TYPE_MASK;
    uint8_t seq_id = header & HEADER_SEQ_MASK;
    const uint8_t *payload = data + 1;
    size_t payload_len = len - 1;

    if (type == TYPE_SINGLE) {
        if (payload_len > MAX_MESSAGE_SIZE) {
            mcp_transport_logf(MCP_TRANSPORT_LOG_ERROR, "Message too large");
            return;
        }
        if (!rx_ensure_capacity(payload_len + 1)) {
            mcp_transport_logf(MCP_TRANSPORT_LOG_ERROR, "RX buffer alloc failed: %d", (int)payload_len);
            return;
        }
        memcpy(rx_buffer, payload, payload_len);
        rx_buffer[payload_len] = 0;
        mcp_transport_logf(MCP_TRANSPORT_LOG_INFO, "Received Single: %d bytes", (int)payload_len);
        if (s_message_cb) {
            s_message_cb((char *)rx_buffer, s_message_ctx);
        } else {
            mcp_transport_logf(MCP_TRANSPORT_LOG_ERROR, "Message callback not set");
        }
        rx_reset_state();
        rx_shrink_to_baseline();

    } else if (type == TYPE_START) {
        if (payload_len < 4) return;

        rx_total_len = (payload[0] << 24) | (payload[1] << 16) | (payload[2] << 8) | payload[3];

        if (rx_total_len > MAX_MESSAGE_SIZE) {
            mcp_transport_logf(MCP_TRANSPORT_LOG_ERROR, "Message too large: %d", (int)rx_total_len);
            rx_total_len = 0;
            return;
        }

        if (!rx_ensure_capacity(rx_total_len + 1)) {
            mcp_transport_logf(MCP_TRANSPORT_LOG_ERROR, "RX buffer alloc failed: %d", (int)rx_total_len);
            rx_reset_state();
            return;
        }

        rx_received_len = 0;
        rx_in_progress = true;
        rx_expect_seq_id = (uint8_t)((seq_id + 1) & HEADER_SEQ_MASK);
        payload += 4;
        payload_len -= 4;

        if (payload_len > rx_total_len) {
            mcp_transport_logf(MCP_TRANSPORT_LOG_ERROR, "Start payload too large");
            rx_total_len = 0;
            rx_in_progress = false;
            return;
        }
        memcpy(rx_buffer + rx_received_len, payload, payload_len);
        rx_received_len += payload_len;

    } else if (type == TYPE_CONT) {
        if (rx_total_len == 0) return;
        if (!rx_in_progress) return;
        if (seq_id != rx_expect_seq_id) {
            mcp_transport_logf(MCP_TRANSPORT_LOG_ERROR, "Sequence mismatch");
            rx_total_len = 0;
            rx_received_len = 0;
            rx_in_progress = false;
            return;
        }
        rx_expect_seq_id = (uint8_t)((rx_expect_seq_id + 1) & HEADER_SEQ_MASK);

        if (rx_received_len + payload_len > rx_total_len) {
            mcp_transport_logf(MCP_TRANSPORT_LOG_ERROR, "Overflow");
            rx_total_len = 0;
            rx_received_len = 0;
            rx_in_progress = false;
            return;
        }
        memcpy(rx_buffer + rx_received_len, payload, payload_len);
        rx_received_len += payload_len;

    } else if (type == TYPE_END) {
        if (rx_total_len == 0) return;
        if (!rx_in_progress) return;
        if (seq_id != rx_expect_seq_id) {
            mcp_transport_logf(MCP_TRANSPORT_LOG_ERROR, "Sequence mismatch");
            rx_total_len = 0;
            rx_received_len = 0;
            rx_in_progress = false;
            return;
        }

        if (rx_received_len + payload_len > rx_total_len) {
            mcp_transport_logf(MCP_TRANSPORT_LOG_ERROR, "Overflow");
            rx_total_len = 0;
            rx_received_len = 0;
            rx_in_progress = false;
            return;
        }
        memcpy(rx_buffer + rx_received_len, payload, payload_len);
        rx_received_len += payload_len;

        if (rx_received_len == rx_total_len) {
            rx_buffer[rx_received_len] = 0;
            mcp_transport_logf(MCP_TRANSPORT_LOG_INFO, "Received Complete: %d bytes", (int)rx_received_len);
            if (s_message_cb) {
                s_message_cb((char *)rx_buffer, s_message_ctx);
            } else {
                mcp_transport_logf(MCP_TRANSPORT_LOG_ERROR, "Message callback not set");
            }
        } else {
            mcp_transport_logf(MCP_TRANSPORT_LOG_ERROR, "Length mismatch: exp %d, got %d", (int)rx_total_len, (int)rx_received_len);
        }
        rx_reset_state();
        rx_shrink_to_baseline();
    }
}

void mcp_transport_send_message(const char *json_message) {
    if (!s_send_fn || !tx_buffer) {
        mcp_transport_logf(MCP_TRANSPORT_LOG_ERROR, "Transport not ready");
        return;
    }

    if (!json_message) {
        mcp_transport_logf(MCP_TRANSPORT_LOG_ERROR, "Message is null");
        return;
    }

    if (s_lock_fn) {
        s_lock_fn(true, s_lock_ctx);
    }

    size_t total_len = strlen(json_message);
    if (total_len > MAX_MESSAGE_SIZE) {
        mcp_transport_logf(MCP_TRANSPORT_LOG_ERROR, "Message too large");
        if (s_lock_fn) s_lock_fn(false, s_lock_ctx);
        return;
    }

    size_t offset = 0;
    uint8_t seq_id = 0;

    size_t packet_len_max = mcp_transport_max_packet_len();

    if (total_len + 1 <= packet_len_max) {
        tx_buffer[0] = TYPE_SINGLE | (seq_id & HEADER_SEQ_MASK);
        memcpy(tx_buffer + 1, json_message, total_len);
        if (!mcp_transport_send_packet(tx_buffer, total_len + 1)) {
            mcp_transport_logf(MCP_TRANSPORT_LOG_ERROR, "Send failed");
        }
    } else {
        if (packet_len_max <= 5) {
            mcp_transport_logf(MCP_TRANSPORT_LOG_ERROR, "MTU too small");
            if (s_lock_fn) s_lock_fn(false, s_lock_ctx);
            return;
        }

        size_t chunk_len = packet_len_max - 5;

        tx_buffer[0] = TYPE_START | (seq_id & HEADER_SEQ_MASK);
        tx_buffer[1] = (total_len >> 24) & 0xFF;
        tx_buffer[2] = (total_len >> 16) & 0xFF;
        tx_buffer[3] = (total_len >> 8) & 0xFF;
        tx_buffer[4] = total_len & 0xFF;

        memcpy(tx_buffer + 5, json_message + offset, chunk_len);
        if (!mcp_transport_send_packet(tx_buffer, chunk_len + 5)) {
            mcp_transport_logf(MCP_TRANSPORT_LOG_ERROR, "Send failed");
            if (s_lock_fn) s_lock_fn(false, s_lock_ctx);
            return;
        }

        offset += chunk_len;
        seq_id++;

        while (offset < total_len) {
            if (s_tx_gap_ticks > 0 && s_sleep_fn) {
                s_sleep_fn(s_tx_gap_ticks, s_sleep_ctx);
            }

            size_t remaining = total_len - offset;
            if (remaining > (packet_len_max - 1)) {
                chunk_len = packet_len_max - 1;
                tx_buffer[0] = TYPE_CONT | (seq_id & HEADER_SEQ_MASK);
                memcpy(tx_buffer + 1, json_message + offset, chunk_len);
                if (!mcp_transport_send_packet(tx_buffer, chunk_len + 1)) {
                    mcp_transport_logf(MCP_TRANSPORT_LOG_ERROR, "Send failed");
                    if (s_lock_fn) s_lock_fn(false, s_lock_ctx);
                    return;
                }
                offset += chunk_len;
                seq_id++;
            } else {
                chunk_len = remaining;
                tx_buffer[0] = TYPE_END | (seq_id & HEADER_SEQ_MASK);
                memcpy(tx_buffer + 1, json_message + offset, chunk_len);
                if (!mcp_transport_send_packet(tx_buffer, chunk_len + 1)) {
                    mcp_transport_logf(MCP_TRANSPORT_LOG_ERROR, "Send failed");
                    if (s_lock_fn) s_lock_fn(false, s_lock_ctx);
                    return;
                }
                offset += chunk_len;
            }
        }
    }

    if (s_lock_fn) {
        s_lock_fn(false, s_lock_ctx);
    }
}
