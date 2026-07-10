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

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class wxWindow;

namespace agi {
class Glossary;
struct GlossaryEntry;
struct GlossaryMatch;
}

/// @class Glossary
/// @brief A wrapper around agi::Glossary that reads the active dictionary from
///        Aegisub's options and shares one database across the app.
class Glossary {
	std::unique_ptr<agi::Glossary> impl;

public:
	Glossary();
	~Glossary();

	/// Re-read the active-dictionary/enabled options and rebuild the matcher.
	/// Cheap enough to call whenever the relevant options change.
	void Refresh();

	/// Find all active-dictionary terms in a UTF-8 line (empty if disabled).
	std::vector<agi::GlossaryMatch> Match(std::string_view line_utf8);

	/// Full entry for a match's entry id.
	agi::GlossaryEntry GetEntry(int64_t entry_id);

	/// Edit an entry in a modal dialog, saving it to the active dictionary and
	/// rebuilding the matcher on OK. Returns whether the entry was changed.
	bool EditEntry(wxWindow *parent, int64_t entry_id);

	/// Add a new entry to the active dictionary through the modal editor.
	bool AddEntry(wxWindow *parent, std::string const& term);
};

/// Names of all glossary dictionaries (sorted), read from the shared database.
std::vector<std::string> GlossaryDictionaryNames();
/// Name of the active dictionary, or empty if none / glossary disabled.
std::string ActiveGlossaryDictionary();
/// Make `name` the active dictionary and enable glossary matching.
void ActivateGlossaryDictionary(std::string const& name);
/// Clear the active dictionary and disable glossary matching.
void DeactivateGlossaryDictionary();
