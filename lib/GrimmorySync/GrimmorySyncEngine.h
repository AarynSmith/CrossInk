#pragma once
#include <string>
#include <vector>

#include "GrimmoryApiClient.h"

// Outcome of the single-book progress comparison step (see runSync's
// resolveSingleBookProgress parameter and GrimmorySyncSummary::
// singleBookProgressOutcome below). Only ever populated when runSync() is
// called with resolveSingleBookProgress=true and exactly one book path.
enum class GrimmoryProgressOutcome {
  NONE,            // resolveSingleBookProgress was false, or nothing to resolve (login/match failed).
  NO_CHANGE,       // Local and remote already agree (within epsilon).
  APPLIED_REMOTE,  // SMART behavior: remote was further along; pulled into progress.bin.
  PUSHED_LOCAL,    // SMART behavior (or no remote progress existed yet): local was further along; pushed.
  ASK_REQUIRED,    // ASK_EVERY_TIME behavior and the two differ: caller must show a choice and
                   // call GrimmorySyncEngine::applySingleBookProgressChoice().
  FAILED,          // Network/parse error while comparing.
};

// Result of one manual "Sync Now" run, for the summary screen.
struct GrimmorySyncSummary {
  GrimmoryApiClient::Error loginError = GrimmoryApiClient::OK;
  int booksMatched = 0;
  int sessionsSynced = 0;
  int sessionsFailed = 0;
  // Koreader-sync-protocol progress pushes (see GrimmorySyncEngine.cpp's
  // pushKoreaderProgressForBook), attempted once per book that just had a
  // reading session synced above. When the koreader-sync account has "sync
  // progress with web reader" enabled (auto-enabled the first time this
  // runs), this is what actually updates Grimmory's own native/web-reader
  // progress, not just its separate "KOReader Progress" field.
  int progressPushed = 0;
  int progressPushFailed = 0;

  // Populated only when runSync() was called with resolveSingleBookProgress=true
  // (the "Sync to Grimmory" reader hotkey's single-book path). See
  // GrimmoryProgressOutcome for what each value means.
  GrimmoryProgressOutcome singleBookProgressOutcome = GrimmoryProgressOutcome::NONE;
  float singleBookLocalPercent = 0.0f;   // 0-100. Valid whenever outcome != NONE/FAILED.
  float singleBookRemotePercent = 0.0f;  // 0-100. Valid whenever outcome != NONE/FAILED/PUSHED_LOCAL(no-remote case).
  std::string singleBookRemoteDevice;    // Device name reported by the server, for the Ask screen.

  bool succeeded() const { return loginError == GrimmoryApiClient::OK; }
};

/**
 * Drives one manual Grimmory sync run: log in, then (depending on the
 * GrimmoryCredentialStore toggles) resolve local books to Grimmory shelf
 * membership and drain any not-yet-synced reading sessions.
 *
 * `bookPaths` is the caller-supplied candidate list to check — in practice
 * the caller (GrimmorySyncActivity) passes RecentBooksStore's paths, since
 * that is what a book can possibly have pending sessions or stale shelf tags
 * for (a session can only be logged by closing a book that was opened, which
 * always adds it to RecentBooksStore). This library intentionally has no
 * direct dependency on RecentBooksStore/app-level stores.
 */
class GrimmorySyncEngine {
 public:
  // `resolveSingleBookProgress`: only meaningful when `bookPaths` has exactly
  // one entry (the "Sync to Grimmory" reader hotkey's single-book path, see
  // GrimmorySyncActivity). After the usual shelf-match/session-push work,
  // additionally compares this book's current local progress against its
  // current remote koreader-sync progress and resolves the difference per
  // GrimmoryCredentialStore::getSyncBehavior() — see
  // GrimmorySyncSummary::singleBookProgressOutcome for the result. Reuses
  // this call's own login/session, so it costs no extra TLS handshake.
  static GrimmorySyncSummary runSync(const std::vector<std::string>& bookPaths, bool resolveSingleBookProgress = false);

  // Applies the user's explicit choice after a runSync() call returned
  // ASK_REQUIRED for bookPath (GrimmorySyncActivity's single-book
  // "ask every time" flow). applyRemote=true pulls the book's current
  // remote koreader-sync progress into progress.bin; false pushes local
  // progress instead. This is a separate, later call made only after the
  // user has chosen, so it logs in fresh rather than reusing runSync()'s
  // session.
  static bool applySingleBookProgressChoice(const std::string& bookPath, bool applyRemote);
};
