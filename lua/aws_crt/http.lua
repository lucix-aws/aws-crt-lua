-- aws_crt.http: LuaJIT FFI binding to the aws-crt-lua HTTP shim.
-- Conforms to the smithy.http client interface: function(request) -> response, err

local ffi = require("ffi")

ffi.cdef[[
typedef struct aws_crt_lua_client aws_crt_lua_client;

typedef struct aws_crt_lua_header {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
} aws_crt_lua_header;

typedef struct aws_crt_lua_request_options {
    const char *method;
    const char *uri;
    const aws_crt_lua_header *headers;
    size_t num_headers;
    const uint8_t *body;
    size_t body_len;
} aws_crt_lua_request_options;

typedef struct aws_crt_lua_response {
    int status_code;
    aws_crt_lua_header *headers;
    size_t num_headers;
    uint8_t *body;
    size_t body_len;
    int error_code;
    const char *error_message;
} aws_crt_lua_response;

void aws_crt_lua_init(void);
void aws_crt_lua_cleanup(void);
aws_crt_lua_client *aws_crt_lua_client_new(size_t max_connections);
void aws_crt_lua_client_free(aws_crt_lua_client *client);
aws_crt_lua_response *aws_crt_lua_request(aws_crt_lua_client *client, const aws_crt_lua_request_options *options);
void aws_crt_lua_response_free(aws_crt_lua_response *response);
]]

local lib = ffi.load("aws-crt-lua")

local M = {}
local initialized = false

function M.available()
    local ok = pcall(ffi.load, "aws-crt-lua")
    return ok
end

--- Create a new CRT HTTP client.
--- Returns: http_client_fn, close_fn
function M.new(opts)
    if not initialized then
        lib.aws_crt_lua_init()
        initialized = true
    end

    local max_conn = (opts and opts.max_connections) or 16
    local client = lib.aws_crt_lua_client_new(max_conn)
    if client == nil then
        error("aws_crt_lua_client_new failed")
    end

    local closed = false

    local function http_client(request)
        if closed then
            return nil, { type = "http", code = "ClientClosed", message = "client has been closed" }
        end

        -- Build C headers array
        local header_list = {}
        local header_count = 0
        for k, v in pairs(request.headers or {}) do
            header_count = header_count + 1
            header_list[header_count] = { name = k, value = v }
        end

        local c_headers = nil
        if header_count > 0 then
            c_headers = ffi.new("aws_crt_lua_header[?]", header_count)
            for i, h in ipairs(header_list) do
                c_headers[i - 1].name = h.name
                c_headers[i - 1].name_len = #h.name
                c_headers[i - 1].value = h.value
                c_headers[i - 1].value_len = #h.value
            end
        end

        -- Read body (supports reader functions or plain strings)
        local body_str = nil
        if request.body then
            if type(request.body) == "function" then
                local chunks = {}
                while true do
                    local chunk = request.body()
                    if not chunk then break end
                    chunks[#chunks + 1] = chunk
                end
                body_str = table.concat(chunks)
            elseif type(request.body) == "string" then
                body_str = request.body
            end
        end

        -- Build request options
        local req_opts = ffi.new("aws_crt_lua_request_options")
        req_opts.method = request.method
        req_opts.uri = request.url
        req_opts.headers = c_headers
        req_opts.num_headers = header_count
        if body_str and #body_str > 0 then
            req_opts.body = body_str
            req_opts.body_len = #body_str
        end

        -- Execute (blocks until response is complete)
        local resp = lib.aws_crt_lua_request(client, req_opts)
        if resp == nil then
            return nil, { type = "http", code = "CrtError", message = "request returned NULL" }
        end

        -- Check for CRT-level error
        if resp.error_code ~= 0 then
            local msg = resp.error_message ~= nil and ffi.string(resp.error_message) or "unknown CRT error"
            lib.aws_crt_lua_response_free(resp)
            return nil, { type = "http", code = "CrtError", message = msg }
        end

        -- Convert headers
        local resp_headers = {}
        for i = 0, tonumber(resp.num_headers) - 1 do
            local name = ffi.string(resp.headers[i].name, resp.headers[i].name_len):lower()
            local value = ffi.string(resp.headers[i].value, resp.headers[i].value_len)
            resp_headers[name] = value
        end

        -- Copy body into Lua string
        local resp_body = ""
        if resp.body ~= nil and resp.body_len > 0 then
            resp_body = ffi.string(resp.body, resp.body_len)
        end

        local status_code = resp.status_code
        lib.aws_crt_lua_response_free(resp)

        -- Return response with body as a one-shot reader
        local body_read = false
        return {
            status_code = status_code,
            headers = resp_headers,
            body = function()
                if body_read then return nil end
                body_read = true
                return resp_body
            end,
        }, nil
    end

    local function close()
        if not closed then
            closed = true
            lib.aws_crt_lua_client_free(client)
        end
    end

    return http_client, close
end

return M
