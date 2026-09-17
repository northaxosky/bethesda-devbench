#pragma once

#include <spdlog/spdlog.h>

#include <format>
#include <utility>

namespace logs
{
	template <class... Args>
	void Write(spdlog::level::level_enum a_level, std::format_string<Args...> a_format, Args&&... a_args)
	{
		auto* logger = spdlog::default_logger_raw();
		if (!logger->should_log(a_level))
			return;
		const auto message = std::format(a_format, std::forward<Args>(a_args)...);
		logger->log(a_level, spdlog::string_view_t(message.data(), message.size()));
	}

	template <class... Args>
	void trace(std::format_string<Args...> a_format, Args&&... a_args)
	{
		Write(spdlog::level::trace, a_format, std::forward<Args>(a_args)...);
	}

	template <class... Args>
	void debug(std::format_string<Args...> a_format, Args&&... a_args)
	{
		Write(spdlog::level::debug, a_format, std::forward<Args>(a_args)...);
	}

	template <class... Args>
	void info(std::format_string<Args...> a_format, Args&&... a_args)
	{
		Write(spdlog::level::info, a_format, std::forward<Args>(a_args)...);
	}

	template <class... Args>
	void warn(std::format_string<Args...> a_format, Args&&... a_args)
	{
		Write(spdlog::level::warn, a_format, std::forward<Args>(a_args)...);
	}

	template <class... Args>
	void error(std::format_string<Args...> a_format, Args&&... a_args)
	{
		Write(spdlog::level::err, a_format, std::forward<Args>(a_args)...);
	}

	template <class... Args>
	void critical(std::format_string<Args...> a_format, Args&&... a_args)
	{
		Write(spdlog::level::critical, a_format, std::forward<Args>(a_args)...);
	}
}
