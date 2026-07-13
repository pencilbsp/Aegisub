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

#include "mcp_host.h"

#include "mcp_http_server.h"
#include "mcp_tools.h"
#include "mcp_tools_edit.h"
#include "mcp_tools_glossary.h"
#include "mcp_tools_media.h"
#include "options.h"
#include "version.h"

#include <libaegisub/cajun/writer.h>
#include <libaegisub/fs.h>
#include <libaegisub/io.h>
#include <libaegisub/log.h>
#include <libaegisub/mcp/protocol.h>
#include <libaegisub/path.h>

#include <atomic>
#include <random>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include <wx/app.h>

namespace {

std::unique_ptr<agi::mcp::Dispatcher> dispatcher;
std::unique_ptr<mcp::HttpServer> server;
std::vector<agi::signal::Connection> option_connections;
// Mirrored from the option: read from the server thread, where touching the
// option system directly wouldn't be safe
std::atomic<bool> read_only{false};

agi::fs::path DiscoveryPath() {
	return config::path->Decode("?user/mcp_server.json");
}

std::string GenerateToken() {
	std::random_device rd;
	std::string token;
	token.reserve(32);
	for (int i = 0; i < 4; ++i) {
		auto word = rd();
		for (int j = 0; j < 8; ++j) {
			token += "0123456789abcdef"[word & 0xF];
			word >>= 4;
		}
	}
	return token;
}

void WriteDiscoveryFile(unsigned short port, std::string const& token) {
	auto path = DiscoveryPath();
	{
		json::Object o;
		o.emplace("port", port);
		o.emplace("token", token);
		o.emplace("pid", static_cast<int64_t>(wxGetProcessId()));
		o.emplace("url", "http://127.0.0.1:" + std::to_string(port) + "/mcp");
		o.emplace("protocolVersion", "2025-06-18");
		o.emplace("app", "Aegisub");
		o.emplace("version", GetAegisubShortVersionString());

		agi::io::Save file(path);
		agi::JsonWriter::Write(o, file.Get());
	}
#ifndef _WIN32
	// The file holds the bearer token; keep it owner-readable only
	chmod(path.string().c_str(), S_IRUSR | S_IWUSR);
#endif
}

void RemoveDiscoveryFile() {
	try {
		agi::fs::Remove(DiscoveryPath());
	}
	catch (agi::Exception const& e) {
		LOG_E("mcp/host") << "failed to remove discovery file: " << e.GetMessage();
	}
}

void Start() {
	if (server) return;

	if (!dispatcher) {
		dispatcher = std::make_unique<agi::mcp::Dispatcher>("aegisub", GetAegisubShortVersionString());
		dispatcher->SetReadOnlyCheck([] { return read_only.load(); });
		mcp::RegisterTextTools(*dispatcher);
		mcp::RegisterEditTools(*dispatcher);
		mcp::RegisterMediaTools(*dispatcher);
		mcp::RegisterGlossaryTools(*dispatcher);
	}

	auto port = static_cast<unsigned short>(OPT_GET("MCP/Port")->GetInt());
	auto token = GenerateToken();
	try {
		server = std::make_unique<mcp::HttpServer>(*dispatcher, port, token);
	}
	catch (std::exception const& e) {
		LOG_E("mcp/host") << "failed to start MCP server: " << e.what();
		return;
	}

	WriteDiscoveryFile(server->BoundPort(), token);
	LOG_I("mcp/host") << "MCP server listening on http://127.0.0.1:" << server->BoundPort() << "/mcp";
}

void Stop() {
	if (!server) return;
	server->Shutdown([] { wxTheApp->ProcessPendingEvents(); });
	server.reset();
	RemoveDiscoveryFile();
}

}

namespace mcp {

void Init() {
	read_only = OPT_GET("MCP/Read Only")->GetBool();

	option_connections.push_back(OPT_SUB("MCP/Enabled", [](agi::OptionValue const& v) {
		v.GetBool() ? Start() : Stop();
	}));
	option_connections.push_back(OPT_SUB("MCP/Port", [](agi::OptionValue const&) {
		if (server) {
			Stop();
			Start();
		}
	}));
	option_connections.push_back(OPT_SUB("MCP/Read Only", [](agi::OptionValue const& v) {
		read_only = v.GetBool();
	}));

	if (OPT_GET("MCP/Enabled")->GetBool())
		Start();
}

void Shutdown() {
	Stop();
	option_connections.clear();
	dispatcher.reset();
}

}
