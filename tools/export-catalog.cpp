#include "CorePch.h"
#include "GameProfile.h"
#include "tools/ToolCatalog.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

int wmain(int a_argc, wchar_t** a_argv)
{
	try
	{
		if (a_argc != 2 && a_argc != 3)
			throw std::runtime_error(
				"usage: devbench-catalog [fo4|se|vr] <output.json>");
		const std::filesystem::path output =
			std::filesystem::absolute(a_argv[a_argc - 1]);
		dvb::json catalog;
		if (a_argc == 2)
		{
			catalog = dvb::tools::BuildCoreToolCatalogBundle();
		}
		else
		{
			const std::wstring game = a_argv[1];
			const auto profile =
				game == L"fo4" ? dvb::Fallout4Profile() :
				game == L"se" ? dvb::SkyrimProfile(dvb::RuntimeVariant::kSkyrimAE) :
				game == L"vr" ? dvb::SkyrimProfile(dvb::RuntimeVariant::kSkyrimVR) :
					throw std::runtime_error("catalog game must be fo4, se, or vr");
			catalog = dvb::tools::BuildCoreToolCatalog(profile);
		}
		const std::string content = catalog.dump(2) + "\n";
		if (std::filesystem::exists(output))
		{
			std::ifstream existing(output, std::ios::binary);
			if (!existing)
				throw std::runtime_error("could not read existing tool catalog");
			const std::string previous{ std::istreambuf_iterator<char>(existing), {} };
			if (existing.bad())
				throw std::runtime_error("could not finish reading existing tool catalog");
			if (previous == content)
				return 0;
		}
		std::filesystem::create_directories(output.parent_path());
		auto temporary = output;
		temporary += L".tmp";
		{
			std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
			stream.exceptions(std::ios::failbit | std::ios::badbit);
			stream.write(content.data(), static_cast<std::streamsize>(content.size()));
			stream.close();
		}
		if (!MoveFileExW(temporary.c_str(), output.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			const auto      error = GetLastError();
			std::error_code cleanupError;
			std::filesystem::remove(temporary, cleanupError);
			if (cleanupError)
				std::cerr << "catalog temporary-file cleanup failed: " << cleanupError.message() << '\n';
			throw std::runtime_error("could not replace tool catalog: Win32 error " + std::to_string(error));
		}
		return 0;
	}
	catch (const std::exception& a_error)
	{
		std::cerr << "devbench-catalog: " << a_error.what() << '\n';
		return 1;
	}
}
