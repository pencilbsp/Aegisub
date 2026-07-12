// Copyright (c) 2005, Rodrigo Braz Monteiro
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

#include <libaegisub/signal.h>

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include <wx/stc/stc.h>
#include <wx/timer.h>

class Thesaurus;
class Glossary;
class GlossaryPopup;
class wxMouseEvent;
namespace agi {
	class SpellChecker;
	struct Context;
	struct GlossaryMatch;
	namespace ass { struct DialogueToken; }
}

/// @class SubsTextEditCtrl
/// @brief A Scintilla control with spell checking and syntax highlighting
class SubsTextEditCtrl final : public wxStyledTextCtrl, private agi::signal::ConnectionScope {
	/// Backend spellchecker to use
	std::unique_ptr<agi::SpellChecker> spellchecker;

	/// Backend thesaurus to use
	std::unique_ptr<Thesaurus> thesaurus;

	/// User glossary, for underlining known terms and showing their notes
	std::unique_ptr<Glossary> glossary;

	/// Glossary term matches for the current line, cached for hover lookup
	std::vector<agi::GlossaryMatch> glossary_matches;

	/// Currently shown glossary note tooltip, if any
	GlossaryPopup *glossary_popup = nullptr;
	int64_t glossary_popup_entry_id = -1;
	size_t glossary_popup_offset = 0;
	size_t glossary_popup_length = 0;

	/// Project context, for splitting lines
	agi::Context *context;

	/// Whether programmatic text updates should update the shared text
	/// selection controller. Disabled for the read-only original text box.
	bool use_context_selection = true;

	/// Whether line-splitting commands should be exposed in the context menu.
	bool show_split_menu = true;
	bool text_styling_refresh_pending = false;

	/// Options prefix for the font face/size to use (e.g. "Subtitle/Edit Box").
	/// Lets a second instance (the read-only original box) have its own font.
	std::string font_opt_prefix;

	/// The word right-clicked on, used for spellchecker replacing
	std::string currentWord;

	/// The beginning of the word right-clicked on, for spellchecker replacing
	std::pair<int, int> currentWordPos;

	/// Spellchecker suggestions for the last right-clicked word
	std::vector<std::string> sugs;

	/// Thesaurus suggestions for the last right-clicked word
	std::vector<std::string> thesSugs;

	/// Text of the currently shown calltip, to avoid flickering from
	/// pointlessly reshowing the current tip
	std::string calltip_text;

	/// Position of the currently show calltip
	size_t calltip_position = 0;

	/// Cursor position which the current calltip is for
	int cursor_pos;

	/// The last seen line text, used to avoid reparsing the line for syntax
	/// highlighting when possible
	std::string line_text;

	/// Tokenized version of line_text
	std::vector<agi::ass::DialogueToken> tokenized_line;

	void OnContextMenu(wxContextMenuEvent &);
	void OnDoubleClick(wxStyledTextEvent&);
	void OnUseSuggestion(wxCommandEvent &event);
	void OnFindSelection(wxCommandEvent &event);
	void OnReplaceSelection(wxCommandEvent &event);
	void OnAddSelectionToGlossary(wxCommandEvent &event);
	void OnSetDicLanguage(wxCommandEvent &event);
	void OnSetThesLanguage(wxCommandEvent &event);
	void OnLoseFocus(wxFocusEvent &event);
	void OnKeyDown(wxKeyEvent &event);

	void SetSyntaxStyle(int id, wxFont &font, std::string const& name, wxColor const& default_background);
	void Subscribe(std::string const& name);

	void StyleSpellCheck();
	void UpdateCallTip();
	void SetStyles();

	void UpdateStyle();
	void RefreshTextStyling();
	void ScheduleTextStylingRefresh();

	/// Re-run glossary matching and apply the underline indicator
	void UpdateGlossary();

	/// Rebuild the active glossary and restyle when glossary options change
	void OnGlossaryOptionChanged();

	/// Mouse hover handler driving the glossary note tooltip
	void OnGlossaryMouseMove(wxMouseEvent &event);
	void HideGlossaryPopup(bool fade = true);
	/// Open the Glossary Entry editor for an entry, then refresh the matches
	void OpenGlossaryEntryEditor(int64_t entry_id);
	/// Hide the popup at once unless the pointer is still over the term, the
	/// popup, or the gap bridging them
	void MaybeDismissGlossaryPopup();
	void CancelGlossaryPopupDismiss();
	/// Whether the pointer is currently within the term/popup/bridge safe zone
	bool GlossaryHoverActive();

	/// The glossary match whose rectangle contains a client point, or nullptr
	const agi::GlossaryMatch *GlossaryMatchAt(wxPoint const& pt);

	/// Client rectangle occupied by a glossary match
	wxRect GlossaryMatchRect(agi::GlossaryMatch const& match);

	/// Add the thesaurus suggestions to a menu
	void AddThesaurusEntries(wxMenu &menu);

	/// Add the spell checker suggestions to a menu
	void AddSpellCheckerEntries(wxMenu &menu);

	/// Generate a languages submenu from a list of locales and a current language
	/// @param base_id ID to use for the first menu item
	/// @param curLang Currently selected language
	/// @param langs Full list of languages
	wxMenu *GetLanguagesMenu(int base_id, wxString const& curLang, wxArrayString const& langs);

public:
	SubsTextEditCtrl(wxWindow* parent, wxSize size, long style, agi::Context *context,
		std::string font_opt_prefix = "Subtitle/Edit Box", bool use_context_selection = true,
		bool show_split_menu = true);
	~SubsTextEditCtrl();

	void SetTextTo(std::string const& text);
	void UpdateVerticalScrollState();
	void Paste() override;

	std::pair<int, int> GetBoundsOfWordAtPosition(int pos);

	DECLARE_EVENT_TABLE()
};
