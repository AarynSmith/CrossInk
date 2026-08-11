#include "GrimmoryShelfSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "GrimmoryCredentialStore.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "network/WifiUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;
}  // namespace

GrimmoryShelfSelectActivity::GrimmoryShelfSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity(NAME, renderer, mappedInput), uiTarget(makeUiTarget(renderer)), app(uiTarget, uiTarget.deviceContext()) {}

void GrimmoryShelfSelectActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    RenderLock lock(*this);
    state = FAILED;
    errorMessage = tr(STR_GRIMMORY_WIFI_CONN_FAILED);
    requestUpdate();
    return;
  }

  sdFontSystem.releaseForNetwork(renderer);

  {
    RenderLock lock(*this);
    state = LOADING;
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("Grimmory", "Loading screen could not be rendered before request");
    requestUpdate(true);
  }

  loadShelves();
}

void GrimmoryShelfSelectActivity::loadShelves() {
  GrimmoryApiSession apiSession;
  std::string accessToken;
  const auto loginError = GrimmoryApiClient::login(apiSession, accessToken);
  if (loginError != GrimmoryApiClient::OK) {
    RenderLock lock(*this);
    state = FAILED;
    errorMessage = GrimmoryApiClient::errorString(loginError);
    requestUpdate();
    return;
  }

  std::vector<GrimmoryShelf> fetched;
  const auto shelvesError = GrimmoryApiClient::getShelves(apiSession, accessToken, fetched);

  RenderLock lock(*this);
  if (shelvesError != GrimmoryApiClient::OK) {
    state = FAILED;
    errorMessage = GrimmoryApiClient::errorString(shelvesError);
    requestUpdate();
    return;
  }

  shelves = std::move(fetched);
  const int64_t currentTarget = GRIMMORY_STORE.getTargetShelfId();
  selectedIndex = 0;
  for (size_t i = 0; i < shelves.size(); i++) {
    if (shelves[i].id == currentTarget) {
      selectedIndex = i + 1;
      break;
    }
  }

  state = SHOWING_LIST;
  uiReady = false;
  visibleRows = 1;
  topIndex = 0;
  app.setTheme(uiThemeTokens(uiTarget));
  app.on(ACTION_ROW, &GrimmoryShelfSelectActivity::onRowEvent, this);
  app.setScreen(&GrimmoryShelfSelectActivity::listScreen, this);
  requestUpdate();
}

void GrimmoryShelfSelectActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<GrimmoryShelfSelectActivity*>(user);
  if (event.value < 0 || event.value >= self->getItemCount()) return;
  self->selectedIndex = static_cast<size_t>(event.value);
  self->app.clearTapFlash();
  self->handleSelection();
}

void GrimmoryShelfSelectActivity::handleSelection() {
  if (selectedIndex == 0) {
    GRIMMORY_STORE.setTargetShelf(-1, "");
  } else {
    const auto& shelf = shelves[selectedIndex - 1];
    GRIMMORY_STORE.setTargetShelf(shelf.id, shelf.name);
  }
  GRIMMORY_STORE.saveToFile();
  finish();
}

void GrimmoryShelfSelectActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.releaseLoadedFont(renderer);

  if (hasActiveStationWifiConnection()) {
    onWifiSelectionComplete(true);
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void GrimmoryShelfSelectActivity::onExit() {
  Activity::onExit();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  silentRestart();
}

void GrimmoryShelfSelectActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<GrimmoryShelfSelectActivity*>(user)->buildListScreen(screen);
}

void GrimmoryShelfSelectActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const int itemCount = getItemCount();
  std::vector<fui::ListItem> items;
  items.reserve(itemCount);

  fui::ListItem allItem;
  allItem.label = tr(STR_GRIMMORY_ALL_SHELVES);
  allItem.actionValue = 0;
  items.push_back(allItem);

  for (size_t i = 0; i < shelves.size(); i++) {
    fui::ListItem item;
    item.label = shelves[i].name.c_str();
    item.actionValue = static_cast<int16_t>(i + 1);
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, itemCount);
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void GrimmoryShelfSelectActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  if (state == SHOWING_LIST) {
    const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
    if (mappedInput.hasTouchHardware()) {
      TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_GRIMMORY_SHELF_TO_SYNC), false);
    } else {
      GUI.drawHeader(renderer, header, tr(STR_GRIMMORY_SHELF_TO_SYNC));
    }

    uiReady = false;
    app.render();
    uiReady = true;

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const Rect header{0, metrics.topPadding, pageWidth, TouchHeaderBackButton::height(metrics, mappedInput)};
  if (state == FAILED && mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, tr(STR_GRIMMORY_SHELF_TO_SYNC), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_GRIMMORY_SHELF_TO_SYNC));
  }
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_GRIMMORY_LOADING_SHELVES));
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

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
}

void GrimmoryShelfSelectActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }

  if (state == FAILED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      finishAfterBackPress();
    }
    return;
  }

  if (state != SHOWING_LIST) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }

  const int itemCount = getItemCount();
  buttonNavigator.onNext([this, itemCount] {
    selectedIndex = (selectedIndex + 1) % itemCount;
    topIndex = followListSelection(static_cast<int>(selectedIndex), topIndex, visibleRows, itemCount);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, itemCount] {
    selectedIndex = (selectedIndex + itemCount - 1) % itemCount;
    topIndex = followListSelection(static_cast<int>(selectedIndex), topIndex, visibleRows, itemCount);
    requestUpdate();
  });
}
