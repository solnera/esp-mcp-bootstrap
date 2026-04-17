#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Build-time configuration:
 *   -DMCP_TRANSPORT_MAX_MESSAGE_SIZE=<bytes>
 * Defaults to 8192 bytes when not supplied.
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

#ifdef __cplusplus
}
#endif
