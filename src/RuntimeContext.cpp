#include "RuntimeContext.h"

#include "io/WindowsPaths.h"

#include <Windows.h>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace dvb
{
	namespace
	{
		std::mutex                      g_mutex;
		std::shared_ptr<RuntimeContext> g_context;

		std::filesystem::path ModulePath(HMODULE a_module)
		{
			std::vector<wchar_t> buffer(512);
			while (buffer.size() <= 32768)
			{
				const auto length = ::GetModuleFileNameW(
					a_module, buffer.data(), static_cast<DWORD>(buffer.size()));
				if (length == 0)
					throw std::system_error(
						static_cast<int>(::GetLastError()), std::system_category(),
						"GetModuleFileNameW");
				if (length < buffer.size())
					return io::PhysicalFilePath(
						std::wstring(buffer.data(), length));
				buffer.resize(buffer.size() * 2);
			}
			throw std::runtime_error("module path exceeds the Windows path limit");
		}
	}

	RuntimeContext MakeRuntimeContext(
		GameProfile a_profile, std::string a_runtimeVersion,
		const void* a_pluginAddress, std::function<int()> a_currentFrame,
		std::function<bool(std::function<void()>)> a_enqueueTask)
	{
		if (!a_pluginAddress)
			throw std::invalid_argument("plugin module address is required");
		HMODULE module = nullptr;
		if (!::GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(a_pluginAddress), &module))
			throw std::system_error(
				static_cast<int>(::GetLastError()), std::system_category(),
				"GetModuleHandleExW");
		return {
			.profile = std::move(a_profile),
			.runtimeVersion = std::move(a_runtimeVersion),
			.executablePath = ModulePath(nullptr),
			.pluginPath = ModulePath(module),
			.currentFrame = std::move(a_currentFrame),
			.enqueueTask = std::move(a_enqueueTask),
		};
	}

	void InitializeRuntimeContext(RuntimeContext a_context)
	{
		if (a_context.profile.id.empty() || a_context.profile.displayName.empty())
			throw std::invalid_argument("runtime context requires a game profile");
		if (!a_context.currentFrame)
			throw std::invalid_argument("runtime context requires a frame provider");
		if (!a_context.enqueueTask)
			throw std::invalid_argument("runtime context requires a task enqueue provider");
		const std::lock_guard lock(g_mutex);
		g_context = std::make_shared<RuntimeContext>(std::move(a_context));
	}

	void ResetRuntimeContext()
	{
		const std::lock_guard lock(g_mutex);
		g_context.reset();
	}

	const RuntimeContext& GetRuntimeContext()
	{
		const std::lock_guard lock(g_mutex);
		if (!g_context)
			throw std::logic_error("runtime context is not initialized");
		return *g_context;
	}

	const GameProfile& CurrentGameProfile()
	{
		return GetRuntimeContext().profile;
	}
}
