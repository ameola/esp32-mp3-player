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
| `LibStore` | Library file reading/writing, CRC checks, and safe switching between generations |
| `Indexer` | Builds/updates the on-card library at boot: folder walk, tag reading (reusing unchanged songs), on-card sorting, artist/album grouping, crash resume |
| `ExtSort` | External merge sort on the card (fixed RAM use at any library size) |
| `Ui` | Screen drawing and touch handling |
| `CardSettings` | Per-card settings file (`/.mp3player/settings.txt`): album art downloads on or off |
| `WifiSetup` | First-run WiFi screen: network list, on-screen keyboard, connection test; credentials in NVS |
| `ArtScan` | Serial `artscan`: survey of the album art on the card (embedded pictures and folder images) |
| `ArtFetch` | Album art downloads while no music plays: MusicBrainz search, Cover Art Archive 250 px cover, cache in `/.mp3player/art` |
| `Bench`, `BenchStore`, `BenchIndex` | Benchmarks and tests, only built with `-DRUN_BENCH=N`: `1` SD/listing, `2` tag reader, `3` library format, `4` indexer; `5`/`6`/`7`/`8` card tools (add test song, remove it, delete the library, card info); `9` writes 48,000 test songs to `/_bench/big`; `10` scale test (index, check, patched add/retag/remove each compared with a full sort, list speed); `11`/`12` move those songs to `/Big Test` (visible to the player) and back; `13` SD clock check; `14` sort and tag-read profile. Modes 4, 10, 13, 14 and the card tools wait for a line on Serial before starting, so the monitor can attach first |

## Using It

- Copy `.mp3` files anywhere on the card (folders up to 8 deep). Cards must be FAT32: this ESP32 core's FAT library has exFAT disabled, so large cards need reformatting.
- Tabs along the top: **Songs** (all songs A-Z), **Artists** (artist → albums → songs, or "All songs"), **Albums** (A-Z → songs in track order), **Folders** (the card's folders). Tapping the current tab goes back to its top; each tab remembers where you were.
- The bar under the tabs has a back button, the trail of where you are, and volume - / + (0–10).
- Swipe the list to scroll it; flick it to make it coast (it slows down and settles on a row). Flicking again while it's still moving adds speed, for crossing long lists quickly. Touching a moving list stops it.
- Drag or tap the A-Z strip on the right of Songs, Artists and Albums to jump to a letter. The page buttons (▲ ▼) move a screen at a time.
- Tapping a song plays it (on release, if the finger didn't move); next/previous and auto-advance follow the list it was started from (the album, the artist, the folder, or all songs). "Previous" restarts the current song if it is past the start. With nothing playing, play starts the list on screen.
- Rows show the title and artist from the file's ID3 tags (track numbers in album lists) and the song length. Files without a title tag show their file name. The built-in font is ASCII only: accented Latin letters are shown without accents (é → e), and other characters show as `?`.
- The now-playing panel shows elapsed / total time, estimated from the playback position in the file.
- If no card is found at startup, insert one and tap the screen to retry.
- The first time a card is used (no `/.mp3player/settings.txt`), the player asks for a WiFi network for album art downloads: tap a network, type its password on the on-screen keyboard (**shift** for one capital, **123** / **#+=** for digits and symbols) and tap **Join**; or tap **Skip** to turn album art downloads off for that card. The password is kept in the ESP32's NVS, not on the card.
- Album art downloads run by themselves when nothing has played for 10 seconds (stopped or paused) and the library isn't updating; the status corner shows "Art" and how far it got. Pressing play stops a download (usually well under a second, at most about 5 s) and continues the paused song where it was. Albums without an album name or artist are skipped. Covers aren't shown yet.
- The library index is stored on the card (`/.mp3player`, hidden), so the list appears straight away at startup. The card is then checked for changes in the background (status in the top-right corner); if anything changed, the library is rebuilt while you keep listening and the lists refresh when it's done. Only new or changed files have their tags read, and only the changed folders are re-sorted (merged into the previous sort). The very first time, a progress screen is shown while every song is read. Albums with no album artist and several track artists in one folder are listed under "Various Artists".
- Folders are listed A-Z, except folders with more than 256 entries, which are listed in the card's own order.
- To rebuild the library from scratch (re-reading every song's tags), hold the top-right status corner for 2 seconds. A progress screen shows the current step, an overall progress bar, the song count and the folder being worked on; music keeps playing, and tapping returns to the player while the rebuild continues.
- Debugging: over Serial (115200), `tap x y`, `drag x y1 y2 [ms]` (with a duration it moves like a finger; short = a flick) and `hold x y ms` inject touches. `audiocpu` reports how much of 3 s went to decoding. Each scroll prints a `[scroll]` line with its frame rate. `mem` prints free memory after each startup step and now (`byte-addressable` is what buffers can use; the larger `free` figure includes instruction RAM, which only takes 32-bit accesses). `artscan` surveys album art on the card, `wifisetup` shows the WiFi screen again, `artfetch` starts an album art pass now and `artstop` stops it (each album logs its search and download hops with timings).

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
- **Search** over plain HTTP (`musicbrainz.org/ws/2/release-group`, album name without "(Disc 1)" / "[...]" suffixes plus album artist; score 90 or more), one request per second, retried on 503.
- **Cover**: `coverartarchive.org/release-group/<id>/front-250`, redirected via archive.org to a storage node; the `dn*` nodes force HTTPS (the only TLS connection), `ia*` nodes serve plain HTTP. Redirects are followed by hand so a stop request is noticed between reads.
- **Cache**: `/.mp3player/art/<xx>/<key>.jpg`, key = hash of album artist and album name (survives library rebuilds); `<key>.no` marks albums without a match or cover, retried after 30 days.
- About 2-5 s per album (1-2.5 hours for 2,000 albums, once). Every cover so far was a baseline JPEG of about 250x250, 12-42 KB.

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
