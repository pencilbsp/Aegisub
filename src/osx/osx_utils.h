// Copyright (c) 2026
//
// macOS-specific UI utilities shared by the application code.

#pragma once

class wxFrame;

namespace osx {
/// Put `below` immediately behind `above` in AppKit's document-window order.
/// This keeps Cmd+` following an explicit project sequence after a long task
/// has caused AppKit to fall back to most-recently-used ordering.
void order_window_immediately_below(wxFrame *above, wxFrame *below);
}
