// GPU-free fixture: link with mac_gl.mm and AppKit; never create NSApplication or a window.
#import <AppKit/AppKit.h>
#include "window_state.h"
#include <cstdio>

int main() {
  @autoreleasepool {
    const bool no_app_before = NSApp == nil;
    mac_disable_window_restoration();
    mac_disable_window_restoration();  // Multiple startup guards are harmless.
    NSDictionary* registered = [NSUserDefaults.standardUserDefaults volatileDomainForName:NSRegistrationDomain];
    const bool passed = [registered[@"ApplePersistenceIgnoreState"] isEqual:@YES] &&
        [registered[@"NSQuitAlwaysKeepsWindows"] isEqual:@NO] &&
        no_app_before && NSApp == nil;
    std::puts(passed ? "Mac window restoration defaults: PASS" : "Mac window restoration defaults: FAIL");
    return passed ? 0 : 1;
  }
}
