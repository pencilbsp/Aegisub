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
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
// IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include <wx/string.h>
#include <wx/timer.h>
#include <wx/window.h>

#include <functional>
#include <vector>

class wxDC;

namespace agi {
struct GlossaryEntry;
}

class GlossaryPopup final : public wxWindow {
	struct SourceLine {
		wxString text;
		bool link = false;
		bool bold = false;
	};

	struct DrawLine {
		wxString text;
		bool link = false;
		bool bold = false;
		wxRect rect;
	};

	wxTimer fade_timer;
	std::vector<SourceLine> source_lines;
	std::vector<DrawLine> lines;
	wxString link_url;
	std::function<void()> keep_alive;
	std::function<void()> begin_dismiss;
	std::function<void()> close_now;
	std::function<void()> open_editor;
	double opacity = 1.0;
	int arrow_x = 0;
	int hovered_line = -1;
	bool arrow_on_top = true;
	bool dismissing = false;

	wxSize CalculateBestSize();
	void WrapText(wxDC& dc, int max_width);
	void LayoutLines();
	wxFont LineFont(bool bold) const;
	/// Index of the link line under a client point, or -1 if none.
	int LinkLineAt(wxPoint const& point) const;
	void OnPaint(wxPaintEvent&);
	void OnSize(wxSizeEvent&);
	void OnFadeTimer(wxTimerEvent&);
	void OnMouseEnter(wxMouseEvent&);
	void OnMouseLeave(wxMouseEvent&);
	void OnMouseMove(wxMouseEvent&);
	void OnLeftUp(wxMouseEvent&);
	void OnDoubleClick(wxMouseEvent&);
	void OpenLink();

public:
	GlossaryPopup(wxWindow *anchor, agi::GlossaryEntry const& entry, std::function<void()> keep_alive,
		std::function<void()> begin_dismiss, std::function<void()> close_now,
		std::function<void()> open_editor);

	bool AcceptsFocus() const override { return false; }
	bool AcceptsFocusFromKeyboard() const override { return false; }

	void PopupAt(wxRect const& anchor_rect);
	void Dismiss(bool fade);
	void CancelDismiss();
	bool ContainsScreenPoint(wxPoint const& point) const;
	bool ContainsWindow(wxWindow *window) const;
};
