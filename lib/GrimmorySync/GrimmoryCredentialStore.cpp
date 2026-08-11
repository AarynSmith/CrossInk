#include "GrimmoryCredentialStore.h"

#include <Logging.h>
#include <ObfuscationUtils.h>

void GrimmoryCredentialStore::toJson(JsonDocument& doc) const {
  doc["username"] = getUsername();
  doc["password_obf"] = obfuscation::obfuscateToBase64(getPassword());
  doc["serverUrl"] = getServerUrl();
  doc["syncStatsEnabled"] = getSyncStatsEnabled();
  doc["syncShelvesEnabled"] = getSyncShelvesEnabled();
  doc["targetShelfId"] = targetShelfId;
  doc["targetShelfName"] = targetShelfName;
  doc["syncBehavior"] = static_cast<uint8_t>(getSyncBehavior());
}

bool GrimmoryCredentialStore::fromJson(JsonVariantConst doc) {
  const std::string user = doc["username"] | "";

  obfuscation::DecodeStatus status = obfuscation::DecodeStatus::INVALID;
  std::string pass = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", &status);
  if (status == obfuscation::DecodeStatus::INVALID && !pass.empty()) {
    LOG_ERR("GRM", "Ignoring unreadable Grimmory password");
    pass.clear();
  }

  setCredentials(user, pass);
  setServerUrl(doc["serverUrl"] | "");
  setSyncStatsEnabled(doc["syncStatsEnabled"] | true);
  setSyncShelvesEnabled(doc["syncShelvesEnabled"] | true);
  setTargetShelf(doc["targetShelfId"] | (int64_t)-1, doc["targetShelfName"] | "");

  const uint8_t behavior = doc["syncBehavior"] | static_cast<uint8_t>(GrimmorySyncBehavior::SMART);
  if (behavior <= static_cast<uint8_t>(GrimmorySyncBehavior::SMART)) {
    setSyncBehavior(static_cast<GrimmorySyncBehavior>(behavior));
  } else {
    LOG_DBG("GRM", "Invalid syncBehavior %u in JSON, resetting to SMART", behavior);
    setSyncBehavior(GrimmorySyncBehavior::SMART);
  }

  if (status == obfuscation::DecodeStatus::LEGACY) {
    requestResave();
  }

  return true;
}

void GrimmoryCredentialStore::setCredentials(const std::string& user, const std::string& pass) {
  username = user;
  password = pass;
}

bool GrimmoryCredentialStore::hasCredentials() const { return !username.empty() && !password.empty(); }

void GrimmoryCredentialStore::clearCredentials() {
  username.clear();
  password.clear();
  saveToFile();
}

void GrimmoryCredentialStore::setServerUrl(const std::string& url) { serverUrl = url; }

std::string GrimmoryCredentialStore::getBaseUrl() const {
  if (serverUrl.empty()) return "";

  std::string url = serverUrl.find("://") == std::string::npos ? "http://" + serverUrl : serverUrl;
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  return url;
}

void GrimmoryCredentialStore::setSyncStatsEnabled(bool enabled) { syncStatsEnabled = enabled; }

void GrimmoryCredentialStore::setSyncShelvesEnabled(bool enabled) { syncShelvesEnabled = enabled; }

void GrimmoryCredentialStore::setTargetShelf(int64_t shelfId, const std::string& shelfName) {
  targetShelfId = shelfId;
  targetShelfName = shelfId >= 0 ? shelfName : "";
}

void GrimmoryCredentialStore::setSyncBehavior(GrimmorySyncBehavior behavior) {
  if (static_cast<uint8_t>(behavior) > static_cast<uint8_t>(GrimmorySyncBehavior::SMART)) {
    behavior = GrimmorySyncBehavior::SMART;
  }
  syncBehavior = behavior;
}
