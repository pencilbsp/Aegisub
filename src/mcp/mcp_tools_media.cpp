// Copyright (c) 2026, Aegisub Project http://www.aegisub.org/
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#include "mcp_tools_media.h"

#include "mcp_util.h"

#include "ass_dialogue.h"
#include "async_video_provider.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "project.h"
#include "video_frame.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/format.h>
#include <libaegisub/vfr.h>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <vector>

#include <wx/image.h>
#include <wx/mstream.h>

using namespace agi::mcp;

namespace {

constexpr int max_clip_ms = 60 * 1000;

const char *const OPT_OCR_VISUAL_TOOL_USE_REGION = "Tool/OCR/Visual Tool/Use Region";
const char *const OPT_OCR_VISUAL_TOOL_REGION_SET = "Tool/OCR/Visual Tool/Region/Set";
const char *const OPT_OCR_VISUAL_TOOL_REGION_X = "Tool/OCR/Visual Tool/Region/X";
const char *const OPT_OCR_VISUAL_TOOL_REGION_Y = "Tool/OCR/Visual Tool/Region/Y";
const char *const OPT_OCR_VISUAL_TOOL_REGION_WIDTH = "Tool/OCR/Visual Tool/Region/Width";
const char *const OPT_OCR_VISUAL_TOOL_REGION_HEIGHT = "Tool/OCR/Visual Tool/Region/Height";

struct FrameRoi {
	double x = 0.0;
	double y = 0.0;
	double width = 1.0;
	double height = 1.0;
	int pixel_x = 0;
	int pixel_y = 0;
	int pixel_width = 0;
	int pixel_height = 0;

	bool IsValid() const {
		return width > 0.0 && height > 0.0;
	}
};

std::string EncodePng(wxImage const& image) {
	wxMemoryOutputStream stream;
	if (!image.IsOk() || !image.SaveFile(stream, wxBITMAP_TYPE_PNG))
		throw ToolError("Failed to encode video frame as PNG");
	std::string png(stream.GetSize(), '\0');
	stream.CopyTo(png.data(), png.size());
	return mcp::Base64(png.data(), png.size());
}

std::string RenderWav(agi::AudioProvider const& provider, int start_ms, int end_ms) {
	std::ostringstream wav(std::ios::out | std::ios::binary);
	agi::SaveAudioClip(provider, wav, start_ms, end_ms);
	return std::move(wav).str();
}

FrameRoi ClampRoi(FrameRoi roi, int image_width, int image_height) {
	if (!roi.IsValid() || roi.x < 0.0 || roi.y < 0.0 || roi.x >= 1.0 || roi.y >= 1.0)
		throw ToolError("ROI must have normalized x/y in [0, 1) and positive width/height");

	roi.width = std::min(roi.width, 1.0 - roi.x);
	roi.height = std::min(roi.height, 1.0 - roi.y);
	if (!roi.IsValid())
		throw ToolError("ROI is outside the video frame");

	roi.pixel_x = std::clamp(static_cast<int>(std::lround(roi.x * image_width)), 0, image_width - 1);
	roi.pixel_y = std::clamp(static_cast<int>(std::lround(roi.y * image_height)), 0, image_height - 1);
	roi.pixel_width = std::clamp(static_cast<int>(std::lround(roi.width * image_width)), 1, image_width - roi.pixel_x);
	roi.pixel_height = std::clamp(static_cast<int>(std::lround(roi.height * image_height)), 1, image_height - roi.pixel_y);
	return roi;
}

FrameRoi ParseRoi(json::Object const& args) {
	auto it = args.find("roi");
	if (it == args.end())
		throw ToolError("Missing required argument: roi");

	json::Object const& roi = it->second;
	return {
		mcp::ArgDouble(roi, "x"),
		mcp::ArgDouble(roi, "y"),
		mcp::ArgDouble(roi, "width"),
		mcp::ArgDouble(roi, "height")
	};
}

FrameRoi ReadVisualToolRoi() {
	FrameRoi roi{
		OPT_GET(OPT_OCR_VISUAL_TOOL_REGION_X)->GetDouble(),
		OPT_GET(OPT_OCR_VISUAL_TOOL_REGION_Y)->GetDouble(),
		OPT_GET(OPT_OCR_VISUAL_TOOL_REGION_WIDTH)->GetDouble(),
		OPT_GET(OPT_OCR_VISUAL_TOOL_REGION_HEIGHT)->GetDouble()
	};

	if (!OPT_GET(OPT_OCR_VISUAL_TOOL_USE_REGION)->GetBool()
	 || !OPT_GET(OPT_OCR_VISUAL_TOOL_REGION_SET)->GetBool()
	 || !roi.IsValid())
		throw ToolError("No OCR ROI is enabled in this window");

	return roi;
}

json::Object RoiJson(FrameRoi const& roi) {
	json::Object result;
	result.emplace("x", roi.x);
	result.emplace("y", roi.y);
	result.emplace("width", roi.width);
	result.emplace("height", roi.height);
	result.emplace("pixel_x", roi.pixel_x);
	result.emplace("pixel_y", roi.pixel_y);
	result.emplace("pixel_width", roi.pixel_width);
	result.emplace("pixel_height", roi.pixel_height);
	return result;
}

ToolResult GetVideoFrame(json::Object const& args) {
	int window_id = mcp::ArgInt(args, "window_id");
	bool has_row = mcp::HasArg(args, "row");
	bool has_time = mcp::HasArg(args, "time_ms");
	bool has_frame = mcp::HasArg(args, "frame");
	if (static_cast<int>(has_row) + static_cast<int>(has_time) + static_cast<int>(has_frame) != 1)
		throw ToolError("Specify exactly one of row, time_ms, or frame");
	bool has_roi = mcp::HasArg(args, "roi");
	bool use_ocr_roi = mcp::ArgBool(args, "use_ocr_roi", false);
	if (has_roi && use_ocr_roi)
		throw ToolError("Specify either roi or use_ocr_roi, not both");
	bool has_max_width = mcp::HasArg(args, "max_width");
	int max_width = mcp::ArgInt(args, "max_width", 0);
	if (max_width < 0)
		throw ToolError("max_width must be non-negative");

	wxImage image;
	int frame = 0;
	int time_ms = 0;
	int source_width = 0;
	int source_height = 0;
	bool roi_applied = false;
	FrameRoi applied_roi;
	mcp::WithWindow(window_id, [&](FrameMain *, agi::Context *c) {
		if (!has_max_width)
			max_width = OPT_GET("MCP/Frame Max Width")->GetInt();

		auto provider = c->project->VideoProvider();
		if (!provider || provider->GetFrameCount() <= 0)
			throw ToolError("No video is loaded in this window");

		if (has_row) {
			auto index = mcp::EventIndex(c);
			auto row = mcp::ArgInt(args, "row");
			if (row < 0 || row >= static_cast<int64_t>(index.size()))
				throw ToolError(agi::format("Row %d is out of range (file has %d lines)", row, index.size()));
			time_ms = (static_cast<int>(index[row]->Start) + static_cast<int>(index[row]->End)) / 2;
			frame = c->project->Timecodes().FrameAtTime(time_ms);
		}
		else if (has_time) {
			time_ms = mcp::ArgInt(args, "time_ms");
			frame = c->project->Timecodes().FrameAtTime(time_ms);
		}
		else
			frame = mcp::ArgInt(args, "frame");

		frame = std::clamp(frame, 0, provider->GetFrameCount() - 1);
		time_ms = c->project->Timecodes().TimeAtFrame(frame, agi::vfr::START);
		image = GetImage(*provider->GetFrame(frame, time_ms, false));
		source_width = image.GetWidth();
		source_height = image.GetHeight();
		if (has_roi || use_ocr_roi) {
			applied_roi = ClampRoi(has_roi ? ParseRoi(args) : ReadVisualToolRoi(), source_width, source_height);
			image = image.GetSubImage(wxRect(applied_roi.pixel_x, applied_roi.pixel_y, applied_roi.pixel_width, applied_roi.pixel_height));
			roi_applied = true;
		}
	});

	int const crop_width = image.GetWidth();
	int const crop_height = image.GetHeight();
	if (max_width > 0 && image.GetWidth() > max_width) {
		int height = static_cast<int>((static_cast<int64_t>(image.GetHeight()) * max_width) / image.GetWidth());
		image = image.Scale(max_width, std::max(1, height), wxIMAGE_QUALITY_HIGH);
	}

	json::Object metadata;
	metadata.emplace("frame", frame);
	metadata.emplace("time_ms", time_ms);
	metadata.emplace("source_width", source_width);
	metadata.emplace("source_height", source_height);
	metadata.emplace("width", image.GetWidth());
	metadata.emplace("height", image.GetHeight());
	metadata.emplace("max_width", max_width);
	metadata.emplace("cropped", roi_applied);
	metadata.emplace("crop_width", crop_width);
	metadata.emplace("crop_height", crop_height);
	metadata.emplace("scaled", image.GetWidth() != crop_width || image.GetHeight() != crop_height);
	if (roi_applied)
		metadata.emplace("roi", RoiJson(applied_roi));
	ToolResult result;
	result.content.emplace_back(ImageContent(EncodePng(image)));
	result.content.emplace_back(TextContent(mcp::SerializeJson(metadata)));
	return result;
}

ToolResult GetLineAudio(json::Object const& args) {
	int window_id = mcp::ArgInt(args, "window_id");
	int64_t first_row = mcp::ArgInt(args, "row");
	int64_t last_row = mcp::ArgInt(args, "end_row", first_row);
	int64_t pad_before = mcp::ArgInt(args, "pad_before_ms", 0);
	int64_t pad_after = mcp::ArgInt(args, "pad_after_ms", 0);
	if (last_row < first_row)
		throw ToolError("end_row must be greater than or equal to row");
	if (pad_before < 0 || pad_after < 0)
		throw ToolError("Audio padding must be non-negative");

	std::string wav;
	int start_ms = 0;
	int end_ms = 0;
	mcp::WithWindow(window_id, [&](FrameMain *, agi::Context *c) {
		auto provider = c->project->AudioProvider();
		if (!provider)
			throw ToolError("No audio is loaded in this window");
		auto index = mcp::EventIndex(c);
		if (first_row < 0 || last_row >= static_cast<int64_t>(index.size()))
			throw ToolError(agi::format("Row range %d..%d is out of range (file has %d lines)", first_row, last_row, index.size()));

		start_ms = static_cast<int>(index[first_row]->Start);
		end_ms = static_cast<int>(index[first_row]->End);
		for (int64_t row = first_row + 1; row <= last_row; ++row) {
			start_ms = std::min(start_ms, static_cast<int>(index[row]->Start));
			end_ms = std::max(end_ms, static_cast<int>(index[row]->End));
		}
		int duration_ms = static_cast<int>(provider->GetNumSamples() * 1000 / provider->GetSampleRate());
		start_ms = std::max<int64_t>(0, static_cast<int64_t>(start_ms) - pad_before);
		end_ms = std::min<int64_t>(duration_ms, static_cast<int64_t>(end_ms) + pad_after);
		if (end_ms <= start_ms)
			throw ToolError("The requested line range contains no audio");
		if (end_ms - start_ms > max_clip_ms)
			throw ToolError("Audio clips are limited to 60 seconds");
		wav = RenderWav(*provider, start_ms, end_ms);
	});
	auto audio = mcp::Base64(wav.data(), wav.size());

	json::Array rows;
	for (int64_t row = first_row; row <= last_row; ++row)
		rows.emplace_back(row);
	json::Object metadata;
	metadata.emplace("start_ms", start_ms);
	metadata.emplace("end_ms", end_ms);
	metadata.emplace("rows", std::move(rows));
	ToolResult result;
	result.content.emplace_back(AudioContent(std::move(audio)));
	result.content.emplace_back(TextContent(mcp::SerializeJson(metadata)));
	return result;
}

ToolResult GetAudioClip(json::Object const& args) {
	int window_id = mcp::ArgInt(args, "window_id");
	int64_t requested_start = mcp::ArgInt(args, "start_ms");
	int64_t requested_end = mcp::ArgInt(args, "end_ms");
	if (requested_start < 0 || requested_end <= requested_start)
		throw ToolError("start_ms must be non-negative and end_ms must be greater than start_ms");
	if (requested_end - requested_start > max_clip_ms)
		throw ToolError("Audio clips are limited to 60 seconds");

	std::string wav;
	int start_ms = 0;
	int end_ms = 0;
	mcp::WithWindow(window_id, [&](FrameMain *, agi::Context *c) {
		auto provider = c->project->AudioProvider();
		if (!provider)
			throw ToolError("No audio is loaded in this window");
		int64_t duration_ms = provider->GetNumSamples() * 1000 / provider->GetSampleRate();
		start_ms = std::min(requested_start, duration_ms);
		end_ms = std::min(requested_end, duration_ms);
		if (end_ms <= start_ms)
			throw ToolError("The requested range is outside the loaded audio");
		wav = RenderWav(*provider, start_ms, end_ms);
	});
	auto audio = mcp::Base64(wav.data(), wav.size());

	json::Object metadata;
	metadata.emplace("start_ms", start_ms);
	metadata.emplace("end_ms", end_ms);
	ToolResult result;
	result.content.emplace_back(AudioContent(std::move(audio)));
	result.content.emplace_back(TextContent(mcp::SerializeJson(metadata)));
	return result;
}

}

namespace mcp {

void RegisterMediaTools(agi::mcp::Dispatcher& d) {
	d.RegisterTool({
		"get_video_frame",
		"Get a rendered PNG video frame, including visible subtitles, from one Aegisub window. Select it by subtitle row midpoint, time in milliseconds, or frame number (exactly one). Optionally crop to a normalized ROI before downscaling, or use the persisted OCR visual-tool ROI. Frames are downscaled to the Aegisub MCP preference by default to reduce image tokens. Returns image content plus JSON metadata.",
		R"json({"type":"object","properties":{"window_id":{"type":"integer","description":"Window id from list_windows"},"row":{"type":"integer","description":"Subtitle row whose midpoint selects the frame"},"time_ms":{"type":"integer","description":"Video time in milliseconds"},"frame":{"type":"integer","description":"Zero-based video frame"},"max_width":{"type":"integer","description":"Override the Aegisub frame max-width preference for this request; 0 returns the original size"},"roi":{"type":"object","description":"Optional normalized crop rectangle in source video coordinates, applied before max_width scaling","properties":{"x":{"type":"number","description":"Left edge, normalized 0..1"},"y":{"type":"number","description":"Top edge, normalized 0..1"},"width":{"type":"number","description":"Width, normalized 0..1"},"height":{"type":"number","description":"Height, normalized 0..1"}},"required":["x","y","width","height"]},"use_ocr_roi":{"type":"boolean","description":"When true, crop to the persisted OCR visual-tool ROI for this window. Mutually exclusive with roi."}},"required":["window_id"]})json",
		GetVideoFrame,
		false
	});
	d.RegisterTool({
		"get_line_audio",
		"Get a WAV audio clip aligned to one subtitle row or an inclusive row range. Optional padding expands the clip; the actual range is returned as JSON metadata. Clips are limited to 60 seconds.",
		R"json({"type":"object","properties":{"window_id":{"type":"integer","description":"Window id from list_windows"},"row":{"type":"integer","description":"First subtitle row (0-based)"},"end_row":{"type":"integer","description":"Last subtitle row, inclusive (default row)"},"pad_before_ms":{"type":"integer","description":"Non-negative leading padding (default 0)"},"pad_after_ms":{"type":"integer","description":"Non-negative trailing padding (default 0)"}},"required":["window_id","row"]})json",
		GetLineAudio,
		false
	});
	d.RegisterTool({
		"get_audio_clip",
		"Get a WAV audio clip for an explicit millisecond range from one Aegisub window. Use get_line_audio for the normal subtitle translation workflow. Clips are limited to 60 seconds.",
		R"json({"type":"object","properties":{"window_id":{"type":"integer","description":"Window id from list_windows"},"start_ms":{"type":"integer","description":"Clip start in milliseconds"},"end_ms":{"type":"integer","description":"Clip end in milliseconds"}},"required":["window_id","start_ms","end_ms"]})json",
		GetAudioClip,
		false
	});
}

}
