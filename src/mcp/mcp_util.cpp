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

#include "mcp_util.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "frame_main.h"
#include "include/aegisub/context.h"
#include "main.h"

#include <libaegisub/cajun/writer.h>
#include <libaegisub/dispatch.h>
#include <libaegisub/format.h>

#include <sstream>
#include <wx/base64.h>

namespace {

json::UnknownElement const *find_arg(json::Object const& args, const char *name) {
	auto it = args.find(name);
	return it == args.end() ? nullptr : &it->second;
}

// Agents routinely send integral values as JSON doubles; accept both
int64_t to_int(json::UnknownElement const& el) {
	try {
		return static_cast<json::Integer const&>(el);
	}
	catch (json::Exception const&) {
		return static_cast<int64_t>(static_cast<json::Double const&>(el));
	}
}

double to_double(json::UnknownElement const& el) {
	try {
		return static_cast<json::Double const&>(el);
	}
	catch (json::Exception const&) {
		return static_cast<json::Integer const&>(el);
	}
}

}

namespace mcp {

void WithWindow(int window_id, std::function<void(FrameMain *, agi::Context *)> fn) {
	agi::dispatch::Main().Sync([&] {
		auto frame = wxGetApp().GetFrameById(window_id);
		if (!frame)
			throw agi::mcp::ToolError(agi::format("Window %d not found (it may have been closed); call list_windows to see the currently open windows", window_id));
		fn(frame, frame->GetContext());
	});
}

int64_t ArgInt(json::Object const& args, const char *name) {
	auto el = find_arg(args, name);
	if (!el)
		throw agi::mcp::ToolError(std::string("Missing required argument: ") + name);
	return to_int(*el);
}

int64_t ArgInt(json::Object const& args, const char *name, int64_t def) {
	auto el = find_arg(args, name);
	return el ? to_int(*el) : def;
}

double ArgDouble(json::Object const& args, const char *name) {
	auto el = find_arg(args, name);
	if (!el)
		throw agi::mcp::ToolError(std::string("Missing required argument: ") + name);
	return to_double(*el);
}

double ArgDouble(json::Object const& args, const char *name, double def) {
	auto el = find_arg(args, name);
	return el ? to_double(*el) : def;
}

std::string ArgString(json::Object const& args, const char *name) {
	auto el = find_arg(args, name);
	if (!el)
		throw agi::mcp::ToolError(std::string("Missing required argument: ") + name);
	return static_cast<json::String const&>(*el);
}

std::string ArgString(json::Object const& args, const char *name, std::string def) {
	auto el = find_arg(args, name);
	return el ? static_cast<json::String const&>(*el) : std::move(def);
}

bool ArgBool(json::Object const& args, const char *name, bool def) {
	auto el = find_arg(args, name);
	return el ? static_cast<json::Boolean const&>(*el) : def;
}

bool HasArg(json::Object const& args, const char *name) {
	return find_arg(args, name) != nullptr;
}

std::string Base64(const void *data, size_t len) {
	return from_wx(wxBase64Encode(data, len));
}

std::string SerializeJson(json::Object const& obj) {
	std::ostringstream oss;
	agi::JsonWriter::Write(obj, oss);
	return oss.str();
}

agi::mcp::ToolResult JsonResult(json::Object obj) {
	agi::mcp::ToolResult r;
	r.content.emplace_back(agi::mcp::TextContent(SerializeJson(obj)));
	return r;
}

std::vector<AssDialogue *> EventIndex(agi::Context *c) {
	std::vector<AssDialogue *> index;
	for (auto& line : c->ass->Events)
		index.push_back(&line);
	return index;
}

}
