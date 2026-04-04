#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <string_view>

namespace ui::win {

inline std::wstring to_wide(std::string_view s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        w.data(), n);
    return w;
}

inline std::wstring to_wide_path(std::string_view s)
{
    std::wstring w = to_wide(s);
    for (auto& c : w)
        if (c == L'/') c = L'\\';
    return w;
}

inline std::string to_utf8(const wchar_t* w)
{
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

inline const wchar_t* wide_or_null(const std::wstring& s)
{
    return s.empty() ? nullptr : s.c_str();
}

} // namespace ui::win