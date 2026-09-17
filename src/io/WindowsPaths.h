#pragma once

#include <filesystem>

namespace dvb::io
{
	std::filesystem::path PhysicalFilePath(const std::filesystem::path& a_path);
}
