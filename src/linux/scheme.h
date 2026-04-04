#pragma once

#include <webkit2/webkit2.h>

// Registered with webkit_web_context_register_uri_scheme in webview.cpp.
// gpointer is expected to be a ui::WebViewImpl*.
void on_uri_scheme_request(WebKitURISchemeRequest* request, gpointer user_data);