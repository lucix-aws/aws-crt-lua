# aws-crt-lua

LuaJIT FFI bindings to the [AWS Common Runtime (CRT)](https://docs.aws.amazon.com/sdkref/latest/guide/common-runtime.html) HTTP client.

This provides an optional high-performance HTTP transport for the AWS Lua SDK. It exposes a synchronous blocking API suitable for LuaJIT FFI consumption.

## What this gives you

- HTTP/1.1 and HTTP/2 via the CRT
- TLS via s2n (Linux) or platform TLS (macOS/Windows)
- Connection pooling and keep-alive
- DNS load balancing

## Building

### Prerequisites

- CMake 3.9+
- C compiler (gcc, clang, MSVC)
- Git (for submodules)

### Steps

```bash
git clone --recursive https://github.com/YOUR_ORG/aws-crt-lua.git
cd aws-crt-lua
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

This produces `libaws-crt-lua.dylib` (macOS) or `libaws-crt-lua.so` (Linux).

### Running tests

```bash
# From the repo root, with the built library on the path:
cd test
LD_LIBRARY_PATH=../build DYLD_LIBRARY_PATH=../build luajit test_http.lua
```

## Usage

```lua
local crt_http = require("aws_crt.http")

-- Create a client (returns http_client function + close function)
local http_client, close = crt_http.new({ max_connections = 16 })

-- Make a request (blocks until response is complete)
local resp, err = http_client({
    method = "GET",
    url = "https://example.com/",
    headers = { ["Accept"] = "text/html" },
})

if err then
    print("error: " .. err.message)
else
    print("status: " .. resp.status_code)
    print("body: " .. resp.body())
end

-- Clean up when done
close()
```

## Interface

The HTTP client conforms to the same interface as `smithy.http` transports:

```lua
function http_client(request) -> response, err
```

**Request:**
```lua
{
    method = "POST",
    url = "https://...",
    headers = { ["Content-Type"] = "application/json" },
    body = "..." or function() return chunk end,
}
```

**Response:**
```lua
{
    status_code = 200,
    headers = { ["content-type"] = "application/json" },
    body = function() return chunk_or_nil end,
}
```

## Architecture

```
┌─────────────────────────────────────────────┐
│  LuaJIT (your code)                         │
│    require("aws_crt.http")                  │
│    ffi.load("aws-crt-lua")                  │
└──────────────────┬──────────────────────────┘
                   │ FFI call (blocks)
┌──────────────────▼──────────────────────────┐
│  C shim (src/crt_http.c)                    │
│    - Synchronous wrapper                    │
│    - mutex + condvar to bridge async→sync   │
│    - Owns event loop, connection manager    │
└──────────────────┬──────────────────────────┘
                   │ CRT internal callbacks
┌──────────────────▼──────────────────────────┐
│  AWS CRT (aws-c-http, aws-c-io, etc.)      │
│    - Async event-loop-driven HTTP client    │
│    - TLS, HTTP/2, connection pooling        │
└─────────────────────────────────────────────┘
```

## License

Apache-2.0
