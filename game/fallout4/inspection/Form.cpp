#include "Form.h"

#include <charconv>
#include <limits>

namespace dvb::fallout4::inspection
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
				value > std::numeric_limits<RE::TESFormID>::max())
				return nullptr;
			return RE::TESForm::GetFormByID(static_cast<RE::TESFormID>(value));
		}
	}

	RE::TESForm* ResolveForm(std::string_view a_identifier)
	{
		if (a_identifier.empty())
			return nullptr;

		if (a_identifier.starts_with("0x") || a_identifier.starts_with("0X"))
			return ResolveHex(a_identifier.substr(2));

		if (auto* form = RE::TESForm::GetFormByEditorID(RE::BSFixedString(a_identifier)))
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
			{ "formType", nullptr },
			{ "name", nullptr },
			{ "editorId", nullptr },
		};

		if (const auto type = a_form->GetFormTypeString(); type && HasVisibleText(type))
			out["formType"] = std::string(type);

		const auto name = RE::TESFullName::GetFullName(*a_form);
		if (HasVisibleText(name))
			out["name"] = std::string(name);

		// FO4 does not expose a dependable CELL editor ID through the runtime form
		// surface. Keep it explicitly null instead of publishing an inferred value.
		if (!a_form->Is(RE::ENUM_FORM_ID::kCELL))
		{
			if (const auto editorId = a_form->GetFormEditorID(); editorId && HasVisibleText(editorId))
				out["editorId"] = std::string(editorId);
		}
		return out;
	}
}
