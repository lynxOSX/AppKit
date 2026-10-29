# AppKit

The lynxOS application framework: **Qt 6 Widgets with the naming
conventions of the real macOS 10.6 SDK**, so lynxOS apps read like
AppKit code. Apps built with [AppBuilder](https://github.com/lynxOSX/AppBuilder)
vendor this library into their `Sources/` (see below), or link the
static library.

```cpp
#include "AppKit.h"

int main()
{
    NSApplication *app = NSApplication::sharedApplication();

    NSWindow *win = NSWindow::windowWithTitle("Hello", 420, 300);
    auto *label = new QLabel("Hello from a lynxOS app", win->contentView());
    win->contentLayout()->addWidget(label);

    NSMenu *file = new NSMenu("File");
    file->addItem(new NSMenuItem("New Window", [] { /* ... */ }));
    win->setMainMenu(file);

    /* the app menu on the left — SystemUIServer picks it up: */
    NSMenu *appMenu = new NSMenu("Hello");
    appMenu->addItem(new NSMenuItem("About Hello",
        [app] { app->orderFrontStandardAboutPanel(); }));
    appMenu->addSeparator();
    appMenu->addItem(new NSMenuItem("Quit Hello", [app] { exit(0); },
        "Ctrl+Q"));
    app->setDefaultApplicationMenu(appMenu);

    win->makeKeyAndOrderFront();
    app->run();
    return 0;
}
```

## What maps to what

| macOS 10.6 AppKit | AppKit (this library) | backed by |
|---|---|---|
| `NSApplication` / `+sharedApplication` / `-run` | `NSApplication::sharedApplication()` / `run()` | `QApplication` |
| `-orderFrontStandardAboutPanel:` | `NSApplication::orderFrontStandardAboutPanel()` | `QDialog` + bundle `Info.json` |
| `NSWindow`, `-makeKeyAndOrderFront:` | `NSWindow::windowWithTitle()`, `makeKeyAndOrderFront()` | `QMainWindow` |
| `NSMenu`, `NSMenuItem` (`initWithTitle:action:keyEquivalent:`) | `NSMenu`, `NSMenuItem(title, action, keyEquivalent)` | `QMenuBar` |
| `NSAlert` (`messageText`, `informativeText`, `alertStyle`, `addButtonWithTitle:`, `-runModal`) | same names | `QMessageBox` |
| `NSInformationalAlertStyle` / `NSWarningAlertStyle` / `NSCriticalAlertStyle` | same enum values | — |
| `NSAlertFirstButtonReturn` (1000) / Second / Third | same constants | — |
| `+alertWithMessageText:...` | `NSAlert::alertWithMessageText(...)` | — |
| `NSRunAlertPanel` / `NSRunInformationalAlertPanel` | same C functions | `NSAlert` |
| `NSBundle`, `-objectForInfoDictionaryKey:` | `NSBundle::mainBundle()`, `objectForInfoDictionaryKey()` | the bundle's `Contents/Info.json` |
| `NSString` | `QString` | — |

The about panel is the standard macOS 10.6 layout — app icon (128 px), the
app name in bold, an optional one-line description, "*<Name> version x.y
(build N)*" and the copyright in small text — all read from the bundle's
`Info.json` (`CFBundleName`, `CFBundleDisplayName`,
`CFBundleShortVersionString`, `CFBundleVersion`, `NSApplicationDescription`,
`NSHumanReadableCopyright`), so the panel is always in sync with
`App.toml`. The menu item that opens it is conventionally
**About \<AppName\>** — with the system menu bar it lives in the app
menu on the left (`setDefaultApplicationMenu`), as on macOS 10.6.
## The global menu (the system menu bar)

lynxOS has a system-managed menu bar: SystemUIServer draws one bar at
the top of the screen and shows the menus of the **frontmost app** —
apps don't render their own menu bars. AppKit handles the whole
protocol; nothing to do beyond the normal API:

- `NSWindow::setMainMenu(menu)` publishes the window's menus to the
  system bar (besides building the local Qt bar as before).
- The **application menu** — the bold title on the left, next to the
  Apple menu — defaults to *About <App> / sep / Quit <App>*; an app
  replaces it with `app->setApplicationMenu(menu)`. The old convention
  of putting "About" in the File menu is obsolete: it belongs in the
  application menu, 10.6-style.
- Menu titles ("File", …) come from the `NSMenu` titles you pass to
  `setMainMenu` — don't hardcode glyphs or fake title bars; the
  system bar renders them in the 10.6 style.
- Items trigger through their `std::function` actions as usual,
  whether the user clicks them in the system bar or presses their
  keyEquivalent there.
- `app->setDefaultApplicationMenu(menu)` publishes the fallback for
  when **no window is active** — the Finder-clone case: a file manager
  often runs with zero windows, and the bar should still show its
  menus. Pass `nullptr` to clear. Apps that keep running windowless
  should call `QApplication::setQuitOnLastWindowClosed(false)`.

Under the hood this is two X properties plus a ClientMessage:
`_LYNX_GLOBAL_MENU` (a JSON menu model) on each window that has a main
menu, `_LYNX_DEFAULT_MENU` on the root for the default app (stamped
with the publisher's PID so a crashed app's menu dies with it), and
`_LYNX_MENU_ACTION` messages sent back to the publishing window — or
to the root, which publishers select for with StructureNotifyMask —
when the user picks an item. The bar side lives in
`lynxOSX/SystemUIServer` (`Sources/GlobalMenu.cpp`).

## Using it in an AppBuilder project

`appbuilder init` scaffolds a GUI app that vendors `AppKit.h` and
`AppKit.cpp` into `Sources/` — nothing to install, `appbuilder build`
just works:

```
command = ["sh", "-c",
  "g++ -O2 -fPIC -std=c++17 -o {out} Sources/main.cpp Sources/AppKit.cpp "
  "$(pkg-config --cflags --libs Qt6Widgets)"]
```

To use the canonical library instead of the vendored copy:

```
make            # -> libAppKit.a
make install    # -> /usr/local/{lib,include}
```

and link with `-lAppKit` (keep the vendored `Sources/AppKit.h/cpp`
removed from the build command then).

`example/` is a small app showing the window, the File menu with
"About", and an `NSAlert` — build it with AppBuilder.

## Notes

- `-fPIC` is required when linking against Qt 6 (copy relocations).
- `NSWindow::setMainMenu()` appends the menu as one titled menu
  ("File", …) whose items are the dropdown, and publishes the window's
  menus to the system menu bar (see "The global menu" above).
- Qt dialogs replace the NS panel art. Alert icons come from the system
  icon theme, resolved by path (`/usr/share/icons/<theme>/<NxN>/<cat>/…`,
  themes discovered at runtime — never QIcon): the warning style shows
  `dialog-warning`; informational and critical both show the app's bundle
  icon, as in the 10.6 NSAlert (see `guide_PRIVATE/SDK-guides/dialogAlert.png`).
- Alerts and the about panel get a full-size titlebar with only minimize
  and close enabled: the maximize button hint is left out, so Qt writes
  `_MOTIF_WM_HINTS` without `MWM_FUNC_MAXIMIZE` and WindowServerWM draws
  the green bulb disabled.
- **WindowServerWM quirk:** a window that maps already application-modal
  (what `QDialog::exec()` does) loses its first paint — the WM's map path
  for `_NET_WM_STATE_MODAL` windows discards the client's initial paint.
  `NSAlert::runModal()` works around it by mapping the alert before it
  becomes modal. If you call Qt's `exec()` on a dialog of your own,
  `show()` it first.
