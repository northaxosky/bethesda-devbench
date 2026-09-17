#include "InspectTool.h"

#include "ToolExtensions.h"

#include <algorithm>
#include <array>
#include <memory>

namespace dvb::tools
{
	namespace
	{
		constexpr std::array<std::string_view, 12> kBuiltInKinds{
			"health", "state", "vm", "player", "scene", "mods", "inventory", "quests",
			"effects", "refs", "registrants", "extensions"
		};

		json InvokeBuiltIn(const std::function<json()>& a_handler, std::string_view a_kind)
		{
			if (!a_handler)
				throw ToolError(503, std::format("inspect kind '{}' is unavailable", a_kind));
			return a_handler();
		}

		json InvokeBuiltIn(
			const std::function<json(const inspection::Request&)>& a_handler,
			const inspection::Request&                             a_request)
		{
			if (!a_handler)
				throw ToolError(503, std::format("inspect kind '{}' is unavailable", a_request.kind));
			return a_handler(a_request);
		}

		json HandleInspect(const json& a_args, const ToolContext& a_context, const InspectBackend& a_backend)
		{
			const auto kind = inspection::ReadKind(a_args);
			if (kind == "health")
				return InvokeBuiltIn(a_backend.health, kind);
			if (kind == "state")
				return InvokeBuiltIn(a_backend.state, kind);
			if (kind == "vm")
				return InvokeBuiltIn(a_backend.vm, inspection::ParseRequest(a_args));
			if (kind == "player")
				return InvokeBuiltIn(a_backend.player, kind);
			if (kind == "scene")
				return InvokeBuiltIn(a_backend.scene, kind);
			if (kind == "mods")
				return InvokeBuiltIn(a_backend.mods, kind);
			if (kind == "inventory")
				return InvokeBuiltIn(a_backend.inventory, inspection::ParseRequest(a_args));
			if (kind == "quests")
				return InvokeBuiltIn(a_backend.quests, inspection::ParseRequest(a_args));
			if (kind == "effects")
				return InvokeBuiltIn(a_backend.effects, inspection::ParseRequest(a_args));
			if (kind == "refs")
				return InvokeBuiltIn(a_backend.refs, inspection::ParseRequest(a_args));
			if (kind == "registrants")
				return InvokeBuiltIn(a_backend.registrants, inspection::ParseRequest(a_args));
			if (kind == "extensions")
			{
				json extensions = json::array();
				for (const auto& key : ToolExtensions::Keys("inspect"))
				{
					json item{ { "kind", key } };
					if (const auto entry = ToolExtensions::Find("inspect", key))
						item["descriptor"] = entry->descriptor;
					extensions.push_back(std::move(item));
				}
				return json{ { "extensions", std::move(extensions) } };
			}
			if (const auto extension = ToolExtensions::Find("inspect", kind))
				return extension->handler(a_args, a_context);

			throw ToolError(400,
				std::format(
					"unsupported inspect kind '{}' (supported: health|state|vm|player|scene|mods|inventory|quests|effects|refs|registrants|extensions, or a registered extension)",
					kind));
		}
	}

	PlayerReadiness AssessPlayerReadiness(const PlayerReadinessInput& a_input)
	{
		if (!a_input.gameDataReady)
			return { false, "gameDataNotReady" };
		if (a_input.inMainMenu)
			return { false, "mainMenu" };
		if (a_input.inLoadingMenu)
			return { false, "loadingMenu" };
		if (a_input.loadInProgress)
			return { false, "loadInProgress" };
		if (a_input.loadFailed)
			return { false, "loadFailed" };
		if (!a_input.hasPlayer)
			return { false, "playerUnavailable" };
		if (!a_input.hasNpc)
			return { false, "playerBaseUnavailable" };
		if (!a_input.has3D)
			return { false, "player3DUnavailable" };
		return { true, {} };
	}

	ToolDescriptor BuildInspectDescriptor(std::span<const std::string_view> a_declaredExtensions)
	{
		json kinds = json::array();
		for (const auto kind : kBuiltInKinds)
			kinds.push_back(kind);
		for (const auto& kind : ToolExtensions::Keys("inspect"))
			kinds.push_back(kind);
		for (const auto kind : a_declaredExtensions)
		{
			if (std::none_of(kinds.begin(), kinds.end(), [kind](const json& a_value) {
					return a_value.get_ref<const std::string&>() == kind;
				}))
				kinds.push_back(kind);
		}

		ToolDescriptor descriptor;
		descriptor.name = "inspect";
		descriptor.readOnly = true;
		descriptor.description =
			"Read game/devbench state. health is answered off the game thread and reports "
			"identity plus queued main-thread task health. state reports lifecycle, menu, and real "
			"player readiness. vm reports lock-protected Papyrus collection sizes and state. "
			"player reports identity, equipment, and Fallout 4 current/base/permanent actor values. "
			"scene reports cell/worldspace/location/current scene, position, time, and weather. "
			"mods reports the final full/light load order after gameDataReady. inventory reads a "
			"player or container inventory; quests reads Fallout 4 instanced journal objectives; "
			"effects reads an actor's active-effect list; refs resolves, selects, or enumerates loaded "
			"forms; registrants reports the existing HostApi consumer/registration ledger. extensions lists "
			"consumer-registered inspect kinds; registered kinds dispatch through ToolExtensions.";
		descriptor.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
								{ "kind", json{ { "type", "string" }, { "enum", std::move(kinds) }, { "default", "state" } } },
								{ "formId", json{ { "type", "string" }, { "description", "inventory owner, effects actor, or refs form: 0x-prefixed/bare hexadecimal FormID or EditorID" } } },
								{ "selected", json{ { "type", "boolean" }, { "description", "refs only: use the console's current picked reference handle" } } },
								{ "formType", json{ { "type", "string" }, { "description", "inventory/refs: case-insensitive record type code or friendly alias" } } },
								{ "radius", json{ { "type", "number" }, { "minimum", 0 }, { "description", "refs enumeration radius from the player; 0 scans the loaded grid" } } },
								{ "limit", json{ { "type", "integer" }, { "minimum", 0 }, { "maximum", inspection::kMaxListLimit }, { "default", inspection::kDefaultListLimit } } },
							} },
		};
		return descriptor;
	}

	void RegisterInspectTool(
		ToolRegistry&  a_registry,
		InspectBackend a_backend,
		bool           a_refreshDescriptorOnExtension)
	{
		auto backend = std::make_shared<InspectBackend>(std::move(a_backend));
		auto handler = [backend](const json& a_args, const ToolContext& a_context) {
			return HandleInspect(a_args, a_context, *backend);
		};
		a_registry.Register(BuildInspectDescriptor(), handler);
		if (a_refreshDescriptorOnExtension)
		{
			ToolExtensions::SetChangeListener([registry = &a_registry, handler](const std::string& a_baseTool) {
				if (inspection::LowerAscii(a_baseTool) == "inspect")
					registry->Register(BuildInspectDescriptor(), handler);
			});
		}
	}
}
