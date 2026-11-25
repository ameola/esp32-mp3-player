# ELEGOO ESP32 MP3 Player

A touchscreen MP3 player for the ELEGOO ESP32-WROOM-32 (`ESP32 Dev Module`). It lists the MP3 files on the LCD board's microSD card and plays them through the MAX98357A I2S amplifier.

The original component test (display, touch, SD, two test tones) is kept in `ComponentTest/ComponentTest.ino`.

## Libraries

- esp32 by Espressif Systems (Boards Manager, tested with 3.3.12)
- Adafruit GFX Library, Adafruit ILI9341
- ESP8266Audio by Earle F. Philhower (2.x; works on ESP32). ESP32-audioI2S is not used because it requires PSRAM, which the WROOM-32 lacks.

## Build and Upload

```
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app --upload -p /dev/cu.usbserial-0001 .
```

The WiFi and HTTPS code makes the program about 1.38 MB, more than the default 1.25 MB app partition, so it needs the **Huge APP** scheme (3 MB app, no OTA; in the Arduino IDE: Tools → Partition Scheme). Settings saved in NVS (the WiFi network) are kept.

### Benchmark build

`Bench.cpp` measures SD clock speeds, folder listing methods and tag reading time, and prints the results to Serial (115200). It writes 2000 synthetic MP3s to `/_bench` on first run (the player ignores that folder).

```
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app --build-property compiler.cpp.extra_flags=-DRUN_BENCH=1 --upload -p /dev/cu.usbserial-0001 .
```

## Source Layout

| File | Purpose |
|---|---|
| `MP3Player.ino` | `setup()` / `loop()` |
| `Config.h` | Pins, clocks, tuning, layout, colors |
| `Lcd` | Raw-SPI display driver and off-screen canvas |
| `Touch` | FT6336U polling |
| `AudioPlayer` | MP3 playback on core 0: read-ahead task, Helix decoder task, always-on block-written I2S output |
| `Text` | Character conversion for the ASCII font |
| `Tags` | ID3 tag reader (title, artist, album, album artist, track, disc, year) and MP3 length (Xing/VBRI, TLEN, CBR estimate) |
| `CardFile` | Direct FatFs file access (faster than Arduino `File` for tag reading) |
| `Library` | Card mounting and background indexing task (core 0) |
| `Catalog` | Read access to the live library for the UI (songs in title order, paths) |
| `LibFormat.h` | On-card library format: file layout and record structures |
| `LibClean` | Cleans song text while the library is built: encodings, track numbers, junk tags, untagged "Artist - Title" files, disc suffixes; normalized names for grouping |
| `LibStore` | Library file reading/writing, CRC checks, and safe switching between generations |
| `Indexer` | Builds/updates the on-card library at boot: folder walk, tag reading (reusing unchanged songs), on-card sorting, artist/album grouping, crash resume |
| `ExtSort` | External merge sort on the card (fixed RAM use at any library size) |
| `Ui` | Screen drawing and touch handling |
| `CardSettings` | Per-card settings file (`/.mp3player/settings.txt`): album art downloads on or off |
| `WifiSetup` | First-run WiFi screen: network list, on-screen keyboard, connection test; credentials in NVS |
| `ArtScan` | Serial `artscan`: survey of the album art on the card (embedded pictures and folder images) |
| `ArtFetch` | Downloads while no music plays: songs' lyrics (LRCLIB), album covers (MusicBrainz search, Cover Art Archive 250 px) and artist photos (Deezer), cache in `/.mp3player/art` |
| `LyricStore` | Downloaded lyrics in one card file, `/.mp3player/lyrics.dat`: a 16,384-slot index (key, text position, kind, date) and the texts |
| `ArtView` | Draws a cover JPEG from the card with the ESP32 ROM's TJpgDec decoder, block by block straight to the LCD (~4 KB while drawing); makes the 30 px list thumbnails (progressive JPEGs from their DC scan) |
| `ThumbStore` | All list thumbnails in one 16 MB file (`/.mp3player/art/thumbs.dat`), read with a seek and one 2 KB read |
| `Bench`, `BenchStore`, `BenchIndex` | Benchmarks and tests, only built with `-DRUN_BENCH=N`: `1` SD/listing, `2` tag reader, `3` library format, `4` indexer; `5`/`6`/`7`/`8` card tools (add test song, remove it, delete the library, card info); `9` writes 48,000 test songs to `/_bench/big`; `10` scale test (index, check, patched add/retag/remove each compared with a full sort, list speed); `11`/`12` move those songs to `/Big Test` (visible to the player) and back; `13` SD clock check; `14` sort and tag-read profile. Modes 4, 10, 13, 14 and the card tools wait for a line on Serial before starting, so the monitor can attach first |

## Using It

- Copy `.mp3` files anywhere on the card (folders up to 8 deep). Cards must be FAT32: this ESP32 core's FAT library has exFAT disabled, so large cards need reformatting.
- Tabs along the bottom: **Songs** (all songs A-Z), **Artists** (artist → albums → songs, or "All songs"), **Albums** (A-Z → songs in track order), **Folders** (the card's folders), **Settings**. Tapping the current tab goes back to its top.
- The header shows where you are (a back arrow below a tab's top, or the tab's name and count) and, on the right, library status and the WiFi state (lit while album art downloads).
- Swipe the list to scroll it; flick it to make it coast (it slows down and settles on a row). Flicking again while it's still moving adds speed, for crossing long lists quickly. Touching a moving list stops it.
- Drag or tap the A-Z strip on the right of Songs, Artists and Albums to jump to a letter.
- Tapping a song plays it (on release, if the finger didn't move); next/previous and auto-advance follow the list it was started from (the album, the artist, the folder, or all songs). "Previous" restarts the current song if it is past the start. With nothing playing, play starts the list on screen.
- Rows show the title and artist from the file's ID3 tags, cleaned (see Library Clean-up; track numbers in album lists) and the song length. Files without a title tag show their file name. The built-in font is ASCII only: accented Latin letters are shown without accents (é → e), and other characters show as `?`.
- The mini player above the tabs shows the playing song with its cover, play/pause and next, and a progress line. Tap it for **Now Playing**: the cover, title and artist, a seek bar with elapsed / total time (estimated from the playback position in the file), previous / play / next and the volume slider (0–10). The arrow at its top left closes it.
- If no card is found at startup, insert one and tap the screen to retry.
- The first time a card is used (no `/.mp3player/settings.txt`), the player asks for a WiFi network for album art downloads: tap a network, type its password on the on-screen keyboard (**shift** for one capital, **123** / **#+=** for digits and symbols) and tap **Join**; or tap **Skip** to turn album art downloads off for that card. The password is kept in the ESP32's NVS, not on the card.
- Album art downloads run by themselves when nothing has played for 10 seconds (stopped or paused) and the library isn't updating; the status corner shows "Art" and how far it got. Pressing play stops a download (usually well under a second, at most about 5 s) and continues the paused song where it was. Albums without an album name or artist are skipped. Covers aren't shown yet.
- The library index is stored on the card (`/.mp3player`, hidden), so the list appears straight away at startup. The card is then checked for changes in the background (status in the top-right corner); if anything changed, the library is rebuilt while you keep listening and the lists refresh when it's done. Only new or changed files have their tags read, and only the changed folders are re-sorted (merged into the previous sort). The very first time, a progress screen is shown while every song is read. Albums with no album artist and several track artists in one folder are listed under "Various Artists".
- Folders are listed A-Z, except folders with more than 256 entries, which are listed in the card's own order.
- To rebuild the library from scratch (re-reading every song's tags), use Settings → Rebuild library (tap twice), or hold the status at the top right for 2 seconds. A progress screen shows the current step, an overall progress bar, the song count and the folder being read; tap it to go back to the player while the rebuild continues.
- Debugging: over Serial (115200), `tap x y`, `drag x y1 y2 [ms]` (with a duration it moves like a finger; short = a flick), `swipe y x1 x2` (sideways) and `hold x y ms` inject touches. `audiocpu` reports how much of 3 s went to decoding. Each scroll prints a `[scroll]` line with its frame rate. `mem` prints free memory after each startup step and now (`byte-addressable` is what buffers can use; the larger `free` figure includes instruction RAM, which only takes 32-bit accesses). `artscan` surveys album art on the card, `wifisetup` shows the WiFi screen again, `artfetch` starts an album art pass now, `artartists` one that starts at the artist steps (the albums it skips are done by the next pass), and `artstop` stops it (each album and artist logs its search and download hops with timings).

## Look and Settings

- **Text and icons** are anti-aliased: Inter (8 sizes and weights) and Material Symbols icons (3 sizes), rendered to 4-bit coverage bitmaps by `tools/make_fonts.py` into `UiFontData.cpp` (80 KB of flash; fonts and their licences in `tools/fonts/`). `UiFont` decodes UTF-8 and blends each pixel with what is already in the canvas, so text sits cleanly on tinted rows. The fonts carry Latin-1 accents and typographic quotes, dashes and "…". Names are kept as UTF-8 ("Beyoncé", "Señorita"); letters the fonts lack (Greek, Cyrillic) show as "?" but are kept. Sorting, A-Z buckets, grouping, cover keys and search use an accent-folded form (`foldAscii`), so "Beyonce" and "Beyoncé" agree.
- **Themes** (Settings → Theme): Night radio (amber), Deep sea (teal), Graphite (coral) and Cover colour, which takes its accent and tint from the playing album's thumbnail (its main hue, weighted by how colourful each pixel is). Each theme is ten colours (`UiTheme.h`); screens use only those names, so every theme fits every screen.
- **Layout** (Settings): density Comfortable (40 px rows) or Compact (34 px), text size Standard or Large (rows 6 px taller), corners Round or Square. Rows that don't fill the list exactly leave the last one cut off at the list's end.
- **Brightness** (Low / Medium / High / Max) dims the backlight with 25 kHz PWM (above hearing, so it can't whine through the amp). **Screen off after** 30 s, 1 min, 5 min, 15 min or never: the backlight turns off without a touch, and the touch that wakes it does nothing else.
- **Shuffle** (Now Playing, left of the controls) plays the whole list once in a random order starting at the chosen song: a keyed Feistel permutation, so no memory is spent on the order. **Repeat** (right) cycles off, all, one.
- **Settings tab**: Display (theme, density, text size, corners, brightness, screen off, lyrics), Audio (bass, treble, maximum volume), WiFi & downloads (network, downloads on/off, download now), Library (counts, rebuild: tap twice), System (version, board, free memory, card size, running time). A setting with choices opens a list of them with the current one ticked; themes show a swatch, and a pick applies at once so others can be tried.
- **Audio** (Settings → Audio, saved in NVS "audio"): three sliders on one page, each a row with its value and a track of steps (touch or drag anywhere on the row; the sound changes as the knob moves, saved when the finger lifts). Bass and Treble from -6 to +6 dB in 2 dB steps, a low shelf at 180 Hz (about where a small speaker still plays) and a high shelf at 4 kHz (RBJ biquads, slope 1, float, on the mono mix in the I2S output stage, retuned when a song's sample rate changes). With a boost the signal is first turned down by half the largest boost, then clamped, so loud songs clip less. Cost: about 2% of a core (decoding 617 ms per 3 s instead of 560). Maximum volume (Full, 75%, 50%) scales the volume control's top step, so the slider keeps its range but can't go too loud.
- **Lyrics** (Settings → Display → Lyrics, on by default): swipe Now Playing's cover left to show the song's lyrics, right to go back. Lyrics come from an `.lrc` file next to the song (`Song.mp3` → `Song.lrc`), which wins, else from the download pass (below); a song that starts playing without lyrics is looked up first in the next pass, and its lyrics appear by themselves when they arrive. The lyrics page is there for every song: without lyrics it says why (not looked up yet, downloads off, none found, or "Instrumental"). Two dots under the cover show there's a second page when the song has lyrics, and the first three times the top bar says "Swipe the cover for lyrics". Timed lines (`[mm:ss.xx]`, several stamps per line, `[offset:]`) keep the playing one two rows from the top, lit and larger (15 px bold, 18 px with Large text), with every part of a line that wraps lit together; plain text scrolls with the song. UTF-8 or Latin-1, up to 12 KB. Tap a timed line, or the seek bar, to jump there (the position is estimated from the song's length). Off: no lyrics are read or shown.
- **Search**: the search icon in the Songs, Artists and Albums headers opens a field with the on-screen keyboard (`Keyboard`, shared with the WiFi setup). Search finds the text anywhere in artist and album names and in songs' titles, artists and albums. Results come in sections: Artists, Albums and Songs, each with its count, its first few hits (3, 3, 5) and "See all" for the rest; songs play through all the song hits, A-Z. Songs are found in one pass over `songs.dat` (about 2.3 s for 8,000 while playing, slower while an album art download uses the card); copies of a song show once.
- Settings are saved on the device (NVS), not the card. Serial `uistyle reset` restores the defaults (`uistyle theme compact large square brightness sleep` sets them). Settings also has album art downloads on / off, WiFi network (opens the WiFi setup), Rebuild library and the firmware version.
- The A-Z strip scrolls with the list in hardware, so a moving list re-sends it every frame: it's kept composed in a 5.8 KB buffer and sent in one or two transfers. Scrolling while music plays: 17 ms per frame, as before the redesign.

## Library Clean-up

What the player shows is cleaned while the library is built; the files on the card aren't changed. The rules came from a real 8,000-song card (`LibClean.h` lists them):

- **Text**: URL encoding (`%20`) and HTML entities (`&quot;`) decoded; underscores as spaces; `.mp3` and double spaces dropped.
- **Track numbers** taken off the front of titles (`01 - `, `(02) - `, `04.`, `01 `, `1-01 `) and used as the track number when the tag has none; real titles that start with a number ("22 Acacia Avenue", "2-4-6-8 Motorway", "99 Red Balloons") keep it. A title that's only a number ("18") takes the file's name when that says more; "7" or "36" stay.
- **Untagged files**: "Artist - Title.mp3" (also "Artist - Album - 03 - Title") gives artist, album and track; "Track 1" titles take the file's name. "Artist - " before, and " - Artist" or " - Various Artists" after a title are dropped when they repeat its artist.
- **Junk tags** ("Artist", "Album", "title", "<Unknown>", "Tuner", web addresses) count as missing.
- **Grouping**: artists and albums group by a normalized name (case, punctuation, "&"/"and", a leading "The" ignored), so "Korn", "KoRn" and "korn" are one artist, shown as spelled most often. "Disc 2", "(CD 1)", "[CD2]" come off album names and become the disc number, so an album's discs are one album (in disc, then track order).
- **"none" albums**: albums with 3 songs or fewer, and songs without an album, go to their artist's "none" album (last on the artist's page, songs A-Z). The Albums tab has one "none" with every artist's, A-Z. On the test card: 300 albums listed, 3,180 songs in "none".
- **Duplicates**: copies of a song (same title and artist after cleaning, lengths within 2 s) are shown once, the copy in the folder with the most songs (most likely its album). The Folders tab still shows every file. On the test card: 1,202 hidden.
- **How**: titles are sorted first, and copies of a song land next to each other there. The artist sort runs on normalized keys; a second sort ("regroup") counts each album's songs as they stream by (an album's songs are together) and moves small albums to "none", keyed by title. The Albums tab's "none" (`none.idx`) is the title order filtered. Full build of 8,072 songs: 106 s.
- Album art keys use the same normalized names, so spellings and discs share a cover. Pictures saved under the earlier keys are moved once by the download pass (about 70 s); a `keys-normalized` file in the art folder then skips that check for good.
- Changing the rules: bump `LIB_CLEAN_VERSION` (`LibFormat.h`), and the next start rebuilds the library with every tag re-read.
- `libdump` (Serial) prints every song's cleaned tags and path, tab-separated, for checking rules against a card on a computer.

## Library Performance

Measured on the ESP32 with 50,188 songs (6,453 folders) on a 2 GB FAT32 card:

| | Player (music playing) | Benchmark (`RUN_BENCH=10`) |
|---|---|---|
| Check at startup (nothing changed) | 20 s, in the background | 21 s |
| Update after adding, removing or retagging a song | about 4 min (48k songs) | about 4 min |
| Full build (every tag read) | | 5.7 min |
| Songs list, one screen of 6 rows | | 11 ms |
| Library shown after display init | about 0.25 s | |

How it stays workable at that size, and things to keep if you change it:

- **Opening folders and files by cluster.** FatFs finds a name by scanning the parent folder from the start, which is about 100 ms per folder in a folder of 1,600 artists. The walk captures each entry's start cluster while listing the parent and opens it directly (`CardDir`, `CardFile::openCluster`), checking the folder's `.` entry and falling back to the path.
- **Fast seeks.** This core's FatFs has fast seek compiled out, so every backward seek walks the file's cluster chain from the start. `SeekIndex` keeps every 16th cluster of a library file, so random reads take about 1.5 ms instead of 7 ms.
- **Single-sector writes.** The SD driver's multi-block write occasionally failed in the player with no message (FatFs `FR_DISK_ERR`) while the LCD and audio shared the bus. All library writes go through `cardWrite`, which hands FatFs at most one sector at a time.
- **Sort-key patch.** Each build keeps its sorted title and artist keys (`title.keys`, `artist.keys`). The next build drops the songs of changed folders, shifts the ids of the rest, and merges in the freshly sorted changed songs. It's used when at least half the songs are kept, and falls back to a full sort if the keys are missing or corrupt, the folder order changed, or the build resumed.
- **Memory.** The player has about 100 KB of byte-addressable heap free while it decodes (with WiFi linked, which costs 25 KB of static RAM), and the heap is fragmented. Sort buffers are sized from what's free, small working arrays check before they grow, and running out of memory fails the build instead of the player.
- **Retries.** A failed build is retried up to 3 times, resuming from its last checkpoint (every 256 songs).
- **CPU.** The indexer runs on core 0 above the idle task and sleeps one tick every 50 ms so the watchdog stays fed. Audio also runs on core 0, at higher priority; the UI has core 1 to itself.
- **Sorter memory.** Each sorter reserves its run table and temp-file handle when it starts. Grown later, they needed 5 KB blocks that no longer existed once reading songs had fragmented the heap (a full sort makes ~300 runs).
- **Compact file objects.** This core's FatFs is built for 4 KB sectors, so a `FIL` carries a 4 KB buffer, but an SD card's sectors are 512 bytes and FatFs only uses that much of it. `cardNewFil()` allocates `FIL`s with a 512-byte buffer once the mount shows 512-byte sectors, and the card is mounted with one VFS file slot instead of five (each reserves a 4 KB `FIL`; nothing uses Arduino `File` now). Together with reading the song file through FatFs directly, this freed about 35 KB while playing.
- **Tag reading** uses a 2 KB block cache: about 29 small reads per real song become about 2 card reads (7.3 to 5.2 ms per song).

## Album Art Downloads

WiFi and TLS need about 90 KB, more than the player has free while it decodes (about 100 KB, with nothing left for the library or the screen), and WiFi connecting causes audio gaps (its driver runs on core 0 far above the decoder). So downloads only run while no music plays, with the audio engine released (about 45 KB more):

- **WiFi** is started through ESP-IDF directly (`esp_wifi_init` with fewer buffers, no A-MPDU, no Arduino event task: about 43 KB) and power save off (it made every TLS round trip wait for a beacon: 5-7 s handshakes became about 3 s). Everything created for a pass is freed after it, except lwIP's task (about 7 KB, once).
- **Search** over plain HTTP (`musicbrainz.org/ws/2/release-group`), one request every 2 s (closer together, MusicBrainz answers about half with 503), retried on 503:
  1. the exact album name (without "(Disc 1)" / "[...]" suffixes) and album artist as phrases, score 90 or more;
  2. if nothing: every word of the album name (except "the", "and", "Vol." and words with an apostrophe), any word of the artist, then the top 3 are checked here: same artist and title after ignoring case, punctuation, "&"/"+"/"and", a leading "The" and spaces in the artist ("2 Pac" is "2Pac"); a name cut short (43 bytes in the library, 30 in ID3v1 tags) may match the start of a longer title, and a title with 3+ words may be the start of the other ("Greatest Hits, Vol. 1" is "Greatest Hits 1 1962-1965");
  3. if still nothing and the name has a subtitle: step 2 with the part before the ":".
- **Cover**: `coverartarchive.org/release-group/<id>/front-250`, redirected via archive.org to a storage node; the `dn*` nodes force HTTPS (the only TLS connection), `ia*` nodes serve plain HTTP. Redirects are followed by hand so a stop request is noticed between reads.
- **Artist photos**: after the album covers, a pass gives each artist a picture. First, offline, a stand-in: the thumbnail of their first album with a cover. Then a Deezer search (`api.deezer.com/search/artist`, plain HTTP, no account, one request per 0.5 s): the first result with the same name (compared like album artists) gives its 250 px photo from Deezer's CDN, which replaces the stand-in. Deezer has no photo for some artists (an empty picture id); those, and names it doesn't know ("30 Seconds to Mars" is "Thirty Seconds to Mars" there), get a `.no` marker like albums and keep the stand-in. About 0.5-1 s per artist. "Various Artists" and "Unknown Artist" are skipped.
- **Progressive JPEGs**: about 1 in 5 Deezer photos (and some covers) are progressive, which TJpgDec can't read. Their first scan holds each 8x8 block's DC coefficient, the image at 1/8 scale (31 px for 250 px), enough for a thumbnail: a small decoder in `ArtView` reads just that scan (2 bytes per block instead of ~190 KB of coefficients), ~100 ms. (Other sizes from Deezer's CDN come back baseline at first and progressive later, so asking for one doesn't help.) The cover view still needs a baseline file.
- **List thumbnails**: artist rows (and an artist's "All songs" row), album rows and song rows show a 30×30 thumbnail; placeholders are a person or a note. Thumbnails live in `ThumbStore`: one 16 MB file of 8,192 2 KB slots (key, flags, CRC, 1,800 bytes of RGB565), a slot found by hashing the key with a few probes. As separate `<key>.thm` files each read was a FatFs path lookup from the root folder (300 entries here) down to its bucket: 30-70 ms, too slow for a moving list. In the open store it's a seek (`SeekIndex`) and one read. The pass writes the store (created with zeros once, ~1 minute; older `.thm` files are moved in and deleted); the list only reads it. The last 8 thumbnails (~15 KB) and the last 64 keys without one are kept in RAM, reset on a tab switch. While the list moves, at most 2 thumbnails are read per frame; rows past that show the placeholder until the list stops. Scrolling, measured while playing: 16-17 ms per frame in every list, also over rows never seen. A song finds its album's cover the way the indexer names the album: album artist (else track artist, else "Unknown Artist") and the album name cut to 43 characters; with no album artist it also tries "Various Artists", since compilations are filed there. A playing song's row shows a play badge over its cover; folder rows show a large folder icon in the same slot so the text lines up.
- **Sharing the card**: steps of the pass that need no network pause 10 ms between items, which also keeps them from starving core 0's idle task (the task watchdog reset the board otherwise).
- **Now Playing** shows the cover at 125×125: a 250 px cover decoded at 1/2 by the ROM's TJpgDec, straight to the screen.
- **Cache**: `/.mp3player/art/<xx>/<key>.jpg`, key = hash of album artist and album name, or of the artist name for photos (survives library rebuilds); `<key>.no` marks albums and artists without a match or picture, retried after 30 days; its size is the version of the search that wrote it, and markers from an older search are retried right away.
- About 2-5 s per album (1-2.5 hours for 2,000 albums, once). Every cover so far was a baseline JPEG of about 250x250, 12-42 KB.

- **Other formats named .mp3** (a WMA file was found in this library) aren't played: before a song starts, its first bytes are checked for WMA/ASF, FLAC, Ogg, MP4/AAC and WAV signatures. The decoder would otherwise find false frame syncs all through such a file and play noise, stuttering, at half a core. The player skips to the next song (once round the list at most) and the status corner says "Skipped: not an MP3" for 4 s.

## Lyrics Downloads

LRCLIB (`lrclib.net/api/get`, free, no account) finds a song by artist, title and length (within 2 s); 404 if it has no lyrics. In a sample of 40 songs from this library it had 33, 27 of them timed. It's HTTPS only, and the player has no memory for TLS while it decodes, so lyrics come in the same idle pass as album art:

- **Order**: songs the player asked for first (Now Playing opened on a song without lyrics), then every song, then the pictures. A pass stops when music plays; the next one skips what's known (one index lookup per song).
- **One TLS connection per pass**, kept open between songs: the first request takes about 2.3 s (the handshake), the rest about 0.4 s. LRCLIB drops a kept connection every hundred or so requests; the next handshake waits 1.5 s, since straight away the heap is still fragmented by the old one.
- **Rate**: after a burst of about 60 requests LRCLIB answers every other one 429 at 0.6 s apart. The gap between requests starts at 0.25 s and grows by 0.5 s (to 3 s) on each 429, and the song is asked again after its Retry-After. Measured: about 1 song per second, so 7,300 songs take about 2 hours of idle time, once.
- **Memory**: the TLS handshake needs nearly all the heap left with WiFi up (~64 KB), so the lyrics steps and the picture steps each have their files open only while they run, and the answer (up to ~35 KB of JSON, the lyrics twice and more) is read as it streams in, its text written straight into the store through a 256-byte buffer: the timed text, which comes after the plain one, is written over it. A song's text is kept up to 12 KB, cut after a whole line.
- **Store**: `/.mp3player/lyrics.dat`, one file so thousands of songs don't each take a cluster: 16,384 16-byte slots (key, offset, length, kind: timed / plain / instrumental / none, day looked up), then the texts appended. Key: hash of artist (else album artist) and title folded to ASCII, lower case, so copies share it and it survives rebuilds. "None" is retried after 30 days. Untagged songs (no artist) are skipped. The player opens the file for each lookup (a long-open handle would cache a sector and the file's size).

Known: on this board (no PSRAM) covers on archive.org's HTTPS-only `dn*` nodes no longer download: their certificate check runs out of memory (about 58-62 KB free with WiFi up). Those albums are skipped without a marker and retried by the next pass (on the WROVER this goes away).

## Display Driver Note

Adafruit_ILI9341's drawing calls don't work with this panel. Like the component test, the player only uses `tft.begin()` and then the vendor init table, and sends all pixels with raw SPI (CASET/RASET/RAMWR, then pixel data). UI regions are composed in an off-screen `GFXcanvas16` and blitted line by line, releasing the shared SPI bus after every line so the audio and library tasks' SD reads can interleave.

The list area uses the panel's hardware vertical scrolling (VSCRDEF / VSCRSADD): while the list moves, only the lines that come into view are drawn. Everything drawn into the list area goes through `listBlit`, which maps screen lines to the rotated frame memory; full-screen layouts reset the offset.

### Audio

Playback never depends on the UI loop:

- **Decoder task** (core 0, priority 3, above the indexer): Helix decoder (`AudioGeneratorMP3a`), decodes ahead until the output is full, then sleeps a few ms. About 21% of a core.
- **Read-ahead task** (core 0, priority 2): keeps 12 KB of the song file in RAM (0.3 s at 320 kbps, 0.75 s at 128 kbps). While the indexer writes, the card can stall a read for 150 ms or more; the decoder only ever reads from RAM.
- **Output**: 6 DMA buffers (78 ms). ESP8266Audio's `AudioOutputI2S` makes one `i2s_channel_write()` per sample (about 40% of a core on its own); `ContinuousI2S` stages samples and writes blocks. The I2S clocks never stop (see the noise note above).
- The UI calls (play, stop, pause, position, volume) take a lock shared with the decoder task.
- Serial `audiocpu` shows decode time, underruns (the output running dry: an audible gap) and the read-ahead low-water mark over 3 s; `[scroll]` lines include the underrun count.

- **Released while idle.** For album art downloads the decoder and the read-ahead ring are freed (`audioReleaseEngine`, only while stopped or paused); `audioPlay` / `audioResume` rebuild them, and a paused song reopens at its byte position. The decoder's loop guards against a false frame sync right after such a seek (Helix then reports 0 channels and the library would divide by zero).

Measured with music playing and a full library rebuild running: no underruns; scrolling about 20 fps or better, slower for moments while the sort is busy on the card.

## Tuning (`Config.h`)

- `TOUCH_SWAP_XY`, `TOUCH_INVERT_X`, `TOUCH_INVERT_Y`: fix taps that land in the wrong place. Serial (115200) prints raw and mapped coordinates for every tap.
- `LCD_SPI_HZ`: display SPI clock. 2 MHz is the speed the component test proved works; try 8 MHz for faster redraws.
- `SD_SPI_HZ`: SD card SPI clock, 20 MHz (verified by the benchmark). Lower it if songs fail to load. Faster settings have no effect: this core's SD driver caps the clock at 25 MHz, and the nearest the ESP32 can make below that is 20 MHz.
- `GAIN_PER_STEP`, `DEFAULT_VOLUME`: volume range.

## Wiring

- Shared SPI: GPIO18 SCK, GPIO19 MISO, GPIO23 MOSI. TFT_CS=13, SD_CS=27.
- TFT DC=16, shared LCD/CTP reset=17, backlight=33.
- Touch SDA=21, SCL=22 at 100 kHz, address 0x38. CTP_INT stays disconnected; touch is polled.
- MAX98357A: BCLK=25, LRCLK=26, DIN=32. Connect the speaker across OUT+ and OUT-, never to ground.
- Display VCC and MAX98357A VIN use 5 V with common grounds. Use a 5 V supply that can handle both.
