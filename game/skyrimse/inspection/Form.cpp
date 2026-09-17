#include "Form.h"

#include <charconv>
#include <limits>

namespace dvb::skyrimse::inspection
{
	namespace
	{
		bool HasVisibleText(std::string_view a_text)
		{
			return a_text.find_first_not_of(" \t\r\n\f\v") != std::string_view::npos;
		}

		RE::TESForm* ResolveHex(std::string_view a_text)
		{
			if (a_text.empty())
				return nullptr;

			std::uint64_t value = 0;
			const auto [end, error] =
				std::from_chars(a_text.data(), a_text.data() + a_text.size(), value, 16);
			if (error != std::errc{} || end != a_text.data() + a_text.size() ||
				value > std::numeric_limits<RE::FormID>::max())
				return nullptr;
			return RE::TESForm::LookupByID(static_cast<RE::FormID>(value));
		}
	}

	RE::TESForm* ResolveForm(std::string_view a_identifier)
	{
		if (a_identifier.empty())
			return nullptr;
		if (a_identifier.starts_with("0x") || a_identifier.starts_with("0X"))
			return ResolveHex(a_identifier.substr(2));
		if (auto* form = RE::TESForm::LookupByEditorID(a_identifier))
			return form;
		return ResolveHex(a_identifier);
	}

	json SerializeForm(const RE::TESForm* a_form)
	{
		if (!a_form)
			return nullptr;

		const auto formId = a_form->GetFormID();
		json       out{
			{ "formId", formId },
			{ "formIdHex", std::format("0x{:08X}", formId) },
			{ "formType", std::string(RE::FormTypeToString(a_form->GetFormType())) },
			{ "name", nullptr },
			{ "editorId", nullptr },
		};
		if (const auto name = a_form->GetName(); name && HasVisibleText(name))
			out["name"] = std::string(name);
		if (const auto editorId = a_form->GetFormEditorID();
			editorId && HasVisibleText(editorId))
			out["editorId"] = std::string(editorId);
		return out;
	}
}
