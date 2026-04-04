#include "scheme.h"
#include "impl.h"
#include "wstring.h"

#include <shared/helpers.h>
#include <logger/logger.h>

#include <ole2.h>
#include <wrl/client.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// ── IStream helpers ───────────────────────────────────────────────────────────

static std::vector<uint8_t> read_stream(IStream* stream)
{
    if (!stream) return {};
    std::vector<uint8_t> out;
    uint8_t buf[4096];
    ULONG   read = 0;
    while (SUCCEEDED(stream->Read(buf, sizeof(buf), &read)) && read > 0)
        out.insert(out.end(), buf, buf + read);
    return out;
}

static ComPtr<IStream> make_stream(const std::vector<uint8_t>& data)
{
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, data.size());
    if (!hg) return nullptr;
    void* ptr = GlobalLock(hg);
    if (ptr) { memcpy(ptr, data.data(), data.size()); GlobalUnlock(hg); }
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(hg, TRUE, &stream))) { GlobalFree(hg); return nullptr; }
    return ComPtr<IStream>(stream);
}

// ── Route handler ─────────────────────────────────────────────────────────────

HRESULT handle_resource_request(
    ui::WebViewImpl*                            impl,
    ICoreWebView2WebResourceRequestedEventArgs* args)
{
    ComPtr<ICoreWebView2WebResourceRequest> req;
    args->get_Request(&req);

    LPWSTR raw_uri = nullptr;
    req->get_Uri(&raw_uri);
    std::string uri = ui::win::to_utf8(raw_uri);
    CoTaskMemFree(raw_uri);

    logger::Debug("WebResourceRequested: {}", uri);

    // Strip "ui-ipc://" (9 chars)
    std::string path = uri.substr(9);

    auto first_slash = path.find('/');
    if (first_slash == std::string::npos) return S_OK;

    const std::string authority = path.substr(0, first_slash);
    const std::string rest      = path.substr(first_slash + 1);
    const auto        second_slash = rest.find('/');

    // ── ui-ipc://app — serves user content ───────────────────────────────────
    if (authority == "app") {
        std::lock_guard lock(impl->load_mutex);

        switch (impl->load_mode) {

        case ui::LoadMode::Html: {
            if (rest.empty() || rest == "/" || rest.back() == '/') {
                auto stream = make_stream({
                    impl->html_src.begin(),
                    impl->html_src.end()});
                ComPtr<ICoreWebView2WebResourceResponse> response;
                impl->env->CreateWebResourceResponse(
                    stream.Get(), 200, L"OK",
                    L"Content-Type: text/html; charset=utf-8",
                    &response);
                args->put_Response(response.Get());
                logger::Info("app/html: served inline HTML ({} bytes)",
                             impl->html_src.size());
            }
            return S_OK;
        }

        case ui::LoadMode::File: {
            namespace fs = std::filesystem;

            fs::path requested = (fs::path(impl->file_root) / rest).lexically_normal();
            auto     root_abs  = fs::absolute(impl->file_root);
            auto     req_abs   = fs::absolute(requested);

            if (req_abs.string().rfind(root_abs.string(), 0) != 0) {
                logger::Warn("app/file: path traversal blocked: {}", req_abs.string());
                return S_OK;
            }

            std::ifstream f(requested, std::ios::binary);
            if (!f) {
                logger::Warn("app/file: not found: {}", requested.string());
                return S_OK;
            }

            std::vector<uint8_t> data(
                (std::istreambuf_iterator<char>(f)),
                 std::istreambuf_iterator<char>());

            auto stream = make_stream(data);
            ComPtr<ICoreWebView2WebResourceResponse> response;
            impl->env->CreateWebResourceResponse(
                stream.Get(), 200, L"OK",
                ui::shared::mime_for_ext(requested.extension().string()).empty()
                    ? L"Content-Type: application/octet-stream"
                    : [&]() -> std::wstring {
                        // Build the wide header string from the MIME result
                        std::string header = "Content-Type: "
                            + ui::shared::mime_for_ext(requested.extension().string());
                        return ui::win::to_wide(header);
                    }().c_str(),
                &response);
            args->put_Response(response.Get());

            logger::Info("app/file: served '{}' ({} bytes)",
                         requested.string(), data.size());
            return S_OK;
        }

        default:
            logger::Warn("app: request received but load_mode is None");
            return S_OK;
        }
    }

    // ── ui-ipc://js — JS → C++ messages ──────────────────────────────────────
    if (authority == "js") {
        if (second_slash == std::string::npos) return S_OK;

        const std::string verb    = rest.substr(0, second_slash);
        const std::string channel = rest.substr(second_slash + 1);

        ComPtr<IStream> body_stream;
        req->get_Content(&body_stream);
        std::vector<uint8_t> bytes = read_stream(body_stream.Get());

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

        ComPtr<ICoreWebView2WebResourceResponse> response;
        impl->env->CreateWebResourceResponse(
            nullptr, 204, L"No Content",
            L"Access-Control-Allow-Origin: *",
            &response);
        args->put_Response(response.Get());
        return S_OK;
    }

    // ── ui-ipc://host — C++ → JS binary fetch ────────────────────────────────
    if (authority == "host") {
        if (second_slash == std::string::npos) return S_OK;

        const std::string verb = rest.substr(0, second_slash);
        const std::string tail = rest.substr(second_slash + 1);

        if (verb == "message") {
            auto token_slash = tail.find('/');
            if (token_slash == std::string::npos) return S_OK;

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

            ComPtr<IStream> stream = make_stream(data);
            ComPtr<ICoreWebView2WebResourceResponse> response;
            impl->env->CreateWebResourceResponse(
                stream.Get(), 200, L"OK",
                L"Content-Type: application/octet-stream\r\n"
                L"Access-Control-Allow-Origin: *",
                &response);
            args->put_Response(response.Get());
            return S_OK;
        }
    }

    logger::Warn("unhandled ui-ipc route: authority='{}'", authority);
    return S_OK;
}