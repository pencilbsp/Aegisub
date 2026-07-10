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

#include "libaegisub/glossary.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <deque>
#include <unordered_map>

#include <sqlite3.h>
#include <unicode/uchar.h>
#include <unicode/utf8.h>

namespace {
using namespace agi;

// ---- SQLite helpers -------------------------------------------------------

// RAII wrapper around a prepared statement with terse bind/column helpers.
class Stmt {
	sqlite3 *db;
	sqlite3_stmt *st = nullptr;
public:
	Stmt(sqlite3 *db, const char *sql) : db(db) {
		if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
			throw GlossaryError(sqlite3_errmsg(db));
	}
	~Stmt() { sqlite3_finalize(st); }

	Stmt(Stmt const&) = delete;
	Stmt& operator=(Stmt const&) = delete;

	void bind(int i, int64_t v) { sqlite3_bind_int64(st, i, v); }
	void bind(int i, std::string_view v) {
		sqlite3_bind_text(st, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
	}

	// Advance one row; returns true if a row is available.
	bool step() {
		int r = sqlite3_step(st);
		if (r == SQLITE_ROW) return true;
		if (r == SQLITE_DONE) return false;
		throw GlossaryError(sqlite3_errmsg(db));
	}

	int64_t col_int(int i) { return sqlite3_column_int64(st, i); }
	std::string col_str(int i) {
		auto p = reinterpret_cast<const char *>(sqlite3_column_text(st, i));
		return p ? std::string(p, sqlite3_column_bytes(st, i)) : std::string();
	}
};

// ---- Case folding with an offset map --------------------------------------

// Simple case-fold `in`, appending to `out`. When `offsets` is non-null, for
// every byte pushed to `out` we record the byte offset in `in` of the code
// point it came from, with a trailing entry mapping to in.size(). Folding is
// done per code point so folded ranges stay aligned to input code points.
void fold(std::string_view in, std::string &out, std::vector<size_t> *offsets) {
	int32_t i = 0;
	int32_t n = static_cast<int32_t>(in.size());
	while (i < n) {
		int32_t cp_start = i;
		UChar32 c;
		U8_NEXT(in.data(), i, n, c);
		if (c < 0) c = 0xFFFD;
		UChar32 f = u_foldCase(c, U_FOLD_CASE_DEFAULT);

		uint8_t buf[4];
		int32_t len = 0;
		UBool err = false;
		U8_APPEND(buf, len, 4, f, err);
		for (int32_t k = 0; k < len; ++k) {
			out.push_back(static_cast<char>(buf[k]));
			if (offsets) offsets->push_back(static_cast<size_t>(cp_start));
		}
	}
	if (offsets) offsets->push_back(in.size());
}

// A code point is "boundary-sensitive" (part of a space-delimited word) if it
// is a cased letter or a digit. Ideographs, kana and hangul are uncased "other
// letters", so terms may match as substrings within them, as intended for CJK.
bool word_char(UChar32 c) {
	switch (u_charType(c)) {
	case U_UPPERCASE_LETTER:
	case U_LOWERCASE_LETTER:
	case U_TITLECASE_LETTER:
	case U_DECIMAL_DIGIT_NUMBER:
		return true;
	default:
		return false;
	}
}

UChar32 cp_at(std::string_view s, size_t i) {
	UChar32 c;
	int idx = static_cast<int>(i);
	U8_NEXT(s.data(), idx, static_cast<int>(s.size()), c);
	return c < 0 ? 0xFFFD : c;
}

UChar32 cp_before(std::string_view s, size_t i) {
	UChar32 c;
	int idx = static_cast<int>(i);
	U8_PREV(s.data(), 0, idx, c);
	return c < 0 ? 0xFFFD : c;
}
} // namespace

namespace agi {

// ---- Aho-Corasick matcher -------------------------------------------------

struct GlossaryMatcher {
	struct Node {
		std::unordered_map<unsigned char, int> next;
		int fail = 0;
		// (folded byte length, entry id) for every pattern ending here
		std::vector<std::pair<size_t, int64_t>> out;
	};
	std::vector<Node> nodes{1};

	void Add(std::string const &folded, int64_t entry_id) {
		if (folded.empty()) return;
		int cur = 0;
		for (unsigned char ch : folded) {
			auto it = nodes[cur].next.find(ch);
			if (it == nodes[cur].next.end()) {
				int nxt = static_cast<int>(nodes.size());
				nodes.emplace_back();
				nodes[cur].next[ch] = nxt;
				cur = nxt;
			} else {
				cur = it->second;
			}
		}
		nodes[cur].out.emplace_back(folded.size(), entry_id);
	}

	void Build() {
		std::deque<int> q;
		for (auto &[ch, nxt] : nodes[0].next) {
			nodes[nxt].fail = 0;
			q.push_back(nxt);
		}
		while (!q.empty()) {
			int cur = q.front();
			q.pop_front();
			// Copy to avoid iterator invalidation concerns (no resize here).
			for (auto [ch, nxt] : nodes[cur].next) {
				int f = nodes[cur].fail;
				while (f != 0 && !nodes[f].next.count(ch))
					f = nodes[f].fail;
				auto it = nodes[f].next.find(ch);
				nodes[nxt].fail = (it != nodes[f].next.end() && it->second != nxt) ? it->second : 0;
				// Merge failure-link outputs so a single walk reports all hits.
				auto const &fout = nodes[nodes[nxt].fail].out;
				nodes[nxt].out.insert(nodes[nxt].out.end(), fout.begin(), fout.end());
				q.push_back(nxt);
			}
		}
	}

	// Report matches over folded text as (folded_end, folded_len, entry_id).
	template<typename F>
	void Run(std::string_view folded, F &&cb) const {
		int cur = 0;
		for (size_t i = 0; i < folded.size(); ++i) {
			unsigned char ch = static_cast<unsigned char>(folded[i]);
			while (cur != 0 && !nodes[cur].next.count(ch))
				cur = nodes[cur].fail;
			auto it = nodes[cur].next.find(ch);
			cur = it != nodes[cur].next.end() ? it->second : 0;
			for (auto const &[plen, id] : nodes[cur].out)
				cb(i + 1, plen, id);
		}
	}
};

// ---- Glossary -------------------------------------------------------------

void Glossary::Exec(const char *sql) {
	char *err = nullptr;
	if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
		std::string msg = err ? err : "unknown SQLite error";
		sqlite3_free(err);
		throw GlossaryError(msg);
	}
}

Glossary::Glossary(agi::fs::path const& db_path) {
	if (sqlite3_open_v2(db_path.string().c_str(), &db,
	                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
		std::string msg = db ? sqlite3_errmsg(db) : "could not open glossary database";
		sqlite3_close(db);
		throw GlossaryError(msg);
	}

	sqlite3_busy_timeout(db, 1000);
	Exec("PRAGMA foreign_keys = ON;");
	Exec(
		"CREATE TABLE IF NOT EXISTS dictionaries ("
		"  id INTEGER PRIMARY KEY,"
		"  name TEXT UNIQUE NOT NULL,"
		"  created_at INTEGER);"
		"CREATE TABLE IF NOT EXISTS entries ("
		"  id INTEGER PRIMARY KEY,"
		"  dict_id INTEGER NOT NULL REFERENCES dictionaries(id) ON DELETE CASCADE,"
		"  term TEXT NOT NULL,"
		"  term_norm TEXT NOT NULL,"
		"  note_text TEXT,"
		"  note_url TEXT,"
		"  updated_at INTEGER);"
		"CREATE INDEX IF NOT EXISTS idx_entries_dict_norm ON entries(dict_id, term_norm);");
}

Glossary::~Glossary() {
	sqlite3_close(db);
}

std::vector<std::pair<int64_t, std::string>> Glossary::ListDictionaries() {
	std::vector<std::pair<int64_t, std::string>> out;
	Stmt st(db, "SELECT id, name FROM dictionaries ORDER BY name COLLATE NOCASE;");
	while (st.step())
		out.emplace_back(st.col_int(0), st.col_str(1));
	return out;
}

int64_t Glossary::CreateDictionary(std::string_view name) {
	Stmt st(db, "INSERT INTO dictionaries (name, created_at) VALUES (?, ?);");
	st.bind(1, name);
	st.bind(2, static_cast<int64_t>(std::time(nullptr)));
	st.step();
	return sqlite3_last_insert_rowid(db);
}

int64_t Glossary::GetDictionaryId(std::string_view name) {
	Stmt st(db, "SELECT id FROM dictionaries WHERE name = ?;");
	st.bind(1, name);
	return st.step() ? st.col_int(0) : -1;
}

void Glossary::RenameDictionary(int64_t id, std::string_view name) {
	Stmt st(db, "UPDATE dictionaries SET name = ? WHERE id = ?;");
	st.bind(1, name);
	st.bind(2, id);
	st.step();
}

void Glossary::DeleteDictionary(int64_t id) {
	Stmt st(db, "DELETE FROM dictionaries WHERE id = ?;");
	st.bind(1, id);
	st.step();
	if (id == active_dict)
		SetActiveDictionary(-1);
}

std::vector<GlossaryEntry> Glossary::ListEntries(int64_t dict_id) {
	std::vector<GlossaryEntry> out;
	Stmt st(db,
		"SELECT id, term, note_text, note_url FROM entries WHERE dict_id = ? ORDER BY term COLLATE NOCASE;");
	st.bind(1, dict_id);
	while (st.step()) {
		GlossaryEntry e;
		e.id = st.col_int(0);
		e.term = st.col_str(1);
		e.note_text = st.col_str(2);
		e.note_url = st.col_str(3);
		out.emplace_back(std::move(e));
	}
	return out;
}

GlossaryEntry Glossary::GetEntry(int64_t entry_id) {
	Stmt st(db,
		"SELECT id, term, note_text, note_url FROM entries WHERE id = ?;");
	st.bind(1, entry_id);
	GlossaryEntry e;
	if (st.step()) {
		e.id = st.col_int(0);
		e.term = st.col_str(1);
		e.note_text = st.col_str(2);
		e.note_url = st.col_str(3);
	}
	return e;
}

int64_t Glossary::UpsertEntry(int64_t dict_id, GlossaryEntry const& entry) {
	std::string norm;
	fold(entry.term, norm, nullptr);

	int64_t id = entry.id;
	if (id == 0) {
		Stmt st(db,
			"INSERT INTO entries (dict_id, term, term_norm, note_text, note_url, updated_at)"
			" VALUES (?, ?, ?, ?, ?, ?);");
		st.bind(1, dict_id);
		st.bind(2, entry.term);
		st.bind(3, norm);
		st.bind(4, entry.note_text);
		st.bind(5, entry.note_url);
		st.bind(6, static_cast<int64_t>(std::time(nullptr)));
		st.step();
		id = sqlite3_last_insert_rowid(db);
	} else {
		Stmt st(db,
			"UPDATE entries SET term=?, term_norm=?, note_text=?, note_url=?, updated_at=?"
			" WHERE id=?;");
		st.bind(1, entry.term);
		st.bind(2, norm);
		st.bind(3, entry.note_text);
		st.bind(4, entry.note_url);
		st.bind(5, static_cast<int64_t>(std::time(nullptr)));
		st.bind(6, id);
		st.step();
	}

	if (dict_id == active_dict)
		SetActiveDictionary(active_dict); // rebuild matcher
	return id;
}

void Glossary::DeleteEntry(int64_t entry_id) {
	int64_t dict_id = -1;
	{
		Stmt q(db, "SELECT dict_id FROM entries WHERE id = ?;");
		q.bind(1, entry_id);
		if (q.step()) dict_id = q.col_int(0);
	}
	Stmt st(db, "DELETE FROM entries WHERE id = ?;");
	st.bind(1, entry_id);
	st.step();
	if (dict_id == active_dict && active_dict != -1)
		SetActiveDictionary(active_dict);
}

void Glossary::SetActiveDictionary(int64_t dict_id) {
	active_dict = dict_id;
	matcher.reset();
	if (dict_id < 0) return;

	auto m = std::make_unique<GlossaryMatcher>();
	Stmt st(db, "SELECT id, term_norm FROM entries WHERE dict_id = ?;");
	st.bind(1, dict_id);
	bool any = false;
	while (st.step()) {
		m->Add(st.col_str(1), st.col_int(0));
		any = true;
	}
	if (!any) return; // leave matcher null; Match() returns nothing
	m->Build();
	matcher = std::move(m);
}

std::vector<GlossaryMatch> Glossary::Match(std::string_view line_utf8) {
	std::vector<GlossaryMatch> out;
	if (!matcher || line_utf8.empty()) return out;

	std::string folded;
	std::vector<size_t> map; // folded byte index -> original byte offset
	folded.reserve(line_utf8.size());
	map.reserve(line_utf8.size() + 1);
	fold(line_utf8, folded, &map);

	struct Cand { size_t start, end; int64_t id; };
	std::vector<Cand> cands;

	matcher->Run(folded, [&](size_t fend, size_t flen, int64_t id) {
		size_t fstart = fend - flen;
		size_t ostart = map[fstart];
		size_t oend = map[fend];
		if (oend <= ostart) return;

		// Word-boundary filter for space-delimited scripts.
		UChar32 first = cp_at(line_utf8, ostart);
		if (word_char(first) && ostart > 0 && word_char(cp_before(line_utf8, ostart)))
			return;
		UChar32 last = cp_before(line_utf8, oend);
		if (word_char(last) && oend < line_utf8.size() && word_char(cp_at(line_utf8, oend)))
			return;

		cands.push_back({ostart, oend, id});
	});

	if (cands.empty()) return out;

	// Resolve overlaps preferring the longest match (then earliest).
	std::sort(cands.begin(), cands.end(), [](Cand const &a, Cand const &b) {
		size_t la = a.end - a.start, lb = b.end - b.start;
		if (la != lb) return la > lb;
		return a.start < b.start;
	});
	std::vector<char> occupied(line_utf8.size(), 0);
	for (auto const &c : cands) {
		bool free = true;
		for (size_t i = c.start; i < c.end; ++i)
			if (occupied[i]) { free = false; break; }
		if (!free) continue;
		for (size_t i = c.start; i < c.end; ++i) occupied[i] = 1;
		out.push_back({c.start, c.end - c.start, c.id});
	}

	std::sort(out.begin(), out.end(),
	          [](GlossaryMatch const &a, GlossaryMatch const &b) { return a.offset < b.offset; });
	return out;
}

}
