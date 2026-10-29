/* Demo — AppKit example: a window, a File menu, an NSAlert, and the
   system menu bar (SystemUIServer). The bold app menu — "About Demo",
   "Quit Demo" — is provided by AppKit itself. Set LYNX_DEFAULT_APP=1
   to also publish this app's menu as the system default (what a
   Finder clone does), running windowless like one. */
#include "AppKit.h"

#include <QGuiApplication>
#include <QLabel>
#include <QPushButton>
#include <cstdlib>
#include <unistd.h>

int main()
{
    NSApplication *app = NSApplication::sharedApplication();
    const bool defaultApp = qEnvironmentVariableIntValue("LYNX_DEFAULT_APP") != 0;
    if (defaultApp)
        QGuiApplication::setQuitOnLastWindowClosed(false);

    NSWindow *win = NSWindow::windowWithTitle("Demo", 420, 260);
    auto *label = new QLabel("AppKit demo — a lynxOS app", win->contentView());
    label->setAlignment(Qt::AlignCenter);
    win->contentLayout()->addWidget(label);

    auto *button = new QPushButton("Show Alert", win->contentView());
    win->contentLayout()->addWidget(button);

    QObject::connect(button, &QPushButton::clicked, [app, label] {
        NSAlert *alert = NSAlert::alertWithMessageText(
            "Delete all files?", NSWarningAlertStyle,
            "Delete", "Cancel", QString(),
            "This cannot be undone. (NSAlert demo)");
        NSModalResponse r = alert->runModal();
        if (r == NSAlertFirstButtonReturn)
            label->setText("Deleted (not really).");
        else
            label->setText("Cancelled.");
    });

    /* the File menu, 10.6 style (About/Quit live in the app menu) */
    NSMenu *file = new NSMenu("File");
    file->addItem(new NSMenuItem("Show Alert",
        [app] {
            NSAlert *alert = NSAlert::alertWithMessageText(
                "Delete all files?", NSWarningAlertStyle,
                "Delete", "Cancel", QString(),
                "This cannot be undone. (NSAlert demo)");
            alert->runModal();
        }, "Ctrl+D"));
    win->setMainMenu(file);

    if (defaultApp)
        app->setDefaultApplicationMenu(file);

    win->makeKeyAndOrderFront();
    app->run();
    return 0;
}
