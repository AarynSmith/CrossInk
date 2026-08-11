#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Owns the transport connection for one Grimmory sync run (login through the
// last reading-session upload), so the whole run pays for one TLS handshake
// instead of one per request. A books catalog paged 20-at-a-time can be
// dozens of requests; tearing down and re-handshaking wolfSSL TLS 1.3 for
// each one previously fragmented/exhausted the C3's heap over the run even
// though any single handshake succeeded. Construct one per sync run and pass
// it to every GrimmoryApiClient call in that run; destroying it closes the
// connection. The concrete transport (freeink::SecureHttpClient on device,
// Arduino HTTPClient in the simulator) is hidden here so this header stays
// platform-independent.
class GrimmoryApiSession {
 public:
  GrimmoryApiSession();
  ~GrimmoryApiSession();
  GrimmoryApiSession(const GrimmoryApiSession&) = delete;
  GrimmoryApiSession& operator=(const GrimmoryApiSession&) = delete;

  struct Impl;
  std::unique_ptr<Impl> impl;
};

struct GrimmoryShelf {
  int64_t id = 0;
  std::string name;
};

// Minimal per-book fields needed to match a local book to a Grimmory book
// record (by isbn10/isbn13/asin) and to know its shelf membership.
struct GrimmoryBookSummary {
  int64_t id = 0;
  std::string isbn10;
  std::string isbn13;
  std::string asin;
  std::vector<int64_t> shelfIds;
  // Server-reported page count (metadata.pageCount), 0 if the server has
  // none. Used as a fallback to estimate a book-wide page number for EPUBs
  // without local XLocations reference-page data (see
  // EpubReaderActivity::resolveGrimmoryPage).
  uint32_t pageCount = 0;
};

struct GrimmoryReadingSession {
  int64_t grimmoryBookId = 0;
  int64_t startTime = 0;
  int64_t endTime = 0;
  float startProgress = 0.0f;
  float endProgress = 0.0f;
  uint32_t startPage = 0;
  uint32_t endPage = 0;
};

// The user's koreader-sync account on the Grimmory server (GET/PUT
// /api/v1/koreader-users/me) — a separate credential pair from the main
// Grimmory login, used to authenticate the koreader-sync-protocol progress
// push below. `syncEnabled` gates whether koreader-sync is active at all;
// `syncWithWebReader` gates whether a pushed koreader position also updates
// Grimmory's own native/web-reader progress (an XPointer->EPUB-CFI
// conversion done server-side) — this is the flag that makes
// pushKoreaderProgress visibly update the book's Grimmory progress, not just
// its KOReader Progress field.
struct GrimmoryKoreaderUser {
  std::string username;
  std::string password;
  std::string passwordMd5;
  bool syncEnabled = false;
  bool syncWithWebReader = false;
};

// One koreader-sync-protocol progress upload. Mirrors KOReaderProgress (see
// lib/KOReaderSync/KOReaderSyncClient.h) minus the CrossPoint-only
// extensions, which must never be sent to a third-party server like
// Grimmory.
struct GrimmoryKoreaderProgress {
  std::string document;     // KOReader partial-MD5 document hash (KOReaderDocumentId::calculate)
  std::string progress;     // XPath-like position string (ProgressMapper::toKOReader)
  float percentage = 0.0f;  // 0.0 - 1.0, NOT 0-100
  std::string device;
  std::string deviceId;
};

/**
 * HTTP client for the Grimmory server REST API.
 *
 * Authentication: POST /api/v1/auth/login returns a JWT accessToken, sent as
 * `Authorization: Bearer <token>` on subsequent requests. Unlike the KOReader
 * plugin this ports, there is no refresh-token persistence: a fresh login()
 * happens at the start of every manual sync run and the token is held only
 * in memory for that run (see GrimmorySyncActivity).
 */
class GrimmoryApiClient {
 public:
  enum Error {
    OK = 0,
    NO_CREDENTIALS,
    NETWORK_ERROR,
    AUTH_FAILED,
    SERVER_ERROR,
    JSON_ERROR,
    LOW_MEMORY,
    NOT_FOUND,
  };

  // Logs in with the credentials in GrimmoryCredentialStore, over
  // `apiSession`'s connection. On success, outAccessToken holds the bearer
  // token to pass to the calls below.
  static Error login(GrimmoryApiSession& apiSession, std::string& outAccessToken);

  static Error getShelves(GrimmoryApiSession& apiSession, const std::string& accessToken,
                          std::vector<GrimmoryShelf>& out);

  // Fetches one page (0-indexed, pageSize items) of
  // GET /api/v1/books/page?sort=addedOn&page=N&size=pageSize, invoking onBook
  // once per book as the response is parsed and setting outTotalElements from
  // the page envelope. Streams books to the callback rather than collecting
  // them into a vector, so a page's peak memory is one JsonDocument plus one
  // transient GrimmoryBookSummary rather than a second full copy of the page.
  static Error getBooksPage(GrimmoryApiSession& apiSession, const std::string& accessToken, int page, int pageSize,
                            int& outTotalElements, const std::function<void(const GrimmoryBookSummary&)>& onBook);

  // Fetches every book on shelf `shelfId` via GET /api/v1/shelves/<id>/books.
  // Unlike getBooksPage, this returns a flat JSON array (no "content"/"page"
  // envelope, no pagination) and is scoped server-side, so it's the right
  // call whenever sync is restricted to one shelf: one request instead of
  // walking every page of the whole catalog and filtering client-side.
  static Error getShelfBooks(GrimmoryApiSession& apiSession, const std::string& accessToken, int64_t shelfId,
                             const std::function<void(const GrimmoryBookSummary&)>& onBook);

  static Error postReadingSession(GrimmoryApiSession& apiSession, const std::string& accessToken,
                                  const GrimmoryReadingSession& session);

  // GET /api/v1/koreader-users/me — the koreader-sync account tied to the
  // main (JWT) login. Returns an all-default GrimmoryKoreaderUser (empty
  // username) if the user has never configured one.
  static Error getKoreaderUser(GrimmoryApiSession& apiSession, const std::string& accessToken,
                               GrimmoryKoreaderUser& out);

  // PUT /api/v1/koreader-users/me — creates or replaces the koreader-sync
  // username/password.
  static Error setKoreaderCredentials(GrimmoryApiSession& apiSession, const std::string& accessToken,
                                      const std::string& username, const std::string& password);

  // PATCH /api/v1/koreader-users/me/sync?enabled=
  static Error setKoreaderSyncEnabled(GrimmoryApiSession& apiSession, const std::string& accessToken, bool enabled);

  // PATCH /api/v1/koreader-users/me/sync-progress-with-grimmory?enabled= —
  // the flag that makes a koreader-sync progress push also update Grimmory's
  // native/web-reader progress.
  static Error setKoreaderSyncWithWebReader(GrimmoryApiSession& apiSession, const std::string& accessToken,
                                            bool enabled);

  // PUT /api/koreader/syncs/progress. Authenticated with the koreader-sync
  // x-auth-user/x-auth-key headers (from GrimmoryKoreaderUser), NOT the JWT
  // bearer token used by every other call in this client — this is a
  // separate, koreader-protocol-compatible endpoint under a different path
  // prefix on the same server.
  static Error pushKoreaderProgress(GrimmoryApiSession& apiSession, const std::string& koreaderUsername,
                                    const std::string& koreaderPasswordMd5, const GrimmoryKoreaderProgress& progress);

  // GET /api/koreader/syncs/progress/<document>. Same koreader-sync
  // x-auth-user/x-auth-key auth as pushKoreaderProgress. Returns NOT_FOUND
  // when the server has no progress on record for this document hash yet
  // (a brand new koreader-sync account, or a book never opened anywhere
  // else) rather than treating that as a hard failure.
  static Error pullKoreaderProgress(GrimmoryApiSession& apiSession, const std::string& koreaderUsername,
                                    const std::string& koreaderPasswordMd5, const std::string& documentHash,
                                    GrimmoryKoreaderProgress& out);

  static std::string errorString(Error error);

  /** HTTP status code from the last request (for diagnostics). */
  static int lastHttpCode;
};
