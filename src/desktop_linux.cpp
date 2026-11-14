#include "desktop.hpp"
#include <FL/Fl_Window.H>
#include <FL/platform.H>
#include <X11/Xlib.h>

void pin_window(Fl_Window& window, bool pinned) {
    auto xid = fl_xid(&window);
    if (!xid || !fl_display) return;
    Atom state = XInternAtom(fl_display, "_NET_WM_STATE", False);
    Atom above = XInternAtom(fl_display, "_NET_WM_STATE_ABOVE", False);
    XEvent message{};
    message.xclient.type = ClientMessage;
    message.xclient.window = xid;
    message.xclient.message_type = state;
    message.xclient.format = 32;
    message.xclient.data.l[0] = pinned ? 1 : 0;
    message.xclient.data.l[1] = above;
    message.xclient.data.l[3] = 1;
    XSendEvent(fl_display, DefaultRootWindow(fl_display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &message);
    XFlush(fl_display);
}
