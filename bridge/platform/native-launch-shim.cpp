#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <bcrypt.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace
{
	constexpr std::wstring_view kGameFlag = L"-GameExe";
	constexpr std::string_view  kProtocol = "devbench-launch/1";
	constexpr std::string_view  kPipePrefix = "BethesdaDevBench.Launch.";
	constexpr std::uint32_t     kMaximumFieldSize = 65536;
	constexpr std::uint64_t     kFileTimeTicksPerMillisecond = 10000;
	constexpr DWORD             kConnectTimeoutMilliseconds = 10000;
	constexpr DWORD             kOfferTimeoutMilliseconds = 10000;
	constexpr DWORD             kPollMilliseconds = 25;
	constexpr DWORD             kProcessAccess = PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;

	class Error : public std::runtime_error
	{
	public:
		Error(std::string a_code, std::string a_message) :
			std::runtime_error(std::move(a_message)),
			code_(std::move(a_code))
		{}

		[[nodiscard]] const std::string& Code() const noexcept { return code_; }

	private:
		std::string code_;
	};

	class Handle
	{
	public:
		Handle() noexcept = default;
		explicit Handle(HANDLE a_handle) noexcept : handle_(a_handle) {}
		~Handle() { Reset(); }

		Handle(const Handle&) = delete;
		Handle& operator=(const Handle&) = delete;

		Handle(Handle&& a_other) noexcept : handle_(a_other.Release()) {}

		Handle& operator=(Handle&& a_other) noexcept
		{
			if (this != &a_other)
			{
				Reset(a_other.Release());
			}
			return *this;
		}

		[[nodiscard]] HANDLE   Get() const noexcept { return handle_; }
		[[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

		HANDLE Release() noexcept
		{
			HANDLE result = handle_;
			handle_ = nullptr;
			return result;
		}

		void Reset(HANDLE a_handle = nullptr) noexcept
		{
			if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE)
			{
				CloseHandle(handle_);
			}
			handle_ = a_handle;
		}

	private:
		HANDLE handle_{ nullptr };
	};

	struct ProcessIdentity
	{
		DWORD         pid{};
		DWORD         parentPid{};
		std::uint64_t creationFileTime{};
		std::wstring  imagePath;
	};

	struct OwnedGame
	{
		Handle        handle;
		DWORD         pid{};
		std::uint64_t creationFileTime{};
	};

	struct LaunchGuard
	{
		Handle        loader;
		DWORD         loaderPid{};
		std::uint64_t loaderCreationFileTime{};
	};

	[[nodiscard]] std::string WideToUtf8(std::wstring_view a_value)
	{
		if (a_value.empty())
		{
			return {};
		}
		if (a_value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
		{
			throw Error("TEXT_TOO_LONG", "Text exceeds the supported Windows conversion length.");
		}
		const int sourceLength = static_cast<int>(a_value.size());
		const int length = WideCharToMultiByte(
			CP_UTF8,
			WC_ERR_INVALID_CHARS,
			a_value.data(),
			sourceLength,
			nullptr,
			0,
			nullptr,
			nullptr);
		if (length <= 0)
		{
			throw Error("TEXT_ENCODING_FAILED", "Converting UTF-16 text to UTF-8 failed.");
		}
		std::string result(static_cast<std::size_t>(length), '\0');
		if (WideCharToMultiByte(
				CP_UTF8,
				WC_ERR_INVALID_CHARS,
				a_value.data(),
				sourceLength,
				result.data(),
				length,
				nullptr,
				nullptr) != length)
		{
			throw Error("TEXT_ENCODING_FAILED", "Converting UTF-16 text to UTF-8 failed.");
		}
		return result;
	}

	[[nodiscard]] std::wstring Utf8ToWide(std::string_view a_value)
	{
		if (a_value.empty())
		{
			return {};
		}
		if (a_value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
		{
			throw Error("TEXT_TOO_LONG", "Text exceeds the supported Windows conversion length.");
		}
		const int sourceLength = static_cast<int>(a_value.size());
		const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a_value.data(), sourceLength, nullptr, 0);
		if (length <= 0)
		{
			throw Error("SHIM_PROTOCOL_ERROR", "The broker sent invalid UTF-8.");
		}
		std::wstring result(static_cast<std::size_t>(length), L'\0');
		if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a_value.data(), sourceLength, result.data(), length) != length)
		{
			throw Error("SHIM_PROTOCOL_ERROR", "The broker sent invalid UTF-8.");
		}
		return result;
	}

	[[nodiscard]] std::string WindowsMessage(DWORD a_error)
	{
		wchar_t*    buffer = nullptr;
		const DWORD length = FormatMessageW(
			FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr,
			a_error,
			0,
			reinterpret_cast<wchar_t*>(&buffer),
			0,
			nullptr);
		if (length == 0 || buffer == nullptr)
		{
			return "Win32 error " + std::to_string(a_error);
		}
		std::wstring message(buffer, length);
		LocalFree(buffer);
		while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' '))
		{
			message.pop_back();
		}
		return "Win32 error " + std::to_string(a_error) + ": " + WideToUtf8(message);
	}

	[[noreturn]] void ThrowLastError(std::string a_code, std::string a_action, DWORD a_error = GetLastError())
	{
		throw Error(std::move(a_code), std::move(a_action) + " failed with " + WindowsMessage(a_error) + ".");
	}

	void EnsureDirectory(const std::wstring& a_path)
	{
		if (!CreateDirectoryW(a_path.c_str(), nullptr))
		{
			const DWORD error = GetLastError();
			if (error != ERROR_ALREADY_EXISTS)
			{
				return;
			}
		}
	}

	void ReportFailure(const Error& a_error)
	{
		const std::string diagnostic = "[devbench-launch] " + a_error.Code() + ": " + a_error.what() + "\r\n";
		const HANDLE      stderrHandle = GetStdHandle(STD_ERROR_HANDLE);
		if (stderrHandle != nullptr && stderrHandle != INVALID_HANDLE_VALUE)
		{
			DWORD written = 0;
			WriteFile(stderrHandle, diagnostic.data(), static_cast<DWORD>(diagnostic.size()), &written, nullptr);
		}

		const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
		if (required <= 1)
		{
			return;
		}
		std::wstring localAppData(required, L'\0');
		const DWORD  copied = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData.data(), required);
		if (copied == 0 || copied >= required)
		{
			return;
		}
		localAppData.resize(copied);
		const std::wstring devbenchDirectory = localAppData + L"\\devbench";
		const std::wstring logDirectory = devbenchDirectory + L"\\fo4";
		EnsureDirectory(devbenchDirectory);
		EnsureDirectory(logDirectory);
		const std::wstring logPath = logDirectory + L"\\launch-shim.log";
		Handle             log(CreateFileW(
			logPath.c_str(),
			GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_DELETE,
			nullptr,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr));
		if (!log)
		{
			return;
		}
		DWORD written = 0;
		WriteFile(log.Get(), diagnostic.data(), static_cast<DWORD>(diagnostic.size()), &written, nullptr);
	}

	[[nodiscard]] bool IsAbsolutePath(std::wstring_view a_path)
	{
		if (a_path.size() >= 3 && std::iswalpha(a_path[0]) != 0 && a_path[1] == L':' &&
			(a_path[2] == L'\\' || a_path[2] == L'/'))
		{
			return true;
		}
		return a_path.size() >= 2 && a_path[0] == L'\\' && a_path[1] == L'\\';
	}

	[[nodiscard]] std::wstring CanonicalizeExistingFile(const std::wstring& a_path)
	{
		if (!IsAbsolutePath(a_path))
		{
			throw Error("INVALID_PATH", "The executable path must be absolute.");
		}
		Handle file(CreateFileW(
			a_path.c_str(),
			0,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr));
		if (!file)
		{
			ThrowLastError("PATH_ACCESS_FAILED", "Opening " + WideToUtf8(a_path));
		}

		std::vector<wchar_t> buffer(32768);
		const DWORD          length = GetFinalPathNameByHandleW(file.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), 0);
		if (length == 0)
		{
			ThrowLastError("PATH_CANONICALIZATION_FAILED", "Canonicalizing " + WideToUtf8(a_path));
		}
		if (length >= buffer.size())
		{
			throw Error("PATH_TOO_LONG", "The canonical path exceeds the supported Windows path length.");
		}
		std::wstring                result(buffer.data(), length);
		constexpr std::wstring_view uncPrefix = LR"(\\?\UNC\)";
		constexpr std::wstring_view localPrefix = LR"(\\?\)";
		if (result.size() >= uncPrefix.size() &&
			CompareStringOrdinal(result.data(), static_cast<int>(uncPrefix.size()), uncPrefix.data(), static_cast<int>(uncPrefix.size()), TRUE) == CSTR_EQUAL)
		{
			result = L"\\\\" + result.substr(uncPrefix.size());
		}
		else if (result.size() >= localPrefix.size() &&
				 CompareStringOrdinal(result.data(), static_cast<int>(localPrefix.size()), localPrefix.data(), static_cast<int>(localPrefix.size()), TRUE) == CSTR_EQUAL)
		{
			result.erase(0, localPrefix.size());
		}
		return result;
	}

	[[nodiscard]] bool SamePath(std::wstring_view a_left, std::wstring_view a_right)
	{
		if (a_left.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
			a_right.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
		{
			return false;
		}
		return CompareStringOrdinal(
				   a_left.data(),
				   static_cast<int>(a_left.size()),
				   a_right.data(),
				   static_cast<int>(a_right.size()),
				   TRUE) == CSTR_EQUAL;
	}

	[[nodiscard]] std::wstring ParentPath(const std::wstring& a_path)
	{
		const std::size_t separator = a_path.find_last_of(L"\\/");
		if (separator == std::wstring::npos)
		{
			throw Error("INVALID_PATH", "The executable path has no parent directory.");
		}
		if (separator == 2 && a_path.size() >= 3 && a_path[1] == L':')
		{
			return a_path.substr(0, 3);
		}
		return a_path.substr(0, separator);
	}

	[[nodiscard]] std::wstring FileName(const std::wstring& a_path)
	{
		const std::size_t separator = a_path.find_last_of(L"\\/");
		return separator == std::wstring::npos ? a_path : a_path.substr(separator + 1);
	}

	[[nodiscard]] std::wstring UpperInvariant(std::wstring_view a_value)
	{
		if (a_value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
		{
			throw Error("TEXT_TOO_LONG", "The canonical game path is too long.");
		}
		const int sourceLength = static_cast<int>(a_value.size());
		const int length = LCMapStringEx(
			LOCALE_NAME_INVARIANT,
			LCMAP_UPPERCASE,
			a_value.data(),
			sourceLength,
			nullptr,
			0,
			nullptr,
			nullptr,
			0);
		if (length <= 0)
		{
			ThrowLastError("TEXT_ENCODING_FAILED", "Uppercasing the canonical game path");
		}
		std::wstring result(static_cast<std::size_t>(length), L'\0');
		if (LCMapStringEx(
				LOCALE_NAME_INVARIANT,
				LCMAP_UPPERCASE,
				a_value.data(),
				sourceLength,
				result.data(),
				length,
				nullptr,
				nullptr,
				0) != length)
		{
			ThrowLastError("TEXT_ENCODING_FAILED", "Uppercasing the canonical game path");
		}
		return result;
	}

	[[nodiscard]] std::string Sha256Hex(std::string_view a_value)
	{
		BCRYPT_ALG_HANDLE algorithm = nullptr;
		if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
		{
			throw Error("HASH_FAILED", "Opening the Windows SHA-256 provider failed.");
		}

		BCRYPT_HASH_HANDLE    hash = nullptr;
		std::vector<UCHAR>    object;
		std::array<UCHAR, 32> digest{};
		try
		{
			DWORD objectLength = 0;
			DWORD bytes = 0;
			if (BCryptGetProperty(
					algorithm,
					BCRYPT_OBJECT_LENGTH,
					reinterpret_cast<PUCHAR>(&objectLength),
					sizeof(objectLength),
					&bytes,
					0) < 0)
			{
				throw Error("HASH_FAILED", "Querying the Windows SHA-256 provider failed.");
			}
			object.resize(objectLength);
			if (BCryptCreateHash(
					algorithm,
					&hash,
					object.data(),
					static_cast<ULONG>(object.size()),
					nullptr,
					0,
					0) < 0)
			{
				throw Error("HASH_FAILED", "Creating the Windows SHA-256 hash failed.");
			}
			if (a_value.size() > static_cast<std::size_t>(std::numeric_limits<ULONG>::max()))
			{
				throw Error("HASH_FAILED", "The canonical game path is too long to hash.");
			}
			if (BCryptHashData(
					hash,
					reinterpret_cast<PUCHAR>(const_cast<char*>(a_value.data())),
					static_cast<ULONG>(a_value.size()),
					0) < 0 ||
				BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
			{
				throw Error("HASH_FAILED", "Computing the Windows SHA-256 hash failed.");
			}
		}
		catch (...)
		{
			if (hash != nullptr)
			{
				BCryptDestroyHash(hash);
			}
			BCryptCloseAlgorithmProvider(algorithm, 0);
			throw;
		}
		BCryptDestroyHash(hash);
		BCryptCloseAlgorithmProvider(algorithm, 0);

		constexpr char digits[] = "0123456789ABCDEF";
		std::string    result(digest.size() * 2, '\0');
		for (std::size_t index = 0; index < digest.size(); ++index)
		{
			result[index * 2] = digits[digest[index] >> 4];
			result[index * 2 + 1] = digits[digest[index] & 0x0F];
		}
		return result;
	}

	[[nodiscard]] std::wstring PipePath(const std::wstring& a_canonicalGame)
	{
		const std::string hash = Sha256Hex(WideToUtf8(UpperInvariant(a_canonicalGame)));
		return L"\\\\.\\pipe\\" + Utf8ToWide(std::string(kPipePrefix) + hash);
	}

	[[nodiscard]] std::uint64_t CurrentFileTime()
	{
		FILETIME value{};
		GetSystemTimeAsFileTime(&value);
		ULARGE_INTEGER combined{};
		combined.LowPart = value.dwLowDateTime;
		combined.HighPart = value.dwHighDateTime;
		return combined.QuadPart;
	}

	[[nodiscard]] std::uint64_t ProcessCreationFileTime(HANDLE a_process)
	{
		FILETIME creation{};
		FILETIME exit{};
		FILETIME kernel{};
		FILETIME user{};
		if (!GetProcessTimes(a_process, &creation, &exit, &kernel, &user))
		{
			ThrowLastError("PROCESS_QUERY_FAILED", "Querying process creation time");
		}
		ULARGE_INTEGER combined{};
		combined.LowPart = creation.dwLowDateTime;
		combined.HighPart = creation.dwHighDateTime;
		return combined.QuadPart;
	}

	[[nodiscard]] std::wstring ProcessImagePath(HANDLE a_process)
	{
		std::vector<wchar_t> buffer(32768);
		DWORD                length = static_cast<DWORD>(buffer.size());
		if (!QueryFullProcessImageNameW(a_process, 0, buffer.data(), &length))
		{
			ThrowLastError("PROCESS_QUERY_FAILED", "Querying process image path");
		}
		return CanonicalizeExistingFile(std::wstring(buffer.data(), length));
	}

	[[nodiscard]] bool IsRunning(HANDLE a_process)
	{
		const DWORD wait = WaitForSingleObject(a_process, 0);
		if (wait == WAIT_TIMEOUT)
		{
			return true;
		}
		if (wait == WAIT_OBJECT_0)
		{
			return false;
		}
		ThrowLastError("PROCESS_WAIT_FAILED", "Checking a process handle");
	}

	[[nodiscard]] Handle OpenProcessChecked(DWORD a_pid, DWORD a_access, std::string_view a_code)
	{
		Handle result(OpenProcess(a_access, FALSE, a_pid));
		if (!result)
		{
			ThrowLastError(std::string(a_code), "Opening process " + std::to_string(a_pid));
		}
		return result;
	}

	[[nodiscard]] std::vector<BYTE> ProcessUserSid(HANDLE a_process)
	{
		HANDLE rawToken = nullptr;
		if (!OpenProcessToken(a_process, TOKEN_QUERY, &rawToken))
		{
			ThrowLastError("SHIM_IDENTITY_MISMATCH", "Opening a process token");
		}
		Handle token(rawToken);
		DWORD  required = 0;
		GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &required);
		if (required == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		{
			ThrowLastError("SHIM_IDENTITY_MISMATCH", "Querying a process owner");
		}
		std::vector<BYTE> buffer(required);
		if (!GetTokenInformation(token.Get(), TokenUser, buffer.data(), required, &required))
		{
			ThrowLastError("SHIM_IDENTITY_MISMATCH", "Querying a process owner");
		}
		const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
		if (!IsValidSid(user->User.Sid))
		{
			throw Error("SHIM_IDENTITY_MISMATCH", "A process owner SID is invalid.");
		}
		const DWORD       sidLength = GetLengthSid(user->User.Sid);
		std::vector<BYTE> sid(sidLength);
		if (!CopySid(sidLength, sid.data(), user->User.Sid))
		{
			ThrowLastError("SHIM_IDENTITY_MISMATCH", "Copying a process owner SID");
		}
		return sid;
	}

	void VerifySameOwner(HANDLE a_broker)
	{
		const std::vector<BYTE> self = ProcessUserSid(GetCurrentProcess());
		const std::vector<BYTE> broker = ProcessUserSid(a_broker);
		if (!EqualSid(
				reinterpret_cast<PSID>(const_cast<BYTE*>(self.data())),
				reinterpret_cast<PSID>(const_cast<BYTE*>(broker.data()))))
		{
			throw Error("SHIM_IDENTITY_MISMATCH", "The launch broker is owned by another Windows user.");
		}
	}

	[[nodiscard]] DWORD RemainingTickTimeout(std::uint64_t a_deadline)
	{
		const std::uint64_t now = GetTickCount64();
		if (now >= a_deadline)
		{
			return 0;
		}
		return static_cast<DWORD>(std::min<std::uint64_t>(a_deadline - now, MAXDWORD - 1));
	}

	[[nodiscard]] DWORD RemainingFileTimeTimeout(std::uint64_t a_deadline)
	{
		const std::uint64_t now = CurrentFileTime();
		if (now >= a_deadline)
		{
			return 0;
		}
		const std::uint64_t remaining = (a_deadline - now + kFileTimeTicksPerMillisecond - 1) / kFileTimeTicksPerMillisecond;
		return static_cast<DWORD>(std::min<std::uint64_t>(remaining, MAXDWORD - 1));
	}

	void CancelAndDrain(HANDLE a_pipe, OVERLAPPED& a_overlapped)
	{
		if (!CancelIoEx(a_pipe, &a_overlapped) && GetLastError() != ERROR_NOT_FOUND)
		{
			ThrowLastError("SHIM_PROTOCOL_ERROR", "Cancelling a timed-out pipe operation");
		}
		DWORD transferred = 0;
		if (!GetOverlappedResult(a_pipe, &a_overlapped, &transferred, TRUE))
		{
			const DWORD error = GetLastError();
			if (error != ERROR_OPERATION_ABORTED && error != ERROR_BROKEN_PIPE && error != ERROR_PIPE_NOT_CONNECTED)
			{
				ThrowLastError("SHIM_PROTOCOL_ERROR", "Draining a cancelled pipe operation", error);
			}
		}
	}

	void TransferExact(HANDLE a_pipe, void* a_buffer, DWORD a_size, bool a_write, DWORD a_timeout)
	{
		if (a_timeout == 0)
		{
			throw Error("SHIM_TIMEOUT", "The launch pipe operation timed out.");
		}
		auto*               bytes = static_cast<BYTE*>(a_buffer);
		DWORD               offset = 0;
		const std::uint64_t deadline = GetTickCount64() + a_timeout;
		while (offset < a_size)
		{
			Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
			if (!event)
			{
				ThrowLastError("SHIM_PROTOCOL_ERROR", "Creating a pipe operation event");
			}
			OVERLAPPED operation{};
			operation.hEvent = event.Get();
			DWORD      transferred = 0;
			const BOOL started = a_write ? WriteFile(a_pipe, bytes + offset, a_size - offset, &transferred, &operation) : ReadFile(a_pipe, bytes + offset, a_size - offset, &transferred, &operation);
			if (!started)
			{
				const DWORD error = GetLastError();
				if (error != ERROR_IO_PENDING)
				{
					if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA)
					{
						throw Error("SHIM_DISCONNECTED", "The launch peer disconnected.");
					}
					ThrowLastError("SHIM_PROTOCOL_ERROR", a_write ? "Writing to the launch pipe" : "Reading from the launch pipe", error);
				}
				const DWORD remaining = RemainingTickTimeout(deadline);
				if (remaining == 0)
				{
					CancelAndDrain(a_pipe, operation);
					throw Error("SHIM_TIMEOUT", "The launch pipe operation timed out.");
				}
				const DWORD wait = WaitForSingleObject(event.Get(), remaining);
				if (wait == WAIT_TIMEOUT)
				{
					CancelAndDrain(a_pipe, operation);
					throw Error("SHIM_TIMEOUT", "The launch pipe operation timed out.");
				}
				if (wait != WAIT_OBJECT_0)
				{
					const DWORD waitError = GetLastError();
					CancelAndDrain(a_pipe, operation);
					ThrowLastError("SHIM_PROTOCOL_ERROR", "Waiting for a launch pipe operation", waitError);
				}
				if (!GetOverlappedResult(a_pipe, &operation, &transferred, FALSE))
				{
					const DWORD resultError = GetLastError();
					if (resultError == ERROR_BROKEN_PIPE || resultError == ERROR_PIPE_NOT_CONNECTED || resultError == ERROR_NO_DATA)
					{
						throw Error("SHIM_DISCONNECTED", "The launch peer disconnected.");
					}
					ThrowLastError("SHIM_PROTOCOL_ERROR", "Completing a launch pipe operation", resultError);
				}
			}
			if (transferred == 0)
			{
				throw Error("SHIM_DISCONNECTED", "The launch peer disconnected.");
			}
			offset += transferred;
		}
	}

	class PipeWire
	{
	public:
		explicit PipeWire(HANDLE a_pipe) : pipe_(a_pipe) {}

		[[nodiscard]] std::string Read(DWORD a_timeout)
		{
			const std::uint64_t deadline = GetTickCount64() + a_timeout;
			std::array<BYTE, 4> lengthBytes{};
			TransferExact(
				pipe_,
				lengthBytes.data(),
				static_cast<DWORD>(lengthBytes.size()),
				false,
				RemainingTickTimeout(deadline));
			const std::uint32_t length =
				static_cast<std::uint32_t>(lengthBytes[0]) |
				(static_cast<std::uint32_t>(lengthBytes[1]) << 8) |
				(static_cast<std::uint32_t>(lengthBytes[2]) << 16) |
				(static_cast<std::uint32_t>(lengthBytes[3]) << 24);
			if (length > kMaximumFieldSize)
			{
				throw Error("SHIM_PROTOCOL_ERROR", "The launch broker sent an oversized field.");
			}
			std::string value(length, '\0');
			if (length != 0)
			{
				TransferExact(pipe_, value.data(), length, false, RemainingTickTimeout(deadline));
			}
			return value;
		}

		void Write(std::string_view a_value, DWORD a_timeout)
		{
			const std::uint64_t deadline = GetTickCount64() + a_timeout;
			if (a_value.size() > kMaximumFieldSize)
			{
				throw Error("SHIM_PROTOCOL_ERROR", "A launch protocol field exceeds its bound.");
			}
			const std::uint32_t length = static_cast<std::uint32_t>(a_value.size());
			std::array<BYTE, 4> lengthBytes{
				static_cast<BYTE>(length & 0xFF),
				static_cast<BYTE>((length >> 8) & 0xFF),
				static_cast<BYTE>((length >> 16) & 0xFF),
				static_cast<BYTE>((length >> 24) & 0xFF)
			};
			TransferExact(
				pipe_,
				lengthBytes.data(),
				static_cast<DWORD>(lengthBytes.size()),
				true,
				RemainingTickTimeout(deadline));
			if (length != 0)
			{
				TransferExact(pipe_, const_cast<char*>(a_value.data()), length, true, RemainingTickTimeout(deadline));
			}
		}

	private:
		HANDLE pipe_;
	};

	[[nodiscard]] Handle ConnectPipe(const std::wstring& a_pipePath)
	{
		const std::uint64_t deadline = GetTickCount64() + kConnectTimeoutMilliseconds;
		while (true)
		{
			Handle pipe(CreateFileW(
				a_pipePath.c_str(),
				GENERIC_READ | GENERIC_WRITE,
				0,
				nullptr,
				OPEN_EXISTING,
				FILE_FLAG_OVERLAPPED,
				nullptr));
			if (pipe)
			{
				return pipe;
			}
			const DWORD error = GetLastError();
			if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY)
			{
				ThrowLastError("SHIM_CONNECT_FAILED", "Connecting to the launch broker", error);
			}
			const DWORD remaining = RemainingTickTimeout(deadline);
			if (remaining == 0)
			{
				throw Error("NO_PENDING_BROKER", "No pending launch broker accepted this game path.");
			}
			const DWORD wait = std::min<DWORD>(remaining, 100);
			if (!WaitNamedPipeW(a_pipePath.c_str(), wait))
			{
				const DWORD waitError = GetLastError();
				if (waitError != ERROR_SEM_TIMEOUT && waitError != ERROR_FILE_NOT_FOUND && waitError != ERROR_PIPE_BUSY)
				{
					ThrowLastError("SHIM_CONNECT_FAILED", "Waiting for the launch broker", waitError);
				}
				if (waitError == ERROR_FILE_NOT_FOUND)
				{
					Sleep(std::min<DWORD>(remaining, kPollMilliseconds));
				}
			}
		}
	}

	template <class Integer>
	[[nodiscard]] Integer ParseUnsignedDecimal(std::string_view a_value, std::string_view a_field)
	{
		if (a_value.empty())
		{
			throw Error("SHIM_PROTOCOL_ERROR", "The " + std::string(a_field) + " field is empty.");
		}
		Integer    result{};
		const auto parsed = std::from_chars(a_value.data(), a_value.data() + a_value.size(), result, 10);
		if (parsed.ec != std::errc{} || parsed.ptr != a_value.data() + a_value.size())
		{
			throw Error("SHIM_PROTOCOL_ERROR", "The " + std::string(a_field) + " field is invalid.");
		}
		return result;
	}

	[[nodiscard]] bool IsGuidNonce(std::string_view a_value)
	{
		return a_value.size() == 32 && std::all_of(a_value.begin(), a_value.end(), [](char a_character) {
			return (a_character >= '0' && a_character <= '9') ||
			       (a_character >= 'a' && a_character <= 'f') ||
			       (a_character >= 'A' && a_character <= 'F');
		});
	}

	[[nodiscard]] Handle OpenBroker(HANDLE a_pipe, DWORD& a_pid)
	{
		if (!GetNamedPipeServerProcessId(a_pipe, &a_pid))
		{
			ThrowLastError("SHIM_IDENTITY_MISMATCH", "Identifying the launch broker");
		}
		Handle broker = OpenProcessChecked(
			a_pid,
			PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
			"SHIM_IDENTITY_MISMATCH");
		if (!IsRunning(broker.Get()))
		{
			throw Error("SHIM_IDENTITY_MISMATCH", "The launch broker exited before authorization.");
		}
		VerifySameOwner(broker.Get());
		return broker;
	}

	[[nodiscard]] std::vector<ProcessIdentity> FindExactImage(const std::wstring& a_canonicalImage)
	{
		const std::wstring targetName = FileName(a_canonicalImage);
		Handle             snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
		if (!snapshot)
		{
			ThrowLastError("PROCESS_SNAPSHOT_FAILED", "Creating a process snapshot");
		}

		std::vector<ProcessIdentity> result;
		PROCESSENTRY32W              entry{};
		entry.dwSize = sizeof(entry);
		if (!Process32FirstW(snapshot.Get(), &entry))
		{
			const DWORD error = GetLastError();
			if (error == ERROR_NO_MORE_FILES)
			{
				return result;
			}
			ThrowLastError("PROCESS_SNAPSHOT_FAILED", "Reading a process snapshot", error);
		}
		do
		{
			if (SamePath(entry.szExeFile, targetName))
			{
				Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID));
				if (!process)
				{
					const DWORD error = GetLastError();
					if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_FOUND && error != ERROR_INVALID_HANDLE)
					{
						throw Error(
							"PROCESS_ACCESS_DENIED",
							"A process named " + WideToUtf8(targetName) +
								" could not be identified exactly. " + WindowsMessage(error) + ".");
					}
				}
				else if (IsRunning(process.Get()))
				{
					const std::wstring image = ProcessImagePath(process.Get());
					if (SamePath(image, a_canonicalImage))
					{
						result.push_back(ProcessIdentity{
							entry.th32ProcessID,
							entry.th32ParentProcessID,
							ProcessCreationFileTime(process.Get()),
							image });
					}
				}
			}
			entry.dwSize = sizeof(entry);
		} while (Process32NextW(snapshot.Get(), &entry));

		const DWORD error = GetLastError();
		if (error != ERROR_NO_MORE_FILES)
		{
			ThrowLastError("PROCESS_SNAPSHOT_FAILED", "Reading a process snapshot", error);
		}
		return result;
	}

	[[nodiscard]] OwnedGame RetainGame(const ProcessIdentity& a_identity)
	{
		Handle process = OpenProcessChecked(a_identity.pid, kProcessAccess, "OWNED_PROCESS_OPEN_FAILED");
		if (!IsRunning(process.Get()) ||
			ProcessCreationFileTime(process.Get()) != a_identity.creationFileTime ||
			!SamePath(ProcessImagePath(process.Get()), a_identity.imagePath))
		{
			throw Error("PROCESS_IDENTITY_CHANGED", "The game PID changed before ownership could be retained.");
		}
		return OwnedGame{ std::move(process), a_identity.pid, a_identity.creationFileTime };
	}

	[[nodiscard]] OwnedGame AcquireOwnedGame(
		const std::wstring& a_canonicalGame,
		DWORD               a_loaderPid,
		std::uint64_t       a_loaderCreation,
		std::uint64_t       a_deadline)
	{
		while (CurrentFileTime() < a_deadline)
		{
			const std::vector<ProcessIdentity> candidates = FindExactImage(a_canonicalGame);
			if (candidates.size() > 1)
			{
				throw Error("AMBIGUOUS_GAME_PROCESS", "Multiple matching game processes appeared.");
			}
			if (!candidates.empty())
			{
				const ProcessIdentity& candidate = candidates.front();
				if (candidate.parentPid != a_loaderPid || candidate.creationFileTime < a_loaderCreation)
				{
					throw Error(
						"OWNERSHIP_UNPROVEN",
						"A game appeared outside the retained F4SE loader's direct ancestry.");
				}
				return RetainGame(candidate);
			}
			Sleep(kPollMilliseconds);
		}
		throw Error("START_TIMEOUT", "F4SE did not create a provably owned game before the launch deadline.");
	}

	[[nodiscard]] OwnedGame TryRetainDirectGameForCleanup(
		const std::wstring& a_canonicalGame,
		DWORD               a_loaderPid,
		std::uint64_t       a_loaderCreation)
	{
		try
		{
			std::vector<ProcessIdentity> direct;
			for (const ProcessIdentity& candidate : FindExactImage(a_canonicalGame))
			{
				if (candidate.parentPid == a_loaderPid && candidate.creationFileTime >= a_loaderCreation)
				{
					direct.push_back(candidate);
				}
			}
			if (direct.size() == 1)
			{
				return RetainGame(direct.front());
			}
		}
		catch (const Error&)
		{}
		return {};
	}

	void WaitForProcess(HANDLE a_process)
	{
		const DWORD wait = WaitForSingleObject(a_process, INFINITE);
		if (wait != WAIT_OBJECT_0)
		{
			ThrowLastError("PROCESS_WAIT_FAILED", "Waiting for the owned process");
		}
	}

	[[nodiscard]] std::wstring QuoteCommandArgument(const std::wstring& a_argument)
	{
		std::wstring result = L"\"";
		std::size_t  backslashes = 0;
		for (const wchar_t character : a_argument)
		{
			if (character == L'\\')
			{
				++backslashes;
				continue;
			}
			if (character == L'"')
			{
				result.append(backslashes * 2 + 1, L'\\');
				result.push_back(L'"');
				backslashes = 0;
				continue;
			}
			result.append(backslashes, L'\\');
			backslashes = 0;
			result.push_back(character);
		}
		result.append(backslashes * 2, L'\\');
		result.push_back(L'"');
		return result;
	}

	[[nodiscard]] OwnedGame LaunchOwnedGame(
		const std::wstring& a_canonicalGame,
		const std::wstring& a_loaderOffer,
		std::uint64_t       a_deadline,
		LaunchGuard&        a_guard)
	{
		if (!FindExactImage(a_canonicalGame).empty())
		{
			throw Error("GAME_ALREADY_RUNNING", "A game appeared before the shim launched F4SE.");
		}

		const std::wstring canonicalLoader = CanonicalizeExistingFile(a_loaderOffer);
		const std::wstring gameParent = ParentPath(a_canonicalGame);
		if (!SamePath(ParentPath(canonicalLoader), gameParent))
		{
			throw Error("INVALID_LOADER_PATH", "The F4SE loader must be beside the configured game executable.");
		}
		if (CurrentFileTime() >= a_deadline)
		{
			throw Error("START_TIMEOUT", "The launch authorization expired before F4SE could start.");
		}

		std::wstring         commandLine = QuoteCommandArgument(canonicalLoader);
		std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
		mutableCommand.push_back(L'\0');
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION process{};
		if (!CreateProcessW(
				canonicalLoader.c_str(),
				mutableCommand.data(),
				nullptr,
				nullptr,
				FALSE,
				0,
				nullptr,
				gameParent.c_str(),
				&startup,
				&process))
		{
			ThrowLastError("LOADER_START_FAILED", "Starting the configured F4SE loader");
		}
		a_guard.loader.Reset(process.hProcess);
		Handle loaderThread(process.hThread);
		a_guard.loaderPid = process.dwProcessId;
		a_guard.loaderCreationFileTime = ProcessCreationFileTime(a_guard.loader.Get());
		return AcquireOwnedGame(
			a_canonicalGame,
			a_guard.loaderPid,
			a_guard.loaderCreationFileTime,
			a_deadline);
	}

	[[nodiscard]] std::string UpperHex(std::uintptr_t a_value)
	{
		std::array<char, sizeof(std::uintptr_t) * 2> buffer{};
		const auto                                   result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), a_value, 16);
		if (result.ec != std::errc{})
		{
			throw Error("SHIM_PROTOCOL_ERROR", "Formatting the owned game handle failed.");
		}
		std::string value(buffer.data(), result.ptr);
		std::transform(value.begin(), value.end(), value.begin(), [](char a_character) {
			return a_character >= 'a' && a_character <= 'f' ? static_cast<char>(a_character - ('a' - 'A')) : a_character;
		});
		return value;
	}

	[[nodiscard]] int Run(const std::wstring& a_gameArgument)
	{
		const std::wstring canonicalGame = CanonicalizeExistingFile(a_gameArgument);
		Handle             pipe = ConnectPipe(PipePath(canonicalGame));
		DWORD              brokerPid = 0;
		Handle             broker = OpenBroker(pipe.Get(), brokerPid);
		PipeWire           wire(pipe.Get());
		bool               launchAuthorized = false;
		OwnedGame          game;
		LaunchGuard        launchGuard;

		try
		{
			const std::uint64_t offerTickDeadline = GetTickCount64() + kOfferTimeoutMilliseconds;
			const auto          readOffer = [&]() {
				const DWORD timeout = RemainingTickTimeout(offerTickDeadline);
				if (timeout == 0)
				{
					throw Error("SHIM_TIMEOUT", "The launch broker did not send its offer in time.");
				}
				return wire.Read(timeout);
			};

			if (readOffer() != kProtocol)
			{
				throw Error("SHIM_PROTOCOL_ERROR", "The launch broker uses an unsupported protocol.");
			}
			const std::string   nonce = readOffer();
			const DWORD         advertisedPid = ParseUnsignedDecimal<DWORD>(readOffer(), "broker PID");
			const std::uint64_t advertisedCreation =
				ParseUnsignedDecimal<std::uint64_t>(readOffer(), "broker creation time");
			const std::wstring  expectedGame = Utf8ToWide(readOffer());
			const std::wstring  loader = Utf8ToWide(readOffer());
			const std::uint64_t deadline = ParseUnsignedDecimal<std::uint64_t>(readOffer(), "launch deadline");

			if (!IsGuidNonce(nonce) ||
				advertisedPid != brokerPid ||
				advertisedCreation != ProcessCreationFileTime(broker.Get()) ||
				!SamePath(expectedGame, canonicalGame) ||
				!IsRunning(broker.Get()) ||
				deadline <= CurrentFileTime())
			{
				throw Error("SHIM_IDENTITY_MISMATCH", "The launch offer is stale or targets another process.");
			}

			DWORD timeout = RemainingFileTimeTimeout(deadline);
			if (timeout == 0)
			{
				throw Error("START_TIMEOUT", "The launch authorization expired.");
			}
			wire.Write(nonce, timeout);
			timeout = RemainingFileTimeTimeout(deadline);
			if (timeout == 0 || wire.Read(timeout) != "launch" || deadline <= CurrentFileTime())
			{
				throw Error("SHIM_PROTOCOL_ERROR", "The launch was not authorized before its deadline.");
			}
			launchAuthorized = true;

			game = LaunchOwnedGame(canonicalGame, loader, deadline, launchGuard);
			timeout = RemainingFileTimeTimeout(deadline);
			if (timeout == 0)
			{
				throw Error("START_TIMEOUT", "The ownership handoff deadline expired.");
			}
			wire.Write("owned", timeout);
			wire.Write(nonce, RemainingFileTimeTimeout(deadline));
			wire.Write(std::to_string(game.pid), RemainingFileTimeTimeout(deadline));
			wire.Write(std::to_string(game.creationFileTime), RemainingFileTimeTimeout(deadline));
			wire.Write(
				UpperHex(reinterpret_cast<std::uintptr_t>(game.handle.Get())),
				RemainingFileTimeTimeout(deadline));
			if (wire.Read(RemainingFileTimeTimeout(deadline)) != "retained")
			{
				throw Error("SHIM_PROTOCOL_ERROR", "The launch broker did not retain the owned game handle.");
			}
		}
		catch (const Error& error)
		{
			if (launchAuthorized && !game.handle)
			{
				try
				{
					wire.Write("error", kOfferTimeoutMilliseconds);
					wire.Write(error.what(), kOfferTimeoutMilliseconds);
				}
				catch (const Error&)
				{}
			}
			if (game.handle)
			{
				try
				{
					WaitForProcess(game.handle.Get());
				}
				catch (const Error& waitError)
				{
					ReportFailure(waitError);
				}
			}
			else if (launchGuard.loader)
			{
				bool retainedCleanupGame = false;
				try
				{
					if (IsRunning(launchGuard.loader.Get()))
					{
						WaitForProcess(launchGuard.loader.Get());
					}
					OwnedGame cleanup = TryRetainDirectGameForCleanup(
						canonicalGame,
						launchGuard.loaderPid,
						launchGuard.loaderCreationFileTime);
					if (cleanup.handle)
					{
						retainedCleanupGame = true;
						WaitForProcess(cleanup.handle.Get());
					}
				}
				catch (const Error& waitError)
				{
					ReportFailure(waitError);
				}
				if (!retainedCleanupGame)
				{
					throw Error(
						error.Code(),
						std::string(error.what()) +
							" F4SE was started, but no remaining direct game process could be retained; verify MO2 state manually.");
				}
			}
			throw;
		}

		pipe.Reset();
		broker.Reset();
		WaitForProcess(game.handle.Get());
		return 0;
	}
}

int wmain(int a_argumentCount, wchar_t** a_arguments)
{
	try
	{
		if (a_argumentCount != 3 || std::wstring_view(a_arguments[1]) != kGameFlag || a_arguments[2][0] == L'\0')
		{
			throw Error(
				"INVALID_SHIM_CONFIG",
				"Usage: devbench-launch.exe -GameExe <absolute Fallout4.exe path>.");
		}
		return Run(a_arguments[2]);
	}
	catch (const Error& error)
	{
		ReportFailure(error);
		return 1;
	}
	catch (const std::exception& error)
	{
		const Error wrapped("SHIM_UNEXPECTED_ERROR", error.what());
		ReportFailure(wrapped);
		return 2;
	}
	catch (...)
	{
		const Error wrapped("SHIM_UNEXPECTED_ERROR", "An unknown native launch error occurred.");
		ReportFailure(wrapped);
		return 2;
	}
}
