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

#include <libaegisub/exception.h>
#include <libaegisub/fs.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct sqlite3;

namespace agi {

DEFINE_EXCEPTION(GlossaryError, Exception);

/// A single glossary entry: a term/phrase plus a note (text and url).
struct GlossaryEntry {
	int64_t id = 0;
	std::string term;
	std::string note_text;
	std::string note_url;
};

/// An occurrence of a glossary term inside a line of text.
struct GlossaryMatch {
	size_t offset;    ///< Byte offset of the match in the original UTF-8 line
	size_t length;    ///< Byte length of the match in the original line
	int64_t entry_id; ///< Entry the match came from
};

struct GlossaryMatcher;

/// User-managed glossary/dictionary backed by a SQLite database.
///
/// The database holds any number of named dictionaries, each with a set of
/// entries. One dictionary can be made "active", building a case-insensitive
/// (Aho-Corasick) matcher used by Match() to locate terms in subtitle text.
class Glossary {
	sqlite3 *db = nullptr;
	int64_t active_dict = -1;
	std::unique_ptr<GlossaryMatcher> matcher;

	void Exec(const char *sql);

public:
	/// Open (creating if needed) the glossary database at db_path.
	explicit Glossary(agi::fs::path const& db_path);
	~Glossary();

	Glossary(Glossary const&) = delete;
	Glossary& operator=(Glossary const&) = delete;

	// --- Dictionary management ---
	/// All dictionaries as (id, name), ordered by name.
	std::vector<std::pair<int64_t, std::string>> ListDictionaries();
	/// Create a dictionary and return its id (throws if the name exists).
	int64_t CreateDictionary(std::string_view name);
	/// Look up a dictionary id by name, or -1 if not found.
	int64_t GetDictionaryId(std::string_view name);
	void RenameDictionary(int64_t id, std::string_view name);
	void DeleteDictionary(int64_t id);

	// --- Entry management ---
	/// Entries of a dictionary.
	std::vector<GlossaryEntry> ListEntries(int64_t dict_id);
	/// Full entry for an id.
	GlossaryEntry GetEntry(int64_t entry_id);
	/// Insert (id==0) or update (id!=0) an entry; returns the entry id.
	int64_t UpsertEntry(int64_t dict_id, GlossaryEntry const& entry);
	void DeleteEntry(int64_t entry_id);

	// --- Matching ---
	/// Build the matcher for a dictionary (-1 disables matching).
	void SetActiveDictionary(int64_t dict_id);
	int64_t GetActiveDictionary() const { return active_dict; }
	/// Find all active-dictionary terms occurring in a UTF-8 line.
	std::vector<GlossaryMatch> Match(std::string_view line_utf8);
};

}
