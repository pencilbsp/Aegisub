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

#include <functional>
#include <memory>
#include <string>

namespace agi::mcp { class Dispatcher; }

namespace mcp {

/// Localhost-only HTTP transport for the MCP dispatcher, run on its own
/// thread. Requests are handled one at a time on that thread; tool handlers
/// marshal GUI access via agi::dispatch::Main().Sync themselves.
class HttpServer {
public:
	// Opaque implementation; public only so the connection session type in
	// the .cpp can name it
	class Impl;

private:
	std::unique_ptr<Impl> impl;

public:

	/// Bind 127.0.0.1:port (falling back to an ephemeral port when taken)
	/// and start serving. Throws boost::system::system_error when even the
	/// fallback bind fails.
	HttpServer(agi::mcp::Dispatcher& dispatcher, unsigned short port, std::string token);
	~HttpServer();

	/// The port actually bound, which differs from the requested one when it
	/// was already in use
	unsigned short BoundPort() const;

	/// Stop the server and join its thread. Must be called from the GUI
	/// thread. The server thread may be blocked inside Main().Sync waiting
	/// for the GUI thread, so pump_events (e.g. ProcessPendingEvents) is
	/// called in a loop while draining; without it a request in flight at
	/// shutdown would deadlock.
	void Shutdown(std::function<void()> pump_events = {});
};

}
