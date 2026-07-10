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
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "compat.h"
#include "dialogs.h"
#include "format.h"
#include "glossary.h"
#include "options.h"
#include "include/aegisub/context.h"

#include <libaegisub/glossary.h>
#include <libaegisub/cajun/writer.h>
#include <libaegisub/io.h>
#include <libaegisub/json.h>
#include <libaegisub/log.h>
#include <libaegisub/path.h>

#include <algorithm>
#include <cctype>
#include <iterator>

#include <wx/button.h>
#include <wx/dcclient.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>

namespace {
enum class GlossaryFileFormat {
	SQLite,
	JSON,
	CSV,
};

struct GlossaryExportDictionary {
	std::string name;
	std::vector<agi::GlossaryEntry> entries;
};

wxString GlossaryFileWildcard() {
	return _("Aegisub glossary SQLite") + " (*.sqlite;*.agdict)|*.sqlite;*.agdict|" +
		_("Aegisub glossary JSON") + " (*.json)|*.json|" +
		_("Aegisub glossary CSV") + " (*.csv)|*.csv";
}

std::string lower_ascii(std::string str) {
	std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return str;
}

GlossaryFileFormat FormatFromFilter(int filter_index) {
	if (filter_index == 1)
		return GlossaryFileFormat::JSON;
	if (filter_index == 2)
		return GlossaryFileFormat::CSV;
	return GlossaryFileFormat::SQLite;
}

GlossaryFileFormat FormatFromExtension(agi::fs::path const& path, int filter_index) {
	std::string ext = lower_ascii(path.extension().string());
	if (ext == ".json")
		return GlossaryFileFormat::JSON;
	if (ext == ".csv")
		return GlossaryFileFormat::CSV;
	if (ext == ".sqlite" || ext == ".agdict")
		return GlossaryFileFormat::SQLite;

	return FormatFromFilter(filter_index);
}

agi::fs::path ApplyGlossaryExtension(agi::fs::path path, GlossaryFileFormat format) {
	std::string ext = lower_ascii(path.extension().string());
	if ((format == GlossaryFileFormat::SQLite && (ext == ".sqlite" || ext == ".agdict")) ||
		(format == GlossaryFileFormat::JSON && ext == ".json") ||
		(format == GlossaryFileFormat::CSV && ext == ".csv"))
		return path;

	switch (format) {
	case GlossaryFileFormat::SQLite: path.replace_extension(".sqlite"); break;
	case GlossaryFileFormat::JSON: path.replace_extension(".json"); break;
	case GlossaryFileFormat::CSV: path.replace_extension(".csv"); break;
	}
	return path;
}

std::string DictionaryNameFromPath(agi::fs::path const& path) {
	auto name = path.stem().string();
	if (name.empty())
		throw agi::GlossaryError("Glossary file name cannot be empty");
	return name;
}

void ExportSQLite(agi::fs::path const& path, GlossaryExportDictionary const& dict) {
	agi::fs::Remove(path);
	agi::Glossary out(path);
	auto out_dict = out.CreateDictionary(dict.name);
	for (auto entry : dict.entries) {
		entry.id = 0;
		out.UpsertEntry(out_dict, entry);
	}
}

void ExportJSON(agi::fs::path const& path, GlossaryExportDictionary const& dict) {
	json::Object root;
	root.emplace("format", "aegisub-glossary");
	root.emplace("version", 1);

	json::Array entries;
	for (auto const& entry : dict.entries) {
		json::Object out_entry;
		out_entry.emplace("term", entry.term);
		out_entry.emplace("note", entry.note_text);
		out_entry.emplace("url", entry.note_url);
		entries.emplace_back(std::move(out_entry));
	}
	root.emplace("entries", std::move(entries));

	agi::JsonWriter::Write(root, agi::io::Save(path).Get());
}

std::string CsvEscape(std::string const& value) {
	bool quote = value.find_first_of(",\"\r\n") != std::string::npos;
	if (!quote)
		return value;

	std::string out = "\"";
	for (char c : value) {
		if (c == '"')
			out += "\"\"";
		else
			out += c;
	}
	out += '"';
	return out;
}

void ExportCSV(agi::fs::path const& path, GlossaryExportDictionary const& dict) {
	agi::io::Save file(path);
	auto& out = file.Get();
	out << "\xEF\xBB\xBF";
	out << "term,note,url\n";
	for (auto const& entry : dict.entries) {
		out << CsvEscape(entry.term) << ','
			<< CsvEscape(entry.note_text) << ','
			<< CsvEscape(entry.note_url) << '\n';
	}
}

std::string const& JsonString(json::Object const& obj, char const *key) {
	auto it = obj.find(key);
	if (it == obj.end())
		throw agi::GlossaryError(agi::format("Missing JSON field \"%s\"", key));
	return static_cast<std::string const&>(it->second);
}

std::string JsonOptionalString(json::Object const& obj, char const *key) {
	auto it = obj.find(key);
	return it == obj.end() ? std::string() : static_cast<std::string const&>(it->second);
}

void AppendJSONEntries(json::Array const& entries, GlossaryExportDictionary& dict) {
	for (auto const& entry_unknown : entries) {
		auto const& entry_obj = static_cast<json::Object const&>(entry_unknown);
		agi::GlossaryEntry entry;
		entry.term = JsonString(entry_obj, "term");
		entry.note_text = JsonOptionalString(entry_obj, "note");
		entry.note_url = JsonOptionalString(entry_obj, "url");
		dict.entries.emplace_back(std::move(entry));
	}
}

std::vector<GlossaryExportDictionary> ImportJSON(agi::fs::path const& path) {
	auto stream = agi::io::Open(path);
	auto root = agi::json_util::parse(*stream);
	auto const& root_obj = static_cast<json::Object const&>(root);

	if (JsonString(root_obj, "format") != "aegisub-glossary")
		throw agi::GlossaryError("Unsupported glossary JSON format");
	if (static_cast<int64_t const&>(root_obj.at("version")) != 1)
		throw agi::GlossaryError("Unsupported glossary JSON version");

	GlossaryExportDictionary dict;
	dict.name = DictionaryNameFromPath(path);

	if (auto it = root_obj.find("entries"); it != root_obj.end())
		AppendJSONEntries(static_cast<json::Array const&>(it->second), dict);
	else {
		auto const& dictionaries = static_cast<json::Array const&>(root_obj.at("dictionaries"));
		for (auto const& dict_unknown : dictionaries) {
			auto const& dict_obj = static_cast<json::Object const&>(dict_unknown);
			AppendJSONEntries(static_cast<json::Array const&>(dict_obj.at("entries")), dict);
		}
	}

	std::vector<GlossaryExportDictionary> out;
	out.emplace_back(std::move(dict));
	return out;
}

std::vector<std::vector<std::string>> ParseCSV(std::string const& data) {
	std::vector<std::vector<std::string>> rows;
	std::vector<std::string> row;
	std::string field;
	bool quoted = false;

	for (size_t i = 0; i < data.size(); ++i) {
		char c = data[i];
		if (quoted) {
			if (c == '"') {
				if (i + 1 < data.size() && data[i + 1] == '"') {
					field += '"';
					++i;
				}
				else
					quoted = false;
			}
			else
				field += c;
			continue;
		}

		if (c == '"' && field.empty())
			quoted = true;
		else if (c == ',') {
			row.emplace_back(std::move(field));
			field.clear();
		}
		else if (c == '\r' || c == '\n') {
			if (c == '\r' && i + 1 < data.size() && data[i + 1] == '\n')
				++i;
			row.emplace_back(std::move(field));
			field.clear();
			rows.emplace_back(std::move(row));
			row.clear();
		}
		else
			field += c;
	}

	if (quoted)
		throw agi::GlossaryError("Unterminated quoted CSV field");
	if (!field.empty() || !row.empty()) {
		row.emplace_back(std::move(field));
		rows.emplace_back(std::move(row));
	}
	return rows;
}

std::vector<GlossaryExportDictionary> ImportCSV(agi::fs::path const& path) {
	auto stream = agi::io::Open(path);
	std::string data((std::istreambuf_iterator<char>(*stream)), {});
	if (data.starts_with("\xEF\xBB\xBF"))
		data.erase(0, 3);

	auto rows = ParseCSV(data);
	if (rows.empty())
		return {};
	bool old_format = rows[0] == std::vector<std::string>{"dictionary", "term", "note", "url"};
	if (rows[0] != std::vector<std::string>{"term", "note", "url"} && !old_format)
		throw agi::GlossaryError("CSV header must be: term,note,url");

	GlossaryExportDictionary dict;
	dict.name = DictionaryNameFromPath(path);
	for (size_t i = 1; i < rows.size(); ++i) {
		if (rows[i].size() != (old_format ? 4u : 3u))
			throw agi::GlossaryError(agi::format("CSV row %d has the wrong number of columns", static_cast<int>(i + 1)));
		size_t term_col = old_format ? 1 : 0;
		if (rows[i][term_col].empty())
			throw agi::GlossaryError(agi::format("CSV row %d has an empty term", static_cast<int>(i + 1)));

		agi::GlossaryEntry entry;
		entry.term = std::move(rows[i][term_col]);
		entry.note_text = std::move(rows[i][term_col + 1]);
		entry.note_url = std::move(rows[i][term_col + 2]);
		dict.entries.emplace_back(std::move(entry));
	}

	std::vector<GlossaryExportDictionary> out;
	out.emplace_back(std::move(dict));
	return out;
}

std::vector<GlossaryExportDictionary> ImportSQLite(agi::fs::path const& path) {
	agi::Glossary in(path);
	GlossaryExportDictionary dict;
	dict.name = DictionaryNameFromPath(path);
	for (auto const& src : in.ListDictionaries()) {
		auto entries = in.ListEntries(src.first);
		dict.entries.insert(dict.entries.end(), entries.begin(), entries.end());
	}

	std::vector<GlossaryExportDictionary> out;
	out.emplace_back(std::move(dict));
	return out;
}

// Modal editor for a single glossary entry.
struct EntryEditor {
	wxDialog d;
	agi::GlossaryEntry &entry;

	wxTextCtrl *term;
	wxTextCtrl *note;
	wxTextCtrl *url;

	EntryEditor(wxWindow *parent, agi::GlossaryEntry &entry);

	bool Run() {
		if (d.ShowModal() != wxID_OK) return false;
		entry.term = from_wx(term->GetValue());
		entry.note_text = from_wx(note->GetValue());
		entry.note_url = from_wx(url->GetValue());
		return true;
	}
};

EntryEditor::EntryEditor(wxWindow *parent, agi::GlossaryEntry &entry)
: d(parent, -1, _("Glossary Entry"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
, entry(entry)
{
	term = new wxTextCtrl(&d, -1, to_wx(entry.term));
	note = new wxTextCtrl(&d, -1, to_wx(entry.note_text), wxDefaultPosition, wxSize(360, 120), wxTE_MULTILINE);
	url = new wxTextCtrl(&d, -1, to_wx(entry.note_url));

	auto grid = new wxFlexGridSizer(3, 2, 5, 5);
	grid->AddGrowableCol(1);
	grid->Add(new wxStaticText(&d, -1, _("Term/phrase:")), 0, wxALIGN_CENTER_VERTICAL);
	grid->Add(term, 1, wxEXPAND);
	grid->Add(new wxStaticText(&d, -1, _("Note:")), 0, wxALIGN_TOP);
	grid->Add(note, 1, wxEXPAND);
	grid->Add(new wxStaticText(&d, -1, _("URL:")), 0, wxALIGN_CENTER_VERTICAL);
	grid->Add(url, 1, wxEXPAND);

	auto main = new wxBoxSizer(wxVERTICAL);
	main->Add(grid, 1, wxEXPAND | wxALL, 8);
	main->Add(d.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);
	d.SetSizerAndFit(main);
	d.CenterOnParent();
}

struct DialogGlossary {
	wxDialog d;
	std::unique_ptr<agi::Glossary> glossary;

	wxListBox *dict_list;
	wxListView *entry_list;
	wxButton *rename_btn, *delete_btn, *active_btn, *export_btn;
	wxButton *add_entry_btn, *edit_entry_btn, *delete_entry_btn;
	bool note_label_refresh_pending = false;

	std::vector<std::pair<int64_t, std::string>> dicts;
	std::vector<agi::GlossaryEntry> entries;

	DialogGlossary(wxWindow *parent);

	int64_t SelectedDict() const;
	int64_t SelectedEntry() const;
	std::string ActiveName() const {
		if (!OPT_GET("Tool/Glossary/Enabled")->GetBool())
			return {};
		return OPT_GET("Tool/Glossary/Active Dictionary")->GetString();
	}

	void RefreshDicts(int64_t select_id = -1);
	void RefreshEntries();
	void RefreshEntryNoteLabels(bool visible_only = false);
	void QueueRefreshEntryNoteLabels();
	wxString NoteLabel(agi::GlossaryEntry const& entry) const;
	void UpdateEntryColumns();
	void UpdateButtons();
	void NotifyChanged();

	void OnNewDict(wxCommandEvent &);
	void OnRenameDict(wxCommandEvent &);
	void OnDeleteDict(wxCommandEvent &);
	void OnSetActive(wxCommandEvent &);
	void OnExport(wxCommandEvent &);
	void OnImport(wxCommandEvent &);
	void ImportDictionaries(std::vector<GlossaryExportDictionary> const& imported);

	void OnAddEntry(wxCommandEvent &);
	void OnEditEntry(wxCommandEvent &);
	void OnDeleteEntry(wxCommandEvent &);
};

DialogGlossary::DialogGlossary(wxWindow *parent)
: d(parent, -1, _("Glossary Manager"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
	glossary = std::make_unique<agi::Glossary>(config::path->Decode("?user/glossary.db"));

	// Left: dictionaries
	dict_list = new wxListBox(&d, -1, wxDefaultPosition, wxSize(180, 320));
	auto new_btn = new wxButton(&d, -1, _("&New"));
	rename_btn = new wxButton(&d, -1, _("&Rename"));
	delete_btn = new wxButton(&d, -1, _("&Delete"));
	active_btn = new wxButton(&d, -1, _("Set &Active"));
	auto import_btn = new wxButton(&d, -1, _("&Import..."));
	export_btn = new wxButton(&d, -1, _("&Export..."));

	auto dict_btns = new wxBoxSizer(wxHORIZONTAL);
	dict_btns->Add(new_btn, 1);
	dict_btns->Add(rename_btn, 1, wxLEFT, 3);
	dict_btns->Add(delete_btn, 1, wxLEFT, 3);
	auto dict_btns2 = new wxBoxSizer(wxHORIZONTAL);
	dict_btns2->Add(active_btn, 1);
	dict_btns2->Add(import_btn, 1, wxLEFT, 3);
	dict_btns2->Add(export_btn, 1, wxLEFT, 3);

	auto left = new wxBoxSizer(wxVERTICAL);
	left->Add(new wxStaticText(&d, -1, _("Dictionaries:")), 0, wxBOTTOM, 3);
	left->Add(dict_list, 1, wxEXPAND | wxBOTTOM, 5);
	left->Add(dict_btns, 0, wxEXPAND | wxBOTTOM, 3);
	left->Add(dict_btns2, 0, wxEXPAND);

	// Right: entries
	entry_list = new wxListView(&d, -1, wxDefaultPosition, wxSize(440, 320));
	entry_list->InsertColumn(0, _("Term"), wxLIST_FORMAT_LEFT, 140);
	entry_list->InsertColumn(1, _("Note"), wxLIST_FORMAT_LEFT, 200);
	entry_list->InsertColumn(2, _("URL"), wxLIST_FORMAT_CENTER, 40);

	// wxListCtrl doesn't stretch its last column, so grow the Note column to
	// fill whatever width is left over instead of leaving an empty gap.
	entry_list->Bind(wxEVT_SIZE, [this](wxSizeEvent &e) {
		e.Skip();
		UpdateEntryColumns();
		RefreshEntryNoteLabels();
	});

	add_entry_btn = new wxButton(&d, -1, _("A&dd"));
	edit_entry_btn = new wxButton(&d, -1, _("Ed&it"));
	delete_entry_btn = new wxButton(&d, -1, _("Re&move"));

	auto entry_btns = new wxBoxSizer(wxHORIZONTAL);
	entry_btns->Add(add_entry_btn, 1);
	entry_btns->Add(edit_entry_btn, 1, wxLEFT, 3);
	entry_btns->Add(delete_entry_btn, 1, wxLEFT, 3);

	auto right = new wxBoxSizer(wxVERTICAL);
	right->Add(new wxStaticText(&d, -1, _("Terms:")), 0, wxBOTTOM, 3);
	right->Add(entry_list, 1, wxEXPAND | wxBOTTOM, 5);
	right->Add(entry_btns, 0, wxEXPAND);

	auto cols = new wxBoxSizer(wxHORIZONTAL);
	cols->Add(left, 0, wxEXPAND | wxRIGHT, 8);
	cols->Add(right, 1, wxEXPAND);

	auto main = new wxBoxSizer(wxVERTICAL);
	main->Add(cols, 1, wxEXPAND | wxALL, 8);
	main->Add(d.CreateStdDialogButtonSizer(wxCLOSE), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
	d.SetSizerAndFit(main);
	d.SetEscapeId(wxID_CLOSE);
	d.CenterOnParent();

	new_btn->Bind(wxEVT_BUTTON, &DialogGlossary::OnNewDict, this);
	rename_btn->Bind(wxEVT_BUTTON, &DialogGlossary::OnRenameDict, this);
	delete_btn->Bind(wxEVT_BUTTON, &DialogGlossary::OnDeleteDict, this);
	active_btn->Bind(wxEVT_BUTTON, &DialogGlossary::OnSetActive, this);
	import_btn->Bind(wxEVT_BUTTON, &DialogGlossary::OnImport, this);
	export_btn->Bind(wxEVT_BUTTON, &DialogGlossary::OnExport, this);
	add_entry_btn->Bind(wxEVT_BUTTON, &DialogGlossary::OnAddEntry, this);
	edit_entry_btn->Bind(wxEVT_BUTTON, &DialogGlossary::OnEditEntry, this);
	delete_entry_btn->Bind(wxEVT_BUTTON, &DialogGlossary::OnDeleteEntry, this);

	dict_list->Bind(wxEVT_LISTBOX, [this](wxCommandEvent &) { RefreshEntries(); });
	entry_list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent &) { UpdateButtons(); });
	entry_list->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent &) { UpdateButtons(); });
	entry_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, &DialogGlossary::OnEditEntry, this);
	entry_list->Bind(wxEVT_LIST_COL_DRAGGING, [this](wxListEvent &e) {
		e.Skip();
		QueueRefreshEntryNoteLabels();
	});
	entry_list->Bind(wxEVT_LIST_COL_END_DRAG, [this](wxListEvent &) { RefreshEntryNoteLabels(); });

	// Select the active dictionary initially if there is one.
	int64_t initial = glossary->GetDictionaryId(ActiveName());
	RefreshDicts(initial);
}

int64_t DialogGlossary::SelectedDict() const {
	int sel = dict_list->GetSelection();
	if (sel == wxNOT_FOUND) return -1;
	return dicts[sel].first;
}

int64_t DialogGlossary::SelectedEntry() const {
	long sel = entry_list->GetFirstSelected();
	if (sel == -1) return -1;
	return entries[sel].id;
}

void DialogGlossary::RefreshDicts(int64_t select_id) {
	dicts = glossary->ListDictionaries();
	auto active = ActiveName();

	dict_list->Clear();
	int to_select = wxNOT_FOUND;
	for (size_t i = 0; i < dicts.size(); ++i) {
		wxString label = to_wx(dicts[i].second);
		if (dicts[i].second == active)
			label += "  ★"; // star marks the active dictionary
		dict_list->Append(label);
		if (dicts[i].first == select_id)
			to_select = static_cast<int>(i);
	}
	if (to_select == wxNOT_FOUND && !dicts.empty())
		to_select = 0;
	if (to_select != wxNOT_FOUND)
		dict_list->SetSelection(to_select);

	RefreshEntries();
}

void DialogGlossary::RefreshEntries() {
	entry_list->DeleteAllItems();
	entries.clear();

	int64_t dict = SelectedDict();
	if (dict != -1)
		entries = glossary->ListEntries(dict);

	for (size_t i = 0; i < entries.size(); ++i) {
		auto const& e = entries[i];
		long row = entry_list->InsertItem(i, to_wx(e.term));
		entry_list->SetItem(row, 1, NoteLabel(e));
		entry_list->SetItem(row, 2, e.note_url.empty() ? "" : "✓");
	}

	UpdateButtons();
}

void DialogGlossary::UpdateEntryColumns() {
	int fixed = entry_list->GetColumnWidth(0) + entry_list->GetColumnWidth(2);
	int note = entry_list->GetClientSize().GetWidth() - fixed;
	entry_list->SetColumnWidth(1, std::max(120, note));
}

wxString DialogGlossary::NoteLabel(agi::GlossaryEntry const& entry) const {
	wxString note = to_wx(entry.note_text);
	note.Replace("\r", " ");
	note.Replace("\n", " ");

	// macOS' native wxListCtrl ellipsizing can leave broken glyph fragments for
	// composed Unicode text. Pre-ellipsize so the control draws a complete label.
	wxClientDC dc(entry_list);
	dc.SetFont(entry_list->GetFont());
	int width = std::max(1, entry_list->GetColumnWidth(1) - 16);
	if (dc.GetTextExtent(note).GetWidth() <= width)
		return note;

	return wxControl::Ellipsize(note, dc, wxELLIPSIZE_END, width, wxELLIPSIZE_FLAGS_NONE);
}

void DialogGlossary::RefreshEntryNoteLabels(bool visible_only) {
	long start = 0;
	long end = static_cast<long>(entries.size());

	if (visible_only && end > 0) {
		start = std::max<long>(0, entry_list->GetTopItem());
		end = std::min<long>(end, start + entry_list->GetCountPerPage() + 1);
	}

	for (long i = start; i < end; ++i) {
		wxString label = NoteLabel(entries[i]);
		if (entry_list->GetItemText(i, 1) != label)
			entry_list->SetItem(i, 1, label);
	}
}

void DialogGlossary::QueueRefreshEntryNoteLabels() {
	if (note_label_refresh_pending)
		return;

	note_label_refresh_pending = true;
	entry_list->CallAfter([this] {
		note_label_refresh_pending = false;
		RefreshEntryNoteLabels(true);
	});
}

void DialogGlossary::UpdateButtons() {
	int64_t selected_dict = SelectedDict();
	bool has_dict = selected_dict != -1;
	rename_btn->Enable(has_dict);
	delete_btn->Enable(has_dict);
	active_btn->Enable(has_dict);
	export_btn->Enable(has_dict);
	add_entry_btn->Enable(has_dict);
	active_btn->SetLabel(has_dict && dicts[dict_list->GetSelection()].second == ActiveName() ? _("&Deactivate") : _("Set &Active"));

	bool has_entry = SelectedEntry() != -1;
	edit_entry_btn->Enable(has_entry);
	delete_entry_btn->Enable(has_entry);
}

// Force any open edit boxes to rebuild their matcher and restyle.
void DialogGlossary::NotifyChanged() {
	OPT_SET("Tool/Glossary/Active Dictionary")->SetString(ActiveName());
}

void DialogGlossary::OnNewDict(wxCommandEvent &) {
	wxString name = wxGetTextFromUser(_("Dictionary name:"), _("New Dictionary"), "", &d);
	if (name.empty()) return;
	if (glossary->GetDictionaryId(from_wx(name)) != -1) {
		wxMessageBox(_("A dictionary with that name already exists."), _("Glossary"), wxOK | wxICON_ERROR, &d);
		return;
	}
	auto id = glossary->CreateDictionary(from_wx(name));
	RefreshDicts(id);
}

void DialogGlossary::OnRenameDict(wxCommandEvent &) {
	int64_t id = SelectedDict();
	if (id == -1) return;
	int sel = dict_list->GetSelection();
	bool was_active = dicts[sel].second == ActiveName();

	wxString name = wxGetTextFromUser(_("Dictionary name:"), _("Rename Dictionary"), to_wx(dicts[sel].second), &d);
	if (name.empty() || from_wx(name) == dicts[sel].second) return;
	if (glossary->GetDictionaryId(from_wx(name)) != -1) {
		wxMessageBox(_("A dictionary with that name already exists."), _("Glossary"), wxOK | wxICON_ERROR, &d);
		return;
	}
	glossary->RenameDictionary(id, from_wx(name));
	if (was_active) {
		OPT_SET("Tool/Glossary/Active Dictionary")->SetString(from_wx(name));
		NotifyChanged();
	}
	RefreshDicts(id);
}

void DialogGlossary::OnDeleteDict(wxCommandEvent &) {
	int64_t id = SelectedDict();
	if (id == -1) return;
	int sel = dict_list->GetSelection();
	if (wxMessageBox(fmt_tl("Delete dictionary \"%s\" and all its terms?", dicts[sel].second),
	                 _("Glossary"), wxYES_NO | wxICON_QUESTION, &d) != wxYES)
		return;

	bool was_active = dicts[sel].second == ActiveName();
	glossary->DeleteDictionary(id);
	if (was_active) {
		OPT_SET("Tool/Glossary/Active Dictionary")->SetString("");
		NotifyChanged();
	}
	RefreshDicts();
}

void DialogGlossary::OnSetActive(wxCommandEvent &) {
	int sel = dict_list->GetSelection();
	if (sel == wxNOT_FOUND) return;
	if (dicts[sel].second == ActiveName())
		DeactivateGlossaryDictionary();
	else
		ActivateGlossaryDictionary(dicts[sel].second);
	NotifyChanged();
	RefreshDicts(dicts[sel].first);
}

void DialogGlossary::OnAddEntry(wxCommandEvent &) {
	int64_t dict = SelectedDict();
	if (dict == -1) return;

	agi::GlossaryEntry e;
	EntryEditor editor(&d, e);
	if (!editor.Run() || e.term.empty()) return;

	auto id = glossary->UpsertEntry(dict, e);
	if (dicts[dict_list->GetSelection()].second == ActiveName())
		NotifyChanged();
	RefreshEntries();
	// Keep the newly added entry selected.
	for (size_t i = 0; i < entries.size(); ++i)
		if (entries[i].id == id) { entry_list->Select(i); break; }
}

void DialogGlossary::OnEditEntry(wxCommandEvent &) {
	int64_t id = SelectedEntry();
	if (id == -1) return;

	agi::GlossaryEntry e = glossary->GetEntry(id);
	if (e.id == 0) return;

	EntryEditor editor(&d, e);
	if (!editor.Run() || e.term.empty()) return;

	glossary->UpsertEntry(SelectedDict(), e);
	if (dicts[dict_list->GetSelection()].second == ActiveName())
		NotifyChanged();
	RefreshEntries();
}

void DialogGlossary::OnDeleteEntry(wxCommandEvent &) {
	int64_t id = SelectedEntry();
	if (id == -1) return;
	glossary->DeleteEntry(id);
	if (dicts[dict_list->GetSelection()].second == ActiveName())
		NotifyChanged();
	RefreshEntries();
}

void DialogGlossary::OnExport(wxCommandEvent &) {
	int sel = dict_list->GetSelection();
	if (sel == wxNOT_FOUND) return;

	wxFileDialog dlg(&d, _("Export dictionary"), "", to_wx(dicts[sel].second) + ".sqlite",
		GlossaryFileWildcard(),
		wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
	if (dlg.ShowModal() != wxID_OK) return;

	try {
		agi::fs::path selected = from_wx(dlg.GetPath());
		auto format = FormatFromFilter(dlg.GetFilterIndex());
		auto path = ApplyGlossaryExtension(selected, format);
		if (path != selected && agi::fs::FileExists(path)) {
			int res = wxMessageBox(_("A file with this name already exists. Do you want to replace it?"),
				_("Confirm overwrite"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING | wxCENTER, &d);
			if (res != wxYES)
				return;
		}

		GlossaryExportDictionary out{DictionaryNameFromPath(path), glossary->ListEntries(dicts[sel].first)};
		switch (format) {
		case GlossaryFileFormat::SQLite: ExportSQLite(path, out); break;
		case GlossaryFileFormat::JSON: ExportJSON(path, out); break;
		case GlossaryFileFormat::CSV: ExportCSV(path, out); break;
		}
	}
	catch (agi::Exception const& e) {
		wxMessageBox(to_wx(e.GetMessage()), _("Glossary"), wxOK | wxICON_ERROR, &d);
	}
	catch (std::exception const& e) {
		wxMessageBox(to_wx(e.what()), _("Glossary"), wxOK | wxICON_ERROR, &d);
	}
}

void DialogGlossary::OnImport(wxCommandEvent &) {
	wxFileDialog dlg(&d, _("Import dictionary"), "", "",
		GlossaryFileWildcard(),
		wxFD_OPEN | wxFD_FILE_MUST_EXIST);
	if (dlg.ShowModal() != wxID_OK) return;

	try {
		agi::fs::path path = from_wx(dlg.GetPath());
		switch (FormatFromExtension(path, dlg.GetFilterIndex())) {
		case GlossaryFileFormat::SQLite: ImportDictionaries(ImportSQLite(path)); break;
		case GlossaryFileFormat::JSON: ImportDictionaries(ImportJSON(path)); break;
		case GlossaryFileFormat::CSV: ImportDictionaries(ImportCSV(path)); break;
		}
	}
	catch (agi::Exception const& e) {
		wxMessageBox(to_wx(e.GetMessage()), _("Glossary"), wxOK | wxICON_ERROR, &d);
	}
	catch (std::exception const& e) {
		wxMessageBox(to_wx(e.what()), _("Glossary"), wxOK | wxICON_ERROR, &d);
	}
}

void DialogGlossary::ImportDictionaries(std::vector<GlossaryExportDictionary> const& imported) {
	int64_t last = -1;

	for (auto const& src : imported) {
		if (src.name.empty())
			throw agi::GlossaryError("Imported glossary contains a dictionary with an empty name");

		std::string name = src.name;
		int64_t existing = glossary->GetDictionaryId(name);
		if (existing != -1) {
			int r = wxMessageBox(
				fmt_tl("A dictionary named \"%s\" already exists. Overwrite it?\n(No creates a copy, Cancel skips it.)", name),
				_("Import"), wxYES_NO | wxCANCEL | wxICON_QUESTION, &d);
			if (r == wxCANCEL) continue;
			if (r == wxYES) {
				bool was_active = name == ActiveName();
				glossary->DeleteDictionary(existing);
				if (was_active) OPT_SET("Tool/Glossary/Active Dictionary")->SetString("");
			}
			else {
				int n = 2;
				while (glossary->GetDictionaryId(name + agi::format(" (%d)", n)) != -1) ++n;
				name += agi::format(" (%d)", n);
			}
		}

		last = glossary->CreateDictionary(name);
		for (auto entry : src.entries) {
			if (entry.term.empty())
				throw agi::GlossaryError(agi::format("Dictionary \"%s\" contains an entry with an empty term", name));
			entry.id = 0;
			glossary->UpsertEntry(last, entry);
		}
	}

	NotifyChanged();
	RefreshDicts(last);
}
} // namespace

void ShowGlossaryDialog(agi::Context *c) {
	try {
		DialogGlossary(c->parent).d.ShowModal();
	}
	catch (agi::Exception const& e) {
		LOG_E("glossary") << e.GetMessage();
		wxMessageBox(to_wx(e.GetMessage()), _("Glossary"), wxOK | wxICON_ERROR, c->parent);
	}
}

bool EditGlossaryEntry(wxWindow *parent, agi::Glossary &glossary, int64_t entry_id) {
	try {
		agi::GlossaryEntry e = glossary.GetEntry(entry_id);
		if (e.id == 0) return false;

		EntryEditor editor(parent, e);
		if (!editor.Run() || e.term.empty()) return false;

		glossary.UpsertEntry(glossary.GetActiveDictionary(), e);
		return true;
	}
	catch (agi::Exception const& e) {
		LOG_E("glossary") << e.GetMessage();
		wxMessageBox(to_wx(e.GetMessage()), _("Glossary"), wxOK | wxICON_ERROR, parent);
		return false;
	}
}

bool AddGlossaryEntry(wxWindow *parent, agi::Glossary &glossary, std::string const& term) {
	try {
		int64_t dict = glossary.GetActiveDictionary();
		if (dict == -1) {
			wxMessageBox(_("No active glossary dictionary is selected."), _("Glossary"), wxOK | wxICON_INFORMATION, parent);
			return false;
		}

		agi::GlossaryEntry e;
		e.term = term;

		EntryEditor editor(parent, e);
		if (!editor.Run() || e.term.empty()) return false;

		glossary.UpsertEntry(dict, e);
		return true;
	}
	catch (agi::Exception const& e) {
		LOG_E("glossary") << e.GetMessage();
		wxMessageBox(to_wx(e.GetMessage()), _("Glossary"), wxOK | wxICON_ERROR, parent);
		return false;
	}
}
