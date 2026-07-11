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

#include <libaegisub/glossary.h>

#include <algorithm>

#include <wx/cursor.h>
#include <wx/dcclient.h>
#include <wx/intl.h>
#include <wx/log.h>
#include <wx/settings.h>
#include <wx/utils.h>

namespace {
constexpr int PaddingX = 8;
constexpr int PaddingY = 5;
constexpr int MaxWidth = 360;
constexpr int MinWidth = 80;
constexpr int GapBelowAnchor = 1;
constexpr int ArrowWidth = 18;
constexpr int ArrowHeight = 10;
constexpr int ArrowInset = 18;
constexpr int FadeIntervalMs = 16;
constexpr double FadeStep = 0.18;

const wxColour TooltipBg(255, 255, 225);
const wxColour TooltipBorder(118, 118, 118);
const wxColour TextColour(32, 32, 32);
const wxColour LinkColour(0, 102, 204);
const wxColour LinkHoverColour(0, 51, 153);

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
	std::function<void()> begin_dismiss, std::function<void()> close_now, std::function<void()> open_editor)
: wxWindow()
, fade_timer(this)
, link_url(to_wx(entry.note_url))
, keep_alive(std::move(keep_alive))
, begin_dismiss(std::move(begin_dismiss))
, close_now(std::move(close_now))
, open_editor(std::move(open_editor))
{
	// wxWidgets requires transparent background style to be selected before
	// the native window is created.
	SetBackgroundStyle(wxBG_STYLE_TRANSPARENT);
	Create(PopupParentFor(anchor), wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);

	auto add_line = [this](std::string const& line, bool link = false, bool bold = false) {
		if (!line.empty())
			source_lines.push_back({to_wx(line), link, bold});
	};

	add_line(entry.term, false, true);
	add_line(entry.note_text);
	add_line(entry.note_url, true);

	SetCanFocus(false);
	SetFont(wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT));

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
	Bind(wxEVT_LEFT_UP, &GlossaryPopup::OnLeftUp, this);
	Bind(wxEVT_LEFT_DCLICK, &GlossaryPopup::OnDoubleClick, this);
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

	for (auto const& source : source_lines) {
		dc.SetFont(LineFont(source.bold));
		for (wxString source_line : wxSplit(source.text, '\n')) {
			if (source_line.empty()) {
				lines.push_back({"", source.link, source.bold, wxRect()});
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
					lines.push_back({line, source.link, source.bold, wxRect()});
					line = ch;
				}
				else {
					line = candidate;
				}
				pending_space.clear();
			}

			if (!line.empty())
				lines.push_back({line, source.link, source.bold, wxRect()});
		}
	}

	if (lines.empty())
		lines.push_back({"", false, false, wxRect()});
}

wxSize GlossaryPopup::CalculateBestSize() {
	wxClientDC dc(this);
	dc.SetFont(GetFont());
	WrapText(dc, MaxWidth - PaddingX * 2);

	int width = MinWidth;
	int height = PaddingY * 2;
	for (auto const& line : lines) {
		dc.SetFont(LineFont(line.bold));
		wxSize extent = dc.GetTextExtent(line.text.empty() ? wxString(" ") : line.text);
		width = std::max(width, extent.GetWidth() + PaddingX * 2);
		height += extent.GetHeight();
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
		y += extent.GetHeight();
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

	for (size_t i = 0; i < lines.size(); ++i) {
		DrawLine const& line = lines[i];
		if (line.text.empty())
			continue;

		bool hovered = line.link && static_cast<int>(i) == hovered_line;
		wxColour colour = line.link ? (hovered ? LinkHoverColour : LinkColour) : TextColour;
		dc.SetFont(line.bold ? bold : (hovered ? underlined : plain));
		dc.SetTextForeground(Blend(colour, parent_bg, opacity));
		dc.DrawText(line.text, line.rect.GetPosition());
	}
}

void GlossaryPopup::OnMouseEnter(wxMouseEvent& event) {
	if (keep_alive)
		keep_alive();
	event.Skip();
}

void GlossaryPopup::OnMouseLeave(wxMouseEvent& event) {
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
	int link = LinkLineAt(event.GetPosition());
	if (link != hovered_line) {
		hovered_line = link;
		Refresh();
	}
	SetCursor(link >= 0 ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
	event.Skip();
}

void GlossaryPopup::OnLeftUp(wxMouseEvent& event) {
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
