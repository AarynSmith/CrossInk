#include "GrimmorySyncEngine.h"

#include <Epub.h>
#ifndef SIMULATOR
#include <HalClock.h>
#endif
#include <HalStorage.h>
#include <Logging.h>
#include <esp_system.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "GrimmoryBookSidecar.h"
#include "GrimmoryCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "ProgressMapper.h"

namespace {
constexpr char EPUB_CACHE_DIR[] = "/.crosspoint";
// Bounds the size of a single books-page response so parsing one page never
// requires buffering an unbounded amount of JSON/string data at once — a
// large Grimmory library previously caused an out-of-memory abort here. Kept
// small deliberately: each book's raw (unfiltered-on-the-wire) JSON includes
// title/author/paths/covers/etc, so even a handful of books can be several
// KB, and now that the connection is reused for the whole run (see
// GrimmoryApiSession), extra page requests no longer cost a fresh handshake.
constexpr int BOOKS_PAGE_SIZE = 5;

// Same epsilon KOReaderSyncActivity's smart-sync mode uses to treat two
// progress percentages (0.0-1.0 scale) as "already in sync" rather than
// churning a write, or asking the user to choose, over noise.
constexpr float SAME_PROGRESS_EPSILON = 0.001f;

// Identifies this firmware to Grimmory's koreader-sync protocol, matching
// the constant KOReaderSyncClient.cpp uses for CrossInk's own (separate)
// KOReader sync feature.
constexpr char KOREADER_DEVICE_ID[] = "crossink-device";
constexpr char KOREADER_DEVICE_NAME[] = "CrossInk";

// Generates a random lowercase-hex string via the ESP32's hardware RNG, used
// to bootstrap a fresh koreader-sync username/password the first time this
// Grimmory account has none configured (see ensureKoreaderSyncReady). Not
// meant to be memorable: the user never needs to type it, only Grimmory and
// this firmware ever see it.
std::string generateRandomHex(const size_t len) {
  static const char hexDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(len);
  for (size_t i = 0; i < len; i++) {
    out.push_back(hexDigits[esp_random() % 16]);
  }
  return out;
}

// Ensures this Grimmory account has a koreader-sync username/password, with
// sync enabled and "sync progress with web reader" enabled, bootstrapping or
// flipping on whatever is missing so a fresh account works without the user
// first visiting Settings > Devices in Grimmory's own UI. Returns false
// (outUser left however far it got) if any step fails; the passwordMd5 is
// what pushKoreaderProgressForBook needs as the x-auth-key.
bool ensureKoreaderSyncReady(GrimmoryApiSession& apiSession, const std::string& accessToken,
                             GrimmoryKoreaderUser& outUser) {
  if (GrimmoryApiClient::getKoreaderUser(apiSession, accessToken, outUser) != GrimmoryApiClient::OK) {
    LOG_ERR("Grimmory", "Failed to fetch koreader-sync account");
    return false;
  }

  if (outUser.username.empty()) {
    const std::string username = std::string("crossink-") + generateRandomHex(8);
    const std::string password = generateRandomHex(24);
    LOG_INF("Grimmory", "No koreader-sync account configured; creating one (username=%s)", username.c_str());
    if (GrimmoryApiClient::setKoreaderCredentials(apiSession, accessToken, username, password) !=
        GrimmoryApiClient::OK) {
      LOG_ERR("Grimmory", "Failed to create koreader-sync credentials");
      return false;
    }
    // Re-fetch rather than trusting the local password: the server computes
    // and returns passwordMD5, sparing this client an MD5 implementation.
    if (GrimmoryApiClient::getKoreaderUser(apiSession, accessToken, outUser) != GrimmoryApiClient::OK) {
      LOG_ERR("Grimmory", "Failed to re-fetch koreader-sync account after creating credentials");
      return false;
    }
  }

  if (!outUser.syncEnabled) {
    if (GrimmoryApiClient::setKoreaderSyncEnabled(apiSession, accessToken, true) != GrimmoryApiClient::OK) {
      LOG_ERR("Grimmory", "Failed to enable koreader sync");
      return false;
    }
    outUser.syncEnabled = true;
  }

  if (!outUser.syncWithWebReader) {
    if (GrimmoryApiClient::setKoreaderSyncWithWebReader(apiSession, accessToken, true) != GrimmoryApiClient::OK) {
      LOG_ERR("Grimmory", "Failed to enable koreader sync-progress-with-grimmory");
      return false;
    }
    outUser.syncWithWebReader = true;
  }

  if (outUser.passwordMd5.empty()) {
    LOG_ERR("Grimmory", "Koreader-sync account has no passwordMD5; cannot authenticate progress pushes");
    return false;
  }
  return true;
}

// Minimal read-only re-implementation of
// EpubReaderUtils::readProgressFile/loadProgress (src/activities/reader/),
// which this lib deliberately does not depend on to keep GrimmorySync
// lib-only (no src/ includes) — see the file layout convention noted
// elsewhere in this repo. Binary layout must stay in sync with
// EpubReaderUtils::saveProgress.
struct SavedEpubProgress {
  int spineIndex = 0;
  int pageNumber = 0;
  int pageCount = 0;
};

bool readProgressBinFile(const std::string& path, SavedEpubProgress& out) {
  if (!Storage.exists(path.c_str())) return false;
  FsFile f;
  if (!Storage.openFileForRead("Grimmory", path, f)) return false;
  uint8_t data[10];
  const int dataSize = f.read(data, sizeof(data));
  f.close();
  if (dataSize != 4 && dataSize != 6 && dataSize != 10) return false;

  out.spineIndex = static_cast<int>(static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8));
  out.pageNumber = static_cast<int>(static_cast<uint16_t>(data[2]) | (static_cast<uint16_t>(data[3]) << 8));
  if (out.pageNumber == UINT16_MAX) out.pageNumber = 0;
  if (dataSize == 6 || dataSize == 10) {
    out.pageCount = static_cast<int>(static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8));
  }
  return true;
}

bool loadSavedEpubProgress(const Epub& epub, SavedEpubProgress& out) {
  const std::string progressPath = epub.getCachePath() + "/progress.bin";
  if (readProgressBinFile(progressPath, out)) return true;
  return readProgressBinFile(progressPath + ".bak", out);
}

// Minimal re-implementation of EpubReaderUtils::saveProgress's atomic
// tmp-write + backup-rotate + rename sequence (same reasoning as
// readProgressBinFile above: this lib stays out of src/). Always writes the
// 6-byte layout (no visibleTextOffset) since a pulled remote position has no
// such offset to preserve; readProgressBinFile already treats that as a
// valid, complete record.
bool writeProgressBinFile(const std::string& progressPath, int spineIndex, int pageNumber, int pageCount) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("Grimmory", "Pulled progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber,
            pageCount);
    return false;
  }
  const std::string tmpPath = progressPath + ".tmp";
  const std::string backupPath = progressPath + ".bak";

  if (Storage.exists(tmpPath.c_str()) && !Storage.remove(tmpPath.c_str())) {
    LOG_ERR("Grimmory", "Could not remove stale progress temp file");
    return false;
  }

  FsFile f;
  if (!Storage.openFileForWrite("Grimmory", tmpPath, f)) {
    LOG_ERR("Grimmory", "Could not open progress temp file for write");
    return false;
  }
  uint8_t data[6];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = pageNumber & 0xFF;
  data[3] = (pageNumber >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  const size_t written = f.write(data, sizeof(data));
  if (written != sizeof(data)) {
    LOG_ERR("Grimmory", "Short write saving pulled progress: %u/%u bytes", (unsigned)written, (unsigned)sizeof(data));
    f.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }
  f.flush();
  if (!f.sync() || !f.close()) {
    LOG_ERR("Grimmory", "Failed to sync/close progress temp file");
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) {
    LOG_ERR("Grimmory", "Could not remove old progress backup");
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (Storage.exists(progressPath.c_str()) && !Storage.rename(progressPath.c_str(), backupPath.c_str())) {
    LOG_ERR("Grimmory", "Could not rotate progress backup");
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(tmpPath.c_str(), progressPath.c_str())) {
    LOG_ERR("Grimmory", "Could not replace progress file");
    if (Storage.exists(backupPath.c_str()) && !Storage.exists(progressPath.c_str())) {
      Storage.rename(backupPath.c_str(), progressPath.c_str());
    }
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return true;
}

// Maps `remote` to a local position and overwrites progress.bin with it.
// Shared by pullRemoteProgressIfAhead (the one-time initial-sync pull) and
// resolveSingleBookProgressNow/applySingleBookProgressChoice (the per-sync
// "Sync to Grimmory" hotkey comparison, gated by GrimmorySyncBehavior).
bool applyRemoteKoreaderProgress(const std::shared_ptr<Epub>& epub, const std::string& path,
                                 const GrimmoryKoreaderProgress& remote) {
  const KOReaderPosition remoteKoPos{remote.progress, remote.percentage};
  const CrossPointPosition remotePos =
      ProgressMapper::toCrossPoint(epub, remoteKoPos, -1, 0, PositionCoordinateSpace::CurrentDocument);
  if (!remotePos.valid) {
    LOG_ERR("Grimmory", "Could not map remote progress for %s", path.c_str());
    return false;
  }

  const int pageCount = std::max(remotePos.totalPages, remotePos.pageNumber + 1);
  const std::string progressPath = epub->getCachePath() + "/progress.bin";
  if (!writeProgressBinFile(progressPath, remotePos.spineIndex, remotePos.pageNumber, pageCount)) {
    LOG_ERR("Grimmory", "Failed to write remote progress for %s", path.c_str());
    return false;
  }
  return true;
}

// Loads this book's current on-disk saved position and maps it to koreader
// format (percentage + xpath) on the same book-wide scale remote progress
// uses. Shared by pushKoreaderProgressForBook and
// resolveSingleBookProgressNow. Returns an invalid KOReaderPosition
// (valid=false) if there's no saved local progress or it can't be mapped.
KOReaderPosition loadLocalKoreaderPosition(const std::shared_ptr<Epub>& epub) {
  SavedEpubProgress saved;
  if (!loadSavedEpubProgress(*epub, saved)) return KOReaderPosition{"", 0.0f, false};

  const CrossPointPosition localPos{saved.spineIndex, saved.pageNumber, std::max(1, saved.pageCount)};
  return ProgressMapper::toKOReader(epub, localPos, PositionCoordinateSpace::CurrentDocument);
}

// Pushes this book's current on-disk saved position via the koreader-sync
// protocol (PUT /api/koreader/syncs/progress). Since
// ensureKoreaderSyncReady enables "sync progress with web reader" above,
// this also updates Grimmory's own native/web-reader progress server-side
// (Grimmory converts the XPath-like position to EPUB CFI itself), not just
// its separate "KOReader Progress" field. Only called for books that just
// had a reading session synced (see runSync) — that is the signal a book
// was actually read since the last sync.
void pushKoreaderProgressForBook(GrimmoryApiSession& apiSession, const GrimmoryKoreaderUser& koUser,
                                 const std::string& path, GrimmorySyncSummary& summary) {
  auto epub = std::make_shared<Epub>(path, EPUB_CACHE_DIR);
  if (!epub->load(false, true)) {
    LOG_ERR("Grimmory", "Could not load %s for koreader progress push", path.c_str());
    summary.progressPushFailed++;
    return;
  }

  const KOReaderPosition koPos = loadLocalKoreaderPosition(epub);
  if (!koPos.valid) {
    LOG_INF("Grimmory", "No saved/mappable progress for %s; skipping koreader progress push", path.c_str());
    return;
  }

  const std::string documentHash = KOReaderDocumentId::calculate(path);
  if (documentHash.empty()) {
    LOG_ERR("Grimmory", "Could not hash %s for koreader progress push", path.c_str());
    summary.progressPushFailed++;
    return;
  }

  GrimmoryKoreaderProgress progress;
  progress.document = documentHash;
  progress.progress = koPos.xpath;
  progress.percentage = koPos.percentage;
  progress.device = KOREADER_DEVICE_NAME;
  progress.deviceId = KOREADER_DEVICE_ID;

  if (GrimmoryApiClient::pushKoreaderProgress(apiSession, koUser.username, koUser.passwordMd5, progress) !=
      GrimmoryApiClient::OK) {
    LOG_ERR("Grimmory", "Failed to push koreader progress for %s", path.c_str());
    summary.progressPushFailed++;
    return;
  }

  LOG_INF("Grimmory", "Pushed koreader progress for %s: %.1f%%, xpath=\"%s\"", path.c_str(),
          (double)(koPos.percentage * 100.0f), koPos.xpath.c_str());
  summary.progressPushed++;
}

// Called once per book (gated by sidecar.initialProgressPulled in runSync,
// not by "just matched this run" — a book matched in an earlier sync still
// needs exactly one pull attempt on whatever its next sync is). Mirrors
// KOReaderSyncActivity's "furthest progress wins" behavior: if the server
// already has koreader-sync progress for this book (e.g. from a
// KOReader/other Grimmory client, or a previous device) that is further
// along than whatever is saved locally, pull it down and overwrite the
// local progress.bin rather than silently leaving the device behind.
//
// Returns true once a *determinate* answer came back from the server (found
// remote progress, or definitively none exists) so the caller can mark this
// book's one-time check done; returns false on a transient network/auth/
// server failure so runSync leaves the flag unset and this retries next
// sync instead of the book silently never getting checked.
bool pullRemoteProgressIfAhead(GrimmoryApiSession& apiSession, const GrimmoryKoreaderUser& koUser,
                               const std::string& path) {
  const std::string documentHash = KOReaderDocumentId::calculate(path);
  if (documentHash.empty()) {
    LOG_ERR("Grimmory", "Could not hash %s for initial koreader progress pull", path.c_str());
    return false;
  }

  GrimmoryKoreaderProgress remote;
  const auto result =
      GrimmoryApiClient::pullKoreaderProgress(apiSession, koUser.username, koUser.passwordMd5, documentHash, remote);
  if (result == GrimmoryApiClient::NOT_FOUND) {
    LOG_INF("Grimmory", "No remote koreader progress yet for %s; nothing to pull", path.c_str());
    return true;
  }
  if (result != GrimmoryApiClient::OK) {
    LOG_ERR("Grimmory", "Failed to pull remote koreader progress for %s", path.c_str());
    return false;
  }

  auto epub = std::make_shared<Epub>(path, EPUB_CACHE_DIR);
  if (!epub->load(false, true)) {
    LOG_ERR("Grimmory", "Could not load %s to apply pulled remote progress", path.c_str());
    return true;  // remote answer was determinate even though we can't act on it locally
  }

  // Reuse toKOReader (the same mapping pushKoreaderProgressForBook uses) so
  // "local percentage" is computed on the identical book-wide scale as the
  // remote percentage below, not compared page-by-page across two different
  // devices' local spine/page numbers, which mean nothing without a shared scale.
  const KOReaderPosition localKoPos = loadLocalKoreaderPosition(epub);
  const bool hasLocal = localKoPos.valid;

  if (hasLocal && remote.percentage <= localKoPos.percentage + SAME_PROGRESS_EPSILON) {
    LOG_INF("Grimmory", "Local progress for %s (%.1f%%) already at/ahead of remote (%.1f%%); not pulling", path.c_str(),
            (double)(localKoPos.percentage * 100.0f), (double)(remote.percentage * 100.0f));
    return true;
  }

  if (!applyRemoteKoreaderProgress(epub, path, remote)) return true;

  LOG_INF("Grimmory", "Applied remote progress for %s: %.1f%% (was %.1f%% locally)", path.c_str(),
          (double)(remote.percentage * 100.0f), (double)(localKoPos.percentage * 100.0f));
  return true;
}

// The "Sync to Grimmory" reader hotkey's single-book comparison step (see
// runSync's resolveSingleBookProgress parameter). Unlike
// pullRemoteProgressIfAhead (a one-time-per-book check), this runs every
// time the hotkey is used while a book is open, and can resolve in either
// direction — pulling remote into progress.bin *or* pushing local via
// koreader-sync — per GrimmoryCredentialStore::getSyncBehavior().
void resolveSingleBookProgressNow(GrimmoryApiSession& apiSession, const GrimmoryKoreaderUser& koUser,
                                  const std::string& path, GrimmorySyncSummary& summary) {
  const std::string documentHash = KOReaderDocumentId::calculate(path);
  if (documentHash.empty()) {
    LOG_ERR("Grimmory", "Could not hash %s for progress resolution", path.c_str());
    summary.singleBookProgressOutcome = GrimmoryProgressOutcome::FAILED;
    return;
  }

  GrimmoryKoreaderProgress remote;
  const auto result =
      GrimmoryApiClient::pullKoreaderProgress(apiSession, koUser.username, koUser.passwordMd5, documentHash, remote);
  if (result != GrimmoryApiClient::OK && result != GrimmoryApiClient::NOT_FOUND) {
    LOG_ERR("Grimmory", "Failed to fetch remote progress for %s", path.c_str());
    summary.singleBookProgressOutcome = GrimmoryProgressOutcome::FAILED;
    return;
  }
  const bool hasRemote = result == GrimmoryApiClient::OK;

  auto epub = std::make_shared<Epub>(path, EPUB_CACHE_DIR);
  if (!epub->load(false, true)) {
    LOG_ERR("Grimmory", "Could not load %s to resolve progress", path.c_str());
    summary.singleBookProgressOutcome = GrimmoryProgressOutcome::FAILED;
    return;
  }

  const KOReaderPosition localKoPos = loadLocalKoreaderPosition(epub);
  summary.singleBookLocalPercent = localKoPos.valid ? localKoPos.percentage * 100.0f : 0.0f;
  summary.singleBookRemotePercent = hasRemote ? remote.percentage * 100.0f : 0.0f;
  summary.singleBookRemoteDevice = hasRemote ? remote.device : std::string();

  if (!hasRemote) {
    // No remote koreader-sync progress at all yet — nothing to compare
    // against or ask about; just push local so the server has something.
    pushKoreaderProgressForBook(apiSession, koUser, path, summary);
    summary.singleBookProgressOutcome = GrimmoryProgressOutcome::PUSHED_LOCAL;
    return;
  }

  const float delta = (localKoPos.valid ? localKoPos.percentage : 0.0f) - remote.percentage;
  if (std::fabs(delta) <= SAME_PROGRESS_EPSILON) {
    summary.singleBookProgressOutcome = GrimmoryProgressOutcome::NO_CHANGE;
    return;
  }

  if (GRIMMORY_STORE.getSyncBehavior() == GrimmorySyncBehavior::ASK_EVERY_TIME) {
    // Caller (GrimmorySyncActivity) shows a choice screen and later calls
    // applySingleBookProgressChoice() with the user's decision.
    summary.singleBookProgressOutcome = GrimmoryProgressOutcome::ASK_REQUIRED;
    return;
  }

  // SMART: auto-resolve to whichever side is furthest ahead.
  if (delta > 0) {
    pushKoreaderProgressForBook(apiSession, koUser, path, summary);
    summary.singleBookProgressOutcome = GrimmoryProgressOutcome::PUSHED_LOCAL;
  } else if (applyRemoteKoreaderProgress(epub, path, remote)) {
    summary.singleBookProgressOutcome = GrimmoryProgressOutcome::APPLIED_REMOTE;
  } else {
    summary.singleBookProgressOutcome = GrimmoryProgressOutcome::FAILED;
  }
}

// Ensures the sidecar's identifiers are populated, extracting them from the
// EPUB's content.opf the first time a book is seen without any. Does not
// touch CrossInk's own BookMetadataCache/book.bin.
void ensureIdentifiers(const std::string& bookPath, GrimmoryBookSidecar& sidecar) {
  if (sidecar.hasIdentifiers()) {
    LOG_INF("Grimmory", "%s already has identifiers: isbn10=\"%s\" isbn13=\"%s\" asin=\"%s\"", bookPath.c_str(),
            sidecar.isbn10.c_str(), sidecar.isbn13.c_str(), sidecar.asin.c_str());
    return;
  }

  Epub epub(bookPath, EPUB_CACHE_DIR);
  std::string isbn10, isbn13, asin;
  if (!epub.extractIdentifiers(isbn10, isbn13, asin)) {
    LOG_ERR("Grimmory", "Could not read content.opf to extract identifiers from %s", bookPath.c_str());
    return;
  }
  sidecar.isbn10 = isbn10;
  sidecar.isbn13 = isbn13;
  sidecar.asin = asin;
  LOG_INF("Grimmory", "%s extracted identifiers: isbn10=\"%s\" isbn13=\"%s\" asin=\"%s\"", bookPath.c_str(),
          isbn10.c_str(), isbn13.c_str(), asin.c_str());
}

// Resolves each candidate book to a Grimmory book id + shelf names, then
// invokes matchBook per remote book seen. When a target shelf is chosen,
// GrimmoryApiClient::getShelfBooks scopes the fetch server-side (one
// request, already filtered); with "All Shelves" there is no such endpoint,
// so this pages through the entire catalog instead (BOOKS_PAGE_SIZE books at
// a time) and matchBook runs against every book unfiltered.
void resolveBookMatches(GrimmoryApiSession& apiSession, const std::string& accessToken,
                        std::unordered_map<std::string, GrimmoryBookSidecar>& sidecarsByPath,
                        GrimmorySyncSummary& summary) {
  std::vector<GrimmoryShelf> shelves;
  if (GrimmoryApiClient::getShelves(apiSession, accessToken, shelves) != GrimmoryApiClient::OK) {
    LOG_ERR("Grimmory", "Failed to fetch shelves");
    return;
  }
  std::unordered_map<int64_t, std::string> shelfNameById;
  for (const auto& shelf : shelves) {
    shelfNameById[shelf.id] = shelf.name;
  }

  // identifier value -> local book paths that carry it (a book may be
  // registered under more than one of isbn10/isbn13/asin).
  std::unordered_map<std::string, std::vector<std::string>> pathsByIdentifier;
  for (auto& [path, sidecar] : sidecarsByPath) {
    ensureIdentifiers(path, sidecar);
    if (!sidecar.isbn10.empty()) pathsByIdentifier[sidecar.isbn10].push_back(path);
    if (!sidecar.isbn13.empty()) pathsByIdentifier[sidecar.isbn13].push_back(path);
    if (!sidecar.asin.empty()) pathsByIdentifier[sidecar.asin].push_back(path);
  }
  if (pathsByIdentifier.empty()) {
    LOG_INF("Grimmory", "No local books have identifiers to match against; skipping shelf/catalog fetch");
    return;  // nothing we could ever match
  }

  std::unordered_set<std::string> matchedPaths;
  const auto matchBook = [&](const GrimmoryBookSummary& remoteBook) {
    const char* identifiers[] = {remoteBook.isbn10.c_str(), remoteBook.isbn13.c_str(), remoteBook.asin.c_str()};
    bool matched = false;
    for (const char* identifier : identifiers) {
      if (!identifier || identifier[0] == '\0') continue;
      const auto it = pathsByIdentifier.find(identifier);
      if (it == pathsByIdentifier.end()) continue;

      matched = true;
      for (const auto& path : it->second) {
        GrimmoryBookSidecar& sidecar = sidecarsByPath.at(path);
        sidecar.grimmoryBookId = remoteBook.id;
        sidecar.remotePageCount = remoteBook.pageCount;
        sidecar.shelves.clear();
        for (const int64_t shelfId : remoteBook.shelfIds) {
          const auto nameIt = shelfNameById.find(shelfId);
          if (nameIt != shelfNameById.end()) sidecar.shelves.push_back(nameIt->second);
        }
        if (matchedPaths.insert(path).second) summary.booksMatched++;
      }
    }
    LOG_INF("Grimmory", "Remote book %lld: isbn10=\"%s\" isbn13=\"%s\" asin=\"%s\" -> %s", (long long)remoteBook.id,
            remoteBook.isbn10.c_str(), remoteBook.isbn13.c_str(), remoteBook.asin.c_str(),
            matched ? "matched" : "no local match");
  };

  const int64_t targetShelfId = GRIMMORY_STORE.getTargetShelfId();
  if (targetShelfId >= 0) {
    if (GrimmoryApiClient::getShelfBooks(apiSession, accessToken, targetShelfId, matchBook) != GrimmoryApiClient::OK) {
      LOG_ERR("Grimmory", "Failed to fetch books for shelf %lld", (long long)targetShelfId);
    }
    return;
  }

  int page = 0;
  int seenBooks = 0;
  for (;;) {
    int totalElements = 0;
    int booksOnPage = 0;
    const auto pageResult = GrimmoryApiClient::getBooksPage(apiSession, accessToken, page, BOOKS_PAGE_SIZE,
                                                            totalElements, [&](const GrimmoryBookSummary& remoteBook) {
                                                              booksOnPage++;
                                                              matchBook(remoteBook);
                                                            });

    if (pageResult != GrimmoryApiClient::OK) {
      LOG_ERR("Grimmory", "Failed to fetch books page %d", page);
      break;
    }
    if (booksOnPage == 0) break;

    seenBooks += booksOnPage;
    page++;
    if (seenBooks >= totalElements) break;
  }
}

// Uploads every pending session for an already-matched book that carries a
// real startTimeUtc (see GrimmoryPendingSession) individually, using each
// session's own real start/end time and progress/page delta — no combining,
// no backdating, since these devices had trustworthy RTC time when the
// session was recorded. Returns the summed duration of sessions it failed to
// sync (still left in sidecar.pendingSessions) via outRemaining; sidecar's
// pendingSessions ends up containing only the sessions this call could not
// sync (real-timestamp failures) plus every legacy (startTimeUtc == 0)
// session, which drainLegacySessions handles next.
void drainRealTimestampSessions(GrimmoryApiSession& apiSession, const std::string& accessToken,
                                GrimmoryBookSidecar& sidecar, const std::string& path,
                                GrimmorySyncSummary& summary, bool& outAnySynced) {
  std::vector<GrimmoryPendingSession> remaining;
  remaining.reserve(sidecar.pendingSessions.size());

  for (const auto& session : sidecar.pendingSessions) {
    if (session.startTimeUtc == 0) {
      remaining.push_back(session);
      continue;
    }

    GrimmoryReadingSession request;
    request.grimmoryBookId = sidecar.grimmoryBookId;
    request.startTime = session.startTimeUtc;
    request.endTime = session.startTimeUtc + static_cast<int64_t>(session.durationSeconds);
    request.startProgress = session.startProgress;
    request.endProgress = session.endProgress;
    request.startPage = session.startPage;
    request.endPage = session.endPage;

    if (GrimmoryApiClient::postReadingSession(apiSession, accessToken, request) != GrimmoryApiClient::OK) {
      LOG_ERR("Grimmory", "Failed to sync reading session for %s (%u sec at %lld)", path.c_str(),
              (unsigned)session.durationSeconds, (long long)session.startTimeUtc);
      summary.sessionsFailed++;
      remaining.push_back(session);
      continue;
    }

    LOG_INF("Grimmory", "Synced reading session for %s: %u sec at %lld, progress %.1f%%->%.1f%%", path.c_str(),
            (unsigned)session.durationSeconds, (long long)session.startTimeUtc, session.startProgress,
            session.endProgress);
    summary.sessionsSynced++;
    outAnySynced = true;
  }

  sidecar.pendingSessions = std::move(remaining);
}

// Combines every remaining pending session for an already-matched book (all
// of which carry no real timestamp — see GrimmoryPendingSession, and
// drainRealTimestampSessions above which already synced/removed any that
// did) into ONE upload, backdated from "now" by the summed duration. Many
// devices have no battery-backed RTC and even a synced clock drifts over
// long unpowered stretches, so there is nothing trustworthy to reconstruct
// per-session for these; only the total duration and the overall
// progress/page delta across everything queued since the last sync are
// meaningful. "now" is whatever runSync() established just before calling
// this (freshly NTP-synced where possible).
void drainLegacySessions(GrimmoryApiSession& apiSession, const std::string& accessToken,
                         GrimmoryBookSidecar& sidecar, const std::string& path, GrimmorySyncSummary& summary,
                         int64_t now, bool& outAnySynced) {
  if (sidecar.pendingSessions.empty()) return;

  uint32_t totalDuration = 0;
  for (const auto& session : sidecar.pendingSessions) {
    totalDuration += session.durationSeconds;
  }

  GrimmoryReadingSession request;
  request.grimmoryBookId = sidecar.grimmoryBookId;
  request.endTime = now;
  request.startTime = now - static_cast<int64_t>(totalDuration);
  request.startProgress = sidecar.pendingSessions.front().startProgress;
  request.endProgress = sidecar.pendingSessions.back().endProgress;
  request.startPage = sidecar.pendingSessions.front().startPage;
  request.endPage = sidecar.pendingSessions.back().endPage;

  const size_t sessionsCombined = sidecar.pendingSessions.size();
  if (GrimmoryApiClient::postReadingSession(apiSession, accessToken, request) != GrimmoryApiClient::OK) {
    LOG_ERR("Grimmory", "Failed to sync aggregated reading session for %s (%u queued session(s), %u sec)",
            path.c_str(), (unsigned)sessionsCombined, (unsigned)totalDuration);
    summary.sessionsFailed += static_cast<int>(sessionsCombined);
    return;
  }

  LOG_INF(
      "Grimmory", "Synced aggregated reading session for %s: %u queued session(s) -> %u sec, progress %.1f%%->%.1f%%",
      path.c_str(), (unsigned)sessionsCombined, (unsigned)totalDuration, request.startProgress, request.endProgress);
  sidecar.pendingSessions.clear();
  summary.sessionsSynced += static_cast<int>(sessionsCombined);
  outAnySynced = true;
}

// Drains a book's pending sessions in two passes: real-timestamp sessions
// (RTC-equipped devices, e.g. X3/X4Pro) upload individually with their true
// times, then whatever remains (legacy/no-RTC, e.g. X4) is combined into one
// backdated aggregate as before. A book counts as "synced" (added to
// outSyncedPaths, for the koreader-progress push below) if either pass
// uploaded anything.
void drainPendingSessions(GrimmoryApiSession& apiSession, const std::string& accessToken,
                          std::unordered_map<std::string, GrimmoryBookSidecar>& sidecarsByPath,
                          GrimmorySyncSummary& summary, int64_t now, std::vector<std::string>& outSyncedPaths) {
  for (auto& [path, sidecar] : sidecarsByPath) {
    if (sidecar.pendingSessions.empty() || !sidecar.isMatched()) continue;

    bool anySynced = false;
    drainRealTimestampSessions(apiSession, accessToken, sidecar, path, summary, anySynced);
    drainLegacySessions(apiSession, accessToken, sidecar, path, summary, now, anySynced);

    if (anySynced) outSyncedPaths.push_back(path);
  }
}
}  // namespace

GrimmorySyncSummary GrimmorySyncEngine::runSync(const std::vector<std::string>& bookPaths,
                                                const bool resolveSingleBookProgress) {
  GrimmorySyncSummary summary;

  // One connection for the whole run: login, every shelves/books-page
  // request, and every reading-session upload all reuse it instead of
  // paying a fresh wolfSSL TLS 1.3 handshake per request. A paginated
  // catalog walk can be dozens of requests; handshaking that many times in a
  // row previously fragmented/exhausted the C3's heap over the run.
  GrimmoryApiSession apiSession;

  // Pending sessions carry a duration only, not an absolute timestamp (see
  // GrimmoryPendingSession), and get backdated from "now" once drained
  // below. Sync time here so "now" is accurate even on RTC-less hardware
  // (syncSystemTimeFromNTP only needs WiFi, which this sync run already
  // has) — mirrors KOReaderSyncActivity's syncTimeWithNTP() call.
#ifndef SIMULATOR
  if (!halClock.syncSystemTimeFromNTP()) {
    LOG_ERR("Grimmory", "NTP time sync failed; reading-session timestamps may be inaccurate");
  }
#endif

  std::string accessToken;
  summary.loginError = GrimmoryApiClient::login(apiSession, accessToken);
  if (summary.loginError != GrimmoryApiClient::OK) {
    LOG_ERR("Grimmory", "Login failed: %s", GrimmoryApiClient::errorString(summary.loginError).c_str());
    return summary;
  }

  std::unordered_map<std::string, GrimmoryBookSidecar> sidecarsByPath;
  for (const auto& path : bookPaths) {
    sidecarsByPath.emplace(path, GrimmoryBookSidecar::load(path));
  }
  LOG_INF("Grimmory", "Loaded %u candidate sidecar(s) from RecentBooksStore", (unsigned)sidecarsByPath.size());

  if (GRIMMORY_STORE.getSyncShelvesEnabled()) {
    resolveBookMatches(apiSession, accessToken, sidecarsByPath, summary);
  }
  if (GRIMMORY_STORE.getSyncStatsEnabled()) {
    // Matched books that haven't had their one-time "pull server progress if
    // it's further along" check yet (see GrimmoryBookSidecar::initialProgressPulled).
    // Gated on the persisted flag rather than "matched for the first time
    // this run": a book matched in an earlier sync (before this flag
    // existed, or simply on a prior run) still needs exactly one pull
    // attempt, on whichever sync run first sees it matched with the flag
    // unset — it does not have to be the same run that matched it.
    // A book targeted by resolveSingleBookProgress takes the newer,
    // behavior-aware comparison below instead of the older one-time silent
    // pull — running both would double the network round trips for the same
    // book on the same sync.
    const bool hasSingleBookTarget = resolveSingleBookProgress && bookPaths.size() == 1;
    const std::string singleBookPath = hasSingleBookTarget ? bookPaths.front() : std::string();

    std::vector<std::string> pullCandidatePaths;
    for (auto& [path, sidecar] : sidecarsByPath) {
      if (sidecar.isMatched() && !sidecar.initialProgressPulled && path != singleBookPath) {
        pullCandidatePaths.push_back(path);
      }
    }

    std::vector<std::string> syncedPaths;
    drainPendingSessions(apiSession, accessToken, sidecarsByPath, summary, static_cast<int64_t>(time(nullptr)),
                         syncedPaths);

    const bool needsSingleBookResolve =
        hasSingleBookTarget && sidecarsByPath.count(singleBookPath) && sidecarsByPath.at(singleBookPath).isMatched();

    // The koreader-sync account is needed to pull a not-yet-checked book's
    // remote progress, push local progress for books that just had a
    // session synced above, and/or resolve the single-book hotkey's
    // progress comparison, so prepare it once and reuse it for all three.
    if (!pullCandidatePaths.empty() || !syncedPaths.empty() || needsSingleBookResolve) {
      GrimmoryKoreaderUser koUser;
      if (ensureKoreaderSyncReady(apiSession, accessToken, koUser)) {
        for (const auto& path : pullCandidatePaths) {
          if (pullRemoteProgressIfAhead(apiSession, koUser, path)) {
            sidecarsByPath.at(path).initialProgressPulled = true;
          }
        }
        for (const auto& path : syncedPaths) {
          pushKoreaderProgressForBook(apiSession, koUser, path, summary);
        }
        if (needsSingleBookResolve) {
          resolveSingleBookProgressNow(apiSession, koUser, singleBookPath, summary);
          if (summary.singleBookProgressOutcome != GrimmoryProgressOutcome::FAILED) {
            sidecarsByPath.at(singleBookPath).initialProgressPulled = true;
          }
        }
      } else {
        LOG_ERR("Grimmory", "Skipping koreader progress pull/push/resolve: could not prepare koreader-sync account");
      }
    }
  }

  LOG_INF("Grimmory", "Saving %u sidecar(s)", (unsigned)sidecarsByPath.size());
  for (const auto& [path, sidecar] : sidecarsByPath) {
    sidecar.save(path);
  }

  return summary;
}

bool GrimmorySyncEngine::applySingleBookProgressChoice(const std::string& bookPath, const bool applyRemote) {
  GrimmoryApiSession apiSession;
  std::string accessToken;
  const auto loginResult = GrimmoryApiClient::login(apiSession, accessToken);
  if (loginResult != GrimmoryApiClient::OK) {
    LOG_ERR("Grimmory", "Login failed applying progress choice: %s",
            GrimmoryApiClient::errorString(loginResult).c_str());
    return false;
  }

  GrimmoryKoreaderUser koUser;
  if (!ensureKoreaderSyncReady(apiSession, accessToken, koUser)) {
    LOG_ERR("Grimmory", "Could not prepare koreader-sync account to apply progress choice");
    return false;
  }

  GrimmorySyncSummary throwawaySummary;  // pushKoreaderProgressForBook needs somewhere to count into
  if (!applyRemote) {
    pushKoreaderProgressForBook(apiSession, koUser, bookPath, throwawaySummary);
    return throwawaySummary.progressPushed > 0;
  }

  const std::string documentHash = KOReaderDocumentId::calculate(bookPath);
  if (documentHash.empty()) {
    LOG_ERR("Grimmory", "Could not hash %s to apply progress choice", bookPath.c_str());
    return false;
  }
  GrimmoryKoreaderProgress remote;
  if (GrimmoryApiClient::pullKoreaderProgress(apiSession, koUser.username, koUser.passwordMd5, documentHash, remote) !=
      GrimmoryApiClient::OK) {
    LOG_ERR("Grimmory", "Failed to re-fetch remote progress to apply for %s", bookPath.c_str());
    return false;
  }

  auto epub = std::make_shared<Epub>(bookPath, EPUB_CACHE_DIR);
  if (!epub->load(false, true)) {
    LOG_ERR("Grimmory", "Could not load %s to apply progress choice", bookPath.c_str());
    return false;
  }
  return applyRemoteKoreaderProgress(epub, bookPath, remote);
}
