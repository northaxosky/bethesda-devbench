-- devbench — shared native MCP/REST host with one RE adapter selected per build

set_xmakever("2.8.2")

option("game")
    set_default("fallout4")
    set_showmenu(true)
    set_values("fallout4", "skyrimse")
    set_description("Native game adapter")
    after_check(function(game)
        import("core.project.config")
        config.set("builddir", path.join("build", game:value()), { force = true })
    end)
option_end()

local selected_game = get_config("game") or "fallout4"
if selected_game ~= "fallout4" and selected_game ~= "skyrimse" then
    raise("unsupported game '%s' (expected fallout4 or skyrimse)", selected_game)
end

local plugin_name = "devbench"
local plugin_version = "0.1.0"
local version_major, version_minor, version_patch =
    plugin_version:match("^(%d+)%.(%d+)%.(%d+)$")
local game_builddir = path.join("build", selected_game)
local generated_dir =
    path.join(game_builddir, ".gens", "$(target)", "$(plat)", "$(arch)", "$(mode)")

set_project(plugin_name)
set_version(plugin_version)
set_license("GPL-3.0")
set_languages("c++26")
set_toolchains("msvc")
set_warnings("allextra")
set_encodings("utf-8")
set_targetdir(path.join(game_builddir, "$(plat)", "$(arch)", "$(mode)"))
set_objectdir(path.join(game_builddir, ".objs", "$(target)", "$(plat)", "$(arch)", "$(mode)"))

set_policy("build.optimization.lto", true)
set_policy("package.requires_lock", true)

add_rules("mode.debug", "mode.release", "mode.releasedbg")
set_defaultmode("releasedbg")
add_rules("plugin.vsxmake.autoupdate")

if selected_game == "fallout4" then
    set_config("commonlib_toml", true)
    includes("lib/commonlibf4")
else
    add_repositories("devbench-pkgs xmake-pkgs")
    includes("lib/commonlibsse-ng")
    -- These options are declared by the included project, so force them only
    -- after inclusion; setting an unknown option before inclusion is ignored.
    set_config("rex_ini", true)
    set_config("skse_xbyak", true)
end
includes("xmake/cpp-mcp.lua")

-- Included SDK projects pin their own language levels. Keep the framework and
-- selected RE SDK on the repository's established MSVC/C++26 contract.
set_languages("c++26")
if selected_game == "fallout4" then
    for _, dependency in ipairs({ "commonlib-shared", "commonlibf4" }) do
        target(dependency, function()
            set_languages("c++26")
        end)
    end
else
    target("commonlibsse-ng", function()
        set_languages("c++26")
    end)
end

add_requires("nlohmann_json")
add_requires("stb")

if selected_game == "skyrimse" then
    add_requires("skse-menu-framework-api 3.7.0")
    add_requires("fuck-api 1.0.0")
    add_requires("imgui")
    add_requires("simpleini")
    add_requires("xbyak v7.06")
end

local function common_compile_settings()
    add_defines(
        "_WINSOCKAPI_",
        'PLUGIN_NAME="' .. plugin_name .. '"',
        'PLUGIN_VERSION="' .. plugin_version .. '"',
        "PLUGIN_VERSION_MAJOR=" .. version_major,
        "PLUGIN_VERSION_MINOR=" .. version_minor,
        "PLUGIN_VERSION_PATCH=" .. version_patch
    )
    add_cxxflags("/permissive-", "/Zc:preprocessor", { public = true })
    add_cxflags("/utf-8", "/EHsc", { force = true })
end

local function version_config()
    set_configdir(generated_dir)
    add_configfiles("src/Version.h.in")
    add_includedirs(generated_dir)
    set_configvar("VERSION_MAJOR", tonumber(version_major))
    set_configvar("VERSION_MINOR", tonumber(version_minor))
    set_configvar("VERSION_PATCH", tonumber(version_patch))
    set_configvar("VERSION_STRING", plugin_version)
end

target("devbench-core", function()
    set_kind("static")
    set_default(false)
    add_packages("nlohmann_json", "stb", "spdlog", { public = true })
    add_deps("cpp-mcp")
    add_syslinks("ws2_32", "shell32", "ole32", "uuid", { public = true })
    add_includedirs(".", "shared", "src", "include", { public = true })
    add_files("shared/**.cpp", "src/**.cpp")
    add_headerfiles("shared/**.h", "src/**.h", "include/**.h")
    set_pcxxheader("src/CorePch.h")
    version_config()
    common_compile_settings()
end)

target("devbench-catalog", function()
    set_kind("binary")
    set_default(false)
    set_group("tools")
    add_deps("devbench-core")
    add_files("tools/export-catalog.cpp")
    common_compile_settings()
end)

local catalog_profiles = { "fo4", "se", "vr" }
for _, profile in ipairs(catalog_profiles) do
    target("devbench-catalog-" .. profile, function()
        set_kind("phony")
        set_default(false)
        set_group("tools")
        add_deps("devbench-catalog")
        on_build(function(target)
            local output =
                path.join(os.projectdir(), game_builddir, "catalog", profile .. ".json")
            os.mkdir(path.directory(output))
            os.vrunv(target:dep("devbench-catalog"):targetfile(), { profile, output })
        end)
    end)
end

target("devbench-catalogs", function()
    set_kind("phony")
    set_default(false)
    set_group("tools")
    add_deps("devbench-catalog")
    on_build(function(target)
        local output =
            path.join(os.projectdir(), game_builddir, "catalog", "core-tools.json")
        os.mkdir(path.directory(output))
        os.vrunv(target:dep("devbench-catalog"):targetfile(), { output })
    end)
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
        local catalog = path.join(root, "src", "generated", "core-tools.json")
        os.vrunv(target:dep("devbench-catalog"):targetfile(), { catalog })
        local executable = path.join(root, "standalone", "devbench-bridge.exe")
        local helper = path.join(root, "standalone", "platform", "windows-session.ps1")
        local shim = path.join(root, "standalone", "platform", "LaunchShim.cs")
        local launcher = path.join(root, "standalone", "platform", "devbench-launch.exe")
        local files = table.join(
            os.files(path.join(root, "src", "**")),
            os.files(path.join(root, "platform", "**")),
            os.files(path.join(root, "scripts", "*.mjs")),
            {
                path.join(root, "package.json"),
                path.join(root, "package-lock.json"),
                path.join(root, "tsconfig.json")
            }
        )
        depend.on_changed(function()
            os.vrunv(node.program, {
                path.join(root, "scripts", "build.mjs"), "--catalog-ready"
            }, { curdir = root })
            os.vrunv(node.program, {
                path.join(root, "scripts", "compile.mjs")
            }, { curdir = root })
            if not os.isfile(executable) or not os.isfile(helper) or
                not os.isfile(shim) or not os.isfile(launcher) then
                raise("Bridge build did not produce all standalone artifacts.")
            end
        end, {
            dependfile = path.join(game_builddir, ".deps", "devbench-bridge"),
            files = files,
            changed = not os.isfile(executable) or not os.isfile(helper) or
                not os.isfile(shim) or not os.isfile(launcher)
        })
    end)
end)

if selected_game == "skyrimse" then
    target("devbench-ui-smf", function()
        set_kind("static")
        set_default(false)
        add_deps("commonlibsse-ng")
        add_packages("skse-menu-framework-api", "nlohmann_json")
        add_includedirs(".", "shared", "src", "game/skyrimse")
        add_defines(
            "_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING",
            "UNICODE", "_UNICODE", "_WINSOCKAPI_",
            "DEVBENCH_GAME_SKYRIMSE"
        )
        add_cxflags("/utf-8", "/EHsc", { force = true })
        add_files("game/skyrimse/ui/RecordingsMenu.cpp")
    end)

    target("devbench-ui-fuck", function()
        set_kind("static")
        set_default(false)
        add_deps("commonlibsse-ng")
        add_packages("fuck-api", "imgui", "simpleini", "nlohmann_json")
        add_includedirs(".", "shared", "src", "game/skyrimse")
        add_defines("UNICODE", "_UNICODE", "_WINSOCKAPI_", "DEVBENCH_GAME_SKYRIMSE")
        add_cxflags("/utf-8", "/EHsc", { force = true })
        add_files("game/skyrimse/ui/RecordingsMenuFuck.cpp")
    end)
end

target(plugin_name, function()
    set_kind("shared")
    set_basename(plugin_name)
    add_deps("devbench-core")
    add_packages("nlohmann_json", "stb")
    add_syslinks("ws2_32", "shell32", "ole32", "uuid")
    add_includedirs(".", "shared", "src", "include")
    set_pcxxheader("src/pch.h")
    version_config()
    common_compile_settings()

    if selected_game == "fallout4" then
        add_deps("commonlibf4")
        add_defines("DEVBENCH_GAME_FALLOUT4")
        add_files("game/fallout4/**.cpp", "res/commonlibf4-plugin.cpp")
    else
        add_deps("commonlibsse-ng", "devbench-ui-smf", "devbench-ui-fuck")
        add_packages("xbyak")
        add_defines("DEVBENCH_GAME_SKYRIMSE", "SKSE_SUPPORT_XBYAK=1")
        add_files("game/skyrimse/**.cpp", "res/commonlibsse-ng-plugin.cpp")
        remove_files(
            "game/skyrimse/ui/RecordingsMenu.cpp",
            "game/skyrimse/ui/RecordingsMenuFuck.cpp"
        )
    end

    -- Build targets intentionally have no install directory or install files.
    -- Packaging/deployment is explicit and never runs as an after-build action.
    on_config(function(target)
        target:set("installdir", nil)
    end)
end)

target("devbench-package", function()
    set_kind("phony")
    set_default(false)
    set_group("package")
    add_deps("devbench", "devbench-bridge", "devbench-catalogs")
end)

if selected_game == "fallout4" then
    target("devbench-interop", function()
        set_kind("shared")
        set_default(false)
        set_group("tests")
        add_deps("commonlibf4")
        add_packages("nlohmann_json")
        add_includedirs("include")
        add_defines(
            "_WINSOCKAPI_",
            "DEVBENCH_INTEROP_ENABLE_CAPTURE",
            "DEVBENCH_GAME_FALLOUT4"
        )
        add_files(
            "tests/interop/main.cpp",
            "include/DevBenchAPI.cpp",
            "res/commonlibf4-plugin.cpp"
        )
        add_headerfiles("include/DevBenchAPI.h", "include/DevBenchAPIVersion.h")
        common_compile_settings()
        on_config(function(target)
            target:set("installdir", nil)
        end)
    end)
else
    target("devbench-api-consumer", function()
        set_kind("static")
        set_default(false)
        set_group("tests")
        add_deps("commonlibsse-ng")
        add_includedirs("include")
        add_defines("DEVBENCH_API_SKSE", "DEVBENCH_GAME_SKYRIMSE")
        add_files("include/DevBenchAPI.cpp")
        common_compile_settings()
    end)
end

target("devbench-tests", function()
    set_kind("binary")
    set_default(false)
    set_group("tests")
    add_deps("devbench-core")
    add_packages("nlohmann_json", "stb")
    add_includedirs(".", "shared", "src", "include", "tests")
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
        "tests/GameProfile_test.cpp",
        "tests/InspectionQuery_test.cpp",
        "tests/Fallout4SaveHeader_test.cpp",
        "tests/Fallout4Calendar_test.cpp",
        "tests/SkyrimSaveHeader_test.cpp",
        "tests/SkyrimCalendar_test.cpp",
        "tests/PapyrusTool_test.cpp",
        "tests/KeyboardInputState_test.cpp",
        "tests/InputTool_test.cpp",
        "tests/StallWatchdog_test.cpp",
        "tests/ExtensionDescriptorRefresh_test.cpp",
        "tests/RecordingTool_test.cpp",
        "tests/SkyrimRecordingActivity_test.cpp",
        "tests/SkyrimVRInputState_test.cpp",
        "tests/MenuTool_test.cpp",
        "tests/CameraTool_test.cpp",
        "tests/CaptureTool_test.cpp",
        "tests/RestTool_test.cpp",
        "game/fallout4/data/Fallout4Calendar.cpp",
        "game/fallout4/data/Fallout4SaveHeader.cpp",
        "game/skyrimse/save/SkyrimCalendar.cpp",
        "game/skyrimse/save/SkyrimSaveHeader.cpp",
        "game/skyrimse/recording/RecordingActivity.cpp",
        "game/skyrimse/vr/VRInputState.cpp"
    )
    add_headerfiles("tests/*.h")
    set_pcxxheader("tests/pch.h")
    common_compile_settings()
end)
