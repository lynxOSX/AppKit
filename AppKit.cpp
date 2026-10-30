/* AppKit — lynxOS application framework (implementation).
   See AppKit.h; the naming follows the macOS 10.6 SDK on Qt 6 Widgets. */

#include "AppKit.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QGridLayout>
#include <QGuiApplication>
#include <QScreen>
#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QAbstractNativeEventFilter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>
#include <QtGui/qguiapplication_platform.h>

#include <xcb/xcb.h>

#include <unistd.h>
#include <limits.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

/* ------------------------------------------------------------------ */
/* NSBundle                                                           */

NSBundle *NSBundle::mainBundle()
{
    static NSBundle *b = nullptr;
    if (!b) b = new NSBundle();
    return b;
}

NSBundle::NSBundle()
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return;
    exe[n] = '\0';
    QString exePath = QString::fromLocal8Bit(exe);
    QFileInfo fi(exePath);
    /* a bundle binary lives at <root>/Contents/lynxOS/<exe>; when run
       from a build tree, fall back to the executable's directory */
    QDir dir = fi.dir();                    /* …/Contents/lynxOS */
    if (dir.dirName() == "lynxOS" && dir.cdUp() &&
        dir.dirName() == "Contents" && dir.cdUp())
        bundlePath_ = dir.absolutePath();   /* the bundle root */
    else
        bundlePath_ = fi.absolutePath();
}

QVariant NSBundle::objectForInfoDictionaryKey(const QString &key) const
{
    QFile f(bundlePath_ + "/Contents/Info.json");
    if (!f.open(QIODevice::ReadOnly)) return QVariant();
    QJsonDocument d = QJsonDocument::fromJson(f.readAll());
    return d.object().value(key).toVariant();
}

QString NSBundle::pathForResource(const QString &name,
                                  const QString &ext) const
{
    QString p = bundlePath_ + "/Contents/Resources/" + name;
    if (!ext.isEmpty()) p += "." + ext;
    return QFileInfo::exists(p) ? p : QString();
}

/* ------------------------------------------------------------------ */
/* System icons — the freedesktop layout, resolved by path (never     */
/* through QIcon):                                                    */
/*   /usr/share/icons/<theme>/<size>x<size>/<category>/<name>.png     */
/* The installed themes are discovered from the directory itself, so  */
/* any icon theme on the system works, not just hicolor.              */

static QPixmap systemDialogIcon(const char *name)
{
    static QStringList themes;
    if (themes.isEmpty()) {
        QDir icons("/usr/share/icons");
        themes = icons.entryList(QDir::Dirs | QDir::NoDotAndDotDot,
                                 QDir::Name);
        themes.removeAll("hicolor");
        themes.append("hicolor");   /* the freedesktop fallback, last */
    }
    static const int sizes[] = { 48, 32, 64, 24, 16 };
    static const char *cats[] = { "status", "apps", "actions",
                                  "devices", "places", "mimetypes" };
    for (const QString &theme : themes)
        for (int size : sizes)
            for (const char *cat : cats) {
                const QString p = QString("/usr/share/icons/%1/%2x%2/%3/%4.png")
                                      .arg(theme).arg(size).arg(cat).arg(name);
                if (QFileInfo::exists(p))
                    return QPixmap(p);
            }
    return QPixmap();
}

/* The bundle's app icon (used by alerts and the about panel). */
static QPixmap bundleAppIcon()
{
    NSBundle *b = NSBundle::mainBundle();
    return QPixmap(b->pathForResource(
        b->objectForInfoDictionaryKey("CFBundleIconFile")
            .toString().replace(".png", ""), "png"));
}

/* ------------------------------------------------------------------ */
/* WindowServerWM caption protocol                                    */

void NSSetWindowCompactTitlebar(QWidget *widget, bool compact)
{
    /* the WM reads this property when it first manages the window, so
       it must be set before the window is shown; a private display
       connection keeps AppKit free of Qt platform-plugin internals */
    Display *d = XOpenDisplay(nullptr);
    if (!d || !widget) return;
    const Atom a = XInternAtom(d, "_LYNX_WM_COMPACT_CAPTION", False);
    const unsigned long v = compact ? 1 : 0;
    XChangeProperty(d, widget->winId(), a, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&v, 1);
    XCloseDisplay(d);
}

void NSSetWindowUnifiedToolbar(QWidget *widget, bool unified)
{
    /* The unified look of macOS X's textured windows (Finder, System
       Preferences, ...): the caption's bottom separator is painted with
       the title background's continuing color, so the caption and the
       app's toolbar (drawn right below, starting with the same color)
       read as one gradient. Set before the window is shown; see
       framerender.c in WindowServerWM. */
    Display *d = XOpenDisplay(nullptr);
    if (!d || !widget) return;
    const Atom a = XInternAtom(d, "_LYNX_WM_UNIFIED_TOOLBAR", False);
    const unsigned long v = unified ? 1 : 0;
    XChangeProperty(d, widget->winId(), a, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&v, 1);
    XCloseDisplay(d);
}

/* ------------------------------------------------------------------ */
/* The system menu bar (SystemUIServer)                               */
/*                                                                    */
/* Apps PUBLISH their menus; the bar renders whatever the frontmost   */
/* app published — nothing is hardcoded in the bar. Two channels:     */
/*                                                                    */
/*   _LYNX_GLOBAL_MENU  UTF8_STRING on every app window that has a    */
/*                      main menu; the active window wins.            */
/*   _LYNX_DEFAULT_MENU UTF8_STRING on the root window — the default  */
/*                      app's menus (the Finder-clone fallback), what */
/*                      the bar shows when no window is active.       */
/*                                                                    */
/* Both hold JSON:                                                    */
/*   { "appName": "Finder",                                           */
/*     "appMenu": [ { "id": 1, "title": "About Finder" } ],           */
/*     "menus":  [ { "title": "File", "items": [ ... ] } ] }          */
/* Items: { "id": n, "title": t, "key": "Ctrl+Q", "enabled": false,   */
/*          "submenu": [...] } or { "sep": true }.                    */
/*                                                                    */
/* Activation goes back the same way: SystemUIServer sends a          */
/* ClientMessage of type _LYNX_MENU_ACTION (format 32, data.l[0] =    */
/* the item id) to the window — or to the root for the default app,   */
/* which must XSelectInput(root, StructureNotifyMask) to receive it.  */
/* The dispatch map below triggers the matching QAction.              */

static QHash<int, QAction *> g_menuItemsById;
static int g_nextMenuItemId = 1;

static QString lynxAppName()
{
    QString name = NSApplication::sharedApplication()->applicationName();
    if (name.isEmpty())
        name = QCoreApplication::applicationName();
    if (name.isEmpty())
        name = QFileInfo(QCoreApplication::applicationFilePath())
                   .completeBaseName();
    return name;
}

static QJsonObject lynxSerializeAction(QAction *a)
{
    QJsonObject o;
    const int id = g_nextMenuItemId++;
    g_menuItemsById.insert(id, a);
    o["id"] = id;
    o["title"] = a->text();
    if (!a->shortcut().isEmpty())
        o["key"] = a->shortcut().toString();
    if (!a->isEnabled())
        o["enabled"] = false;
    return o;
}

static QJsonArray lynxSerializeItems(const QList<QAction *> &actions)
{
    QJsonArray items;
    for (QAction *a : actions) {
        if (a->isSeparator()) {
            items.append(QJsonObject{ {"sep", true} });
        } else if (QMenu *sub = a->menu()) {
            QJsonObject o;
            o["title"] = a->text();
            o["submenu"] = lynxSerializeItems(sub->actions());
            items.append(o);
        } else {
            items.append(lynxSerializeAction(a));
        }
    }
    return items;
}

/* The app menu every app gets until it replaces one: About + Quit. */
static NSMenu *lynxDefaultAppMenu()
{
    NSApplication *app = NSApplication::sharedApplication();
    NSMenu *m = new NSMenu(lynxAppName());
    m->addItem(new NSMenuItem(QObject::tr("About %1").arg(lynxAppName()),
        [app] { app->orderFrontStandardAboutPanel(); }));
    m->addSeparator();
    m->addItem(new NSMenuItem(QObject::tr("Quit %1").arg(lynxAppName()),
        [] { QApplication::quit(); }, "Ctrl+Q"));
    return m;
}

/* menus: (title, items) pairs — a window bar's titled menus, or the
   single default-application menu */
static QByteArray
lynxSerializeMenu(const QList<QAction *> &appMenuItems,
                  const QVector<QPair<QString, QList<QAction *>>> &menus)
{
    QJsonObject root;
    root["appName"] = lynxAppName();

    QJsonArray appMenu;
    for (QAction *a : appMenuItems)
        appMenu.append(a->isSeparator() ? QJsonObject{ {"sep", true} }
                                        : lynxSerializeAction(a));
    root["appMenu"] = appMenu;

    QJsonArray jsonMenus;
    for (const auto &menu : menus) {
        QJsonObject m;
        m["title"] = menu.first;
        m["items"] = lynxSerializeItems(menu.second);
        jsonMenus.append(m);
    }
    root["menus"] = jsonMenus;

    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

static Display *lynxXDisplay()
{
    /* a private Xlib connection for the property writes; the atoms are
       server-side, so they match the numbers Qt's xcb events carry */
    return XOpenDisplay(nullptr);
}

static Atom lynxAtom(const char *name)
{
    static QHash<QString, Atom> cache;
    if (!cache.contains(name)) {
        if (Display *d = XOpenDisplay(nullptr)) {
            cache.insert(name, XInternAtom(d, name, False));
            XCloseDisplay(d);
        }
    }
    return cache.value(name);
}

static void lynxPublish(const Atom atom, const Window win,
                        const QByteArray &json)
{
    if (Display *d = lynxXDisplay()) {
        const Atom utf8 = XInternAtom(d, "UTF8_STRING", False);
        XChangeProperty(d, win, atom, utf8, 8, PropModeReplace,
                        (const unsigned char *)json.constData(),
                        json.size());
        XCloseDisplay(d);
    }
}

/* ---- dispatch: SystemUIServer's _LYNX_MENU_ACTION ClientMessages --- */

class LynxMenuActionFilter : public QAbstractNativeEventFilter {
public:
    bool nativeEventFilter(const QByteArray &type, void *message,
                           qintptr *) override
    {
        if (type != "xcb_generic_event_t") return false;
        auto *ev = static_cast<xcb_generic_event_t *>(message);
        if ((ev->response_type & 0x7f) != XCB_CLIENT_MESSAGE) return false;
        auto *cm = reinterpret_cast<xcb_client_message_event_t *>(ev);
        if (cm->format != 32 || cm->type != lynxAtom("_LYNX_MENU_ACTION"))
            return false;
        if (QAction *a = g_menuItemsById.value(cm->data.data32[0])) {
            a->trigger();
            return true;
        }
        return false;
    }
};

/* ------------------------------------------------------------------ */
/* NSApplication                                                      */

NSApplication *NSApplication::shared_ = nullptr;

NSApplication::NSApplication(int &argc, char **argv)
{
    (void)argc;
    (void)argv;
    /* QApplication is created in sharedApplication(). */
}

NSApplication *NSApplication::sharedApplication()
{
    if (!shared_) {
        static int dummy_argc = 1;
        static char dummy_argv0[] = "app";
        static char *dummy_argv[1] = { dummy_argv0 };
        (void)new QApplication(dummy_argc, dummy_argv);
        QApplication::setQuitOnLastWindowClosed(true);
        shared_ = new NSApplication(dummy_argc, dummy_argv);
        static LynxMenuActionFilter menuFilter;
        qApp->installNativeEventFilter(&menuFilter);
        shared_->setApplicationName(
            NSBundle::mainBundle()
                ->objectForInfoDictionaryKey("CFBundleDisplayName")
                .toString());
    }
    return shared_;
}

void NSApplication::run()
{
    QApplication::exec();
}

void NSApplication::setApplicationName(const QString &name)
{
    name_ = name;
    QApplication::setApplicationName(name);
    QApplication::setApplicationDisplayName(name);
}

QString NSApplication::applicationName() const
{
    return name_;
}

void NSApplication::activateIgnoringOtherApps()
{
    /* Qt raises all windows by default; nothing to do on lynxOS. */
}

void NSApplication::setApplicationMenu(NSMenu *menu)
{
    appMenu_ = menu;
}

void NSApplication::setDefaultApplicationMenu(NSMenu *menu)
{
    Display *d = lynxXDisplay();
    if (!d) return;
    const Window root = DefaultRootWindow(d);
    if (!menu) {
        XDeleteProperty(d, root, lynxAtom("_LYNX_DEFAULT_MENU"));
        XCloseDisplay(d);
        return;
    }
    const QByteArray json = lynxSerializeMenu(
        appMenu_ ? appMenu_->qtActions()
                 : lynxDefaultAppMenu()->qtActions(),
        { qMakePair(menu->title(), menu->qtActions()) });
    lynxPublish(lynxAtom("_LYNX_DEFAULT_MENU"), root, json);
    /* stamp our PID so SystemUIServer can drop the fallback when this
       app dies without cleaning up (a crash leaves the property) */
    const unsigned long pid = static_cast<unsigned long>(getpid());
    XChangeProperty(d, root, lynxAtom("_LYNX_DEFAULT_MENU_PID"),
                    XA_CARDINAL, 32, PropModeReplace,
                    (const unsigned char *)&pid, 1);
    /* receive the activation ClientMessages on the root (SystemUIServer
       sends with event_mask = StructureNotifyMask) */
    XSelectInput(d, root, StructureNotifyMask);
    XCloseDisplay(d);
}

void NSApplication::orderFrontStandardAboutPanel()
{
    NSBundle *b = NSBundle::mainBundle();
    const QVariant disp = b->objectForInfoDictionaryKey("CFBundleDisplayName");
    const QVariant name = b->objectForInfoDictionaryKey("CFBundleName");
    const QVariant ver = b->objectForInfoDictionaryKey("CFBundleShortVersionString");
    const QVariant build = b->objectForInfoDictionaryKey("CFBundleVersion");
    const QVariant cr = b->objectForInfoDictionaryKey("NSHumanReadableCopyright");
    const QVariant desc = b->objectForInfoDictionaryKey("NSApplicationDescription");

    QDialog *panel = new QDialog(nullptr,
                                 Qt::Dialog | Qt::WindowTitleHint |
                                 Qt::WindowMinimizeButtonHint |
                                 Qt::WindowCloseButtonHint |
                                 Qt::CustomizeWindowHint);
    panel->setAttribute(Qt::WA_DeleteOnClose);
    panel->setWindowTitle(QObject::tr("About %1").arg(disp.toString()));

    auto *lay = new QVBoxLayout(panel);
    lay->setContentsMargins(20, 24, 20, 20);
    lay->setSpacing(2);

    /* the app icon from the bundle, large like the 10.6 panel */
    const QString iconPath =
        b->pathForResource(b->objectForInfoDictionaryKey("CFBundleIconFile")
                           .toString().replace(".png", ""), "png");
    if (!iconPath.isEmpty()) {
        QLabel *icon = new QLabel(panel);
        icon->setPixmap(QPixmap(iconPath).scaled(
            128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        icon->setAlignment(Qt::AlignCenter);
        lay->addWidget(icon);
        lay->addSpacing(20);
    }

    /* the app name, bold */
    auto *title = new QLabel(disp.isValid() ? disp.toString() : name.toString(),
                             panel);
    QFont bold = title->font();
    bold.setBold(true);
    title->setFont(bold);
    title->setAlignment(Qt::AlignCenter);
    lay->addWidget(title);

    /* an optional one-line description of the app */
    if (desc.isValid() && !desc.toString().isEmpty()) {
        auto *dv = new QLabel(desc.toString(), panel);
        dv->setAlignment(Qt::AlignCenter);
        lay->addWidget(dv);
        lay->addSpacing(10);
    }

    /* "<Name> version x.y (build N)" — the 10.6 wording */
    if (ver.isValid()) {
        QString v = QObject::tr("%1 version %2")
                        .arg(name.isValid() ? name.toString() : disp.toString(),
                             ver.toString());
        if (build.isValid())
            v += QObject::tr(" (build %1)").arg(build.toString());
        auto *lv = new QLabel(v, panel);
        lv->setAlignment(Qt::AlignCenter);
        lay->addWidget(lv);
    }

    /* the copyright, small, after a gap at the bottom */
    if (cr.isValid()) {
        lay->addSpacing(16);
        auto *c = new QLabel(cr.toString(), panel);
        QFont small = c->font();
        small.setPointSizeF(small.pointSizeF() * 0.85);
        c->setFont(small);
        c->setAlignment(Qt::AlignCenter);
        c->setWordWrap(true);
        lay->addWidget(c);
    }

    panel->setFixedSize(panel->sizeHint());
    panel->show();
    panel->raise();
    panel->activateWindow();
}

/* ------------------------------------------------------------------ */
/* NSWindow                                                           */

NSWindow::NSWindow(const QString &title, int w, int h)
{
    win_ = new QMainWindow;
    win_->setWindowTitle(title);
    win_->resize(w, h);
    /* center like -[NSWindow center]-ish default placement */
    const QRect desk = QGuiApplication::primaryScreen()->geometry();
    win_->move(desk.x() + desk.width() / 2 - w / 2,
               desk.y() + desk.height() / 2 - h / 2);
    auto *central = new QWidget(win_);
    auto *lay = new QVBoxLayout(central);
    lay->setContentsMargins(16, 16, 16, 16);
    win_->setCentralWidget(central);
}

NSWindow *NSWindow::windowWithTitle(const QString &title, int w, int h)
{
    return new NSWindow(title, w, h);
}

QWidget *NSWindow::contentView() const
{
    return win_->centralWidget();
}

QLayout *NSWindow::contentLayout() const
{
    return win_->centralWidget()->layout();
}

void NSWindow::setTitle(const QString &title)
{
    win_->setWindowTitle(title);
}

void NSWindow::setMainMenu(NSMenu *menu)
{
    /* like macOS: each NSMenu becomes one titled menu in the bar
       ("File", ...) whose items are the dropdown; the item that opens
       the about panel is conventionally "About <AppName>". */
    QMenuBar *bar = win_->menuBar();
    QMenu *m = new QMenu(menu->title(), win_);
    for (QAction *a : menu->qtActions())
        m->addAction(a);
    /* a popup that maps while the pointer is grabbed (button held on
       the menubar) loses its first paint; force a queued repaint right
       after it shows */
    QObject::connect(m, &QMenu::aboutToShow, m, [m] {
        QTimer::singleShot(0, m, [m] { m->update(); });
    });
    bar->addMenu(m);

    /* the system menu bar shows the frontmost window's menus */
    NSApplication *app = NSApplication::sharedApplication();
    QVector<QPair<QString, QList<QAction *>>> menus;
    for (QAction *a : bar->actions())
        if (a->menu() && !a->isSeparator())
            menus.append(qMakePair(a->text(), a->menu()->actions()));
    NSMenu *appMenu = app->applicationMenu();
    if (!appMenu) appMenu = lynxDefaultAppMenu(); /* the auto About/Quit */
    const QByteArray json = lynxSerializeMenu(appMenu->qtActions(), menus);
    lynxPublish(lynxAtom("_LYNX_GLOBAL_MENU"), win_->winId(), json);
}

void NSWindow::makeKeyAndOrderFront()
{
    win_->show();
    win_->raise();
    win_->activateWindow();
}

void NSWindow::close()
{
    win_->close();
}

/* ------------------------------------------------------------------ */
/* NSMenu / NSMenuItem                                                */

NSMenuItem::NSMenuItem(const QString &title, std::function<void()> action,
                       const QString &keyEquivalent)
    : title_(title), action_(std::move(action)), key_(keyEquivalent),
      submenu_(nullptr)
{
}

void NSMenuItem::setSubmenu(NSMenu *submenu)
{
    submenu_ = submenu;
}

void NSMenuItem::setEnabled(bool enabled)
{
    enabled_ = enabled;
}

NSMenu::NSMenu(const QString &title) : title_(title), bar_(nullptr)
{
}

void NSMenu::addItem(NSMenuItem *item)
{
    if (item->submenu_) {
        QMenu *m = new QMenu(item->title_);
        for (QAction *a : item->submenu_->qtActions())
            m->addAction(a);
        QAction *a = m->menuAction();
        a->setText(item->title_);
        actions_.append(a);
        return;
    }
    QAction *a = new QAction(item->title_);
    if (!item->key_.isEmpty())
        a->setShortcut(QKeySequence(item->key_));
    a->setEnabled(item->enabled_);
    if (item->action_) {
        std::function<void()> fn = item->action_;
        QObject::connect(a, &QAction::triggered, [fn] { fn(); });
    }
    actions_.append(a);
}

void NSMenu::addSeparator()
{
    actions_.append(new QAction(nullptr)); /* filled in by setAsMainMenu */
    actions_.last()->setSeparator(true);
}

void NSMenu::setAsMainMenu()
{
    /* the actions carry into whichever window is frontmost; NSWindow
       applies them through setMainMenu() */
}

/* ------------------------------------------------------------------ */
/* NSAlert                                                            */

NSAlert::NSAlert() : style_(NSInformationalAlertStyle)
{
}

void NSAlert::setMessageText(const QString &message)
{
    message_ = message;
}

void NSAlert::setInformativeText(const QString &informativeText)
{
    info_ = informativeText;
}

void NSAlert::setAlertStyle(NSAlertStyle style)
{
    style_ = style;
}

void NSAlert::addButtonWithTitle(const QString &title)
{
    buttons_.append(title);
}

NSModalResponse NSAlert::runModal()
{
    QMessageBox box;
    /* A full-size titlebar with only minimize + close enabled: leaving
       the maximize hint out makes Qt write _MOTIF_WM_HINTS without
       MWM_FUNC_MAXIMIZE, and WindowServerWM renders the green bulb with
       the disabled art (yellow = minimize, red = close stay enabled). */
    box.setWindowFlags(Qt::Dialog | Qt::WindowTitleHint |
                       Qt::WindowSystemMenuHint |
                       Qt::WindowMinimizeButtonHint |
                       Qt::WindowCloseButtonHint |
                       Qt::CustomizeWindowHint);
    box.setWindowTitle(message_); /* the strip above, like NSAlert */
    box.setText(message_);
    if (!info_.isEmpty())
        box.setInformativeText(info_);
    /* Icon mapping (guide_PRIVATE/SDK-guides/dialogAlert.png, the
       Alert column): warning shows the caution triangle from the
       system icon theme; informational AND critical show the app's
       bundle icon — the 10.6 NSAlert has no dedicated stop icon. */
    auto setIcon = [&box](const QPixmap &icon) {
        if (!icon.isNull())
            box.setIconPixmap(icon.scaled(48, 48, Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation));
    };
    switch (style_) {
    case NSWarningAlertStyle: {
        const QPixmap icon = systemDialogIcon("dialog-warning");
        if (!icon.isNull())
            setIcon(icon);
        else
            setIcon(bundleAppIcon());
        break;
    }
    case NSCriticalAlertStyle:
    case NSInformationalAlertStyle:
    default:
        setIcon(bundleAppIcon());
        break;
    }
    if (buttons_.isEmpty())
        buttons_.append(QObject::tr("OK"));
    for (const QString &t : buttons_)
        box.addButton(t, QMessageBox::AcceptRole);
    /* 10.6 alert panels have a minimum size (the reference chart's ~3:1
       proportions: 420x130) and stick the buttons to the bottom. QMessageBox
       pins the dialog to its content: updateSize() setFixedSize()es it to
       the layout minimum on every layout pass (WM_NORMAL_HINTS min = max =
       sizeHint, so the WM refuses any other size), so growing the CONTENT
       is the only way to grow the panel: a wider message label gives the
       width, and a spacer row under the text gives the height with the
       button row moved below it. */
    if (auto *lbl = box.findChild<QLabel *>("qt_msgbox_label"))
        lbl->setMinimumWidth(370);
    QDialogButtonBox *bb = box.findChild<QDialogButtonBox *>();
    QGridLayout *grid = qobject_cast<QGridLayout *>(box.layout());
    if (bb && grid) {
        for (int i = 0; i < grid->count(); ++i) {
            int r, c, rs, cs;
            if (grid->itemAt(i)->widget() != bb)
                continue;
            grid->getItemPosition(i, &r, &c, &rs, &cs);
            grid->removeWidget(bb);
            QWidget *pad = new QWidget(&box);
            pad->setFixedHeight(30);
            grid->addWidget(pad, r, 0, 1, grid->columnCount());
            grid->addWidget(bb, r + 1, 0, 1, grid->columnCount());
            break;
        }
    }
    /* Map the panel before it becomes modal. A window that maps already
       application-modal loses its first paint under WindowServerWM (the
       WM's map path for _NET_WM_STATE_MODAL windows discards the client's
       initial paint); showing it first paints it while still non-modal,
       then exec() only changes the modality of a mapped window. */
    box.show();
    box.exec();
    /* map the chosen button back to NSAlert..Return constants */
    int idx = 0;
    QList<QAbstractButton *> bs = box.buttons();
    QAbstractButton *clicked = box.clickedButton();
    for (int i = 0; i < bs.size() && i < 3; ++i)
        if (bs[i] == clicked) idx = i;
    return NSAlertFirstButtonReturn + idx;
}

NSAlert *NSAlert::alertWithMessageText(const QString &messageText,
                                       NSAlertStyle style,
                                       const QString &defaultButton,
                                       const QString &alternateButton,
                                       const QString &otherButton,
                                       const QString &informativeText)
{
    NSAlert *a = new NSAlert();
    a->setMessageText(messageText);
    a->setInformativeText(informativeText);
    a->setAlertStyle(style);
    if (!defaultButton.isEmpty()) a->addButtonWithTitle(defaultButton);
    if (!alternateButton.isEmpty()) a->addButtonWithTitle(alternateButton);
    if (!otherButton.isEmpty()) a->addButtonWithTitle(otherButton);
    return a;
}

NSInteger NSRunAlertPanel(const QString &title, const QString &msg,
                          const QString &defaultButton,
                          const QString &alternateButton,
                          const QString &otherButton)
{
    NSAlert *a = NSAlert::alertWithMessageText(
        title, NSWarningAlertStyle,
        defaultButton.isEmpty() ? "OK" : defaultButton,
        alternateButton, otherButton, msg);
    return a->runModal();
}

NSInteger NSRunInformationalAlertPanel(const QString &title,
                                       const QString &msg,
                                       const QString &defaultButton,
                                       const QString &alternateButton,
                                       const QString &otherButton)
{
    NSAlert *a = NSAlert::alertWithMessageText(
        title, NSInformationalAlertStyle,
        defaultButton.isEmpty() ? "OK" : defaultButton,
        alternateButton, otherButton, msg);
    return a->runModal();
}
