#include <QRegExp>
#include <QStringList>
#include <cmath>

#include "nb_calc.h"

NPSource np_parse_source(const QString &s)
{
    QString v = s.trimmed().toLower();
    if (v == "pages")    return NPSourcePages;
    if (v == "percent")  return NPSourcePercent;
    if (v == "progress") return NPSourceProgress;
    return NPSourceAuto;
}

static double np_percent(const QString &caption)
{
    // "45%", "45 %", "45 % read" (fr/de use a space before %)
    QRegExp re("(\\d{1,3})\\s*%");
    if (re.indexIn(caption) == -1)
        return -1;
    int p = re.cap(1).toInt();
    if (p < 0 || p > 100)
        return -1;
    return p / 100.0;
}

static double np_pages(const QString &caption)
{
    // "Page 34 of 280", "Page 34 sur 280", "34 / 280", "Seite 34 von 280"...
    // Use the first two numbers; the position must not exceed the total.
    QRegExp re("(\\d+)");
    QStringList nums;
    int pos = 0;
    while ((pos = re.indexIn(caption, pos)) != -1 && nums.size() < 2) {
        nums << re.cap(1);
        pos += re.matchedLength();
    }
    if (nums.size() < 2)
        return -1;
    int cur = nums[0].toInt(), tot = nums[1].toInt();
    if (tot <= 0 || cur < 0 || cur > tot)
        return -1;
    return static_cast<double>(cur) / tot;
}

double np_fraction_from_caption(const QString &caption, NPSource src)
{
    switch (src) {
    case NPSourcePages:    return np_pages(caption);
    case NPSourcePercent:  return np_percent(caption);
    case NPSourceProgress: return -1;
    case NPSourceAuto:
    default: {
        double f = np_percent(caption);
        if (f >= 0)
            return f;
        return np_pages(caption);
    }
    }
}

int np_page_for_fraction(double fraction, int total)
{
    if (total <= 0)
        return 0;
    if (fraction < 0) fraction = 0;
    if (fraction > 1) fraction = 1;
    int page = static_cast<int>(std::floor(fraction * total + 0.5));
    if (page < 1) page = 1;
    if (page > total) page = total;
    return page;
}

static QString np_swap_numbers(const QString &orig, int page, int total)
{
    QRegExp re("(\\d+)");
    int first = re.indexIn(orig);
    if (first == -1)
        return QString();
    int firstLen = re.matchedLength();
    int second = re.indexIn(orig, first + firstLen);
    if (second == -1)
        return QString();
    int secondLen = re.matchedLength();
    QString out = orig;
    out.replace(second, secondLen, QString::number(total)); // back to front keeps offsets valid
    out.replace(first, firstLen, QString::number(page));
    return out;
}

QString np_format(const QString &fmt, int page, int total, double fraction, const QString &orig)
{
    if (fmt.trimmed().isEmpty()) {
        QString swapped = np_swap_numbers(orig, page, total);
        return swapped.isEmpty() ? QString("%1 / %2").arg(page).arg(total) : swapped;
    }
    QString out = fmt;
    out.replace("{page}", QString::number(page));
    out.replace("{total}", QString::number(total));
    out.replace("{left}", QString::number(total - page));
    out.replace("{percent}", QString::number(static_cast<int>(std::floor(fraction * 100 + 0.5))));
    out.replace("{orig}", orig);
    return out;
}

int np_caption_percent(const QString &caption)
{
    QRegExp re("(\\d{1,3})\\s*%");
    if (re.indexIn(caption) == -1)
        return -1;
    int p = re.cap(1).toInt();
    return (p >= 0 && p <= 100) ? p : -1;
}

QString np_insert_before_percent(const QString &caption, const QString &item)
{
    QRegExp re("(\\d{1,3})\\s*%");
    int pos = re.indexIn(caption);
    if (pos == -1)
        return QString();
    // Find the separator right before the percentage segment ("•", "·", "|", "–").
    static const char seps[] = "\xe2\x80\xa2|\xc2\xb7|\\||\xe2\x80\x93";
    QRegExp sepRe(QString::fromUtf8(seps));
    int sepPos = -1, sepLen = 0, from = 0, i;
    while ((i = sepRe.indexIn(caption, from)) != -1 && i < pos) {
        sepPos = i; sepLen = sepRe.matchedLength(); from = i + sepLen;
    }
    if (sepPos == -1) {
        // no separator: "45% read" -> "39 of 320 • 45% read"
        return item + QString::fromUtf8(" \xe2\x80\xa2 ") + caption;
    }
    QString sep = caption.mid(sepPos, sepLen);
    int segStart = sepPos + sepLen;
    while (segStart < caption.size() && caption.at(segStart).isSpace())
        segStart++;
    QString out = caption;
    out.insert(segStart, item + " " + sep + " ");
    return out;
}

QString np_title_from_caption(const QString &caption, bool *elided)
{
    static const char seps[] = "\xe2\x80\xa2|\xc2\xb7|\\||\xe2\x80\x93";
    QRegExp sepRe(QString::fromUtf8(seps));
    int i = sepRe.indexIn(caption);
    if (i <= 0)
        return QString(); // no "title • ..." structure
    QString t = caption.left(i).trimmed();
    bool e = false;
    if (t.endsWith(QString::fromUtf8("\xe2\x80\xa6"))) { t.chop(1); e = true; }
    else if (t.endsWith("...")) { t.chop(3); e = true; }
    t = t.trimmed();
    if (elided) *elided = e;
    // a segment that is only numbers/percent (e.g. "12 of 300") is not a title
    if (t.isEmpty() || QRegExp("^[\\d\\s%/]*$").exactMatch(t))
        return QString();
    return t;
}

bool nb_title_from_caption_is_numeric(const QString &seg)
{
    return QRegExp("^[\\d\\s%/]*$").exactMatch(seg) || QRegExp("(\\d{1,3})\\s*%").indexIn(seg) != -1;
}

NBDisplay nb_parse_display(const QString &s)
{
    QString v = s.trimmed().toLower();
    if (v == "pages")                     return NBDisplayPages;   // Physical book pages
    if (v == "kobo" || v == "percent")    return NBDisplayPercent; // Default Kobo options
    return NBDisplayBoth;                                          // Physical book pages + percent
}

static bool nb_same_title(const QString &seg, const QString &title)
{
    if (title.isEmpty() || seg.isEmpty())
        return false;
    QString s = seg;
    bool elided = false;
    if (s.endsWith(QString::fromUtf8("\xe2\x80\xa6"))) { s.chop(1); elided = true; }
    else if (s.endsWith("...")) { s.chop(3); elided = true; }
    s = s.trimmed();
    if (s.isEmpty())
        return false;
    return elided ? title.startsWith(s, Qt::CaseInsensitive)
                  : s.compare(title, Qt::CaseInsensitive) == 0;
}

QString nb_build_caption(const QString &orig, const QString &book_title, bool hide_title,
                         NBDisplay display, const QString &page_item)
{
    static const char seps[] = "\xe2\x80\xa2|\xc2\xb7|\\||\xe2\x80\x93";
    QRegExp sepRe(QString::fromUtf8(seps));

    // split into segments, remembering Kobo's separator
    QStringList segs;
    QString sep = QString::fromUtf8("\xe2\x80\xa2");
    int from = 0, i;
    bool sawSep = false;
    while ((i = sepRe.indexIn(orig, from)) != -1) {
        if (!sawSep) { sep = orig.mid(i, sepRe.matchedLength()); sawSep = true; }
        segs << orig.mid(from, i - from).trimmed();
        from = i + sepRe.matchedLength();
    }
    segs << orig.mid(from).trimmed();

    int titleIdx = -1, pctIdx = -1;
    QRegExp pctRe("(\\d{1,3})\\s*%");
    for (int k = 0; k < segs.size(); k++)
        if (pctIdx < 0 && pctRe.indexIn(segs[k]) != -1)
            pctIdx = k;
    if (segs.size() >= 2 && pctIdx != 0 && !nb_title_from_caption_is_numeric(segs[0]))
        titleIdx = 0;                                  // "Title • ..."
    else if (segs.size() == 1 && nb_same_title(segs[0], book_title))
        titleIdx = 0;                                  // header showing only the title

    QString item = page_item;
    if (pctIdx >= 0) {
        QString p = segs[pctIdx];
        if (p.toUpper() == p && p.toLower() != p)      // Kobo writes "45% READ"
            item = item.toUpper();
    }

    QStringList out;
    for (int k = 0; k < segs.size(); k++) {
        if (k == titleIdx && hide_title)
            continue;
        if (k == pctIdx && !item.isEmpty()) {
            if (display == NBDisplayBoth)  { out << item << segs[k]; continue; }
            if (display == NBDisplayPages) { out << item; continue; }
        }
        if (!segs[k].isEmpty())
            out << segs[k];
    }
    QString result = out.join(" " + sep + " ");
    // keep Kobo's exact text when nothing changed
    if (out == segs || (!hide_title && (pctIdx < 0 || item.isEmpty() || display == NBDisplayPercent)))
        return orig;
    return result;
}
