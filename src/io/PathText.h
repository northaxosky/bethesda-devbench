#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace dvb
{
	/// Utf8() with forward slashes, for paths the contracts report that way.
	inline std::string Utf8Generic(const std::filesystem::path& a_path)
	{
		const std::u8string bytes = a_path.generic_u8string();
		return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
	}

	/// Inverse of Utf8(): a path from JSON or client text, never the ANSI page.
	inline std::filesystem::path PathFromUtf8(std::string_view a_text)
	{
		return std::filesystem::path(std::u8string(
			reinterpret_cast<const char8_t*>(a_text.data()), a_text.size()));
	}
}
