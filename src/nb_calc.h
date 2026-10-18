// Pure (Qt-only, no libnickel) logic for NickelPages, kept separate so it can
// be unit tested on a desktop machine.
#ifndef NB_CALC_H
#define NB_CALC_H

#include <QString>

enum NPSource {
    NPSourceAuto,     // percentage if the caption has one, else "X of Y", else ReadingView progress
    NPSourcePages,    // caption must contain "X ... Y" (Kobo's own book page numbering)
    NPSourcePercent,  // caption must contain "NN%"
    NPSourceProgress, // ignore the caption, use ReadingView::getCalculatedReadProgress()
};

NPSource np_parse_source(const QString &s);

// Extracts the reading position (0.0 - 1.0) from a header/footer caption.
// Returns a negative value if nothing usable was found.
double np_fraction_from_caption(const QString &caption, NPSource src);

// Maps a reading position onto the metadata page count (1..total).
int np_page_for_fraction(double fraction, int total);

// Expands {page}, {total}, {percent}, {left} and {orig} in fmt.
// An empty fmt keeps Kobo's own wording (any language) and only swaps the
// first two numbers: "Page 34 sur 280" -> "Page 39 sur 320".
QString np_format(const QString &fmt, int page, int total, double fraction, const QString &orig);

// Inserts `item` as its own segment just before the percentage segment of a
// Kobo caption: "Title • 45% read" -> "Title • 39 of 320 • 45% read".
// Returns an empty string if the caption has no percentage.
QString np_insert_before_percent(const QString &caption, const QString &item);

// Percentage (0-100) in a caption, or -1.
int np_caption_percent(const QString &caption);

// The book title in a Kobo caption: the text before the first separator
// ("The Jasad Crown • 45% read" -> "The Jasad Crown"). Sets *elided if Kobo
// shortened it with "…". Empty if the caption has no title segment.
QString np_title_from_caption(const QString &caption, bool *elided);

enum NBDisplay {
    NBDisplayBoth,     // "Title • 302 OF 671 • 45% READ"
    NBDisplayPages,    // "Title • 302 OF 671"
    NBDisplayPercent,  // Kobo's default: "Title • 45% READ"
};
NBDisplay nb_parse_display(const QString &s);
bool nb_title_from_caption_is_numeric(const QString &seg);

// Rebuilds a Kobo caption from its segments (split on "•" etc.):
//   * the book title segment is dropped if hide_title (book_title is the known
//     title; a caption consisting only of it becomes empty)
//   * the percentage segment gets the page item before it / replaced by it
// page_item is upper-cased when Kobo's own percentage text is upper case.
// Returns the caption unchanged if there is nothing to do.
QString nb_build_caption(const QString &orig, const QString &book_title, bool hide_title,
                         NBDisplay display, const QString &page_item);

#endif
