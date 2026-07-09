// Copyright (c) 2013, Thomas Goyne <plorkyeran@aegisub.org>
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
//
// Aegisub Project http://www.aegisub.org/

/// @file dialog_search_replace.cpp
/// @brief Find and Search/replace dialogue box and logic
/// @ingroup secondary_ui
///

#include "dialog_search_replace.h"

#include "ass_file.h"
#include "compat.h"
#include "dialog_manager.h"
#include "include/aegisub/context.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "search_replace_engine.h"
#include "selection_controller.h"
#include "text_selection_controller.h"
#include "utils.h"
#include "validators.h"

#include <functional>
#include <utility>

#include <wx/button.h>
#include <wx/bmpbndl.h>
#include <wx/checkbox.h>
#include <wx/control.h>
#include <wx/combobox.h>
#include <wx/dcbuffer.h>
#include <wx/radiobut.h>
#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/valgen.h>

enum class PreviewSegmentType {
	Normal,
	OldMatch,
	Replacement
};

struct PreviewSegment {
	PreviewSegmentType type;
	wxString text;
};

static std::vector<PreviewSegment> make_segments(SearchReplaceMatch const& match, bool include_replacement) {
	std::vector<PreviewSegment> segments;
	if (match.start > 0) {
		segments.push_back({PreviewSegmentType::Normal, to_wx(match.current_text.substr(0, match.start))});
	}
	segments.push_back({PreviewSegmentType::OldMatch, to_wx(match.matched_text)});
	if (include_replacement && !match.replacement_match.empty()) {
		segments.push_back({PreviewSegmentType::Replacement, to_wx(match.replacement_match)});
	}
	if (match.end < match.current_text.size()) {
		segments.push_back({PreviewSegmentType::Normal, to_wx(match.current_text.substr(match.end))});
	}
	return segments;
}

// "replace" icon from VS Code codicons, (c) Microsoft Corporation, licensed
// under CC BY 4.0 (https://github.com/microsoft/vscode-codicons). The fill is
// set to currentColor so it can be recolored to the current theme below.
static const char *kReplaceIconSVG = R"SVG(<svg width="16" height="16" viewBox="0 0 16 16" xmlns="http://www.w3.org/2000/svg" fill="currentColor"><path fill-rule="evenodd" clip-rule="evenodd" d="M10 2.813C10.295 2.619 10.634 2.5 11 2.5C12.103 2.5 13 3.509 13 4.75C13 5.991 12.103 7 11 7C10.62 7 10.269 6.873 9.966 6.666C9.897 6.86 9.717 7 9.5 7C9.224 7 9 6.776 9 6.5V1.5C9 1.224 9.224 1 9.5 1C9.776 1 10 1.224 10 1.5V2.813ZM10 4.75C10 5.439 10.448 6 11 6C11.552 6 12 5.439 12 4.75C12 4.061 11.552 3.5 11 3.5C10.448 3.5 10 4.061 10 4.75Z"/><path fill-rule="evenodd" clip-rule="evenodd" d="M2 9H7C7.552 9 8 9.448 8 10V15C8 15.552 7.552 16 7 16H2C1.448 16 1 15.552 1 15V10C1 9.448 1.448 9 2 9ZM5.039 13.645C4.81 13.849 4.199 13.968 3.83 13.506C3.617 13.24 3.5 12.88 3.5 12.492C3.5 12.104 3.618 11.744 3.83 11.478C4.201 11.014 4.811 11.133 5.04 11.339C5.244 11.525 5.56 11.507 5.746 11.303C5.931 11.098 5.915 10.782 5.709 10.597C4.922 9.887 3.733 10.001 3.049 10.853C2.695 11.297 2.5 11.878 2.5 12.492C2.5 13.106 2.695 13.688 3.049 14.131C3.43 14.605 3.945 14.867 4.5 14.867C4.941 14.867 5.359 14.701 5.708 14.387C5.913 14.202 5.93 13.886 5.745 13.681C5.559 13.476 5.243 13.458 5.039 13.645Z"/><path d="M3.99998 4.5C3.99998 3.673 4.67298 3 5.49998 3H7.50198C7.77798 3 8.00198 3.224 8.00198 3.5C8.00198 3.776 7.77798 4 7.50198 4H5.50198C5.22598 4 5.00198 4.225 5.00198 4.5V6.293L6.14798 5.147C6.34298 4.952 6.65998 4.952 6.85498 5.147C7.04998 5.342 7.04998 5.659 6.85498 5.854L4.85498 7.854C4.75698 7.951 4.62898 8 4.50098 8C4.37298 8 4.24498 7.952 4.14698 7.854L2.14698 5.854C1.95198 5.659 1.95198 5.342 2.14698 5.147C2.34198 4.952 2.65898 4.952 2.85398 5.147L3.99998 6.293V4.5Z"/></svg>)SVG";

static wxBitmapBundle make_replace_icon(wxColour colour) {
	wxString svg(kReplaceIconSVG, wxConvUTF8);
	svg.Replace("currentColor", colour.GetAsString(wxC2S_HTML_SYNTAX));
	auto utf8 = svg.utf8_str();
	return wxBitmapBundle::FromSVG(utf8.data(), wxSize(16, 16));
}

template<typename Enum>
static wxSizer *make_radio_group(wxWindow *parent, wxString const& label, std::vector<std::pair<wxString, Enum>> const& choices, Enum *value, std::function<void(wxCommandEvent&)> const& on_change) {
	auto sizer = new wxStaticBoxSizer(wxHORIZONTAL, parent, label);
	auto box = sizer->GetStaticBox();
	int gap = parent->FromDIP(24);

	for (size_t i = 0; i < choices.size(); ++i) {
		auto const& choice = choices[i];
		auto button = new wxRadioButton(box, -1, choice.first, wxDefaultPosition, wxDefaultSize, i == 0 ? wxRB_GROUP : 0);
		button->SetValue(*value == choice.second);
		button->Bind(wxEVT_RADIOBUTTON, [value, enum_value = choice.second, on_change](wxCommandEvent& event) {
			*value = enum_value;
			on_change(event);
		});
		sizer->Add(button, wxSizerFlags().Center().Border(wxRIGHT, i + 1 == choices.size() ? 0 : gap));
	}

	return sizer;
}

class SearchReplacePreview final : public wxScrolledWindow {
	std::vector<SearchReplaceMatch> matches;
	int selected = -1;
	std::function<void(size_t)> select_match;
	std::function<void(size_t)> replace_match;

	bool HasReplaceAction() const { return static_cast<bool>(replace_match); }
	int RowHeight() const { return FromDIP(24); }
	int Padding() const { return FromDIP(6); }
	int IconSize() const { return FromDIP(16); }
	int IconX() const { return GetClientSize().x - Padding() - IconSize(); }
	int TextMaxX() const { return HasReplaceAction() ? IconX() - Padding() : GetClientSize().x - Padding(); }

	wxRect IconRect(int row) const {
		return wxRect(IconX(), row * RowHeight() + (RowHeight() - IconSize()) / 2, IconSize(), IconSize());
	}

	bool IsOnReplaceIcon(wxPoint pos, int row) const {
		if (!HasReplaceAction())
			return false;

		int x, y;
		CalcUnscrolledPosition(pos.x, pos.y, &x, &y);
		return IconRect(row).Contains(wxPoint(x, y));
	}

	int RowFromPoint(wxPoint pos) const {
		int x, y;
		CalcUnscrolledPosition(pos.x, pos.y, &x, &y);
		int row = y / RowHeight();
		if (row < 0 || static_cast<size_t>(row) >= matches.size())
			return -1;
		return row;
	}

	void DrawReplaceIcon(wxDC& dc, wxRect rect, wxColour colour) {
		auto replace_icon = make_replace_icon(colour);
		dc.DrawBitmap(replace_icon.GetBitmap(wxSize(rect.width, rect.height)), rect.x, rect.y, true);
	}

	int DrawTextSegment(wxDC& dc, wxString const& text, int x, int y, int max_x, wxColour colour, wxColour background, bool strike) {
		if (text.empty() || x >= max_x)
			return x;

		wxDCClipper clip(dc, x, y, max_x - x, RowHeight());
		auto extent = dc.GetTextExtent(text);
		if (background.IsOk()) {
			dc.SetBrush(wxBrush(background));
			dc.SetPen(wxPen(background));
			dc.DrawRectangle(x, y, extent.x, extent.y);
		}
		dc.SetTextForeground(colour);
		dc.DrawText(text, x, y);
		if (strike) {
			dc.SetPen(wxPen(colour, 1));
			dc.DrawLine(x, y + extent.y / 2, x + extent.x, y + extent.y / 2);
		}
		return x + extent.x;
	}

	void DrawPreviewLine(wxDC& dc, std::vector<PreviewSegment> const& segments, int y, int max_x) {
		auto normal = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
		auto red = wxColour(200, 0, 0);
		auto red_bg = wxColour(255, 220, 220);
		auto green = wxColour(0, 120, 0);
		auto green_bg = wxColour(220, 255, 220);
		auto orange_bg = wxColour(255, 225, 160);

		int x = Padding() * 2 + dc.GetTextExtent("0000").x + Padding() * 2;
		for (auto const& segment : segments) {
			switch (segment.type) {
				case PreviewSegmentType::Normal:
					x = DrawTextSegment(dc, segment.text, x, y, max_x, normal, wxNullColour, false);
					break;
				case PreviewSegmentType::OldMatch:
					if (HasReplaceAction())
						x = DrawTextSegment(dc, segment.text, x, y, max_x, red, red_bg, true);
					else
						x = DrawTextSegment(dc, segment.text, x, y, max_x, normal, orange_bg, false);
					break;
				case PreviewSegmentType::Replacement:
					x = DrawTextSegment(dc, segment.text, x, y, max_x, green, green_bg, false);
					break;
			}
		}
	}
	void OnPaint(wxPaintEvent&) {
		wxAutoBufferedPaintDC dc(this);
		PrepareDC(dc);
		dc.SetBackground(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW)));
		dc.Clear();
		dc.SetFont(GetFont());

		auto client = GetClientSize();
		for (int row = 0; row < static_cast<int>(matches.size()); ++row) {
			int y = row * RowHeight();
			bool is_selected = row == selected;
			wxColour base = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
			wxColour alt = wxColour(
				std::max(0, (int)base.Red()   - 10),
				std::max(0, (int)base.Green() - 10),
				std::max(0, (int)base.Blue()  - 10)
			);
			auto bg = is_selected ? wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT) : (row % 2 == 0 ? base : alt);
			auto fg = is_selected ? wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT) : wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);

			dc.SetPen(*wxTRANSPARENT_PEN);
			dc.SetBrush(wxBrush(bg));
			dc.DrawRectangle(0, y, client.x, RowHeight());
			dc.SetPen(wxPen(wxSystemSettings::GetColour(wxSYS_COLOUR_3DLIGHT)));
			dc.DrawLine(0, y + RowHeight() - 1, client.x, y + RowHeight() - 1);

			auto const& match = matches[row];
			auto segments = make_segments(match, HasReplaceAction());
			dc.SetTextForeground(fg);
			int text_y = y + (RowHeight() - dc.GetCharHeight()) / 2;
			dc.DrawText(wxString::Format("%d", match.line_number), Padding(), text_y);
			DrawPreviewLine(dc, segments, text_y, TextMaxX());
			if (HasReplaceAction())
				DrawReplaceIcon(dc, IconRect(row), fg);
		}
	}

	void OnLeftDown(wxMouseEvent& event) {
		SetFocus();
		int row = RowFromPoint(event.GetPosition());
		if (row == -1)
			return;

		if (IsOnReplaceIcon(event.GetPosition(), row)) {
			replace_match(row);
			return;
		}

		SetSelection(row);
	}

	void OnLeftDClick(wxMouseEvent& event) {
		int row = RowFromPoint(event.GetPosition());
		if (row == -1)
			return;

		if (HasReplaceAction())
			replace_match(row);
		else
			SetSelection(row);
	}

	void OnMotion(wxMouseEvent& event) {
		int row = RowFromPoint(event.GetPosition());
		bool on_replace_icon = row != -1 && IsOnReplaceIcon(event.GetPosition(), row);
		SetCursor(on_replace_icon ? wxCursor(wxCURSOR_HAND) : wxNullCursor);

		if (on_replace_icon) {
			auto tooltip = _("Replace selected");
			if (tooltip != GetToolTipText())
				SetToolTip(tooltip);
		}
		else {
			UnsetToolTip();
		}
	}

	void OnLeaveWindow(wxMouseEvent&) {
		SetCursor(wxNullCursor);
		UnsetToolTip();
	}

public:
	SearchReplacePreview(wxWindow *parent, std::function<void(size_t)> select_match, std::function<void(size_t)> replace_match)
	: wxScrolledWindow(parent, -1, wxDefaultPosition, wxSize(1, 220), wxBORDER_THEME | wxVSCROLL)
	, select_match(std::move(select_match))
	, replace_match(std::move(replace_match))
	{
		SetBackgroundStyle(wxBG_STYLE_PAINT);
		SetScrollRate(0, RowHeight());
		Bind(wxEVT_PAINT, &SearchReplacePreview::OnPaint, this);
		Bind(wxEVT_LEFT_DOWN, &SearchReplacePreview::OnLeftDown, this);
		Bind(wxEVT_LEFT_DCLICK, &SearchReplacePreview::OnLeftDClick, this);
		Bind(wxEVT_MOTION, &SearchReplacePreview::OnMotion, this);
		Bind(wxEVT_LEAVE_WINDOW, &SearchReplacePreview::OnLeaveWindow, this);
		Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
			SetVirtualSize(GetClientSize().x, RowHeight() * matches.size());
			event.Skip();
		});
	}

	void SetMatches(std::vector<SearchReplaceMatch> new_matches) {
		matches = std::move(new_matches);
		selected = matches.empty() ? -1 : std::min(selected, static_cast<int>(matches.size()) - 1);
		SetVirtualSize(GetClientSize().x, RowHeight() * matches.size());
		Refresh();
	}

	int GetSelection() const {
		return selected;
	}

	void SetSelection(int row) {
		if (row < 0 || static_cast<size_t>(row) >= matches.size())
			return;

		selected = row;

		int yScrollUnit = 0;
		GetViewStart(nullptr, &yScrollUnit);
		int yPos = yScrollUnit * RowHeight();
		int h = GetClientSize().y;
		int row_top = row * RowHeight();
		int row_bottom = (row + 1) * RowHeight();

		if (row_top < yPos) {
			Scroll(0, row_top / RowHeight());
		}
		else if (row_bottom > yPos + h) {
			Scroll(0, std::max(0, row_bottom - h) / RowHeight());
		}

		Refresh();
		select_match(row);
	}
	size_t GetItemCount() const {
		return matches.size();
	}
};

template<bool has_replace>
DialogSearchReplace<has_replace>::DialogSearchReplace(agi::Context* c)
: wxDialog(c->parent, -1, has_replace ? _("Replace") : _("Find"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
, c(c)
, settings(std::make_unique<SearchReplaceSettings>())
, preview_timer(this)
{
	auto recent_find(lagi_MRU_wxAS("Find"));
	auto recent_replace(lagi_MRU_wxAS("Replace"));

	settings->field = static_cast<SearchReplaceSettings::Field>(OPT_GET("Tool/Search Replace/Field")->GetInt());
	settings->limit_to = static_cast<SearchReplaceSettings::Limit>(OPT_GET("Tool/Search Replace/Affect")->GetInt());
	settings->find = recent_find.empty() ? std::string() : from_wx(recent_find.front());
	settings->replace_with = has_replace && !recent_replace.empty() ? from_wx(recent_replace.front()) : std::string();
	settings->match_case = OPT_GET("Tool/Search Replace/Match Case")->GetBool();
	settings->use_regex = OPT_GET("Tool/Search Replace/RegExp")->GetBool();
	settings->ignore_comments = OPT_GET("Tool/Search Replace/Skip Comments")->GetBool();
	settings->skip_tags = OPT_GET("Tool/Search Replace/Skip Tags")->GetBool();
	settings->exact_match = false;
	if (has_replace && settings->field == SearchReplaceSettings::Field::ORIGINAL)
		settings->field = SearchReplaceSettings::Field::TEXT;

	auto schedule_preview = [this](wxCommandEvent& event) {
		SchedulePreviewUpdate();
		event.Skip();
	};

	auto find_sizer = new wxFlexGridSizer(2, 2, 5, 15);
	find_sizer->AddGrowableCol(1, 1);
	find_edit = new wxComboBox(this, -1, "", wxDefaultPosition, wxSize(300, -1), recent_find, wxCB_DROPDOWN | wxTE_PROCESS_ENTER, StringBinder(&settings->find));
	find_edit->SetMaxLength(0);
	find_sizer->Add(new wxStaticText(this, -1, _("Find what:")), wxSizerFlags().Center().Left());
	find_sizer->Add(find_edit, wxSizerFlags(1).Expand());

	if (has_replace) {
		replace_edit = new wxComboBox(this, -1, "", wxDefaultPosition, wxSize(300, -1), lagi_MRU_wxAS("Replace"), wxCB_DROPDOWN | wxTE_PROCESS_ENTER, StringBinder(&settings->replace_with));
		replace_edit->SetMaxLength(0);
		find_sizer->Add(new wxStaticText(this, -1, _("Replace with:")), wxSizerFlags().Center().Left());
		find_sizer->Add(replace_edit, wxSizerFlags(1).Expand());
	}

	auto options_sizer = new wxBoxSizer(wxVERTICAL);
	options_sizer->Add(new wxCheckBox(this, -1, _("&Match case"), wxDefaultPosition, wxDefaultSize, 0, wxGenericValidator(&settings->match_case)), wxSizerFlags().Border(wxBOTTOM));
	options_sizer->Add(new wxCheckBox(this, -1, _("&Use regular expressions"), wxDefaultPosition, wxDefaultSize, 0, wxGenericValidator(&settings->use_regex)), wxSizerFlags().Border(wxBOTTOM));
	options_sizer->Add(new wxCheckBox(this, -1, _("&Skip Comments"), wxDefaultPosition, wxDefaultSize, 0, wxGenericValidator(&settings->ignore_comments)), wxSizerFlags().Border(wxBOTTOM));
	options_sizer->Add(new wxCheckBox(this, -1, _("S&kip Override Tags"), wxDefaultPosition, wxDefaultSize, 0, wxGenericValidator(&settings->skip_tags)));
	auto left_sizer = new wxBoxSizer(wxVERTICAL);
	left_sizer->Add(find_sizer, wxSizerFlags().Expand().DoubleBorder(wxBOTTOM));
	left_sizer->Add(options_sizer);

	std::vector<std::pair<wxString, SearchReplaceSettings::Field>> field = {
		{ _("&Text"), SearchReplaceSettings::Field::TEXT },
		{ _("St&yle"), SearchReplaceSettings::Field::STYLE },
		{ _("A&ctor"), SearchReplaceSettings::Field::ACTOR },
		{ _("&Effect"), SearchReplaceSettings::Field::EFFECT }
	};
	if (!has_replace)
		field.emplace_back(_("&Original"), SearchReplaceSettings::Field::ORIGINAL);
	std::vector<std::pair<wxString, SearchReplaceSettings::Limit>> affect = {
		{ _("A&ll rows"), SearchReplaceSettings::Limit::ALL },
		{ _("Selected &rows"), SearchReplaceSettings::Limit::SELECTED }
	};
	auto limit_sizer = new wxBoxSizer(wxHORIZONTAL);
	limit_sizer->Add(make_radio_group(this, _("In Field"), field, &settings->field, schedule_preview), wxSizerFlags().Border(wxRIGHT));
	limit_sizer->Add(make_radio_group(this, _("Limit to"), affect, &settings->limit_to, schedule_preview));

#ifdef __WXMAC__
	// wxOSX turns each button's '&' mnemonic into an NSButton keyEquivalent
	// with the Cmd modifier (see wxButtonCocoaImpl::SetAcceleratorFromLabel),
	// which steals Cmd+A / Cmd+N / Cmd+F from the text fields. Strip the
	// which steals Cmd+A / Cmd+N / Cmd+F from the text fields. Strip the
	// mnemonics before passing the label to wxButton.
	auto find_next = new wxButton(this, -1, wxControl::RemoveMnemonics(_("&Find next")));
	auto replace_next = new wxButton(this, -1, wxControl::RemoveMnemonics(_("Replace &next")));
	auto replace_all = new wxButton(this, -1, wxControl::RemoveMnemonics(_("Replace &all")));
#else
	auto find_next = new wxButton(this, -1, _("&Find next"));
	auto replace_next = new wxButton(this, -1, _("Replace &next"));
	auto replace_all = new wxButton(this, -1, _("Replace &all"));
#endif
	find_next->SetDefault();

	auto button_sizer = new wxBoxSizer(wxVERTICAL);
	button_sizer->Add(find_next, wxSizerFlags().Expand().Border(wxBOTTOM));
	button_sizer->Add(replace_next, wxSizerFlags().Expand().Border(wxBOTTOM));
	button_sizer->Add(replace_all, wxSizerFlags().Expand().Border(wxBOTTOM));
	button_sizer->Add(new wxButton(this, wxID_CANCEL), wxSizerFlags().Expand());

	if (!has_replace) {
		button_sizer->Hide(replace_next);
		button_sizer->Hide(replace_all);
	}

	auto top_sizer = new wxBoxSizer(wxHORIZONTAL);
	top_sizer->Add(left_sizer, wxSizerFlags(1).Expand().Border());
	top_sizer->Add(button_sizer, wxSizerFlags().Expand().Border());

	auto main_sizer = new wxBoxSizer(wxVERTICAL);
	main_sizer->Add(top_sizer, wxSizerFlags().Expand());
	main_sizer->Add(limit_sizer, wxSizerFlags().Expand().Border());

	preview_list = new SearchReplacePreview(
		this,
		[this](size_t index) { GoToPreviewSelection(index); },
		has_replace ? std::function<void(size_t)>([this](size_t index) { ReplacePreviewMatch(index); }) : std::function<void(size_t)>());
	main_sizer->Add(preview_list, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));

	SetSizerAndFit(main_sizer);
	SetMinSize(GetSize());
	CenterOnParent();

	TransferDataToWindow();
	find_edit->SetFocus();
	find_edit->SelectAll();

	find_edit->Bind(wxEVT_TEXT_ENTER, std::bind(&DialogSearchReplace::FindReplace, this, &SearchReplaceEngine::FindNext));
	if (has_replace)
		replace_edit->Bind(wxEVT_TEXT_ENTER, std::bind(&DialogSearchReplace::FindReplace, this, &SearchReplaceEngine::ReplaceNext));
	find_next->Bind(wxEVT_BUTTON, std::bind(&DialogSearchReplace::FindReplace, this, &SearchReplaceEngine::FindNext));
	replace_next->Bind(wxEVT_BUTTON, std::bind(&DialogSearchReplace::FindReplace, this, &SearchReplaceEngine::ReplaceNext));
	replace_all->Bind(wxEVT_BUTTON, std::bind(&DialogSearchReplace::FindReplace, this, &SearchReplaceEngine::ReplaceAll));

	Bind(wxEVT_TIMER, [this](wxTimerEvent&) { UpdatePreviewIfNeeded(); }, preview_timer.GetId());
	Bind(wxEVT_TEXT, schedule_preview, find_edit->GetId());
	Bind(wxEVT_COMBOBOX, schedule_preview, find_edit->GetId());
	if (has_replace) {
		Bind(wxEVT_TEXT, schedule_preview, replace_edit->GetId());
		Bind(wxEVT_COMBOBOX, schedule_preview, replace_edit->GetId());
	}
	Bind(wxEVT_CHECKBOX, schedule_preview);
	UpdatePreview();
	preview_timer.Start(150);
}

template<bool has_replace>
void DialogSearchReplace<has_replace>::FindReplace(bool (SearchReplaceEngine::*func)()) {
	SyncSettingsFromControls();

	if (settings->find.empty())
		return;

	c->search->Configure(*settings);
	try {
		if (!((*c->search).*func)())
			wxBell();
	}
	catch (std::exception const& e) {
		wxMessageBox(to_wx(e.what()), _("Error"), wxOK | wxICON_ERROR | wxCENTER, this);
		return;
	}

	SaveSettings();
	UpdateDropDowns();
	UpdatePreview();
}

template<bool has_replace>
void DialogSearchReplace<has_replace>::SaveSettings() {
	config::mru->Add("Find", settings->find);
	if (has_replace)
		config::mru->Add("Replace", settings->replace_with);

	OPT_SET("Tool/Search Replace/Match Case")->SetBool(settings->match_case);
	OPT_SET("Tool/Search Replace/RegExp")->SetBool(settings->use_regex);
	OPT_SET("Tool/Search Replace/Skip Comments")->SetBool(settings->ignore_comments);
	OPT_SET("Tool/Search Replace/Skip Tags")->SetBool(settings->skip_tags);
	OPT_SET("Tool/Search Replace/Field")->SetInt(static_cast<int>(settings->field));
	OPT_SET("Tool/Search Replace/Affect")->SetInt(static_cast<int>(settings->limit_to));
}

static void update_mru(wxComboBox *cb, const char *mru_name) {
	cb->Freeze();
	cb->Clear();
	cb->Append(lagi_MRU_wxAS(mru_name));
	if (!cb->IsListEmpty())
		cb->SetSelection(0);
	cb->Thaw();
}

template<bool has_replace>
void DialogSearchReplace<has_replace>::UpdateDropDowns() {
	update_mru(find_edit, "Find");

	if (has_replace)
		update_mru(replace_edit, "Replace");
}

template<bool has_replace>
void DialogSearchReplace<has_replace>::SchedulePreviewUpdate() {
	if (!preview_list)
		return;

	preview_dirty = true;
	if (!preview_timer.IsRunning())
		preview_timer.Start(150);
}

template<bool has_replace>
void DialogSearchReplace<has_replace>::SyncSettingsFromControls() {
	TransferDataFromWindow();
	settings->find = from_wx(find_edit->GetValue());
	if (has_replace && replace_edit)
		settings->replace_with = from_wx(replace_edit->GetValue());
}

template<bool has_replace>
std::string DialogSearchReplace<has_replace>::GetPreviewKey() const {
	std::string key;
	key.reserve(settings->find.size() + settings->replace_with.size() + 32);
	key += settings->find;
	key.push_back('\0');
	key += settings->replace_with;
	key.push_back('\0');
	key += std::to_string(static_cast<int>(settings->field));
	key.push_back('\0');
	key += std::to_string(static_cast<int>(settings->limit_to));
	key.push_back('\0');
	key += settings->match_case ? '1' : '0';
	key += settings->use_regex ? '1' : '0';
	key += settings->ignore_comments ? '1' : '0';
	key += settings->skip_tags ? '1' : '0';
	return key;
}

template<bool has_replace>
void DialogSearchReplace<has_replace>::UpdatePreviewIfNeeded() {
	if (!preview_list)
		return;

	SyncSettingsFromControls();
	auto key = GetPreviewKey();
	if (!preview_dirty && key == last_preview_key)
		return;

	UpdatePreview();
}

template<bool has_replace>
void DialogSearchReplace<has_replace>::UpdatePreview() {
	if (!preview_list)
		return;

	SyncSettingsFromControls();
	last_preview_key = GetPreviewKey();
	preview_dirty = false;
	preview_matches.clear();
	preview_list->SetMatches({});

	if (settings->find.empty())
		return;

	c->search->Configure(*settings);
	try {
		preview_matches = c->search->GetMatches();
	}
	catch (std::exception const& e) {
		(void)e;
		return;
	}

	preview_list->SetMatches(preview_matches);
}
template<bool has_replace>
void DialogSearchReplace<has_replace>::GoToPreviewSelection(size_t index) {
	if (index >= preview_matches.size())
		return;

	auto const& match = preview_matches[index];
	c->selectionController->SetSelectionAndActive({ match.line }, match.line);

	if (settings->field == SearchReplaceSettings::Field::TEXT) {
		c->textSelectionController->SetSelection(match.start, match.end);
	}
}

template<bool has_replace>
void DialogSearchReplace<has_replace>::ReplacePreviewMatch(size_t index) {
	if (!has_replace || !preview_list)
		return;

	if (index >= preview_matches.size())
		return;

	SyncSettingsFromControls();
	c->search->Configure(*settings);
	try {
		if (!c->search->ReplaceMatch(preview_matches[index])) {
			wxMessageBox(_("The selected match is no longer available."), _("Replace"), wxOK | wxICON_INFORMATION | wxCENTER, this);
			UpdatePreview();
			return;
		}
	}
	catch (std::exception const& e) {
		wxMessageBox(to_wx(e.what()), _("Error"), wxOK | wxICON_ERROR | wxCENTER, this);
		return;
	}

	SaveSettings();
	UpdateDropDowns();

	int xPos = 0, yPos = 0;
	preview_list->GetViewStart(&xPos, &yPos);

	preview_list->SetSelection(-1);

	UpdatePreview();

	preview_list->Scroll(xPos, yPos);
}
template<bool replace>
void ShowSearchReplaceDialog(agi::Context *context) {
	auto other = context->dialog->Get<DialogSearchReplace<!replace>>();
	if (other != nullptr) {
		other->Close();
	}

	context->dialog->Show<DialogSearchReplace<replace>>(context);
	auto dialog = context->dialog->Get<DialogSearchReplace<replace>>();

	dialog->find_edit->SetFocus();
	dialog->find_edit->SelectAll();
	dialog->Raise();
}

void ShowSearchReplaceDialog(agi::Context *context, bool replace) {
	if (replace) {
		ShowSearchReplaceDialog<true>(context);
	} else {
		ShowSearchReplaceDialog<false>(context);
	}
}
