// Copyright (c) 2005-2010, Niels Martin Hansen
// Copyright (c) 2005-2010, Rodrigo Braz Monteiro
// Copyright (c) 2010, Amar Takhar
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//	 this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//	 this list of conditions and the following disclaimer in the documentation
//	 and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//	 may be used to endorse or promote products derived from this software
//	 without specific prior written permission.
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

#include "command.h"

#include "../ass_file.h"
#include "../ass_dialogue.h"
#include "../async_video_provider.h"
#include "../compat.h"
#include "../dialog_detached_video.h"
#include "../dialog_manager.h"
#include "../dialogs.h"
#include "../format.h"
#include "../frame_main.h"
#include "../include/aegisub/context.h"
#include "../include/aegisub/subtitles_provider.h"
#include "../libresrc/libresrc.h"
#include "../options.h"
#include "../project.h"
#include "../selection_controller.h"
#include "../utils.h"
#include "../video_controller.h"
#include "../video_display.h"
#include "../video_frame.h"
#ifdef __APPLE__
#include "../osx/video_ocr.h"
#endif

#include <libaegisub/ass/time.h>
#include <libaegisub/fs.h>
#include <libaegisub/path.h>
#include <libaegisub/util.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <future>
#include <thread>
#include <wx/app.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dcbuffer.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textdlg.h>

namespace {
	using cmd::Command;

struct validator_video_loaded : public Command {
	CMD_TYPE(COMMAND_VALIDATE)
	bool Validate(const agi::Context *c) override {
		return !!c->project->VideoProvider();
	}
};

struct validator_video_attached : public Command {
	CMD_TYPE(COMMAND_VALIDATE)
	bool Validate(const agi::Context *c) override {
		return !!c->project->VideoProvider() && !c->dialog->Get<DialogDetachedVideo>();
	}
};

struct video_aspect_cinematic final : public validator_video_loaded {
	CMD_NAME("video/aspect/cinematic")
	STR_MENU("&Cinematic (2.35)")
	STR_DISP("Cinematic (2.35)")
	STR_HELP("Force video to 2.35 aspect ratio")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

	bool IsActive(const agi::Context *c) override {
		return c->videoController->GetAspectRatioType() == AspectRatio::Cinematic;
	}

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		c->videoController->SetAspectRatio(AspectRatio::Cinematic);
		c->frame->SetDisplayMode(1,-1);
	}
};

struct video_aspect_custom final : public validator_video_loaded {
	CMD_NAME("video/aspect/custom")
	STR_MENU("C&ustom...")
	STR_DISP("Custom")
	STR_HELP("Force video to a custom aspect ratio")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

	bool IsActive(const agi::Context *c) override {
		return c->videoController->GetAspectRatioType() == AspectRatio::Custom;
	}

	void operator()(agi::Context *c) override {
		c->videoController->Stop();

		std::string value = from_wx(wxGetTextFromUser(
			_("Enter aspect ratio in either:\n  decimal (e.g. 2.35)\n  fractional (e.g. 16:9)\n  specific resolution (e.g. 853x480)"),
			_("Enter aspect ratio"),
			std::to_wstring(c->videoController->GetAspectRatioValue())));
		if (value.empty()) return;

		double numval = 0;
		if (agi::util::try_parse(value, &numval)) {
			//Nothing to see here, move along
		}
		else {
			std::vector<std::string> chunks;
			split(chunks, value, boost::is_any_of(":/xX"));
			if (chunks.size() == 2) {
				double num, den;
				if (agi::util::try_parse(chunks[0], &num) && agi::util::try_parse(chunks[1], &den))
					numval = num / den;
			}
		}

		if (numval < 0.5 || numval > 5.0)
			wxMessageBox(_("Invalid value! Aspect ratio must be between 0.5 and 5.0."),_("Invalid Aspect Ratio"),wxOK | wxICON_ERROR | wxCENTER);
		else {
			c->videoController->SetAspectRatio(numval);
			c->frame->SetDisplayMode(1,-1);
		}
	}
};

struct video_aspect_default final : public validator_video_loaded {
	CMD_NAME("video/aspect/default")
	STR_MENU("&Default")
	STR_DISP("Default")
	STR_HELP("Use video's original aspect ratio")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

	bool IsActive(const agi::Context *c) override {
		return c->videoController->GetAspectRatioType() == AspectRatio::Default;
	}

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		c->videoController->SetAspectRatio(AspectRatio::Default);
		c->frame->SetDisplayMode(1,-1);
	}
};

struct video_aspect_full final : public validator_video_loaded {
	CMD_NAME("video/aspect/full")
	STR_MENU("&Fullscreen (4:3)")
	STR_DISP("Fullscreen (4:3)")
	STR_HELP("Force video to 4:3 aspect ratio")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

	bool IsActive(const agi::Context *c) override {
		return c->videoController->GetAspectRatioType() == AspectRatio::Fullscreen;
	}

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		c->videoController->SetAspectRatio(AspectRatio::Fullscreen);
		c->frame->SetDisplayMode(1,-1);
	}
};

struct video_aspect_wide final : public validator_video_loaded {
	CMD_NAME("video/aspect/wide")
	STR_MENU("&Widescreen (16:9)")
	STR_DISP("Widescreen (16:9)")
	STR_HELP("Force video to 16:9 aspect ratio")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

	bool IsActive(const agi::Context *c) override {
		return c->videoController->GetAspectRatioType() == AspectRatio::Widescreen;
	}

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		c->videoController->SetAspectRatio(AspectRatio::Widescreen);
		c->frame->SetDisplayMode(1,-1);
	}
};

struct video_close final : public validator_video_loaded {
	CMD_NAME("video/close")
	CMD_ICON(close_video_menu)
	STR_MENU("&Close Video")
	STR_DISP("Close Video")
	STR_HELP("Close the currently open video file")

	void operator()(agi::Context *c) override {
		c->project->CloseVideo();
	}
};

struct video_copy_coordinates final : public validator_video_loaded {
	CMD_NAME("video/copy_coordinates")
	STR_MENU("Copy coordinates to Clipboard")
	STR_DISP("Copy coordinates to Clipboard")
	STR_HELP("Copy the current coordinates of the mouse over the video to the clipboard")

	void operator()(agi::Context *c) override {
		SetClipboard(c->videoDisplay->GetMousePosition().Str());
	}
};

struct video_cycle_subtitles_provider final : public cmd::Command {
	CMD_NAME("video/subtitles_provider/cycle")
	STR_MENU("Cycle active subtitles provider")
	STR_DISP("Cycle active subtitles provider")
	STR_HELP("Cycle through the available subtitles providers")

	void operator()(agi::Context *c) override {
		auto providers = SubtitlesProviderFactory::GetClasses();
		if (providers.empty()) return;

		auto it = find(begin(providers), end(providers), OPT_GET("Subtitle/Provider")->GetString());
		if (it != end(providers)) ++it;
		if (it == end(providers)) it = begin(providers);

		OPT_SET("Subtitle/Provider")->SetString(*it);
		c->frame->StatusTimeout(fmt_tl("Subtitles provider set to %s", *it), 5000);
	}
};

struct video_detach final : public validator_video_loaded {
	CMD_NAME("video/detach")
	CMD_ICON(detach_video_menu)
	STR_MENU("&Detach Video")
	STR_DISP("Detach Video")
	STR_HELP("Detach the video display from the main window, displaying it in a separate Window")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_TOGGLE)

	bool IsActive(const agi::Context *c) override {
		return !!c->dialog->Get<DialogDetachedVideo>();
	}

	void operator()(agi::Context *c) override {
		if (DialogDetachedVideo *d = c->dialog->Get<DialogDetachedVideo>())
			d->Close();
		else
			c->dialog->Show<DialogDetachedVideo>(c);
	}
};

struct video_details final : public validator_video_loaded {
	CMD_NAME("video/details")
	CMD_ICON(show_video_details_menu)
	STR_MENU("Show &Video Details")
	STR_DISP("Show Video Details")
	STR_HELP("Show video details")

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		ShowVideoDetailsDialog(c);
	}
};

struct video_focus_seek final : public validator_video_loaded {
	CMD_NAME("video/focus_seek")
	STR_MENU("Toggle video slider focus")
	STR_DISP("Toggle video slider focus")
	STR_HELP("Toggle focus between the video slider and the previous thing to have focus")

	void operator()(agi::Context *c) override {
		wxWindow *curFocus = wxWindow::FindFocus();
		if (curFocus == c->videoSlider) {
			if (c->previousFocus) c->previousFocus->SetFocus();
		}
		else {
			c->previousFocus = curFocus;
			c->videoSlider->SetFocus();
		}
	}
};

wxImage get_image(agi::Context *c, bool raw, bool subsonly = false) {
	auto frame = c->videoController->GetFrameN();
	if (subsonly) {
		return GetImageWithAlpha(c->project->VideoProvider()->GetSubtitles(c->project->Timecodes().TimeAtFrame(frame)));
	} else {
		return GetImage(*c->project->VideoProvider()->GetFrame(frame, c->project->Timecodes().TimeAtFrame(frame), raw));
	}
}

wxImage get_image_at_frame(agi::Context *c, int frame, bool raw) {
	auto *provider = c->project->VideoProvider();
	if (!provider)
		return {};

	int const frame_count = provider->GetFrameCount();
	if (frame_count <= 0)
		return {};

	frame = std::clamp(frame, 0, frame_count - 1);
	int const frame_time = c->project->Timecodes().TimeAtFrame(frame, agi::vfr::START);
	return GetImage(*provider->GetFrame(frame, frame_time, raw));
}

#ifdef __APPLE__
enum class OcrLineFrameMode {
	First,
	Middle,
	Last
};

enum class OcrLineInsertMode {
	InsertBefore,
	InsertAfter,
	Replace
};

struct OcrNormalizedRect {
	double x = 0.0;
	double y = 0.0;
	double width = 1.0;
	double height = 1.0;

	bool IsValid() const {
		return width > 0.0 && height > 0.0;
	}
};

class OcrRoiPickerDialog final : public wxDialog {
	enum class DragMode {
		None,
		NewSelection,
		LeftEdge,
		RightEdge,
		TopEdge,
		BottomEdge
	};

	agi::Context *context = nullptr;
	wxImage image;
	wxPanel *canvas = nullptr;
	wxStaticText *frame_label = nullptr;
	int frame = 0;
	int frame_count = 0;
	bool has_selection = false;
	bool dragging = false;
	DragMode drag_mode = DragMode::None;
	wxPoint drag_anchor_image;
	wxRect selection_image;

	OcrNormalizedRect GetSelectionNormalizedInternal() const {
		if (!has_selection || selection_image.IsEmpty() || !image.IsOk())
			return {};

		return {
			static_cast<double>(selection_image.x) / image.GetWidth(),
			static_cast<double>(selection_image.y) / image.GetHeight(),
			static_cast<double>(selection_image.width) / image.GetWidth(),
			static_cast<double>(selection_image.height) / image.GetHeight()
		};
	}

	void SetSelectionFromNormalized(OcrNormalizedRect const& normalized) {
		if (!image.IsOk() || !normalized.IsValid()) {
			has_selection = false;
			selection_image = {};
			return;
		}

		int const width = image.GetWidth();
		int const height = image.GetHeight();
		int x = std::clamp(static_cast<int>(std::lround(normalized.x * width)), 0, width - 1);
		int y = std::clamp(static_cast<int>(std::lround(normalized.y * height)), 0, height - 1);
		int w = std::clamp(static_cast<int>(std::lround(normalized.width * width)), 1, width - x);
		int h = std::clamp(static_cast<int>(std::lround(normalized.height * height)), 1, height - y);
		selection_image = wxRect(x, y, w, h);
		has_selection = true;
	}

	void SetSelectionFullFrame() {
		if (!image.IsOk() || image.GetWidth() <= 0 || image.GetHeight() <= 0) {
			has_selection = false;
			selection_image = {};
			return;
		}

		selection_image = wxRect(0, 0, image.GetWidth(), image.GetHeight());
		has_selection = true;
	}

	void UpdateFrameLabel() {
		if (!frame_label)
			return;

		if (frame_count > 0)
			frame_label->SetLabel(fmt_tl("Frame %d/%d", frame + 1, frame_count));
		else
			frame_label->SetLabel(_("Frame N/A"));
	}

	bool LoadFrame(int new_frame) {
		if (!context)
			return false;

		auto *provider = context->project->VideoProvider();
		if (!provider)
			return false;

		int const count = provider->GetFrameCount();
		if (count <= 0)
			return false;

		frame_count = count;
		new_frame = std::clamp(new_frame, 0, frame_count - 1);

		OcrNormalizedRect previous_selection = GetSelectionNormalizedInternal();
		wxImage next_image = get_image_at_frame(context, new_frame, true);
		if (!next_image.IsOk())
			return false;

		image = std::move(next_image);
		frame = new_frame;
		if (previous_selection.IsValid())
			SetSelectionFromNormalized(previous_selection);
		else
			SetSelectionFullFrame();
		UpdateFrameLabel();

		if (canvas)
			canvas->Refresh(false);
		return true;
	}

	wxRect GetImageDrawRect() const {
		if (!canvas || !image.IsOk())
			return {};

		wxSize client = canvas->GetClientSize();
		if (client.GetWidth() <= 0 || client.GetHeight() <= 0)
			return {};

		int const margin = 8;
		int const available_width = std::max(1, client.GetWidth() - margin * 2);
		int const available_height = std::max(1, client.GetHeight() - margin * 2);
		double const scale = std::min(
			static_cast<double>(available_width) / image.GetWidth(),
			static_cast<double>(available_height) / image.GetHeight());

		int draw_width = std::max(1, static_cast<int>(std::lround(image.GetWidth() * scale)));
		int draw_height = std::max(1, static_cast<int>(std::lround(image.GetHeight() * scale)));
		int draw_x = (client.GetWidth() - draw_width) / 2;
		int draw_y = (client.GetHeight() - draw_height) / 2;
		return wxRect(draw_x, draw_y, draw_width, draw_height);
	}

	bool IsInsideImage(wxPoint point) const {
		return GetImageDrawRect().Contains(point);
	}

	wxPoint ClientToImage(wxPoint point) const {
		wxRect draw_rect = GetImageDrawRect();
		if (!draw_rect.IsEmpty()) {
			int x = (point.x - draw_rect.x) * image.GetWidth() / draw_rect.width;
			int y = (point.y - draw_rect.y) * image.GetHeight() / draw_rect.height;
			x = std::clamp(x, 0, image.GetWidth() - 1);
			y = std::clamp(y, 0, image.GetHeight() - 1);
			return wxPoint(x, y);
		}
		return {};
	}

	wxRect ImageToClient(wxRect image_rect) const {
		wxRect draw_rect = GetImageDrawRect();
		if (draw_rect.IsEmpty() || image_rect.IsEmpty())
			return {};

		int x = draw_rect.x + image_rect.x * draw_rect.width / image.GetWidth();
		int y = draw_rect.y + image_rect.y * draw_rect.height / image.GetHeight();
		int width = std::max(1, image_rect.width * draw_rect.width / image.GetWidth());
		int height = std::max(1, image_rect.height * draw_rect.height / image.GetHeight());
		return wxRect(x, y, width, height);
	}

	DragMode HitTestDragMode(wxPoint point) const {
		if (!has_selection || selection_image.IsEmpty() || !IsInsideImage(point))
			return DragMode::None;

		wxRect selection_client = ImageToClient(selection_image);
		if (selection_client.IsEmpty())
			return DragMode::None;

		int const threshold = 8;
		DragMode mode = DragMode::None;
		int best_distance = threshold + 1;

		auto try_candidate = [&](int distance, DragMode candidate) {
			if (distance <= threshold && distance < best_distance) {
				best_distance = distance;
				mode = candidate;
			}
		};

		try_candidate(std::abs(point.x - selection_client.GetLeft()), DragMode::LeftEdge);
		try_candidate(std::abs(point.x - selection_client.GetRight()), DragMode::RightEdge);
		try_candidate(std::abs(point.y - selection_client.GetTop()), DragMode::TopEdge);
		try_candidate(std::abs(point.y - selection_client.GetBottom()), DragMode::BottomEdge);
		return mode;
	}

	void UpdateSelection(wxPoint image_point) {
		int x1 = std::min(drag_anchor_image.x, image_point.x);
		int y1 = std::min(drag_anchor_image.y, image_point.y);
		int x2 = std::max(drag_anchor_image.x, image_point.x);
		int y2 = std::max(drag_anchor_image.y, image_point.y);
		selection_image = wxRect(x1, y1, x2 - x1 + 1, y2 - y1 + 1);
		has_selection = true;
		if (canvas)
			canvas->Refresh(false);
	}

	void AdjustSelectionEdge(DragMode mode, wxPoint image_point) {
		if (!has_selection || selection_image.IsEmpty() || !image.IsOk())
			return;

		int const min_size = 8;
		int left = selection_image.x;
		int top = selection_image.y;
		int right = selection_image.x + selection_image.width - 1;
		int bottom = selection_image.y + selection_image.height - 1;

		switch (mode) {
			case DragMode::LeftEdge: {
				int const new_left = std::clamp(image_point.x, 0, right - min_size);
				left = new_left;
				break;
			}
			case DragMode::RightEdge: {
				int const new_right = std::clamp(image_point.x, left + min_size, image.GetWidth() - 1);
				right = new_right;
				break;
			}
			case DragMode::TopEdge: {
				int const new_top = std::clamp(image_point.y, 0, bottom - min_size);
				top = new_top;
				break;
			}
			case DragMode::BottomEdge: {
				int const new_bottom = std::clamp(image_point.y, top + min_size, image.GetHeight() - 1);
				bottom = new_bottom;
				break;
			}
			default:
				return;
		}

		selection_image = wxRect(left, top, right - left + 1, bottom - top + 1);
		has_selection = true;
		if (canvas)
			canvas->Refresh(false);
	}

	void ClearSelection() {
		SetSelectionFullFrame();
		if (canvas)
			canvas->Refresh(false);
	}

	void OnPaint(wxPaintEvent&) {
		wxAutoBufferedPaintDC dc(canvas);
		dc.Clear();

		if (!image.IsOk())
			return;

		wxRect draw_rect = GetImageDrawRect();
		if (draw_rect.IsEmpty())
			return;

		wxImage scaled = image.Scale(draw_rect.width, draw_rect.height, wxIMAGE_QUALITY_HIGH);
		dc.DrawBitmap(wxBitmap(scaled), draw_rect.x, draw_rect.y, false);

		if (has_selection && !selection_image.IsEmpty()) {
			wxRect selection_client = ImageToClient(selection_image);
			wxPen const guide_pen(wxColour(0, 160, 255), 2);
			dc.SetPen(guide_pen);
			dc.SetBrush(*wxTRANSPARENT_BRUSH);
			dc.DrawRectangle(selection_client);
			dc.DrawLine(selection_client.GetLeft(), draw_rect.GetTop(), selection_client.GetLeft(), draw_rect.GetBottom());
			dc.DrawLine(selection_client.GetRight(), draw_rect.GetTop(), selection_client.GetRight(), draw_rect.GetBottom());
			dc.DrawLine(draw_rect.GetLeft(), selection_client.GetTop(), draw_rect.GetRight(), selection_client.GetTop());
			dc.DrawLine(draw_rect.GetLeft(), selection_client.GetBottom(), draw_rect.GetRight(), selection_client.GetBottom());
		}
	}

	void OnLeftDown(wxMouseEvent &event) {
		if (!IsInsideImage(event.GetPosition()))
			return;

		canvas->SetFocus();
		drag_mode = HitTestDragMode(event.GetPosition());
		if (drag_mode == DragMode::None)
			drag_mode = DragMode::NewSelection;
		dragging = true;
		drag_anchor_image = ClientToImage(event.GetPosition());
		if (drag_mode == DragMode::NewSelection)
			UpdateSelection(drag_anchor_image);
		canvas->CaptureMouse();
	}

	void OnLeftUp(wxMouseEvent &event) {
		if (!dragging)
			return;

		dragging = false;
		if (canvas->HasCapture())
			canvas->ReleaseMouse();
		if (IsInsideImage(event.GetPosition())) {
			wxPoint image_point = ClientToImage(event.GetPosition());
			if (drag_mode == DragMode::NewSelection)
				UpdateSelection(image_point);
			else
				AdjustSelectionEdge(drag_mode, image_point);
		}
		drag_mode = DragMode::None;
	}

	void OnMotion(wxMouseEvent &event) {
		if (!dragging || !event.LeftIsDown())
			return;

		wxPoint point = event.GetPosition();
		wxRect draw_rect = GetImageDrawRect();
		point.x = std::clamp(point.x, draw_rect.x, draw_rect.x + draw_rect.width - 1);
		point.y = std::clamp(point.y, draw_rect.y, draw_rect.y + draw_rect.height - 1);
		wxPoint image_point = ClientToImage(point);
		if (drag_mode == DragMode::NewSelection)
			UpdateSelection(image_point);
		else
			AdjustSelectionEdge(drag_mode, image_point);
	}

	void OnKeyDown(wxKeyEvent &event) {
		int delta = 0;
		switch (event.GetKeyCode()) {
			case WXK_LEFT: delta = -1; break;
			case WXK_RIGHT: delta = 1; break;
			case WXK_UP: delta = -10; break;
			case WXK_DOWN: delta = 10; break;
			default:
				break;
		}

		if (delta == 0) {
			event.Skip();
			return;
		}

		if (!LoadFrame(frame + delta))
			wxBell();
	}

	void OnClear(wxCommandEvent&) {
		ClearSelection();
	}

	void OnOk(wxCommandEvent &event) {
		if (!has_selection) {
			wxMessageBox(_("Please select an OCR region first."), _("OCR Region"), wxOK | wxICON_INFORMATION | wxCENTER, this);
			return;
		}
		event.Skip();
	}

public:
	OcrRoiPickerDialog(wxWindow *parent, agi::Context *context, int initial_frame, bool has_initial_selection, OcrNormalizedRect const& initial_selection)
	: wxDialog(parent, wxID_ANY, _("Select OCR Region"), wxDefaultPosition, wxSize(960, 620), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, context(context)
	, frame(initial_frame)
	{
		if (context && context->project->VideoProvider())
			frame_count = context->project->VideoProvider()->GetFrameCount();
		if (frame_count > 0)
			frame = std::clamp(frame, 0, frame_count - 1);
		else
			frame = 0;

		auto *main_sizer = new wxBoxSizer(wxVERTICAL);
		main_sizer->Add(new wxStaticText(this, wxID_ANY, _("Drag ROI border lines (2 vertical + 2 horizontal) to set OCR region.")),
			0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
		main_sizer->Add(new wxStaticText(this, wxID_ANY, _("Use arrow keys: Left/Right = +/-1 frame, Up/Down = +/-10 frames.")),
			0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
		frame_label = new wxStaticText(this, wxID_ANY, _("Frame N/A"));
		main_sizer->Add(frame_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

		canvas = new wxPanel(this, wxID_ANY);
		canvas->SetBackgroundStyle(wxBG_STYLE_PAINT);
		canvas->SetMinSize(wxSize(860, 460));
		main_sizer->Add(canvas, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

		auto *button_row = new wxBoxSizer(wxHORIZONTAL);
		button_row->Add(new wxButton(this, wxID_CLEAR, _("Clear")), 0, wxRIGHT, 8);
		button_row->AddStretchSpacer();
		button_row->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxALIGN_CENTER_VERTICAL);
		main_sizer->Add(button_row, 0, wxEXPAND | wxALL, 10);

		SetSizerAndFit(main_sizer);
		CentreOnParent();

		canvas->Bind(wxEVT_PAINT, &OcrRoiPickerDialog::OnPaint, this);
		canvas->Bind(wxEVT_LEFT_DOWN, &OcrRoiPickerDialog::OnLeftDown, this);
		canvas->Bind(wxEVT_LEFT_UP, &OcrRoiPickerDialog::OnLeftUp, this);
		canvas->Bind(wxEVT_MOTION, &OcrRoiPickerDialog::OnMotion, this);
		Bind(wxEVT_CHAR_HOOK, &OcrRoiPickerDialog::OnKeyDown, this);
		Bind(wxEVT_BUTTON, &OcrRoiPickerDialog::OnClear, this, wxID_CLEAR);
		Bind(wxEVT_BUTTON, &OcrRoiPickerDialog::OnOk, this, wxID_OK);

		LoadFrame(frame);
		if (has_initial_selection && initial_selection.IsValid())
			SetSelectionFromNormalized(initial_selection);
		else
			SetSelectionFullFrame();
		UpdateFrameLabel();
		canvas->SetFocus();
	}

	bool HasSelection() const {
		return has_selection && !selection_image.IsEmpty();
	}

	int GetFrame() const {
		return frame;
	}

	OcrNormalizedRect GetSelection() const {
		return GetSelectionNormalizedInternal();
	}
};

class OcrSelectedLinesDialog final : public wxDialog {
	wxChoice *frame_mode;
	wxChoice *insert_mode;
	wxChoice *language_mode;
	wxCheckBox *use_roi = nullptr;
	wxStaticText *roi_summary = nullptr;
	agi::Context *context = nullptr;
	int roi_preview_frame = 0;
	std::vector<std::string> supported_languages;
	bool has_roi_selection = false;
	OcrNormalizedRect roi_selection;

	void UpdateRoiSummary() {
		if (!roi_summary)
			return;

		if (!has_roi_selection || !roi_selection.IsValid()) {
			roi_summary->SetLabel(_("Full frame"));
			return;
		}

		roi_summary->SetLabel(fmt_tl("x=%.1f%% y=%.1f%% w=%.1f%% h=%.1f%%",
			roi_selection.x * 100.0,
			roi_selection.y * 100.0,
			roi_selection.width * 100.0,
			roi_selection.height * 100.0));
	}

	void OnPickRegion(wxCommandEvent&) {
		if (!context || !context->project->VideoProvider()) {
			wxMessageBox(_("Could not load video frame for ROI selection."), _("OCR Region"), wxOK | wxICON_ERROR | wxCENTER, this);
			return;
		}

		OcrRoiPickerDialog picker(this, context, roi_preview_frame, has_roi_selection, roi_selection);
		if (picker.ShowModal() != wxID_OK)
			return;

		if (!picker.HasSelection())
			return;

		roi_selection = picker.GetSelection();
		roi_preview_frame = picker.GetFrame();
		has_roi_selection = roi_selection.IsValid();
		use_roi->SetValue(has_roi_selection);
		UpdateRoiSummary();
		Layout();
		Fit();
	}

	void OnOk(wxCommandEvent &event) {
		if (use_roi->GetValue() && !has_roi_selection) {
			wxMessageBox(_("Please pick an OCR region or disable \"Use OCR region\"."), _("OCR Region"), wxOK | wxICON_INFORMATION | wxCENTER, this);
			return;
		}
		event.Skip();
	}

public:
	OcrSelectedLinesDialog(wxWindow *parent, agi::Context *context, std::vector<std::string> languages)
	: wxDialog(parent, wxID_ANY, _("OCR selected Lines"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, context(context)
	, roi_preview_frame(context ? context->videoController->GetFrameN() : 0)
	, supported_languages(std::move(languages))
	{
		auto *main_sizer = new wxBoxSizer(wxVERTICAL);
		auto *grid = new wxFlexGridSizer(2, 6, 8);
		grid->AddGrowableCol(1, 1);

		grid->Add(new wxStaticText(this, wxID_ANY, _("OCR frame:")), 0, wxALIGN_CENTER_VERTICAL);
		frame_mode = new wxChoice(this, wxID_ANY);
		frame_mode->Append(_("First frame of each line"));
		frame_mode->Append(_("Middle frame of each line"));
		frame_mode->Append(_("Last frame of each line"));
		frame_mode->SetSelection(1);
		grid->Add(frame_mode, 1, wxEXPAND);

		grid->Add(new wxStaticText(this, wxID_ANY, _("Insert mode:")), 0, wxALIGN_CENTER_VERTICAL);
		insert_mode = new wxChoice(this, wxID_ANY);
		insert_mode->Append(_("Insert before"));
		insert_mode->Append(_("Insert after"));
		insert_mode->Append(_("Replace line text"));
		insert_mode->SetSelection(2);
		grid->Add(insert_mode, 1, wxEXPAND);

		grid->Add(new wxStaticText(this, wxID_ANY, _("Language:")), 0, wxALIGN_CENTER_VERTICAL);
		language_mode = new wxChoice(this, wxID_ANY);
		language_mode->Append(_("Auto"));
		for (auto const& language : supported_languages)
			language_mode->Append(to_wx(language));
		language_mode->SetSelection(0);
		grid->Add(language_mode, 1, wxEXPAND);

		grid->Add(new wxStaticText(this, wxID_ANY, _("OCR region:")), 0, wxALIGN_CENTER_VERTICAL);
		auto *roi_box = new wxBoxSizer(wxHORIZONTAL);
		use_roi = new wxCheckBox(this, wxID_ANY, _("Use OCR region"));
		roi_box->Add(use_roi, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
		auto *pick_roi = new wxButton(this, wxID_ANY, _("Pick region..."));
		roi_box->Add(pick_roi, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
		roi_summary = new wxStaticText(this, wxID_ANY, _("Full frame"));
		roi_box->Add(roi_summary, 1, wxALIGN_CENTER_VERTICAL);
		grid->Add(roi_box, 1, wxEXPAND);

		main_sizer->Add(grid, 1, wxEXPAND | wxALL, 10);
		main_sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

		SetSizerAndFit(main_sizer);
		CentreOnParent();
		UpdateRoiSummary();

		pick_roi->Bind(wxEVT_BUTTON, &OcrSelectedLinesDialog::OnPickRegion, this);
		Bind(wxEVT_BUTTON, &OcrSelectedLinesDialog::OnOk, this, wxID_OK);
	}

	OcrLineFrameMode GetFrameMode() const {
		switch (frame_mode->GetSelection()) {
			case 0: return OcrLineFrameMode::First;
			case 2: return OcrLineFrameMode::Last;
			default: return OcrLineFrameMode::Middle;
		}
	}

	OcrLineInsertMode GetInsertMode() const {
		switch (insert_mode->GetSelection()) {
			case 0: return OcrLineInsertMode::InsertBefore;
			case 1: return OcrLineInsertMode::InsertAfter;
			default: return OcrLineInsertMode::Replace;
		}
	}

	bool IsAutoLanguage() const {
		return language_mode->GetSelection() <= 0;
	}

	std::string GetLanguageCode() const {
		int const selection = language_mode->GetSelection();
		if (selection <= 0)
			return {};

		size_t const language_index = static_cast<size_t>(selection - 1);
		if (language_index >= supported_languages.size())
			return {};

		return supported_languages[language_index];
	}

	bool UseRoi() const {
		return use_roi && use_roi->GetValue() && has_roi_selection && roi_selection.IsValid();
	}

	OcrNormalizedRect GetRoi() const {
		return roi_selection;
	}
};

int frame_for_line(agi::Context *c, AssDialogue const* line, OcrLineFrameMode mode) {
	int start_frame = c->videoController->FrameAtTime(line->Start, agi::vfr::START);
	int end_frame = c->videoController->FrameAtTime(line->End, agi::vfr::END);
	if (start_frame > end_frame)
		std::swap(start_frame, end_frame);

	switch (mode) {
		case OcrLineFrameMode::First:
			return start_frame;
		case OcrLineFrameMode::Last:
			return end_frame;
		case OcrLineFrameMode::Middle:
		default:
			return start_frame + (end_frame - start_frame) / 2;
	}
}

std::string merge_ocr_text(std::string const& existing_text, std::string const& ocr_text, OcrLineInsertMode mode) {
	bool const has_existing_text = !existing_text.empty();

	switch (mode) {
		case OcrLineInsertMode::InsertBefore:
			return has_existing_text ? ocr_text + "\\N" + existing_text : ocr_text;
		case OcrLineInsertMode::InsertAfter:
			return has_existing_text ? existing_text + "\\N" + ocr_text : ocr_text;
		case OcrLineInsertMode::Replace:
		default:
			return ocr_text;
	}
}

wxImage crop_image_to_roi(wxImage const& image, OcrNormalizedRect const& roi) {
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
#endif

struct video_frame_copy final : public validator_video_loaded {
	CMD_NAME("video/frame/copy")
	STR_MENU("Copy image to Clipboard")
	STR_DISP("Copy image to Clipboard")
	STR_HELP("Copy the currently displayed frame to the clipboard")

	void operator()(agi::Context *c) override {
		SetClipboard(wxBitmap(get_image(c, false), 24));
	}
};

struct video_frame_copy_raw final : public validator_video_loaded {
	CMD_NAME("video/frame/copy/raw")
	STR_MENU("Copy image to Clipboard (no subtitles)")
	STR_DISP("Copy image to Clipboard (no subtitles)")
	STR_HELP("Copy the currently displayed frame to the clipboard, without the subtitles")

	void operator()(agi::Context *c) override {
		SetClipboard(wxBitmap(get_image(c, true), 24));
	}
};

struct video_frame_copy_subs final : public validator_video_loaded {
	CMD_NAME("video/frame/copy/subs")
	STR_MENU("Copy image to Clipboard (only subtitles)")
	STR_DISP("Copy image to Clipboard (only subtitles)")
	STR_HELP("Copy the currently displayed subtitles to the clipboard, with transparent background")

	void operator()(agi::Context *c) override {
		SetClipboard(wxBitmap(get_image(c, false, true), 32));
	}
};

#ifdef __APPLE__
struct video_frame_ocr final : public validator_video_loaded {
	CMD_NAME("video/frame/ocr")
	CMD_ICON(ocr_button)
	STR_MENU("OCR Current Frame")
	STR_DISP("OCR Current Frame")
	STR_HELP("Recognize text from the current frame and put it in the active subtitle line")
	CMD_TYPE(COMMAND_VALIDATE)

	bool Validate(const agi::Context *c) override {
		return validator_video_loaded::Validate(c) && !c->videoController->IsPlaying();
	}

	void operator()(agi::Context *c) override {
		auto result = osx::ocr::RecognizeText(get_image(c, true));
		if (!result.error.empty()) {
			wxLogError(fmt_tl("OCR failed: %s", result.error));
			return;
		}

		if (result.text.empty()) {
			c->frame->StatusTimeout(_("No text detected in the current frame."), 5000);
			return;
		}

		SetClipboard(result.text);

		if (auto *line = c->selectionController->GetActiveLine()) {
			line->Text = result.text;
			c->ass->Commit(_("ocr frame"), AssFile::COMMIT_DIAG_TEXT, -1, line);
			c->frame->StatusTimeout(_("OCR inserted into the active line and copied to clipboard."), 5000);
		}
		else {
			c->frame->StatusTimeout(_("OCR result copied to clipboard."), 5000);
		}
	}
};

struct video_ocr_selected_lines final : public validator_video_loaded {
	CMD_NAME("video/ocr/selected_lines")
	STR_MENU("OCR selected Lines")
	STR_DISP("OCR selected Lines")
	STR_HELP("Recognize text for each selected subtitle line using configurable OCR options")
	CMD_TYPE(COMMAND_VALIDATE)

	bool Validate(const agi::Context *c) override {
		return validator_video_loaded::Validate(c)
		    && c->selectionController->GetSelectedSet().size() > 1
		    && !c->videoController->IsPlaying();
	}

	void operator()(agi::Context *c) override {
		auto *provider = c->project->VideoProvider();
		if (!provider)
			return;

		auto selected_lines = c->selectionController->GetSortedSelection();
		if (selected_lines.size() <= 1)
			return;

		OcrSelectedLinesDialog dialog(c->parent, c, osx::ocr::SupportedRecognitionLanguages());
		if (dialog.ShowModal() != wxID_OK)
			return;

		osx::ocr::Options ocr_options;
		ocr_options.auto_detect_language = dialog.IsAutoLanguage();
		if (!ocr_options.auto_detect_language) {
			auto language_code = dialog.GetLanguageCode();
			if (!language_code.empty())
				ocr_options.recognition_languages.push_back(std::move(language_code));
		}

		int const frame_count = provider->GetFrameCount();
		if (frame_count <= 0)
			return;

		int changed = 0;
		int empty = 0;
		int failed = 0;
		int commit_id = -1;
		size_t line_index = 0;
		bool cancelled = false;
		int const total_lines = static_cast<int>(selected_lines.size());
		wxProgressDialog progress_dialog(
			_("OCR selected Lines"),
			fmt_tl("OCR 0/%d...", total_lines),
			total_lines,
			c->parent,
			wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE);

		for (auto *line : selected_lines) {
			++line_index;
			if (!line)
				continue;

			int const progress_step = static_cast<int>(line_index - 1);
			wxString const progress_label = fmt_tl("Processing line %d/%d...", static_cast<int>(line_index), total_lines);
			if (!progress_dialog.Update(progress_step, progress_label)) {
				cancelled = true;
				break;
			}

			int frame = frame_for_line(c, line, dialog.GetFrameMode());
			frame = std::clamp(frame, 0, frame_count - 1);
			if (c->videoController->GetFrameN() != frame) {
				c->videoController->JumpToFrame(frame);
				if (wxTheApp)
					wxTheApp->ProcessPendingEvents();
			}

			int const frame_time = c->project->Timecodes().TimeAtFrame(frame, agi::vfr::START);
			wxImage image = GetImage(*provider->GetFrame(frame, frame_time, true));
			if (dialog.UseRoi())
				image = crop_image_to_roi(image, dialog.GetRoi());

			auto task = std::async(std::launch::async, [image = std::move(image), ocr_options]() mutable {
				return osx::ocr::RecognizeText(image, ocr_options);
			});
			bool cancel_after_current_line = false;
			while (task.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
				if (!progress_dialog.Update(progress_step, progress_label))
					cancel_after_current_line = true;
				if (wxTheApp)
					wxTheApp->ProcessPendingEvents();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			auto result = task.get();
			if (!result.error.empty()) {
				++failed;
				if (cancel_after_current_line) {
					cancelled = true;
					break;
				}
				continue;
			}

			if (result.text.empty()) {
				++empty;
				if (cancel_after_current_line) {
					cancelled = true;
					break;
				}
				continue;
			}

			line->Text = merge_ocr_text(line->Text.get(), result.text, dialog.GetInsertMode());
			++changed;
			commit_id = c->ass->Commit(_("ocr selected lines"), AssFile::COMMIT_DIAG_TEXT, commit_id, line);
			if (wxTheApp)
				wxTheApp->ProcessPendingEvents();

			c->frame->StatusTimeout(fmt_tl("OCR %d/%d...", static_cast<int>(line_index), static_cast<int>(selected_lines.size())), 1000);
			if (!progress_dialog.Update(static_cast<int>(line_index), progress_label))
				cancel_after_current_line = true;
			if (cancel_after_current_line) {
				cancelled = true;
				break;
			}
		}

		if (cancelled) {
			c->frame->StatusTimeout(fmt_tl("OCR cancelled. Updated %d lines (%d empty, %d failed).", changed, empty, failed), 7000);
		}
		else if (changed == 0 && empty == 0 && failed == 0) {
			c->frame->StatusTimeout(_("No lines were processed."), 4000);
		}
		else if (changed == 0 && empty > 0 && failed == 0) {
			c->frame->StatusTimeout(_("No text detected for selected lines."), 5000);
		}
		else {
			c->frame->StatusTimeout(fmt_tl("OCR updated %d lines (%d empty, %d failed).", changed, empty, failed), 7000);
		}
	}
};
#endif

struct video_frame_next final : public validator_video_loaded {
	CMD_NAME("video/frame/next")
	STR_MENU("Next Frame")
	STR_DISP("Next Frame")
	STR_HELP("Seek to the next frame")

	void operator()(agi::Context *c) override {
		c->videoController->NextFrame();
	}
};

struct video_frame_next_boundary final : public validator_video_loaded {
	CMD_NAME("video/frame/next/boundary")
	STR_MENU("Next Boundary")
	STR_DISP("Next Boundary")
	STR_HELP("Seek to the next beginning or end of a subtitle")

	void operator()(agi::Context *c) override {
		AssDialogue *active_line = c->selectionController->GetActiveLine();
		if (!active_line) return;

		int target = c->videoController->FrameAtTime(active_line->Start, agi::vfr::START);
		if (target > c->videoController->GetFrameN()) {
			c->videoController->JumpToFrame(target);
			return;
		}

		target = c->videoController->FrameAtTime(active_line->End, agi::vfr::END);
		if (target > c->videoController->GetFrameN()) {
			c->videoController->JumpToFrame(target);
			return;
		}

		c->selectionController->NextLine();
		AssDialogue *new_line = c->selectionController->GetActiveLine();
		if (new_line != active_line)
		c->videoController->JumpToTime(new_line->Start);
	}
};

struct video_frame_next_keyframe final : public validator_video_loaded {
	CMD_NAME("video/frame/next/keyframe")
	STR_MENU("Next Keyframe")
	STR_DISP("Next Keyframe")
	STR_HELP("Seek to the next keyframe")

	void operator()(agi::Context *c) override {
		auto const& kf = c->project->Keyframes();
		auto pos = lower_bound(kf.begin(), kf.end(), c->videoController->GetFrameN() + 1);

		c->videoController->JumpToFrame(pos == kf.end() ? c->project->VideoProvider()->GetFrameCount() - 1 : *pos);
	}
};

struct video_frame_next_large final : public validator_video_loaded {
	CMD_NAME("video/frame/next/large")
	STR_MENU("Fast jump forward")
	STR_DISP("Fast jump forward")
	STR_HELP("Fast jump forward")

	void operator()(agi::Context *c) override {
		c->videoController->JumpToFrame(
			c->videoController->GetFrameN() +
			OPT_GET("Video/Slider/Fast Jump Step")->GetInt());
	}
};

struct video_frame_prev final : public validator_video_loaded {
	CMD_NAME("video/frame/prev")
	STR_MENU("Previous Frame")
	STR_DISP("Previous Frame")
	STR_HELP("Seek to the previous frame")

	void operator()(agi::Context *c) override {
		c->videoController->PrevFrame();
	}
};

struct video_frame_prev_boundary final : public validator_video_loaded {
	CMD_NAME("video/frame/prev/boundary")
	STR_MENU("Previous Boundary")
	STR_DISP("Previous Boundary")
	STR_HELP("Seek to the previous beginning or end of a subtitle")

	void operator()(agi::Context *c) override {
		AssDialogue *active_line = c->selectionController->GetActiveLine();
		if (!active_line) return;

		int target = c->videoController->FrameAtTime(active_line->End, agi::vfr::END);
		if (target < c->videoController->GetFrameN()) {
			c->videoController->JumpToFrame(target);
			return;
		}

		target = c->videoController->FrameAtTime(active_line->Start, agi::vfr::START);
		if (target < c->videoController->GetFrameN()) {
			c->videoController->JumpToFrame(target);
			return;
		}

		c->selectionController->PrevLine();
		AssDialogue *new_line = c->selectionController->GetActiveLine();
		if (new_line != active_line)
			c->videoController->JumpToTime(new_line->End, agi::vfr::END);
	}
};

struct video_frame_prev_keyframe final : public validator_video_loaded {
	CMD_NAME("video/frame/prev/keyframe")
	STR_MENU("Previous Keyframe")
	STR_DISP("Previous Keyframe")
	STR_HELP("Seek to the previous keyframe")

	void operator()(agi::Context *c) override {
		auto const& kf = c->project->Keyframes();
		if (kf.empty()) {
			c->videoController->JumpToFrame(0);
			return;
		}

		auto pos = lower_bound(kf.begin(), kf.end(), c->videoController->GetFrameN());

		if (pos != kf.begin())
			--pos;

		c->videoController->JumpToFrame(*pos);
	}
};

struct video_frame_prev_large final : public validator_video_loaded {
	CMD_NAME("video/frame/prev/large")
	STR_MENU("Fast jump backwards")
	STR_DISP("Fast jump backwards")
	STR_HELP("Fast jump backwards")

	void operator()(agi::Context *c) override {
		c->videoController->JumpToFrame(
			c->videoController->GetFrameN() -
			OPT_GET("Video/Slider/Fast Jump Step")->GetInt());
	}
};

static void save_snapshot(agi::Context *c, bool raw, bool subsonly = false) {
	auto option = OPT_GET("Path/Screenshot")->GetString();
	agi::fs::path basepath;

	auto videoname = c->project->VideoName();
	bool is_dummy = videoname.string().starts_with("?dummy");

	// Is it a path specifier and not an actual fixed path?
	if (option[0] == '?') {
		// If dummy video is loaded, we can't save to the video location
		if (option.starts_with("?video") && is_dummy) {
			// So try the script location instead
			option = "?script";
		}
		// Find out where the ?specifier points to
		basepath = c->path->Decode(option);
		// If where ever that is isn't defined, we can't save there
		if ((basepath == "\\") || (basepath == "/")) {
			// So save to the current user's home dir instead
			basepath = wxGetHomeDir().utf8_str().data();
		}
	}
	// Actual fixed (possibly relative) path, decode it
	else
		basepath = c->path->MakeAbsolute(option, "?user/");

	basepath /= is_dummy ? "dummy" : videoname.stem();

	// Get full path
	int session_shot_count = 1;
	std::string path;
	do {
		path = agi::format("%s_%03d_%d.png", basepath.string(), session_shot_count++, c->videoController->GetFrameN());
	} while (agi::fs::FileExists(path));

	get_image(c, raw, subsonly).SaveFile(to_wx(path), wxBITMAP_TYPE_PNG);
}

struct video_frame_save final : public validator_video_loaded {
	CMD_NAME("video/frame/save")
	STR_MENU("Save PNG snapshot")
	STR_DISP("Save PNG snapshot")
	STR_HELP("Save the currently displayed frame to a PNG file in the video's directory")

	void operator()(agi::Context *c) override {
		save_snapshot(c, false);
	}
};

struct video_frame_save_raw final : public validator_video_loaded {
	CMD_NAME("video/frame/save/raw")
	STR_MENU("Save PNG snapshot (no subtitles)")
	STR_DISP("Save PNG snapshot (no subtitles)")
	STR_HELP("Save the currently displayed frame without the subtitles to a PNG file in the video's directory")

	void operator()(agi::Context *c) override {
		save_snapshot(c, true);
	}
};

struct video_frame_save_subs final : public validator_video_loaded {
	CMD_NAME("video/frame/save/subs")
	STR_MENU("Save PNG snapshot (only subtitles)")
	STR_DISP("Save PNG snapshot (only subtitles)")
	STR_HELP("Save the currently displayed subtitles with transparent background to a PNG file in the video's directory")

	void operator()(agi::Context *c) override {
		save_snapshot(c, false, true);
	}
};

struct video_jump final : public validator_video_loaded {
	CMD_NAME("video/jump")
	CMD_ICON(jumpto_button)
	STR_MENU("&Jump to...")
	STR_DISP("Jump to")
	STR_HELP("Jump to frame or time")

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		ShowJumpToDialog(c);
		c->videoSlider->SetFocus();
	}
};

struct video_jump_end final : public validator_video_loaded {
	CMD_NAME("video/jump/end")
	CMD_ICON(video_to_subend)
	STR_MENU("Jump Video to &End")
	STR_DISP("Jump Video to End")
	STR_HELP("Jump the video to the end frame of current subtitle")

	void operator()(agi::Context *c) override {
		if (auto active_line = c->selectionController->GetActiveLine())
			c->videoController->JumpToTime(active_line->End, agi::vfr::END);
	}
};

struct video_jump_start final : public validator_video_loaded {
	CMD_NAME("video/jump/start")
	CMD_ICON(video_to_substart)
	STR_MENU("Jump Video to &Start")
	STR_DISP("Jump Video to Start")
	STR_HELP("Jump the video to the start frame of current subtitle")

	void operator()(agi::Context *c) override {
		if (auto active_line = c->selectionController->GetActiveLine())
			c->videoController->JumpToTime(active_line->Start);
	}
};

struct video_open final : public Command {
	CMD_NAME("video/open")
	CMD_ICON(open_video_menu)
	STR_MENU("&Open Video...")
	STR_DISP("Open Video")
	STR_HELP("Open a video file")

	void operator()(agi::Context *c) override {
		auto str = from_wx(_("Video Formats") + " (*.asf,*.avi,*.avs,*.d2v,*.h264,*.hevc,*.m2ts,*.m4v,*.mkv,*.mov,*.mp4,*.mpeg,*.mpg,*.ogm,*.webm,*.wmv,*.ts,*.y4m,*.yuv)|*.asf;*.avi;*.avs;*.d2v;*.h264;*.hevc;*.m2ts;*.m4v;*.mkv;*.mov;*.mp4;*.mpeg;*.mpg;*.ogm;*.webm;*.wmv;*.ts;*.y4m;*.yuv|"
		         + _("All Files") + " (*.*)|*.*");
		auto filename = OpenFileSelector(_("Open Video File"), "Path/Last/Video", "", "", str, c->parent);
		if (!filename.empty())
			c->project->LoadVideo(filename);
	}
};

struct video_open_dummy final : public Command {
	CMD_NAME("video/open/dummy")
	CMD_ICON(use_dummy_video_menu)
	STR_MENU("&Use Dummy Video...")
	STR_DISP("Use Dummy Video")
	STR_HELP("Open a placeholder video clip with solid color")

	void operator()(agi::Context *c) override {
		std::string fn = CreateDummyVideo(c->parent);
		if (!fn.empty())
			c->project->LoadVideo(fn);
	}
};

struct video_opt_autoscroll final : public Command {
	CMD_NAME("video/opt/autoscroll")
	CMD_ICON(toggle_video_autoscroll)
	STR_MENU("Toggle autoscroll of video")
	STR_DISP("Toggle autoscroll of video")
	STR_HELP("Toggle automatically seeking video to the start time of selected lines")
	CMD_TYPE(COMMAND_TOGGLE)

	bool IsActive(const agi::Context *) override {
		return OPT_GET("Video/Subtitle Sync")->GetBool();
	}

	void operator()(agi::Context *) override {
		OPT_SET("Video/Subtitle Sync")->SetBool(!OPT_GET("Video/Subtitle Sync")->GetBool());
	}
};

struct video_play final : public validator_video_loaded {
	CMD_NAME("video/play")
	CMD_ICON(button_play)
	STR_MENU("Play")
	STR_DISP("Play")
	STR_HELP("Play the video starting on this position")

	void operator()(agi::Context *c) override {
		c->videoController->Play();
	}
};

struct video_play_line final : public validator_video_loaded {
	CMD_NAME("video/play/line")
	CMD_ICON(button_playline)
	STR_MENU("Play line")
	STR_DISP("Play line")
	STR_HELP("Play the video for the current line")

	void operator()(agi::Context *c) override {
		c->videoController->PlayLine();
	}
};

static void change_playback_speed(agi::Context *c, double delta) {
	constexpr double speed_step = 0.25;

	double speed = c->videoController->GetPlaybackSpeed() + delta * speed_step;
	speed = mid(VideoController::MinPlaybackSpeed, speed, VideoController::MaxPlaybackSpeed);

	c->videoController->SetPlaybackSpeed(speed);
	c->frame->StatusTimeout(fmt_tl("Playback speed: %gx", speed), 2000);
}

struct video_playback_speed_decrease final : public validator_video_loaded {
	CMD_NAME("video/playback/speed/decrease")
	STR_MENU("Decrease playback speed")
	STR_DISP("Decrease playback speed")
	STR_HELP("Decrease playback speed by 0.25x")

	void operator()(agi::Context *c) override {
		change_playback_speed(c, -1.0);
	}
};

struct video_playback_speed_increase final : public validator_video_loaded {
	CMD_NAME("video/playback/speed/increase")
	STR_MENU("Increase playback speed")
	STR_DISP("Increase playback speed")
	STR_HELP("Increase playback speed by 0.25x")

	void operator()(agi::Context *c) override {
		change_playback_speed(c, 1.0);
	}
};

struct video_show_overscan final : public validator_video_loaded {
	CMD_NAME("video/show_overscan")
	STR_MENU("Show &Overscan Mask")
	STR_DISP("Show Overscan Mask")
	STR_HELP("Show a mask over the video, indicating areas that might get cropped off by overscan on televisions")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_TOGGLE)

	bool IsActive(const agi::Context *) override {
		return OPT_GET("Video/Overscan Mask")->GetBool();
	}

	void operator()(agi::Context *c) override {
		OPT_SET("Video/Overscan Mask")->SetBool(!OPT_GET("Video/Overscan Mask")->GetBool());
		c->videoDisplay->Render();
	}
};

struct video_reset_pan final : public validator_video_loaded {
       CMD_NAME("video/reset_pan")
       STR_MENU("Reset Video &Pan")
       STR_DISP("Reset Video Pan")
       STR_HELP("Reset the video's position in the video display")

       void operator()(agi::Context *c) override {
		   c->videoDisplay->ResetContentZoom();
       }
};

class video_zoom_100: public validator_video_attached {
public:
	CMD_NAME("video/zoom/100")
	STR_MENU("&100%")
	STR_DISP("100%")
	STR_HELP("Set zoom to 100%")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

	bool IsActive(const agi::Context *c) override {
		return c->videoDisplay->GetWindowZoom() == 1.;
	}

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		c->videoDisplay->SetWindowZoom(1.);
	}
};

class video_stop: public validator_video_loaded {
public:
	CMD_NAME("video/stop")
	CMD_ICON(button_pause)
	STR_MENU("Stop video")
	STR_DISP("Stop video")
	STR_HELP("Stop video playback")

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
	}
};

class video_zoom_200: public validator_video_attached {
public:
	CMD_NAME("video/zoom/200")
	STR_MENU("&200%")
	STR_DISP("200%")
	STR_HELP("Set zoom to 200%")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

	bool IsActive(const agi::Context *c) override {
		return c->videoDisplay->GetWindowZoom() == 2.;
	}

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		c->videoDisplay->SetWindowZoom(2.);
	}
};

class video_zoom_50: public validator_video_attached {
public:
	CMD_NAME("video/zoom/50")
	STR_MENU("&50%")
	STR_DISP("50%")
	STR_HELP("Set zoom to 50%")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_RADIO)

	bool IsActive(const agi::Context *c) override {
		return c->videoDisplay->GetWindowZoom() == .5;
	}

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		c->videoDisplay->SetWindowZoom(.5);
	}
};

struct video_zoom_in final : public validator_video_attached {
	CMD_NAME("video/zoom/in")
	CMD_ICON(zoom_in_button)
	STR_MENU("Zoom In")
	STR_DISP("Zoom In")
	STR_HELP("Zoom video in")

	void operator()(agi::Context *c) override {
		c->videoDisplay->SetWindowZoom(c->videoDisplay->GetWindowZoom() + .125);
	}
};

struct video_zoom_out final : public validator_video_attached {
	CMD_NAME("video/zoom/out")
	CMD_ICON(zoom_out_button)
	STR_MENU("Zoom Out")
	STR_DISP("Zoom Out")
	STR_HELP("Zoom video out")

	void operator()(agi::Context *c) override {
		c->videoDisplay->SetWindowZoom(c->videoDisplay->GetWindowZoom() - .125);
	}
};
}

namespace cmd {
	void init_video() {
		reg(std::make_unique<video_aspect_cinematic>());
		reg(std::make_unique<video_aspect_custom>());
		reg(std::make_unique<video_aspect_default>());
		reg(std::make_unique<video_aspect_full>());
		reg(std::make_unique<video_aspect_wide>());
		reg(std::make_unique<video_close>());
		reg(std::make_unique<video_copy_coordinates>());
		reg(std::make_unique<video_cycle_subtitles_provider>());
		reg(std::make_unique<video_detach>());
		reg(std::make_unique<video_details>());
		reg(std::make_unique<video_focus_seek>());
		reg(std::make_unique<video_frame_copy>());
		reg(std::make_unique<video_frame_copy_raw>());
		reg(std::make_unique<video_frame_copy_subs>());
#ifdef __APPLE__
		reg(std::make_unique<video_frame_ocr>());
		reg(std::make_unique<video_ocr_selected_lines>());
#endif
		reg(std::make_unique<video_frame_next>());
		reg(std::make_unique<video_frame_next_boundary>());
		reg(std::make_unique<video_frame_next_keyframe>());
		reg(std::make_unique<video_frame_next_large>());
		reg(std::make_unique<video_frame_prev>());
		reg(std::make_unique<video_frame_prev_boundary>());
		reg(std::make_unique<video_frame_prev_keyframe>());
		reg(std::make_unique<video_frame_prev_large>());
		reg(std::make_unique<video_frame_save>());
		reg(std::make_unique<video_frame_save_raw>());
		reg(std::make_unique<video_frame_save_subs>());
		reg(std::make_unique<video_jump>());
		reg(std::make_unique<video_jump_end>());
		reg(std::make_unique<video_jump_start>());
		reg(std::make_unique<video_open>());
		reg(std::make_unique<video_open_dummy>());
		reg(std::make_unique<video_opt_autoscroll>());
		reg(std::make_unique<video_play>());
		reg(std::make_unique<video_play_line>());
		reg(std::make_unique<video_playback_speed_decrease>());
		reg(std::make_unique<video_playback_speed_increase>());
		reg(std::make_unique<video_show_overscan>());
		reg(std::make_unique<video_reset_pan>());
		reg(std::make_unique<video_stop>());
		reg(std::make_unique<video_zoom_100>());
		reg(std::make_unique<video_zoom_200>());
		reg(std::make_unique<video_zoom_50>());
		reg(std::make_unique<video_zoom_in>());
		reg(std::make_unique<video_zoom_out>());
	}
}
