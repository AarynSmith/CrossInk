#pragma once

#include "GrimmorySyncEngine.h"
#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"

struct Rect;

/**
 * Runs one manual "Sync Now" pass against the Grimmory server: connect to
 * WiFi, then log in and run GrimmorySyncEngine::runSync() over either every
 * book in RecentBooksStore (default constructor — Settings' "Sync Now" and
 * the "sync everything" hotkey outside the reader) or a single book (the
 * bookPath constructor — the hotkey pressed while a book is open). Modeled
 * on KOReaderAuthActivity's flow (this app's convention for network-touching
 * activities is to run them after a minimal "network boot" — see
 * SilentRestart.h / NetworkBootTarget::GRIMMORY_SYNC).
 */
class GrimmorySyncActivity final : public Activity {
 public:
  static constexpr const char* NAME = "GrimmorySync";

  // Tag type for the reader-handoff constructor below.
  struct ReaderHandoff {};

  explicit GrimmorySyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity(NAME, renderer, mappedInput) {}

  // Single-book sync, path already known — used directly after the network
  // boot once bookPath has been resolved from APP_STATE.openEpubPath (see
  // main.cpp's NetworkBootTarget::GRIMMORY_SYNC resume case).
  GrimmorySyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath)
      : Activity(NAME, renderer, mappedInput), singleBookPath(std::move(bookPath)) {}

  // Handoff-only: constructed pre-reboot from inside the reader so
  // ActivityManager's replaceActivity() runs EpubReaderActivity::onExit()
  // (closing the file handle, saving progress) before onEnter() below
  // performs the actual network-boot reboot. Mirrors
  // KOReaderSyncActivity's identical handoff pattern. The book path itself
  // isn't threaded through here — it's re-resolved from
  // APP_STATE.openEpubPath after reboot, same as the bookPath constructor.
  GrimmorySyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, ReaderHandoff)
      : Activity(NAME, renderer, mappedInput), restartBeforeNetwork(true) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == SYNCING || state == APPLYING_CHOICE; }

 private:
  // SHOWING_CHOICE/APPLYING_CHOICE only ever apply to the single-book path,
  // when GrimmorySyncBehavior::ASK_EVERY_TIME and local/remote progress
  // actually differ (see performSync()).
  enum State { WIFI_SELECTION, CONNECTING, SYNCING, SHOWING_CHOICE, APPLYING_CHOICE, SUCCESS, FAILED };

  State state = WIFI_SELECTION;
  ScreenTransitionRefresh screenTransitionRefresh;
  std::string statusMessage;
  std::string errorMessage;
  int booksMatched = 0;
  int sessionsSynced = 0;
  int sessionsFailed = 0;
  int progressPushFailed = 0;
  // Empty means "sync every RecentBooksStore book" (the existing default
  // behavior). Non-empty means "sync just this one book" (the reader
  // hotkey), and onExit() returns to the reader instead of home.
  std::string singleBookPath;
  bool restartBeforeNetwork = false;

  // Single-book progress comparison result (see GrimmorySyncEngine::
  // resolveSingleBookProgress), used to drive the SHOWING_CHOICE screen and
  // the auto-return behavior below.
  GrimmoryProgressOutcome progressOutcome = GrimmoryProgressOutcome::NONE;
  float progressLocalPercent = 0.0f;
  float progressRemotePercent = 0.0f;
  std::string progressRemoteDevice;
  int selectedOption = 0;  // SHOWING_CHOICE: 0 = Apply Remote, 1 = Upload Local

  // Single-book syncs return to the reader without waiting for a Back press
  // whenever there was nothing to ask about — either GrimmorySyncBehavior::
  // SMART resolved things automatically, or local/remote already agreed.
  // ASK_EVERY_TIME only interrupts with SHOWING_CHOICE when the two actually
  // differ; once the user picks, that confirmation still waits for Back,
  // matching this activity's existing (all-books) behavior.
  unsigned long autoReturnAt = 0;
  static constexpr unsigned long AUTO_RETURN_DELAY_MS = 1200;
  void markAutoReturn();

  void onWifiSelectionComplete(bool success);
  void performSync();
  void applyProgressChoice();
  // SHOWING_CHOICE's two buttons (0 = Apply Remote, 1 = Upload Local).
  // Shared by render() and loop() so hit-testing always matches what's drawn.
  Rect choiceButtonRect(int option) const;
};
