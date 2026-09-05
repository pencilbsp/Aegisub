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

#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/visitor.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

struct Discovery {
	std::string url;
	std::string token;
	std::string protocol_version = "2025-06-18";
};

struct Endpoint {
	std::string host;
	std::string port;
	std::string target;
};

class CompactJsonWriter final : json::ConstVisitor {
	std::ostream& os;

	void Visit(json::Array const& array) override {
		os << '[';
		bool first = true;
		for (auto const& entry : array) {
			if (!first) os << ',';
			first = false;
			Visit(entry);
		}
		os << ']';
	}

	void Visit(bool boolean) override {
		os << (boolean ? "true" : "false");
	}

	void Visit(double number) override {
		os.precision(20);
		os << number;
	}

	void Visit(int64_t number) override {
		os << number;
	}

	void Visit(json::Null const&) override {
		os << "null";
	}

	void Visit(json::Object const& object) override {
		os << '{';
		bool first = true;
		for (auto const& entry : object) {
			if (!first) os << ',';
			first = false;
			Visit(entry.first);
			os << ':';
			Visit(entry.second);
		}
		os << '}';
	}

	void Visit(std::string const& str) override {
		os << '"';
		for (unsigned char c : str) {
			switch (c) {
				case '"': os << "\\\""; break;
				case '\\': os << "\\\\"; break;
				case '\b': os << "\\b"; break;
				case '\f': os << "\\f"; break;
				case '\n': os << "\\n"; break;
				case '\r': os << "\\r"; break;
				case '\t': os << "\\t"; break;
				default:
					if (c < 0x20) {
						static constexpr char hex[] = "0123456789abcdef";
						os << "\\u00" << hex[c >> 4] << hex[c & 0x0F];
					}
					else
						os << c;
			}
		}
		os << '"';
	}

	void Visit(json::UnknownElement const& unknown) {
		unknown.Accept(*this);
	}

public:
	explicit CompactJsonWriter(std::ostream& os) : os(os) { }

	template<typename T>
	static std::string Write(T const& value) {
		std::ostringstream ss;
		CompactJsonWriter(ss).Visit(value);
		return ss.str();
	}
};

[[noreturn]] void fail(std::string const& message) {
	throw std::runtime_error(message);
}

std::string getenv_string(char const *name) {
	if (auto value = std::getenv(name))
		return value;
	return {};
}

std::string default_discovery_path() {
	if (auto override_path = getenv_string("AEGISUB_MCP_DISCOVERY"); !override_path.empty())
		return override_path;

#ifdef _WIN32
	auto appdata = getenv_string("APPDATA");
	if (appdata.empty()) fail("APPDATA is not set; use --discovery or AEGISUB_MCP_DISCOVERY");
	return appdata + "\\Aegisub\\mcp_server.json";
#elif defined(__APPLE__)
	auto home = getenv_string("HOME");
	if (home.empty()) fail("HOME is not set; use --discovery or AEGISUB_MCP_DISCOVERY");
	return home + "/Library/Application Support/Aegisub/mcp_server.json";
#else
	auto home = getenv_string("HOME");
	if (home.empty()) fail("HOME is not set; use --discovery or AEGISUB_MCP_DISCOVERY");
	return home + "/.aegisub/mcp_server.json";
#endif
}

json::Object const& as_object(json::UnknownElement const& value, std::string const& context) {
	try {
		return static_cast<json::Object const&>(value);
	}
	catch (json::Exception const&) {
		fail(context + " must be a JSON object");
	}
}

std::string required_string(json::Object const& object, std::string_view key, std::string const& context) {
	auto it = object.find(key);
	if (it == object.end())
		fail(context + " is missing required field '" + std::string(key) + "'");
	try {
		return static_cast<json::String const&>(it->second);
	}
	catch (json::Exception const&) {
		fail(context + " field '" + std::string(key) + "' must be a string");
	}
}

std::string optional_string(json::Object const& object, std::string_view key, std::string fallback) {
	auto it = object.find(key);
	if (it == object.end())
		return fallback;
	try {
		return static_cast<json::String const&>(it->second);
	}
	catch (json::Exception const&) {
		return fallback;
	}
}

Discovery read_discovery(std::string const& path) {
	std::ifstream file(path);
	if (!file)
		fail("Aegisub MCP discovery file was not found at " + path +
			". Open Aegisub and enable Preferences > Advanced > MCP Server.");

	json::UnknownElement root;
	try {
		json::Reader::Read(root, file);
	}
	catch (json::Exception const& e) {
		fail("Failed to parse Aegisub MCP discovery file at " + path + ": " + e.what());
	}

	auto const& object = as_object(root, "Aegisub MCP discovery file");
	Discovery discovery;
	discovery.url = required_string(object, "url", "Aegisub MCP discovery file");
	discovery.token = required_string(object, "token", "Aegisub MCP discovery file");
	discovery.protocol_version = optional_string(object, "protocolVersion", discovery.protocol_version);
	return discovery;
}

Endpoint parse_endpoint(std::string const& url) {
	constexpr std::string_view scheme = "http://";
	if (url.substr(0, scheme.size()) != scheme)
		fail("Only http:// Aegisub MCP endpoints are supported: " + url);

	auto rest = std::string_view(url).substr(scheme.size());
	auto slash = rest.find('/');
	auto host_port = rest.substr(0, slash);
	auto target = slash == std::string_view::npos ? "/" : rest.substr(slash);
	if (host_port.empty())
		fail("Invalid Aegisub MCP endpoint URL: " + url);

	Endpoint endpoint;
	auto colon = host_port.rfind(':');
	if (colon == std::string_view::npos) {
		endpoint.host = std::string(host_port);
		endpoint.port = "80";
	}
	else {
		endpoint.host = std::string(host_port.substr(0, colon));
		endpoint.port = std::string(host_port.substr(colon + 1));
	}
	endpoint.target = std::string(target);
	if (endpoint.target.empty())
		endpoint.target = "/";
	return endpoint;
}

std::string http_host_header(Endpoint const& endpoint) {
	return endpoint.port == "80" ? endpoint.host : endpoint.host + ":" + endpoint.port;
}

std::string build_http_request(Endpoint const& endpoint, Discovery const& discovery, std::string const& body) {
	std::ostringstream req;
	req << "POST " << endpoint.target << " HTTP/1.1\r\n"
		<< "Host: " << http_host_header(endpoint) << "\r\n"
		<< "Authorization: Bearer " << discovery.token << "\r\n"
		<< "Content-Type: application/json\r\n"
		<< "Accept: application/json, text/event-stream\r\n"
		<< "MCP-Protocol-Version: " << discovery.protocol_version << "\r\n"
		<< "Connection: close\r\n"
		<< "Content-Length: " << body.size() << "\r\n"
		<< "\r\n"
		<< body;
	return req.str();
}

#ifdef _WIN32
class Wsa {
public:
	Wsa() {
		WSADATA data;
		if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
			fail("WSAStartup failed");
	}

	~Wsa() {
		WSACleanup();
	}
};

using socket_t = SOCKET;
constexpr socket_t invalid_socket = INVALID_SOCKET;

void close_socket(socket_t socket) {
	closesocket(socket);
}

std::string socket_error() {
	return "Winsock error " + std::to_string(WSAGetLastError());
}
#else
using socket_t = int;
constexpr socket_t invalid_socket = -1;

void close_socket(socket_t socket) {
	close(socket);
}

std::string socket_error() {
	return std::strerror(errno);
}
#endif

std::string gai_error(int code) {
#ifdef _WIN32
	return gai_strerrorA(code);
#else
	return gai_strerror(code);
#endif
}

class Socket {
	socket_t socket_ = invalid_socket;

public:
	Socket() = default;
	explicit Socket(socket_t socket) : socket_(socket) { }
	Socket(Socket const&) = delete;
	Socket& operator=(Socket const&) = delete;

	Socket(Socket&& other) noexcept : socket_(other.socket_) {
		other.socket_ = invalid_socket;
	}

	Socket& operator=(Socket&& other) noexcept {
		if (this != &other) {
			if (socket_ != invalid_socket)
				close_socket(socket_);
			socket_ = other.socket_;
			other.socket_ = invalid_socket;
		}
		return *this;
	}

	~Socket() {
		if (socket_ != invalid_socket)
			close_socket(socket_);
	}

	socket_t get() const {
		return socket_;
	}

	explicit operator bool() const {
		return socket_ != invalid_socket;
	}
};

Socket connect_tcp(Endpoint const& endpoint) {
	addrinfo hints = {};
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_family = AF_UNSPEC;

	addrinfo *addresses = nullptr;
	auto gai = getaddrinfo(endpoint.host.c_str(), endpoint.port.c_str(), &hints, &addresses);
	if (gai != 0)
		fail("Failed to resolve " + endpoint.host + ":" + endpoint.port + ": " + gai_error(gai));

	Socket connected;
	for (auto addr = addresses; addr; addr = addr->ai_next) {
		Socket candidate(socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol));
		if (!candidate)
			continue;

		if (connect(candidate.get(), addr->ai_addr, static_cast<int>(addr->ai_addrlen)) == 0) {
			connected = std::move(candidate);
			break;
		}
	}

	freeaddrinfo(addresses);
	if (!connected)
		fail("Failed to connect to Aegisub MCP server at " + endpoint.host + ":" + endpoint.port + ": " + socket_error());
	return connected;
}

void send_all(socket_t socket, std::string const& data) {
	size_t sent = 0;
	while (sent < data.size()) {
#ifdef _WIN32
		auto n = send(socket, data.data() + sent, static_cast<int>(data.size() - sent), 0);
#else
		auto n = send(socket, data.data() + sent, data.size() - sent, 0);
#endif
		if (n <= 0)
			fail("Failed to send request to Aegisub MCP server: " + socket_error());
		sent += static_cast<size_t>(n);
	}
}

std::string recv_all(socket_t socket) {
	std::string data;
	char buffer[8192];
	for (;;) {
#ifdef _WIN32
		auto n = recv(socket, buffer, static_cast<int>(sizeof buffer), 0);
#else
		auto n = recv(socket, buffer, sizeof buffer, 0);
#endif
		if (n == 0)
			return data;
		if (n < 0)
			fail("Failed to read response from Aegisub MCP server: " + socket_error());
		data.append(buffer, static_cast<size_t>(n));
	}
}

std::optional<std::string> parse_http_response(std::string const& response) {
	auto header_end = response.find("\r\n\r\n");
	if (header_end == std::string::npos)
		fail("Invalid HTTP response from Aegisub MCP server");

	auto status_line_end = response.find("\r\n");
	if (status_line_end == std::string::npos)
		fail("Invalid HTTP status line from Aegisub MCP server");

	std::istringstream status_line(response.substr(0, status_line_end));
	std::string http_version;
	int status = 0;
	status_line >> http_version >> status;
	if (status == 202)
		return std::nullopt;

	auto body = response.substr(header_end + 4);
	if (status >= 200 && status < 300)
		return body;

	std::string message = "Aegisub MCP server returned HTTP " + std::to_string(status);
	if (!body.empty())
		message += ": " + body;
	fail(message);
}

std::optional<std::string> post_to_aegisub(Discovery const& discovery, std::string const& body) {
	auto endpoint = parse_endpoint(discovery.url);
	auto socket = connect_tcp(endpoint);
	auto request = build_http_request(endpoint, discovery, body);
	send_all(socket.get(), request);
	return parse_http_response(recv_all(socket.get()));
}

std::string compact_json(std::string const& json) {
	json::UnknownElement root;
	std::istringstream input(json);
	json::Reader::Read(root, input);
	return CompactJsonWriter::Write(root);
}

std::optional<std::string> json_rpc_error_for(std::string const& message, int code, std::string const& error_message) {
	json::Object response;
	response.emplace("jsonrpc", "2.0");

	try {
		json::UnknownElement root;
		std::istringstream input(message);
		json::Reader::Read(root, input);
		auto& object = static_cast<json::Object&>(root);
		auto id = object.find("id");
		if (id == object.end())
			return std::nullopt;
		response.emplace("id", std::move(id->second));
	}
	catch (json::Exception const&) {
		response.emplace("id", json::Null());
	}

	json::Object error;
	error.emplace("code", static_cast<int64_t>(code));
	error.emplace("message", error_message);
	response.emplace("error", std::move(error));
	return CompactJsonWriter::Write(response);
}

void usage() {
	std::cerr
		<< "Usage: aegisub-mcp-stdio [stdio] [--discovery PATH]\n"
		<< "\n"
		<< "Bridges MCP stdio clients to Aegisub's local Streamable HTTP MCP server.\n"
		<< "Set AEGISUB_MCP_DISCOVERY or pass --discovery to override the discovery file.\n";
}

std::string parse_args(int argc, char **argv) {
	std::optional<std::string> discovery;
	for (int i = 1; i < argc; ++i) {
		std::string arg = argv[i];
		if (arg == "stdio" || arg == "--stdio")
			continue;
		if (arg == "--help" || arg == "-h") {
			usage();
			std::exit(0);
		}
		if (arg == "--discovery") {
			if (++i >= argc) {
				usage();
				std::exit(2);
			}
			discovery = argv[i];
			continue;
		}
		std::cerr << "Unknown argument: " << arg << "\n";
		usage();
		std::exit(2);
	}
	return discovery ? *discovery : default_discovery_path();
}

}

int main(int argc, char **argv) {
#ifndef _WIN32
	signal(SIGPIPE, SIG_IGN);
#else
	Wsa wsa;
#endif

	auto discovery_path = parse_args(argc, argv);

	std::string line;
	while (std::getline(std::cin, line)) {
		if (line.empty())
			continue;

		try {
			auto discovery = read_discovery(discovery_path);
			if (auto response = post_to_aegisub(discovery, line)) {
				if (!response->empty())
					std::cout << compact_json(*response) << '\n' << std::flush;
			}
		}
		catch (std::exception const& e) {
			std::cerr << "aegisub-mcp-stdio: " << e.what() << "\n";
			if (auto error = json_rpc_error_for(line, -32000, e.what()))
				std::cout << *error << '\n' << std::flush;
		}
	}

	return 0;
}
