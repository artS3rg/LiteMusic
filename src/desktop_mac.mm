#include "desktop.hpp"
#include <FL/Fl_Window.H>
#include <FL/platform.H>
#include <Cocoa/Cocoa.h>

void pin_window(Fl_Window& window, bool pinned) {
    // FLTK only forward-declares its private NSWindow subclass.
    auto* native = (NSWindow*)fl_xid(&window);
    [native setLevel:pinned ? NSFloatingWindowLevel : NSNormalWindowLevel];
}
