#pragma once

// Windows-specific IPC shim.
// Delivery differs from Linux/Mac: text arrives via PostWebMessageAsJson +
// chrome.webview, binary is still fetched via ui-ipc://host/. The public
// surface (window.__ui) is identical across all three platforms.

namespace ui::win {

inline constexpr const char* k_shim = R"js(
window.__ui = (() => {
    const _listeners = {};

    window.chrome.webview.addEventListener('message', async (event) => {
        const msg = event.data;
        if (!msg || msg.type !== 'host_ipc_message') return;

        const { channel, text, token } = msg;

        if (text !== undefined) {
            const cb = _listeners[channel];
            if (cb) cb(text);
            return;
        }

        const res = await fetch(`ui-ipc://host/message/${channel}/${token}`);
        if (!res.ok) return;
        const buf = await res.arrayBuffer();
        const cb  = _listeners[channel];
        if (cb) cb(buf);
    });

    return {
        on(channel, cb)  { _listeners[channel] = cb; },
        off(channel)     { delete _listeners[channel]; },

        post(channel, data) {
            if (data instanceof ArrayBuffer || ArrayBuffer.isView(data)) {
                const body = data instanceof ArrayBuffer ? data : data.buffer;
                fetch('ui-ipc://js/binary/' + channel, { method: 'POST', body });
            } else {
                fetch('ui-ipc://js/text/' + channel, {
                    method: 'POST',
                    body: new TextEncoder().encode(String(data))
                });
            }
        }
    };
})();
)js";

} // namespace ui::win