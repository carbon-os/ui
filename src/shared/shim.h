#pragma once

// Shared IPC shim for Linux (GTK/WebKit) and macOS (WKWebView).
// Windows uses a different delivery mechanism (PostWebMessageAsJson +
// chrome.webview) and defines its own shim in src/win/shim.h.
//
// All fetches target ui-ipc://app/ so they are same-origin — WebKit's
// CORS check never fires. IPC paths are reserved under the /-/ prefix.

namespace ui::shared {

inline constexpr const char* k_shim = R"js(
window.__ui = (() => {
    const _listeners = {};

    return {
        on(channel, cb)  { _listeners[channel] = cb; },
        off(channel)     { delete _listeners[channel]; },

        // C++ → JS binary: fetch payload, deliver as ArrayBuffer
        async _dispatch(channel, token) {
            const res = await fetch(
                'ui-ipc://app/-/host/message/' + channel + '/' + token);
            if (!res.ok) return;
            const buf = await res.arrayBuffer();
            const cb  = _listeners[channel];
            if (cb) cb(buf);
        },

        // C++ → JS text: direct delivery via eval, no fetch needed
        _dispatchText(channel, text) {
            const cb = _listeners[channel];
            if (cb) cb(text);
        },

        // JS → C++
        post(channel, data) {
            if (data instanceof ArrayBuffer || ArrayBuffer.isView(data)) {
                const body = data instanceof ArrayBuffer ? data : data.buffer;
                fetch('ui-ipc://app/-/js/binary/' + channel,
                      { method: 'POST', body });
            } else {
                fetch('ui-ipc://app/-/js/text/' + channel, {
                    method: 'POST',
                    body: new TextEncoder().encode(String(data))
                });
            }
        }
    };
})();
)js";

} // namespace ui::shared