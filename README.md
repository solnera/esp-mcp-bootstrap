# ESP-MCP

Unified MCP (Model Context Protocol) Server library for ESP32. Supports both **HTTP JSON-RPC** and **BLE** transports through a single dependency.

## Features

- **Unified API** - Common data structures (`Tool`, `ToolHandler`, `Properties`, etc.) shared across transports
- **HTTP transport** - MCP over HTTP/JSON-RPC using ESPAsyncWebServer; tool calls run on a worker task, so handlers may block
- **BLE transport** - MCP over Bluetooth Low Energy with automatic message fragmentation
- **Dual transport** - Use both HTTP and BLE simultaneously in a single project
- **Conditional compilation** - Only the transports whose dependencies are present get compiled
- **Protocol negotiation** - Defaults to MCP `2025-11-25` while accepting `2025-06-18`, `2025-03-26`, and `2024-11-05` initialize versions; `ping` is answered per spec

## Installation

Add to your `platformio.ini`:

```ini
lib_deps =
    solnera/ESP-MCP@^0.5.0
```

Then add transport-specific dependencies based on your needs:

### HTTP only
```ini
lib_deps =
    solnera/ESP-MCP@^0.5.0
    ESP32Async/ESPAsyncWebServer@^3.6.0
    ESP32Async/AsyncTCP@^3.3.2
```

### BLE only
```ini
lib_deps =
    solnera/ESP-MCP@^0.5.0
    h2zero/NimBLE-Arduino@^2.0.0
```

### Both transports
```ini
lib_deps =
    solnera/ESP-MCP@^0.5.0
    ESP32Async/ESPAsyncWebServer@^3.6.0
    ESP32Async/AsyncTCP@^3.3.2
    h2zero/NimBLE-Arduino@^2.0.0
```

## Quick Start

### HTTP MCP Server

```cpp
#include <HttpMCPServer.h>

class MyHandler : public ToolHandler {
public:
    JsonDocument call(JsonVariantConst params) override {
        JsonDocument result;
        result["message"] = "Hello from ESP32!";
        return result;
    }
};

HttpMCPServer* server;

void setup() {
    // ... WiFi setup ...

    server = new HttpMCPServer(3000, "my-server", "1.0.0");

    Tool myTool;
    myTool.name = "hello";
    myTool.description = "Say hello";
    myTool.inputSchema.type = "object";
    myTool.handler = std::make_shared<MyHandler>();
    server->RegisterTool(myTool);
    server->begin();
}
```

### BLE MCP Server

```cpp
#include <BLEMCPServer.h>

class MyHandler : public ToolHandler {
public:
    JsonDocument call(JsonVariantConst params) override {
        JsonDocument result;
        result["message"] = "Hello from ESP32!";
        return result;
    }
};

BLEMCPServer server("my-server", "1.0.0");

void setup() {
    Tool myTool;
    myTool.name = "hello";
    myTool.description = "Say hello";
    myTool.inputSchema.type = "object";
    myTool.handler = std::make_shared<MyHandler>();
    server.RegisterTool(myTool);

    server.begin();
}
```

### Dual Transport

```cpp
#include <BLEMCPServer.h>
#include <HttpMCPServer.h>

BLEMCPServer bleServer("my-ble-server", "1.0.0");
HttpMCPServer* httpServer = nullptr;

void setup() {
    Tool myTool;
    // ... define tool ...

    // Register with both servers
    bleServer.RegisterTool(myTool);
    bleServer.begin();

    // ... WiFi setup ...

    httpServer = new HttpMCPServer(3000, "my-http-server", "1.0.0");
    httpServer->RegisterTool(myTool);
    httpServer->begin();
}
```

## Architecture

```
MCPServerBase (common protocol handling)
├── HttpMCPServer (HTTP transport via ESPAsyncWebServer)
└── BLEMCPServer  (BLE transport via NimBLE + fragmentation layer)
```

All MCP protocol logic (initialize, tools/list, tools/call, etc.) is implemented once in `MCPServerBase`. Transport-specific classes only handle their respective communication layers.

## API Reference

### Common Types (MCPServer.h)

- **`ToolHandler`** - Abstract base class. Override `JsonDocument call(JsonVariantConst params)` to implement tool logic. To report a tool execution failure per MCP (`result.isError: true`), additionally override `JsonDocument call(JsonVariantConst params, bool& isError)`, set `isError = true`, and return a document describing the failure — it is serialized into the `content` text; `structuredContent` is omitted on error. Handlers that only implement the single-argument overload always report success, unchanged.
- **`Tool`** - Tool definition with name, description, inputSchema, outputSchema, and handler.
- **`Properties`** - JSON Schema builder for defining tool input/output schemas.
- **`MCPRequest` / `MCPResponse`** - Internal protocol message types.
- **`ErrorCode`** - JSON-RPC 2.0 error codes.
- **`RegisterTool(tool)`** also has an rvalue overload: `RegisterTool(std::move(tool))` skips the deep copy of the schema tree, which is worth doing when a tool declares many nested properties. Registration is what invalidates the cached `tools/list` body, so both overloads are safe to mix.
- **`-DMCP_OMIT_TEXT_WHEN_STRUCTURED=1`** - Stop mirroring a structured tool result into the `content` text block. MCP says a structured result *SHOULD* also be returned as serialized text, so this is opt-in; turning it on halves both the wire size and the peak RAM of an object-returning tool, at the cost of clients that predate `structuredContent`. Results that cannot be structured (scalars, arrays, error payloads) always keep their text block.

### Performance notes

- `tools/list` is answered from a body serialized once per tool-set change, not rebuilt per request — capabilities advertise `listChanged: false`, and clients ask for it on every session start.
- Request parsing keeps the parsed document and views `params` into it, and responses are serialized directly rather than assembled in a wrapper document. A tool payload is therefore no longer deep-copied several times on its way out, which is what dominated peak heap for large results.

### HttpMCPServer

- **`HttpMCPServer(port, name, version, instructions)`** - Constructor. Configures the server without listening yet.
- **`RegisterTool(tool)`** - Register an MCP tool. Register all tools before `begin()`; `RegisterTool` is not synchronized against in-flight requests.
- **`begin()`** - Starts the worker, HTTP listener, and mDNS advertisement. Call it after WiFi is connected and all tools are registered. This lightweight transport returns JSON responses over POST and responds `405` to GET because server-to-client SSE streams are not implemented.
- **Tool execution model** - `tools/call` runs on a dedicated worker task (stack `-DMCP_HTTP_WORKER_STACK_SIZE=<bytes>`, default `8192`), so handlers may block — sensor waits, `delay()`, outbound HTTP calls — without starving the `async_tcp` task or tripping the task watchdog. A call that finishes within `-DMCP_HTTP_FAST_PATH_WAIT_MS=<ms>` (default `20`) is answered inline as an ordinary length-delimited response; anything slower falls back to a chunked response (`Transfer-Encoding: chunked`), which HTTP/1.1 libraries decode transparently. The fast path matters because the deferred body can only be written when the connection next polls — one lwIP coarse tick, roughly 500 ms — so without it even a 2 ms tool answered in half a second. The cost is that `async_tcp` is blocked for at most that window; set the flag to `0` to always defer. Either way HTTP/1.0 `tools/call` requests are rejected with `505 HTTP Version Not Supported`, since the fallback still needs chunked framing. `initialize`, `tools/list`, `ping`, notifications, and every transport-level rejection are still answered inline — those are pure in-memory JSON work. If the worker task cannot be created (out of memory during `begin()`), `tools/call` degrades to inline execution and a log line reports it; handlers must then return quickly.
- Pending tool calls are bounded (`-DMCP_HTTP_JOB_QUEUE_DEPTH=<n>`, default `4`); when the queue is full the server immediately answers JSON-RPC error `-32000` ("Server busy") so clients can back off and retry. Tool calls execute one at a time, in arrival order. A per-call job allocation that fails under memory pressure is answered with JSON-RPC `-32603` (HTTP `500`) instead of aborting, in both exception modes; allocations made while a handler runs (JSON documents, the serialized result) keep the library-wide fail-fast policy.
- A client disconnect does not cancel a running tool: execution always completes — tools have side effects (a WiFi-reconfig tool must finish even though reconfiguring drops the link) — and the result is discarded if nobody is left to read it. Slow tools are bounded by the client's HTTP timeout, not by the server.
- POST bodies are capped at `8192` bytes (`-DMCP_HTTP_MAX_BODY_SIZE=<bytes>`); larger requests are rejected with HTTP `413` before any buffering, so a hostile Content-Length cannot exhaust the heap. Non-JSON content types get `415`. A request without a usable Content-Length is answered `411`: the body buffer is sized up front from the declared length, so a chunked request body has nothing to size against. ESPAsyncWebServer gained chunked *request* body parsing in 3.11, but this library does not rely on it — MCP clients send Content-Length, and the fixed-size buffer is what keeps `MCP_HTTP_MAX_BODY_SIZE` enforceable before any allocation.
- JSON-RPC notifications receive `202 Accepted` with no body, per the MCP Streamable HTTP transport.

### BLEMCPServer

- **`BLEMCPServer(name, version, instructions)`** - Constructor.
- **`RegisterTool(tool)`** - Register an MCP tool.
- **`begin()`** - Initialize BLE and start advertising.
- **`end()`** - Full teardown: joins the worker task, stops advertising, disconnects the active central, and quiesces the NimBLE host task before releasing transport buffers. Advertising stays off until the next `begin()`. Do **not** call `end()` from inside a `ToolHandler` — it joins the worker task that runs handlers, which would self-deadlock; such calls are detected and ignored with a log line.
- **`loop()`** - API-compatible no-op. A background FreeRTOS task handles BLE message processing.
- **Worker stack** - the RX worker that runs handlers is sized by
  `-DMCP_BLE_WORKER_STACK_SIZE=<bytes>` (default `8192`). This is permanently
  resident RAM, and a dual-transport build pays for it alongside
  `MCP_HTTP_WORKER_STACK_SIZE`.

### BLE throughput

Two `BleServerConfig` fields dominate how long a response takes to get out.
Both must be set before `begin()`.

- **`preferredMtu`** (default `517`) is offered during negotiation. Fragment
  count follows directly from the negotiated MTU: at the 23-byte ATT default a
  packet carries 19 payload bytes, at 517 it carries 511 — about 27x fewer
  packets for the same message. A central that cannot go this high simply
  negotiates down, so the maximum is a safe default.
- **`txGapTicks`** (default `0`) is the delay inserted between outbound
  fragments. This was previously a hardcoded 1 tick, which on a 1 kHz tick meant
  an 8 KiB message at the default MTU spent roughly 430 ms asleep. Backpressure
  is already handled by the send-retry path, so the gap now defaults to off;
  raise it if a particular central drops notifications under back-to-back
  writes.

### BLE transport limits

The BLE transport is designed as a small-message control/configuration channel,
not a bulk-data channel.

- BLE MCP is single-client. The first connected central becomes the active MCP
  session; additional centrals are disconnected so fragmented messages, MTU
  state, and responses cannot be mixed across clients.
- A single reassembled MCP message defaults to `8192` bytes
  (`-DMCP_TRANSPORT_MAX_MESSAGE_SIZE=<bytes>`). Keep normal BLE responses around
  `1-2 KiB` when possible; larger values depend on available contiguous heap,
  negotiated MTU, and client-side timeout behavior.
- The RX reassembly buffer starts at `-DMCP_TRANSPORT_RX_BASELINE_CAP=<bytes>`
  (default `512`, sized so an ordinary `tools/call` request needs no allocation)
  and is only handed back to the heap once it exceeds
  `-DMCP_TRANSPORT_RX_SHRINK_FACTOR=<n>` times that baseline (default `4`).
  Shrinking after every message made traffic hovering near the baseline
  reallocate twice per message, which is what fragments a long-lived heap.
- Only one fragmented message may be in progress per connection. Clients should
  subscribe to the TX notification characteristic before writing requests and
  send large responses through application-level pagination/chunk tools.
- Complete messages waiting for the worker share a `16384` byte heap budget
  (`-DMCP_BLE_RX_QUEUE_MAX_BYTES=<bytes>`), independent of the queue's item
  depth. Requests beyond either limit receive the busy control error.
- Transport errors are reported as a reserved control notification instead of
  relying on client-side timeouts. Error frames use header `0x3F`, opcode `0x01`,
  then one error-code byte and optional ASCII detail text. Defined error codes:
  `1` message too large, `2` bad sequence, `3` overflow, `4` length mismatch,
  `5` out of memory, `6` busy, and `7` send failed/reserved. A physical notify
  failure is retried and logged locally because it cannot be reliably reported
  over the same failed link. Clients should abort the current in-flight write on
  received error frames and retry or back off according to the error code.
- For large data transfer, use BLE to negotiate/control the operation and move
  the payload through HTTP/WiFi or a chunked tool API.

## Examples

- **`examples/http_echo/`** - HTTP-only echo tool example
- **`examples/ble_config_wifi/`** - BLE-only WiFi configuration example
- **`examples/dual_transport/`** - Both transports running simultaneously

## License

MIT
