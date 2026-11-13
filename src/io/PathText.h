#pragma once

#include <filesystem>
#include <string>

namespace dvb
{
	/// Utf8() with forward slashes, for paths the contracts report that way.
	inline std::string Utf8Generic(const std::filesystem::path& a_path)
	{
		const std::u8string bytes = a_path.generic_u8string();
		return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
	}
}
