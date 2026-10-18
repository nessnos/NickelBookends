// Desktop test for NickelBookends v12: fake Kobo DB + fake reading view, offscreen.
#define NB_CONFIG_DIR NB_TEST_DIR
#include <QString>
static QString nb_db_path_override;
#define NB_DB_PATH nb_db_path_override
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QThread>
#include "../src/nickelbookends.cc"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define CHECKEQ(a, b) do { QString _a = (a), _b = (b); if (_a != _b) { fprintf(stderr, "FAIL %s:%d\n  got:  %s\n  want: %s\n", __FILE__, __LINE__, qPrintable(_a), qPrintable(_b)); fails++; } } while (0)

static void fakeView(ReadingView*) {}
static void wait(int ms) { QElapsedTimer t; t.start(); while (t.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents, 20); }
static void toggle(const char *key) {   // what the NickelMenu toggle does (sed on settings.ini)
    QThread::msleep(1100);              // mtime resolution
    QFile f(NB_TEST_DIR "/settings.ini"); f.open(QIODevice::ReadOnly); QString t = QString::fromUtf8(f.readAll()); f.close();
    QString on = QString(key) + "=true", off = QString(key) + "=false";
    if (t.contains(on)) t.replace(on, off); else t.replace(off, on);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate); f.write(t.toUtf8()); f.close();
}
static void tapAt(QWidget *w, QPoint p) {
    QMouseEvent e(QEvent::MouseButtonRelease, QPointF(p), QPointF(w->mapToGlobal(p)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}
// What the label shows on screen vs. Kobo's normal painting of `expect`.
static bool shows(QLabel *l, const QString &expect) {
    QImage a = l->grab().toImage();
    QString keep = l->text();
    l->removeEventFilter(nb_filter);
    l->setText(expect);
    QImage b = l->grab().toImage();
    l->setText(keep);
    l->installEventFilter(nb_filter);
    return a == b;
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
    QFile::remove(NB_TEST_DIR "/settings.ini");
    QFile::copy("../res/settings.ini", NB_TEST_DIR "/settings.ini");   // shipped defaults
    nb_init();
    CHECK(!nb->debug);                                                  // debug off by default

    QWidget rv;
    QWidget *footer = new QWidget(&rv); footer->setObjectName("footer");
    QLabel *cap = new QLabel(footer); cap->setObjectName("caption"); (new QHBoxLayout(footer))->addWidget(cap);
    QWidget *header = new QWidget(&rv); header->setObjectName("header");
    QLabel *hcap = new QLabel(header); hcap->setObjectName("caption"); (new QHBoxLayout(header))->addWidget(hcap);
    rv.resize(600, 800); rv.show();
    footer->setGeometry(0, 760, 600, 40); header->setGeometry(0, 0, 600, 40);
    cap->setAlignment(Qt::AlignCenter); hcap->setAlignment(Qt::AlignCenter);
    auto kobo = [&](const QString &t) { cap->setText(t); cap->repaint(); wait(30); };
    auto page = [&](const QString &pctSeg) {
        double f = nb_fraction(np_caption_percent(pctSeg));
        return QString("%1 OF %2").arg(np_page_for_fraction(f, nb->store_pages)).arg(nb->store_pages) + B + pctSeg;
    };

    hcap->setText("The Jasad Crown");
    cap->setText("The Jasad Crown" + B + "45% READ");
    _nb_ReadingView__readerIsDoneLoading(&rv);
    wait(1300);
    CHECK(nb->store_pages == 671);                                       // by title, not Alchemised
    CHECKEQ(cap->text(), "The Jasad Crown" + B + "45% READ");            // Kobo's text never changes
    CHECK(shows(cap, "The Jasad Crown" + B + "302 OF 671" + B + "45% READ"));

    // toolbar is only looked at after a tap in the MIDDLE of the screen
    QWidget menu; QLabel *pg = new QLabel(&menu); pg->setObjectName("page"); pg->setText("Page 350 of 700");
    menu.resize(400, 100); menu.show(); wait(30);
    tapAt(&rv, QPoint(580, 400)); wait(800);                             // right edge = page turn area
    CHECK(!nb->tpp_exact);
    tapAt(&rv, QPoint(300, 400)); wait(800);                             // middle = opens the toolbar
    CHECK(nb->tpp_exact && qAbs(nb->turns_per_pct - 7.0) < 1e-9);
    CHECKEQ(pg->text(), "Page 350 of 700");                              // toolbar untouched
    menu.hide(); wait(30);

    // each tap = one Kobo page = 671/700 physical pages
    bool ok = true;
    for (int n = 1; n <= 14; n++) {
        int koboPage = 350 + n, pct = koboPage * 100 / 700;
        _nb_ReadingView__goToNextPage(&rv);
        kobo("The Jasad Crown" + B + QString("%1% READ").arg(pct));
        if (qAbs(nb_fraction(pct) - koboPage / 700.0) > 1e-6) ok = false;
    }
    CHECK(ok);
    wait(400);
    CHECK(shows(cap, "The Jasad Crown" + B + page("52% READ")));

    // Hide book titles
    toggle("hide_title");
    _nb_ReadingView__goToNextPage(&rv); wait(400);
    CHECKEQ(cap->text(), "The Jasad Crown" + B + "52% READ");            // still Kobo's text
    CHECK(shows(cap, page("52% READ")));
    CHECK(shows(hcap, ""));
    // Physical book pages off -> only the title is hidden
    toggle("physical_pages");
    _nb_ReadingView__goToNextPage(&rv); wait(400);
    CHECK(shows(cap, "52% READ"));
    // both off -> exactly Kobo
    toggle("hide_title");
    _nb_ReadingView__goToNextPage(&rv); wait(400);
    CHECK(shows(cap, "The Jasad Crown" + B + "52% READ"));
    CHECK(shows(hcap, "The Jasad Crown"));
    toggle("physical_pages");

    // book without a page count: Kobo's footer
    hcap->setText("No Pages Book"); cap->setText("No Pages Book" + B + "10% READ");
    _nb_ReadingView__readerIsDoneLoading(&rv); wait(1300);
    CHECK(shows(cap, "No Pages Book" + B + "10% READ"));

    CHECK(!QFile::exists(NB_TEST_DIR "/nickelbookends.log"));          // nothing written with debug off
    fprintf(stderr, fails ? "%d FAILED\n" : "all tests passed\n", fails);
    return fails != 0;
}
