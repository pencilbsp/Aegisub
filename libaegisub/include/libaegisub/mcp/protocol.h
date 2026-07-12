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

#include <libaegisub/cajun/elements.h>
#include <libaegisub/exception.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agi::mcp {

/// Thrown by tool handlers to report a tool-level failure. Surfaces to the
/// client as a successful JSON-RPC response with isError: true, so the agent
/// can read the message and recover.
DEFINE_EXCEPTION(ToolError, Exception);

/// Build a text content item for a tool result
json::Object TextContent(std::string text);
/// Build an image content item from base64-encoded data
json::Object ImageContent(std::string base64_data, std::string mime_type = "image/png");
/// Build an audio content item from base64-encoded data
json::Object AudioContent(std::string base64_data, std::string mime_type = "audio/wav");

struct ToolResult {
	json::Array content;
	bool is_error = false;
};

struct Tool {
	std::string name;
	std::string description;
	/// JSON Schema for the tool arguments, as a JSON string literal. Stored
	/// as text because cajun elements are move-only and each tools/list
	/// response needs a fresh copy.
	std::string input_schema_json;
	std::function<ToolResult(json::Object const& args)> handler;
	/// Mutating tools are rejected while the read-only check returns true
	bool mutating = false;
};

/// MCP server protocol core: JSON-RPC 2.0 dispatch for the methods needed by
/// a tools-only MCP server (initialize, ping, tools/list, tools/call).
/// Transport-agnostic and thread-agnostic: HandlePost is a pure
/// request-body -> response-body function; handlers are responsible for any
/// thread marshalling they need.
class Dispatcher {
	std::string server_name;
	std::string server_version;
	std::vector<Tool> tools;
	std::function<bool()> is_read_only;

public:
	Dispatcher(std::string server_name, std::string server_version)
	: server_name(std::move(server_name)), server_version(std::move(server_version)) { }

	void RegisterTool(Tool tool) { tools.emplace_back(std::move(tool)); }
	void SetReadOnlyCheck(std::function<bool()> check) { is_read_only = std::move(check); }

	/// Handle one JSON-RPC message from the body of a POST to the MCP
	/// endpoint. Returns the response body to send with Content-Type
	/// application/json, or nullopt if the message was a notification (the
	/// transport should respond 202 Accepted with no body).
	std::optional<std::string> HandlePost(std::string_view body);
};

}
