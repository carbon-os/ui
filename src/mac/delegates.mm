#include "delegates.h"
#include "impl.h"

#include <shared/helpers.h>
#include <logger/logger.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ── C++ helper: read NSURLRequest body ───────────────────────────────────────

static std::vector<uint8_t> read_request_body(NSURLRequest* req)
{
    if (NSData* body = req.HTTPBody; body && body.length > 0) {
        const auto* bytes = static_cast<const uint8_t*>(body.bytes);
        return {bytes, bytes + body.length};
    }
    if (NSInputStream* stream = req.HTTPBodyStream) {
        [stream open];
        std::vector<uint8_t> out;
        uint8_t buf[4096];
        NSInteger n;
        while ((n = [stream read:buf maxLength:sizeof(buf)]) > 0)
            out.insert(out.end(), buf, buf + n);
        [stream close];
        return out;
    }
    return {};
}

// ── UISchemeHandler ───────────────────────────────────────────────────────────

@implementation UISchemeHandler {
    ui::WebViewImpl* _impl;
    NSMutableSet*    _active;
}

- (instancetype)initWithImpl:(ui::WebViewImpl*)impl
{
    self   = [super init];
    _impl  = impl;
    _active = [NSMutableSet new];
    return self;
}

- (void)webView:(WKWebView*)webView startURLSchemeTask:(id<WKURLSchemeTask>)task
{
    [_active addObject:task];

    NSURLRequest* req = task.request;
    std::string uri = req.URL.absoluteString.UTF8String
                    ? req.URL.absoluteString.UTF8String : "";

    logger::Debug("WebResourceRequested: {}", uri);

    if (uri.size() < 9) { [self error:task code:400]; return; }
    std::string path = uri.substr(9); // strip "ui-ipc://"

    auto first_slash = path.find('/');
    if (first_slash == std::string::npos) { [self error:task code:400]; return; }

    const std::string authority = path.substr(0, first_slash);
    const std::string rest      = path.substr(first_slash + 1);

    if (authority != "app") {
        logger::Warn("unhandled ui-ipc authority: '{}'", authority);
        [self error:task code:400];
        return;
    }

    // ── /-/js/{verb}/{channel}  — JS → C++ ───────────────────────────────────
    if (rest.rfind("-/js/", 0) == 0) {
        const std::string ipc    = rest.substr(5);
        const auto        vslash = ipc.find('/');
        if (vslash == std::string::npos) { [self noContent:task]; return; }

        const std::string verb    = ipc.substr(0, vslash);
        const std::string channel = ipc.substr(vslash + 1);

        std::vector<uint8_t> bytes = read_request_body(req);

        logger::Info("JS → C++ [{}] via '{}' ({} bytes)", verb, channel, bytes.size());

        ui::WebView::MessageCallback cb;
        {
            std::lock_guard lock(_impl->channels_mutex);
            auto it = _impl->channels.find(channel);
            if (it != _impl->channels.end()) cb = it->second;
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

        [self noContent:task];
        return;
    }

    // ── /-/host/message/{channel}/{token}  — C++ → JS binary fetch ───────────
    if (rest.rfind("-/host/message/", 0) == 0) {
        const std::string tail      = rest.substr(15);
        const auto        tok_slash = tail.find('/');
        if (tok_slash == std::string::npos) { [self noContent:task]; return; }

        const std::string channel   = tail.substr(0, tok_slash);
        const std::string token_str = tail.substr(tok_slash + 1);
        const std::string slot_key  = channel + ":" + token_str;

        std::vector<uint8_t> data;
        {
            std::lock_guard lock(_impl->slots_mutex);
            auto it = _impl->slots.find(slot_key);
            if (it != _impl->slots.end()) {
                data = std::move(it->second);
                _impl->slots.erase(it);
                logger::Info("C++ → JS binary fetch: channel='{}' token={} ({} bytes)",
                             channel, token_str, data.size());
            } else {
                logger::Warn("binary slot not found for key '{}'", slot_key);
            }
        }

        NSData* ns_data = [NSData dataWithBytes:data.data() length:data.size()];
        [self respond:task data:ns_data mime:@"application/octet-stream" status:200];
        return;
    }

    // ── App content (Html / File) ─────────────────────────────────────────────
    std::lock_guard lock(_impl->load_mutex);

    switch (_impl->load_mode) {

    case ui::LoadMode::Html: {
        if (rest.empty() || rest == "/" || rest.back() == '/') {
            NSData* data = [NSData dataWithBytes:_impl->html_src.data()
                                          length:_impl->html_src.size()];
            logger::Info("app/html: served inline HTML ({} bytes)", _impl->html_src.size());
            [self respond:task data:data mime:@"text/html; charset=utf-8" status:200];
        } else {
            [self error:task code:404];
        }
        return;
    }

    case ui::LoadMode::File: {
        namespace fs = std::filesystem;

        fs::path requested = (fs::path(_impl->file_root) / rest).lexically_normal();
        auto     root_abs  = fs::absolute(_impl->file_root);
        auto     req_abs   = fs::absolute(requested);

        if (req_abs.string().rfind(root_abs.string(), 0) != 0) {
            logger::Warn("app/file: path traversal blocked: {}", req_abs.string());
            [self error:task code:403];
            return;
        }

        std::ifstream f(requested, std::ios::binary);
        if (!f) {
            logger::Warn("app/file: not found: {}", requested.string());
            [self error:task code:404];
            return;
        }

        std::vector<uint8_t> file_data(
            (std::istreambuf_iterator<char>(f)),
             std::istreambuf_iterator<char>());

        NSData*   ns_data = [NSData dataWithBytes:file_data.data() length:file_data.size()];
        NSString* ns_mime = [NSString stringWithUTF8String:
            ui::shared::mime_for_ext(requested.extension().string()).c_str()];

        logger::Info("app/file: served '{}' ({} bytes)", requested.string(), file_data.size());
        [self respond:task data:ns_data mime:ns_mime status:200];
        return;
    }

    default:
        logger::Warn("app: request received but load_mode is None");
        [self error:task code:404];
        return;
    }
}

- (void)webView:(WKWebView*)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task
{
    [_active removeObject:task];
}

// ── Private response helpers ──────────────────────────────────────────────────

- (void)respond:(id<WKURLSchemeTask>)task
           data:(NSData*)data
           mime:(NSString*)mime
         status:(NSInteger)status
{
    if (![_active containsObject:task]) return;

    NSDictionary* headers = @{
        @"Content-Type":                mime,
        @"Access-Control-Allow-Origin": @"*",
    };
    NSHTTPURLResponse* resp =
        [[NSHTTPURLResponse alloc] initWithURL:task.request.URL
                                    statusCode:status
                                   HTTPVersion:@"HTTP/1.1"
                                  headerFields:headers];
    [task didReceiveResponse:resp];
    [task didReceiveData:data ? data : [NSData data]];
    [task didFinish];
    [_active removeObject:task];
}

- (void)noContent:(id<WKURLSchemeTask>)task
{
    if (![_active containsObject:task]) return;

    NSHTTPURLResponse* resp =
        [[NSHTTPURLResponse alloc] initWithURL:task.request.URL
                                    statusCode:204
                                   HTTPVersion:@"HTTP/1.1"
                                  headerFields:@{}];
    [task didReceiveResponse:resp];
    [task didReceiveData:[NSData data]];
    [task didFinish];
    [_active removeObject:task];
}

- (void)error:(id<WKURLSchemeTask>)task code:(NSInteger)code
{
    if (![_active containsObject:task]) return;

    [task didFailWithError:[NSError errorWithDomain:NSURLErrorDomain
                                               code:code
                                           userInfo:nil]];
    [_active removeObject:task];
}

@end

// ── UIWindowDelegate ──────────────────────────────────────────────────────────

@implementation UIWindowDelegate {
    ui::WebViewImpl* _impl;
}

- (instancetype)initWithImpl:(ui::WebViewImpl*)impl
{
    self  = [super init];
    _impl = impl;
    return self;
}

- (BOOL)windowShouldClose:(NSWindow*)sender
{
    logger::Info("windowShouldClose received");
    if (_impl->on_close_cb) {
        if (!_impl->on_close_cb()) {
            logger::Info("on_close callback suppressed close");
            return NO;
        }
    }
    return YES;
}

- (void)windowWillClose:(NSNotification*)notification
{
    logger::Info("windowWillClose — stopping run loop");
    [NSApp stop:nil];
    NSEvent* dummy = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSMakePoint(0, 0)
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0];
    [NSApp postEvent:dummy atStart:YES];
}

@end