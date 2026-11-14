#include "NoticeAnnotator.h"

#include "ModalLogic.h"
#include "Notices.h"
#include "ToolRegistry.h"

#include <format>

namespace dvb::Notices
{
	namespace
	{
		ToolRegistry::Annotation Annotate(const Modal::ModalSource& a_modal)
		{
			ToolRegistry::Annotation out;
			out.fields = json::object();

			if (a_modal.openMenus)
			{
				const auto blockers =
					Modal::Blockers(a_modal.nonBlockingMenus, a_modal.openMenus());
				if (!blockers.empty())
				{
					json list = json::array();
					for (const auto& blocker : blockers)
					{
						list.push_back(json{ { "name", blocker.name }, { "hint", blocker.hint } });
						out.summary += std::format(
							"blocked by {}: {}; ", blocker.name, blocker.hint);
					}
					out.fields["blockedBy"] = std::move(list);
				}
			}

			const auto notices = Global().Active(NowMs());
			if (!notices.empty())
			{
				out.fields["notices"] = ToJson(notices);
				for (const auto& notice : notices)
					out.summary += std::format(
						"{} notice from {}: {}; ", SeverityName(notice.severity), notice.source, notice.text);
			}
			if (!out.summary.empty())
				out.summary.resize(out.summary.size() - 2);
			return out;
		}
	}

	void AttachToReplies(ToolRegistry& a_registry, Modal::ModalSource a_modal)
	{
		a_registry.SetAnnotator(
			[modal = std::move(a_modal)] { return Annotate(modal); });
	}
}
