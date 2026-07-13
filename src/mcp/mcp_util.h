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

#include <libaegisub/mcp/protocol.h>

#include <functional>
#include <string>
#include <vector>

class AssDialogue;
class FrameMain;
namespace agi { struct Context; }

namespace mcp {

/// Run fn on the GUI thread with the frame and context of the given window.
/// Must be called from the server thread. Throws ToolError (rethrown on the
/// calling thread) if no window has this id.
void WithWindow(int window_id, std::function<void(FrameMain *, agi::Context *)> fn);

// Typed argument accessors. The no-default overloads throw ToolError when
// the argument is missing; all throw json::Exception on a type mismatch,
// which the dispatcher reports as invalid params.
int64_t ArgInt(json::Object const& args, const char *name);
int64_t ArgInt(json::Object const& args, const char *name, int64_t def);
double ArgDouble(json::Object const& args, const char *name);
double ArgDouble(json::Object const& args, const char *name, double def);
std::string ArgString(json::Object const& args, const char *name);
std::string ArgString(json::Object const& args, const char *name, std::string def);
bool ArgBool(json::Object const& args, const char *name, bool def);
bool HasArg(json::Object const& args, const char *name);

std::string Base64(const void *data, size_t len);

/// Serialize a JSON object to a compact string
std::string SerializeJson(json::Object const& obj);

/// Convenience: wrap a JSON object as the single text content of a result
agi::mcp::ToolResult JsonResult(json::Object obj);

/// Row -> line pointers for the file's events, in file order. GUI thread only.
std::vector<AssDialogue *> EventIndex(agi::Context *c);

}
