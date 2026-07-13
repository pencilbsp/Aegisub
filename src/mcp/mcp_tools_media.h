// Copyright (c) 2026, Aegisub Project http://www.aegisub.org/
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#pragma once

namespace agi::mcp { class Dispatcher; }

namespace mcp {

/// Video frames and line/range audio clips for multimodal clients
void RegisterMediaTools(agi::mcp::Dispatcher& d);

}
