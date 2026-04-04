#include "scheme.h"
#include "impl.h"

#include <shared/helpers.h>
#include <logger/logger.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ── Stream helpers ────────────────────────────────────────────────────────────

static std::vector<uint8_t> read_gstream(GInputStream* stream)
{
    if (!stream) return {};
    std::vector<uint8_t> out;
    uint8_t buf[4096];
    while (true) {
        gsize    n   = 0;
        GError*  err = nullptr;
        gboolean ok  = g_input_stream_read_all(stream, buf, sizeof(buf),
                                               &n, nullptr, &err);
        if (err) { g_error_free(err); break; }
        if (n > 0) out.insert(out.end(), buf, buf + n);
        if (!ok || n < sizeof(buf)) break;
    }
    return out;
}

static void finish_stream(WebKitURISchemeRequest* req,
                           const void* data, gsize size, const char* mime)
{
    GBytes*       bytes  = g_bytes_new(data, size);
    GInputStream* stream = g_memory_input_stream_new_from_bytes(bytes);
    g_bytes_unref(bytes);
    webkit_uri_scheme_request_finish(req, stream, static_cast<gint64>(size), mime);
    g_object_unref(stream);
}

static void finish_no_content(WebKitURISchemeRequest* req)
{
    GInputStream* empty = g_memory_input_stream_new();
    webkit_uri_scheme_request_finish(req, empty, 0, "text/plain");
    g_object_unref(empty);
}

static void finish_error(WebKitURISchemeRequest* req,
                          GIOErrorEnum code, const char* msg)
{
    webkit_uri_scheme_request_finish_error(req,
        g_error_new(G_IO_ERROR, code, "%s", msg));
}

// ── Route handler ─────────────────────────────────────────────────────────────

void on_uri_scheme_request(WebKitURISchemeRequest* request, gpointer user_data)
{
    auto* impl = static_cast<ui::WebViewImpl*>(user_data);

    std::string uri  = webkit_uri_scheme_request_get_uri(request);
    std::string path = uri.substr(9); // strip "ui-ipc://"

    logger::Debug("WebResourceRequested: {}", uri);

    auto first_slash = path.find('/');
    if (first_slash == std::string::npos) {
        finish_error(request, G_IO_ERROR_INVALID_ARGUMENT, "bad uri");
        return;
    }

    const std::string authority = path.substr(0, first_slash);
    const std::string rest      = path.substr(first_slash + 1);

    if (authority != "app") {
        logger::Warn("unhandled ui-ipc authority: '{}'", authority);
        finish_error(request, G_IO_ERROR_INVALID_ARGUMENT, "unknown authority");
        return;
    }

    // ── /-/js/{verb}/{channel}  — JS → C++ ───────────────────────────────────
    if (rest.rfind("-/js/", 0) == 0) {
        const std::string ipc    = rest.substr(5);
        const auto        vslash = ipc.find('/');
        if (vslash == std::string::npos) { finish_no_content(request); return; }

        const std::string verb    = ipc.substr(0, vslash);
        const std::string channel = ipc.substr(vslash + 1);

        std::vector<uint8_t> bytes =
            read_gstream(webkit_uri_scheme_request_get_http_body(request));

        logger::Info("JS → C++ [{}] via '{}' ({} bytes)", verb, channel, bytes.size());

        ui::WebView::MessageCallback cb;
        {
            std::lock_guard lock(impl->channels_mutex);
            auto it = impl->channels.find(channel);
            if (it != impl->channels.end()) cb = it->second;
        }

        if (cb) {
            logger::Debug("dispatching to registered handler for '{}'", channel);
            if (verb == "text")
                cb(ui::Message(std::string(bytes.begin(), bytes.end())));
            else
                cb(ui::Message(std::move(bytes)));
        } else {
            logger::Warn("no handler registered for channel '{}'", channel);
        }

        finish_no_content(request);
        return;
    }

    // ── /-/host/message/{channel}/{token}  — C++ → JS binary fetch ───────────
    if (rest.rfind("-/host/message/", 0) == 0) {
        const std::string tail        = rest.substr(15);
        const auto        token_slash = tail.find('/');
        if (token_slash == std::string::npos) { finish_no_content(request); return; }

        const std::string channel   = tail.substr(0, token_slash);
        const std::string token_str = tail.substr(token_slash + 1);
        const std::string slot_key  = channel + ":" + token_str;

        std::vector<uint8_t> data;
        {
            std::lock_guard lock(impl->slots_mutex);
            auto it = impl->slots.find(slot_key);
            if (it != impl->slots.end()) {
                data = std::move(it->second);
                impl->slots.erase(it);
                logger::Info("C++ → JS binary fetch: channel='{}' token={} ({} bytes)",
                             channel, token_str, data.size());
            } else {
                logger::Warn("binary slot not found for key '{}'", slot_key);
            }
        }

        finish_stream(request, data.data(), data.size(), "application/octet-stream");
        return;
    }

    // ── App content (Html / File) ─────────────────────────────────────────────
    std::lock_guard lock(impl->load_mutex);

    switch (impl->load_mode) {

    case ui::LoadMode::Html: {
        if (rest.empty() || rest == "/" || rest.back() == '/') {
            finish_stream(request,
                impl->html_src.data(), impl->html_src.size(),
                "text/html; charset=utf-8");
            logger::Info("app/html: served inline HTML ({} bytes)", impl->html_src.size());
        } else {
            finish_error(request, G_IO_ERROR_NOT_FOUND, "not found");
        }
        return;
    }

    case ui::LoadMode::File: {
        namespace fs = std::filesystem;

        fs::path requested = (fs::path(impl->file_root) / rest).lexically_normal();
        auto     root_abs  = fs::absolute(impl->file_root);
        auto     req_abs   = fs::absolute(requested);

        if (req_abs.string().rfind(root_abs.string(), 0) != 0) {
            logger::Warn("app/file: path traversal blocked: {}", req_abs.string());
            finish_error(request, G_IO_ERROR_PERMISSION_DENIED, "forbidden");
            return;
        }

        std::ifstream f(requested, std::ios::binary);
        if (!f) {
            logger::Warn("app/file: not found: {}", requested.string());
            finish_error(request, G_IO_ERROR_NOT_FOUND, "not found");
            return;
        }

        std::vector<uint8_t> data(
            (std::istreambuf_iterator<char>(f)),
             std::istreambuf_iterator<char>());

        std::string mime = ui::shared::mime_for_ext(requested.extension().string());
        finish_stream(request, data.data(), data.size(), mime.c_str());
        logger::Info("app/file: served '{}' ({} bytes)", requested.string(), data.size());
        return;
    }

    default:
        logger::Warn("app: request received but load_mode is None");
        finish_error(request, G_IO_ERROR_NOT_FOUND, "no content loaded");
        return;
    }
}