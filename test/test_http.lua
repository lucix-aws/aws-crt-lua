#!/usr/bin/env luajit
-- Test: make a real HTTP request using the CRT HTTP client.
-- Requires: libaws-crt-lua.dylib/so built and on the library path.

package.path = package.path .. ";../lua/?.lua;../lua/?/init.lua"

local crt_http = require("aws_crt.http")

print("=== aws-crt-lua HTTP client test ===")
print()

-- Test 1: Simple GET to a public endpoint
print("[test 1] GET https://httpbin.org/get")
local http_client, close = crt_http.new()

local resp, err = http_client({
    method = "GET",
    url = "https://httpbin.org/get",
    headers = {
        ["User-Agent"] = "aws-crt-lua/0.1",
        ["Accept"] = "application/json",
    },
})

if err then
    print("  FAIL: " .. err.message)
    close()
    os.exit(1)
end

print("  status: " .. resp.status_code)
assert(resp.status_code == 200, "expected 200, got " .. resp.status_code)

local body = resp.body()
assert(body and #body > 0, "expected non-empty body")
print("  body length: " .. #body)
print("  content-type: " .. (resp.headers["content-type"] or "nil"))
print("  PASS")
print()

-- Test 2: POST with body
print("[test 2] POST https://httpbin.org/post")
local resp2, err2 = http_client({
    method = "POST",
    url = "https://httpbin.org/post",
    headers = {
        ["Content-Type"] = "application/json",
        ["User-Agent"] = "aws-crt-lua/0.1",
    },
    body = '{"hello":"world"}',
})

if err2 then
    print("  FAIL: " .. err2.message)
    close()
    os.exit(1)
end

print("  status: " .. resp2.status_code)
assert(resp2.status_code == 200, "expected 200, got " .. resp2.status_code)

local body2 = resp2.body()
assert(body2 and body2:find('"hello"'), "expected body to echo our JSON")
print("  body contains our JSON: yes")
print("  PASS")
print()

-- Test 3: Body as reader function
print("[test 3] POST with body reader function")
local chunks_sent = { '{"chunked":', '"true"}' }
local chunk_idx = 0
local resp3, err3 = http_client({
    method = "POST",
    url = "https://httpbin.org/post",
    headers = {
        ["Content-Type"] = "application/json",
        ["User-Agent"] = "aws-crt-lua/0.1",
    },
    body = function()
        chunk_idx = chunk_idx + 1
        return chunks_sent[chunk_idx]
    end,
})

if err3 then
    print("  FAIL: " .. err3.message)
    close()
    os.exit(1)
end

print("  status: " .. resp3.status_code)
assert(resp3.status_code == 200)
local body3 = resp3.body()
assert(body3 and body3:find('"chunked"'), "expected echoed body")
print("  PASS")
print()

-- Cleanup
close()
print("=== All tests passed ===")
