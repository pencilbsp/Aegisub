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
#include <string>
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
		bool direct_drag = false;
	};

	struct DrawLine {
		wxString text;
		bool link = false;
		bool bold = false;
		size_t source_index = 0;
		size_t text_offset = 0;
		wxRect rect;
	};

	wxTimer fade_timer;
	std::vector<SourceLine> source_lines;
	std::vector<DrawLine> lines;
	wxString display_text;
	wxString link_url;
	std::function<void()> keep_alive;
	std::function<void()> begin_dismiss;
	std::function<void()> close_now;
	std::function<void()> open_editor;
	std::function<void(std::string const&, bool)> open_search;
	double opacity = 1.0;
	int arrow_x = 0;
	int hovered_line = -1;
	bool arrow_on_top = true;
	bool dismissing = false;
	bool context_menu_open = false;
	bool selecting = false;
	bool selection_dragged = false;
	bool drag_candidate = false;
	bool dragging_text = false;
	wxPoint mouse_down_position;
	wxString direct_drag_text;
	long selection_anchor = -1;
	long selection_caret = -1;

	wxSize CalculateBestSize();
	void WrapText(wxDC& dc, int max_width);
	void LayoutLines();
	wxFont LineFont(bool bold) const;
	/// Index of the link line under a client point, or -1 if none.
	int LinkLineAt(wxPoint const& point) const;
	/// Index of the visible text line under a client point, or -1 if none.
	int TextLineAt(wxPoint const& point) const;
	/// Insertion position in display_text nearest a client point.
	long TextPositionAt(wxPoint const& point, bool clamp) const;
	wxString SelectedText() const;
	wxString AllText() const;
	void BeginTextDrag(wxString const& text);
	void OnPaint(wxPaintEvent&);
	void OnSize(wxSizeEvent&);
	void OnFadeTimer(wxTimerEvent&);
	void OnMouseEnter(wxMouseEvent&);
	void OnMouseLeave(wxMouseEvent&);
	void OnMouseMove(wxMouseEvent&);
	void OnLeftDown(wxMouseEvent&);
	void OnLeftUp(wxMouseEvent&);
	void OnDoubleClick(wxMouseEvent&);
	void OnContextMenu(wxContextMenuEvent&);
	void OnCaptureLost(wxMouseCaptureLostEvent&);
	void OnKeyDown(wxKeyEvent&);
	void OpenLink();

public:
	GlossaryPopup(wxWindow *anchor, agi::GlossaryEntry const& entry, std::function<void()> keep_alive,
		std::function<void()> begin_dismiss, std::function<void()> close_now,
		std::function<void()> open_editor,
		std::function<void(std::string const&, bool)> open_search);
	/// True while a native text drag originating from a glossary popup is active.
	static bool IsTextDragActive();

	bool AcceptsFocus() const override { return true; }
	bool AcceptsFocusFromKeyboard() const override { return true; }

	void PopupAt(wxRect const& anchor_rect);
	void Dismiss(bool fade);
	void CancelDismiss();
	bool ContainsScreenPoint(wxPoint const& point) const;
	bool ContainsWindow(wxWindow *window) const;
};
