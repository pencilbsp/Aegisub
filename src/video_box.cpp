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

#include "video_box.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "audio_controller.h"
#include "compat.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "include/aegisub/toolbar.h"
#include "options.h"
#include "project.h"
#include "selection_controller.h"
#include "video_controller.h"
#include "video_display.h"
#include "video_slider.h"

#include <boost/range/algorithm/binary_search.hpp>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/textctrl.h>
#include <wx/toolbar.h>

namespace {
wxString FormatPlaybackSpeed(double speed, bool with_suffix = false) {
	wxString formatted = wxString::Format("%.3f", speed);
	while (formatted.EndsWith("0") && !formatted.EndsWith(".0"))
		formatted.RemoveLast();
	if (formatted.EndsWith("."))
		formatted += "0";
	if (with_suffix)
		formatted += "x";
	return formatted;
}

wxString TrimPlaybackSpeedSuffix(wxString text) {
	if (text.EndsWith("x") || text.EndsWith("X"))
		text.RemoveLast();
	return text;
}

void SizeComboBoxToFit(wxComboBox *combo, wxString const& initial_value, wxArrayString const& choices) {
	wxString widest = initial_value;
	int widest_width = combo->GetTextExtent(widest).GetWidth();

	for (auto const& choice : choices) {
		int width = combo->GetTextExtent(choice).GetWidth();
		if (width > widest_width) {
			widest = choice;
			widest_width = width;
		}
	}

	combo->SetInitialSize(combo->GetSizeFromText(widest));
}
}

VideoBox::VideoBox(wxWindow *parent, bool isDetached, agi::Context *context)
: wxPanel(parent, -1)
, context(context)
{
	auto videoSlider = new VideoSlider(this, context);
	videoSlider->SetToolTip(_("Seek video"));

	auto mainToolbar = toolbar::GetToolbar(this, "video", context, "Video", false);

	VideoPosition = new wxTextCtrl(this, -1, "", wxDefaultPosition, wxSize(110, -1), wxTE_READONLY);
	VideoPosition->SetToolTip(_("Current frame time and number"));

	VideoSubsPos = new wxTextCtrl(this, -1, "", wxDefaultPosition, wxSize(110, -1), wxTE_READONLY);
	VideoSubsPos->SetToolTip(_("Time of this frame relative to start and end of current subs"));

	wxArrayString choices;
	for (int i = 1; i <= 24; ++i)
		choices.Add(fmt_wx("%g%%", i * 12.5));
	auto zoomBox = new wxComboBox(this, -1, "75%", wxDefaultPosition, wxDefaultSize, choices, wxCB_DROPDOWN | wxTE_PROCESS_ENTER);
	SizeComboBoxToFit(zoomBox, "75%", choices);

	// Playback speed selector
	wxArrayString speedChoices;
	speedChoices.Add("0.25x");
	speedChoices.Add("0.5x");
	speedChoices.Add("0.75x");
	speedChoices.Add("1.0x");
	speedChoices.Add("1.25x");
	speedChoices.Add("1.5x");
	speedChoices.Add("2.0x");
	SpeedBox = new wxComboBox(this, -1, "1.0x", wxDefaultPosition, wxDefaultSize, speedChoices, wxCB_DROPDOWN | wxTE_PROCESS_ENTER);
	SizeComboBoxToFit(SpeedBox, "10.0x", speedChoices);
	SpeedBox->SetToolTip(_("Playback speed"));
	auto applySpeed = [this](bool show_suffix) {
		wxString text = TrimPlaybackSpeedSuffix(SpeedBox->GetValue());
		double speed = 1.0;
		if (text.ToDouble(&speed)) {
			speed = std::max(VideoController::MinPlaybackSpeed, std::min(speed, VideoController::MaxPlaybackSpeed));
			this->context->videoController->SetPlaybackSpeed(speed);
			SpeedBox->SetValue(FormatPlaybackSpeed(speed, show_suffix));
		} else {
			SpeedBox->SetValue(FormatPlaybackSpeed(this->context->videoController->GetPlaybackSpeed(), show_suffix));
		}
	};
	SpeedBox->Bind(wxEVT_COMBOBOX, [applySpeed](wxCommandEvent &) { applySpeed(true); });
	SpeedBox->Bind(wxEVT_TEXT_ENTER, [applySpeed](wxCommandEvent &) { applySpeed(true); });
	SpeedBox->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent &event) {
		SpeedBox->SetValue(TrimPlaybackSpeedSuffix(SpeedBox->GetValue()));
		SpeedBox->SetInsertionPointEnd();
		event.Skip();
	});
	SpeedBox->Bind(wxEVT_KILL_FOCUS, [applySpeed](wxFocusEvent &event) {
		applySpeed(true);
		event.Skip();
	});
	UpdatePlaybackSpeed(context->videoController->GetPlaybackSpeed());

	auto visualToolBar = toolbar::GetToolbar(this, "visual_tools", context, "Video", true);
	auto visualSubToolBar = new wxToolBar(this, -1, wxDefaultPosition, wxDefaultSize, wxTB_VERTICAL | wxTB_BOTTOM | wxTB_NODIVIDER | wxTB_FLAT);

	auto videoDisplay = new VideoDisplay(visualSubToolBar, isDetached, zoomBox, this, context);
	videoDisplay->MoveBeforeInTabOrder(videoSlider);

	auto toolbarSizer = new wxBoxSizer(wxVERTICAL);
	toolbarSizer->Add(visualToolBar, wxSizerFlags(1));
	toolbarSizer->Add(visualSubToolBar, wxSizerFlags());

	auto topSizer = new wxBoxSizer(wxHORIZONTAL);
	topSizer->Add(toolbarSizer, 0, wxEXPAND);
	if (isDetached) {
		topSizer->Add(videoDisplay, 1, wxEXPAND);
	}
	else {
		topSizer->AddStretchSpacer();
		topSizer->Add(videoDisplay, wxSizerFlags().CenterVertical());
		topSizer->AddStretchSpacer();
	}

	auto videoBottomSizer = new wxBoxSizer(wxHORIZONTAL);
	videoBottomSizer->Add(mainToolbar, wxSizerFlags(0).Center());
	videoBottomSizer->Add(VideoPosition, wxSizerFlags(1).Center().Border(wxLEFT));
	videoBottomSizer->Add(VideoSubsPos, wxSizerFlags(1).Center().Border(wxLEFT));
	videoBottomSizer->Add(zoomBox, wxSizerFlags(0).Center().Border(wxLEFT | wxRIGHT));
	videoBottomSizer->Add(SpeedBox, wxSizerFlags(0).Center().Border(wxRIGHT));

	auto VideoSizer = new wxBoxSizer(wxVERTICAL);
	VideoSizer->Add(topSizer, 1, wxEXPAND, 0);
	VideoSizer->Add(new wxStaticLine(this), 0, wxEXPAND, 0);
	VideoSizer->Add(videoSlider, 0, wxEXPAND, 0);
	VideoSizer->Add(videoBottomSizer, 0, wxEXPAND | wxBOTTOM, 5);
	SetSizer(VideoSizer);

	UpdateTimeBoxes();

	connections = agi::signal::make_vector({
		context->ass->AddCommitListener(&VideoBox::UpdateTimeBoxes, this),
		context->project->AddKeyframesListener(&VideoBox::UpdateTimeBoxes, this),
		context->project->AddTimecodesListener(&VideoBox::UpdateTimeBoxes, this),
		context->project->AddVideoProviderListener(&VideoBox::UpdateTimeBoxes, this),
		context->selectionController->AddSelectionListener(&VideoBox::UpdateTimeBoxes, this),
		context->videoController->AddSeekListener(&VideoBox::UpdateTimeBoxes, this),
		context->videoController->AddPlaybackSpeedChangeListener(&VideoBox::UpdatePlaybackSpeed, this),
	});
}

void VideoBox::UpdatePlaybackSpeed(double speed) {
	SpeedBox->SetValue(FormatPlaybackSpeed(speed, !SpeedBox->HasFocus()));
}

void VideoBox::UpdateTimeBoxes() {
	if (!context->project->VideoProvider()) return;

	int frame = context->videoController->GetFrameN();
	int time = context->videoController->TimeAtFrame(frame, agi::vfr::EXACT);

	// Set the text box for frame number and time
	VideoPosition->SetValue(fmt_wx("%s - %d", agi::Time(time).GetAssFormatted(true), frame));
	if (boost::binary_search(context->project->Keyframes(), frame)) {
		// Set the background color to indicate this is a keyframe
		VideoPosition->SetBackgroundColour(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Selection")->GetColor()));
		VideoPosition->SetForegroundColour(to_wx(OPT_GET("Colour/Subtitle Grid/Selection")->GetColor()));
	}
	else {
		VideoPosition->SetBackgroundColour(wxNullColour);
		VideoPosition->SetForegroundColour(wxNullColour);
	}

	AssDialogue *active_line = context->selectionController->GetActiveLine();
	if (!active_line)
		VideoSubsPos->SetValue("");
	else {
		VideoSubsPos->SetValue(fmt_wx(
			"%+dms; %+dms",
			time - active_line->Start,
			time - active_line->End));
	}
}
