#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>

namespace dvb::tools
{
	// Thread-safe rolling text capped at a_capacity bytes; the oldest lines go first.
	class BoundedTextLog
	{
	public:
		explicit BoundedTextLog(std::size_t a_capacity) :
			m_capacity(a_capacity)
		{}

		void Append(std::string_view a_text)
		{
			const std::lock_guard lock{ m_mutex };
			m_text.append(a_text);
			if (m_text.size() <= m_capacity)
				return;
			// Trim to 3/4 so the front is erased once per capacity/4 bytes, not per append.
			auto cut = m_text.size() - m_capacity * 3 / 4;
			if (const auto newline = m_text.find('\n', cut - 1); newline != std::string::npos)
				cut = newline + 1;
			m_text.erase(0, cut);
		}

		std::string Snapshot() const
		{
			const std::lock_guard lock{ m_mutex };
			return m_text;
		}

	private:
		mutable std::mutex m_mutex;
		std::string        m_text;
		std::size_t        m_capacity;
	};
}
