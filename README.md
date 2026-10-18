# NickelBookends

A [NickelHook](https://github.com/pgaskin/NickelHook) mod for Kobo eReaders with three items in the reading menu (the **⋯** menu in the top toolbar while reading, via [NickelMenu](https://pgaskin.net/NickelMenu/)):

- **Physical book progress.** A toast like **"Page 302 of 671"**.
- **Physical book pages in footer** (toggle). `The Jasad Crown • 302 OF 671 • 45% READ`
- **Hide book titles** (toggle). `302 OF 671 • 45% READ`

The page count is the "Pages" value from **Book details** (`content.StorePages`). Kobo's own caption text is never changed: NickelBookends draws its version in the caption's own paint event, so nothing is resized or re-laid out.

## How the physical page is calculated

One tap is **not** one physical page. NickelBookends tracks your position as a fraction of the book and only then converts it to physical pages:

    position = Kobo's percentage + taps since the last anchor / (100 × taps per 1%)
    physical page = round(position × Pages)

So each tap adds **Pages ÷ (100 × taps per 1%)** physical pages. For *The Jasad Crown*, that's 671 ÷ (100 × 7) ≈ 0.96, so the number usually goes up by one and sometimes stays the same.

Taps per 1% is learned per book and saved in `learned.ini`:
- **Exactly**, whenever the toolbar is open: its "Page X of Y" gives Kobo's page count at your current font, so 1% = Y ÷ 100 taps, and X ÷ Y is an exact anchor.
- **Otherwise by counting** the taps in one full 1% step.

Until either has happened for a book, the page follows Kobo's percentage in 1% steps. Each change of Kobo's percentage re-anchors the position, so errors can't build up.

The book is identified by the title Kobo shows, because `setVolume` isn't called on firmware 4.46 and the last-read date still points to the previous book.

## Settings (`.adds/nickelbookends/settings.ini`)

| key | default | meaning |
|---|---|---|
| `physical_pages` | `true` | toggled by "Physical book pages in footer" |
| `hide_title` | `false` | toggled by "Hide book titles" |
| `page_format` | `{page} OF {total}` | footer page text |
| `toast_format` | `Page {page} of {total}` | Tokens: `{page}` `{total}` `{left}` `{percent}` |
| `enabled` | `true` | master switch |
| `debug` | `true` | detailed `nickelbookends.log` |
| `override_total` | `0` | force a page count (testing) |

## Installation
 
1. Download **`KoboRoot.tgz`** from the [**Releases**](../../releases/latest) page.
2. Connect your Kobo to your computer.
3. Copy `KoboRoot.tgz` into the hidden **`.kobo`** folder on the Kobo.
   - On a Mac, press **Cmd + Shift + .** in Finder to show hidden folders.
   - On Windows, enable *Hidden items* in File Explorer's **View** menu.
4. **Eject** the Kobo. It installs the mod and restarts by itself.
> **Tip:** close any open book before installing, so the Kobo restarts to the home screen.

## How it talks to NickelMenu

"Physical book progress" touches `/tmp/nickelbookends_progress`. That's in RAM, not on the SD card. NickelBookends checks for that file for about 1.5 s after a tap and then shows the toast with Kobo's own `MainWindowController::toast`, the same one NickelDBus uses. "Hide book titles" edits `settings.ini`, and NickelBookends picks up the change at the next page turn or tap.

Credits: NickelHook and NickelMenu (MIT); the hooks follow NickelClock, NickelHardcover, NickelDissolve and NickelDBus.
