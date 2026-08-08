/*
 * main.m -- legacy Mac OS X Cocoa GUI frontend wrapper for BoltPack
 *
 * Target:   Mac OS X 10.5 Leopard on PowerPC and Mac OS X 10.5-10.8 on Intel
 * Toolkit:  Cocoa (AppKit) in legacy compatibility mode
 * Build:    native Apple GCC / Clang without ARC
 *
 * This frontend intentionally reuses the shared implementation in
 * src/frontend/macosx_gui/main.m, but compiles it with BP_LEGACY_COCOA=1 so
 * older Objective-C syntax and Leopard-era AppKit APIs are selected.
 */

/* PowerPC native legacy GUI builds default development logging on.
 * Override with -DBP_GUI_DEV_LOGGING=0 to disable it again, or set 1
 * explicitly on later development builds if you want it back. */
#ifndef BP_GUI_DEV_LOGGING
#if defined(__POWERPC__) || defined(__ppc__) || defined(__ppc64__)
#define BP_GUI_DEV_LOGGING 1
#else
#define BP_GUI_DEV_LOGGING 0
#endif
#endif

#define BP_LEGACY_COCOA 1
#include "../macosx_gui/main.m"