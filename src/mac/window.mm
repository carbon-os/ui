#include "impl.h"

#include <ui/webview.h>
#include <logger/logger.h>

#include <string>

namespace ui {

void WebView::set_title(std::string_view title)
{
    logger::Debug("set_title: '{}'", title);
    [impl_->window setTitle:[NSString stringWithUTF8String:std::string(title).c_str()]];
}

void WebView::set_size(int width, int height)
{
    logger::Debug("set_size: {}x{}", width, height);
    NSRect frame = impl_->window.frame;
    frame.size   = NSMakeSize(static_cast<CGFloat>(width),
                              static_cast<CGFloat>(height));
    [impl_->window setFrame:frame display:YES];
}

} // namespace ui