// Desktop test for NickelBookends v8: fake Kobo DB + fake reading view, offscreen.
#define NB_CONFIG_DIR NB_TEST_DIR
#include <QString>
static QString nb_db_path_override;
#define NB_DB_PATH nb_db_path_override
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHBoxLayout>
#include <QThread>
#include <QMouseEvent>
#include "../src/nickelbookends.cc"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define CHECKEQ(a, b) do { QString _a = (a), _b = (b); if (_a != _b) { fprintf(stderr, "FAIL %s:%d\n  got:  %s\n  want: %s\n", __FILE__, __LINE__, qPrintable(_a), qPrintable(_b)); fails++; } } while (0)

static void fakeView(ReadingView*) {}
// What the label actually shows on screen, compared with a reference label
// that shows `expect` with Kobo's normal QLabel painting.
static QString cap2page(const QString &pctSeg) {
    int pct = np_caption_percent(pctSeg);
    double f = nb_fraction(pct);
    return QString("%1 OF %2").arg(np_page_for_fraction(f, nb->store_pages)).arg(nb->store_pages) + QString::fromUtf8(" \u2022 ") + pctSeg;
}
static bool shows(QLabel *l, const QString &expect) {
    QImage a = l->grab().toImage();                 // what NickelBookends makes it look like
    QString keep = l->text();
    l->removeEventFilter(nb_filter);                // Kobo's normal painting...
    l->setText(expect);                             // ...of the expected text
    QImage b = l->grab().toImage();
    l->setText(keep);
    l->installEventFilter(nb_filter);
    if (a != b) { a.save("/tmp/nb_got.png"); b.save("/tmp/nb_want.png"); }
    return a == b;
}
static QString g_toast;
static QObject *g_mwc = nullptr;
static MainWindowController *fakeMwc() { return g_mwc; }
static void fakeToast(MainWindowController*, QString const &m, QString const &, int) { g_toast = m; }
static int g_progress = -1;
static int fakeProgress(QWidget*) { return g_progress; }
static void tapMenuItem() { QFile f(NB_TRIGGER); f.open(QIODevice::WriteOnly); f.close(); }
static void tap(QWidget *w) { QMouseEvent e(QEvent::MouseButtonRelease, QPointF(1,1), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(w, &e); }
static void wait(int ms) { QElapsedTimer t; t.start(); while (t.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents, 20); }
// what the NickelMenu toggle does: sed on settings.ini
static void toggle(const char *key) {
    QThread::msleep(1100); // mtime resolution
    QFile f(NB_TEST_DIR "/settings.ini"); f.open(QIODevice::ReadOnly); QString t = QString::fromUtf8(f.readAll()); f.close();
    QString on = QString(key) + "=true", off = QString(key) + "=false";
    if (t.contains(on)) t.replace(on, off); else t.replace(off, on);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate); f.write(t.toUtf8()); f.close();
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QDir().mkpath(NB_TEST_DIR);
    const QString B = QString::fromUtf8(" • ");

    QString db = QDir::temp().filePath("nb_test.sqlite");
    QFile::remove(db);
    {
        QSqlDatabase d = QSqlDatabase::addDatabase("QSQLITE", "mk");
        d.setDatabaseName(db); d.open();
        QSqlQuery q(d);
        q.exec("CREATE TABLE content (ContentID TEXT, ContentType INT, StorePages INT, DateLastRead TEXT, Title TEXT)");
        q.exec("INSERT INTO content VALUES ('jasad', 6, 671, '2026-10-17T10:00:00Z', 'The Jasad Crown')");
        q.exec("INSERT INTO content VALUES ('alch', 6, 1255, '2026-10-18T12:00:00Z', 'Alchemised')");
        q.exec("INSERT INTO content VALUES ('nopages', 6, NULL, '2026-10-01T12:00:00Z', 'No Pages Book')");
        d.close();
    }
    QSqlDatabase::removeDatabase("mk");
    nb_db_path_override = db;
    ReadingView__readerIsDoneLoading = fakeView;
    ReadingView__goToNextPage = fakeView;
    ReadingView__goToPrevPage = fakeView;
    QFile::remove(NB_TEST_DIR "/learned.ini");
    QFile::copy("../res/settings.ini", NB_TEST_DIR "/settings.ini.new");
    QFile::remove(NB_TEST_DIR "/settings.ini");
    QFile::rename(NB_TEST_DIR "/settings.ini.new", NB_TEST_DIR "/settings.ini"); // the shipped defaults
    nb_init();

    g_mwc = new QObject();
    MainWindowController__sharedInstance = fakeMwc;
    MainWindowController__toast = fakeToast;
    ReadingView__getCalculatedReadProgress = fakeProgress;

    QWidget rv;
    QWidget *footer = new QWidget(&rv); footer->setObjectName("footer");
    QLabel *cap = new QLabel(footer); cap->setObjectName("caption"); (new QHBoxLayout(footer))->addWidget(cap);
    QWidget *header = new QWidget(&rv); header->setObjectName("header");
    QLabel *hcap = new QLabel(header); hcap->setObjectName("caption"); (new QHBoxLayout(header))->addWidget(hcap);
    rv.resize(600, 200); rv.show();
    auto kobo = [&](const QString &t) { cap->setText(t); cap->repaint(); wait(30); };
    auto turn = [&](const QString &t) { _nb_ReadingView__goToNextPage(&rv); kobo(t); wait(320); };
    auto progress = [&]() { g_toast.clear(); tapMenuItem(); tap(&rv); wait(900); return g_toast; };

    hcap->setText("The Jasad Crown");
    cap->setText("The Jasad Crown" + B + "45% READ");
    _nb_ReadingView__readerIsDoneLoading(&rv);
    wait(1300);
    CHECK(nb->store_pages == 671);                                       // by title, not Alchemised
    CHECKEQ(cap->text(), "The Jasad Crown" + B + "45% READ");            // Kobo's text never changes
    CHECKEQ(hcap->text(), "The Jasad Crown");
    cap->setAlignment(Qt::AlignCenter); hcap->setAlignment(Qt::AlignCenter);
    footer->setGeometry(0, 150, 600, 40); header->setGeometry(0, 0, 600, 40); wait(50);
    CHECK(shows(cap, "The Jasad Crown" + B + "302 OF 671" + B + "45% READ")); // drawn with pages

    // Physical book progress: toast
    CHECKEQ(progress(), "Page 302 of 671");                              // 45% of 671
    CHECKEQ(cap->text(), "The Jasad Crown" + B + "45% READ");
    CHECK(!QFile::exists(NB_TRIGGER));

    // learning turns per 1% (footer shows the percentage)
    turn("The Jasad Crown" + B + "46% READ");
    for (int i = 0; i < 6; i++) turn("The Jasad Crown" + B + "46% READ");
    turn("The Jasad Crown" + B + "47% READ");
    CHECK(qAbs(nb->turns_per_pct - 7) < 0.01);
    turn("The Jasad Crown" + B + "47% READ");
    turn("The Jasad Crown" + B + "47% READ");
    CHECKEQ(progress(), "Page 317 of 671");                              // 47% + 2/7 of a step = 47.29% -> 317
    CHECKEQ(cap->text(), "The Jasad Crown" + B + "47% READ");

    // toolbar open: Kobo's exact page (read only)
    QWidget menu; QLabel *pg = new QLabel(&menu); pg->setObjectName("page"); pg->setText("Page 350 of 700");
    menu.resize(400, 100); menu.show(); wait(30);
    CHECKEQ(progress(), "Page 336 of 671");
    CHECKEQ(pg->text(), "Page 350 of 700");
    menu.hide();

    // Hide book titles toggle (NickelMenu edits settings.ini)
    CHECK(cap->width() > 300);
    toggle("hide_title");
    _nb_ReadingView__goToNextPage(&rv); wait(400);
    CHECKEQ(cap->text(), "The Jasad Crown" + B + "47% READ");            // Kobo's text is never changed (v10)
    CHECK(shows(cap, cap2page("47% READ")));                             // ...but this is what's drawn
    CHECK(shows(hcap, ""));                                              // header with only the title: blank
    kobo("The Jasad Crown" + B + "48% READ");
    CHECK(shows(cap, cap2page("48% READ")));
    CHECK(progress().endsWith("of 671"));                                // still finds the book
    toggle("hide_title");
    _nb_ReadingView__goToNextPage(&rv); wait(400);
    CHECK(shows(cap, "The Jasad Crown" + B + cap2page("48% READ")));
    CHECK(shows(hcap, "The Jasad Crown"));

    // ---- per-tap arithmetic: physical pages per tap = Pages / (100 * taps per 1%) ----
    {
        // toolbar says Kobo page 350 of 700 -> 1% = 7 taps (exact)
        QWidget menu2; QLabel *pg2 = new QLabel(&menu2); pg2->setObjectName("page"); pg2->setText("Page 350 of 700");
        menu2.resize(400, 100); menu2.show(); wait(30);
        tap(&rv); wait(700);                                             // toolbar read ~600 ms after a tap
        CHECK(nb->tpp_exact && qAbs(nb->turns_per_pct - 7.0) < 1e-9);
        menu2.hide(); wait(30);
        // walk 14 Kobo pages from Kobo page 350: fraction must be exactly (350 + n) / 700
        bool ok = true;
        for (int n = 1; n <= 14; n++) {
            int koboPage = 350 + n;
            int pct = koboPage * 100 / 700;                              // Kobo shows whole percents
            _nb_ReadingView__goToNextPage(&rv);
            kobo("The Jasad Crown" + B + QString("%1% READ").arg(pct));
            double f = nb_fraction(pct);
            double want = static_cast<double>(koboPage) / 700;
            if (qAbs(f - want) > 1e-6) { ok = false; fprintf(stderr, "  n=%d f=%.5f want=%.5f\n", n, f, want); }
        }
        CHECK(ok);
        // 14 taps * 671/700 = 13.42 physical pages
        fprintf(stderr, "physical pages per tap: %.3f (671 / (100 * 7))\n", 671.0 / 700);
        wait(400);
    }

    // footer without a percentage: Nickel's own progress is used
    kobo("The Jasad Crown");
    g_progress = 60;
    nb->turns_per_pct = -1;
    QString t = progress();
    CHECK(t == "Page 403 of 671" || t == "Page 406 of 671");             // 60% (or mid-step after a jump)
    fprintf(stderr, "no-percent footer toast: %s\n", qPrintable(t));

    // book without a page count
    hcap->setText("No Pages Book"); cap->setText("No Pages Book" + B + "10% READ");
    _nb_ReadingView__readerIsDoneLoading(&rv); wait(1300);
    CHECKEQ(progress(), "No page count");

    fprintf(stderr, fails ? "%d FAILED\n" : "all tests passed\n", fails);
    return fails != 0;
}
