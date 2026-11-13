#include "NoticeAnnotator.h"

#include "Notices.h"
#include "ToolRegistry.h"

#include <format>

namespace dvb::Notices
{
	namespace
	{
		ToolRegistry::Annotation Annotate()
		{
			ToolRegistry::Annotation out;
			out.fields = json::object();
			const auto notices = Global().Active(NowMs());
			if (notices.empty())
				return out;
			out.fields["notices"] = ToJson(notices);
			for (const auto& notice : notices)
				out.summary += std::format(
					"{} notice from {}: {}; ", SeverityName(notice.severity), notice.source, notice.text);
			out.summary.resize(out.summary.size() - 2);
			return out;
		}
	}

	void AttachToReplies(ToolRegistry& a_registry)
	{
		a_registry.SetAnnotator(Annotate);
	}
}
