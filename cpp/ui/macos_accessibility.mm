#include "main_window.hpp"
#import <AppKit/AppKit.h>
#include <QDebug>
#include <QGuiApplication>
#include <QOperatingSystemVersion>

namespace xm::ui {
bool disableMacWindowAccessibility(QWidget *window) {
    // Offscreen/minimal platforms use synthetic WIds, not NSView pointers.
    if (QGuiApplication::platformName() != "cocoa")
        return false;
    if (!shouldDisableMacAccessibility("darwin", QOperatingSystemVersion::current().majorVersion(),
                                       qgetenv("ROBOT_XML_MERGE_ENABLE_MACOS_ACCESSIBILITY")))
        return false;
    @try {
        auto *view = reinterpret_cast<NSView *>(window->winId());
        NSWindow *nativeWindow = [view window];
        if (!nativeWindow)
            return false;
        [nativeWindow setAccessibilityElement:NO];
        qWarning("Disabled native window accessibility on macOS 26+ to avoid a Qt crash. Set "
                 "ROBOT_XML_MERGE_ENABLE_MACOS_ACCESSIBILITY=1 to opt back in.");
        return true;
    } @catch (NSException *exception) {
        qWarning().noquote() << "Unable to apply macOS accessibility workaround:"
                             << QString::fromUtf8([[exception reason] UTF8String]);
        return false;
    }
}
} // namespace xm::ui
