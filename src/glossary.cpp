// Copyright (c) 2026, Aegisub Project http://www.aegisub.org/
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "glossary.h"

#include "dialogs.h"
#include "options.h"

#include <libaegisub/glossary.h>
#include <libaegisub/log.h>
#include <libaegisub/path.h>

Glossary::Glossary() {
	try {
		impl = std::make_unique<agi::Glossary>(config::path->Decode("?user/glossary.db"));
		Refresh();
	}
	catch (agi::Exception const& e) {
		LOG_E("glossary") << e.GetMessage();
	}
}

Glossary::~Glossary() = default;

void Glossary::Refresh() {
	if (!impl) return;

	if (!OPT_GET("Tool/Glossary/Enabled")->GetBool()) {
		impl->SetActiveDictionary(-1);
		return;
	}

	auto name = OPT_GET("Tool/Glossary/Active Dictionary")->GetString();
	int64_t id = name.empty() ? -1 : impl->GetDictionaryId(name);
	try {
		impl->SetActiveDictionary(id);
	}
	catch (agi::Exception const& e) {
		LOG_E("glossary") << e.GetMessage();
	}
}

std::vector<agi::GlossaryMatch> Glossary::Match(std::string_view line_utf8) {
	if (!impl) return {};
	try {
		return impl->Match(line_utf8);
	}
	catch (agi::Exception const& e) {
		LOG_E("glossary") << e.GetMessage();
		return {};
	}
}

agi::GlossaryEntry Glossary::GetEntry(int64_t entry_id) {
	if (!impl) return {};
	try {
		return impl->GetEntry(entry_id);
	}
	catch (agi::Exception const& e) {
		LOG_E("glossary") << e.GetMessage();
		return {};
	}
}

bool Glossary::EditEntry(wxWindow *parent, int64_t entry_id) {
	if (!impl) return false;
	return EditGlossaryEntry(parent, *impl, entry_id);
}

bool Glossary::AddEntry(wxWindow *parent, std::string const& term) {
	if (!impl) return false;
	return AddGlossaryEntry(parent, *impl, term);
}

std::vector<std::string> GlossaryDictionaryNames() {
	std::vector<std::string> out;
	try {
		agi::Glossary g(config::path->Decode("?user/glossary.db"));
		for (auto const& dict : g.ListDictionaries())
			out.push_back(dict.second);
	}
	catch (agi::Exception const& e) {
		LOG_E("glossary") << e.GetMessage();
	}
	return out;
}

std::string ActiveGlossaryDictionary() {
	if (!OPT_GET("Tool/Glossary/Enabled")->GetBool())
		return {};
	return OPT_GET("Tool/Glossary/Active Dictionary")->GetString();
}

void ActivateGlossaryDictionary(std::string const& name) {
	OPT_SET("Tool/Glossary/Active Dictionary")->SetString(name);
	OPT_SET("Tool/Glossary/Enabled")->SetBool(true);
}

void DeactivateGlossaryDictionary() {
	OPT_SET("Tool/Glossary/Active Dictionary")->SetString("");
	OPT_SET("Tool/Glossary/Enabled")->SetBool(false);
}
