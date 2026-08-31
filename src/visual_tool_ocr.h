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

#pragma once

#include "visual_tool.h"

#include "osx/video_ocr.h"

#include <string>
#include <vector>

class wxMouseEvent;

enum class VisualOcrRoiEdge {
	None,
	Left,
	Right,
	Top,
	Bottom
};

struct VisualOcrRoi {
	double x = 0.0;
	double y = 0.0;
	double width = 1.0;
	double height = 1.0;

	bool IsValid() const {
		return width > 0.0 && height > 0.0;
	}
};

class VisualToolOCR final : public VisualToolBase {
	std::vector<osx::ocr::Region> regions;
	std::vector<osx::ocr::Character> characters;
	std::vector<bool> selected_regions;
	std::vector<bool> selected_characters;
	int hovered_region = -1;
	int hovered_character = -1;
	bool dragging_character_selection = false;
	bool drag_additive_selection = false;
	int drag_anchor_character = -1;
	int drag_focus_character = -1;
	bool has_roi = false;
	bool dragging_roi = false;
	VisualOcrRoiEdge hovered_roi_edge = VisualOcrRoiEdge::None;
	VisualOcrRoiEdge dragged_roi_edge = VisualOcrRoiEdge::None;
	VisualOcrRoi roi;
	std::string last_error;

	enum class InsertMode {
		ReplaceText,
		ReplaceOriginal,
		AtCaret
	};

	void RefreshOcrData();
	void LoadPersistedRoi();
	void SavePersistedRoi() const;
	void ClearRoi();
	bool IsInsideVideo(Vector2D pos) const;
	Vector2D ClampToVideo(Vector2D pos) const;
	VisualOcrRoi EditableRoi() const;
	Vector2D RoiTopLeft(VisualOcrRoi const& draw_roi) const;
	Vector2D RoiBottomRight(VisualOcrRoi const& draw_roi) const;
	VisualOcrRoiEdge HitTestRoiEdge(Vector2D pos) const;
	void AdjustRoiEdge(VisualOcrRoiEdge edge, Vector2D pos);
	void FinishRoiDrag();
	void DrawRoiOverlay(VisualOcrRoi const& draw_roi);
	int HitTestRegion(Vector2D pos) const;
	int HitTestCharacter(Vector2D pos) const;
	std::vector<size_t> SortedCharacterIndicesForRegion(size_t region_index) const;
	void SelectCharacterRange(int anchor_index, int focus_index, bool additive);
	void UpdateCursor();
	std::string SelectedText() const;
	bool HasSelection() const;
	void CopySelectedText();
	void InsertSelectedText(InsertMode mode);
	void OpenContextMenu(Vector2D mouse_point);
	void OnPlaybackStateChanged(bool is_playing);

	void OnFrameChanged() override;
	void OnCoordinateSystemsChanged() override;

public:
	VisualToolOCR(VideoDisplay *parent, agi::Context *context);
	~VisualToolOCR() override;

	void OnMouseEvent(wxMouseEvent &event) override;
	bool OnContextMenu(wxContextMenuEvent &event) override;
	bool OnKeyDown(wxKeyEvent &event) override;
	void Draw() override;
};
