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

#include <libaegisub/mcp/protocol.h>

#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>

#include <algorithm>
#include <sstream>

namespace {

constexpr const char *protocol_version = "2025-06-18";

// JSON-RPC 2.0 error codes
constexpr int err_parse = -32700;
constexpr int err_invalid_request = -32600;
constexpr int err_method_not_found = -32601;
constexpr int err_invalid_params = -32602;
constexpr int err_internal = -32603;

json::UnknownElement *find(json::Object &obj, std::string_view key) {
	auto it = obj.find(key);
	return it == obj.end() ? nullptr : &it->second;
}

std::string serialize(json::Object const& obj) {
	std::ostringstream oss;
	agi::JsonWriter::Write(obj, oss);
	return oss.str();
}

json::Object error_body(int code, std::string message) {
	json::Object err;
	err.emplace("code", code);
	err.emplace("message", std::move(message));
	return err;
}

// Error response when no request id could be extracted (id must be null)
std::string error_without_id(int code, std::string message) {
	json::Object resp;
	resp.emplace("jsonrpc", "2.0");
	resp.emplace("id", json::Null());
	resp.emplace("error", error_body(code, std::move(message)));
	return serialize(resp);
}

}

namespace agi::mcp {

json::Object TextContent(std::string text) {
	json::Object o;
	o.emplace("type", "text");
	o.emplace("text", std::move(text));
	return o;
}

json::Object ImageContent(std::string base64_data, std::string mime_type) {
	json::Object o;
	o.emplace("type", "image");
	o.emplace("data", std::move(base64_data));
	o.emplace("mimeType", std::move(mime_type));
	return o;
}

json::Object AudioContent(std::string base64_data, std::string mime_type) {
	json::Object o;
	o.emplace("type", "audio");
	o.emplace("data", std::move(base64_data));
	o.emplace("mimeType", std::move(mime_type));
	return o;
}

std::optional<std::string> Dispatcher::HandlePost(std::string_view body) {
	json::UnknownElement root;
	try {
		std::istringstream iss{std::string(body)};
		json::Reader::Read(root, iss);
	}
	catch (json::Exception const&) {
		return error_without_id(err_parse, "Parse error");
	}

	json::Object *request;
	try {
		request = &static_cast<json::Object&>(root);
	}
	catch (json::Exception const&) {
		// Includes top-level arrays: JSON-RPC batching was removed in
		// protocol version 2025-06-18
		return error_without_id(err_invalid_request, "Invalid Request: expected a single request object");
	}

	auto id_it = request->find("id");
	// Notifications get no response of any kind, and every client->server
	// notification in MCP (notifications/initialized etc.) is a no-op for a
	// tools-only server, so they are all absorbed here
	if (id_it == request->end()) return std::nullopt;

	auto error = [&](int code, std::string message) {
		json::Object resp;
		resp.emplace("jsonrpc", "2.0");
		resp.emplace("id", std::move(id_it->second));
		resp.emplace("error", error_body(code, std::move(message)));
		return serialize(resp);
	};
	auto success = [&](json::Object result) {
		json::Object resp;
		resp.emplace("jsonrpc", "2.0");
		resp.emplace("id", std::move(id_it->second));
		resp.emplace("result", std::move(result));
		return serialize(resp);
	};

	try {
		auto version = find(*request, "jsonrpc");
		if (!version || static_cast<json::String const&>(*version) != "2.0")
			return error(err_invalid_request, "Invalid Request: jsonrpc must be \"2.0\"");

		auto method_el = find(*request, "method");
		if (!method_el)
			return error(err_invalid_request, "Invalid Request: missing method");
		std::string method = static_cast<json::String const&>(*method_el);

		json::Object no_params;
		json::Object *params = &no_params;
		if (auto p = find(*request, "params"))
			params = &static_cast<json::Object&>(*p);

		if (method == "initialize") {
			json::Object caps;
			caps.emplace("tools", json::Object());
			json::Object info;
			info.emplace("name", server_name);
			info.emplace("version", server_version);
			json::Object result;
			result.emplace("protocolVersion", protocol_version);
			result.emplace("capabilities", std::move(caps));
			result.emplace("serverInfo", std::move(info));
			return success(std::move(result));
		}

		if (method == "ping")
			return success(json::Object());

		if (method == "tools/list") {
			json::Array list;
			for (auto const& tool : tools) {
				json::Object o;
				o.emplace("name", tool.name);
				o.emplace("description", tool.description);
				std::istringstream schema{tool.input_schema_json};
				json::UnknownElement parsed_schema;
				json::Reader::Read(parsed_schema, schema);
				o.emplace("inputSchema", std::move(parsed_schema));
				list.emplace_back(std::move(o));
			}
			json::Object result;
			result.emplace("tools", std::move(list));
			return success(std::move(result));
		}

		if (method == "tools/call") {
			auto name_el = find(*params, "name");
			if (!name_el)
				return error(err_invalid_params, "Missing tool name");
			std::string name = static_cast<json::String const&>(*name_el);

			auto tool = std::find_if(tools.begin(), tools.end(),
				[&](Tool const& t) { return t.name == name; });
			if (tool == tools.end())
				return error(err_invalid_params, "Unknown tool: " + name);

			json::Object no_args;
			json::Object *args = &no_args;
			if (auto a = find(*params, "arguments"))
				args = &static_cast<json::Object&>(*a);

			ToolResult tr;
			if (tool->mutating && is_read_only && is_read_only()) {
				tr.content.emplace_back(TextContent("The Aegisub MCP server is in read-only mode; tool '" + name + "' is disabled. The user can change this in Preferences > Advanced > MCP Server."));
				tr.is_error = true;
			}
			else {
				try {
					tr = tool->handler(*args);
				}
				catch (ToolError const& e) {
					tr.content.emplace_back(TextContent(e.GetMessage()));
					tr.is_error = true;
				}
				catch (json::Exception const&) {
					return error(err_invalid_params, "Invalid arguments for tool: " + name);
				}
				catch (agi::Exception const& e) {
					return error(err_internal, "Internal error: " + e.GetMessage());
				}
				catch (std::exception const& e) {
					return error(err_internal, std::string("Internal error: ") + e.what());
				}
			}

			json::Object result;
			result.emplace("content", std::move(tr.content));
			result.emplace("isError", tr.is_error);
			return success(std::move(result));
		}

		return error(err_method_not_found, "Method not found: " + method);
	}
	catch (json::Exception const&) {
		// Type mismatch while reading the request envelope (e.g. non-string
		// method, non-object params)
		return error(err_invalid_request, "Invalid Request");
	}
}

}
