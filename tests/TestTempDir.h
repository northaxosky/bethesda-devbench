#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <process.h>
#include <string_view>

// Unique per process and instance so parallel test runs never share files.
class TempDir
{
public:
	explicit TempDir(std::string_view a_name)
	{
		static std::atomic_uint64_t sequence{ 0 };
		path = std::filesystem::temp_directory_path() /
		       std::format("devbench_{}_{}_{}", a_name, _getpid(), ++sequence);
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path);
	}

	TempDir(const TempDir&) = delete;
	TempDir& operator=(const TempDir&) = delete;

	~TempDir()
	{
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
	}

	std::filesystem::path path;
};
