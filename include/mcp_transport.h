#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Build-time configuration:
 *   -DMCP_TRANSPORT_MAX_MESSAGE_SIZE=<bytes>
 *     Upper bound on a single reassembled message. Acts as a ceiling / DoS guard
 *     only — the RX buffer is allocated on demand per message and is NOT kept
 *     resident at this size, so raising it does not cost idle RAM. Defaults to
 *     8192 bytes. The practical limit is the largest contiguous heap block
 *     available when a message arrives.
 *   -DMCP_TRANSPORT_RX_BASELINE_CAP=<bytes>
 *     Size the RX buffer is grown from and shrunk back to between messages.
 *     Messages within this size incur no per-message allocation. Defaults to 256.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*mcp_transport_send_fn_t)(const uint8_t *data, size_t len, void *ctx);
typedef void (*mcp_transport_message_cb_t)(const char *message, void *ctx);
typedef void (*mcp_transport_sleep_fn_t)(uint32_t ticks, void *ctx);
typedef void (*mcp_transport_log_fn_t)(int level, const char *tag, const char *message, void *ctx);
typedef void (*mcp_transport_lock_fn_t)(bool lock, void *ctx);

enum {
    MCP_TRANSPORT_LOG_ERROR = 1,
    MCP_TRANSPORT_LOG_WARN = 2,
    MCP_TRANSPORT_LOG_INFO = 3,
    MCP_TRANSPORT_LOG_DEBUG = 4,
};

void mcp_transport_init(void);
void mcp_transport_deinit(void);
void mcp_transport_set_send_fn(mcp_transport_send_fn_t fn, void *ctx);
void mcp_transport_set_message_cb(mcp_transport_message_cb_t cb, void *ctx);
void mcp_transport_set_sleep_fn(mcp_transport_sleep_fn_t fn, void *ctx);
void mcp_transport_set_log_fn(mcp_transport_log_fn_t fn, void *ctx);
void mcp_transport_set_lock_fn(mcp_transport_lock_fn_t fn, void *ctx);
void mcp_transport_set_mtu(uint16_t mtu);
void mcp_transport_set_tx_gap_ticks(uint32_t gap_ticks);
void mcp_transport_set_send_retry(uint8_t max_retries, uint32_t retry_delay_ticks);
void mcp_transport_receive(const uint8_t *data, size_t len);
void mcp_transport_send_message(const char *json_message);

/* Abort any in-progress RX reassembly and release growth back to baseline.
 * Call on link loss (e.g. BLE disconnect) so a partial message cannot be
 * stitched together with fragments from a later connection. */
void mcp_transport_reset_rx(void);

#ifdef MCP_TRANSPORT_TEST_HOOKS
/* Test-only: force the next `n` RX-buffer allocations to fail (simulate OOM).
 * Compiled out of production builds. */
void mcp_transport_test_fail_next_alloc(int n);
#endif

#ifdef __cplusplus
}
#endif
