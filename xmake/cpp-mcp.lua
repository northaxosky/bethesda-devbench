-- In-tree server-only build of the pinned cpp-mcp submodule.
local cpp_mcp_root = path.join(os.projectdir(), "lib", "cpp-mcp")
local patched_inc = path.join(os.projectdir(), "build", "cpp-mcp-patched", "include")

target("cpp-mcp")
set_kind("static")
set_languages("c++17")
set_warnings("none")
set_group("extern")

add_files(
    path.join(cpp_mcp_root, "src", "mcp_message.cpp"),
    path.join(cpp_mcp_root, "src", "mcp_resource.cpp"),
    path.join(cpp_mcp_root, "src", "mcp_server.cpp"),
    path.join(cpp_mcp_root, "src", "mcp_tool.cpp")
)

add_includedirs(patched_inc, path.join(cpp_mcp_root, "common"), { public = true })

add_defines(
    "MCP_MAX_SESSIONS=10",
    "MCP_SESSION_TIMEOUT=30",
    "_WINSOCKAPI_",
    "_CRT_SECURE_NO_WARNINGS",
    { public = true }
)

add_cxflags("/utf-8", "/bigobj", "/std:c++17", { force = true })
add_packages("nlohmann_json")
add_syslinks("ws2_32", "crypt32")

on_load(function(target)
    local function write_if_changed(file, content)
        if not os.isfile(file) or io.readfile(file) ~= content then
            io.writefile(file, content)
        end
    end
    local function replace_once(content, anchor, replacement)
        local first, last = content:find(anchor, 1, true)
        if not first or content:find(anchor, last + 1, true) then
            raise("cpp-mcp: missing or ambiguous httplib patch anchor: %s", anchor)
        end
        return content:sub(1, first - 1) .. replacement .. content:sub(last + 1)
    end

    local root = path.join(os.projectdir(), "lib", "cpp-mcp")
    local out = path.join(os.projectdir(), "build", "cpp-mcp-patched", "include")
    if not os.isfile(path.join(root, "src", "mcp_server.cpp")) then
        raise("cpp-mcp submodule missing. Run: git submodule update --init --recursive lib/cpp-mcp")
    end
    os.mkdir(out)
    for _, hdr in ipairs(os.files(path.join(root, "include", "*.h"))) do
        local name = path.filename(hdr)
        local content = io.readfile(hdr)
        if name == "mcp_message.h" then
            if not content:find('#include "json.hpp"', 1, true) then
                raise(
                    'cpp-mcp: expected `#include "json.hpp"` in mcp_message.h; upstream changed — review xmake/cpp-mcp.lua'
                )
            end
            content = content:gsub('#include "json%.hpp"', "#include <nlohmann/json.hpp>")
        elseif name == "mcp_server.h" then
            local anchor = "\nprivate:\n    std::string host_;"
            if not content:find(anchor, 1, true) then
                raise(
                    "cpp-mcp: expected private section anchor in mcp_server.h; upstream changed — review xmake/cpp-mcp.lua"
                )
            end
            local getter = "\n"
                .. "    /**\n"
                .. "     * @brief Access the underlying httplib server to register custom routes.\n"
                .. "     * Lets a consumer mount additional endpoints (e.g. a REST facade) on the\n"
                .. "     * same host/port as the MCP transport. Register routes before start().\n"
                .. "     */\n"
                .. "    httplib::Server* http() { return http_server_.get(); }\n"
                .. anchor
            content = content:gsub("\nprivate:\n    std::string host_;", getter, 1)
        end
        write_if_changed(path.join(out, name), content)
    end

    -- Reject stale instances after consuming the body, before routing to any tool.
    -- The upstream pre-routing hook leaves unread POST bytes and can reset the connection.
    local http = io.readfile(path.join(root, "common", "httplib.h"))
    local declaration = "  Server &set_pre_routing_handler(HandlerWithResponse handler);"
    http = replace_once(http, declaration,
        declaration .. "\n  Server &set_pre_request_handler(HandlerWithResponse handler);")
    local member = "  HandlerWithResponse pre_routing_handler_;"
    http = replace_once(http, member,
        member .. "\n  HandlerWithResponse pre_request_handler_;")
    local definition = "inline Server &Server::set_pre_routing_handler(HandlerWithResponse handler) {\n"
        .. "  pre_routing_handler_ = std::move(handler);\n"
        .. "  return *this;\n}"
    http = replace_once(http, definition, definition .. "\n\n"
        .. "inline Server &Server::set_pre_request_handler(HandlerWithResponse handler) {\n"
        .. "  pre_request_handler_ = std::move(handler);\n"
        .. "  return *this;\n}")
    local dispatch = "  // Regular handler\n"
    http = replace_once(http, dispatch,
        "  if (pre_request_handler_ &&\n"
        .. "      pre_request_handler_(req, res) == HandlerResponse::Handled) {\n"
        .. "    return true;\n"
        .. "  }\n\n" .. dispatch)
    write_if_changed(path.join(out, "httplib.h"), http)
end)
target_end()
