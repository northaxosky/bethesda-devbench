#pragma once

#include "GameProfile.h"

#include <filesystem>
#include <functional>
#include <string>

namespace dvb
{
	struct RuntimeContext
	{
		GameProfile                       profile;
		std::string                       runtimeVersion;
		std::filesystem::path             executablePath;
		std::filesystem::path             pluginPath;
		std::function<int()>              currentFrame;
		std::function<bool(std::function<void()>)> enqueueTask;
	};

	RuntimeContext MakeRuntimeContext(
		GameProfile a_profile, std::string a_runtimeVersion,
		const void* a_pluginAddress, std::function<int()> a_currentFrame,
		std::function<bool(std::function<void()>)> a_enqueueTask);

	void                  InitializeRuntimeContext(RuntimeContext a_context);
	void                  ResetRuntimeContext();
	const RuntimeContext& GetRuntimeContext();
	const GameProfile&    CurrentGameProfile();
}
