// NickelBookends - two items in the reading menu (NickelMenu):
//
//   * Hide book titles (toggle): removes the book title from the reading
//     header and footer ("The Jasad Crown • 45% READ" -> "45% READ").
//   * Physical book progress: shows a toast "Page 302 of 671" - your position
//     in the printed edition, using "Pages" from Book details (StorePages).
//     Nothing is added to the header, footer or toolbar.
//
// Position, most precise first: the toolbar's "Page X of Y" if it is open
// (read only), else Kobo's percentage refined by counted page turns (1% is
// learned per book), else Kobo's percentage.
//
// Safety rules (see README for the history):
//   * No global objects with constructors: NickelHook runs nb_init() from an
//     ELF constructor, before C++ static initializers. State lives in NBState.
//   * Only hooks used by stable mods: NickelClock (readerIsDoneLoading),
//     NickelHardcover (setVolume), NickelDissolve (goToNext/PrevPage).
//   * DB reads happen from a timer, never during a paint, once per book.
//   * Captions are rewritten in the paint in which Kobo changes them (no
//     flicker). Watchdog: too many rewrites -> off until reboot.

#include <QApplication>
#include <QDateTime>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>
#include <QPointer>
#include <QRegExp>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <QWidget>

#include <NickelHook.h>

#include "nb_calc.h"

#ifndef NB_CONFIG_DIR
#define NB_CONFIG_DIR "/mnt/onboard/.adds/nickelbookends"
#endif
#ifndef NB_DB_PATH
#define NB_DB_PATH    "/mnt/onboard/.kobo/KoboReader.sqlite"
#endif
#define NB_DB_CONN    "nickelbookends"
#define NB_SETTINGS   NB_CONFIG_DIR "/settings.ini"
#define NB_CACHE      NB_CONFIG_DIR "/learned.ini"
#define NB_LOG_PATH   NB_CONFIG_DIR "/nickelbookends.log"
#define NB_VERSION    "v11"
#define NB_TRIGGER    "/tmp/nickelbookends_progress" // touched by the NickelMenu item (tmpfs)

typedef QWidget ReadingView;
typedef void ReadingController;
typedef void Volume;
typedef void Bookmark;

static void    (*ReadingController__setVolume)(ReadingController *_this, Volume *volume, Bookmark *bookmark);
static void    (*ReadingView__readerIsDoneLoading)(ReadingView *_this);
static void    (*ReadingView__goToNextPage)(ReadingView *_this);
static void    (*ReadingView__goToPrevPage)(ReadingView *_this);
static QString (*Content__getId)(Volume *_this);
static int     (*ReadingView__getCalculatedReadProgress)(QWidget *_this);
typedef QObject MainWindowController;
static MainWindowController *(*MainWindowController__sharedInstance)();
static void    (*MainWindowController__toast)(MainWindowController *_this, QString const &main, QString const &sub, int ms);

static const char nb_prop_hooked[] = "NickelBookendsHooked";

struct NBState {
    // settings
    bool      enabled = true;
    bool      hide_title = false;
    bool      physical_pages = true;   // "Physical book pages in footer"
    QString   page_format;
    QString   toast_format;
    bool      debug = true;
    int       override_total = 0;
    qint64    settings_mtime = -1;
    // book
    QString   content_id;            // from setVolume (not called on 4.46)
    QString   book_key;
    QString   book_title;
    int       store_pages = -1;      // -1 = not looked up yet
    int       lookups_without_title = 0;
    // position tracking
    int       last_pct = -1;
    bool      step_clean = false;    // current 1% step was entered at its start
    int       turns_in_step = 0;
    double    turns_per_pct = -1;
    bool      tpp_exact = false;     // turns_per_pct came from the toolbar (Kobo's own page count)    // learned, per book
    double    anchor = -1;
    int       turns_since_anchor = 0;
    // ui
    QPointer<QWidget> reading_view;
    QTimer   *timer = nullptr;
    QTimer   *trigger_timer = nullptr;
    int       trigger_checks = 0;
    // watchdog
    bool      tripped = false;
    qint64    window_start = 0;
    int       window_writes = 0;
};

static NBState *nb = nullptr;      // POD pointer, zero before any code runs
static QObject *nb_filter = nullptr;

// ---- log (open, append, close: never keep a handle on /mnt/onboard) ---------

static void nb_log(const QString &msg)
{
    nh_log("%s", qPrintable(msg));
    if (nb && !nb->debug && !msg.startsWith("!"))
        return;
    QFile f(NB_LOG_PATH);
    if (f.size() > 64 * 1024)
        f.remove();
    if (f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        f.write(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss ").toUtf8());
        f.write(msg.toUtf8());
        f.write("\n");
        f.close();
    }
}

// ---- settings ---------------------------------------------------------------

static void nb_load_settings()
{
    QSettings s(NB_SETTINGS, QSettings::IniFormat);
    nb->enabled        = s.value("enabled", true).toBool();
    nb->hide_title     = s.value("hide_title", false).toBool();
    nb->physical_pages = s.value("physical_pages", true).toBool();
    nb->page_format    = s.value("page_format", "{page} OF {total}").toString();
    nb->toast_format   = s.value("toast_format", "Page {page} of {total}").toString();
    nb->debug          = s.value("debug", true).toBool();
    nb->override_total = s.value("override_total", 0).toInt();
    nb->settings_mtime = QFileInfo(NB_SETTINGS).lastModified().toMSecsSinceEpoch();
}

// NickelMenu toggles edit settings.ini: pick that up without reopening the book.
static bool nb_settings_changed()
{
    qint64 m = QFileInfo(NB_SETTINGS).lastModified().toMSecsSinceEpoch();
    if (m == nb->settings_mtime)
        return false;
    nb_load_settings();
    nb_log(QString("settings reloaded: hide_title=%1").arg(nb->hide_title ? "true" : "false"));
    return true;
}

// ---- learned turns per 1% (per book) ----------------------------------------

static QString nb_cache_key()
{
    QString k = nb->book_key;
    return QString::fromLatin1(k.toUtf8().toBase64()); // safe as an ini key
}

static void nb_cache_load()
{
    QSettings s(NB_CACHE, QSettings::IniFormat);
    double v = s.value("turns_per_pct/" + nb_cache_key(), -1).toDouble();
    nb->turns_per_pct = v > 0 ? v : -1;
    nb->tpp_exact = s.value("exact/" + nb_cache_key(), false).toBool();
}

static void nb_cache_save()
{
    if (nb->turns_per_pct <= 0)
        return;
    QSettings s(NB_CACHE, QSettings::IniFormat);
    s.setValue("turns_per_pct/" + nb_cache_key(), nb->turns_per_pct);
    s.setValue("exact/" + nb_cache_key(), nb->tpp_exact);
}

// ---- metadata ---------------------------------------------------------------

enum NBLookup { NBById, NBByTitle, NBByTitlePrefix, NBByLastRead };

static int nb_query(NBLookup how, const QString &key, QString *title)
{
    int pages = -1;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", NB_DB_CONN);
        db.setDatabaseName(NB_DB_PATH);
        db.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=300");
        if (!db.open()) {
            nb_log("! db open failed: " + db.lastError().text());
        } else {
            QSqlQuery q(db);
            const char *base = "SELECT StorePages, Title FROM content WHERE ContentType = 6 ";
            if (how == NBById) {
                q.prepare(QString(base) + "AND ContentID = ? LIMIT 1");
                q.addBindValue(key);
            } else if (how == NBByTitle) {
                q.prepare(QString(base) + "AND Title = ? ORDER BY DateLastRead DESC LIMIT 1");
                q.addBindValue(key);
            } else if (how == NBByTitlePrefix) {
                QString like = key;
                like.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_");
                q.prepare(QString(base) + "AND Title LIKE ? ESCAPE '\\' ORDER BY DateLastRead DESC LIMIT 1");
                q.addBindValue(like + "%");
            } else {
                q.prepare(QString(base) + "ORDER BY DateLastRead DESC LIMIT 1");
            }
            if (!q.exec()) {
                nb_log("! db query failed: " + q.lastError().text());
            } else if (q.next()) {
                pages = q.value(0).toInt();
                *title = q.value(1).toString();
            }
            q.finish();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(NB_DB_CONN);
    return pages;
}

static QLabel *nb_caption(const char *name)
{
    QWidget *rv = nb->reading_view.data();
    QWidget *rf = rv ? rv->findChild<QWidget*>(name) : nullptr;
    return rf ? rf->findChild<QLabel*>("caption") : nullptr;
}

// Kobo's own text of a caption (not ours; ours may be empty = hidden title).
static QString nb_kobo_text(QLabel *l)
{
    return l->text(); // v10: we never change a caption's text
}

static void nb_reset_tracking()
{
    nb->last_pct = -1;
    nb->step_clean = false;
    nb->turns_in_step = 0;
    nb->turns_per_pct = -1;
    nb->tpp_exact = false;
    nb->anchor = -1;
    nb->turns_since_anchor = 0;
}

// Identifies the open book: setVolume id, else the title Kobo shows in the
// header/footer, else (last resort) the most recently read book.
// Returns the page count, 0 if none, -1 if it can't be determined yet.
static int nb_total_pages()
{
    if (nb->override_total > 0)
        return nb->override_total;

    QString title;
    bool elided = false;
    if (nb->content_id.isEmpty()) {
        for (const char *name : {"footer", "header"})
            if (QLabel *c = nb_caption(name))
                if (title.isEmpty())
                    title = np_title_from_caption(nb_kobo_text(c), &elided);
        if (title.isEmpty()) {
            // a header showing only the title (no "•" separator)
            for (const char *name : {"header", "footer"})
                if (QLabel *c = nb_caption(name)) {
                    QString t = nb_kobo_text(c).trimmed();
                    if (title.isEmpty() && !t.isEmpty() && !nb_title_from_caption_is_numeric(t)) {
                        title = t;
                        if (t.endsWith(QString::fromUtf8("\xe2\x80\xa6"))) { title.chop(1); elided = true; }
                    }
                }
        }
    }
    QString key = !nb->content_id.isEmpty() ? "id:" + nb->content_id
                : !title.isEmpty()          ? "title:" + title
                : QString();
    if (key.isEmpty() && !nb->book_key.isEmpty())
        return nb->store_pages; // already identified for this opening: keep it
    if (key.isEmpty()) {
        if (++nb->lookups_without_title < 20)
            return -1;
        key = "lastread";
    }
    if (key == nb->book_key)
        return nb->store_pages;
    if (!nb->book_key.isEmpty() && nb->store_pages >= 0 && key.startsWith("title:") &&
        title.compare(nb->book_title, Qt::CaseInsensitive) != 0) {
        // same opening, different caption text (e.g. a chapter title): only
        // switch if that text really is another book in the library
        QString t2;
        if (nb_query(NBByTitle, title, &t2) < 0)
            return nb->store_pages;
    }

    nb->book_key = key;
    nb_reset_tracking();
    QString dbTitle, how;
    int pages = -1;
    if (!nb->content_id.isEmpty()) {
        pages = nb_query(NBById, nb->content_id, &dbTitle);
        how = "setVolume id";
    }
    if (pages < 0 && !title.isEmpty()) {
        pages = nb_query(elided ? NBByTitlePrefix : NBByTitle, title, &dbTitle);
        if (pages < 0 && !elided)
            pages = nb_query(NBByTitlePrefix, title, &dbTitle);
        how = "title '" + title + "'";
    }
    if (pages < 0) {
        pages = nb_query(NBByLastRead, QString(), &dbTitle);
        how = "last-read fallback (may be the previous book!)";
    }
    nb->book_title = dbTitle.isEmpty() ? title : dbTitle;
    nb->store_pages = pages < 0 ? 0 : pages;
    nb_cache_load();
    nb_log(QString("book '%1' (matched by %2): StorePages=%3, learned turns per 1%: %4")
           .arg(nb->book_title, how).arg(nb->store_pages)
           .arg(nb->turns_per_pct > 0 ? QString::number(nb->turns_per_pct, 'f', 2) : QString("not yet")));
    return nb->store_pages;
}

// ---- position ---------------------------------------------------------------

static double nb_fraction(int pct)
{
    if (pct != nb->last_pct) {
        bool fwd1  = nb->last_pct >= 0 && pct == nb->last_pct + 1;
        bool back1 = nb->last_pct >= 0 && pct == nb->last_pct - 1;
        if (fwd1 && nb->step_clean && nb->turns_in_step > 0 && !nb->tpp_exact) {
            double m = nb->turns_in_step;
            double known = nb->turns_per_pct;
            if (known > 0 && (m < known / 2 || m > known * 2)) {
                // e.g. a jump that happened to land on the next percent: ignore
                nb_log(QString("ignored odd step: %1 turns (learned %2)").arg(m).arg(known, 0, 'f', 2));
            } else {
                nb->turns_per_pct = known > 0 ? (known * 2 + m) / 3 : m;
                nb_cache_save();
                nb_log(QString("learned: 1% = %1 turns").arg(nb->turns_per_pct, 0, 'f', 2));
            }
        }
        nb->step_clean = fwd1;
        nb->turns_in_step = 0;
        if (fwd1) {
            nb->anchor = pct / 100.0;           // exactly at the start of this step
            nb->turns_since_anchor = 0;
        } else if (back1) {
            nb->anchor = (pct + 1) / 100.0 - (nb->turns_per_pct > 0 ? 1.0 / (100 * nb->turns_per_pct) : 0);
            nb->turns_since_anchor = 0;
        } else if (nb->anchor < 0 || qAbs(nb->anchor * 100 - pct) > 1.5) {
            nb->anchor = (pct + 0.5) / 100.0;   // jump: assume the middle of the step
            nb->turns_since_anchor = 0;
        }
        nb->last_pct = pct;
    }
    double f = pct / 100.0;
    if (nb->turns_per_pct > 0 && nb->anchor >= 0)
        f = nb->anchor + nb->turns_since_anchor / (100.0 * nb->turns_per_pct);
    double lo = pct / 100.0, hi = (pct + 1) / 100.0;
    if (f < lo - 0.005) f = lo;
    if (f > hi) f = hi - 0.0001;
    return f;
}

// ---- captions ---------------------------------------------------------------

static void nb_schedule(int ms);

// One tap is NOT one physical page: the position is tracked as a fraction of
// the book (Kobo's percentage + turns / (100 * turns_per_pct)) and only then
// mapped to physical pages, so each tap adds  Pages / (100 * turns_per_pct)
// physical pages (e.g. 671 / (100 * 7) = 0.96 for The Jasad Crown).
static QString nb_build(const QString &orig)
{
    if (!nb->enabled)
        return orig;
    QString item;
    int pct = np_caption_percent(orig);
    if (nb->physical_pages && pct >= 0 && nb->store_pages > 0) {
        double f = nb_fraction(pct);
        int page = np_page_for_fraction(f, nb->store_pages);
        item = np_format(nb->page_format, page, nb->store_pages, f, QString());
    }
    return nb_build_caption(orig, nb->book_title, nb->hide_title,
                            item.isEmpty() ? NBDisplayPercent : NBDisplayBoth, item);
}

// v10: Kobo's caption text is NEVER changed. Changing it (v5-v9) made the
// footer re-layout in the middle of a page turn, and the extra redraw briefly
// showed the previous page again. Instead, when a caption should look
// different, NickelBookends paints the shortened text itself, in the label's
// own paint event, with the label's own font, colour, margins and alignment,
// and tells Qt the paint is done. Nothing is resized or re-laid out.
static bool nb_paint_caption(QLabel *l)
{
    if (!l || !nb || nb->tripped || !nb->enabled || (!nb->hide_title && !nb->physical_pages))
        return false;                          // normal Kobo painting
    QString text = l->text();
    if (text.isEmpty())
        return false;
    QString out = nb_build(text);
    if (out == text)
        return false;
    if (nb->store_pages < 0 && nb->override_total <= 0)
        nb_schedule(200);                      // identify the book outside the paint

    QPainter p(l);
    QStyleOption opt;
    opt.initFrom(l);
    l->style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, l); // background (stylesheet)
    if (!out.isEmpty()) {
        QRect r = l->contentsRect();
        int m = l->margin();
        r.adjust(m, m, -m, -m);
        int flags = QStyle::visualAlignment(l->layoutDirection(), l->alignment());
        p.setFont(l->font());
        l->style()->drawItemText(&p, r, flags, l->palette(), l->isEnabled(), out, l->foregroundRole());
    }
    return true;                               // painted: skip QLabel's own paint
}

class NBFilter : public QObject {
public:
    NBFilter() : QObject(nullptr) {}
protected:
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        if (ev->type() == QEvent::Paint)
            return nb_paint_caption(qobject_cast<QLabel*>(obj));
        return false;
    }
};

// Kobo's current percentage: from a caption if one shows it, else Nickel's own.
static int nb_current_pct()
{
    for (const char *name : {"footer", "header"})
        if (QLabel *c = nb_caption(name)) {
            int p = np_caption_percent(nb_kobo_text(c));
            if (p >= 0)
                return p;
        }
    if (ReadingView__getCalculatedReadProgress && nb->reading_view) {
        int p = ReadingView__getCalculatedReadProgress(nb->reading_view.data());
        if (p >= 0 && p <= 100)
            return p;
    }
    return -1;
}

// The toolbar's "Page X of Y" (Kobo's exact page), if it is open. Read only.
static double nb_toolbar_fraction()
{
    for (QWidget *top : QApplication::topLevelWidgets()) {
        if (!top->isVisible())
            continue;
        for (QLabel *l : top->findChildren<QLabel*>("page")) {
            if (!l->isVisible())
                continue;
            QRegExp re("(\\d+)\\D+(\\d+)");
            if (re.indexIn(l->text()) == -1)
                continue;
            int x = re.cap(1).toInt(), y = re.cap(2).toInt();
            if (y > 0 && x >= 1 && x <= y) {
                // Kobo's total page count at the current font: exactly how
                // many taps the book takes, so 1% = y / 100 taps.
                double tpp = y / 100.0;
                if (!nb->tpp_exact || qAbs(tpp - nb->turns_per_pct) > 0.005) {
                    nb->turns_per_pct = tpp;
                    nb->tpp_exact = true;
                    nb_cache_save();
                    nb_log(QString("toolbar: Kobo page %1 of %2 -> 1% = %3 taps").arg(x).arg(y).arg(tpp, 0, 'f', 2));
                }
                nb->anchor = static_cast<double>(x) / y;
                nb->turns_since_anchor = 0;
                return nb->anchor;
            }
        }
    }
    return -1;
}

static void nb_show_progress()
{
    if (!MainWindowController__sharedInstance || !MainWindowController__toast) {
        nb_log("! toast function not found on this firmware");
        return;
    }
    QString msg, sub;
    int total = nb_total_pages();
    if (total <= 0) {
        msg = "No page count";
        sub = "This book has no \"Pages\" value in Book details";
    } else {
        QString src = "toolbar";
        double f = nb_toolbar_fraction();
        if (f < 0) {
            int pct = nb_current_pct();
            if (pct >= 0) {
                f = nb_fraction(pct);
                src = nb->turns_per_pct > 0 ? "percent + counted turns" : "percent";
            }
        }
        if (f < 0) {
            msg = "Position unknown";
            sub = "Turn a page and try again";
        } else {
            int page = np_page_for_fraction(f, total);
            msg = np_format(nb->toast_format, page, total, f, QString());
            nb_log(QString("progress toast: '%1' (from %2)").arg(msg, src));
        }
    }
    MainWindowController__toast(MainWindowController__sharedInstance(), msg, sub, 3000);
}

// The NickelMenu item touches a file in /tmp (RAM, not the SD card); look for it
// a few times right after a tap.
static void nb_check_trigger()
{
    if (!nb)
        return;
    if (QFile::exists(NB_TRIGGER)) {
        QFile::remove(NB_TRIGGER);
        nb->trigger_checks = 0;
        nb_settings_changed();
        nb_show_progress();
        return;
    }
    if (--nb->trigger_checks > 0)
        nb->trigger_timer->start(400);
}

static void nb_update()
{
    if (!nb || nb->tripped)
        return;
    bool changed_settings = nb_settings_changed();
    QWidget *rv = nb->reading_view.data();
    if (!rv || !rv->isVisible())
        return;
    nb_total_pages(); // DB lookup here, never inside a paint
    nb_toolbar_fraction(); // if the toolbar is open: exact position + taps per 1%
    int pct = nb_current_pct();
    if (pct >= 0)
        nb_fraction(pct); // keeps the per-book "turns per 1%" learning up to date
    if (changed_settings)
        for (const char *name : {"footer", "header"})
            if (QLabel *c = nb_caption(name))
                c->update(); // repaint with/without titles (same size, no re-layout)
}

static void nb_schedule(int ms)
{
    if (nb && nb->timer && !nb->tripped)
        nb->timer->start(ms);
}

// Observes only: refresh shortly after a tap (e.g. a NickelMenu toggle or a
// jump from the table of contents).
class NBAppFilter : public QObject {
public:
    NBAppFilter() : QObject(nullptr) {}
protected:
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        switch (ev->type()) {
        case QEvent::MouseButtonRelease:
        case QEvent::TouchEnd:
            if (nb && nb->reading_view) {
                nb_schedule(600);
                nb->trigger_checks = 4; // ~1.6 s of checks for the NickelMenu item
                nb->trigger_timer->start(300);
            }
            break;
        default:
            break;
        }
        return QObject::eventFilter(obj, ev);
    }
};

// After a page turn the page number may change even when Kobo's own caption
// text doesn't: ask for a repaint (same size, no re-layout). Done right in the
// hook, so it is drawn together with Kobo's own page redraw.
static void nb_refresh_captions()
{
    if (!nb->enabled || !nb->physical_pages || !nb->reading_view)
        return;
    for (const char *name : {"footer", "header"})
        if (QLabel *c = nb_caption(name))
            c->update();
}

// ---- hooks ------------------------------------------------------------------

extern "C" __attribute__((visibility("default")))
void _nb_ReadingController__setVolume(ReadingController *_this, Volume *volume, Bookmark *bookmark)
{
    if (nb && volume && Content__getId)
        nb->content_id = Content__getId(volume);
    ReadingController__setVolume(_this, volume, bookmark);
}

extern "C" __attribute__((visibility("default")))
void _nb_ReadingView__readerIsDoneLoading(ReadingView *_this)
{
    ReadingView__readerIsDoneLoading(_this);
    if (!nb || !_this)
        return;
    nb_load_settings();
    nb->reading_view = _this;
    nb->book_key.clear(); // identify the book again
    nb->store_pages = -1;
    nb->lookups_without_title = 0;
    nb_log("book opened (setVolume id: " + (nb->content_id.isEmpty() ? QString("none") : nb->content_id) + ")");
    for (const char *name : {"footer", "header"}) {
        QLabel *c = nb_caption(name);
        if (c && !c->property(nb_prop_hooked).toBool()) {
            c->setProperty(nb_prop_hooked, true);
            c->installEventFilter(nb_filter);
        }
    }
    static bool app_filter = false; // plain bool: constant-initialised, safe
    if (!app_filter && QCoreApplication::instance()) {
        QCoreApplication::instance()->installEventFilter(new NBAppFilter());
        app_filter = true;
    }
    nb_schedule(1000);
}

extern "C" __attribute__((visibility("default")))
void _nb_ReadingView__goToNextPage(ReadingView *_this)
{
    ReadingView__goToNextPage(_this);
    if (!nb)
        return;
    nb->turns_in_step++;
    nb->turns_since_anchor++;
    nb->reading_view = _this;
    nb_refresh_captions();
    nb_schedule(250);
}

extern "C" __attribute__((visibility("default")))
void _nb_ReadingView__goToPrevPage(ReadingView *_this)
{
    ReadingView__goToPrevPage(_this);
    if (!nb)
        return;
    nb->turns_in_step--;
    nb->turns_since_anchor--;
    nb->step_clean = false; // going back breaks a clean measurement
    nb->reading_view = _this;
    nb_refresh_captions();
    nb_schedule(250);
}

// ---- NickelHook wiring ------------------------------------------------------

static int nb_init()
{
    nb = new NBState();
    nb_filter = new NBFilter();
    nb->timer = new QTimer();
    nb->timer->setSingleShot(true);
    QObject::connect(nb->timer, &QTimer::timeout, &nb_update);
    nb->trigger_timer = new QTimer();
    nb->trigger_timer->setSingleShot(true);
    QObject::connect(nb->trigger_timer, &QTimer::timeout, &nb_check_trigger);
    nb_load_settings();
    nb_log("NickelBookends " NB_VERSION " started");
    // NickelBookends replaces NickelPages; both rewriting the same captions
    // undo each other. Ask NickelPages to remove itself (at the next reboot).
    if (QFile::exists("/usr/local/Kobo/imageformats/libnickelpages.so") &&
        QFile::exists("/mnt/onboard/.adds/nickelpages/uninstall")) {
        QFile::remove("/mnt/onboard/.adds/nickelpages/uninstall");
        nb_log("! NickelPages is still installed: asked it to uninstall itself at the next reboot");
    }
    return 0;
}

static bool nb_uninstall()
{
    nh_delete_file(NB_SETTINGS);
    nh_delete_file(NB_CACHE);
    nh_delete_file(NB_LOG_PATH);
    nh_delete_file("/mnt/onboard/.adds/nm/nickelbookends");
    nh_delete_dir(NB_CONFIG_DIR);
    return true;
}

static struct nh_info NickelBookendsInfo = {
    .name            = "NickelBookends",
    .desc            = "Hide book titles; physical book progress toast",
    .uninstall_flag  = nullptr,
    .uninstall_xflag = NB_CONFIG_DIR "/uninstall",
    .failsafe_delay  = 10,
};

static struct nh_hook NickelBookendsHook[] = {
    { .sym = "_ZN17ReadingController9setVolumeERK6VolumeRK8Bookmark", .sym_new = "_nb_ReadingController__setVolume",
      .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(ReadingController__setVolume),
      .desc = "ReadingController::setVolume (current book)", .optional = true },
    { .sym = "_ZN11ReadingView19readerIsDoneLoadingEv", .sym_new = "_nb_ReadingView__readerIsDoneLoading",
      .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(ReadingView__readerIsDoneLoading),
      .desc = "ReadingView::readerIsDoneLoading" },
    { .sym = "_ZN11ReadingView12goToNextPageEv", .sym_new = "_nb_ReadingView__goToNextPage",
      .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(ReadingView__goToNextPage),
      .desc = "ReadingView::goToNextPage", .optional = true },
    { .sym = "_ZN11ReadingView12goToPrevPageEv", .sym_new = "_nb_ReadingView__goToPrevPage",
      .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(ReadingView__goToPrevPage),
      .desc = "ReadingView::goToPrevPage", .optional = true },
    {0},
};

static struct nh_dlsym NickelBookendsDlsym[] = {
    { .name = "_ZNK7Content5getIdEv", .out = nh_symoutptr(Content__getId), .desc = "Content::getId()", .optional = true },
    { .name = "_ZN11ReadingView25getCalculatedReadProgressEv", .out = nh_symoutptr(ReadingView__getCalculatedReadProgress),
      .desc = "ReadingView::getCalculatedReadProgress()", .optional = true },
    { .name = "_ZN20MainWindowController14sharedInstanceEv", .out = nh_symoutptr(MainWindowController__sharedInstance),
      .desc = "MainWindowController::sharedInstance()", .optional = true },
    { .name = "_ZN20MainWindowController5toastERK7QStringS2_i", .out = nh_symoutptr(MainWindowController__toast),
      .desc = "MainWindowController::toast()", .optional = true },
    {0},
};

NickelHook(
    .init      = &nb_init,
    .info      = &NickelBookendsInfo,
    .hook      = NickelBookendsHook,
    .dlsym     = NickelBookendsDlsym,
    .uninstall = &nb_uninstall,
)
