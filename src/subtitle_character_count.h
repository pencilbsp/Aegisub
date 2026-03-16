// Copyright (c) 2026, Aegisub contributors
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#pragma once

#include <libaegisub/character_count.h>

#include <string_view>

namespace subtitle_character_count {

inline int IgnoreMask(bool ignore_whitespace, bool ignore_punctuation) {
	int ignore = agi::IGNORE_BLOCKS;
	if (ignore_whitespace)
		ignore |= agi::IGNORE_WHITESPACE;
	if (ignore_punctuation)
		ignore |= agi::IGNORE_PUNCTUATION;
	return ignore;
}

inline size_t LineLength(std::string_view text, bool ignore_whitespace, bool ignore_punctuation) {
	return agi::MaxLineLength(text, IgnoreMask(ignore_whitespace, ignore_punctuation));
}

inline size_t TotalCharacters(std::string_view text, bool ignore_whitespace, bool ignore_punctuation) {
	return agi::CharacterCount(text, IgnoreMask(ignore_whitespace, ignore_punctuation));
}

}
