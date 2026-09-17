-- devbench — F4SE MCP/REST host

set_xmakever("2.8.2")

includes("lib/commonlibf4")
includes("xmake/cpp-mcp.lua")

local plugin_name = "devbench"
local plugin_version = "0.1.0"
local plugin_version_major, plugin_version_minor, plugin_version_patch =
    plugin_version:match("^(%d+)%.(%d+)%.(%d+)$")

local dev_mod_folder = "DevBench - Dev"

set_project(plugin_name)
set_version(plugin_version)
set_license("GPL-3.0")
set_languages("c++26")
set_toolchains("msvc")
set_warnings("allextra")
set_encodings("utf-8")

set_policy("build.optimization.lto", true)
set_policy("package.requires_lock", true)

add_rules("mode.debug", "mode.release", "mode.releasedbg")
set_defaultmode("releasedbg")
add_rules("plugin.vsxmake.autoupdate")
add_rules("plugin.compile_commands.autoupdate", { lsp = "clangd" })

set_config("commonlib_toml", true)

add_requires("nlohmann_json")
add_requires("stb")

-- CommonLib pins itself to C++23; keep every CommonLib TU on the same CRT feature set.
for _, dep in ipairs({ "commonlib-shared", "commonlibf4" }) do
    target(dep, function()
        set_languages("c++26")
    end)
end

local core_sources = {
    "shared/ToolRegistry.cpp",
    "shared/ToolExtensions.cpp",
    "shared/Ssim.cpp",
    "src/ConfigPolicy.cpp",
    "src/Compatibility.cpp",
    "src/RuntimeProtocol.cpp",
    "src/io/**.cpp",
    "src/tools/**.cpp"
}

target("devbench-core", function()
    set_kind("static")
    set_default(false)
    add_packages("nlohmann_json", "stb", "spdlog", { public = true })
    add_includedirs("shared", "src", "include", { public = true })
    add_deps("cpp-mcp")
    add_syslinks("ws2_32", { public = true })
    add_defines("_WINSOCKAPI_", 'PLUGIN_VERSION="' .. plugin_version .. '"')
    add_cxflags("/utf-8", "/EHsc", { force = true })
    set_pcxxheader("src/CorePch.h")
    add_files(table.unpack(core_sources))
end)

target("devbench-bridge", function()
    set_kind("phony")
    set_default(false)
    set_group("tools")
    add_deps("devbench-catalog")
    on_build(function(target)
        import("core.project.depend")
        import("lib.detect.find_tool")
        local root = path.join(os.projectdir(), "bridge")
        local node = find_tool("node")
        if not node then
            raise("Node.js is required to build the bridge.")
        end
        if not os.isfile(path.join(root, "node_modules", "typescript", "bin", "tsc")) then
            raise("Bridge dependencies missing. Run `npm ci` in bridge first.")
        end
        local executable = path.join(root, "standalone", "devbench-bridge.exe")
        local helper = path.join(root, "standalone", "platform", "windows-session.ps1")
        local shim = path.join(root, "standalone", "platform", "LaunchShim.cs")
        local launcher = path.join(root, "standalone", "platform", "devbench-launch.exe")
        local catalog = path.join(root, "src", "generated", "core-tools.json")
        os.vrunv(target:dep("devbench-catalog"):targetfile(), { catalog })
        local files = table.join(
            os.files(path.join(root, "src", "**")),
            os.files(path.join(root, "platform", "**")),
            os.files(path.join(root, "scripts", "*.mjs")),
            { path.join(root, "package.json"), path.join(root, "package-lock.json"), path.join(root, "tsconfig.json") }
        )
        depend.on_changed(function()
            os.vrunv(node.program, { path.join(root, "scripts", "build.mjs"), "--catalog-ready" }, { curdir = root })
            os.vrunv(node.program, { path.join(root, "scripts", "compile.mjs") }, { curdir = root })
            if not os.isfile(executable) or not os.isfile(helper) or not os.isfile(shim) or not os.isfile(launcher) then
                raise("Bridge build did not produce the executable and Windows helper.")
            end
        end, {
            dependfile = path.join(os.projectdir(), "build", ".deps", "devbench-bridge"),
            files = files,
            changed = not os.isfile(executable) or not os.isfile(helper) or not os.isfile(shim) or not os.isfile(launcher)
        })
    end)
end)

target("devbench-catalog", function()
    set_kind("binary")
    set_default(false)
    set_group("tools")
    add_deps("devbench-core")
    add_defines("_WINSOCKAPI_")
    add_cxflags("/utf-8", "/EHsc", { force = true })
    add_files("tools/export-catalog.cpp")
end)

target("devbench-interop", function()
    set_default(false)
    set_group("tests")
    add_cxxflags("/permissive-", "/Zc:preprocessor", { public = true })
    add_cxflags("/utf-8", "/EHsc", { force = true })
    add_rules("commonlibf4.plugin", {
        name = "devbench-interop",
        author = "Kuz",
        description = "Test-only DevBench C-ABI validation consumer",
        plugin_template = "tests/interop/commonlibf4-plugin.cpp.in"
    })
    add_packages("nlohmann_json")
    add_defines("_WINSOCKAPI_", "DEVBENCH_INTEROP_ENABLE_CAPTURE")
    add_files("tests/interop/main.cpp", "include/DevBenchAPI.cpp")
    add_headerfiles("include/DevBenchAPI.h", "include/DevBenchAPIVersion.h")
    add_includedirs("include")
end)

target(plugin_name, function()
    add_cxxflags("/permissive-", "/Zc:preprocessor", { public = true })

    add_rules("commonlibf4.plugin", {
        name = plugin_name,
        author = "Kuz",
        description = "MCP and REST test bench host for Fallout 4 mod development",
        plugin_template = "res/commonlibf4-plugin.cpp.in"
    })

    add_deps("cpp-mcp", "devbench-core", "devbench-bridge")
    add_packages("nlohmann_json", "stb")
    add_syslinks("ws2_32", "shell32", "ole32", "uuid")
    add_defines("_WINSOCKAPI_")

    add_files("shared/**.cpp", "src/**.cpp")
    remove_files(table.unpack(core_sources))
    add_headerfiles("shared/**.h", "src/**.h", "include/**.h")
    add_includedirs("shared", "src", "include")
    set_pcxxheader("src/pch.h")
    add_installfiles("bridge/standalone/devbench-bridge.exe", {
        prefixdir = "F4SE/Plugins/devbench"
    })
    add_installfiles("bridge/standalone/platform/windows-session.ps1", {
        prefixdir = "F4SE/Plugins/devbench/platform"
    })
    add_installfiles("bridge/standalone/platform/LaunchShim.cs", {
        prefixdir = "F4SE/Plugins/devbench/platform"
    })
    add_installfiles("bridge/standalone/platform/devbench-launch.exe", {
        prefixdir = "F4SE/Plugins/devbench/platform"
    })

    set_configvar("VERSION_MAJOR", tonumber(plugin_version_major))
    set_configvar("VERSION_MINOR", tonumber(plugin_version_minor))
    set_configvar("VERSION_PATCH", tonumber(plugin_version_patch))
    set_configvar("VERSION_STRING", plugin_version)
    add_configfiles("src/Version.h.in")
    set_configdir("$(builddir)/.gens/devbench/$(plat)/$(arch)/$(mode)")
    add_includedirs("$(builddir)/.gens/devbench/$(plat)/$(arch)/$(mode)")

    add_defines(
        'PLUGIN_NAME="' .. plugin_name .. '"',
        'PLUGIN_VERSION="' .. plugin_version .. '"',
        "PLUGIN_VERSION_MAJOR=" .. plugin_version_major,
        "PLUGIN_VERSION_MINOR=" .. plugin_version_minor,
        "PLUGIN_VERSION_PATCH=" .. plugin_version_patch
    )

    -- The CommonLib rule assigns installdir in its own on_config; claim it afterward.
    on_config(function(target)
        local mods_root = os.getenv("FO4_DEV_MODS")

        if mods_root then
            target:set("installdir", path.join(mods_root, dev_mod_folder))
        end
    end)
end)

-- This target intentionally has no CommonLibF4 dependency. It proves shared/ remains game-free.
target("devbench-tests", function()
    set_kind("binary")
    set_default(false)
    set_languages("c++26")
    add_packages("nlohmann_json", "stb")
    add_includedirs("shared", "src", "include", "tests")
    add_deps("devbench-core")
    add_files(
        "tests/test_main.cpp",
        "tests/ToolRegistry_test.cpp",
        "tests/ScenarioPolicy_test.cpp",
        "tests/McpContent_test.cpp",
        "tests/Ssim_test.cpp",
        "tests/ConsoleTool_test.cpp",
        "tests/InspectTool_test.cpp",
        "tests/ConfigPolicy_test.cpp",
        "tests/ScenarioTool_test.cpp",
        "tests/GameTool_test.cpp",
        "tests/ScenarioGame_test.cpp",
        "tests/RuntimeProtocol_test.cpp",
        "tests/FoundationContract_test.cpp",
        "tests/InspectionQuery_test.cpp",
        "tests/Fallout4SaveHeader_test.cpp",
        "tests/Fallout4Calendar_test.cpp",
        "tests/PapyrusTool_test.cpp",
        "tests/KeyboardInputState_test.cpp",
        "tests/InputTool_test.cpp",
        "tests/StallWatchdog_test.cpp",
        "tests/ExtensionDescriptorRefresh_test.cpp",
        "tests/RecordingTool_test.cpp",
        "tests/MenuTool_test.cpp",
        "tests/CameraTool_test.cpp",
        "tests/CaptureTool_test.cpp",
        "tests/RestTool_test.cpp"
    )
    add_headerfiles("tests/*.h")
    set_pcxxheader("tests/pch.h")
    add_defines("_WINSOCKAPI_")
    add_defines('PLUGIN_VERSION="' .. plugin_version .. '"')
    add_cxflags("/utf-8", "/EHsc", { force = true })
end)
