#include "ExtensionDescriptorRefresh.h"

#include "ToolExtensions.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace dvb::tools
{
	namespace
	{
		std::string NormalizeBase(std::string a_base)
		{
			std::transform(a_base.begin(), a_base.end(), a_base.begin(),
				[](unsigned char a_character) { return static_cast<char>(std::tolower(a_character)); });
			return a_base;
		}
	}

	struct ExtensionDescriptorRefresh::State
	{
		std::mutex                                             mutex;
		bool                                                   accepting = true;
		std::unordered_map<std::string, std::function<void()>> callbacks;
	};

	ExtensionDescriptorRefresh::ExtensionDescriptorRefresh(std::vector<Callback> a_callbacks) :
		state_(std::make_shared<State>())
	{
		for (auto& [base, callback] : a_callbacks)
		{
			if (base.empty() || !callback)
				throw std::invalid_argument("extension descriptor refresh requires a base and callback");
			if (!state_->callbacks.emplace(NormalizeBase(std::move(base)), std::move(callback)).second)
				throw std::invalid_argument("duplicate extension descriptor refresh base");
		}
		ToolExtensions::SetChangeListener([weak = std::weak_ptr(state_)](const std::string& a_base) {
			if (const auto state = weak.lock())
			{
				const std::lock_guard lock(state->mutex);
				if (!state->accepting)
					return;
				if (const auto found = state->callbacks.find(NormalizeBase(a_base)); found != state->callbacks.end())
					found->second();
			}
		});
	}

	ExtensionDescriptorRefresh::~ExtensionDescriptorRefresh()
	{
		ToolExtensions::SetChangeListener({});
		// The mirror copies listeners before delivery; drain an in-flight refresh before its captures die.
		const std::lock_guard lock(state_->mutex);
		state_->accepting = false;
		state_->callbacks.clear();
	}
}
