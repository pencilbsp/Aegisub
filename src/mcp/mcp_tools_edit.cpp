// Copyright (c) 2026, Aegisub Project http://www.aegisub.org/
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#include "mcp_tools_edit.h"

#include "mcp_util.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "search_replace_engine.h"

#include <libaegisub/color.h>
#include <libaegisub/format.h>
#include <libaegisub/of_type_adaptor.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/regex/icu.hpp>

#include <algorithm>
#include <charconv>
#include <limits>
#include <optional>
#include <unordered_set>

using namespace agi::mcp;

namespace {

using DialogueField = boost::flyweight<std::string> AssDialogueBase::*;

struct SearchSpec {
	SearchReplaceSettings settings;
	std::optional<std::unordered_set<int64_t>> rows;
};

DialogueField FieldPointer(SearchReplaceSettings::Field field) {
	switch (field) {
		case SearchReplaceSettings::Field::TEXT: return &AssDialogueBase::Text;
		case SearchReplaceSettings::Field::STYLE: return &AssDialogueBase::Style;
		case SearchReplaceSettings::Field::ACTOR: return &AssDialogueBase::Actor;
		case SearchReplaceSettings::Field::EFFECT: return &AssDialogueBase::Effect;
		case SearchReplaceSettings::Field::ORIGINAL: return &AssDialogueBase::Original;
	}
	throw ToolError("Invalid search field");
}

SearchReplaceSettings::Field ParseField(std::string const& field) {
	if (field == "text") return SearchReplaceSettings::Field::TEXT;
	if (field == "original") return SearchReplaceSettings::Field::ORIGINAL;
	if (field == "style") return SearchReplaceSettings::Field::STYLE;
	if (field == "actor") return SearchReplaceSettings::Field::ACTOR;
	if (field == "effect") return SearchReplaceSettings::Field::EFFECT;
	throw ToolError("field must be one of: text, original, style, actor, effect");
}

int64_t ElementInt(json::UnknownElement const& value) {
	try {
		return static_cast<json::Integer const&>(value);
	}
	catch (json::Exception const&) {
		return static_cast<int64_t>(static_cast<json::Double const&>(value));
	}
}

std::optional<std::unordered_set<int64_t>> ParseRows(json::Object const& args) {
	auto it = args.find("rows");
	if (it == args.end()) return std::nullopt;
	json::Array const& values = it->second;
	if (values.empty()) throw ToolError("rows must not be empty when provided");
	std::unordered_set<int64_t> rows;
	for (auto const& value : values)
		rows.insert(ElementInt(value));
	return rows;
}

SearchSpec ParseSearch(json::Object const& args, bool replace) {
	SearchSpec spec;
	spec.settings.find = mcp::ArgString(args, "find");
	if (spec.settings.find.empty()) throw ToolError("find must not be empty");
	if (replace) spec.settings.replace_with = mcp::ArgString(args, "replace");
	spec.settings.field = ParseField(mcp::ArgString(args, "field", "text"));
	spec.settings.limit_to = SearchReplaceSettings::Limit::ALL;
	spec.settings.match_case = mcp::ArgBool(args, "match_case", false);
	spec.settings.use_regex = mcp::ArgBool(args, "regex", false);
	spec.settings.ignore_comments = mcp::ArgBool(args, "ignore_comments", false);
	spec.settings.skip_tags = mcp::ArgBool(args, "skip_tags", false);
	spec.settings.exact_match = mcp::ArgBool(args, "exact_match", false);
	spec.rows = ParseRows(args);
	return spec;
}

void ValidateRows(std::optional<std::unordered_set<int64_t>> const& rows, size_t line_count) {
	if (!rows) return;
	for (auto row : *rows) {
		if (row < 0 || row >= static_cast<int64_t>(line_count))
			throw ToolError(agi::format("Row %d is out of range (file has %d lines)", row, line_count));
	}
}

auto MakeMatcher(SearchReplaceSettings const& settings) {
	try {
		return SearchReplaceEngine::GetMatcher(settings);
	}
	catch (boost::regex_error const& e) {
		throw ToolError(std::string("Invalid regular expression: ") + e.what());
	}
}

std::string Replacement(std::string const& value, MatchState const& match, SearchReplaceSettings const& settings) {
	if (!match.re) return settings.replace_with;
	auto matched = value.substr(match.start, match.end - match.start);
	return boost::u32regex_replace(matched, *match.re, settings.replace_with, boost::format_first_only);
}

ToolResult FindLines(json::Object const& args) {
	int window_id = mcp::ArgInt(args, "window_id");
	auto spec = ParseSearch(args, false);
	int64_t max_results = mcp::ArgInt(args, "max_results", 100);
	if (max_results < 1 || max_results > 10000)
		throw ToolError("max_results must be between 1 and 10000");

	json::Array found;
	bool truncated = false;
	mcp::WithWindow(window_id, [&](FrameMain *, agi::Context *c) {
		auto index = mcp::EventIndex(c);
		ValidateRows(spec.rows, index.size());
		auto matcher = MakeMatcher(spec.settings);
		auto field = FieldPointer(spec.settings.field);
		for (size_t row = 0; row < index.size() && !truncated; ++row) {
			auto line = index[row];
			if (spec.rows && !spec.rows->count(row)) continue;
			if (spec.settings.ignore_comments && line->Comment) continue;
			size_t pos = 0;
			while (pos <= (line->*field).get().size()) {
				auto match = matcher(line, pos);
				if (!match) break;
				if (found.size() >= static_cast<size_t>(max_results)) {
					truncated = true;
					break;
				}
				auto const& value = (line->*field).get();
				json::Object item;
				item.emplace("row", static_cast<int64_t>(row));
				item.emplace("start", static_cast<int64_t>(match.start));
				item.emplace("end", static_cast<int64_t>(match.end));
				item.emplace("matched", value.substr(match.start, match.end - match.start));
				item.emplace("value", value);
				found.emplace_back(std::move(item));
				pos = match.end;
				if (pos == match.start) ++pos;
			}
		}
	});
	json::Object result;
	result.emplace("matches", std::move(found));
	result.emplace("truncated", truncated);
	return mcp::JsonResult(std::move(result));
}

ToolResult ReplaceLines(json::Object const& args) {
	int window_id = mcp::ArgInt(args, "window_id");
	auto spec = ParseSearch(args, true);
	bool fill_original = mcp::ArgBool(args, "fill_original", true);
	int64_t replacement_count = 0;
	std::unordered_set<int64_t> changed_rows;
	mcp::WithWindow(window_id, [&](FrameMain *, agi::Context *c) {
		auto index = mcp::EventIndex(c);
		ValidateRows(spec.rows, index.size());
		auto matcher = MakeMatcher(spec.settings);
		auto field = FieldPointer(spec.settings.field);
		int commit_type = spec.settings.field == SearchReplaceSettings::Field::TEXT
			? AssFile::COMMIT_DIAG_TEXT : AssFile::COMMIT_DIAG_META;
		for (size_t row = 0; row < index.size(); ++row) {
			auto line = index[row];
			if (spec.rows && !spec.rows->count(row)) continue;
			if (spec.settings.ignore_comments && line->Comment) continue;
			bool preserved = false;
			size_t pos = 0;
			while (pos <= (line->*field).get().size()) {
				auto match = matcher(line, pos);
				if (!match) break;
				auto value = (line->*field).get();
				if (!preserved && fill_original && spec.settings.field == SearchReplaceSettings::Field::TEXT && line->Original.get().empty()) {
					line->Original = value;
					commit_type |= AssFile::COMMIT_DIAG_META;
					preserved = true;
				}
				auto replacement = Replacement(value, match, spec.settings);
				(line->*field) = value.substr(0, match.start) + replacement + value.substr(match.end);
				++replacement_count;
				changed_rows.insert(row);
				pos = match.start + replacement.size();
				if (match.start == match.end && replacement.empty()) ++pos;
			}
		}
		if (replacement_count)
			c->ass->Commit(to_wx("MCP: replace"), commit_type, -1,
				changed_rows.size() == 1 ? index[*changed_rows.begin()] : nullptr);
	});
	json::Object result;
	result.emplace("replacements", replacement_count);
	result.emplace("changed_rows", static_cast<int64_t>(changed_rows.size()));
	return mcp::JsonResult(std::move(result));
}

ToolResult ShiftTimes(json::Object const& args) {
	int window_id = mcp::ArgInt(args, "window_id");
	int64_t delta = mcp::ArgInt(args, "delta_ms");
	if (!delta) throw ToolError("delta_ms must not be zero");
	auto fields = mcp::ArgString(args, "fields", "both");
	bool shift_start = fields == "both" || fields == "start";
	bool shift_end = fields == "both" || fields == "end";
	if (!shift_start && !shift_end)
		throw ToolError("fields must be one of: both, start, end");
	auto rows = ParseRows(args);
	int64_t shifted = 0;
	mcp::WithWindow(window_id, [&](FrameMain *, agi::Context *c) {
		auto index = mcp::EventIndex(c);
		ValidateRows(rows, index.size());
		for (size_t row = 0; row < index.size(); ++row) {
			if (rows && !rows->count(row)) continue;
			auto line = index[row];
			if (shift_start) line->Start = static_cast<int>(std::clamp<int64_t>(static_cast<int>(line->Start) + delta, 0, 35999994));
			if (shift_end) line->End = static_cast<int>(std::clamp<int64_t>(static_cast<int>(line->End) + delta, 0, 35999994));
			++shifted;
		}
		if (shifted)
			c->ass->Commit(to_wx("MCP: shift times"), AssFile::COMMIT_DIAG_TIME, -1,
				shifted == 1 ? index[rows ? *rows->begin() : 0] : nullptr);
	});
	json::Object result;
	result.emplace("shifted_rows", shifted);
	result.emplace("delta_ms", delta);
	return mcp::JsonResult(std::move(result));
}

json::Object StyleJson(AssStyle const& style) {
	json::Object o;
	o.emplace("name", style.name);
	o.emplace("font", style.font);
	o.emplace("fontsize", style.fontsize);
	o.emplace("primary", style.primary.GetHexFormatted(true));
	o.emplace("secondary", style.secondary.GetHexFormatted(true));
	o.emplace("outline", style.outline.GetHexFormatted(true));
	o.emplace("shadow", style.shadow.GetHexFormatted(true));
	o.emplace("bold", style.bold);
	o.emplace("italic", style.italic);
	o.emplace("underline", style.underline);
	o.emplace("strikeout", style.strikeout);
	o.emplace("scale_x", style.scalex);
	o.emplace("scale_y", style.scaley);
	o.emplace("spacing", style.spacing);
	o.emplace("angle", style.angle);
	o.emplace("border_style", style.borderstyle);
	o.emplace("outline_width", style.outline_w);
	o.emplace("shadow_width", style.shadow_w);
	o.emplace("alignment", style.alignment);
	o.emplace("margin_left", style.Margin[0]);
	o.emplace("margin_right", style.Margin[1]);
	o.emplace("margin_vertical", style.Margin[2]);
	o.emplace("encoding", style.encoding);
	return o;
}

struct StyleReferenceEditor {
	std::string old_name;
	std::string new_name;
	bool replace = false;
	bool found = false;
	static void Process(std::string const& tag, AssOverrideParameter *param, void *opaque) {
		auto self = static_cast<StyleReferenceEditor *>(opaque);
		if (tag == "\\r" && param->GetType() == VariableDataType::TEXT && param->Get<std::string>() == self->old_name) {
			self->found = true;
			if (self->replace) param->Set(self->new_name);
		}
	}
};

bool EditStyleReferences(agi::Context *c, std::string const& old_name, std::optional<std::string> replacement) {
	StyleReferenceEditor editor{old_name, replacement.value_or(""), replacement.has_value(), false};
	for (auto& line : c->ass->Events) {
		if (line.Style == old_name) {
			editor.found = true;
			if (replacement) line.Style = *replacement;
		}
		auto blocks = line.ParseTags();
		for (auto block : blocks | agi::of_type<AssDialogueBlockOverride>())
			block->ProcessParameters(&StyleReferenceEditor::Process, &editor);
		if (replacement) line.UpdateText(blocks);
	}
	return editor.found;
}

void ApplyStylePatch(AssStyle& style, json::Object const& args) {
	auto color = [&](const char *name) {
		auto value = mcp::ArgString(args, name);
		if (value.size() == 9 && value[0] == '#') {
			unsigned int rgba = 0;
			auto parsed = std::from_chars(value.data() + 1, value.data() + value.size(), rgba, 16);
			if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size())
				throw ToolError(std::string("Invalid color for ") + name + "; expected #RRGGBBAA or an ASS color");
			return agi::Color(rgba >> 24, rgba >> 16, rgba >> 8, rgba);
		}
		return agi::Color(value);
	};
	if (mcp::HasArg(args, "font")) style.font = mcp::ArgString(args, "font");
	if (mcp::HasArg(args, "fontsize")) style.fontsize = mcp::ArgDouble(args, "fontsize");
	if (mcp::HasArg(args, "primary")) style.primary = color("primary");
	if (mcp::HasArg(args, "secondary")) style.secondary = color("secondary");
	if (mcp::HasArg(args, "outline")) style.outline = color("outline");
	if (mcp::HasArg(args, "shadow")) style.shadow = color("shadow");
	if (mcp::HasArg(args, "bold")) style.bold = mcp::ArgBool(args, "bold", false);
	if (mcp::HasArg(args, "italic")) style.italic = mcp::ArgBool(args, "italic", false);
	if (mcp::HasArg(args, "underline")) style.underline = mcp::ArgBool(args, "underline", false);
	if (mcp::HasArg(args, "strikeout")) style.strikeout = mcp::ArgBool(args, "strikeout", false);
	if (mcp::HasArg(args, "scale_x")) style.scalex = mcp::ArgDouble(args, "scale_x");
	if (mcp::HasArg(args, "scale_y")) style.scaley = mcp::ArgDouble(args, "scale_y");
	if (mcp::HasArg(args, "spacing")) style.spacing = mcp::ArgDouble(args, "spacing");
	if (mcp::HasArg(args, "angle")) style.angle = mcp::ArgDouble(args, "angle");
	if (mcp::HasArg(args, "border_style")) style.borderstyle = mcp::ArgInt(args, "border_style");
	if (mcp::HasArg(args, "outline_width")) style.outline_w = mcp::ArgDouble(args, "outline_width");
	if (mcp::HasArg(args, "shadow_width")) style.shadow_w = mcp::ArgDouble(args, "shadow_width");
	if (mcp::HasArg(args, "alignment")) style.alignment = mcp::ArgInt(args, "alignment");
	if (mcp::HasArg(args, "margin_left")) style.Margin[0] = mcp::ArgInt(args, "margin_left");
	if (mcp::HasArg(args, "margin_right")) style.Margin[1] = mcp::ArgInt(args, "margin_right");
	if (mcp::HasArg(args, "margin_vertical")) style.Margin[2] = mcp::ArgInt(args, "margin_vertical");
	if (mcp::HasArg(args, "encoding")) style.encoding = mcp::ArgInt(args, "encoding");
	if (style.font.empty() || style.fontsize <= 0) throw ToolError("font must not be empty and fontsize must be positive");
	if (style.alignment < 1 || style.alignment > 9) throw ToolError("alignment must be between 1 and 9");
	if (style.borderstyle != 1 && style.borderstyle != 3 && style.borderstyle != 4) throw ToolError("border_style must be 1, 3, or 4");
	if (style.scalex < 0 || style.scaley < 0 || style.outline_w < 0 || style.shadow_w < 0) throw ToolError("style scale and border widths must be non-negative");
}

ToolResult ListStyles(json::Object const& args) {
	int window_id = mcp::ArgInt(args, "window_id");
	json::Array styles;
	mcp::WithWindow(window_id, [&](FrameMain *, agi::Context *c) {
		for (auto const& style : c->ass->Styles) styles.emplace_back(StyleJson(style));
	});
	json::Object result;
	result.emplace("styles", std::move(styles));
	return mcp::JsonResult(std::move(result));
}

ToolResult UpsertStyle(json::Object const& args) {
	int window_id = mcp::ArgInt(args, "window_id");
	auto name = mcp::ArgString(args, "name");
	if (name.empty()) throw ToolError("name must not be empty");
	auto new_name = mcp::ArgString(args, "new_name", name);
	if (new_name.empty()) throw ToolError("new_name must not be empty");
	bool created = false;
	json::Object result;
	mcp::WithWindow(window_id, [&](FrameMain *, agi::Context *c) {
		auto style = c->ass->GetStyle(name);
		if (!style) {
			style = new AssStyle;
			style->name = name;
			created = true;
		}
		auto collision = c->ass->GetStyle(new_name);
		if (collision && collision != style) {
			if (created) delete style;
			throw ToolError("A style with the requested new_name already exists");
		}
		try {
			ApplyStylePatch(*style, args);
		}
		catch (...) {
			if (created) delete style;
			throw;
		}
		bool renamed = style->name != new_name;
		if (renamed) EditStyleReferences(c, style->name, new_name);
		style->name = new_name;
		style->UpdateData();
		if (created) c->ass->Styles.push_back(*style);
		c->ass->Commit(to_wx("MCP: upsert style"), AssFile::COMMIT_STYLES | (renamed ? AssFile::COMMIT_DIAG_FULL : 0));
		result.emplace("style", StyleJson(*style));
	});
	result.emplace("created", created);
	return mcp::JsonResult(std::move(result));
}

ToolResult DeleteStyle(json::Object const& args) {
	int window_id = mcp::ArgInt(args, "window_id");
	auto name = mcp::ArgString(args, "name");
	auto replacement = mcp::HasArg(args, "replacement")
		? std::optional<std::string>(mcp::ArgString(args, "replacement")) : std::nullopt;
	bool references_changed = false;
	mcp::WithWindow(window_id, [&](FrameMain *, agi::Context *c) {
		auto style = c->ass->GetStyle(name);
		if (!style) throw ToolError("Style not found: " + name);
		if (c->ass->Styles.size() == 1) throw ToolError("Cannot delete the last style in a script");
		if (replacement) {
			auto target = c->ass->GetStyle(*replacement);
			if (!target || target == style) throw ToolError("replacement must name another existing style");
			references_changed = EditStyleReferences(c, style->name, *replacement);
		}
		else if (EditStyleReferences(c, style->name, std::nullopt))
			throw ToolError("Style is in use; provide replacement to update its references before deletion");
		delete style;
		c->ass->Commit(to_wx("MCP: delete style"), AssFile::COMMIT_STYLES | (references_changed ? AssFile::COMMIT_DIAG_FULL : 0));
	});
	json::Object result;
	result.emplace("deleted", name);
	result.emplace("references_changed", references_changed);
	return mcp::JsonResult(std::move(result));
}

}

namespace mcp {

void RegisterEditTools(agi::mcp::Dispatcher& d) {
	constexpr auto search_properties = R"json("window_id":{"type":"integer","description":"Window id from list_windows"},"find":{"type":"string"},"field":{"type":"string","enum":["text","original","style","actor","effect"],"default":"text"},"rows":{"type":"array","items":{"type":"integer"},"description":"Optional rows to limit the operation"},"match_case":{"type":"boolean","default":false},"regex":{"type":"boolean","default":false},"ignore_comments":{"type":"boolean","default":false},"skip_tags":{"type":"boolean","default":false},"exact_match":{"type":"boolean","default":false})json";
	d.RegisterTool({"find_lines", "Find text or metadata in subtitle lines without changing the current grid selection. Supports regex, case sensitivity, exact matching, ignoring comments, ASS-tag skipping and row limits.", std::string("{\"type\":\"object\",\"properties\":{") + search_properties + R"json(,"max_results":{"type":"integer","default":100,"minimum":1,"maximum":10000}},"required":["window_id","find"]})json", FindLines, false});
	d.RegisterTool({"replace_lines", "Replace all matching occurrences in subtitle text or metadata as one undo step. By default replacing text preserves each line's old text in its empty original field.", std::string("{\"type\":\"object\",\"properties\":{") + search_properties + R"json(,"replace":{"type":"string"},"fill_original":{"type":"boolean","default":true}},"required":["window_id","find","replace"]})json", ReplaceLines, true});
	d.RegisterTool({"shift_times", "Shift start times, end times, or both by a signed millisecond delta for all lines or explicit rows. The operation is one undo step and clamps times to Aegisub's valid range.", R"json({"type":"object","properties":{"window_id":{"type":"integer"},"delta_ms":{"type":"integer","description":"Signed shift; positive is later"},"fields":{"type":"string","enum":["both","start","end"],"default":"both"},"rows":{"type":"array","items":{"type":"integer"},"description":"Optional rows; omit for all lines"}},"required":["window_id","delta_ms"]})json", ShiftTimes, true});
	d.RegisterTool({"list_styles", "List every script style with all editable ASS style properties.", R"json({"type":"object","properties":{"window_id":{"type":"integer"}},"required":["window_id"]})json", ListStyles, false});
	d.RegisterTool({"upsert_style", "Create or patch a script style as one undo step. Existing properties omitted from the call are preserved. Set new_name to rename and update line and override-tag references.", R"json({"type":"object","properties":{"window_id":{"type":"integer"},"name":{"type":"string","description":"Existing style to edit, or new style name"},"new_name":{"type":"string"},"font":{"type":"string"},"fontsize":{"type":"number"},"primary":{"type":"string","description":"#RRGGBBAA (ASS alpha: 00 opaque, FF transparent) or ASS color"},"secondary":{"type":"string"},"outline":{"type":"string"},"shadow":{"type":"string"},"bold":{"type":"boolean"},"italic":{"type":"boolean"},"underline":{"type":"boolean"},"strikeout":{"type":"boolean"},"scale_x":{"type":"number"},"scale_y":{"type":"number"},"spacing":{"type":"number"},"angle":{"type":"number"},"border_style":{"type":"integer","enum":[1,3,4]},"outline_width":{"type":"number"},"shadow_width":{"type":"number"},"alignment":{"type":"integer","minimum":1,"maximum":9},"margin_left":{"type":"integer"},"margin_right":{"type":"integer"},"margin_vertical":{"type":"integer"},"encoding":{"type":"integer"}},"required":["window_id","name"]})json", UpsertStyle, true});
	d.RegisterTool({"delete_style", "Delete a script style as one undo step. Deleting an in-use style requires replacement, which updates base-style and \\rStyle override references. The last style cannot be deleted.", R"json({"type":"object","properties":{"window_id":{"type":"integer"},"name":{"type":"string"},"replacement":{"type":"string","description":"Existing style to replace references with"}},"required":["window_id","name"]})json", DeleteStyle, true});
}

}
