#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace ui::shared {

inline std::string json_escape(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += static_cast<char>(c); break;
        }
    }
    return out;
}

inline std::string mime_for_ext(const std::string& ext)
{
    static const std::unordered_map<std::string, std::string> table = {
        {".html",  "text/html; charset=utf-8"},
        {".htm",   "text/html; charset=utf-8"},
        {".js",    "text/javascript"},
        {".mjs",   "text/javascript"},
        {".css",   "text/css"},
        {".json",  "application/json"},
        {".svg",   "image/svg+xml"},
        {".png",   "image/png"},
        {".jpg",   "image/jpeg"},
        {".jpeg",  "image/jpeg"},
        {".gif",   "image/gif"},
        {".ico",   "image/x-icon"},
        {".webp",  "image/webp"},
        {".woff",  "font/woff"},
        {".woff2", "font/woff2"},
        {".ttf",   "font/ttf"},
    };
    auto it = table.find(ext);
    return it != table.end() ? it->second : "application/octet-stream";
}

} // namespace ui::shared