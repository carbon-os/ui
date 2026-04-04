#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "message.h"

namespace ui {

struct WebViewImpl;

struct WebViewConfig {
    std::string title    = "ui";
    int         width    = 1280;
    int         height   = 800;
    bool        debug    = false;
    bool        logging  = false;

    std::string runtime_path;
    std::string user_data_dir;
    std::string browser_args;
};

class WebView {
public:
    using MessageCallback = std::function<void(Message)>;
    using ReadyCallback   = std::function<void()>;
    using CloseCallback   = std::function<bool()>;

    explicit WebView(WebViewConfig config = {});
    ~WebView();

    WebView(const WebView&)            = delete;
    WebView& operator=(const WebView&) = delete;
    WebView(WebView&&)                 = default;
    WebView& operator=(WebView&&)      = default;

    // Load inline HTML — origin becomes ui-ipc://app/
    void load_html(std::string_view html);

    // Load a local file; sibling assets resolve automatically under ui-ipc://app/
    void load_file(std::string_view path);

    // Load an external http/https URL — ui-ipc:// is rejected at the call site
    void load_url(std::string_view url);

    void eval(std::string_view js);

    void post_message(std::string_view channel, std::string_view text);
    void post_message(std::string_view channel, const std::vector<uint8_t>& data);

    void on_message(std::string_view channel, MessageCallback cb);
    void off_message(std::string_view channel);

    void on_ready(ReadyCallback cb);
    void on_close(CloseCallback cb);
    void run();
    void terminate();

    void set_title(std::string_view title);
    void set_size(int width, int height);

private:
    WebViewImpl* impl_;
};

} // namespace ui