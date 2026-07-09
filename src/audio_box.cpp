// Copyright (c) 2005, Rodrigo Braz Monteiro, Niels Martin Hansen
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

#include "audio_box.h"

#include "include/aegisub/context.h"
#include "include/aegisub/toolbar.h"

#include "audio_controller.h"
#include "audio_display.h"
#include "audio_karaoke.h"
#include "audio_timing.h"
#include "options.h"
#include "project.h"
#include "toggle_bitmap.h"
#include "utils.h"

#include <cmath>
#include <wx/cursor.h>
#include <wx/panel.h>
#include <wx/slider.h>
#include <wx/scrolbar.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/string.h>
#include <wx/toolbar.h>

enum {
	Audio_Horizontal_Zoom = 1600,
	Audio_Vertical_Zoom,
	Audio_Volume
};

AudioBox::AudioBox(wxWindow *parent, agi::Context *context)
: wxSashWindow(parent, -1, wxDefaultPosition, wxDefaultSize, wxSW_3D | wxCLIP_CHILDREN)
, controller(context->audioController.get())
, context(context)
, audio_open_connection(context->audioController->AddAudioPlayerOpenListener(&AudioBox::OnAudioOpen, this))
, panel(new wxPanel(this, -1, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_RAISED))
, audioDisplay(new AudioDisplay(panel, context->audioController.get(), context))
, HorizontalZoom(new wxSlider(panel, Audio_Horizontal_Zoom, -OPT_GET("Audio/Zoom/Horizontal")->GetInt(), -50, 30, wxDefaultPosition, wxDefaultSize, wxSL_VERTICAL|wxSL_BOTH))
, VerticalZoom(new wxSlider(panel, Audio_Vertical_Zoom, OPT_GET("Audio/Zoom/Vertical")->GetInt(), 0, 100, wxDefaultPosition, wxDefaultSize, wxSL_VERTICAL|wxSL_BOTH|wxSL_INVERSE))
, VolumeBar(new wxSlider(panel, Audio_Volume, OPT_GET("Audio/Volume")->GetInt(), 0, 100, wxDefaultPosition, wxDefaultSize, wxSL_VERTICAL|wxSL_BOTH|wxSL_INVERSE))
{
	SetSashVisible(wxSASH_BOTTOM, true);
	Bind(wxEVT_SASH_DRAGGED, &AudioBox::OnSashDrag, this);
	Bind(wxEVT_LEFT_DOWN, &AudioBox::OnResizeMouseDown, this);
	Bind(wxEVT_MOTION, &AudioBox::OnResizeMouseMove, this);
	Bind(wxEVT_LEFT_UP, &AudioBox::OnResizeMouseUp, this);
	panel->Bind(wxEVT_LEFT_DOWN, &AudioBox::OnResizeMouseDown, this);
	panel->Bind(wxEVT_MOTION, &AudioBox::OnResizeMouseMove, this);
	panel->Bind(wxEVT_LEFT_UP, &AudioBox::OnResizeMouseUp, this);

	HorizontalZoom->SetToolTip(_("Horizontal zoom"));
	VerticalZoom->SetToolTip(_("Vertical zoom"));
	VolumeBar->SetToolTip(_("Audio Volume"));

	int waveform_min_height = FromDIP(36);
	audioDisplay->SetMinSize(wxSize(-1, waveform_min_height));
	HorizontalZoom->SetMinSize(wxSize(-1, waveform_min_height));
	VerticalZoom->SetMinSize(wxSize(-1, waveform_min_height));
	VolumeBar->SetMinSize(wxSize(-1, waveform_min_height));

	bool link = OPT_GET("Audio/Link")->GetBool();
	if (link) {
		VolumeBar->SetValue(VerticalZoom->GetValue());
		VolumeBar->Enable(false);
	}

	// VertVol sider
	wxSizer *VertVol = new wxBoxSizer(wxHORIZONTAL);
	VertVol->Add(VerticalZoom,1,wxEXPAND,0);
	VertVol->Add(VolumeBar,1,wxEXPAND,0);
	wxSizer *VertVolArea = new wxBoxSizer(wxVERTICAL);
	VertVolArea->Add(VertVol,1,wxEXPAND,0);

	auto link_btn = new ToggleBitmap(panel, context, "audio/opt/vertical_link", 16, "Audio", FromDIP(wxSize(20, -1)));
	link_btn->SetMaxSize(wxDefaultSize);
	VertVolArea->Add(link_btn, 0, wxRIGHT | wxEXPAND, 0);
	BindConnection(OPT_SUB("Audio/Link", &AudioBox::OnVerticalLink, this));

	// Top sizer
	wxSizer *TopSizer = new wxBoxSizer(wxHORIZONTAL);
	TopSizer->Add(audioDisplay,1,wxEXPAND,0);
	TopSizer->Add(HorizontalZoom,0,wxEXPAND,0);
	TopSizer->Add(VertVolArea,0,wxEXPAND,0);

	context->karaoke = new AudioKaraoke(panel, context);

	// Main sizer
	auto MainSizer = new wxBoxSizer(wxVERTICAL);
	MainSizer->Add(TopSizer,1,wxEXPAND|wxALL,3);
	MainSizer->Add(toolbar::GetToolbar(panel, "audio", context, "Audio"),0,wxEXPAND|wxLEFT|wxRIGHT,3);
	MainSizer->Add(context->karaoke,0,wxEXPAND|wxALL,3);
	MainSizer->Show(context->karaoke, false);
	panel->SetSizer(MainSizer);

	wxSizer *audioSashSizer = new wxBoxSizer(wxHORIZONTAL);
	audioSashSizer->Add(panel, 1, wxEXPAND);
	SetSizerAndFit(audioSashSizer);
	int minimum_height = panel->GetBestSize().GetHeight();
	int display_height = static_cast<int>(OPT_GET("Audio/Display Height")->GetInt());
	int default_height = 190;
	SetMinimumSizeY(minimum_height);
	SetMinSize(wxSize(-1, std::max({minimum_height, display_height, default_height})));

	audioDisplay->EnableTouchEvents(wxTOUCH_ZOOM_GESTURE);
	audioDisplay->Bind(wxEVT_MOUSEWHEEL, &AudioBox::OnMouseWheel, this);
	audioDisplay->Bind(wxEVT_GESTURE_ZOOM, &AudioBox::OnGestureZoom, this);

	audioDisplay->SetZoomLevel(-HorizontalZoom->GetValue());
	audioDisplay->SetAmplitudeScale(pow(mid(1, VerticalZoom->GetValue(), 100) / 50.0, 3));
}

BEGIN_EVENT_TABLE(AudioBox,wxSashWindow)
	EVT_COMMAND_SCROLL(Audio_Horizontal_Zoom, AudioBox::OnHorizontalZoom)
	EVT_COMMAND_SCROLL(Audio_Vertical_Zoom, AudioBox::OnVerticalZoom)
	EVT_COMMAND_SCROLL(Audio_Volume, AudioBox::OnVolume)
END_EVENT_TABLE()

void AudioBox::OnMouseWheel(wxMouseEvent &evt) {
	if (!ForwardMouseWheelEvent(audioDisplay, evt))
		return;
	bool zoom = evt.CmdDown() != OPT_GET("Audio/Wheel Default to Zoom")->GetBool();
	if (!zoom) {
		int amount = -evt.GetWheelRotation();
		// If the user did a horizontal scroll the amount should be inverted
		// for it to be natural.
		if (evt.GetWheelAxis() == 1) amount = -amount;

		// Reset any accumulated zoom
		mouse_zoom_accum = 0;

		audioDisplay->ScrollBy(amount);
	}
	else if (evt.GetWheelAxis() == 0) {
		mouse_zoom_accum += evt.GetWheelRotation();
		int zoom_delta = mouse_zoom_accum / evt.GetWheelDelta();
		mouse_zoom_accum %= evt.GetWheelDelta();
		SetHorizontalZoom(audioDisplay->GetZoomLevel() + zoom_delta);
	}
}

void AudioBox::OnGestureZoom(wxZoomGestureEvent &event) {
	if (!zoom_gesture_active && !event.IsGestureStart())
		return;

	if (event.IsGestureStart()) {
		zoom_gesture_active = true;
		zoom_gesture_start_level = audioDisplay->GetZoomLevel();
		zoom_gesture_anchor_x = event.GetPosition().x;
		zoom_gesture_anchor_time = audioDisplay->TimeFromRelativeX(zoom_gesture_anchor_x);
	}
	else if (event.IsGestureEnd()) {
		zoom_gesture_active = false;
	}

	double target_zoom_factor = audioDisplay->GetZoomLevelFactor(zoom_gesture_start_level) * event.GetZoomFactor();
	SetHorizontalZoomAt(GetClosestZoomLevel(target_zoom_factor), zoom_gesture_anchor_x, zoom_gesture_anchor_time);
}

void AudioBox::ApplyAudioHeight(int new_height) {
	new_height = mid(GetMinimumSizeY(), new_height, GetMaximumAudioHeight());
	SetMinSize(wxSize(-1, new_height));
	GetParent()->Layout();

	// Karaoke mode is always disabled when the audio box is first opened, so
	// the initial height shouldn't include it
	if (context->karaoke->IsEnabled())
		new_height -= context->karaoke->GetSize().GetHeight() + 6;

	OPT_SET("Audio/Display Height")->SetInt(new_height);
}

int AudioBox::GetMaximumAudioHeight() const {
	int parent_height = GetParent()->GetClientSize().GetHeight();
	if (parent_height <= 0)
		parent_height = GetParent()->GetSize().GetHeight();
	if (parent_height <= GetMinimumSizeY())
		return std::max(FromDIP(420), GetMinimumSizeY());

	int max_height = std::min(parent_height * 45 / 100, FromDIP(420));
	return std::max(max_height, GetMinimumSizeY());
}

bool AudioBox::IsOnResizeEdge(wxMouseEvent &event) const {
	auto source = dynamic_cast<wxWindow *>(event.GetEventObject());
	if (!source)
		return false;

	int y = source->ClientToScreen(event.GetPosition()).y;
	wxRect rect = GetScreenRect();
	return y >= rect.GetBottom() - FromDIP(8) && y <= rect.GetBottom() + FromDIP(2);
}

void AudioBox::OnResizeMouseDown(wxMouseEvent &event) {
	if (!IsOnResizeEdge(event)) {
		event.Skip();
		return;
	}

	auto source = static_cast<wxWindow *>(event.GetEventObject());
	resize_drag_active = true;
	resize_drag_window = source;
	resize_drag_start_y = source->ClientToScreen(event.GetPosition()).y;
	resize_drag_start_height = GetSize().GetHeight();
	source->CaptureMouse();
}

void AudioBox::OnResizeMouseMove(wxMouseEvent &event) {
	if (!resize_drag_active) {
		if (IsOnResizeEdge(event))
			static_cast<wxWindow *>(event.GetEventObject())->SetCursor(wxCursor(wxCURSOR_SIZENS));
		else
			event.Skip();
		return;
	}

	if (!event.LeftIsDown()) {
		OnResizeMouseUp(event);
		return;
	}

	auto source = static_cast<wxWindow *>(event.GetEventObject());
	int current_y = source->ClientToScreen(event.GetPosition()).y;
	ApplyAudioHeight(resize_drag_start_height + current_y - resize_drag_start_y);
}

void AudioBox::OnResizeMouseUp(wxMouseEvent &event) {
	if (!resize_drag_active) {
		event.Skip();
		return;
	}

	resize_drag_active = false;
	if (resize_drag_window && resize_drag_window->HasCapture())
		resize_drag_window->ReleaseMouse();
	resize_drag_window = nullptr;
}

void AudioBox::OnSashDrag(wxSashEvent &event) {
	if (event.GetDragStatus() == wxSASH_STATUS_OUT_OF_RANGE)
		return;

	ApplyAudioHeight(event.GetDragRect().GetHeight());
}

void AudioBox::OnHorizontalZoom(wxScrollEvent &event) {
	// Negate the value since we want zoom out to be on bottom and zoom in on top,
	// but the control doesn't want negative on bottom and positive on top.
	SetHorizontalZoom(-event.GetPosition());
}

void AudioBox::SetHorizontalZoom(int new_zoom) {
	audioDisplay->SetZoomLevel(new_zoom);
	HorizontalZoom->SetValue(-new_zoom);
	OPT_SET("Audio/Zoom/Horizontal")->SetInt(new_zoom);
}

void AudioBox::SetHorizontalZoomAt(int new_zoom, int anchor_x, int anchor_time) {
	SetHorizontalZoom(new_zoom);
	audioDisplay->ScrollPixelToLeft(audioDisplay->AbsoluteXFromTime(anchor_time) - anchor_x);
}

int AudioBox::GetClosestZoomLevel(double zoom_factor) const {
	int min_zoom = -HorizontalZoom->GetMax();
	int max_zoom = -HorizontalZoom->GetMin();
	int closest = min_zoom;
	double closest_distance = std::abs(zoom_factor - AudioDisplay::GetZoomLevelFactor(closest));

	for (int zoom = min_zoom + 1; zoom <= max_zoom; ++zoom) {
		double distance = std::abs(zoom_factor - AudioDisplay::GetZoomLevelFactor(zoom));
		if (distance < closest_distance) {
			closest = zoom;
			closest_distance = distance;
		}
	}

	return closest;
}

void AudioBox::OnVerticalZoom(wxScrollEvent &event) {
	int pos = mid(1, event.GetPosition(), 100);
	OPT_SET("Audio/Zoom/Vertical")->SetInt(pos);
	double value = pow(pos / 50.0, 3);
	audioDisplay->SetAmplitudeScale(value);
	if (!VolumeBar->IsEnabled()) {
		VolumeBar->SetValue(pos);
		controller->SetVolume(value);
	}
}

void AudioBox::OnVolume(wxScrollEvent &event) {
	int pos = mid(1, event.GetPosition(), 100);
	OPT_SET("Audio/Volume")->SetInt(pos);
	controller->SetVolume(pow(pos / 50.0, 3));
}

void AudioBox::OnVerticalLink(agi::OptionValue const& opt) {
	if (opt.GetBool()) {
		int pos = mid(1, VerticalZoom->GetValue(), 100);
		double value = pow(pos / 50.0, 3);
		controller->SetVolume(value);
		VolumeBar->SetValue(pos);
	}
	VolumeBar->Enable(!opt.GetBool());
}

void AudioBox::OnAudioOpen() {
	controller->SetVolume(pow(mid(1, VolumeBar->GetValue(), 100) / 50.0, 3));
}

void AudioBox::ShowKaraokeBar(bool show) {
	wxSizer *panel_sizer = panel->GetSizer();
	if (panel_sizer->IsShown(context->karaoke) == show) return;

	int new_height = GetSize().GetHeight();
	int kara_height = context->karaoke->GetSize().GetHeight() + 6;

	if (show)
		new_height += kara_height;
	else
		new_height -= kara_height;

	panel_sizer->Show(context->karaoke, show);
	SetMinSize(wxSize(-1, new_height));
	GetParent()->Layout();
}

void AudioBox::ScrollAudioBy(int pixel_amount) {
	audioDisplay->ScrollBy(pixel_amount);
}

void AudioBox::ScrollToActiveLine(ScrollMode mode) {
	if (controller->GetTimingController()) {
		switch (mode) {
			case ScrollMode::Range:
				audioDisplay->ScrollTimeRangeInView(controller->GetTimingController()->GetIdealVisibleTimeRange());
				break;
			case ScrollMode::Start:
				audioDisplay->ScrollTimeToCenter(controller->GetTimingController()->GetActiveLineRange().begin());
				break;
			case ScrollMode::End:
				audioDisplay->ScrollTimeToCenter(controller->GetTimingController()->GetActiveLineRange().end());
				break;
		}
	}
}
