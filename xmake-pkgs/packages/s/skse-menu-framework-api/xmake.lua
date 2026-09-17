package("skse-menu-framework-api")
set_kind("library", { headeronly = true })
set_homepage("https://github.com/alandtse/SKSE-Menu-Framework-3")
set_description("Client SDK header for SKSE Menu Framework 3")
set_license("LGPL-2.1")

add_urls("https://github.com/alandtse/SKSE-Menu-Framework-3.git", { submodules = false })
add_versions("3.7.0", "24c40c925650c033b42fea2f61bd5a0382e03e74")

on_install(function(package)
    os.cp("resources/SKSEMenuFramework.h", package:installdir("include"))
end)
