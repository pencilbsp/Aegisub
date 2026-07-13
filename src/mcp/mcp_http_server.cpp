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

#include "mcp_http_server.h"

#include <libaegisub/log.h>
#include <libaegisub/mcp/protocol.h>

#include <atomic>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <optional>
#include <thread>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;

bool token_equal(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) return false;
	unsigned char diff = 0;
	for (size_t i = 0; i < a.size(); ++i)
		diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
	return diff == 0;
}

std::string_view to_sv(beast::string_view sv) {
	return std::string_view(sv.data(), sv.size());
}

bool is_local_host_header(std::string_view host) {
	// Strip a :port suffix (the host is never a bracketed IPv6 literal
	// here since we only ever bind IPv4 loopback)
	auto colon = host.rfind(':');
	if (colon != std::string_view::npos)
		host = host.substr(0, colon);
	return host == "127.0.0.1" || host == "localhost";
}

bool is_local_origin(std::string_view origin) {
	for (std::string_view prefix : {"http://127.0.0.1", "http://localhost", "https://127.0.0.1", "https://localhost"}) {
		if (origin.substr(0, prefix.size()) == prefix) {
			auto rest = origin.substr(prefix.size());
			if (rest.empty() || rest[0] == ':' || rest[0] == '/')
				return true;
		}
	}
	return false;
}

}

namespace mcp {

class HttpServer::Impl {
public:
	agi::mcp::Dispatcher& dispatcher;
	std::string token;
	asio::io_context ioc;
	tcp::acceptor acceptor{ioc};
	std::thread thread;
	std::atomic<bool> shutting_down{false};
	std::atomic<bool> thread_done{false};
	unsigned short bound_port = 0;

	Impl(agi::mcp::Dispatcher& dispatcher, unsigned short port, std::string token)
	: dispatcher(dispatcher)
	, token(std::move(token))
	{
		tcp::endpoint ep(asio::ip::make_address_v4("127.0.0.1"), port);
		acceptor.open(ep.protocol());
		acceptor.set_option(asio::socket_base::reuse_address(true));

		boost::system::error_code ec;
		acceptor.bind(ep, ec);
		if (ec == asio::error::address_in_use) {
			LOG_W("mcp/http") << "port " << port << " is in use, falling back to an ephemeral port";
			ep.port(0);
			acceptor.bind(ep); // throws when even this fails
		}
		else if (ec)
			throw boost::system::system_error(ec);

		acceptor.listen();
		bound_port = acceptor.local_endpoint().port();

		Accept();
		thread = std::thread([this] {
			ioc.run();
			thread_done = true;
		});
	}

	void Accept();

	void Stop() {
		if (shutting_down.exchange(true)) return;
		// stop() abandons idle keep-alive reads so the loop can wind down;
		// a handler currently blocked in Main().Sync still runs to
		// completion (its response is dropped, which is fine at shutdown)
		ioc.stop();
	}
};

namespace {

class Session : public std::enable_shared_from_this<Session> {
	HttpServer::Impl& server;
	beast::tcp_stream stream;
	beast::flat_buffer buffer;
	std::optional<http::request_parser<http::string_body>> parser;
	http::response<http::string_body> res;

	void SetPlain(http::status status, std::string_view body) {
		res.result(status);
		res.set(http::field::content_type, "text/plain");
		res.body() = body;
	}

	bool Authorized(http::request<http::string_body> const& req) const {
		auto auth = to_sv(req[http::field::authorization]);
		constexpr std::string_view scheme = "Bearer ";
		return auth.substr(0, scheme.size()) == scheme
			&& token_equal(auth.substr(scheme.size()), server.token);
	}

public:
	Session(HttpServer::Impl& server, tcp::socket sock)
	: server(server), stream(std::move(sock)) { }

	void Read() {
		parser.emplace();
		parser->body_limit(16 * 1024 * 1024);
		stream.expires_after(std::chrono::seconds(30));
		http::async_read(stream, buffer, *parser,
			[self = shared_from_this()](boost::system::error_code ec, size_t) {
				if (ec) return self->Close();
				self->Handle();
			});
	}

	void Handle() {
		// Tool handlers legitimately take a while (they wait for the GUI
		// thread), so the read timeout must not fire mid-request
		stream.expires_never();

		auto req = parser->release();
		res = {};
		res.version(req.version());
		res.keep_alive(req.keep_alive());
		res.set(http::field::server, "aegisub-mcp");

		if (server.shutting_down)
			SetPlain(http::status::service_unavailable, "server is shutting down");
		else if (req.target() != "/mcp")
			SetPlain(http::status::not_found, "not found; the MCP endpoint is /mcp");
		// DNS-rebinding hardening: a browser reaching this server through a
		// hostile hostname sends that hostname in Host/Origin
		else if (!is_local_host_header(to_sv(req[http::field::host])))
			SetPlain(http::status::forbidden, "forbidden");
		else if (req.count(http::field::origin) && !is_local_origin(to_sv(req[http::field::origin])))
			SetPlain(http::status::forbidden, "forbidden");
		else if (req.method() != http::verb::post) {
			SetPlain(http::status::method_not_allowed, "only POST is supported (SSE streams are not)");
			res.set(http::field::allow, "POST");
		}
		else if (!Authorized(req)) {
			SetPlain(http::status::unauthorized, "missing or invalid bearer token");
			res.set(http::field::www_authenticate, "Bearer realm=\"aegisub-mcp\"");
		}
		else if (auto reply = server.dispatcher.HandlePost(req.body())) {
			res.result(http::status::ok);
			res.set(http::field::content_type, "application/json");
			res.body() = std::move(*reply);
		}
		else
			res.result(http::status::accepted); // notification: no body

		res.prepare_payload();
		http::async_write(stream, res,
			[self = shared_from_this()](boost::system::error_code ec, size_t) {
				if (ec || !self->res.keep_alive()) return self->Close();
				self->Read();
			});
	}

	void Close() {
		boost::system::error_code ec;
		stream.socket().shutdown(tcp::socket::shutdown_send, ec);
	}
};

}

void HttpServer::Impl::Accept() {
	acceptor.async_accept([this](boost::system::error_code ec, tcp::socket sock) {
		if (!ec && !shutting_down)
			std::make_shared<Session>(*this, std::move(sock))->Read();
		if (!ec && acceptor.is_open())
			Accept();
	});
}

HttpServer::HttpServer(agi::mcp::Dispatcher& dispatcher, unsigned short port, std::string token)
: impl(std::make_unique<Impl>(dispatcher, port, std::move(token)))
{
}

HttpServer::~HttpServer() {
	if (impl) Shutdown();
}

unsigned short HttpServer::BoundPort() const {
	return impl->bound_port;
}

void HttpServer::Shutdown(std::function<void()> pump_events) {
	if (!impl) return;
	impl->Stop();

	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!impl->thread_done && std::chrono::steady_clock::now() < deadline) {
		if (pump_events) pump_events();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	if (impl->thread_done) {
		impl->thread.join();
		impl.reset();
	}
	else {
		// Should be unreachable: a request survived the drain. Leak the
		// impl rather than freeing state the stuck thread still references.
		LOG_E("mcp/http") << "server thread failed to stop within 3s, detaching";
		impl->thread.detach();
		impl.release();
	}
}

}
