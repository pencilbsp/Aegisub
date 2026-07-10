// Copyright (c) 2026, Aegisub Project http://www.aegisub.org/
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

#include <libaegisub/glossary.h>

#include <main.h>

#include <filesystem>

class lagi_glossary : public libagi {
protected:
	std::string db_path = "data/glossary_test.db";

	void SetUp() override {
		std::error_code ec;
		std::filesystem::remove(db_path, ec);
	}

	void TearDown() override {
		std::error_code ec;
		std::filesystem::remove(db_path, ec);
	}

	// Add a term to a dictionary and return its entry id.
	int64_t add(agi::Glossary& g, int64_t dict, std::string term) {
		agi::GlossaryEntry e;
		e.term = std::move(term);
		return g.UpsertEntry(dict, e);
	}
};

TEST_F(lagi_glossary, create_open) {
	ASSERT_NO_THROW(agi::Glossary{db_path});
	// Reopening an existing database must succeed too.
	ASSERT_NO_THROW(agi::Glossary{db_path});
}

TEST_F(lagi_glossary, dictionary_crud) {
	agi::Glossary g(db_path);
	auto id = g.CreateDictionary("Names");
	EXPECT_EQ(id, g.GetDictionaryId("Names"));
	EXPECT_EQ(-1, g.GetDictionaryId("Missing"));

	g.RenameDictionary(id, "People");
	EXPECT_EQ(-1, g.GetDictionaryId("Names"));
	EXPECT_EQ(id, g.GetDictionaryId("People"));

	ASSERT_EQ(1u, g.ListDictionaries().size());
	g.DeleteDictionary(id);
	EXPECT_EQ(0u, g.ListDictionaries().size());
}

TEST_F(lagi_glossary, entry_roundtrip) {
	agi::Glossary g(db_path);
	auto dict = g.CreateDictionary("d");

	agi::GlossaryEntry e;
	e.term = "hello";
	e.note_text = "xin chao";
	e.note_url = "http://example.com";
	auto id = g.UpsertEntry(dict, e);

	auto got = g.GetEntry(id);
	EXPECT_EQ("hello", got.term);
	EXPECT_EQ("xin chao", got.note_text);
	EXPECT_EQ("http://example.com", got.note_url);

	// Update in place.
	got.note_text = "changed";
	g.UpsertEntry(dict, got);
	EXPECT_EQ("changed", g.GetEntry(id).note_text);

	g.DeleteEntry(id);
	EXPECT_EQ(0u, g.ListEntries(dict).size());
}

TEST_F(lagi_glossary, match_basic) {
	agi::Glossary g(db_path);
	auto dict = g.CreateDictionary("d");
	auto id = add(g, dict, "hello");
	g.SetActiveDictionary(dict);

	auto m = g.Match("say hello world");
	ASSERT_EQ(1u, m.size());
	EXPECT_EQ(4u, m[0].offset);
	EXPECT_EQ(5u, m[0].length);
	EXPECT_EQ(id, m[0].entry_id);
}

TEST_F(lagi_glossary, match_case_insensitive) {
	agi::Glossary g(db_path);
	auto dict = g.CreateDictionary("d");
	add(g, dict, "Hello");
	g.SetActiveDictionary(dict);

	auto m = g.Match("HELLO");
	ASSERT_EQ(1u, m.size());
	EXPECT_EQ(0u, m[0].offset);
	EXPECT_EQ(5u, m[0].length);
}

TEST_F(lagi_glossary, match_word_boundary) {
	agi::Glossary g(db_path);
	auto dict = g.CreateDictionary("d");
	add(g, dict, "cat");
	g.SetActiveDictionary(dict);

	// Must not match inside a longer word.
	EXPECT_EQ(0u, g.Match("category").size());
	EXPECT_EQ(0u, g.Match("scatter").size());
	// But a standalone word matches.
	auto m = g.Match("a cat here");
	ASSERT_EQ(1u, m.size());
	EXPECT_EQ(2u, m[0].offset);
	EXPECT_EQ(3u, m[0].length);
}

TEST_F(lagi_glossary, match_longest) {
	agi::Glossary g(db_path);
	auto dict = g.CreateDictionary("d");
	add(g, dict, "new");
	auto york = add(g, dict, "new york");
	g.SetActiveDictionary(dict);

	auto m = g.Match("new york city");
	ASSERT_EQ(1u, m.size());
	EXPECT_EQ(0u, m[0].offset);
	EXPECT_EQ(8u, m[0].length);
	EXPECT_EQ(york, m[0].entry_id);
}

TEST_F(lagi_glossary, match_cjk_substring) {
	agi::Glossary g(db_path);
	auto dict = g.CreateDictionary("d");
	add(g, dict, "\xE6\x97\xA5\xE6\x9C\xAC"); // 日本
	g.SetActiveDictionary(dict);

	// 私は日本語 -> 日本 begins at byte offset 6.
	auto m = g.Match("\xE7\xA7\x81\xE3\x81\xAF\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
	ASSERT_EQ(1u, m.size());
	EXPECT_EQ(6u, m[0].offset);
	EXPECT_EQ(6u, m[0].length);
}

TEST_F(lagi_glossary, match_multiple_and_inactive) {
	agi::Glossary g(db_path);
	auto dict = g.CreateDictionary("d");
	add(g, dict, "red");
	add(g, dict, "blue");

	// No active dictionary -> no matches.
	EXPECT_EQ(0u, g.Match("red and blue").size());

	g.SetActiveDictionary(dict);
	auto m = g.Match("red and blue");
	ASSERT_EQ(2u, m.size());
	EXPECT_EQ(0u, m[0].offset);
	EXPECT_EQ(8u, m[1].offset);

	g.SetActiveDictionary(-1);
	EXPECT_EQ(0u, g.Match("red and blue").size());
}
