#include "ModalWatch.h"

#include "ModalLogic.h"

namespace dvb::Modal
{
	EventBus::SubId Watch(EventBus& a_events, ModalSource a_source)
	{
		return a_events.Subscribe(
			[&a_events, source = std::move(a_source)](const EventBus::Event& a_event) {
				if (a_event.topic != "menu")
					return;
				const auto name = a_event.payload.value("name", std::string{});
				if (name.empty() || IsNonBlocking(source.nonBlockingMenus, name))
					return;
				if (a_event.payload.value("opening", false))
				{
					a_events.Publish(
						"modal.blocked", json{ { "name", name }, { "hint", HintFor(name) } });
					return;
				}
				if (Blockers(source.nonBlockingMenus, source.openMenus()).empty())
					a_events.Publish("modal.cleared", json{ { "name", name } });
			});
	}
}
