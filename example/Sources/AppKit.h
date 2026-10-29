/* AppKit — lynxOS application framework.
   Qt 6 Widgets with the naming conventions of the macOS 10.6 SDK, so
   lynxOS apps read like AppKit code: NSApplication, NSWindow, NSMenu,
   NSAlert, NSBundle... NSString maps to QString. */

#ifndef APPKIT_H
#define APPKIT_H

#include <QString>
#include <QWidget>
#include <QMainWindow>
#include <QLayout>
#include <functional>

typedef long NSInteger;
typedef NSInteger NSModalResponse;

/* ------------------------------------------------------------------ */
/* NSBundle — the app bundle on disk: Info.json, Resources/.          */

class NSBundle {
public:
    /*! The bundle of the running executable, found through
        /proc/self/exe (Contents/lynxOS/<exe> -> the bundle root). */
    static NSBundle *mainBundle();

    /*! Contents/Info.json values, e.g. key "CFBundleName". */
    QVariant objectForInfoDictionaryKey(const QString &key) const;

    /*! Contents/Resources/<name>.<ext>, or a null QString if absent. */
    QString pathForResource(const QString &name, const QString &ext) const;

    /*! The bundle root (the directory containing Contents/). */
    QString bundlePath() const { return bundlePath_; }

private:
    NSBundle();
    QString bundlePath_;
};

/* ------------------------------------------------------------------ */
/* NSApplication — the app object (wraps QApplication).               */

class NSApplication {
public:
    static NSApplication *sharedApplication();

    /*! The event loop; returns when all windows are closed (or quit). */
    void run();

    void setApplicationName(const QString &name);
    QString applicationName() const;

    /*! Displays the standard about panel — "About <AppName>": the app
        icon from the bundle's Resources/, the display name, the version
        (CFBundleShortVersionString / CFBundleVersion) and
        NSHumanReadableCopyright, all read from the bundle's Info.json. */
    void orderFrontStandardAboutPanel();

    void activateIgnoringOtherApps();

    /* ---- the system menu bar (SystemUIServer) ---- */

    /*! The app menu shown bold at the left of the bar ("About <App>",
        "Quit <App>"). AppKit builds that one automatically; replace it
        with your own (10.6's NSApplication setApplicationMenu:). It is
        published to the system menu bar with the window's menus. */
    void setApplicationMenu(class NSMenu *menu);
    class NSMenu *applicationMenu() const { return appMenu_; }

    /*! Make this app the menu bar's DEFAULT — what the bar shows when
        no window is active, like macOS falls back to Finder. Pass a
        menu for an extra titled menu (File, ...) or nullptr to just
        publish the app menu; call with `nullptr` again to withdraw
        (NSMenu::setDefaultApplicationMenu(nullptr) clears). Windowless
        apps (file managers) publish this way instead. */
    void setDefaultApplicationMenu(class NSMenu *menu = nullptr);

private:
    NSApplication(int &argc, char **argv);
    static NSApplication *shared_;
    QString name_;
    class NSMenu *appMenu_ = nullptr;
};

/* ------------------------------------------------------------------ */
/* NSWindow — a framed window with a menu bar (wraps QMainWindow).    */

class NSWindow {
public:
    /*! A window titled `title`, `w` x `h` points, centered. */
    static NSWindow *windowWithTitle(const QString &title, int w, int h);

    /*! The content view: add your widgets to its layout. */
    QWidget *contentView() const;
    QLayout *contentLayout() const;

    void setTitle(const QString &title);
    void setMainMenu(class NSMenu *menu);
    void makeKeyAndOrderFront();
    void close();

    /* the underlying QMainWindow, for anything NSWindow doesn't wrap */
    class QMainWindow *qtWindow() const { return win_; }

private:
    NSWindow(const QString &title, int w, int h);
    class QMainWindow *win_;
};

/* ------------------------------------------------------------------ */
/* NSMenu / NSMenuItem — the menu bar.                                */

class NSMenu;

class NSMenuItem {
public:
    /*! action: the block to run when chosen (selector + target folded
        into a std::function); keyEquivalent: e.g. "Ctrl+Q" ("" for none). */
    NSMenuItem(const QString &title,
               std::function<void()> action = nullptr,
               const QString &keyEquivalent = QString());

    void setSubmenu(NSMenu *submenu);
    NSMenu *submenu() const { return submenu_; }
    QString title() const { return title_; }

    /*! Greyed-out items publish as disabled and don't trigger. */
    void setEnabled(bool enabled);
    bool isEnabled() const { return enabled_; }

private:
    friend class NSMenu;
    QString title_;
    std::function<void()> action_;
    QString key_;
    NSMenu *submenu_;
    bool enabled_ = true;
};

class NSMenu {
public:
    explicit NSMenu(const QString &title);

    void addItem(NSMenuItem *item);
    void addSeparator();

    /*! The menu's title (e.g. "File" when set as a main menu). */
    QString title() const { return title_; }

    /*! Install this menu as the application's main menu (the window's
        menu bar). */
    void setAsMainMenu();

    const QVector<QAction *> &qtActions() const { return actions_; }

private:
    QString title_;
    QVector<QAction *> actions_;
    class QMenuBar *bar_;
};

/* ------------------------------------------------------------------ */
/* WindowServerWM caption protocol                                    */

/*! Asks WindowServerWM for the compact caption (macOS X's small 14 px
    titlebar) on `widget`'s window. Windows get the full caption by
    default — even dialogs — so this is only needed for palettes and
    other small utility windows. Sets the _LYNX_WM_COMPACT_CAPTION
    property; call BEFORE the window is shown (the WM reads it when it
    first manages the window). */
void NSSetWindowCompactTitlebar(QWidget *widget, bool compact = true);

/* ------------------------------------------------------------------ */
/* NSAlert — a modal alert (10.6 SDK naming).                         */

enum NSAlertStyle {
    NSInformationalAlertStyle = 0,  /* default: app icon, one button   */
    NSWarningAlertStyle       = 1,  /* caution icon                    */
    NSCriticalAlertStyle      = 2   /* critical: stop icon             */
};

/* NSModalResponse values returned by runModal. */
enum {
    NSAlertFirstButtonReturn  = 1000,
    NSAlertSecondButtonReturn = 1001,
    NSAlertThirdButtonReturn  = 1002
};

class NSAlert {
public:
    NSAlert();

    /*! Bold headline, e.g. "Delete this file?" */
    void setMessageText(const QString &message);
    /*! Explanatory body text under the headline. */
    void setInformativeText(const QString &informativeText);
    void setAlertStyle(NSAlertStyle style);

    /*! Buttons appear in order; the first is the default (Return),
        the second the alternate (Escape). Returns the NSAlert...Return
        constant for the button clicked. */
    void addButtonWithTitle(const QString &title);

    NSModalResponse runModal();

    /*! Convenience, 10.6 style: build an alert with its buttons in one
        call. Pass empty strings for no alternate/other button. */
    static NSAlert *alertWithMessageText(const QString &messageText,
                                         NSAlertStyle style,
                                         const QString &defaultButton,
                                         const QString &alternateButton,
                                         const QString &otherButton,
                                         const QString &informativeText);

private:
    QString message_;
    QString info_;
    NSAlertStyle style_;
    QStringList buttons_;
};

/* Legacy C-style panel functions (as in the 10.6 SDK; AppKit kept them
   through 10.6). Return NSAlertFirstButtonReturn (default), second, or
   third; 0 if the panel was cancelled without buttons. */
NSInteger NSRunAlertPanel(const QString &title, const QString &msg,
                          const QString &defaultButton,
                          const QString &alternateButton,
                          const QString &otherButton);
NSInteger NSRunInformationalAlertPanel(const QString &title,
                                       const QString &msg,
                                       const QString &defaultButton,
                                       const QString &alternateButton,
                                       const QString &otherButton);

#endif /* APPKIT_H */
