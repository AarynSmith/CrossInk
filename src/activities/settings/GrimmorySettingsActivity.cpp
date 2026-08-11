#include "GrimmorySettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "GrimmoryCredentialStore.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr int MENU_ITEMS = 8;
const StrId menuNames[MENU_ITEMS] = {StrId::STR_GRIMMORY_SERVER_URL,   StrId::STR_GRIMMORY_USERNAME,
                                     StrId::STR_GRIMMORY_PASSWORD,     StrId::STR_GRIMMORY_SYNC_STATS,
                                     StrId::STR_GRIMMORY_SYNC_SHELVES, StrId::STR_GRIMMORY_SHELF_TO_SYNC,
                                     StrId::STR_SYNC_BEHAVIOR,         StrId::STR_GRIMMORY_SYNC_NOW};
constexpr fui::ActionId ACTION_ROW = 1;
}  // namespace

GrimmorySettingsActivity::GrimmorySettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("GrimmorySettings", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void GrimmorySettingsActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<GrimmorySettingsActivity*>(user);
  if (event.value < 0 || event.value >= MENU_ITEMS) return;
  self->selectedIndex = static_cast<size_t>(event.value);
  self->app.clearTapFlash();
  self->handleSelection();
}

void GrimmorySettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  uiReady = false;
  visibleRows = 1;
  topIndex = 0;
  app.setTheme(uiThemeTokens(uiTarget));
  app.on(ACTION_ROW, &GrimmorySettingsActivity::onRowEvent, this);
  app.setScreen(&GrimmorySettingsActivity::listScreen, this);
  requestUpdate();
}

void GrimmorySettingsActivity::onExit() { Activity::onExit(); }

void GrimmorySettingsActivity::loop() {
  auto activateSelected = [this] { handleSelection(); };

  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finishAfterBackPress();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelected();
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

  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    topIndex = followListSelection(static_cast<int>(selectedIndex), topIndex, visibleRows, MENU_ITEMS);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    topIndex = followListSelection(static_cast<int>(selectedIndex), topIndex, visibleRows, MENU_ITEMS);
    requestUpdate();
  });
}

void GrimmorySettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    // Server URL
    const std::string currentUrl = GRIMMORY_STORE.getServerUrl();
    const std::string prefillUrl = currentUrl.empty() ? "http://" : currentUrl;
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_GRIMMORY_SERVER_URL),
                                                                   prefillUrl, 128, InputType::Url),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               const std::string urlToSave =
                                   (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
                               GRIMMORY_STORE.setServerUrl(urlToSave);
                               GRIMMORY_STORE.saveToFile();
                             }
                           });
  } else if (selectedIndex == 1) {
    // Username
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_GRIMMORY_USERNAME),
                                                                   GRIMMORY_STORE.getUsername(), 64, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               GRIMMORY_STORE.setCredentials(kb.text, GRIMMORY_STORE.getPassword());
                               GRIMMORY_STORE.saveToFile();
                             }
                           });
  } else if (selectedIndex == 2) {
    // Password
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_GRIMMORY_PASSWORD),
                                                GRIMMORY_STORE.getPassword(), 64, InputType::Password),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            const auto& kb = std::get<KeyboardResult>(result.data);
            GRIMMORY_STORE.setCredentials(GRIMMORY_STORE.getUsername(), kb.text);
            GRIMMORY_STORE.saveToFile();
          }
        });
  } else if (selectedIndex == 3) {
    // Sync Reading Stats toggle
    GRIMMORY_STORE.setSyncStatsEnabled(!GRIMMORY_STORE.getSyncStatsEnabled());
    GRIMMORY_STORE.saveToFile();
    requestUpdate();
  } else if (selectedIndex == 4) {
    // Sync Shelf Tags toggle
    GRIMMORY_STORE.setSyncShelvesEnabled(!GRIMMORY_STORE.getSyncShelvesEnabled());
    GRIMMORY_STORE.saveToFile();
    requestUpdate();
  } else if (selectedIndex == 5) {
    // Shelf to Sync (needs a network round-trip to list shelves)
    if (!GRIMMORY_STORE.hasCredentials() || GRIMMORY_STORE.getBaseUrl().empty()) {
      return;
    }
    silentRestartToNetwork(NetworkBootTarget::GRIMMORY_SHELF_SELECT);
  } else if (selectedIndex == 6) {
    // Sync Behavior - toggle between Ask and Smart
    const auto current = GRIMMORY_STORE.getSyncBehavior();
    const auto newBehavior = (current == GrimmorySyncBehavior::ASK_EVERY_TIME) ? GrimmorySyncBehavior::SMART
                                                                               : GrimmorySyncBehavior::ASK_EVERY_TIME;
    GRIMMORY_STORE.setSyncBehavior(newBehavior);
    GRIMMORY_STORE.saveToFile();
    requestUpdate();
  } else if (selectedIndex == 7) {
    // Sync Now
    if (!GRIMMORY_STORE.hasCredentials() || GRIMMORY_STORE.getBaseUrl().empty()) {
      return;
    }
    silentRestartToNetwork(NetworkBootTarget::GRIMMORY_SYNC);
  }
}

void GrimmorySettingsActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<GrimmorySettingsActivity*>(user)->buildListScreen(screen);
}

void GrimmorySettingsActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  std::vector<std::string> values(MENU_ITEMS);
  for (int i = 0; i < MENU_ITEMS; i++) {
    if (i == 0) {
      values[i] = GRIMMORY_STORE.getServerUrl();
      if (values[i].empty()) values[i] = tr(STR_NOT_SET);
    } else if (i == 1) {
      const auto username = GRIMMORY_STORE.getUsername();
      values[i] = username.empty() ? tr(STR_NOT_SET) : username;
    } else if (i == 2) {
      values[i] = GRIMMORY_STORE.getPassword().empty() ? tr(STR_NOT_SET) : "******";
    } else if (i == 3) {
      values[i] = GRIMMORY_STORE.getSyncStatsEnabled() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    } else if (i == 4) {
      values[i] = GRIMMORY_STORE.getSyncShelvesEnabled() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    } else if (i == 5) {
      values[i] =
          GRIMMORY_STORE.getTargetShelfId() < 0 ? tr(STR_GRIMMORY_ALL_SHELVES) : GRIMMORY_STORE.getTargetShelfName();
    } else if (i == 6) {
      values[i] =
          GRIMMORY_STORE.getSyncBehavior() == GrimmorySyncBehavior::SMART ? tr(STR_SMART_SYNC) : tr(STR_ASK_EVERY_TIME);
    } else {
      values[i] = (GRIMMORY_STORE.hasCredentials() && !GRIMMORY_STORE.getBaseUrl().empty())
                      ? ""
                      : std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
    }
  }

  std::vector<fui::ListItem> items;
  items.reserve(MENU_ITEMS);
  for (int i = 0; i < MENU_ITEMS; i++) {
    fui::ListItem item;
    item.label = I18N.get(menuNames[i]);
    if (!values[i].empty()) item.value = values[i].c_str();
    item.actionValue = static_cast<int16_t>(i);
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
  topIndex = scrollListBy(topIndex, 0, visibleRows, MENU_ITEMS);
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void GrimmorySettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_GRIMMORY_SYNC), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_GRIMMORY_SYNC));
  }

  uiReady = false;
  app.render();
  uiReady = true;

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
