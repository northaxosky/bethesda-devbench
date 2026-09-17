#include "WindowsPaths.h"

#include <Windows.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace dvb::io
{
	std::filesystem::path PhysicalFilePath(const std::filesystem::path& a_path)
	{
		const auto raw = ::CreateFileW(a_path.c_str(), 0,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (raw == INVALID_HANDLE_VALUE)
			throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "open physical path");
		const std::unique_ptr<void, decltype(&::CloseHandle)> handle(raw, &::CloseHandle);
		std::vector<wchar_t>                                  buffer(512);
		for (;;)
		{
			const auto length = ::GetFinalPathNameByHandleW(raw, buffer.data(),
				static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
			if (length == 0)
				throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "resolve physical path");
			if (length < buffer.size())
			{
				std::wstring path(buffer.data(), length);
				if (path.starts_with(LR"(\\?\UNC\)"))
					path = L"\\\\" + path.substr(8);
				else if (path.starts_with(LR"(\\?\)"))
					path.erase(0, 4);
				return path;
			}
			if (length >= 32768)
				throw std::runtime_error("physical path exceeds the Windows path limit");
			buffer.resize(static_cast<std::size_t>(length) + 1);
		}
	}
}
