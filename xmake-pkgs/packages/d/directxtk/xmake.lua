-- CommonLibSSE-NG consumes DirectXTK's inline SimpleMath types only. Installing
-- the headers avoids DirectXTK's unrelated shader/tool build during plugin builds.
package("directxtk")
set_kind("library", { headeronly = true })
set_homepage("https://github.com/microsoft/DirectXTK")
set_description("DirectX Tool Kit headers")
set_license("MIT")

add_urls("https://github.com/microsoft/DirectXTK/archive/24.2.0.zip")
add_versions(
    "24.2.0",
    "edb643b2444ff24925339cfb1bc9f76c671d5404a5549d32ecaa0d61bbab28c9"
)

on_install(function(package)
    os.cp("Inc/*", package:installdir("include"))
end)
