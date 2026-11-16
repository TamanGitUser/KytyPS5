// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Loader {
struct Program;

namespace GuestCode {

// Byte patterns use hexadecimal pairs and "??" for relocations or build-specific bytes.
class Pattern {
public:
	explicit Pattern(std::string_view text) {
		const auto digit = [](char c) {
			return c >= '0' && c <= '9' ? c - '0'
			     : c >= 'a' && c <= 'f' ? c - 'a' + 10
			     : c >= 'A' && c <= 'F' ? c - 'A' + 10
		                           : -1;
		};
		for (size_t i = 0; i < text.size();) {
			if (text[i] == ' ') {
				++i;
				continue;
			}
			if (i + 1 >= text.size()) {
				Invalid();
				return;
			}
			if (text[i] == '?' && text[i + 1] == '?') {
				m_bytes.push_back(0);
				m_mask.push_back(0);
			} else if (digit(text[i]) >= 0 && digit(text[i + 1]) >= 0) {
				m_bytes.push_back(static_cast<uint8_t>(digit(text[i]) * 16 + digit(text[i + 1])));
				m_mask.push_back(0xff);
			} else {
				Invalid();
				return;
			}
			i += 2;
		}
		for (size_t i = 0; i < m_mask.size();) {
			size_t run = 0;
			while (i + run < m_mask.size() && m_mask[i + run] != 0) ++run;
			if (run > m_anchor_size) {
				m_anchor = i;
				m_anchor_size = run;
			}
			i += run + 1;
		}
	}

	[[nodiscard]] bool Valid() const { return m_anchor_size != 0; }
	[[nodiscard]] size_t Size() const { return m_bytes.size(); }
	[[nodiscard]] bool Matches(const uint8_t* code) const {
		for (size_t i = 0; i < m_bytes.size(); ++i) {
			if ((code[i] & m_mask[i]) != m_bytes[i]) return false;
		}
		return true;
	}
	[[nodiscard]] std::vector<size_t> Find(std::span<const uint8_t> code, size_t limit) const {
		std::vector<size_t> found;
		if (!Valid() || code.size() < m_bytes.size() || limit == 0) return found;
		const auto* anchor = m_bytes.data() + m_anchor;
		const std::boyer_moore_horspool_searcher searcher(anchor, anchor + m_anchor_size);
		const auto* begin = code.data() + m_anchor;
		const auto* end = code.data() + code.size() - (m_bytes.size() - m_anchor - m_anchor_size);
		for (auto* at = std::search(begin, end, searcher);
		     at != end && found.size() < limit;
		     at = std::search(at + 1, end, searcher)) {
			if (Matches(at - m_anchor)) {
				found.push_back(static_cast<size_t>(at - m_anchor - code.data()));
			}
		}
		return found;
	}

private:
	void Invalid() {
		m_bytes.clear();
		m_mask.clear();
		m_anchor = 0;
		m_anchor_size = 0;
	}

	std::vector<uint8_t> m_bytes;
	std::vector<uint8_t> m_mask;
	size_t m_anchor = 0;
	size_t m_anchor_size = 0;
};

// Search only readable executable PT_LOAD segments of the loaded guest program.
[[nodiscard]] std::vector<uint64_t> Find(const Program& program, const Pattern& pattern, size_t limit);
[[nodiscard]] std::optional<uint64_t> FindUnique(const Program& program, const Pattern& pattern);

} // namespace GuestCode
} // namespace Loader
