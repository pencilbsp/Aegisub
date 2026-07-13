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

#include "mcp_tools.h"

#include "mcp_util.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_info.h"
#include "ass_style.h"
#include "async_video_provider.h"
#include "compat.h"
#include "frame_main.h"
#include "include/aegisub/context.h"
#include "main.h"
#include "project.h"
#include "selection_controller.h"
#include "subs_controller.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/dispatch.h>
#include <libaegisub/format.h>
#include <libaegisub/fs.h>
#include <libaegisub/vfr.h>

#include <boost/algorithm/string/case_conv.hpp>
#include <optional>
#include <unordered_map>

using namespace agi::mcp;
using namespace mcp;

namespace {

ToolResult ListWindows(json::Object const&) {
	json::Array windows;
	agi::dispatch::Main().Sync([&] {
		for (auto frame : wxGetApp().GetFrames()) {
			auto c = frame->GetContext();
			int64_t line_count = 0;
			for (auto& line : c->ass->Events) {
				(void)line;
				++line_count;
			}
			json::Object w;
			w.emplace("window_id", frame->window_id);
			w.emplace("title", from_wx(frame->GetTitle()));
			w.emplace("subtitle_path", c->subsController->HasFilename() ? c->subsController->Filename().string() : "");
			w.emplace("is_modified", c->subsController->IsModified());
			w.emplace("video_path", c->project->VideoName().string());
			w.emplace("audio_path", c->project->AudioName().string());
			w.emplace("line_count", line_count);
			w.emplace("is_focused", frame->IsActive());
			windows.emplace_back(std::move(w));
		}
	});
	json::Object result;
	result.emplace("windows", std::move(windows));
	return JsonResult(std::move(result));
}

ToolResult GetProjectInfo(json::Object const& args) {
	int window_id = ArgInt(args, "window_id");
	json::Object result;
	WithWindow(window_id, [&](FrameMain *, agi::Context *c) {
		json::Object script_info;
		for (auto const& info : c->ass->Info)
			script_info.emplace(std::string(info.Key()), std::string(info.Value()));
		result.emplace("script_info", std::move(script_info));
		result.emplace("play_res_x", c->ass->GetScriptInfoAsInt("PlayResX"));
		result.emplace("play_res_y", c->ass->GetScriptInfoAsInt("PlayResY"));

		json::Array styles;
		for (auto const& style : c->ass->Styles) {
			json::Object s;
			s.emplace("name", style.name);
			s.emplace("font", style.font);
			s.emplace("fontsize", style.fontsize);
			s.emplace("bold", style.bold);
			s.emplace("italic", style.italic);
			s.emplace("alignment", style.alignment);
			styles.emplace_back(std::move(s));
		}
		result.emplace("styles", std::move(styles));

		auto index = EventIndex(c);
		json::Object subtitle;
		subtitle.emplace("path", c->subsController->HasFilename() ? c->subsController->Filename().string() : "");
		subtitle.emplace("is_modified", c->subsController->IsModified());
		subtitle.emplace("line_count", static_cast<int64_t>(index.size()));
		result.emplace("subtitle", std::move(subtitle));

		if (auto provider = c->project->VideoProvider()) {
			json::Object video;
			video.emplace("path", c->project->VideoName().string());
			video.emplace("width", provider->GetWidth());
			video.emplace("height", provider->GetHeight());
			video.emplace("frame_count", provider->GetFrameCount());
			video.emplace("fps", c->project->Timecodes().FPS());
			result.emplace("video", std::move(video));
		}
		else
			result.emplace("video", json::Null());

		if (auto provider = c->project->AudioProvider()) {
			json::Object audio;
			audio.emplace("path", c->project->AudioName().string());
			audio.emplace("sample_rate", provider->GetSampleRate());
			audio.emplace("channels", provider->GetChannels());
			audio.emplace("duration_ms", provider->GetNumSamples() * 1000 / provider->GetSampleRate());
			result.emplace("audio", std::move(audio));
		}
		else
			result.emplace("audio", json::Null());
	});
	return JsonResult(std::move(result));
}

ToolResult GetLines(json::Object const& args) {
	int window_id = ArgInt(args, "window_id");
	int64_t start_row = std::max<int64_t>(0, ArgInt(args, "start_row", 0));
	int64_t count = ArgInt(args, "count", -1);
	bool include_stripped = ArgBool(args, "include_stripped", false);

	json::Array lines;
	WithWindow(window_id, [&](FrameMain *, agi::Context *c) {
		int64_t row = -1;
		for (auto const& line : c->ass->Events) {
			++row;
			if (row < start_row) continue;
			if (count >= 0 && row >= start_row + count) break;

			json::Object l;
			l.emplace("row", row);
			l.emplace("id", line.Id);
			l.emplace("start_ms", static_cast<int>(line.Start));
			l.emplace("end_ms", static_cast<int>(line.End));
			l.emplace("style", line.Style.get());
			l.emplace("actor", line.Actor.get());
			l.emplace("effect", line.Effect.get());
			l.emplace("comment", line.Comment);
			l.emplace("text", line.Text.get());
			l.emplace("original", line.Original.get());
			if (include_stripped)
				l.emplace("stripped_text", line.GetStrippedText());
			lines.emplace_back(std::move(l));
		}
	});
	json::Object result;
	result.emplace("lines", std::move(lines));
	return JsonResult(std::move(result));
}

ToolResult SetLines(json::Object const& args) {
	int window_id = ArgInt(args, "window_id");
	bool fill_original = ArgBool(args, "fill_original", true);
	std::string commit_message = ArgString(args, "commit_message", "");

	struct Update {
		int64_t row;
		std::optional<std::string> text;
		std::optional<std::string> original;
	};
	std::vector<Update> updates;

	auto items_it = args.find("items");
	if (items_it == args.end())
		throw ToolError("Missing required argument: items");
	json::Array const& items = items_it->second;
	if (items.empty())
		throw ToolError("items must not be empty");

	for (auto const& item_el : items) {
		json::Object const& item = item_el;
		Update u;
		u.row = ArgInt(item, "row");
		if (HasArg(item, "text")) u.text = ArgString(item, "text");
		if (HasArg(item, "original")) u.original = ArgString(item, "original");
		if (!u.text && !u.original)
			throw ToolError(agi::format("Item for row %d must set text and/or original", u.row));
		updates.push_back(std::move(u));
	}

	if (commit_message.empty())
		commit_message = agi::format("MCP: update %d line%s", updates.size(), updates.size() == 1 ? "" : "s");

	WithWindow(window_id, [&](FrameMain *, agi::Context *c) {
		auto index = EventIndex(c);

		// Validate everything before mutating anything, so a bad row can't
		// leave a half-applied batch
		for (auto const& u : updates) {
			if (u.row < 0 || u.row >= static_cast<int64_t>(index.size()))
				throw ToolError(agi::format("Row %d is out of range (file has %d lines)", u.row, index.size()));
		}

		int commit_type = 0;
		for (auto const& u : updates) {
			auto line = index[u.row];
			if (u.text) {
				if (fill_original && !u.original && line->Original.get().empty() && line->Text.get() != *u.text) {
					line->Original = line->Text.get();
					commit_type |= AssFile::COMMIT_DIAG_META;
				}
				line->Text = *u.text;
				commit_type |= AssFile::COMMIT_DIAG_TEXT;
			}
			if (u.original) {
				line->Original = *u.original;
				commit_type |= AssFile::COMMIT_DIAG_META;
			}
		}

		c->ass->Commit(to_wx(commit_message), commit_type, -1,
			updates.size() == 1 ? index[updates[0].row] : nullptr);
	});

	json::Object result;
	result.emplace("updated", static_cast<int64_t>(updates.size()));
	result.emplace("commit_message", commit_message);
	return JsonResult(std::move(result));
}

ToolResult GetSelection(json::Object const& args) {
	int window_id = ArgInt(args, "window_id");
	json::Object result;
	WithWindow(window_id, [&](FrameMain *, agi::Context *c) {
		auto index = EventIndex(c);
		std::unordered_map<const AssDialogue *, int64_t> rows;
		for (size_t i = 0; i < index.size(); ++i)
			rows[index[i]] = i;

		json::Array selected;
		for (auto line : c->selectionController->GetSortedSelection())
			selected.emplace_back(rows[line]);
		result.emplace("selected_rows", std::move(selected));

		auto active = c->selectionController->GetActiveLine();
		if (active)
			result.emplace("active_row", rows[active]);
		else
			result.emplace("active_row", json::Null());
	});
	return JsonResult(std::move(result));
}

ToolResult SetSelection(json::Object const& args) {
	int window_id = ArgInt(args, "window_id");

	auto rows_it = args.find("rows");
	if (rows_it == args.end())
		throw ToolError("Missing required argument: rows");
	json::Array const& rows_el = rows_it->second;
	if (rows_el.empty())
		throw ToolError("rows must not be empty");
	std::vector<int64_t> rows;
	for (auto const& r : rows_el)
		rows.push_back(static_cast<json::Integer const&>(r));
	int64_t active_row = ArgInt(args, "active_row", rows.front());

	WithWindow(window_id, [&](FrameMain *, agi::Context *c) {
		auto index = EventIndex(c);
		auto line_at = [&](int64_t row) {
			if (row < 0 || row >= static_cast<int64_t>(index.size()))
				throw ToolError(agi::format("Row %d is out of range (file has %d lines)", row, index.size()));
			return index[row];
		};

		Selection sel;
		for (auto row : rows)
			sel.insert(line_at(row));
		c->selectionController->SetSelectionAndActive(std::move(sel), line_at(active_row));
	});

	json::Object result;
	result.emplace("selected", static_cast<int64_t>(rows.size()));
	return JsonResult(std::move(result));
}

ToolResult FocusWindow(json::Object const& args) {
	int window_id = ArgInt(args, "window_id");
	WithWindow(window_id, [&](FrameMain *frame, agi::Context *) {
		frame->Show(true);
		frame->Raise();
	});
	json::Object result;
	result.emplace("focused", true);
	return JsonResult(std::move(result));
}

ToolResult OpenSubtitle(json::Object const& args) {
	std::string path_str = ArgString(args, "path");
	bool new_window = ArgBool(args, "new_window", false);

	agi::fs::path path(path_str);
	if (!agi::fs::FileExists(path))
		throw ToolError("File not found: " + path_str);
	auto ext = path.extension().string();
	boost::to_lower(ext);
	if (ext != ".ass" && ext != ".srt" && ext != ".ssa" && ext != ".sub" && ext != ".ttxt" && ext != ".txt")
		throw ToolError("Unsupported subtitle format: " + ext);

	json::Object result;
	if (new_window) {
		agi::dispatch::Main().Sync([&] {
			auto& c = wxGetApp().NewProjectContext();
			c.project->LoadSubtitles(path);
			result.emplace("window_id", c.frame->window_id);
		});
	}
	else {
		int window_id = ArgInt(args, "window_id");
		WithWindow(window_id, [&](FrameMain *, agi::Context *c) {
			if (c->subsController->IsModified())
				throw ToolError(agi::format("Window %d has unsaved changes; open in a new window instead (new_window: true) or let the user save first", window_id));
			c->project->LoadSubtitles(path);
			result.emplace("window_id", window_id);
		});
	}
	result.emplace("subtitle_path", path.string());
	return JsonResult(std::move(result));
}

}

namespace mcp {

void RegisterTextTools(agi::mcp::Dispatcher& d) {
	d.RegisterTool({
		"list_windows",
		"List all open Aegisub windows with their window_id, open subtitle/video/audio files and line count. Call this first: every other tool needs a window_id from here.",
		R"json({"type":"object","properties":{}})json",
		ListWindows,
		false
	});
	d.RegisterTool({
		"get_project_info",
		"Get project context for one window: script info, playback resolution, styles, subtitle file state, and the loaded video/audio (null when none is loaded).",
		R"json({"type":"object","properties":{"window_id":{"type":"integer","description":"Window id from list_windows"}},"required":["window_id"]})json",
		GetProjectInfo,
		false
	});
	d.RegisterTool({
		"get_lines",
		"Read subtitle lines from a window. Each line has row, id, start_ms/end_ms, style, actor, effect, comment flag, text (with ASS override tags) and original (the reference/source text kept alongside translations). Set include_stripped to also get text without override tags; when translating, preserve the tags from text.",
		R"json({"type":"object","properties":{"window_id":{"type":"integer","description":"Window id from list_windows"},"start_row":{"type":"integer","description":"First row to return, 0-based (default 0)"},"count":{"type":"integer","description":"Maximum number of rows to return (default: all)"},"include_stripped":{"type":"boolean","description":"Also return stripped_text with override tags removed (default false)"}},"required":["window_id"]})json",
		GetLines,
		false
	});
	d.RegisterTool({
		"set_lines",
		"Update the text and/or original of subtitle lines in one batch. The whole batch is a single undo step. By default, when a line's original is still empty its current text is preserved there before being overwritten, so translating never loses the source text. Rows are validated before anything is changed.",
		R"json({"type":"object","properties":{"window_id":{"type":"integer","description":"Window id from list_windows"},"items":{"type":"array","items":{"type":"object","properties":{"row":{"type":"integer","description":"Row to update (0-based)"},"text":{"type":"string","description":"New line text (keep ASS override tags like {\\i1} and \\N from the previous text)"},"original":{"type":"string","description":"New original/reference text"}},"required":["row"]},"description":"Lines to update; each needs text and/or original"},"commit_message":{"type":"string","description":"Undo description shown to the user"},"fill_original":{"type":"boolean","description":"Preserve old text into empty original fields before overwriting (default true)"}},"required":["window_id","items"]})json",
		SetLines,
		true
	});
	d.RegisterTool({
		"get_selection",
		"Get the rows currently selected in the subtitle grid and the active row the user is editing.",
		R"json({"type":"object","properties":{"window_id":{"type":"integer","description":"Window id from list_windows"}},"required":["window_id"]})json",
		GetSelection,
		false
	});
	d.RegisterTool({
		"set_selection",
		"Select rows in the subtitle grid and set the active line. Useful to show the user which line is being worked on; the video seeks to the active line.",
		R"json({"type":"object","properties":{"window_id":{"type":"integer","description":"Window id from list_windows"},"rows":{"type":"array","items":{"type":"integer"},"description":"Rows to select (0-based)"},"active_row":{"type":"integer","description":"Row to make active (default: first of rows)"}},"required":["window_id","rows"]})json",
		SetSelection,
		true
	});
	d.RegisterTool({
		"focus_window",
		"Bring an Aegisub window to the front so the user can watch the work happening in it.",
		R"json({"type":"object","properties":{"window_id":{"type":"integer","description":"Window id from list_windows"}},"required":["window_id"]})json",
		FocusWindow,
		false
	});
	d.RegisterTool({
		"open_subtitle",
		"Open a subtitle file. With new_window true it opens in a new window (returns the new window_id) — use this to open reference files, e.g. a previously translated episode. Otherwise it replaces the file in the given window, which is refused when that window has unsaved changes.",
		R"json({"type":"object","properties":{"path":{"type":"string","description":"Absolute path to a subtitle file (.ass, .srt, .ssa, .sub, .ttxt, .txt)"},"new_window":{"type":"boolean","description":"Open in a new window (default false)"},"window_id":{"type":"integer","description":"Target window when new_window is false"}},"required":["path"]})json",
		OpenSubtitle,
		true
	});
}

}
