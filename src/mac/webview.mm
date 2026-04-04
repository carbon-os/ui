#include "impl.h"
#include "delegates.h"

#include <ui/webview.h>
#include <shared/shim.h>
#include <logger/logger.h>

#include <string>

namespace ui {

WebView::WebView(WebViewConfig config) : impl_(new WebViewImpl())
{
    if (config.logging)
        logger::SetEnabled(true);

    logger::Info("WebView constructing — title='{}' size={}x{} debug={} logging={}",
                 config.title, config.width, config.height,
                 config.debug, config.logging);

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    // ── Window ────────────────────────────────────────────────────────────────
    NSRect frame = NSMakeRect(0, 0,
                              static_cast<CGFloat>(config.width),
                              static_cast<CGFloat>(config.height));

    NSWindowStyleMask style = NSWindowStyleMaskTitled
                            | NSWindowStyleMaskClosable
                            | NSWindowStyleMaskMiniaturizable
                            | NSWindowStyleMaskResizable;

    impl_->window = [[NSWindow alloc] initWithContentRect:frame
                                                styleMask:style
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    [impl_->window setTitle:[NSString stringWithUTF8String:config.title.c_str()]];
    [impl_->window center];
    logger::Debug("NSWindow created");

    auto* win_delegate = [[UIWindowDelegate alloc] initWithImpl:impl_];
    impl_->window_delegate = win_delegate;
    [impl_->window setDelegate:win_delegate];

    // ── WKWebView configuration ───────────────────────────────────────────────
    WKWebViewConfiguration* wk_config = [[WKWebViewConfiguration alloc] init];

    auto* handler = [[UISchemeHandler alloc] initWithImpl:impl_];
    impl_->scheme_handler = handler;
    [wk_config setURLSchemeHandler:handler forURLScheme:@"ui-ipc"];
    logger::Debug("custom scheme 'ui-ipc' registered");

    WKUserScript* shim =
        [[WKUserScript alloc] initWithSource:[NSString stringWithUTF8String:shared::k_shim]
                               injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                            forMainFrameOnly:NO];
    [wk_config.userContentController addUserScript:shim];
    logger::Debug("IPC shim injected via WKUserScript");

    if (config.debug) {
        [wk_config.preferences setValue:@YES forKey:@"developerExtrasEnabled"];
        logger::Debug("DevTools enabled");
    }

    // ── WKWebView ─────────────────────────────────────────────────────────────
    impl_->webview = [[WKWebView alloc] initWithFrame:frame configuration:wk_config];
    [impl_->window setContentView:impl_->webview];
    [impl_->window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    logger::Info("WebView fully initialised");
}

WebView::~WebView()
{
    logger::Info("WebView destructor");
    delete impl_;
}

void WebView::eval(std::string_view js)
{
    logger::Debug("eval: {} chars of JS", js.size());
    NSString* src = [NSString stringWithUTF8String:std::string(js).c_str()];
    [impl_->webview evaluateJavaScript:src completionHandler:^(id, NSError* err) {
        if (err)
            logger::Warn("eval error: {}", err.localizedDescription.UTF8String);
    }];
}

} // namespace ui