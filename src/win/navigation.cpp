#include "impl.h"
#include "wstring.h"

#include <ui/webview.h>
#include <logger/logger.h>

#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>

namespace ui {

void WebView::load_html(std::string_view html)
{
    logger::Info("load_html: {} bytes of inline HTML", html.size());
    {
        std::lock_guard lock(impl_->load_mutex);
        impl_->load_mode = LoadMode::Html;
        impl_->html_src  = std::string(html);
        impl_->file_root.clear();
        impl_->file_entry.clear();
    }
    impl_->webview->Navigate(L"ui-ipc://app/");
}

void WebView::load_file(std::string_view path)
{
    namespace fs = std::filesystem;
    fs::path p = fs::absolute(fs::path(path).lexically_normal());

    logger::Info("load_file: {}", p.string());
    {
        std::lock_guard lock(impl_->load_mutex);
        impl_->load_mode  = LoadMode::File;
        impl_->html_src.clear();
        impl_->file_root  = p.parent_path().string();
        impl_->file_entry = p.filename().string();
    }

    std::string url = "ui-ipc://app/" + p.filename().string();
    impl_->webview->Navigate(win::to_wide(url).c_str());
}

void WebView::load_url(std::string_view url)
{
    if (url.starts_with("ui-ipc://")) {
        logger::Error("load_url: ui-ipc:// is reserved — use load_html or load_file");
        throw std::invalid_argument("ui::WebView: load_url does not accept ui-ipc:// URLs");
    }
    logger::Info("load_url: {}", url);
    {
        std::lock_guard lock(impl_->load_mutex);
        impl_->load_mode = LoadMode::None;
        impl_->html_src.clear();
        impl_->file_root.clear();
        impl_->file_entry.clear();
    }
    impl_->webview->Navigate(win::to_wide(url).c_str());
}

} // namespace ui