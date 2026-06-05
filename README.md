# ESP-MCP

Unified MCP (Model Context Protocol) Server library for ESP32. Supports both **HTTP JSON-RPC** and **BLE** transports through a single dependency.

## Features

- **Unified API** - Common data structures (`Tool`, `ToolHandler`, `Properties`, etc.) shared across transports
- **HTTP transport** - MCP over HTTP/JSON-RPC using ESPAsyncWebServer
- **BLE transport** - MCP over Bluetooth Low Energy with automatic message fragmentation
- **Dual transport** - Use both HTTP and BLE simultaneously in a single project
- **Conditional compilation** - Only the transports whose dependencies are present get compiled
- **Protocol negotiation** - Defaults to MCP `2025-11-25` while accepting supported legacy initialize versions

## Installation

Add to your `platformio.ini`:

```ini
lib_deps =
    solnera/ESP-MCP
```

Then add transport-specific dependencies based on your needs:

### HTTP only
```ini
lib_deps =
    solnera/ESP-MCP
    me-no-dev/ESPAsyncWebServer@^1.2.4
    me-no-dev/AsyncTCP@^1.1.1
```

### BLE only
```ini
lib_deps =
    solnera/ESP-MCP
    h2zero/NimBLE-Arduino@^2.0.0
```

### Both transports
```ini
lib_deps =
    solnera/ESP-MCP
    me-no-dev/ESPAsyncWebServer@^1.2.4
    me-no-dev/AsyncTCP@^1.1.1
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

- **`ToolHandler`** - Abstract base class. Override `JsonDocument call(JsonVariantConst params)` to implement tool logic.
- **`Tool`** - Tool definition with name, description, inputSchema, outputSchema, and handler.
- **`Properties`** - JSON Schema builder for defining tool input/output schemas.
- **`MCPRequest` / `MCPResponse`** - Internal protocol message types.
- **`ErrorCode`** - JSON-RPC 2.0 error codes.

### HttpMCPServer

- **`HttpMCPServer(port, name, version, instructions)`** - Constructor. Starts HTTP server immediately. This lightweight transport returns JSON responses over POST and responds `405` to GET because server-to-client SSE streams are not implemented.
- **`RegisterTool(tool)`** - Register an MCP tool.

### BLEMCPServer

- **`BLEMCPServer(name, version, instructions)`** - Constructor.
- **`RegisterTool(tool)`** - Register an MCP tool.
- **`begin()`** - Initialize BLE and start advertising.
- **`loop()`** - API-compatible no-op. A background FreeRTOS task handles BLE message processing.

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
- Only one fragmented message may be in progress per connection. Clients should
  subscribe to the TX notification characteristic before writing requests and
  send large responses through application-level pagination/chunk tools.
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
