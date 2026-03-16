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

#include <string>
#include <vector>

class wxImage;

namespace osx::ocr {
struct Options {
	bool auto_detect_language = true;
	std::vector<std::string> recognition_languages;
};

struct Region {
	std::string text;
	double x = 0.0;      // normalized to [0, 1], relative to frame width
	double y = 0.0;      // normalized to [0, 1], relative to frame height (top origin)
	double width = 0.0;  // normalized width
	double height = 0.0; // normalized height
};

struct Character {
	std::string text;
	size_t region_index = 0;
	double x = 0.0;      // normalized to [0, 1], relative to frame width
	double y = 0.0;      // normalized to [0, 1], relative to frame height (top origin)
	double width = 0.0;  // normalized width
	double height = 0.0; // normalized height
};

struct Result {
	std::string text;
	std::vector<Region> regions;
	std::vector<Character> characters;
	std::string error;
};

Result RecognizeText(wxImage const& image, Options const& options = {});
std::vector<std::string> SupportedRecognitionLanguages();
}
