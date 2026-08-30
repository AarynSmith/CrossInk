## [v1.5.1] - 2026-08-20

### Added

- "Sync to Grimmory" is now available from the in-reader menu, alongside "Sync progress" and "Nearby position sync".
- Dictionary lookups can now save the selected word or phrase directly as a clipping.
- Readers can configure an Up + Down side-button shortcut; on touchscreen devices, while the reader touchscreen is disabled, the same chord always opens Settings so it can be restored.
- A new "Progress Decimal Places" reader setting controls how many decimal places are shown in book progress percentages across the status bar, dashboard, and reading statistics. Sync/upload progress precision is unaffected.
- Frontlit readers can choose Never for periodic full-screen refreshes, and Night Mode now applies across the interface.
- Full Xteink X4 Pro support, including USB Drive access to its SD card and direct USB file transfers.
- Tapping the reader status bar on touch devices now toggles it for the current reading session without changing the page layout.
- Touchscreen readers can choose tap, swipe, inverted tap, or disabled page-turn gestures, and assign two-finger swipes to frontlight, chapter, or font-size actions.
- Two-finger pinch gestures now adjust EPUB font size, and two-finger rotation turns supported reader layouts in the same direction.
- Quick Actions provides a menu of favorite reader commands, assignable to Power + Up and, on X4 Pro, Home-button gestures.
- A new Quick Lock shortcut locks the device without putting it to sleep; it uses the regular sleep timeout and can be assigned to Power + Up, long-press Back, or long-press Menu.
- Shortcuts can now go to the previous page or start Nearby Position Sync from an EPUB reader.

### Changed

- GrimmorySync now records real per-session start and end times on devices with a working clock (X3, X4 Pro) instead of combining every session since the last sync into one estimated session; X4 behavior, which has no built-in clock, is unchanged.
- On-screen keyboard keys are taller on every theme, giving touch readers larger tap targets.
- Touch-screen header Back buttons now use a heavier chevron for improved visibility.
- Frontlight schedules can now use one-minute start and end times.
- The reader menu now uses tablet icons to show whether its touchscreen is enabled or disabled.
- Font prewarming now uses a bounded fixed buffer instead of growing temporary strings, reducing heap churn while rendering mixed-style pages.
- Built-in fonts now use smaller sparse kerning data and stronger offline compression, leaving more firmware flash available without changing text layout.
- Screen Margin now has separate Top/Bottom and Left/Right controls, each adjustable in 5- and 10-pixel steps up to 200 pixels.
- Shortcut action pickers now use a consistent option order while hiding actions unsupported by the selected trigger or device.
- Wi-Fi passwords are now shown while entering them, making corrections easier on-device.
- EPUB progress calculations now reuse a bounded in-memory spine-size index while reading, reducing repeated SD-card seeks.
- Waking from deep sleep now keeps the selected sleep screen visible until Home or the reader is ready, removing the boot-up splash screen.
- Controls and Mark as Finished has moved to the settings gear tab in the reader menu.
- Font Family choices now identify built-in and SD-card fonts by their available point-size ranges, and the downloadable-font manager is labeled “Download Fonts”.
- Book Options is now the final item in the in-reader menu.

### Removed

- The undocumented X4 Pro power-button double-click frontlight toggle.
- Built-in reader-font emoticons and hand gestures; SD-card fonts still include emoji fallback support.

### Fixed

- The "Progress Decimal Places" reader setting now actually appears in Settings > Reader and the in-reader options menu.
- EPUB table captions no longer crash low-memory X3/X4 chapter indexing.
- Crash reports now include the faulting instruction and CPU exception details needed to diagnose fails on X3/X4, X4 Pro, and Sticky readers.
- EPUB text layout now exits safely instead of restarting when an X3/X4 runs out of contiguous memory while arranging right-to-left text.
- Holding page-turn buttons while browsing saved clippings no longer crashes.
- File Browser navigation no longer risks corrupted rows while the list is redrawing, and wrapped two-line filenames remain reachable when paging.
- The web EPUB optimizer now removes oversized XHTML comments that can otherwise stop low-memory chapter indexing.
- Mixed reader fonts, including SD-card fallback glyphs, are warmed together to avoid repeated slow redraws.
- Ruby annotations now stay together instead of splitting across lines.
- Transparent BMP sleep overlays no longer develop white holes over the preserved reader page.
- OPDS search now accepts the first Confirm press after closing the keyboard.
- Treat empty KOReader sync responses as no remote reading position.
- The image viewer no longer leaves a white box behind the hidden Previous/Next hint at the first or last image in a folder.
- Holding a side button for a chapter skip now skips immediately instead of waiting for release.
- XTC book covers now appear consistently across all Home screen layouts.
- Choosing Sleep from Quick Actions now reliably shows the selected cover or sleep screen.
- TXT readers now font size changes should work more reliably on touch devices and via shortcuts.
- Sleep-image, dictionary, and SD-font folders now work regardless of capitalization, including `/Sleep`, `/Dictionaries`, and `/Fonts`.
- X4 Pro's Home button shortcut for page turns now work in XTC and XTCH readers.
- The Previous Chapter two-finger swipe now opens the previous chapter at its first page.
- Power-button shortcuts now continue to work while the reader touchscreen is disabled.
- OPDS book lists now use the standard Back header without obscuring its divider, while keeping Search available.
- Opening Select Chapter no longer restarts X3/X4 readers for EPUBs with thousands of chapters.
- X4 now clears the retained sleep screen with one half refresh on wake when Sunlight Fading Fix is enabled, reducing ghosting before the next screen appears.
- Saved EPUB clipping highlights no longer wash out their text when reader text anti-aliasing is enabled.
- Saved EPUB clipping highlights now continue through ellipses, including non-breaking-space separators.
- EPUB clipping previews now retain complete multi-paragraph selections.
- Dismissing a dictionary definition with Back or an outside tap now returns directly to the reader instead of reopening word selection.
- The web Settings page now loads reliably when KOReader Sync has an older saved password.
- Nearby Stats Sync now shows Sync and Cancel buttons on touch readers.
- The image viewer now offers the "Set Cover" option for PNG files.
- Power-button shortcuts and Quick Actions can now toggle the frontlight or reader touchscreen when supported.
- On one-cover Lyra, Dashboard, and Minimal Home screens, swipe left to switch between the two most recent books.
- Dictionary word selection now follows the physical front-button direction in counter-clockwise landscape mode.
- End-of-book suggestions can now be opened by tapping them on touch devices.
- XTC and XTCH readers now ignore overlapping page turns while the display is updating, preventing corrupted pages after rapid swipes.
- XTC and XTCH readers no longer corrupt a page when turning or opening the menu during rendering.
- Dictionary font switches now retry after releasing the reader font when memory is tight.
- XTC table of contents now includes every available page entry, so large books can jump beyond the first 128 pages.
- Saved clipping highlights now remain accurate when a font or font-size change reflows a word across an inserted hyphen.
- Xteink readers wake faster by skipping redundant bootloader image validation after sleep.
- Large EPUB images keep the reader responsive during decoding.
- Full-height EPUB images no longer disappear when their container adds a top margin.
- EPUB page estimates now keep image-only and mixed image pages from being multiplied by XHTML byte density.
- Cancelling a chapter, footnote, location, or QR screen opened from the EPUB menu returns to that menu.
- EPUB and XTC readers retain less memory during ordinary reading by loading end-of-book suggestions only when needed.
- Long inherited dictionary-font names no longer overlap or extend beyond Font Options rows at Large UI size.
- KOReader Sync progress no longer remains interleaved with EPUB image pages after returning to the reader.
- Grimmory sync: manually sync reading sessions and shelf tags with a self-hosted Grimmory server from Settings > System. Matched books show their Grimmory shelf names on the per-book stats screen. Reading sessions now also push a KOReader-sync-protocol progress update, which Grimmory can propagate to its own native reading progress. The first time a book is matched to a Grimmory book, its server-side progress is pulled down if it is further along than this device's own progress, the same "furthest wins" behavior used by KOReader sync.
- "Sync to Grimmory" hotkey/quick-action, assignable to the Power button (short/long press) and the reader's long-press Confirm/Back actions, alongside the existing "Sync Progress" action. Triggered from the home screen or another non-reader screen, it syncs every recent book, same as Settings' "Sync Now"; triggered while a book is open, it syncs only that book and returns to the reader afterward.
- Grimmory Sync Behavior setting (Settings > System > Grimmory Sync), matching KOReader sync's Ask/Smart choice: "Ask every time" shows an Apply-remote/Upload-local screen whenever the single-book sync hotkey finds local and remote progress differ; "Smart sync" resolves it automatically (pulling remote if it's ahead, pushing local if it's ahead) and returns to the reader without waiting for a Back press.
- EPUB books left open on a footnote now reopen at the reading position instead of jumping to the end of the chapter, and keep showing their real progress percentage on the Home screen.
- Reading position is no longer lost when leaving an EPUB from the end-of-book screen.

## [v1.5.0] - 2026-08-08

### Added

- X4 Pro readers can lock the Home button while reading, with a Power-button shortcut to toggle it.
- End-of-book suggestions can now be opened directly by tapping their rows on touch devices.
- Quick Actions lets readers assign up to five favorite reader commands to one Power, Back, or Menu shortcut.

### Fixed

- EPUB tables now lay out a row at a time in both Incremental and Full Section indexing, keeping regular tables readable without whole-table buffering.
- Touch support for Seeed Studio Sticky
- Nearby File Transfer can send EPUB, TXT, XTC, XTCH, PNG, and BMP files directly between two CrossInk devices without a Wi-Fi network.
- Recent Books and image-file long-press actions can send files directly to a nearby CrossInk device.
- Dictionary lookup and lookup history
- EPUB books can use a dedicated SD-card dictionary font while keeping a different reader font.
- EPUB books can set a dedicated dictionary font size independently of the reader font size.
- Dictionary font and size defaults can be set globally from Settings > Reader > Font Options, with per-book choices still taking precedence.
- Reusable dictionary SD-font builder with IPA coverage and per-family ZIP packaging
- RTC-enabled devices can now choose the date format and numeric separator shown in headers from Settings > System > Device.
- The web EPUB optimizer now splits oversized chapters into memory-friendlier sections before sending them to the reader.
- Reader indexing can now use `Incremental` or `Full Section` mode globally or per book; changing modes keeps the current chapter readable and applies when the next chapter needs indexing.
- Look Up Word can now be assigned to short- and long-press Power button shortcuts.
- EPUB readers can now choose from five word-spacing levels, from normal through extra-wide.
- EPUB inline-image pages on X3 now use the grayscale-aware display base before the image grayscale overlay, reducing the moment where images appear too dark before settling.
- EPUB publisher small-caps styling now renders ASCII lowercase text as smaller capital letters without needing extra font files.
- When incremental EPUB indexing runs out of memory at the first unindexed page, the reader now silently restarts once and resumes the book with a fresh heap.

### Changed

- PSRAM-equipped readers now keep EPUB grayscale and image-cache working buffers in external memory, preserving more internal RAM for layout and reducing repeated SD reads on image pages.
- Reader font sizes now persist as actual point sizes, keeping the closest matching size when font families or installed files change.
- SD-card fonts now include the built-in reader fallback stack for common symbols, emoji, and selected CJK glyphs while retaining Noto Sans fallback coverage.
- Downloadable SD-card fonts are now rendered with the same darker anti-aliasing as the built-in reading fonts.
- Full-section EPUB indexing now prepares one-page chapters and direct jumps to a chapter's last page, while avoiding repeated checks after the next chapter is ready.
- EPUB grayscale rendering now reuses its 8 KB strip buffer across stable pages, reducing repeated heap allocation and release during long reading sessions.
- Reading progress is now saved in batches during ordinary page turns, immediately after layout changes, and when leaving a book, reducing repeated SD-card writes without carrying stale pagination into the next session.
- SD-card font discovery now waits until a custom font is selected or font settings are opened, reducing SD-card work during normal startup with built-in fonts.
- EPUB page turns using SD-card fonts now prepare the next page's glyphs while the reader is idle.
- Dictionary lookups now reuse open index files for stem matching, reducing repeated SD-card work after a miss.
- The web file manager now batches directory listings into fewer network packets, improving large-folder response time.
- Firmware releases now identify the supported device type: X3/X4 or Seeed Sticky.
- Image-heavy EPUB chapters now index by reading image headers first and extract each full image only when its page is shown.
- EPUB books with repeated byte-identical stylesheets now parse each unique stylesheet only once when building caches.
- SD-card fonts now reuse their page-sized glyph buffers, reducing heap fragmentation during long reading sessions.
- Firmware builds now prioritize usable heap over oversized system timer stacks and maximum WiFi throughput, leaving more memory for reading and network operations.
- Downloaded-font size range options now show their actual point-size ranges instead of firmware build names.
- KOReader Sync and authentication, OTA updates, and OPDS browsing now restart into a lightweight network mode that leaves reader and Home data unloaded, providing more contiguous memory for WiFi and secure connections.
- The web file manager can now delete non-empty folders recursively and, when hidden files are shown, remove hidden or system-managed SD card items after confirmation.
- SD-font, OPDS catalogs, and other unneeded settings now stay out of memory while reading unless their settings are open.
- EPUB books can now keep more saved clippings without loading every clipping's text into memory while reading.

### Removed

- The font download manager no longer offers a Download All action; fonts can still be downloaded individually or updated together.

### Fixed

- Book menu tab navigation, popup scrolling, customized Reading Stats hints, and short button presses after low-power mode now work reliably.
- Sleep screens now honor the current orientation, avoid X4 transition flashes, fall back to a valid wallpaper when needed, and handle low-memory image decoding without rebooting.
- Choosing Set Cover uses the selected image in place, and Home no longer repeatedly generates missing EPUB covers.
- Finished-book suggestions are now collected before an EPUB is moved to `/Read`.
- Manage Fonts now opens and scans large catalogs more safely on X3/X4, reports low-memory failures instead of restarting, and returns to Font Options when cancelled.
- Network screens refresh cleanly on X4; long errors wrap correctly; saved Wi-Fi networks and KOReader connections recover more reliably after restart or a missing address.
- Translated Wi-Fi and clock labels no longer truncate text or time values, and clock sync no longer risks a reboot while saving settings on memory-constrained X3/X4 devices.
- KOReader Sync no longer crashes during time setup, re-triggers while connecting, or loses precise EPUB positions; CrossPoint-only data stays on the official CrossPoint Sync server.
- Firmware updates reject images for the wrong chip family, and saved Wi-Fi settings safely handle concurrent access and corrupted values.
- EPUB opening, reflow, and background indexing now handle fragmented memory more safely, retry recoverable work, remain responsive to input and setting changes, and show useful errors instead of rebooting or silently returning Home.
- Low-memory EPUB grayscale and sleep rendering now fall back safely without leaving stale display content.
- Full-section indexing preserves more memory for large chapters and cancels speculative work on page turns, keeping the reader responsive.
- SD-card font and clipping work now release temporary data at the right time, preserving memory for reflow, dictionary use, covers, and thumbnails on X3/X4.
- EPUBs with book-specific built-in fonts no longer load an unnecessary global SD-card font, and custom fonts retain ligatures.
- EPUB styling choices apply before style caches load; CSS-heavy books use less temporary memory; and disabling Embedded Style consistently skips unused stylesheet work.
- EPUB layout now keeps CJK ruby and spaces, Russian paragraph continuations, Bionic Reading, underline/strikethrough runs, and right-to-left text correct.
- EPUBs with flowing `<br>` elements, image-led or decorative chapter headings, unsupported images, and dense final pages now lay out without excess gaps, clipping, dropped images, or misleading low-memory warnings.
- EPUB footnote and cross-reference previews now show complete notes, including targets in the middle of a paragraph.
- Saved EPUB positions, clipping highlights, and selections now stay accurate after font, orientation, or indexing changes; selections also remain readable in dark mode and on memory-tight pages.
- Dictionary misses can switch dictionaries without leaving the reader, and dictionary read failures now report an error instead of a false “not found.”
- Reader popups, KOReader Wi-Fi labels, Lyra battery headers, and the sleep message now remain correctly oriented and positioned.
- Manual refreshes preserve EPUB and TXT text anti-aliasing; XTC and XTCH status bars show the configured time-left estimate.
- Watchdog panics with captured diagnostics open crash reporting, while reset-only events return normally; power-button wake timing no longer depends on SD-card startup.
- The web file manager and uploads now handle simulator/device ports and stalled connections safely; unsupported settings stay hidden, and the optimizer removes empty chapter stubs without breaking table-of-contents links.
- KOReader Sync treats an empty response as no remote position and no longer interleaves its progress with EPUB image pages; its settings also persist after returning from lightweight network screens.
- Nearby Position Sync leaves the sender with one Back action and handles repeated packets more reliably, while Nearby Stats Sync exposes Sync and Cancel on touch readers.
- EPUB tables preserve column widths, wrap leading labels, and split oversized words rather than clipping them; large tables now remain usable on low-memory readers.
- EPUB layout handles right-to-left text and ruby annotations safely when memory is tight, and large chapter lists no longer restart X3/X4 readers.
- EPUB image pages remain responsive while decoding, retain full-height images with top margins, and estimate mixed image/text chapters accurately.
- Clearing an EPUB cache now returns Home so it rebuilds safely, and cancelling a chapter, footnote, location, or QR screen returns to the EPUB menu.
- EPUB and XTC readers retain less memory during ordinary reading; Change Font shortcuts also switch away from an active SD-card font before reindexing.
- Saved EPUB clipping highlights preserve contrast, remain continuous through ellipses and reflowed hyphens, and clipping previews retain complete multi-paragraph selections.
- Holding page-turn buttons while browsing saved clippings no longer crashes.
- Dictionary definition popups return directly to the reader when dismissed, no longer leave empty hint blocks, and retry font switches safely when memory is tight.
- XTC and XTCH readers prevent overlapping or in-progress page turns from corrupting pages, show covers across Home layouts, include full table-of-contents entries, stay within memory limits while building covers and thumbnails, and support X4 Pro Home-button page turns.
- TXT font-size changes work reliably from touch controls and shortcuts.
- Network connections no longer trigger repeated full-panel flashes.

## [v1.4.0.1] - 2026-07-28

### Added

- Updates to support Xteink device detection so the correct display panel driver is used.

## [v1.4.0] - 2026-07-10

### Added

- Dashboard UI theme for the Home screen, showing the current book cover and reading stats.
- Nearby Position Sync for sending or applying the current EPUB position between two CrossInk devices over ESP-NOW.
- Web EPUB optimizer support for CrossInk location metadata, so optimized EPUBs can keep better progress and stable page numbers.
- Reading Stats support for XTC and XTCH books, including reader menus, Home and sleep screen stats, mark finished, delete stats, and preserving stats when clearing book caches.
- Web file manager image previews, so PNG, JPEG, BMP, GIF, and WebP files can be viewed inline before downloading.

### Changed

- Large EPUBs, SD-card font-heavy books, and cover thumbnails now open, index, and generate more reliably under low-memory conditions.
- Home and sleep screens now load more cover and thumbnail data only when needed, reducing reader startup work and reusing cached cover data where possible.
- Built-in reader font choices have been reduced to Lexend Deca and Bitter, reducing firmware size while keeping fallback glyph coverage.

### Removed

- Teensy firmware builds are no longer produced for releases or release candidates.

### Fixed

- EPUB render-mode and Safe Mode toast messages now clear reliably, even when the reader is low on memory.
- EPUB Reading Stats no longer drops unsaved page-turn counts after viewing the stats screen mid-session.
- KOSync is more reliable with many SD-card fonts installed, reducing low-memory failures during secure sync requests and uploads.
- Web file manager actions now handle filenames with special characters safely and reject unsafe rename characters before saving.
- Auto Turn interval settings and related action prompts opened from long-press shortcuts now stay open after releasing the shortcut button.
- EPUB footnote previews no longer show clipped status-bar labels or misleading reader progress indicators, and clipping selection now works from footnote previews.
- Font selection no longer reopens the font preview after choosing a font.
- EPUB chapters with stale publisher style data now rebuild it instead of opening without the book's styling.
- Large SD-card font EPUBs no longer overlap characters after font or line-spacing changes, and clipping selection can fall back to a built-in UI font when needed.
- EPUB cover and thumbnail generation is more reliable with custom SD-card fonts selected and optimized books under low-memory conditions.
- Web EPUB optimizer now preserves more PNG and SVG artwork on-device, including transparent PNGs, dividers, and images in malformed or XML-declared chapters.
- Unsupported SVG images in EPUB chapters are now skipped silently instead of triggering low-memory image warnings.
- Nearby Position Sync now silently restarts back into the reader after using ESP-NOW, matching other WiFi sync flows and reducing post-sync memory fragmentation.
- EPUB page cache loading now uses fewer small heap allocations, reducing fragmentation-related reader failures.
- EPUB grayscale page turns on X3 now use the grayscale-aware display base, reducing the moment where new text appears too dark before the anti-aliased overlay finishes.
- EPUB chapters with many inline anchors, footnote links, malformed XHTML, large publisher styles, or SD-card fonts are less likely to fail or get stuck on the indexing screen.
- EPUB opening and image rendering now recover from more low-memory conditions instead of rebooting, including landscape image pages and books that need lighter render modes.
- EPUB clipping selection now follows right-to-left line order when selecting Hebrew and other RTL text.
- Lyra Carousel no longer shows a blank carousel after returning from WiFi-related File Transfer screens and moving between the menu row and book row.
- Generated SD-card font packages now include the same core glyph coverage as built-in reader fonts.
- Manage Fonts no longer crashes while loading or reloading large SD-card font lists.
- Minimal Home no longer swaps to another recent book when returning from Settings when Back button is mapped to the first button.
- Cancelling a font download now stops on the first Cancel button press instead of needing several presses.
- The `Inverted` sleep cover filter now keeps book covers unchanged on Minimal and Dashboard sleep screens while switching the background to white.
- Rare EPUB open or thumbnail crashes during ZIP decompression are fixed.
