#include "GrimmorySyncActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <iterator>

#include "CrossPointSettings.h"
#include "GrimmorySyncEngine.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/WifiUtils.h"

void GrimmorySyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    RenderLock lock(*this);
    state = FAILED;
    errorMessage = tr(STR_GRIMMORY_WIFI_CONN_FAILED);
    requestUpdate();
    return;
  }

  sdFontSystem.releaseForNetwork(renderer);

  // Devices with an RTC (X3/X4 and X4 Pro) need a valid clock before syncing
  // so session timestamps sent to Grimmory aren't garbage. This only runs
  // once per device (same debounce flags WifiSelectionActivity's own
  // on-connect sync sets) — if WiFi was just connected through
  // WifiSelectionActivity, that path already did this; this only fires the
  // first time when WiFi was already active and that path was skipped.
  if (halClock.isAvailable() && (!SETTINGS.clockHasBeenSynced || !SETTINGS.clockDateHasBeenSynced)) {
    if (halClock.syncFromNTP()) {
      SETTINGS.clockHasBeenSynced = 1;
      SETTINGS.clockDateHasBeenSynced = 1;
      SETTINGS.saveToFile();
    }
  }

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_GRIMMORY_SYNCING);
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("Grimmory", "Sync screen could not be rendered before request");
    requestUpdate(true);
  }

  performSync();
}

void GrimmorySyncActivity::performSync() {
  std::vector<std::string> paths;
  if (!singleBookPath.empty()) {
    paths.push_back(singleBookPath);
  } else {
    RECENT_BOOKS.loadFromFile();
    const auto& books = RECENT_BOOKS.getBooks();
    paths.reserve(books.size());
    std::transform(books.begin(), books.end(), std::back_inserter(paths),
                   [](const RecentBook& book) { return book.path; });
  }

  const auto summary = GrimmorySyncEngine::runSync(paths, /*resolveSingleBookProgress=*/!singleBookPath.empty());

  RenderLock lock(*this);
  if (!summary.succeeded()) {
    state = FAILED;
    errorMessage = GrimmoryApiClient::errorString(summary.loginError);
    return;
  }

  booksMatched = summary.booksMatched;
  sessionsSynced = summary.sessionsSynced;
  sessionsFailed = summary.sessionsFailed;
  progressPushFailed = summary.progressPushFailed;

  if (!singleBookPath.empty()) {
    progressOutcome = summary.singleBookProgressOutcome;
    progressLocalPercent = summary.singleBookLocalPercent;
    progressRemotePercent = summary.singleBookRemotePercent;
    progressRemoteDevice = summary.singleBookRemoteDevice;

    // ASK_REQUIRED only ever comes back when local and remote actually
    // differ and GrimmorySyncBehavior::ASK_EVERY_TIME is set (see
    // GrimmorySyncEngine::resolveSingleBookProgress) — every other outcome,
    // including GrimmorySyncBehavior::SMART's auto-resolution, falls through
    // to the auto-return below.
    if (progressOutcome == GrimmoryProgressOutcome::ASK_REQUIRED) {
      state = SHOWING_CHOICE;
      // Default to the option matching the furthest progress, same
      // convention as KOReaderSyncActivity's Apply/Upload result screen.
      selectedOption = progressLocalPercent > progressRemotePercent ? 1 : 0;
      return;
    }
  }

  state = SUCCESS;
  if (!singleBookPath.empty()) {
    // Nothing needed the user's input — SMART resolved it automatically, or
    // local/remote already agreed — so return to the reader without waiting
    // for a Back press.
    markAutoReturn();
  }
}

void GrimmorySyncActivity::applyProgressChoice() {
  {
    RenderLock lock(*this);
    state = APPLYING_CHOICE;
    statusMessage = tr(STR_GRIMMORY_SYNCING);
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("Grimmory", "Apply-choice screen could not be rendered before request");
    requestUpdate(true);
  }

  const bool applyRemote = selectedOption == 0;
  const bool ok = GrimmorySyncEngine::applySingleBookProgressChoice(singleBookPath, applyRemote);

  RenderLock lock(*this);
  if (!ok) {
    state = FAILED;
    errorMessage = tr(STR_GRIMMORY_SERVER_ERROR);
    return;
  }
  // Ask-mode's explicit choice still waits for Back, matching this
  // activity's existing (all-books) confirmation behavior — no auto-return.
  state = SUCCESS;
}

void GrimmorySyncActivity::markAutoReturn() { autoReturnAt = millis() + AUTO_RETURN_DELAY_MS; }

Rect GrimmorySyncActivity::choiceButtonRect(const int option) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const int top = (renderer.getScreenHeight() - height) / 2;
  constexpr int buttonHeight = 40;
  constexpr int buttonGap = 8;
  const int buttonWidth = std::max(1, screen.width - metrics.contentSidePadding * 2);
  const int firstButtonY = top + (height + 10) * 4 + 10;
  return Rect{screen.x + metrics.contentSidePadding, firstButtonY + option * (buttonHeight + buttonGap), buttonWidth,
              buttonHeight};
}

void GrimmorySyncActivity::onEnter() {
  Activity::onEnter();

  // The reader uses this activity as a tiny handoff so ActivityManager can run
  // reader onExit() before rebooting. Network boot uses the bookPath constructor.
  if (restartBeforeNetwork) {
    silentRestartToNetwork(NetworkBootTarget::GRIMMORY_SYNC, /*payload=*/1);
    return;
  }

  sdFontSystem.releaseLoadedFont(renderer);

  if (hasActiveStationWifiConnection()) {
    onWifiSelectionComplete(true);
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void GrimmorySyncActivity::onExit() {
  Activity::onExit();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  if (!singleBookPath.empty()) {
    silentRestartToReader();
  } else {
    silentRestart();
  }
}

void GrimmorySyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  const Rect header{0, metrics.topPadding, pageWidth, TouchHeaderBackButton::height(metrics, mappedInput)};
  const char* title = tr(STR_GRIMMORY_SYNC);
  if ((state == SUCCESS || state == FAILED) && mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, title, false);
  } else {
    GUI.drawHeader(renderer, header, title);
  }
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == CONNECTING || state == SYNCING || state == APPLYING_CHOICE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str());
  } else if (state == SHOWING_CHOICE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_PROGRESS_FOUND), true, EpdFontFamily::BOLD);

    char remoteLine[64];
    snprintf(remoteLine, sizeof(remoteLine), "%s %.1f%%", tr(STR_REMOTE_LABEL), (double)progressRemotePercent);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, remoteLine);
    if (!progressRemoteDevice.empty()) {
      char deviceLine[64];
      snprintf(deviceLine, sizeof(deviceLine), tr(STR_DEVICE_FROM_FORMAT), progressRemoteDevice.c_str());
      renderer.drawCenteredText(UI_10_FONT_ID, top + (height + 10) * 2, deviceLine);
    }
    char localLine[64];
    snprintf(localLine, sizeof(localLine), "%s %.1f%%", tr(STR_LOCAL_LABEL), (double)progressLocalPercent);
    renderer.drawCenteredText(UI_10_FONT_ID, top + (height + 10) * 3, localLine);

    const char* actionLabels[] = {tr(STR_APPLY_REMOTE), tr(STR_UPLOAD_LOCAL)};
    for (int option = 0; option < 2; ++option) {
      const Rect button = choiceButtonRect(option);
      const bool selected = selectedOption == option;
      if (selected) renderer.fillRect(button.x, button.y, button.width, button.height);
      renderer.drawRect(button.x, button.y, button.width, button.height, true);
      const int textX = button.x + (button.width - renderer.getTextWidth(UI_10_FONT_ID, actionLabels[option])) / 2;
      const int textY = button.y + (button.height - height) / 2;
      renderer.drawText(UI_10_FONT_ID, textX, textY, actionLabels[option], !selected);
    }
  } else if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_GRIMMORY_SYNC_COMPLETE), true, EpdFontFamily::BOLD);
    char summaryLine[96];
    snprintf(summaryLine, sizeof(summaryLine), tr(STR_GRIMMORY_SYNC_SUMMARY_FORMAT), booksMatched, sessionsSynced);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, summaryLine);
    int summaryLineIndex = 1;
    if (sessionsFailed > 0) {
      char failedLine[64];
      snprintf(failedLine, sizeof(failedLine), tr(STR_GRIMMORY_SESSIONS_RETRY_FORMAT), sessionsFailed);
      renderer.drawCenteredText(UI_10_FONT_ID, top + (height + 10) * (++summaryLineIndex), failedLine);
    }
    if (progressPushFailed > 0) {
      char progressFailedLine[64];
      snprintf(progressFailedLine, sizeof(progressFailedLine), tr(STR_GRIMMORY_PROGRESS_PUSH_FAILED_FORMAT),
               progressPushFailed);
      renderer.drawCenteredText(UI_10_FONT_ID, top + (height + 10) * (++summaryLineIndex), progressFailedLine);
    }
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_GRIMMORY_SYNC_FAILED), true, EpdFontFamily::BOLD);
    const int messageWidth = screen.width - metrics.contentSidePadding * 2;
    const auto errorLines = renderer.wrappedText(UI_10_FONT_ID, errorMessage.c_str(), messageWidth, 3);
    int messageY = top + height + 10;
    for (const auto& line : errorLines) {
      UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, messageY, line.c_str());
      messageY += height + 4;
    }
  }

  const auto labels = (state == SHOWING_CHOICE)
                          ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN))
                          : mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
}

void GrimmorySyncActivity::loop() {
  if (state == SUCCESS || state == FAILED) {
    if (autoReturnAt != 0 && millis() >= autoReturnAt) {
      finish();
      return;
    }

    const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
    if (TouchHeaderBackButton::wasTapped(mappedInput, header) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finishAfterBackPress();
      return;
    }

    int x = 0;
    int y = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      finish();
    }
    return;
  }

  if (state == SHOWING_CHOICE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finishAfterBackPress();
      return;
    }

    int x = 0;
    int y = 0;
    if (mappedInput.wasScreenTapped(x, y)) {
      for (int option = 0; option < 2; ++option) {
        const Rect button = choiceButtonRect(option);
        if (x >= button.x && x < button.x + button.width && y >= button.y && y < button.y + button.height) {
          selectedOption = option;
          applyProgressChoice();
          return;
        }
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % 2;
      requestUpdate();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      applyProgressChoice();
    }
  }
}
