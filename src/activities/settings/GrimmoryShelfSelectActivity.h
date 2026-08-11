#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>
#include <vector>

#include "GrimmoryApiClient.h"
#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"
#include "util/ButtonNavigator.h"

/**
 * Lets the user restrict Grimmory sync to a single shelf: connects to WiFi,
 * logs in, fetches the shelf list, and shows it (plus an "All Shelves"
 * entry) as a selectable list. Selecting an entry saves it to
 * GrimmoryCredentialStore and returns. Modeled on GrimmorySyncActivity for
 * the network-boot flow and on OpdsServerListActivity for the dynamic list.
 */
class GrimmoryShelfSelectActivity final : public Activity {
 public:
  static constexpr const char* NAME = "GrimmoryShelfSelect";

  explicit GrimmoryShelfSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == LOADING; }

 private:
  enum State { WIFI_SELECTION, CONNECTING, LOADING, SHOWING_LIST, FAILED };
  using UiApp = freeink::ui::FreeInkApp<32, 4>;

  State state = WIFI_SELECTION;
  ScreenTransitionRefresh screenTransitionRefresh;
  std::string errorMessage;
  std::vector<GrimmoryShelf> shelves;

  ButtonNavigator buttonNavigator;
  size_t selectedIndex = 0;
  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;
  int topIndex = 0;

  int getItemCount() const { return static_cast<int>(shelves.size()) + 1; }  // +1 for "All Shelves"

  void onWifiSelectionComplete(bool success);
  void loadShelves();
  void handleSelection();
  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);
};
