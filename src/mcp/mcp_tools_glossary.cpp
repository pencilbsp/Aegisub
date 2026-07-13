// Copyright (c) 2026, Aegisub Project http://www.aegisub.org/
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#include "mcp_tools_glossary.h"

#include "mcp_util.h"
#include "options.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/format.h>
#include <libaegisub/glossary.h>
#include <libaegisub/path.h>

using namespace agi::mcp;

namespace {

agi::fs::path GlossaryPath(int window_id) {
	agi::fs::path path;
	// Validating the window keeps the API consistent with other project
	// tools, while path decoding stays on the GUI thread with the options.
	mcp::WithWindow(window_id, [&](FrameMain *, agi::Context *) {
		path = config::path->Decode("?user/glossary.db");
	});
	return path;
}

json::Object EntryJson(agi::GlossaryEntry const& entry) {
	json::Object o;
	o.emplace("id", entry.id);
	o.emplace("term", entry.term);
	o.emplace("note_text", entry.note_text);
	o.emplace("note_url", entry.note_url);
	return o;
}

ToolResult GlossaryList(json::Object const& args) {
	int window_id = mcp::ArgInt(args, "window_id");
	agi::Glossary glossary(GlossaryPath(window_id));

	json::Array dictionaries;
	for (auto const& [id, name] : glossary.ListDictionaries()) {
		json::Object dictionary;
		dictionary.emplace("id", id);
		dictionary.emplace("name", name);
		dictionaries.emplace_back(std::move(dictionary));
	}

	json::Object result;
	result.emplace("dictionaries", std::move(dictionaries));
	if (mcp::HasArg(args, "dictionary")) {
		auto name = mcp::ArgString(args, "dictionary");
		auto id = glossary.GetDictionaryId(name);
		if (id < 0)
			throw ToolError("Glossary dictionary not found: " + name);
		json::Array entries;
		for (auto const& entry : glossary.ListEntries(id))
			entries.emplace_back(EntryJson(entry));
		result.emplace("dictionary", name);
		result.emplace("entries", std::move(entries));
	}
	return mcp::JsonResult(std::move(result));
}

ToolResult GlossaryUpsert(json::Object const& args) {
	int window_id = mcp::ArgInt(args, "window_id");
	auto dictionary = mcp::ArgString(args, "dictionary");
	agi::Glossary glossary(GlossaryPath(window_id));
	auto dictionary_id = glossary.GetDictionaryId(dictionary);
	if (dictionary_id < 0)
		throw ToolError("Glossary dictionary not found: " + dictionary);

	agi::GlossaryEntry entry;
	entry.id = mcp::ArgInt(args, "entry_id", 0);
	entry.term = mcp::ArgString(args, "term");
	entry.note_text = mcp::ArgString(args, "note_text", "");
	entry.note_url = mcp::ArgString(args, "note_url", "");
	if (entry.term.empty())
		throw ToolError("Glossary term must not be empty");

	entry.id = glossary.UpsertEntry(dictionary_id, entry);
	json::Object result;
	result.emplace("dictionary", dictionary);
	result.emplace("entry", EntryJson(entry));
	return mcp::JsonResult(std::move(result));
}

}

namespace mcp {

void RegisterGlossaryTools(agi::mcp::Dispatcher& d) {
	d.RegisterTool({
		"glossary_list",
		"List the user's glossary dictionaries. Pass a dictionary name to also return all of its terms and notes, which can guide consistent subtitle translation.",
		R"json({"type":"object","properties":{"window_id":{"type":"integer","description":"Window id from list_windows"},"dictionary":{"type":"string","description":"Optional dictionary name whose entries should be returned"}},"required":["window_id"]})json",
		GlossaryList,
		false
	});
	d.RegisterTool({
		"glossary_upsert",
		"Insert or update an entry in an existing Aegisub glossary dictionary. Omit entry_id to insert; provide it to update. Read-only mode disables this tool.",
		R"json({"type":"object","properties":{"window_id":{"type":"integer","description":"Window id from list_windows"},"dictionary":{"type":"string","description":"Existing glossary dictionary name"},"entry_id":{"type":"integer","description":"Existing entry id to update; omit to insert"},"term":{"type":"string","description":"Source term or phrase"},"note_text":{"type":"string","description":"Translation or explanatory note"},"note_url":{"type":"string","description":"Optional reference URL"}},"required":["window_id","dictionary","term"]})json",
		GlossaryUpsert,
		true
	});
}

}
