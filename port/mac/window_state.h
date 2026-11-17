// Process-local AppKit restoration policy. Call before SDL starts video/AppKit.
#pragma once
#ifdef __APPLE__
extern "C" void mac_disable_window_restoration();
extern "C" void mac_window_disable_restoration(void* nswindow);
#endif
