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
#include "subs_edit_box.h"
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
	ID_OCR_REPLACE_TEXT,
	ID_OCR_REPLACE_ORIGINAL,
	ID_OCR_INSERT_CARET,
	ID_OCR_CLEAR_REGION,
	ID_OCR_DIM_OUTSIDE_REGION
};

const char *const OPT_OCR_VISUAL_TOOL_USE_REGION = "Tool/OCR/Visual Tool/Use Region";
const char *const OPT_OCR_VISUAL_TOOL_DIM_OUTSIDE_REGION = "Tool/OCR/Visual Tool/Dim Outside Region";
const char *const OPT_OCR_VISUAL_TOOL_REGION_SET = "Tool/OCR/Visual Tool/Region/Set";
const char *const OPT_OCR_VISUAL_TOOL_REGION_X = "Tool/OCR/Visual Tool/Region/X";
const char *const OPT_OCR_VISUAL_TOOL_REGION_Y = "Tool/OCR/Visual Tool/Region/Y";
const char *const OPT_OCR_VISUAL_TOOL_REGION_WIDTH = "Tool/OCR/Visual Tool/Region/Width";
const char *const OPT_OCR_VISUAL_TOOL_REGION_HEIGHT = "Tool/OCR/Visual Tool/Region/Height";

VisualOcrRoi read_visual_ocr_roi() {
	VisualOcrRoi roi{
		OPT_GET(OPT_OCR_VISUAL_TOOL_REGION_X)->GetDouble(),
		OPT_GET(OPT_OCR_VISUAL_TOOL_REGION_Y)->GetDouble(),
		OPT_GET(OPT_OCR_VISUAL_TOOL_REGION_WIDTH)->GetDouble(),
		OPT_GET(OPT_OCR_VISUAL_TOOL_REGION_HEIGHT)->GetDouble()
	};

	if (!roi.IsValid() || roi.x < 0.0 || roi.y < 0.0 || roi.x >= 1.0 || roi.y >= 1.0)
		return {0.0, 0.0, 0.0, 0.0};

	roi.width = std::min(roi.width, 1.0 - roi.x);
	roi.height = std::min(roi.height, 1.0 - roi.y);
	if (!roi.IsValid())
		return {0.0, 0.0, 0.0, 0.0};

	return roi;
}

wxImage CropImageToRoi(wxImage const& image, VisualOcrRoi const& roi) {
	if (!image.IsOk() || !roi.IsValid())
		return image;

	int const width = image.GetWidth();
	int const height = image.GetHeight();
	if (width <= 1 || height <= 1)
		return image;

	int x = std::clamp(static_cast<int>(std::lround(roi.x * width)), 0, width - 1);
	int y = std::clamp(static_cast<int>(std::lround(roi.y * height)), 0, height - 1);
	int w = std::clamp(static_cast<int>(std::lround(roi.width * width)), 1, width - x);
	int h = std::clamp(static_cast<int>(std::lround(roi.height * height)), 1, height - y);
	return image.GetSubImage(wxRect(x, y, w, h));
}

void MapOcrResultFromRoi(osx::ocr::Result& result, VisualOcrRoi const& roi) {
	auto map_box = [roi](double& x, double& y, double& width, double& height) {
		x = roi.x + x * roi.width;
		y = roi.y + y * roi.height;
		width *= roi.width;
		height *= roi.height;
	};

	for (auto& region : result.regions)
		map_box(region.x, region.y, region.width, region.height);
	for (auto& ch : result.characters)
		map_box(ch.x, ch.y, ch.width, ch.height);
}

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
	LoadPersistedRoi();
	RefreshOcrData();
	UpdateCursor();
}

VisualToolOCR::~VisualToolOCR() {
	parent->SetCursor(wxNullCursor);
}

void VisualToolOCR::LoadPersistedRoi() {
	roi = read_visual_ocr_roi();
	has_roi = OPT_GET(OPT_OCR_VISUAL_TOOL_USE_REGION)->GetBool()
		&& OPT_GET(OPT_OCR_VISUAL_TOOL_REGION_SET)->GetBool()
		&& roi.IsValid();
}

void VisualToolOCR::SavePersistedRoi() const {
	OPT_SET(OPT_OCR_VISUAL_TOOL_USE_REGION)->SetBool(has_roi);
	OPT_SET(OPT_OCR_VISUAL_TOOL_REGION_SET)->SetBool(has_roi && roi.IsValid());
	if (!has_roi || !roi.IsValid())
		return;

	OPT_SET(OPT_OCR_VISUAL_TOOL_REGION_X)->SetDouble(roi.x);
	OPT_SET(OPT_OCR_VISUAL_TOOL_REGION_Y)->SetDouble(roi.y);
	OPT_SET(OPT_OCR_VISUAL_TOOL_REGION_WIDTH)->SetDouble(roi.width);
	OPT_SET(OPT_OCR_VISUAL_TOOL_REGION_HEIGHT)->SetDouble(roi.height);
}

void VisualToolOCR::ClearRoi() {
	if (!has_roi)
		return;

	has_roi = false;
	roi = {};
	SavePersistedRoi();
	RefreshOcrData();
}

bool VisualToolOCR::IsInsideVideo(Vector2D pos) const {
	return pos.X() >= video_pos.X()
	    && pos.Y() >= video_pos.Y()
	    && pos.X() <= video_pos.X() + video_size.X()
	    && pos.Y() <= video_pos.Y() + video_size.Y();
}

Vector2D VisualToolOCR::ClampToVideo(Vector2D pos) const {
	Vector2D video_max = video_pos + video_size;
	return video_pos.Max(video_max.Min(pos));
}

VisualOcrRoi VisualToolOCR::EditableRoi() const {
	return has_roi && roi.IsValid() ? roi : VisualOcrRoi{};
}

Vector2D VisualToolOCR::RoiTopLeft(VisualOcrRoi const& draw_roi) const {
	return Vector2D(
		video_pos.X() + draw_roi.x * video_size.X(),
		video_pos.Y() + draw_roi.y * video_size.Y());
}

Vector2D VisualToolOCR::RoiBottomRight(VisualOcrRoi const& draw_roi) const {
	return Vector2D(
		video_pos.X() + (draw_roi.x + draw_roi.width) * video_size.X(),
		video_pos.Y() + (draw_roi.y + draw_roi.height) * video_size.Y());
}

VisualOcrRoiEdge VisualToolOCR::HitTestRoiEdge(Vector2D pos) const {
	if (!IsInsideVideo(pos) || video_size.X() <= 0.0f || video_size.Y() <= 0.0f)
		return VisualOcrRoiEdge::None;

	auto const edit_roi = EditableRoi();
	auto const top_left = RoiTopLeft(edit_roi);
	auto const bottom_right = RoiBottomRight(edit_roi);
	double const threshold = 8.0;
	double best_distance = threshold + 1.0;
	VisualOcrRoiEdge edge = VisualOcrRoiEdge::None;

	auto try_candidate = [&](double distance, VisualOcrRoiEdge candidate) {
		if (distance <= threshold && distance < best_distance) {
			best_distance = distance;
			edge = candidate;
		}
	};

	try_candidate(std::abs(pos.X() - top_left.X()), VisualOcrRoiEdge::Left);
	try_candidate(std::abs(pos.X() - bottom_right.X()), VisualOcrRoiEdge::Right);
	try_candidate(std::abs(pos.Y() - top_left.Y()), VisualOcrRoiEdge::Top);
	try_candidate(std::abs(pos.Y() - bottom_right.Y()), VisualOcrRoiEdge::Bottom);
	return edge;
}

void VisualToolOCR::AdjustRoiEdge(VisualOcrRoiEdge edge, Vector2D pos) {
	if (edge == VisualOcrRoiEdge::None || video_size.X() <= 0.0f || video_size.Y() <= 0.0f)
		return;

	auto const clamped = ClampToVideo(pos);
	double const x = (clamped.X() - video_pos.X()) / video_size.X();
	double const y = (clamped.Y() - video_pos.Y()) / video_size.Y();
	double const min_width = std::min(1.0, 8.0 / video_size.X());
	double const min_height = std::min(1.0, 8.0 / video_size.Y());

	auto edit_roi = EditableRoi();
	double left = edit_roi.x;
	double top = edit_roi.y;
	double right = edit_roi.x + edit_roi.width;
	double bottom = edit_roi.y + edit_roi.height;

	switch (edge) {
		case VisualOcrRoiEdge::Left:
			left = std::clamp(x, 0.0, std::max(0.0, right - min_width));
			break;
		case VisualOcrRoiEdge::Right:
			right = std::clamp(x, std::min(1.0, left + min_width), 1.0);
			break;
		case VisualOcrRoiEdge::Top:
			top = std::clamp(y, 0.0, std::max(0.0, bottom - min_height));
			break;
		case VisualOcrRoiEdge::Bottom:
			bottom = std::clamp(y, std::min(1.0, top + min_height), 1.0);
			break;
		case VisualOcrRoiEdge::None:
			return;
	}

	roi = {left, top, right - left, bottom - top};
	has_roi = roi.IsValid();
}

void VisualToolOCR::FinishRoiDrag() {
	if (!dragging_roi)
		return;

	dragging_roi = false;
	dragged_roi_edge = VisualOcrRoiEdge::None;
	if (parent->HasCapture())
		parent->ReleaseMouse();
	parent->SetFocus();

	if (has_roi && roi.IsValid()) {
		SavePersistedRoi();
		RefreshOcrData();
		if (c->frame)
			c->frame->StatusTimeout(_("OCR region updated."), 2500);
	}
}

void VisualToolOCR::DrawRoiOverlay(VisualOcrRoi const& draw_roi) {
	if (!draw_roi.IsValid())
		return;

	auto const top_left = RoiTopLeft(draw_roi);
	auto const bottom_right = RoiBottomRight(draw_roi);
	wxColour color(0, 160, 255);

	if (has_roi && OPT_GET(OPT_OCR_VISUAL_TOOL_DIM_OUTSIDE_REGION)->GetBool()) {
		Vector2D const video_min = video_pos;
		Vector2D const video_max = video_pos + video_size;
		float const shaded_alpha = static_cast<float>(shaded_area_alpha_opt->GetDouble());
		gl.SetLineColour(*wxBLACK, 0.0f);
		gl.SetFillColour(*wxBLACK, shaded_alpha);
		gl.DrawRectangle(video_min, Vector2D(video_max, top_left));
		gl.DrawRectangle(Vector2D(video_min, bottom_right), video_max);
		gl.DrawRectangle(Vector2D(video_min, top_left), Vector2D(top_left, bottom_right));
		gl.DrawRectangle(Vector2D(bottom_right, top_left), Vector2D(video_max, bottom_right));
	}

	gl.SetLineColour(color, has_roi ? 0.95f : 0.6f, 2);

	gl.DrawLine(Vector2D(top_left.X(), video_pos.Y()), Vector2D(top_left.X(), video_pos.Y() + video_size.Y()));
	gl.DrawLine(Vector2D(bottom_right.X(), video_pos.Y()), Vector2D(bottom_right.X(), video_pos.Y() + video_size.Y()));
	gl.DrawLine(Vector2D(video_pos.X(), top_left.Y()), Vector2D(video_pos.X() + video_size.X(), top_left.Y()));
	gl.DrawLine(Vector2D(video_pos.X(), bottom_right.Y()), Vector2D(video_pos.X() + video_size.X(), bottom_right.Y()));

	VisualOcrRoiEdge const active_edge = dragging_roi ? dragged_roi_edge : hovered_roi_edge;
	if (active_edge == VisualOcrRoiEdge::None)
		return;

	gl.SetLineColour(color, 1.0f, 4);
	switch (active_edge) {
		case VisualOcrRoiEdge::Left:
			gl.DrawLine(Vector2D(top_left.X(), video_pos.Y()), Vector2D(top_left.X(), video_pos.Y() + video_size.Y()));
			break;
		case VisualOcrRoiEdge::Right:
			gl.DrawLine(Vector2D(bottom_right.X(), video_pos.Y()), Vector2D(bottom_right.X(), video_pos.Y() + video_size.Y()));
			break;
		case VisualOcrRoiEdge::Top:
			gl.DrawLine(Vector2D(video_pos.X(), top_left.Y()), Vector2D(video_pos.X() + video_size.X(), top_left.Y()));
			break;
		case VisualOcrRoiEdge::Bottom:
			gl.DrawLine(Vector2D(video_pos.X(), bottom_right.Y()), Vector2D(video_pos.X() + video_size.X(), bottom_right.Y()));
			break;
		case VisualOcrRoiEdge::None:
			break;
	}
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
	bool const using_roi = has_roi && roi.IsValid();
	if (using_roi)
		image = CropImageToRoi(image, roi);

	auto result = osx::ocr::RecognizeText(image);
	if (!result.error.empty()) {
		last_error = result.error;
		wxLogError(fmt_tl("OCR failed: %s", result.error));
		return;
	}
	if (using_roi)
		MapOcrResultFromRoi(result, roi);

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

	if (dragging_roi) {
		parent->SetCursor(wxCursor(dragged_roi_edge == VisualOcrRoiEdge::Left || dragged_roi_edge == VisualOcrRoiEdge::Right
			? wxCURSOR_SIZEWE
			: wxCURSOR_SIZENS));
		return;
	}

	if (hovered_roi_edge == VisualOcrRoiEdge::Left || hovered_roi_edge == VisualOcrRoiEdge::Right)
		parent->SetCursor(wxCursor(wxCURSOR_SIZEWE));
	else if (hovered_roi_edge == VisualOcrRoiEdge::Top || hovered_roi_edge == VisualOcrRoiEdge::Bottom)
		parent->SetCursor(wxCursor(wxCURSOR_SIZENS));
	else if (dragging_character_selection || hovered_character >= 0 || hovered_region >= 0)
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

	if (!c->editBox || !c->selectionController->GetActiveLine()) {
		if (c->frame)
			c->frame->StatusTimeout(_("No active subtitle line to insert into."), 3000);
		return;
	}

	switch (mode) {
		case InsertMode::ReplaceText:
			c->editBox->ReplaceActiveText(text);
			break;
		case InsertMode::ReplaceOriginal:
			c->editBox->ReplaceActiveOriginal(text);
			break;
		case InsertMode::AtCaret:
			c->editBox->InsertTextAtCaret(text);
			break;
	}

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

	bool const has_selection = HasSelection();
	bool const has_active_line = c->selectionController->GetActiveLine() != nullptr;
	bool const original_editable = c->editBox && c->editBox->IsOriginalEditable();
	SubsEditBox::CaretTarget const caret_target =
		c->editBox ? c->editBox->GetCaretTarget() : SubsEditBox::CaretTarget::None;

	wxMenu menu;
	menu.Append(ID_OCR_COPY, _("Copy"));
	menu.AppendCheckItem(ID_OCR_DIM_OUTSIDE_REGION, _("Dim outside OCR region"))
		->Check(OPT_GET(OPT_OCR_VISUAL_TOOL_DIM_OUTSIDE_REGION)->GetBool());
	if (has_roi)
		menu.Append(ID_OCR_CLEAR_REGION, _("Clear OCR region"));
	menu.AppendSeparator();
	// The two replace-line actions are always listed. Editing Original is gated
	// behind the Edit Original toggle, so that row stays visible but disabled
	// until the toggle is on.
	menu.Append(ID_OCR_REPLACE_TEXT, _("Replace current text line"));
	menu.Append(ID_OCR_REPLACE_ORIGINAL, _("Replace current original line"));

	// The caret-relative insert only appears when a caret is actually parked in
	// one of the edit boxes, and it names the box it will insert into.
	if (caret_target != SubsEditBox::CaretTarget::None) {
		menu.AppendSeparator();
		menu.Append(ID_OCR_INSERT_CARET, caret_target == SubsEditBox::CaretTarget::Original
			? _("Insert at cursor (original)")
			: _("Insert at cursor (text)"));
		menu.Enable(ID_OCR_INSERT_CARET, has_selection && has_active_line);
	}

	menu.Enable(ID_OCR_COPY, has_selection);
	menu.Enable(ID_OCR_REPLACE_TEXT, has_selection && has_active_line && c->editBox);
	menu.Enable(ID_OCR_REPLACE_ORIGINAL, has_selection && has_active_line && original_editable);

	menu.Bind(wxEVT_MENU, [this](wxCommandEvent &evt) {
		switch (evt.GetId()) {
			case ID_OCR_COPY:
				CopySelectedText();
				break;
			case ID_OCR_REPLACE_TEXT:
				InsertSelectedText(InsertMode::ReplaceText);
				break;
			case ID_OCR_REPLACE_ORIGINAL:
				InsertSelectedText(InsertMode::ReplaceOriginal);
				break;
			case ID_OCR_INSERT_CARET:
				InsertSelectedText(InsertMode::AtCaret);
				break;
			case ID_OCR_CLEAR_REGION:
				ClearRoi();
				if (c->frame)
					c->frame->StatusTimeout(_("OCR region cleared."), 2500);
				break;
			case ID_OCR_DIM_OUTSIDE_REGION:
				OPT_SET(OPT_OCR_VISUAL_TOOL_DIM_OUTSIDE_REGION)->SetBool(evt.IsChecked());
				parent->Render();
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
	if (dragging_roi)
		FinishRoiDrag();
	else
		RefreshOcrData();
	parent->Render();
}

void VisualToolOCR::OnCoordinateSystemsChanged() {
	parent->Render();
}

void VisualToolOCR::OnPlaybackStateChanged(bool is_playing) {
	bool const was_dragging_roi = dragging_roi;
	if (was_dragging_roi)
		FinishRoiDrag();
	dragging_character_selection = false;
	drag_additive_selection = false;
	drag_anchor_character = -1;
	drag_focus_character = -1;

	if (is_playing) {
		hovered_roi_edge = VisualOcrRoiEdge::None;
		hovered_region = -1;
		hovered_character = -1;
		UpdateCursor();
		parent->Render();
		return;
	}

	if (!was_dragging_roi)
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
		if (dragging_roi)
			return;
		mouse_pos = Vector2D();
		hovered_roi_edge = VisualOcrRoiEdge::None;
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

	hovered_roi_edge = HitTestRoiEdge(mouse_pos);
	if (hovered_roi_edge != VisualOcrRoiEdge::None) {
		hovered_character = -1;
		hovered_region = -1;
	}
	else {
		hovered_character = HitTestCharacter(mouse_pos);
		hovered_region = HitTestRegion(mouse_pos);
	}
	UpdateCursor();

	bool const is_multi_select = shift_down || ctrl_down || alt_down;

	if (event.LeftUp()) {
		if (dragging_roi) {
			AdjustRoiEdge(dragged_roi_edge, mouse_pos);
			FinishRoiDrag();
			hovered_roi_edge = HitTestRoiEdge(mouse_pos);
			UpdateCursor();
			parent->Render();
			return;
		}

		dragging_character_selection = false;
		drag_additive_selection = false;
		drag_anchor_character = -1;
		drag_focus_character = -1;
		return;
	}

	if (dragging_roi && event.Dragging() && event.LeftIsDown()) {
		AdjustRoiEdge(dragged_roi_edge, mouse_pos);
		parent->Render();
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
		if (hovered_roi_edge != VisualOcrRoiEdge::None) {
			dragging_character_selection = false;
			drag_additive_selection = false;
			drag_anchor_character = -1;
			drag_focus_character = -1;
			dragging_roi = true;
			dragged_roi_edge = hovered_roi_edge;
			if (!parent->HasCapture())
				parent->CaptureMouse();
			UpdateCursor();
			return;
		}

		if (hovered_character >= 0) {
			size_t const region_idx = characters[hovered_character].region_index;
			// A modifier-click (Shift/Ctrl/Cmd/Alt) on an already-selected
			// character toggles it off. A plain click must instead re-anchor a
			// fresh selection on that character, so it falls through to the
			// drag-selection path below.
			if (is_multi_select
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
	hovered_roi_edge = HitTestRoiEdge(mouse_pos);
	if (hovered_roi_edge != VisualOcrRoiEdge::None) {
		hovered_character = -1;
		hovered_region = -1;
	}
	else {
		hovered_character = HitTestCharacter(mouse_pos);
		hovered_region = HitTestRegion(mouse_pos);
	}
	UpdateCursor();

	if (!HasSelection() && hovered_character < 0 && hovered_region < 0 && !has_roi)
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

	DrawRoiOverlay(EditableRoi());
}
