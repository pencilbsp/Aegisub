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

#include "video_ocr.h"

#include <algorithm>
#include <string>
#include <vector>

#include <wx/image.h>

#import <CoreGraphics/CoreGraphics.h>
#import <Vision/Vision.h>

namespace {
struct TextObservation {
	struct CharacterObservation {
		double x;
		double y_top;
		double width;
		double height;
		std::string text;
	};

	double y_top;
	double x;
	double width;
	double height;
	std::string text;
	std::vector<CharacterObservation> characters;
};

NSUInteger highest_supported_text_revision() {
	NSIndexSet *supported_revisions = VNRecognizeTextRequest.supportedRevisions;
	NSUInteger highest_revision = supported_revisions.lastIndex;
	if (highest_revision == NSNotFound)
		highest_revision = VNRecognizeTextRequest.currentRevision;
	return highest_revision;
}

CGImageRef wx_image_to_cg_image(wxImage const& image) {
	if (!image.IsOk() || !image.GetData())
		return nullptr;

	size_t const width = image.GetWidth();
	size_t const height = image.GetHeight();
	if (!width || !height)
		return nullptr;

	size_t const pixel_count = width * height;
	std::vector<unsigned char> rgba(pixel_count * 4);
	unsigned char const* src = image.GetData();
	for (size_t i = 0; i < pixel_count; ++i) {
		rgba[i * 4 + 0] = src[i * 3 + 0];
		rgba[i * 4 + 1] = src[i * 3 + 1];
		rgba[i * 4 + 2] = src[i * 3 + 2];
		rgba[i * 4 + 3] = 255;
	}

	CFDataRef data = CFDataCreate(kCFAllocatorDefault, rgba.data(), rgba.size());
	if (!data)
		return nullptr;

	CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
	if (!provider) {
		CFRelease(data);
		return nullptr;
	}

	CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
	if (!color_space) {
		CGDataProviderRelease(provider);
		CFRelease(data);
		return nullptr;
	}

	CGImageRef cg_image = CGImageCreate(
		width,
		height,
		8,
		32,
		width * 4,
		color_space,
		static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big,
		provider,
		nullptr,
		false,
		kCGRenderingIntentDefault);

	CGColorSpaceRelease(color_space);
	CGDataProviderRelease(provider);
	CFRelease(data);
	return cg_image;
}
}

namespace osx::ocr {
std::vector<std::string> SupportedRecognitionLanguages() {
	std::vector<std::string> languages;

	@autoreleasepool {
		if (@available(macOS 10.15, *)) {
			NSError *error = nil;
			NSArray<NSString *> *supported = nil;
			if (@available(macOS 12.0, *)) {
				VNRecognizeTextRequest *request = [[[VNRecognizeTextRequest alloc] init] autorelease];
				request.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
				request.revision = highest_supported_text_revision();
				supported = [request supportedRecognitionLanguagesAndReturnError:&error];
			}
			else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
				supported = [VNRecognizeTextRequest supportedRecognitionLanguagesForTextRecognitionLevel:VNRequestTextRecognitionLevelAccurate
				                                                                                     revision:highest_supported_text_revision()
				                                                                                        error:&error];
#pragma clang diagnostic pop
			}
			if (!error && supported) {
				languages.reserve(supported.count);
				for (NSString *language in supported) {
					if (!language.length)
						continue;
					languages.emplace_back(language.UTF8String ? language.UTF8String : "");
				}
			}
		}
	}

	std::sort(languages.begin(), languages.end());
	languages.erase(std::unique(languages.begin(), languages.end()), languages.end());
	return languages;
}

Result RecognizeText(wxImage const& image, Options const& options) {
	Result result;

	@autoreleasepool {
		if (!image.IsOk() || !image.GetData()) {
			result.error = "Invalid frame image.";
			return result;
		}

		if (@available(macOS 10.15, *)) {
			CGImageRef cg_image = wx_image_to_cg_image(image);
			if (!cg_image) {
				result.error = "Failed to prepare frame image for OCR.";
				return result;
			}

			VNRecognizeTextRequest *request = [[[VNRecognizeTextRequest alloc] init] autorelease];
			request.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
			request.usesLanguageCorrection = YES;
			request.minimumTextHeight = 0.01;
			request.revision = highest_supported_text_revision();

			if (!options.recognition_languages.empty()) {
				NSMutableArray<NSString *> *recognition_languages = [NSMutableArray arrayWithCapacity:options.recognition_languages.size()];
				for (auto const& language : options.recognition_languages) {
					if (language.empty())
						continue;
					NSString *language_ns = [NSString stringWithUTF8String:language.c_str()];
					if (language_ns.length)
						[recognition_languages addObject:language_ns];
				}
				if (recognition_languages.count)
					request.recognitionLanguages = recognition_languages;
			}

			if (@available(macOS 13.0, *)) {
				if (request.revision >= VNRecognizeTextRequestRevision3)
					request.automaticallyDetectsLanguage = options.auto_detect_language;
			}

			NSError *error = nil;
			VNImageRequestHandler *handler = [[[VNImageRequestHandler alloc] initWithCGImage:cg_image options:@{}] autorelease];
			[handler performRequests:@[request] error:&error];
			CGImageRelease(cg_image);

			if (error) {
				result.error = error.localizedDescription.UTF8String ? error.localizedDescription.UTF8String : "Vision OCR failed.";
				return result;
			}

			std::vector<TextObservation> lines;
			for (VNRecognizedTextObservation *observation in request.results) {
				NSArray<VNRecognizedText *> *candidates = [observation topCandidates:1];
				if (!candidates.count)
					continue;

				VNRecognizedText *best = candidates.firstObject;
				if (!best.string.length)
					continue;

				VNRectangleObservation *bbox = observation;
				double const x = bbox.boundingBox.origin.x;
				double const y_top = 1.0 - (bbox.boundingBox.origin.y + bbox.boundingBox.size.height);
				double const width = bbox.boundingBox.size.width;
				double const height = bbox.boundingBox.size.height;
				__block TextObservation line = {
					y_top,
					x,
					width,
					height,
					best.string.UTF8String ? best.string.UTF8String : "",
					{}
				};

				NSString *recognized = best.string;
				[recognized enumerateSubstringsInRange:NSMakeRange(0, recognized.length)
				                               options:NSStringEnumerationByComposedCharacterSequences
				                            usingBlock:^(NSString * _Nullable substring,
				                                         NSRange substringRange,
				                                         NSRange,
				                                         BOOL *) {
					if (!substring.length)
						return;

					NSError *range_error = nil;
					VNRectangleObservation *char_box = [best boundingBoxForRange:substringRange error:&range_error];
					if (range_error || !char_box)
						return;

					double const char_x = char_box.boundingBox.origin.x;
					double const char_y_top = 1.0 - (char_box.boundingBox.origin.y + char_box.boundingBox.size.height);
					double const char_width = char_box.boundingBox.size.width;
					double const char_height = char_box.boundingBox.size.height;
					line.characters.push_back({
						char_x,
						char_y_top,
						char_width,
						char_height,
						substring.UTF8String ? substring.UTF8String : ""
					});
				}];

				lines.push_back(std::move(line));
			}

			std::sort(lines.begin(), lines.end(), [](TextObservation const& a, TextObservation const& b) {
				if (a.y_top != b.y_top)
					return a.y_top < b.y_top;
				return a.x < b.x;
			});

			for (size_t i = 0; i < lines.size(); ++i) {
				if (i)
					result.text += '\n';
				result.text += lines[i].text;
				result.regions.push_back({lines[i].text, lines[i].x, lines[i].y_top, lines[i].width, lines[i].height});
				for (auto const& character : lines[i].characters) {
					result.characters.push_back({
						character.text,
						i,
						character.x,
						character.y_top,
						character.width,
						character.height
					});
				}
			}
		}
		else {
			result.error = "OCR requires macOS 10.15 or newer.";
		}
	}

	return result;
}
}
