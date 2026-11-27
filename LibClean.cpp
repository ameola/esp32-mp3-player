#include "LibClean.h"

#include <vector>

#include "LibStore.h"
#include "Text.h"

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isSpace(char c) { return c == ' ' || c == '\t'; }

int hexValue(char c) {
  if (isDigit(c)) return c - '0';
  c = tolower(static_cast<unsigned char>(c));
  return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// %XX (printable ASCII only) and the common HTML entities.
String decode(const String &s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    const char c = s[i];
    if (c == '%' && i + 2 < s.length()) {
      const int h = hexValue(s[i + 1]), l = hexValue(s[i + 2]);
      const int v = h * 16 + l;
      if (h >= 0 && l >= 0 && v >= 0x20 && v < 0x7F) {
        out += static_cast<char>(v);
        i += 2;
        continue;
      }
    }
    if (c == '&') {
      static const struct {
        const char *name;
        char ch;
      } entities[] = {{"&quot;", '"'}, {"&amp;", '&'}, {"&apos;", '\''}, {"&#39;", '\''},
                      {"&lt;", '<'},   {"&gt;", '>'},  {"&nbsp;", ' '}};
      bool done = false;
      for (const auto &e : entities) {
        size_t n = strlen(e.name);
        // Also without the ';' ("&amp/"), if no letter follows.
        const bool full = strncasecmp(s.c_str() + i, e.name, n) == 0;
        const bool bare = !full && strncasecmp(s.c_str() + i, e.name, n - 1) == 0 &&
                          !isalnum(static_cast<unsigned char>(s[i + n - 1]));
        if (full || bare) {
          if (bare) n--;
          out += e.ch;
          i += n - 1;
          done = true;
          break;
        }
      }
      if (done) continue;
      if (i + 3 < s.length() && s[i + 1] == '#') {  // &#NNN;
        size_t j = i + 2;
        int v = 0;
        while (j < s.length() && isDigit(s[j]) && v < 1000) v = v * 10 + (s[j++] - '0');
        if (j < s.length() && s[j] == ';' && j > i + 2 && v >= 0x20 && v < 0x7F) {
          out += static_cast<char>(v);
          i = j;
          continue;
        }
      }
    }
    out += c;
  }
  return out;
}

// Decoded, underscores as spaces, single spaces, trimmed.
String tidy(const char *text) {
  const String d = decode(text);
  String out;
  out.reserve(d.length());
  for (size_t i = 0; i < d.length(); i++) {
    char c = d[i] == '_' ? ' ' : d[i];
    // Japanese tags: the ideographic space (U+3000) as a space, full-width
    // ASCII (U+FF01-FF5E, "Ｒｅｍｉｘ") as plain ASCII.
    const uint8_t b0 = d[i], b1 = i + 1 < d.length() ? d[i + 1] : 0, b2 = i + 2 < d.length() ? d[i + 2] : 0;
    if (b0 == 0xE3 && b1 == 0x80 && b2 == 0x80) {
      c = ' ';
      i += 2;
    } else if (b0 == 0xEF && (b1 == 0xBC || b1 == 0xBD) && b2 >= 0x80 && b2 <= 0xBF) {
      const uint32_t cp = 0xF000 | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
      if (cp >= 0xFF01 && cp <= 0xFF5E) {
        c = static_cast<char>(cp - 0xFF01 + 0x21);
        i += 2;
      }
    }
    if (isSpace(c) && (out.isEmpty() || out[out.length() - 1] == ' ')) continue;
    out += isSpace(c) ? ' ' : c;
  }
  out.trim();
  return out;
}

bool endsWithNoCase(const String &s, const char *suffix) {
  const size_t n = strlen(suffix);
  return s.length() >= n && strcasecmp(s.c_str() + s.length() - n, suffix) == 0;
}

bool hasLetter(const String &s) {
  for (size_t i = 0; i < s.length(); i++) {
    if (isalpha(static_cast<unsigned char>(s[i])) || s[i] == '?' || static_cast<uint8_t>(s[i]) >= 0x80) return true;
  }
  return false;
}

bool inList(const String &s, const char *const *list) {
  for (; *list; list++) {
    if (s.equalsIgnoreCase(*list)) return true;
  }
  return false;
}

const char *const kJunkArtists[] = {"artist", "unknown", "unknown artist", "<unknown>", "[unknown]",
                                    "recorded", "mp3", "none", nullptr};
const char *const kJunkAlbums[] = {"title", "album", "unknown", "unknown album", "<unknown>", "[unknown]",
                                   "tuner", "none", "untitled", nullptr};

// Ripper defaults: "New Artist (15)", "New Title (3)", "Track 15",
// "Unknown Album 2".
bool numberedJunk(const String &s) {
  String l = s;
  l.toLowerCase();
  size_t i = 0;
  for (const char *word : {"new artist", "new title", "new album", "track", "unknown artist", "unknown album",
                           "unknown title"}) {
    if (l.startsWith(word)) {
      i = strlen(word);
      break;
    }
  }
  if (!i) return false;
  for (; i < l.length(); i++) {
    if (!isDigit(l[i]) && !isSpace(l[i]) && l[i] != '(' && l[i] != ')' && l[i] != '#') return false;
  }
  return true;
}

String unbracket(String s) {  // "(The Living End)" -> "The Living End"
  if (s.length() > 2 && s[0] == '(' && s[s.length() - 1] == ')' && s.indexOf(')') == static_cast<int>(s.length()) - 1) {
    s = s.substring(1, s.length() - 1);
    s.trim();
  }
  return s;
}

bool webAddress(const String &s) {
  String l = s;
  l.toLowerCase();
  return l.indexOf("www.") >= 0 || l.indexOf(".com") >= 0 || l.indexOf(".net") >= 0 ||
         l.indexOf(".org") >= 0 || l.indexOf("blogspot") >= 0;
}

// Reads 1..maxDigits digits at s[i]; returns the count read (0: none).
int readNumber(const String &s, size_t i, int maxDigits, int &value) {
  int n = 0;
  value = 0;
  while (i + n < s.length() && isDigit(s[i + n]) && n < maxDigits + 1) value = value * 10 + (s[i + n++] - '0');
  return n >= 1 && n <= maxDigits ? n : 0;
}

size_t skipSpaces(const String &s, size_t i) {
  while (i < s.length() && isSpace(s[i])) i++;
  return i;
}

// One leading track number: the length of the prefix to drop (0: none) and
// its number.
size_t numberPrefix(const String &t, int &number) {
  size_t i = 0;
  const bool paren = t.length() && t[0] == '(';
  if (paren) i++;
  int v = 0;
  const int digits = readNumber(t, i, 3, v);
  if (!digits) return 0;
  size_t j = i + digits;
  // "1-01 Title" (disc-track)
  if (!paren && digits == 1 && j + 2 < t.length() && t[j] == '-' && isDigit(t[j + 1]) && isDigit(t[j + 2]) &&
      (j + 3 == t.length() || !isDigit(t[j + 3]))) {
    size_t k = skipSpaces(t, j + 3);
    if (k < t.length() && (t[k] == '-' || t[k] == '.')) k = skipSpaces(t, k + 1);
    if (k > j + 3 && k < t.length()) {
      number = (t[j + 1] - '0') * 10 + (t[j + 2] - '0');
      return k;
    }
  }
  if (paren) {
    if (j >= t.length() || t[j] != ')') return 0;
    j++;
    // "(08) Title", "(13)Title": the brackets set it apart
    const size_t k = skipSpaces(t, j);
    if (k < t.length() && t[k] != '-' && t[k] != '.' && !isDigit(t[k])) {
      number = v;
      return k;
    }
  }
  const size_t afterSpaces = skipSpaces(t, j);
  // "12 - Title" (also before another number: "00 - 01 - Title")
  if (afterSpaces > j && afterSpaces < t.length() && t[afterSpaces] == '-') {
    const size_t k = skipSpaces(t, afterSpaces + 1);
    if (k > afterSpaces + 1 && k < t.length()) {
      number = v;
      return k;
    }
  }
  // "04.Title", "04) Title", "(02) - Title": a separator, then no digit
  if (afterSpaces < t.length() && (t[afterSpaces] == '-' || t[afterSpaces] == '.' || t[afterSpaces] == ')')) {
    const size_t k = skipSpaces(t, afterSpaces + 1);
    if (k < t.length() && !isDigit(t[k])) {
      number = v;
      return k;
    }
  }
  // "01 Title": zero-padded, then a space
  if (!paren && t[i] == '0' && digits >= 2 && afterSpaces > j && afterSpaces < t.length()) {
    number = v;
    return afterSpaces;
  }
  return 0;
}

// Drops up to 3 leading numbers; the last non-zero one is the track.
String stripNumbers(String t, int &track) {
  track = 0;
  for (int n = 0; n < 3; n++) {
    int number = 0;
    const size_t cut = numberPrefix(t, number);
    if (!cut || cut >= t.length()) break;
    if (number) track = number;
    t = t.substring(cut);
  }
  return t;
}

bool trackTitle(const String &t) {  // "Track 1", "Audio Track 01", "Piste 3", "Untitled"
  String l = t;
  l.toLowerCase();
  if (l == "untitled") return true;
  size_t i = 0;
  if (l.startsWith("audio")) i = skipSpaces(l, 5);
  if (l.startsWith("track", i)) i += 5;
  else if (l.startsWith("piste", i)) i += 5;
  else return false;
  i = skipSpaces(l, i);
  int v;
  const int d = readNumber(l, i, 3, v);
  return d && i + d == l.length();
}

// Splits on " - ".
std::vector<String> dashParts(const String &s) {
  std::vector<String> parts;
  int start = 0;
  for (;;) {
    const int at = s.indexOf(" - ", start);
    String p = s.substring(start, at < 0 ? s.length() : at);
    p.trim();
    parts.push_back(p);
    if (at < 0) break;
    start = at + 3;
  }
  return parts;
}

bool isNumber(const String &s) {
  int v;
  return s.length() && readNumber(s, 0, 3, v) == static_cast<int>(s.length());
}

String join(const std::vector<String> &parts, size_t from) {
  String out;
  for (size_t i = from; i < parts.size(); i++) out += (out.length() ? " - " : "") + parts[i];
  return out;
}

// "Disc 2", "(Disc 2)", "[CD2]", "CD 1" at the end of an album name.
String splitDisc(const String &album, int &disc) {
  disc = 0;
  String l = album;
  l.toLowerCase();
  int end = l.length();
  while (end > 0 && isSpace(l[end - 1])) end--;
  const bool closed = end > 0 && (l[end - 1] == ')' || l[end - 1] == ']');
  if (closed) end--;
  int d = end;
  while (d > 0 && isDigit(l[d - 1]) && end - d < 2) d--;
  if (d == end || (d > 0 && isDigit(l[d - 1]))) return album;
  const int number = l.substring(d, end).toInt();
  int w = d;
  while (w > 0 && isSpace(l[w - 1])) w--;
  int start = -1;
  for (const char *word : {"disc", "disk", "cd"}) {
    const int n = strlen(word);
    if (w >= n && l.substring(w - n, w) == word) start = w - n;
  }
  if (start <= 0) return album;
  if (closed) {  // "(Disc 2)", "( Disk 2)", "[CD2]"
    while (start > 0 && isSpace(l[start - 1])) start--;
    if (start == 0 || (l[start - 1] != '(' && l[start - 1] != '[')) return album;
    start--;
  } else if (!isSpace(l[start - 1])) {
    return album;
  }
  String name = album.substring(0, start);
  name.trim();
  while (name.length() && strchr("-:,", name[name.length() - 1])) {
    name.remove(name.length() - 1);
    name.trim();
  }
  if (name.isEmpty()) return album;
  disc = number;
  return name;
}

// Mostly letters the fonts don't have (Japanese, Greek, Cyrillic...): it
// would show as a row of "?". The fonts carry Latin-1 and typographic
// punctuation (U+2000-206F); anything past Latin Extended counts as missing.
bool undrawable(const String &s) {
  int missing = 0, shown = 0;
  for (size_t i = 0; i < s.length();) {
    const uint8_t c = s[i];
    uint32_t cp = c;
    int n = 1;
    if (c >= 0xF0) cp = c & 0x07, n = 4;
    else if (c >= 0xE0) cp = c & 0x0F, n = 3;
    else if (c >= 0xC0) cp = c & 0x1F, n = 2;
    for (int k = 1; k < n && i + k < s.length(); k++) cp = (cp << 6) | (s[i + k] & 0x3F);
    i += n;
    if (cp == ' ') continue;
    if (cp > 0x24F && (cp < 0x2000 || cp > 0x206F)) missing++;
    else shown++;
  }
  return missing > shown;
}

// "Artist - Album [2006]" from the song's folder, skipping disc folders
// ("CD2", "Disc 1"); false if the folder isn't named that way.
bool folderArtistAlbum(const char *folderPath, String &artist, String &album) {
  String path = folderPath ? folderPath : "";
  while (path.length()) {
    const int slash = path.lastIndexOf('/');
    String name = path.substring(slash + 1);
    path = path.substring(0, max(slash, 0));
    name = tidy(name.c_str());
    int d = 0;
    if (splitDisc("x " + name, d) == "x" && d) continue;  // "CD2", "Disc 1"
    const std::vector<String> parts = dashParts(name);
    if (parts.size() != 2 || parts[0].isEmpty() || parts[1].isEmpty()) return false;
    artist = parts[0];
    album = parts[1];
    const int bracket = album.lastIndexOf(" [");  // "Back To Basics [2006]"
    if (bracket > 0 && album.endsWith("]")) album = album.substring(0, bracket);
    album.trim();
    return true;
  }
  return false;
}

}  // namespace

String libNormName(const char *name, bool dropThe) {
  String out;
  const String folded = foldAscii(name);  // "Beyoncé" groups with "Beyonce"
  const char *s = folded.c_str();
  if (dropThe && strncasecmp(s, "the ", 4) == 0 && s[4]) s += 4;
  for (; *s; s++) {
    const char c = *s;
    if (c == '&' || c == '+') out += "and";
    else if (isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(tolower(static_cast<unsigned char>(c)));
  }
  if (out.isEmpty()) {  // nothing but symbols ("????"): keep them apart
    out = name;
    out.toLowerCase();
  }
  return out;
}

uint32_t libNormHash(const char *name, bool dropThe) {
  const String n = libNormName(name, dropThe);
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n.length(); i++) h = (h ^ static_cast<uint8_t>(n[i])) * 16777619u;
  return h;
}

void libCleanSong(SongRecord &rec, const char *fileName, const char *folderPath) {
  String stem = decodeUtf8(reinterpret_cast<const uint8_t *>(fileName), strlen(fileName));
  if (endsWithNoCase(stem, ".mp3")) stem.remove(stem.length() - 4);
  stem = tidy(stem.c_str());

  String artist = unbracket(tidy(rec.artist)), album = tidy(rec.album), albumArtist = tidy(rec.albumArtist);
  auto junkArtist = [](String &a) {
    if (inList(a, kJunkArtists) || numberedJunk(a) || !hasLetter(a)) a = "";
    if (a.equalsIgnoreCase("various") || a.equalsIgnoreCase("various artist")) a = "Various Artists";
  };
  // Tags the fonts can't show: the file and folder names say it instead.
  const bool lostArtist = undrawable(artist) || undrawable(albumArtist);
  if (undrawable(artist)) artist = "";
  if (undrawable(albumArtist)) albumArtist = "";
  if (undrawable(album)) album = "";
  junkArtist(artist);
  junkArtist(albumArtist);
  if (inList(album, kJunkAlbums) || numberedJunk(album) || webAddress(album)) album = "";
  if (lostArtist && artist.isEmpty()) {
    String a, b;
    if (folderArtistAlbum(folderPath, a, b)) {
      artist = a;
      if (albumArtist.isEmpty()) albumArtist = a;
      if (album.isEmpty()) album = b;
    }
  }

  String title = tidy(rec.title);
  if (title.isEmpty() || trackTitle(title) || numberedJunk(title) || undrawable(title)) title = stem;
  if (endsWithNoCase(title, ".mp3")) {
    title.remove(title.length() - 4);
    title.trim();
  }
  // A title that's only a number: often the track number ("18"), with the
  // real title in the file name ("18 - 18 - Can U Dance.mp3"). Real
  // titles like "19" or "7" stay: their file names say no more.
  int number = 0;
  bool digits = title.length() > 0;
  for (size_t i = 0; i < title.length(); i++) digits = digits && isDigit(title[i]);
  if (digits) {
    const String fromName = stripNumbers(stem, number);
    bool nameLetters = false;  // "9_16 - 36.mp3" says no more than "36"
    for (size_t i = 0; i < fromName.length(); i++) {
      nameLetters = nameLetters || isalpha(static_cast<unsigned char>(fromName[i])) || static_cast<uint8_t>(fromName[i]) >= 0x80;
    }
    if (nameLetters) title = stem;
  }
  title = stripNumbers(title, number);
  int track = rec.track ? rec.track : number;
  if (!track) stripNumbers(stem, track);

  // An untagged "Artist - Title" or "Artist - Album - 03 - Title".
  std::vector<String> parts = dashParts(title);
  if (artist.isEmpty() && parts.size() >= 2 && parts[0].length()) {
    artist = parts[0];
    size_t from = 1;
    if (parts.size() >= 4 && isNumber(parts[2])) {
      if (album.isEmpty()) album = parts[1];
      if (!track) track = parts[2].toInt();
      from = 3;
    } else if (parts.size() >= 3 && isNumber(parts[1])) {
      if (!track) track = parts[1].toInt();
      from = 2;
    }
    title = join(parts, from);
    artist = unbracket(artist);
    junkArtist(artist);
  }

  // "Artist - Title", "Title - Artist", "Title - Various Artists".
  const String na = libNormName(artist.c_str(), true);
  for (int n = 0; n < 3 && artist.length(); n++) {
    bool changed = false;
    const int first = title.indexOf(" - ");
    if (first > 0 && libNormName(title.substring(0, first).c_str(), true) == na) {
      title = title.substring(first + 3);
      title.trim();
      changed = true;
    }
    const int last = title.lastIndexOf(" - ");
    if (last > 0) {
      const String suffix = libNormName(title.substring(last + 3).c_str(), true);
      if (suffix == "variousartists" || suffix == "various" || suffix == na ||
          (suffix.length() >= 8 && na.startsWith(suffix))) {
        title = title.substring(0, last);
        title.trim();
        changed = true;
      }
    }
    if (!changed) break;
  }
  while (title.endsWith("-")) {
    title.remove(title.length() - 1);
    title.trim();
  }
  if (title.isEmpty()) title = stem;

  int disc = 0;
  for (int n = 0; n < 2; n++) {  // "Maynard's Rewind CD 2 Disc 2"
    int d = 0;
    const String cut = splitDisc(album, d);
    if (!d) break;
    album = cut;
    if (!disc) disc = d;
  }

  libCopyField(rec.title, sizeof(rec.title), title);
  libCopyField(rec.artist, sizeof(rec.artist), artist);
  libCopyField(rec.album, sizeof(rec.album), album);
  libCopyField(rec.albumArtist, sizeof(rec.albumArtist), albumArtist);
  rec.track = track;
  if (disc) rec.disc = disc;
}
