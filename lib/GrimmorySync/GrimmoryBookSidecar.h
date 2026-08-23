#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Per-book Grimmory sync state, persisted next to the book file as
// "<book-name>.grimmory-data.json" (a sidecar, like KOReader's .sdr folders,
// not CrossInk's internal hash-keyed /.crosspoint/epub_<hash>/ cache dir).
// Deliberately kept out of CrossInk's own EPUB metadata cache: this is the
// only place Grimmory-specific book data (identifiers, matched shelf/book
// IDs, and not-yet-synced reading sessions) lives.
//
// durationSeconds is always accurate regardless of clock state: it's a delta
// measured within one continuous boot via a monotonic timer, not a clock
// read. startTimeUtc is a best-effort real wall-clock anchor, populated only
// when the device had valid RTC time at the moment the session was recorded
// (see HalClock::getUtcEpochSeconds); it is 0 on devices with no
// battery-backed RTC (e.g. the X4) or if the RTC was never set/had a dead
// backup battery. GrimmorySyncEngine uploads sessions with a nonzero
// startTimeUtc individually using their real times; sessions with
// startTimeUtc == 0 fall back to being combined into one aggregated session
// per book, backdated from the sync run's fresh NTP time, since there is
// nothing trustworthy to reconstruct per-session in that case.
struct GrimmoryPendingSession {
  uint32_t durationSeconds = 0;
  int64_t startTimeUtc = 0;  // 0 = unknown (no valid RTC time when recorded)
  float startProgress = 0.0f;
  float endProgress = 0.0f;
  uint32_t startPage = 0;
  uint32_t endPage = 0;
};

class GrimmoryBookSidecar {
 public:
  std::string isbn10;
  std::string isbn13;
  std::string asin;
  int64_t grimmoryBookId = -1;  // -1 means "not yet matched to a Grimmory book"
  // Server-reported page count (metadata.pageCount), refreshed whenever this
  // book is matched during a shelf/catalog sync. 0 if unmatched or the
  // server has none. Used to estimate a book-wide page number for EPUBs
  // without local XLocations reference-page data (see
  // EpubReaderActivity::resolveGrimmoryPage) since it's the same page-count
  // scale Grimmory itself displays.
  uint32_t remotePageCount = 0;
  // Whether GrimmorySyncEngine has already attempted the one-time "pull
  // server progress if it's further along" check for this book (see
  // GrimmorySyncEngine.cpp's pullRemoteProgressIfAhead). Defaults to false
  // for sidecars written before this field existed, so a book matched in an
  // earlier sync still gets exactly one pull attempt on its next sync
  // rather than never getting one. Set true once a determinate answer comes
  // back (server had progress or definitively didn't), not on a transient
  // network/server failure, so a failed attempt can retry next sync.
  bool initialProgressPulled = false;
  std::vector<std::string> shelves;
  std::vector<GrimmoryPendingSession> pendingSessions;

  // Returns the sidecar path for a book file, e.g.
  // "/Books/foo.epub" -> "/Books/foo.grimmory-data.json".
  static std::string sidecarPathFor(const std::string& bookPath);

  // Loads the sidecar for bookPath, or a default-constructed instance if it
  // does not exist yet or fails to parse.
  static GrimmoryBookSidecar load(const std::string& bookPath);

  // Writes the sidecar for bookPath. Returns false on write failure.
  bool save(const std::string& bookPath) const;

  bool hasIdentifiers() const { return !isbn10.empty() || !isbn13.empty() || !asin.empty(); }
  bool isMatched() const { return grimmoryBookId >= 0; }

  // Preferred identifier for server-side matching: isbn13 > isbn10 > asin.
  const std::string& primaryIdentifier() const;

  // Appends a pending session, dropping the oldest entry first if the queue
  // is already at MAX_PENDING_SESSIONS (a book that's opened many times
  // between syncs shouldn't grow this file without bound).
  void addPendingSession(const GrimmoryPendingSession& session);

  static constexpr size_t MAX_PENDING_SESSIONS = 100;
};
