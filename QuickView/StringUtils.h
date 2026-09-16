// StringUtils.h - Shared string utilities for QuickView
// Lightweight helpers that avoid <iostream>, <sstream>, <regex>, <locale>.
#pragma once
#include <string>
#include <vector>
#include <string_view>
#include <span>

namespace QuickView {

// High performance zero-heap UTF conversion into user-supplied buffer.
// Returns the number of characters written to dst (excluding null terminator, or 0 on error/truncation).
size_t Utf8ToWide(std::string_view src, std::span<wchar_t> dst);
size_t WideToUtf8(std::wstring_view src, std::span<char> dst);

// Heap allocating convenience wrappers (when std::(w)string return is needed).
std::wstring Utf8ToWide(std::string_view utf8Str);
std::string WideToUtf8(std::wstring_view wideStr);

// Split a wide string by delimiter, skipping empty tokens.
// Trims leading/trailing whitespace from each token.
std::vector<std::wstring> SplitAndTrimCSV(const std::wstring& str, wchar_t delim = L',');

// Normalize a CSV string: split, trim, deduplicate, filter against allowed keys, truncate to maxItems.
// Returns the cleaned CSV string.
std::wstring NormalizeCSV(const std::wstring& csv,
                         const std::vector<std::wstring>& allowedKeys,
                         int maxItems);

} // namespace QuickView
