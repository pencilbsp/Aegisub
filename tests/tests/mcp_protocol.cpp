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

#include <main.h>

#include <libaegisub/cajun/reader.h>
#include <libaegisub/mcp/protocol.h>

#include <sstream>

class lagi_mcp : public libagi { };

namespace {
using namespace agi::mcp;

json::Object parse_response(std::optional<std::string> const& body) {
	EXPECT_TRUE(body.has_value());
	json::UnknownElement root;
	std::istringstream iss(*body);
	json::Reader::Read(root, iss);
	return std::move(static_cast<json::Object&>(root));
}

json::UnknownElement const& member(json::Object const& obj, const char *key) {
	auto it = obj.find(key);
	if (it == obj.end()) throw json::Exception(std::string("missing key: ") + key);
	return it->second;
}

std::string str(json::Object const& obj, const char *key) {
	json::String const& s = member(obj, key);
	return s;
}

int64_t error_code(json::Object const& resp) {
	json::Integer const& code = member(member(resp, "error"), "code");
	return code;
}

Dispatcher make_dispatcher(bool read_only = false) {
	Dispatcher d("test-server", "1.0");
	d.RegisterTool({
		"echo", "Echo the message back",
		R"({"type":"object","properties":{"message":{"type":"string"}},"required":["message"]})",
		[](json::Object const& args) {
			auto it = args.find("message");
			if (it == args.end()) throw ToolError("missing message");
			ToolResult r;
			r.content.emplace_back(TextContent(static_cast<json::String const&>(it->second)));
			return r;
		},
		false
	});
	d.RegisterTool({
		"mutate", "A mutating tool", R"({"type":"object"})",
		[](json::Object const&) {
			ToolResult r;
			r.content.emplace_back(TextContent("mutated"));
			return r;
		},
		true
	});
	d.SetReadOnlyCheck([=] { return read_only; });
	return d;
}
}

TEST(lagi_mcp, InitializeHandshake) {
	auto d = make_dispatcher();
	auto resp = parse_response(d.HandlePost(
		R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"test","version":"0"}}})"));

	EXPECT_EQ("2.0", str(resp, "jsonrpc"));
	EXPECT_EQ(1, static_cast<json::Integer const&>(member(resp, "id")));

	json::Object const& result = member(resp, "result");
	EXPECT_EQ("2025-06-18", str(result, "protocolVersion"));
	EXPECT_NO_THROW(member(member(result, "capabilities"), "tools"));
	EXPECT_EQ("test-server", str(member(result, "serverInfo"), "name"));

	// Notifications get no response body
	EXPECT_FALSE(d.HandlePost(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").has_value());

	auto pong = parse_response(d.HandlePost(R"({"jsonrpc":"2.0","id":2,"method":"ping"})"));
	EXPECT_NO_THROW(member(pong, "result"));
}

TEST(lagi_mcp, ToolsList) {
	auto d = make_dispatcher();
	auto resp = parse_response(d.HandlePost(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"));

	json::Array const& tools = member(member(resp, "result"), "tools");
	ASSERT_EQ(2u, tools.size());

	json::Object const& echo = tools[0];
	EXPECT_EQ("echo", str(echo, "name"));
	EXPECT_EQ("Echo the message back", str(echo, "description"));
	EXPECT_EQ("object", str(member(echo, "inputSchema"), "type"));
}

TEST(lagi_mcp, ToolsCall) {
	auto d = make_dispatcher();
	auto resp = parse_response(d.HandlePost(
		R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"echo","arguments":{"message":"hi"}}})"));

	json::Object const& result = member(resp, "result");
	EXPECT_FALSE(static_cast<json::Boolean const&>(member(result, "isError")));
	json::Array const& content = member(result, "content");
	ASSERT_EQ(1u, content.size());
	EXPECT_EQ("text", str(content[0], "type"));
	EXPECT_EQ("hi", str(content[0], "text"));

	// Unknown tool -> invalid params
	auto unknown = parse_response(d.HandlePost(
		R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"nope","arguments":{}}})"));
	EXPECT_EQ(-32602, error_code(unknown));

	// ToolError -> isError:true with the message as text content
	auto tool_err = parse_response(d.HandlePost(
		R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"echo","arguments":{}}})"));
	json::Object const& err_result = member(tool_err, "result");
	EXPECT_TRUE(static_cast<json::Boolean const&>(member(err_result, "isError")));
	json::Array const& err_content = member(err_result, "content");
	ASSERT_EQ(1u, err_content.size());
	EXPECT_EQ("missing message", str(err_content[0], "text"));

	// Wrong argument type -> handler's bad cast surfaces as invalid params
	auto bad_type = parse_response(d.HandlePost(
		R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"echo","arguments":{"message":5}}})"));
	EXPECT_EQ(-32602, error_code(bad_type));
}

TEST(lagi_mcp, ReadOnlyMode) {
	auto d = make_dispatcher(true);

	// Mutating tool rejected with isError, not a protocol error
	auto resp = parse_response(d.HandlePost(
		R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"mutate","arguments":{}}})"));
	json::Object const& result = member(resp, "result");
	EXPECT_TRUE(static_cast<json::Boolean const&>(member(result, "isError")));

	// Non-mutating tool still works
	auto echo = parse_response(d.HandlePost(
		R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"echo","arguments":{"message":"ok"}}})"));
	EXPECT_FALSE(static_cast<json::Boolean const&>(member(member(echo, "result"), "isError")));
}

TEST(lagi_mcp, ProtocolErrors) {
	auto d = make_dispatcher();

	// Parse error, id must be null
	auto garbage = parse_response(d.HandlePost("this is not json"));
	EXPECT_EQ(-32700, error_code(garbage));
	EXPECT_NO_THROW(std::ignore = static_cast<json::Null const&>(member(garbage, "id")));

	// Batch requests were removed in 2025-06-18
	auto batch = parse_response(d.HandlePost(R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])"));
	EXPECT_EQ(-32600, error_code(batch));

	// Missing/wrong jsonrpc version
	auto no_version = parse_response(d.HandlePost(R"({"id":1,"method":"ping"})"));
	EXPECT_EQ(-32600, error_code(no_version));

	// Unknown method
	auto unknown = parse_response(d.HandlePost(R"({"jsonrpc":"2.0","id":1,"method":"nope"})"));
	EXPECT_EQ(-32601, error_code(unknown));
}
