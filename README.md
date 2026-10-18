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

## Build and install

```sh
sh build.sh      # Docker + NickelTC -> KoboRoot.tgz
```

Copy `KoboRoot.tgz` into `KOBOeReader/.kobo/` and eject. The tgz also installs the NickelMenu entries (`.adds/nm/nickelbookends`).

**Uninstall:** delete `.adds/nickelbookends/uninstall`, then reboot. If NickelPages is still installed, NickelBookends asks it to uninstall itself at the next reboot.

**Tests:** run `./test/run.sh` (needs Qt5 dev packages). It also checks that no source file has C++ static initializers.

## How it talks to NickelMenu

"Physical book progress" touches `/tmp/nickelbookends_progress`. That's in RAM, not on the SD card. NickelBookends checks for that file for about 1.5 s after a tap and then shows the toast with Kobo's own `MainWindowController::toast`, the same one NickelDBus uses. "Hide book titles" edits `settings.ini`, and NickelBookends picks up the change at the next page turn or tap.

## History / lessons

- **v1–v2 crashed Nickel at boot.** NickelHook runs the mod's init from an ELF constructor, before C++ global objects exist. All state now lives in a struct created in init.
- **v3–v4 used the wrong book;** v5+ uses the title Kobo shows.
- **v7–v8 conflicted with NickelPages** when both were installed.
- **v9 briefly showed the previous page again on page turns with "Hide book titles" on.** Changing the caption's text mid-turn made the footer re-layout, and Kobo redrew its stale page. v10 never changes Kobo's text: it paints the shortened caption itself in the label's own paint event, with the label's font, colour and alignment. Nothing is resized or re-laid out.

Credits: NickelHook and NickelMenu (MIT); the hooks follow NickelClock, NickelHardcover, NickelDissolve and NickelDBus.
