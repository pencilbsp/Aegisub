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
	std::string last_error;

	enum class InsertMode {
		Replace,
		InsertBefore,
		InsertAfter
	};

	void RefreshOcrData();
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
