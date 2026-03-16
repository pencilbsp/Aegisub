// Copyright (c) 2026, Aegisub contributors
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
// OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
// CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#include "visual_tool_ocr.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "async_video_provider.h"
#include "compat.h"
#include "format.h"
#include "frame_main.h"
#include "include/aegisub/context.h"
#include "project.h"
#include "selection_controller.h"
#include "utils.h"
#include "video_controller.h"
#include "video_display.h"
#include "video_frame.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <wx/colour.h>
#include <wx/cursor.h>
#include <wx/event.h>
#include <wx/image.h>
#include <wx/log.h>
#include <wx/menu.h>

namespace {
enum {
	ID_OCR_COPY = wxID_HIGHEST + 6501,
	ID_OCR_INSERT_REPLACE,
	ID_OCR_INSERT_BEFORE,
	ID_OCR_INSERT_AFTER
};

Vector2D RegionTopLeft(osx::ocr::Region const& region, Vector2D const& video_pos, Vector2D const& video_size) {
	return Vector2D(
		video_pos.X() + region.x * video_size.X(),
		video_pos.Y() + region.y * video_size.Y());
}

Vector2D RegionBottomRight(osx::ocr::Region const& region, Vector2D const& video_pos, Vector2D const& video_size) {
	return Vector2D(
		video_pos.X() + (region.x + region.width) * video_size.X(),
		video_pos.Y() + (region.y + region.height) * video_size.Y());
}

Vector2D CharacterTopLeft(osx::ocr::Character const& ch, Vector2D const& video_pos, Vector2D const& video_size) {
	return Vector2D(
		video_pos.X() + ch.x * video_size.X(),
		video_pos.Y() + ch.y * video_size.Y());
}

Vector2D CharacterBottomRight(osx::ocr::Character const& ch, Vector2D const& video_pos, Vector2D const& video_size) {
	return Vector2D(
		video_pos.X() + (ch.x + ch.width) * video_size.X(),
		video_pos.Y() + (ch.y + ch.height) * video_size.Y());
}

bool CharacterOrderLess(osx::ocr::Character const& lhs, osx::ocr::Character const& rhs) {
	if (lhs.region_index != rhs.region_index)
		return lhs.region_index < rhs.region_index;

	double const y_threshold = std::max(lhs.height, rhs.height) * 0.5;
	if (std::abs(lhs.y - rhs.y) > y_threshold)
		return lhs.y < rhs.y;
	return lhs.x < rhs.x;
}
}

VisualToolOCR::VisualToolOCR(VideoDisplay *parent, agi::Context *context)
: VisualToolBase(parent, context)
{
	connections.push_back(c->videoController->AddPlaybackStateChangeListener(&VisualToolOCR::OnPlaybackStateChanged, this));
	parent->SetCursor(wxCursor(wxCURSOR_HAND));
	RefreshOcrData();
	UpdateCursor();
}

VisualToolOCR::~VisualToolOCR() {
	parent->SetCursor(wxNullCursor);
}

void VisualToolOCR::RefreshOcrData() {
	regions.clear();
	characters.clear();
	selected_regions.clear();
	selected_characters.clear();
	hovered_region = -1;
	hovered_character = -1;
	dragging_character_selection = false;
	drag_additive_selection = false;
	drag_anchor_character = -1;
	drag_focus_character = -1;
	last_error.clear();

	auto provider = c->project->VideoProvider();
	if (!provider)
		return;

	int const frame = c->videoController->GetFrameN();
	wxImage image = GetImage(*provider->GetFrame(frame, c->project->Timecodes().TimeAtFrame(frame), true));
	auto result = osx::ocr::RecognizeText(image);
	if (!result.error.empty()) {
		last_error = result.error;
		wxLogError(fmt_tl("OCR failed: %s", result.error));
		return;
	}

	regions = std::move(result.regions);
	characters = std::move(result.characters);
	selected_regions.assign(regions.size(), false);
	selected_characters.assign(characters.size(), false);
}

int VisualToolOCR::HitTestRegion(Vector2D pos) const {
	int best_region = -1;
	double best_area = 0.0;
	bool best_area_initialized = false;

	for (size_t i = 0; i < regions.size(); ++i) {
		auto const top_left = RegionTopLeft(regions[i], video_pos, video_size);
		auto const bottom_right = RegionBottomRight(regions[i], video_pos, video_size);
		if (pos.X() < top_left.X() || pos.X() > bottom_right.X() || pos.Y() < top_left.Y() || pos.Y() > bottom_right.Y())
			continue;

		double area = std::max<double>(1.0, static_cast<double>((bottom_right.X() - top_left.X()) * (bottom_right.Y() - top_left.Y())));
		if (!best_area_initialized || area < best_area) {
			best_region = static_cast<int>(i);
			best_area = area;
			best_area_initialized = true;
		}
	}

	return best_region;
}

int VisualToolOCR::HitTestCharacter(Vector2D pos) const {
	int best_character = -1;
	double best_area = 0.0;
	bool best_area_initialized = false;

	for (size_t i = 0; i < characters.size(); ++i) {
		auto const top_left = CharacterTopLeft(characters[i], video_pos, video_size);
		auto const bottom_right = CharacterBottomRight(characters[i], video_pos, video_size);
		if (pos.X() < top_left.X() || pos.X() > bottom_right.X() || pos.Y() < top_left.Y() || pos.Y() > bottom_right.Y())
			continue;

		double area = std::max<double>(1.0, static_cast<double>((bottom_right.X() - top_left.X()) * (bottom_right.Y() - top_left.Y())));
		if (!best_area_initialized || area < best_area) {
			best_character = static_cast<int>(i);
			best_area = area;
			best_area_initialized = true;
		}
	}

	return best_character;
}

std::vector<size_t> VisualToolOCR::SortedCharacterIndicesForRegion(size_t region_index) const {
	std::vector<size_t> indices;
	indices.reserve(characters.size());
	for (size_t i = 0; i < characters.size(); ++i) {
		if (characters[i].region_index == region_index && !characters[i].text.empty())
			indices.push_back(i);
	}

	std::sort(indices.begin(), indices.end(), [this](size_t lhs, size_t rhs) {
		return CharacterOrderLess(characters[lhs], characters[rhs]);
	});
	return indices;
}

void VisualToolOCR::SelectCharacterRange(int anchor_index, int focus_index, bool additive) {
	if (anchor_index < 0 || focus_index < 0)
		return;
	if (static_cast<size_t>(anchor_index) >= characters.size() || static_cast<size_t>(focus_index) >= characters.size())
		return;

	size_t const region_index = characters[anchor_index].region_index;
	if (characters[focus_index].region_index != region_index)
		return;

	auto ordered = SortedCharacterIndicesForRegion(region_index);
	if (ordered.empty())
		return;

	auto anchor_it = std::find(ordered.begin(), ordered.end(), static_cast<size_t>(anchor_index));
	auto focus_it = std::find(ordered.begin(), ordered.end(), static_cast<size_t>(focus_index));
	if (anchor_it == ordered.end() || focus_it == ordered.end())
		return;

	size_t const anchor_pos = static_cast<size_t>(std::distance(ordered.begin(), anchor_it));
	size_t const focus_pos = static_cast<size_t>(std::distance(ordered.begin(), focus_it));
	size_t const begin_pos = std::min(anchor_pos, focus_pos);
	size_t const end_pos = std::max(anchor_pos, focus_pos);

	if (!additive) {
		std::fill(selected_regions.begin(), selected_regions.end(), false);
		std::fill(selected_characters.begin(), selected_characters.end(), false);
	}

	if (region_index < selected_regions.size())
		selected_regions[region_index] = true;

	for (size_t pos = begin_pos; pos <= end_pos; ++pos)
		selected_characters[ordered[pos]] = true;
}

void VisualToolOCR::UpdateCursor() {
	if (c->videoController->IsPlaying()) {
		parent->SetCursor(wxCursor(wxCURSOR_HAND));
		return;
	}

	if (dragging_character_selection || hovered_character >= 0 || hovered_region >= 0)
		parent->SetCursor(wxCursor(wxCURSOR_IBEAM));
	else
		parent->SetCursor(wxCursor(wxCURSOR_HAND));
}

std::string VisualToolOCR::SelectedText() const {
	std::vector<size_t> selected_char_indices;
	for (size_t i = 0; i < selected_characters.size(); ++i) {
		if (selected_characters[i] && !characters[i].text.empty())
			selected_char_indices.push_back(i);
	}

	if (!selected_char_indices.empty()) {
		std::sort(selected_char_indices.begin(), selected_char_indices.end(), [this](size_t lhs, size_t rhs) {
			return CharacterOrderLess(characters[lhs], characters[rhs]);
		});

		std::string out;
		size_t last_region = std::numeric_limits<size_t>::max();
		for (size_t idx : selected_char_indices) {
			if (last_region != std::numeric_limits<size_t>::max() && characters[idx].region_index != last_region)
				out += '\n';
			out += characters[idx].text;
			last_region = characters[idx].region_index;
		}
		return out;
	}

	std::vector<size_t> selected_region_indices;
	for (size_t i = 0; i < selected_regions.size(); ++i) {
		if (selected_regions[i] && !regions[i].text.empty())
			selected_region_indices.push_back(i);
	}
	std::sort(selected_region_indices.begin(), selected_region_indices.end());

	std::string out;
	for (size_t i = 0; i < selected_region_indices.size(); ++i) {
		if (i)
			out += '\n';
		out += regions[selected_region_indices[i]].text;
	}
	return out;
}

bool VisualToolOCR::HasSelection() const {
	return std::any_of(selected_characters.begin(), selected_characters.end(), [](bool value) { return value; })
	    || std::any_of(selected_regions.begin(), selected_regions.end(), [](bool value) { return value; });
}

void VisualToolOCR::CopySelectedText() {
	auto text = SelectedText();
	if (text.empty()) {
		if (c->frame)
			c->frame->StatusTimeout(_("No OCR selection."), 2500);
		return;
	}

	SetClipboard(text);
	if (c->frame)
		c->frame->StatusTimeout(_("OCR text copied to clipboard."), 2500);
}

void VisualToolOCR::InsertSelectedText(InsertMode mode) {
	auto text = SelectedText();
	if (text.empty()) {
		if (c->frame)
			c->frame->StatusTimeout(_("No OCR selection."), 2500);
		return;
	}

	auto *line = c->selectionController->GetActiveLine();
	if (!line) {
		if (c->frame)
			c->frame->StatusTimeout(_("No active subtitle line to insert into."), 3000);
		return;
	}

	std::string const existing_text = line->Text.get();
	bool const has_existing_text = !existing_text.empty();

	switch (mode) {
		case InsertMode::Replace:
			line->Text = text;
			break;
		case InsertMode::InsertBefore:
			line->Text = has_existing_text ? text + "\\N" + existing_text : text;
			break;
		case InsertMode::InsertAfter:
			line->Text = has_existing_text ? existing_text + "\\N" + text : text;
			break;
	}

	c->ass->Commit(_("ocr frame"), AssFile::COMMIT_DIAG_TEXT, -1, line);

	if (c->frame)
		c->frame->StatusTimeout(_("OCR text inserted."), 2500);
}

void VisualToolOCR::OpenContextMenu(Vector2D mouse_point) {
	if (!HasSelection()) {
		int region_to_select = -1;
		if (hovered_region >= 0)
			region_to_select = hovered_region;
		else if (hovered_character >= 0) {
			size_t const region_idx = characters[hovered_character].region_index;
			if (region_idx < selected_regions.size())
				region_to_select = static_cast<int>(region_idx);
		}

		if (region_to_select >= 0) {
			std::fill(selected_regions.begin(), selected_regions.end(), false);
			std::fill(selected_characters.begin(), selected_characters.end(), false);
			selected_regions[region_to_select] = true;
		}
	}

	wxMenu menu;
	menu.Append(ID_OCR_COPY, _("Copy"));
	menu.AppendSeparator();
	menu.Append(ID_OCR_INSERT_REPLACE, _("Insert: Replace current line"));
	menu.Append(ID_OCR_INSERT_BEFORE, _("Insert: Before current line text"));
	menu.Append(ID_OCR_INSERT_AFTER, _("Insert: After current line text"));

	bool const has_selection = HasSelection();
	bool const has_active_line = c->selectionController->GetActiveLine() != nullptr;
	menu.Enable(ID_OCR_COPY, has_selection);
	menu.Enable(ID_OCR_INSERT_REPLACE, has_selection && has_active_line);
	menu.Enable(ID_OCR_INSERT_BEFORE, has_selection && has_active_line);
	menu.Enable(ID_OCR_INSERT_AFTER, has_selection && has_active_line);

	menu.Bind(wxEVT_MENU, [this](wxCommandEvent &evt) {
		switch (evt.GetId()) {
			case ID_OCR_COPY:
				CopySelectedText();
				break;
			case ID_OCR_INSERT_REPLACE:
				InsertSelectedText(InsertMode::Replace);
				break;
			case ID_OCR_INSERT_BEFORE:
				InsertSelectedText(InsertMode::InsertBefore);
				break;
			case ID_OCR_INSERT_AFTER:
				InsertSelectedText(InsertMode::InsertAfter);
				break;
			default:
				break;
		}
	});

	parent->PopupMenu(&menu, wxPoint(static_cast<int>(mouse_point.X()), static_cast<int>(mouse_point.Y())));
}

void VisualToolOCR::OnFrameChanged() {
	if (c->videoController->IsPlaying())
		return;
	RefreshOcrData();
	parent->Render();
}

void VisualToolOCR::OnCoordinateSystemsChanged() {
	parent->Render();
}

void VisualToolOCR::OnPlaybackStateChanged(bool is_playing) {
	dragging_character_selection = false;
	drag_additive_selection = false;
	drag_anchor_character = -1;
	drag_focus_character = -1;

	if (is_playing) {
		hovered_region = -1;
		hovered_character = -1;
		UpdateCursor();
		parent->Render();
		return;
	}

	RefreshOcrData();
	UpdateCursor();
	parent->Render();
}

void VisualToolOCR::OnMouseEvent(wxMouseEvent &event) {
	if (c->videoController->IsPlaying()) {
		UpdateCursor();
		return;
	}

	shift_down = event.ShiftDown();
	ctrl_down = event.CmdDown();
	alt_down = event.AltDown();
	mouse_pos = event.GetPosition();

	if (event.Leaving()) {
		mouse_pos = Vector2D();
		hovered_region = -1;
		hovered_character = -1;
		dragging_character_selection = false;
		drag_additive_selection = false;
		drag_anchor_character = -1;
		drag_focus_character = -1;
		UpdateCursor();
		parent->Render();
		return;
	}

	hovered_character = HitTestCharacter(mouse_pos);
	hovered_region = HitTestRegion(mouse_pos);
	UpdateCursor();

	bool const is_multi_select = shift_down || ctrl_down || alt_down;

	if (event.LeftUp()) {
		dragging_character_selection = false;
		drag_additive_selection = false;
		drag_anchor_character = -1;
		drag_focus_character = -1;
		return;
	}

	if (dragging_character_selection && event.Dragging() && event.LeftIsDown()) {
		if (hovered_character >= 0 && drag_anchor_character >= 0) {
			size_t const anchor_region = characters[drag_anchor_character].region_index;
			size_t const hovered_region_index = characters[hovered_character].region_index;
			if (anchor_region == hovered_region_index && hovered_character != drag_focus_character) {
				drag_focus_character = hovered_character;
				SelectCharacterRange(drag_anchor_character, drag_focus_character, drag_additive_selection);
				parent->Render();
			}
		}
		return;
	}

	if (event.RightDown()) {
		OpenContextMenu(mouse_pos);
		parent->Render();
		return;
	}

	if (event.LeftDown()) {
		if (hovered_character >= 0) {
			size_t const region_idx = characters[hovered_character].region_index;
			if (!is_multi_select
			    && static_cast<size_t>(hovered_character) < selected_characters.size()
			    && selected_characters[hovered_character]) {
				selected_characters[hovered_character] = false;
				if (region_idx < selected_regions.size()) {
					bool region_has_selected_character = false;
					for (size_t i = 0; i < characters.size(); ++i) {
						if (i < selected_characters.size()
						    && selected_characters[i]
						    && characters[i].region_index == region_idx) {
							region_has_selected_character = true;
							break;
						}
					}
					if (!region_has_selected_character)
						selected_regions[region_idx] = false;
				}
				parent->Render();
				return;
			}

			dragging_character_selection = true;
			drag_additive_selection = is_multi_select;
			drag_anchor_character = hovered_character;
			drag_focus_character = hovered_character;
			SelectCharacterRange(drag_anchor_character, drag_focus_character, drag_additive_selection);

			parent->Render();
			return;
		}

		dragging_character_selection = false;
		drag_additive_selection = false;
		drag_anchor_character = -1;
		drag_focus_character = -1;

		if (hovered_region >= 0) {
			if (!is_multi_select) {
				std::fill(selected_regions.begin(), selected_regions.end(), false);
				std::fill(selected_characters.begin(), selected_characters.end(), false);
				selected_regions[hovered_region] = true;
			}
			else {
				bool const new_state = !selected_regions[hovered_region];
				selected_regions[hovered_region] = new_state;
				if (!new_state) {
					for (size_t i = 0; i < characters.size(); ++i) {
						if (characters[i].region_index == static_cast<size_t>(hovered_region))
							selected_characters[i] = false;
					}
				}
			}

			parent->Render();
			return;
		}

		if (!is_multi_select) {
			std::fill(selected_regions.begin(), selected_regions.end(), false);
			std::fill(selected_characters.begin(), selected_characters.end(), false);
			parent->Render();
		}
		return;
	}

	if (event.Moving() || event.Dragging())
		parent->Render();
}

bool VisualToolOCR::OnContextMenu(wxContextMenuEvent &event) {
	if (c->videoController->IsPlaying())
		return false;

	wxPoint context_point = event.GetPosition();
	if (context_point == wxDefaultPosition)
		context_point = wxPoint(static_cast<int>(mouse_pos.X()), static_cast<int>(mouse_pos.Y()));
	else
		context_point = parent->ScreenToClient(context_point);

	mouse_pos = context_point;
	hovered_character = HitTestCharacter(mouse_pos);
	hovered_region = HitTestRegion(mouse_pos);
	UpdateCursor();

	if (!HasSelection() && hovered_character < 0 && hovered_region < 0)
		return false;

	OpenContextMenu(mouse_pos);
	parent->Render();
	return true;
}

bool VisualToolOCR::OnKeyDown(wxKeyEvent &event) {
	if (c->videoController->IsPlaying())
		return false;

	int const key_code = event.GetKeyCode();
	int const unicode_key = event.GetUnicodeKey();
	bool const is_select_all_shortcut = (event.CmdDown() || event.ControlDown()) && !event.AltDown();
	bool const is_a_key = key_code == 'A' || key_code == 'a' || key_code == 1
		|| unicode_key == 'A' || unicode_key == 'a' || unicode_key == 1;

	if (!is_select_all_shortcut || !is_a_key)
		return false;

	std::fill(selected_regions.begin(), selected_regions.end(), true);
	std::fill(selected_characters.begin(), selected_characters.end(), true);
	dragging_character_selection = false;
	drag_additive_selection = false;
	drag_anchor_character = -1;
	drag_focus_character = -1;
	parent->Render();
	return true;
}

void VisualToolOCR::Draw() {
	if (c->videoController->IsPlaying())
		return;

	// Draw OCR line/region boxes
	for (size_t i = 0; i < regions.size(); ++i) {
		auto const top_left = RegionTopLeft(regions[i], video_pos, video_size);
		auto const bottom_right = RegionBottomRight(regions[i], video_pos, video_size);

		bool const is_selected = i < selected_regions.size() && selected_regions[i];

		wxColour color = *wxWHITE;
		float fill_alpha = 0.03f;
		int line_width = 1;
		if (is_selected) {
			color = wxColour(0, 200, 255);
			fill_alpha = 0.14f;
			line_width = 2;
		}

		gl.SetLineColour(color, 0.9f, line_width);
		gl.SetFillColour(color, fill_alpha);
		gl.DrawRectangle(top_left, bottom_right);
	}

	// Draw character boxes only for selected regions
	for (size_t i = 0; i < characters.size(); ++i) {
		size_t region_idx = characters[i].region_index;
		if (region_idx >= selected_regions.size() || !selected_regions[region_idx])
			continue;

		auto const top_left = CharacterTopLeft(characters[i], video_pos, video_size);
		auto const bottom_right = CharacterBottomRight(characters[i], video_pos, video_size);
		bool const is_selected = i < selected_characters.size() && selected_characters[i];

		wxColour color(120, 220, 255);
		float fill_alpha = 0.08f;
		int line_width = 1;
		if (is_selected) {
			color = wxColour(0, 255, 130);
			fill_alpha = 0.20f;
			line_width = 2;
		}

		gl.SetLineColour(color, 0.95f, line_width);
		gl.SetFillColour(color, fill_alpha);
		gl.DrawRectangle(top_left, bottom_right);
	}
}
