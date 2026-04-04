#pragma once

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

// Forward-declare the C++ impl so callers don't need to pull in the full struct.
namespace ui { struct WebViewImpl; }

@interface UISchemeHandler : NSObject <WKURLSchemeHandler>
- (instancetype)initWithImpl:(ui::WebViewImpl*)impl;
@end

@interface UIWindowDelegate : NSObject <NSWindowDelegate>
- (instancetype)initWithImpl:(ui::WebViewImpl*)impl;
@end