#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dvb::tools
{
	class ExtensionDescriptorRefresh
	{
	public:
		using Callback = std::pair<std::string, std::function<void()>>;

		explicit ExtensionDescriptorRefresh(std::vector<Callback> a_callbacks);
		~ExtensionDescriptorRefresh();

		ExtensionDescriptorRefresh(const ExtensionDescriptorRefresh&) = delete;
		ExtensionDescriptorRefresh& operator=(const ExtensionDescriptorRefresh&) = delete;

	private:
		struct State;
		std::shared_ptr<State> state_;
	};
}
