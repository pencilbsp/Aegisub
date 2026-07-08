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
#include <wx/radiobox.h>
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

static std::vector<PreviewSegment> make_segments(SearchReplaceMatch const& match) {
	std::vector<PreviewSegment> segments;
	if (match.start > 0) {
		segments.push_back({PreviewSegmentType::Normal, to_wx(match.current_text.substr(0, match.start))});
	}
	segments.push_back({PreviewSegmentType::OldMatch, to_wx(match.matched_text)});
	if (!match.replacement_match.empty()) {
		segments.push_back({PreviewSegmentType::Replacement, to_wx(match.replacement_match)});
	}
	if (match.end < match.current_text.size()) {
		segments.push_back({PreviewSegmentType::Normal, to_wx(match.current_text.substr(match.end))});
	}
	return segments;
}











class SearchReplacePreview final : public wxScrolledWindow {
	std::vector<SearchReplaceMatch> matches;
	int selected = -1;
	std::function<void(size_t)> select_match;
	std::function<void(size_t)> replace_match;
	wxBitmapBundle replace_icon;

	int RowHeight() const { return FromDIP(24); }
	int Padding() const { return FromDIP(6); }
	int IconSize() const { return FromDIP(16); }
	int IconX() const { return GetClientSize().x - Padding() - IconSize(); }

	wxRect IconRect(int row) const {
		return wxRect(IconX(), row * RowHeight() + (RowHeight() - IconSize()) / 2, IconSize(), IconSize());
	}

	bool IsOnReplaceIcon(wxPoint pos, int row) const {
		return IconRect(row).Contains(pos);
	}

	int RowFromPoint(wxPoint pos) const {
		int x, y;
		CalcUnscrolledPosition(pos.x, pos.y, &x, &y);
		int row = y / RowHeight();
		if (row < 0 || static_cast<size_t>(row) >= matches.size())
			return -1;
		return row;
	}

	void DrawReplaceIcon(wxDC& dc, wxRect rect) {
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

		int x = Padding() * 2 + dc.GetTextExtent("0000").x + Padding() * 2;
		for (auto const& segment : segments) {
			switch (segment.type) {
				case PreviewSegmentType::Normal:
					x = DrawTextSegment(dc, segment.text, x, y, max_x, normal, wxNullColour, false);
					break;
				case PreviewSegmentType::OldMatch:
					x = DrawTextSegment(dc, segment.text, x, y, max_x, red, red_bg, true);
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
			auto segments = make_segments(match);
			dc.SetTextForeground(fg);
			int text_y = y + (RowHeight() - dc.GetCharHeight()) / 2;
			dc.DrawText(wxString::Format("%d", match.line_number), Padding(), text_y);
			int max_x = IconX() - Padding();
			DrawPreviewLine(dc, segments, text_y, max_x);
			DrawReplaceIcon(dc, IconRect(row));
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
		if (row != -1)
			replace_match(row);
	}

	void OnMotion(wxMouseEvent& event) {
		int row = RowFromPoint(event.GetPosition());
		SetCursor(row != -1 && IsOnReplaceIcon(event.GetPosition(), row) ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
	}

public:
	SearchReplacePreview(wxWindow *parent, std::function<void(size_t)> select_match, std::function<void(size_t)> replace_match)
	: wxScrolledWindow(parent, -1, wxDefaultPosition, wxSize(720, 220), wxBORDER_THEME | wxVSCROLL)
	, select_match(std::move(select_match))
	, replace_match(std::move(replace_match))
	, replace_icon(GETBUNDLE(button_next, 16))
	{
		SetBackgroundStyle(wxBG_STYLE_PAINT);
		SetScrollRate(0, RowHeight());
		Bind(wxEVT_PAINT, &SearchReplacePreview::OnPaint, this);
		Bind(wxEVT_LEFT_DOWN, &SearchReplacePreview::OnLeftDown, this);
		Bind(wxEVT_LEFT_DCLICK, &SearchReplacePreview::OnLeftDClick, this);
		Bind(wxEVT_MOTION, &SearchReplacePreview::OnMotion, this);
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
	settings->replace_with = recent_replace.empty() ? std::string() : from_wx(recent_replace.front());
	settings->match_case = OPT_GET("Tool/Search Replace/Match Case")->GetBool();
	settings->use_regex = OPT_GET("Tool/Search Replace/RegExp")->GetBool();
	settings->ignore_comments = OPT_GET("Tool/Search Replace/Skip Comments")->GetBool();
	settings->skip_tags = OPT_GET("Tool/Search Replace/Skip Tags")->GetBool();
	settings->exact_match = false;

	auto find_sizer = new wxFlexGridSizer(2, 2, 5, 15);
	find_edit = new wxComboBox(this, -1, "", wxDefaultPosition, wxSize(300, -1), recent_find, wxCB_DROPDOWN | wxTE_PROCESS_ENTER, StringBinder(&settings->find));
	find_edit->SetMaxLength(0);
	find_sizer->Add(new wxStaticText(this, -1, _("Find what:")), wxSizerFlags().Center().Left());
	find_sizer->Add(find_edit);

	if (has_replace) {
		replace_edit = new wxComboBox(this, -1, "", wxDefaultPosition, wxSize(300, -1), lagi_MRU_wxAS("Replace"), wxCB_DROPDOWN | wxTE_PROCESS_ENTER, StringBinder(&settings->replace_with));
		replace_edit->SetMaxLength(0);
		find_sizer->Add(new wxStaticText(this, -1, _("Replace with:")), wxSizerFlags().Center().Left());
		find_sizer->Add(replace_edit);
	}

	auto options_sizer = new wxBoxSizer(wxVERTICAL);
	options_sizer->Add(new wxCheckBox(this, -1, _("&Match case"), wxDefaultPosition, wxDefaultSize, 0, wxGenericValidator(&settings->match_case)), wxSizerFlags().Border(wxBOTTOM));
	options_sizer->Add(new wxCheckBox(this, -1, _("&Use regular expressions"), wxDefaultPosition, wxDefaultSize, 0, wxGenericValidator(&settings->use_regex)), wxSizerFlags().Border(wxBOTTOM));
	options_sizer->Add(new wxCheckBox(this, -1, _("&Skip Comments"), wxDefaultPosition, wxDefaultSize, 0, wxGenericValidator(&settings->ignore_comments)), wxSizerFlags().Border(wxBOTTOM));
	options_sizer->Add(new wxCheckBox(this, -1, _("S&kip Override Tags"), wxDefaultPosition, wxDefaultSize, 0, wxGenericValidator(&settings->skip_tags)));
	auto left_sizer = new wxBoxSizer(wxVERTICAL);
	left_sizer->Add(find_sizer, wxSizerFlags().DoubleBorder(wxBOTTOM));
	left_sizer->Add(options_sizer);

	wxString field[] = { _("&Text"), _("St&yle"), _("A&ctor"), _("&Effect") };
	wxString affect[] = { _("A&ll rows"), _("Selected &rows") };
	auto limit_sizer = new wxBoxSizer(wxHORIZONTAL);
	limit_sizer->Add(new wxRadioBox(this, -1, _("In Field"), wxDefaultPosition, wxDefaultSize, std::size(field), field, 0, wxRA_SPECIFY_COLS, MakeEnumBinder(&settings->field)), wxSizerFlags().Border(wxRIGHT));
	limit_sizer->Add(new wxRadioBox(this, -1, _("Limit to"), wxDefaultPosition, wxDefaultSize, std::size(affect), affect, 0, wxRA_SPECIFY_COLS, MakeEnumBinder(&settings->limit_to)));

#ifdef __WXMAC__
	// wxOSX turns each button's '&' mnemonic into an NSButton keyEquivalent
	// with the Cmd modifier (see wxButtonCocoaImpl::SetAcceleratorFromLabel),
	// which steals Cmd+A / Cmd+N / Cmd+F from the text fields. Strip the
	// which steals Cmd+A / Cmd+N / Cmd+F from the text fields. Strip the
	// mnemonics before passing the label to wxButton.
	auto find_next = new wxButton(this, -1, wxControl::RemoveMnemonics(_("&Find next")));
	auto replace_next = new wxButton(this, -1, wxControl::RemoveMnemonics(_("Replace &next")));
	auto replace_all = new wxButton(this, -1, wxControl::RemoveMnemonics(_("Replace &all")));
	replace_selected = new wxButton(this, -1, wxControl::RemoveMnemonics(_("Replace selected")));
#else
	auto find_next = new wxButton(this, -1, _("&Find next"));
	auto replace_next = new wxButton(this, -1, _("Replace &next"));
	auto replace_all = new wxButton(this, -1, _("Replace &all"));
	replace_selected = new wxButton(this, -1, _("Replace selected"));
#endif
	find_next->SetDefault();

	auto button_sizer = new wxBoxSizer(wxVERTICAL);
	button_sizer->Add(find_next, wxSizerFlags().Border(wxBOTTOM));
	button_sizer->Add(replace_next, wxSizerFlags().Border(wxBOTTOM));
	button_sizer->Add(replace_all, wxSizerFlags().Border(wxBOTTOM));
	button_sizer->Add(replace_selected, wxSizerFlags().Border(wxBOTTOM));
	button_sizer->Add(new wxButton(this, wxID_CANCEL));

	if (!has_replace) {
		button_sizer->Hide(replace_next);
		button_sizer->Hide(replace_all);
		button_sizer->Hide(replace_selected);
	}

	auto top_sizer = new wxBoxSizer(wxHORIZONTAL);
	top_sizer->Add(left_sizer, wxSizerFlags().Border());
	top_sizer->Add(button_sizer, wxSizerFlags().Border());

	auto main_sizer = new wxBoxSizer(wxVERTICAL);
	main_sizer->Add(top_sizer);
	main_sizer->Add(limit_sizer, wxSizerFlags().Border());

	if (has_replace) {
		preview_list = new SearchReplacePreview(
			this,
			[this](size_t index) { GoToPreviewSelection(index); },
			[this](size_t index) { ReplacePreviewMatch(index); });
		main_sizer->Add(preview_list, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		replace_selected->Enable(false);
	}

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
	replace_selected->Bind(wxEVT_BUTTON, std::bind(&DialogSearchReplace::ReplaceSelected, this));

	if (has_replace) {
		Bind(wxEVT_TIMER, [this](wxTimerEvent&) { UpdatePreviewIfNeeded(); }, preview_timer.GetId());
		auto schedule_preview = [this](wxCommandEvent& event) {
			SchedulePreviewUpdate();
			event.Skip();
		};
		Bind(wxEVT_TEXT, schedule_preview, find_edit->GetId());
		Bind(wxEVT_COMBOBOX, schedule_preview, find_edit->GetId());
		Bind(wxEVT_TEXT, schedule_preview, replace_edit->GetId());
		Bind(wxEVT_COMBOBOX, schedule_preview, replace_edit->GetId());
		Bind(wxEVT_CHECKBOX, schedule_preview);
		Bind(wxEVT_RADIOBOX, schedule_preview);
		UpdatePreview();
		preview_timer.Start(150);
	}
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
	if (!has_replace || !preview_list)
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
	if (!has_replace || !preview_list)
		return;

	SyncSettingsFromControls();
	auto key = GetPreviewKey();
	if (!preview_dirty && key == last_preview_key)
		return;

	UpdatePreview();
}

template<bool has_replace>
void DialogSearchReplace<has_replace>::UpdatePreview() {
	if (!has_replace || !preview_list)
		return;

	SyncSettingsFromControls();
	last_preview_key = GetPreviewKey();
	preview_dirty = false;
	preview_matches.clear();
	preview_list->SetMatches({});
	replace_selected->Enable(false);

	if (settings->find.empty())
		return;

	c->search->Configure(*settings);
	c->search->Configure(*settings);
	try {
		preview_matches = c->search->GetMatches();
	}
	catch (std::exception const& e) {
		(void)e;
		return;
	}

	preview_list->SetMatches(preview_matches);
	replace_selected->Enable(preview_list->GetSelection() != -1);
}
template<bool has_replace>
void DialogSearchReplace<has_replace>::GoToPreviewSelection(size_t index) {
	replace_selected->Enable(true);
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



template<bool has_replace>
void DialogSearchReplace<has_replace>::ReplaceSelected() {
	if (!has_replace || !preview_list)
		return;

	int selected = preview_list->GetSelection();
	if (selected == -1)
		return;

	ReplacePreviewMatch(selected);
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

