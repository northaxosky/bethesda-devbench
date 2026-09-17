package("fuck-api")
set_kind("library", { headeronly = true })
set_homepage("https://github.com/alandtse/FUCK")
set_description("Client API header for FUCK")
set_license("MIT")

add_urls("https://github.com/alandtse/FUCK.git", { submodules = false })
add_versions("1.0.0", "2522eccf1d2096a794f283782a5c78a16ab83f74")

on_install(function(package)
    os.cp("src/FUCK_API.h", package:installdir("include"))
end)
