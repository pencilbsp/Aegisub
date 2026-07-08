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

/// @file dialog_search_replace.h
/// @see dialog_search_replace.cpp
/// @ingroup secondary_ui
///

#include <libaegisub/signal.h>
#include <memory>
#include <vector>

#include <wx/dialog.h>
#include <wx/timer.h>

#include "search_replace_engine.h"

namespace agi { struct Context; }
class SearchReplaceEngine;
struct SearchReplaceSettings;
class wxComboBox;
class SearchReplacePreview;

template<bool has_replace>
class DialogSearchReplace final : public wxDialog {
	agi::Context *c;
	std::unique_ptr<SearchReplaceSettings> settings;
	wxComboBox *replace_edit;
	SearchReplacePreview *preview_list = nullptr;
	std::vector<SearchReplaceMatch> preview_matches;
	wxTimer preview_timer;
	std::string last_preview_key;
	bool preview_dirty = true;
	agi::signal::Connection file_changed_slot;

	void UpdateDropDowns();
	void FindReplace(bool (SearchReplaceEngine::*func)());
	void SchedulePreviewUpdate();
	void UpdatePreview();
	void UpdatePreviewIfNeeded();
	void ReplacePreviewMatch(size_t index);
	void GoToPreviewSelection(size_t index);
	void SaveSettings();
	void SyncSettingsFromControls();
	std::string GetPreviewKey() const;
	void OnCommit(int type);

public:
	wxComboBox *find_edit;
	DialogSearchReplace(agi::Context* c);
};
