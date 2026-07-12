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

#include "glossary_popup.h"

#include "compat.h"
#include "options.h"
#include "utils.h"

#include <libaegisub/glossary.h>

#include <algorithm>

#include <wx/cursor.h>
#include <wx/dataobj.h>
#include <wx/dcclient.h>
#include <wx/dnd.h>
#include <wx/intl.h>
#include <wx/log.h>
#include <wx/menu.h>
#include <wx/settings.h>
#include <wx/utils.h>

namespace {
enum {
	GLOSSARY_POPUP_COPY = (wxID_HIGHEST + 1) + 11000,
	GLOSSARY_POPUP_COPY_ALL,
	GLOSSARY_POPUP_FIND,
	GLOSSARY_POPUP_REPLACE
};

constexpr int PaddingX = 8;
constexpr int PaddingY = 5;
constexpr int MaxWidth = 360;
constexpr int MinWidth = 80;
constexpr int GapBelowAnchor = 1;
constexpr int ArrowWidth = 18;
constexpr int ArrowHeight = 10;
constexpr int ArrowInset = 18;
constexpr int LineGap = 3;
constexpr int FadeIntervalMs = 16;
constexpr double FadeStep = 0.18;

const wxColour TooltipBg(255, 255, 225);
const wxColour TooltipBorder(118, 118, 118);
const wxColour TextColour(32, 32, 32);
const wxColour LinkColour(0, 102, 204);
const wxColour LinkHoverColour(0, 51, 153);

bool glossary_text_drag_active = false;

bool IsHex(char c) {
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

bool IsSafeUriByte(unsigned char c) {
	if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
		return true;

	switch (c) {
		case '-': case '.': case '_': case '~':
		case ':': case '/': case '?': case '#': case '[': case ']': case '@':
		case '!': case '$': case '&': case '\'': case '(': case ')': case '*':
		case '+': case ',': case ';': case '=':
			return true;
		default:
			return false;
	}
}

wxString EscapeUriForLaunch(wxString const& uri) {
	static const char hex[] = "0123456789ABCDEF";
	wxCharBuffer utf8 = uri.utf8_str();
	std::string escaped;

	for (const unsigned char *p = reinterpret_cast<const unsigned char *>(utf8.data()); p && *p; ++p) {
		if (*p == '%' && IsHex(static_cast<char>(p[1])) && IsHex(static_cast<char>(p[2]))) {
			escaped += '%';
			escaped += static_cast<char>(p[1]);
			escaped += static_cast<char>(p[2]);
			p += 2;
		}
		else if (IsSafeUriByte(*p)) {
			escaped += static_cast<char>(*p);
		}
		else {
			escaped += '%';
			escaped += hex[*p >> 4];
			escaped += hex[*p & 0x0F];
		}
	}

	return wxString::FromUTF8(escaped);
}

wxColour Blend(wxColour fg, wxColour bg, double alpha) {
	alpha = std::clamp(alpha, 0.0, 1.0);
	return wxColour(
		wxColour::AlphaBlend(fg.Red(), bg.Red(), alpha),
		wxColour::AlphaBlend(fg.Green(), bg.Green(), alpha),
		wxColour::AlphaBlend(fg.Blue(), bg.Blue(), alpha));
}

wxWindow *PopupParentFor(wxWindow *anchor) {
	if (wxWindow *top_level = wxGetTopLevelParent(anchor))
		return top_level;
	return anchor->GetParent();
}
}

GlossaryPopup::GlossaryPopup(wxWindow *anchor, agi::GlossaryEntry const& entry, std::function<void()> keep_alive,
	std::function<void()> begin_dismiss, std::function<void()> close_now, std::function<void()> open_editor,
	std::function<void(std::string const&, bool)> open_search)
: wxWindow()
, fade_timer(this)
, link_url(to_wx(entry.note_url))
, keep_alive(std::move(keep_alive))
, begin_dismiss(std::move(begin_dismiss))
, close_now(std::move(close_now))
, open_editor(std::move(open_editor))
, open_search(std::move(open_search))
{
	// wxWidgets requires transparent background style to be selected before
	// the native window is created.
	SetBackgroundStyle(wxBG_STYLE_TRANSPARENT);
	Create(PopupParentFor(anchor), wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);

	auto add_line = [this](std::string const& line, bool link = false, bool bold = false, bool direct_drag = false) {
		if (!line.empty())
			source_lines.push_back({to_wx(line), link, bold, direct_drag});
	};

	add_line(entry.term, false, true, true);
	add_line(entry.note_text, false, false, true);
	add_line(entry.note_url, true);

	SetCanFocus(true);

	wxFont font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
	wxString fontname = FontFace("Tool/Glossary/Popup");
	if (!fontname.empty())
		font.SetFaceName(fontname);
	font.SetPointSize(OPT_GET("Tool/Glossary/Popup/Font Size")->GetInt());
	SetFont(font);

	// The content is drawn directly rather than hosted in a wxTextCtrl: on macOS
	// that control doesn't emit link clicks and can't give the URL a hover
	// style/cursor, and destroying it mid mouse-event has crashed. Owner-drawing
	// gives full control over the link's colour, underline and pointer.
	Bind(wxEVT_PAINT, &GlossaryPopup::OnPaint, this);
	Bind(wxEVT_SIZE, &GlossaryPopup::OnSize, this);
	Bind(wxEVT_TIMER, &GlossaryPopup::OnFadeTimer, this);
	Bind(wxEVT_ENTER_WINDOW, &GlossaryPopup::OnMouseEnter, this);
	Bind(wxEVT_LEAVE_WINDOW, &GlossaryPopup::OnMouseLeave, this);
	Bind(wxEVT_MOTION, &GlossaryPopup::OnMouseMove, this);
	Bind(wxEVT_LEFT_DOWN, &GlossaryPopup::OnLeftDown, this);
	Bind(wxEVT_LEFT_UP, &GlossaryPopup::OnLeftUp, this);
	Bind(wxEVT_LEFT_DCLICK, &GlossaryPopup::OnDoubleClick, this);
	Bind(wxEVT_CONTEXT_MENU, &GlossaryPopup::OnContextMenu, this);
	Bind(wxEVT_MOUSE_CAPTURE_LOST, &GlossaryPopup::OnCaptureLost, this);
	Bind(wxEVT_CHAR_HOOK, &GlossaryPopup::OnKeyDown, this);
	Hide();
}

wxFont GlossaryPopup::LineFont(bool bold) const {
	wxFont font = GetFont();
	if (bold)
		font.SetWeight(wxFONTWEIGHT_BOLD);
	return font;
}

void GlossaryPopup::WrapText(wxDC& dc, int max_width) {
	lines.clear();
	display_text.clear();

	for (size_t source_index = 0; source_index < source_lines.size(); ++source_index) {
		auto const& source = source_lines[source_index];
		dc.SetFont(LineFont(source.bold));
		for (wxString source_line : wxSplit(source.text, '\n')) {
			if (source_line.empty()) {
				lines.push_back({"", source.link, source.bold, source_index, 0, wxRect()});
				continue;
			}

			wxString line;
			wxString pending_space;

			for (size_t i = 0; i < source_line.length(); ++i) {
				wxString ch = source_line.Mid(i, 1);
				if (ch == " " || ch == "\t") {
					pending_space += " ";
					continue;
				}

				wxString candidate = line + pending_space + ch;
				if (!line.empty() && dc.GetTextExtent(candidate).GetWidth() > max_width) {
					lines.push_back({line, source.link, source.bold, source_index, 0, wxRect()});
					line = ch;
				}
				else {
					line = candidate;
				}
				pending_space.clear();
			}

			if (!line.empty())
				lines.push_back({line, source.link, source.bold, source_index, 0, wxRect()});
		}
	}

	if (lines.empty())
		lines.push_back({"", false, false, 0, 0, wxRect()});

	for (auto& line : lines) {
		if (!display_text.empty())
			display_text += "\n";
		line.text_offset = display_text.length();
		display_text += line.text;
	}
}

wxSize GlossaryPopup::CalculateBestSize() {
	wxClientDC dc(this);
	dc.SetFont(GetFont());
	WrapText(dc, MaxWidth - PaddingX * 2);

	int width = MinWidth;
	int height = PaddingY * 2;
	for (size_t i = 0; i < lines.size(); ++i) {
		auto const& line = lines[i];
		dc.SetFont(LineFont(line.bold));
		wxSize extent = dc.GetTextExtent(line.text.empty() ? wxString(" ") : line.text);
		width = std::max(width, extent.GetWidth() + PaddingX * 2);
		height += extent.GetHeight();
		if (i + 1 < lines.size())
			height += FromDIP(LineGap);
	}

	return wxSize(std::min(width, MaxWidth), height + ArrowHeight);
}

void GlossaryPopup::LayoutLines() {
	wxClientDC dc(this);

	int y = (arrow_on_top ? ArrowHeight : 0) + PaddingY;
	for (auto& line : lines) {
		dc.SetFont(LineFont(line.bold));
		wxSize extent = dc.GetTextExtent(line.text.empty() ? wxString(" ") : line.text);
		line.rect = wxRect(PaddingX, y, extent.GetWidth(), extent.GetHeight());
		y += extent.GetHeight() + FromDIP(LineGap);
	}
}

int GlossaryPopup::LinkLineAt(wxPoint const& point) const {
	for (size_t i = 0; i < lines.size(); ++i) {
		if (!lines[i].link || lines[i].text.empty())
			continue;
		wxRect hit = lines[i].rect;
		hit.Inflate(2, 1);
		if (hit.Contains(point))
			return static_cast<int>(i);
	}
	return -1;
}

int GlossaryPopup::TextLineAt(wxPoint const& point) const {
	for (size_t i = 0; i < lines.size(); ++i) {
		if (lines[i].text.empty())
			continue;
		if (lines[i].rect.Contains(point))
			return static_cast<int>(i);
	}
	return -1;
}

long GlossaryPopup::TextPositionAt(wxPoint const& point, bool clamp) const {
	if (lines.empty())
		return -1;

	size_t line_index = lines.size();
	for (size_t i = 0; i < lines.size(); ++i) {
		int top = lines[i].rect.GetTop();
		int bottom = lines[i].rect.GetBottom() + FromDIP(LineGap);
		if (point.y >= top && point.y <= bottom) {
			line_index = i;
			break;
		}
	}

	if (line_index == lines.size()) {
		if (!clamp)
			return -1;
		line_index = point.y < lines.front().rect.GetTop() ? 0 : lines.size() - 1;
	}

	DrawLine const& line = lines[line_index];
	if (line.text.empty())
		return static_cast<long>(line.text_offset);
	if (point.x <= line.rect.GetLeft())
		return static_cast<long>(line.text_offset);
	if (point.x >= line.rect.GetRight())
		return static_cast<long>(line.text_offset + line.text.length());

	wxClientDC dc(const_cast<GlossaryPopup *>(this));
	dc.SetFont(LineFont(line.bold));
	wxArrayInt widths;
	dc.GetPartialTextExtents(line.text, widths);
	int relative_x = point.x - line.rect.GetLeft();
	int previous = 0;
	for (size_t i = 0; i < widths.size(); ++i) {
		if (relative_x < (previous + widths[i]) / 2)
			return static_cast<long>(line.text_offset + i);
		previous = widths[i];
	}
	return static_cast<long>(line.text_offset + line.text.length());
}

wxString GlossaryPopup::SelectedText() const {
	if (selection_anchor < 0 || selection_caret < 0 || selection_anchor == selection_caret)
		return {};
	long start = std::min(selection_anchor, selection_caret);
	long end = std::max(selection_anchor, selection_caret);
	return display_text.Mid(static_cast<size_t>(start), static_cast<size_t>(end - start));
}

wxString GlossaryPopup::AllText() const {
	wxString text;
	for (auto const& source : source_lines) {
		if (!text.empty())
			text += "\n";
		text += source.text;
	}
	return text;
}

void GlossaryPopup::BeginTextDrag(wxString const& text) {
	if (text.empty())
		return;

	// ReleaseMouse() can synchronously deliver mouse-leave/capture-lost on
	// macOS. Mark the native drag active first so those events cannot dismiss
	// and queue-destroy this transient popup while wxDropSource is starting.
	dragging_text = true;
	if (keep_alive)
		keep_alive();
	if (HasCapture())
		ReleaseMouse();

	wxTextDataObject data(text);
	// AppKit retains the source NSView for the duration of DoDragDrop(). The
	// popup is transient and may be hidden after a successful drop, so use the
	// stable top-level Aegisub window rather than the popup's own native view.
	wxWindow *source_window = wxGetTopLevelParent(this);
	wxDropSource source(data, source_window ? source_window : GetParent());
	glossary_text_drag_active = true;
	wxDragResult result = source.DoDragDrop(wxDrag_CopyOnly);
	glossary_text_drag_active = false;
	dragging_text = false;

	if (result == wxDragCopy) {
		// The editor has inserted and committed the text. Closing is deferred
		// by Dismiss(), so returning from the current mouse event is safe.
		if (close_now)
			close_now();
		else
			Dismiss(false);
		return;
	}

	if (!ContainsScreenPoint(wxGetMousePosition()) && begin_dismiss)
		begin_dismiss();
}

bool GlossaryPopup::IsTextDragActive() {
	return glossary_text_drag_active;
}

void GlossaryPopup::PopupAt(wxRect const& anchor_rect) {
	dismissing = false;
	opacity = 1.0;
	hovered_line = -1;
	fade_timer.Stop();

	wxWindow *parent = GetParent();
	wxSize size = CalculateBestSize();
	wxRect client_screen(parent->ClientToScreen(wxPoint()), parent->GetClientSize());

	int x = anchor_rect.GetLeft();
	int y = anchor_rect.GetBottom() + GapBelowAnchor;
	int anchor_center = anchor_rect.GetLeft() + anchor_rect.GetWidth() / 2;

	x = std::max(client_screen.GetLeft(), std::min(x, client_screen.GetRight() - size.GetWidth()));
	arrow_on_top = true;
	if (y + size.GetHeight() > client_screen.GetBottom()) {
		y = anchor_rect.GetTop() - GapBelowAnchor - size.GetHeight();
		arrow_on_top = false;
	}
	y = std::max(client_screen.GetTop(), std::min(y, client_screen.GetBottom() - size.GetHeight()));
	arrow_x = std::clamp(anchor_center - x, ArrowInset, size.GetWidth() - ArrowInset);

	SetSize(wxRect(parent->ScreenToClient(wxPoint(x, y)), size));
	LayoutLines();
	Show();
	Raise();
	Refresh();
}

void GlossaryPopup::Dismiss(bool fade) {
	if (dismissing) return;
	dismissing = true;

	if (fade && IsShown()) {
		fade_timer.Start(FadeIntervalMs);
		return;
	}

	// Hide right away so it disappears instantly, but defer the actual delete:
	// Dismiss() can be called from within this window's own mouse-leave dispatch
	// (begin_dismiss), and destroying it synchronously while AppKit is still
	// routing that tracking-area event frees a peer it keeps using -> crash.
	Hide();
	CallAfter([this] { Destroy(); });
}

void GlossaryPopup::CancelDismiss() {
	if (!dismissing) return;

	fade_timer.Stop();
	dismissing = false;
	opacity = 1.0;
	Refresh();
}

bool GlossaryPopup::ContainsScreenPoint(wxPoint const& point) const {
	return IsShown() && GetScreenRect().Contains(point);
}

bool GlossaryPopup::ContainsWindow(wxWindow *window) const {
	for (; window; window = window->GetParent()) {
		if (window == this)
			return true;
	}
	return false;
}

void GlossaryPopup::OnFadeTimer(wxTimerEvent&) {
	opacity -= FadeStep;
	if (opacity <= 0.0) {
		fade_timer.Stop();
		Hide();
		Destroy();
		return;
	}

	Refresh();
}

void GlossaryPopup::OnSize(wxSizeEvent& event) {
	LayoutLines();
	event.Skip();
}

void GlossaryPopup::OnPaint(wxPaintEvent&) {
	// macOS and GTK paint windows natively double-buffered. Using wxPaintDC
	// here also avoids wxAutoBufferedPaintDC's requirement that the whole
	// rectangular window have an opaque wxBG_STYLE_PAINT background.
	wxPaintDC dc(this);

	wxColour parent_bg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);

	wxColour bg = Blend(TooltipBg, parent_bg, opacity);
	wxColour border = Blend(TooltipBorder, parent_bg, opacity);

	dc.SetPen(wxPen(border));
	dc.SetBrush(wxBrush(bg));

	wxSize size = GetClientSize();
	int body_top = arrow_on_top ? ArrowHeight : 0;
	int body_bottom = arrow_on_top ? size.y - 1 : size.y - ArrowHeight - 1;
	wxPoint shape[7];
	if (arrow_on_top) {
		shape[0] = wxPoint(0, body_top);
		shape[1] = wxPoint(arrow_x - ArrowWidth / 2, body_top);
		shape[2] = wxPoint(arrow_x, 0);
		shape[3] = wxPoint(arrow_x + ArrowWidth / 2, body_top);
		shape[4] = wxPoint(size.x - 1, body_top);
		shape[5] = wxPoint(size.x - 1, size.y - 1);
		shape[6] = wxPoint(0, size.y - 1);
	}
	else {
		shape[0] = wxPoint(0, 0);
		shape[1] = wxPoint(size.x - 1, 0);
		shape[2] = wxPoint(size.x - 1, body_bottom);
		shape[3] = wxPoint(arrow_x + ArrowWidth / 2, body_bottom);
		shape[4] = wxPoint(arrow_x, size.y - 1);
		shape[5] = wxPoint(arrow_x - ArrowWidth / 2, body_bottom);
		shape[6] = wxPoint(0, body_bottom);
	}
	dc.DrawPolygon(7, shape);

	// Draw the wrapped lines. The link line gets a link colour, and turns a
	// different colour with an underline while hovered (see OnMouseMove).
	wxFont plain = GetFont();
	wxFont bold = LineFont(true);
	wxFont underlined = plain;
	underlined.SetUnderlined(true);
	wxColour selection_bg = Blend(wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT), parent_bg, opacity);
	wxColour selection_text = Blend(wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT), parent_bg, opacity);
	long selection_start = std::min(selection_anchor, selection_caret);
	long selection_end = std::max(selection_anchor, selection_caret);

	for (size_t i = 0; i < lines.size(); ++i) {
		DrawLine const& line = lines[i];
		if (line.text.empty())
			continue;

		bool hovered = line.link && static_cast<int>(i) == hovered_line;
		wxColour colour = line.link ? (hovered ? LinkHoverColour : LinkColour) : TextColour;
		wxFont line_font = line.bold ? bold : (hovered ? underlined : plain);
		dc.SetFont(line_font);
		dc.SetTextForeground(Blend(colour, parent_bg, opacity));
		dc.DrawText(line.text, line.rect.GetPosition());

		long line_start = static_cast<long>(line.text_offset);
		long line_end = line_start + static_cast<long>(line.text.length());
		long selected_start = std::max(selection_start, line_start);
		long selected_end = std::min(selection_end, line_end);
		if (selected_start < selected_end) {
			wxString prefix = line.text.Left(static_cast<size_t>(selected_start - line_start));
			wxString selected = line.text.Mid(static_cast<size_t>(selected_start - line_start),
				static_cast<size_t>(selected_end - selected_start));
			int x = line.rect.x + dc.GetTextExtent(prefix).GetWidth();
			int width = dc.GetTextExtent(selected).GetWidth();
			dc.SetPen(*wxTRANSPARENT_PEN);
			dc.SetBrush(wxBrush(selection_bg));
			dc.DrawRectangle(x, line.rect.y, width, line.rect.height);
			dc.SetTextForeground(selection_text);
			dc.DrawText(selected, wxPoint(x, line.rect.y));
		}
	}
}

void GlossaryPopup::OnMouseEnter(wxMouseEvent& event) {
	if (keep_alive)
		keep_alive();
	event.Skip();
}

void GlossaryPopup::OnMouseLeave(wxMouseEvent& event) {
	if (context_menu_open || selecting || dragging_text) {
		event.Skip();
		return;
	}

	if (hovered_line != -1) {
		hovered_line = -1;
		Refresh();
	}
	SetCursor(wxNullCursor);
	event.Skip();
	// begin_dismiss() may hide/queue-destroy this popup; touch nothing after it.
	if (begin_dismiss)
		begin_dismiss();
}

void GlossaryPopup::OnMouseMove(wxMouseEvent& event) {
	if (selecting && event.LeftIsDown() && !direct_drag_text.empty() &&
			!GetClientRect().Contains(event.GetPosition())) {
		wxPoint delta = event.GetPosition() - mouse_down_position;
		if (std::abs(delta.x) >= FromDIP(4) || std::abs(delta.y) >= FromDIP(4)) {
			selecting = false;
			selection_dragged = false;
			BeginTextDrag(direct_drag_text);
			return;
		}
	}

	if (drag_candidate && event.LeftIsDown()) {
		wxPoint delta = event.GetPosition() - mouse_down_position;
		if (std::abs(delta.x) >= FromDIP(4) || std::abs(delta.y) >= FromDIP(4)) {
			drag_candidate = false;
			BeginTextDrag(SelectedText());
			return;
		}
	}

	if (selecting && event.LeftIsDown()) {
		long position = TextPositionAt(event.GetPosition(), true);
		if (position >= 0 && position != selection_caret) {
			selection_caret = position;
			selection_dragged = selection_caret != selection_anchor;
			if (selection_dragged && !HasFocus())
				SetFocus();
			Refresh();
		}
	}

	int link = LinkLineAt(event.GetPosition());
	if (link != hovered_line) {
		hovered_line = link;
		Refresh();
	}
	SetCursor(link >= 0 ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
	event.Skip();
}

void GlossaryPopup::OnLeftDown(wxMouseEvent& event) {
	long position = TextPositionAt(event.GetPosition(), false);
	mouse_down_position = event.GetPosition();
	direct_drag_text.clear();
	int line = TextLineAt(event.GetPosition());
	if (line >= 0 && source_lines[lines[line].source_index].direct_drag)
		direct_drag_text = source_lines[lines[line].source_index].text;
	long selected_start = std::min(selection_anchor, selection_caret);
	long selected_end = std::max(selection_anchor, selection_caret);
	if (position >= selected_start && position < selected_end && !SelectedText().empty()) {
		drag_candidate = true;
		selecting = false;
		direct_drag_text.clear();
		if (!HasCapture())
			CaptureMouse();
		event.Skip();
		return;
	}

	drag_candidate = false;
	selection_anchor = position;
	selection_caret = position;
	selection_dragged = false;
	selecting = position >= 0;
	if (selecting && !HasCapture())
		CaptureMouse();
	Refresh();
	event.Skip();
}

void GlossaryPopup::OnLeftUp(wxMouseEvent& event) {
	if (drag_candidate) {
		drag_candidate = false;
		if (HasCapture())
			ReleaseMouse();
		long position = TextPositionAt(event.GetPosition(), false);
		selection_anchor = position;
		selection_caret = position;
		Refresh();
	}

	if (selecting) {
		long position = TextPositionAt(event.GetPosition(), true);
		if (position >= 0)
			selection_caret = position;
		selection_dragged = selection_caret != selection_anchor;
		selecting = false;
		if (HasCapture())
			ReleaseMouse();
		Refresh();
		if (selection_dragged)
			return;
	}

	if (LinkLineAt(event.GetPosition()) >= 0)
		OpenLink();
	else
		event.Skip();
}

void GlossaryPopup::OnDoubleClick(wxMouseEvent& event) {
	// open_editor is expected to defer its work (it dismisses this popup and
	// opens a modal dialog), so it's safe to return into the event machinery.
	if (open_editor)
		open_editor();
	else
		event.Skip();
}

void GlossaryPopup::OnContextMenu(wxContextMenuEvent& event) {
	if (keep_alive)
		keep_alive();

	wxPoint screen_position = event.GetPosition();
	if (screen_position == wxDefaultPosition)
		screen_position = wxGetMousePosition();
	int line = TextLineAt(ScreenToClient(screen_position));
	// Context actions are only available when the pointer is directly over a
	// rendered glyph row, not over the popup's padding, arrow, or background.
	if (line < 0)
		return;

	wxString selected_text = SelectedText();
	wxString action_text = selected_text.empty()
		? source_lines[lines[line].source_index].text
		: selected_text;

	wxMenu menu;
	menu.Append(GLOSSARY_POPUP_COPY, _("Copy"));
	menu.Append(GLOSSARY_POPUP_COPY_ALL, _("Copy All"));
	menu.AppendSeparator();
	menu.Append(GLOSSARY_POPUP_FIND, _("Find..."));
	menu.Append(GLOSSARY_POPUP_REPLACE, _("Replace with..."));

	context_menu_open = true;
	int selection = GetPopupMenuSelectionFromUser(menu, ScreenToClient(screen_position));
	context_menu_open = false;

	if (selection == GLOSSARY_POPUP_COPY)
		SetClipboard(from_wx(action_text));
	else if (selection == GLOSSARY_POPUP_COPY_ALL)
		SetClipboard(from_wx(AllText()));
	else if ((selection == GLOSSARY_POPUP_FIND || selection == GLOSSARY_POPUP_REPLACE) && open_search)
		open_search(from_wx(action_text), selection == GLOSSARY_POPUP_REPLACE);

	// Opening the native menu generates a mouse-leave event. Once it closes,
	// re-evaluate whether the pointer still keeps this popup alive.
	if (!ContainsScreenPoint(wxGetMousePosition()) && begin_dismiss)
		begin_dismiss();
}

void GlossaryPopup::OnCaptureLost(wxMouseCaptureLostEvent&) {
	selecting = false;
	drag_candidate = false;
}

void GlossaryPopup::OnKeyDown(wxKeyEvent& event) {
	if ((event.GetKeyCode() == 'C' || event.GetKeyCode() == 'c') && event.CmdDown()) {
		wxString selected_text = SelectedText();
		if (!selected_text.empty()) {
			SetClipboard(from_wx(selected_text));
			return;
		}
	}
	event.Skip();
}

void GlossaryPopup::OpenLink() {
	// Copy first: close_now()/Dismiss() may queue this popup for destruction.
	wxString escaped_url = EscapeUriForLaunch(link_url);
	if (close_now)
		close_now();
	else
		Dismiss(false);

	wxLogNull suppress_launch_error;
	wxLaunchDefaultBrowser(escaped_url, wxBROWSER_NEW_WINDOW);
}
