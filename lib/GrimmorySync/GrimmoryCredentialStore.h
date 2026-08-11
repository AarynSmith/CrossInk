#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

// How the "Sync to Grimmory" single-book flow (the reader hotkey/quick
// action) resolves a difference between local and remote koreader-sync
// progress after syncing. Mirrors KOReaderSyncBehavior
// (lib/KOReaderSync/KOReaderCredentialStore.h) exactly. Does not affect the
// "sync everything" flow (Settings' "Sync Now", or the hotkey fired outside
// the reader), which has no single book to compare/resolve.
enum class GrimmorySyncBehavior : uint8_t {
  ASK_EVERY_TIME = 0,  // Show Apply-remote/Upload-local choices and wait for confirmation.
  SMART = 1,           // Auto-resolve using furthest progress, then auto-return to the reader.
};

/**
 * Singleton class for storing Grimmory sync credentials on the SD card.
 * Passwords are XOR-obfuscated with the device's unique hardware MAC address
 * and base64-encoded before writing to JSON, following the same scheme as
 * KOReaderCredentialStore (not cryptographically secure, but prevents casual
 * reading and ties credentials to the specific device).
 */
class GrimmoryCredentialStore : public PersistableStore<GrimmoryCredentialStore> {
 private:
  std::string username;
  std::string password;
  std::string serverUrl;
  bool syncStatsEnabled = true;
  bool syncShelvesEnabled = true;
  int64_t targetShelfId = -1;   // -1 means "all shelves"
  std::string targetShelfName;  // cached display name, empty when targetShelfId == -1
  GrimmorySyncBehavior syncBehavior = GrimmorySyncBehavior::SMART;

  GrimmoryCredentialStore() = default;
  ~GrimmoryCredentialStore() = default;

  friend class PersistableStore<GrimmoryCredentialStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/grimmory.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  void setCredentials(const std::string& user, const std::string& pass);
  const std::string& getUsername() const { return username; }
  const std::string& getPassword() const { return password; }
  bool hasCredentials() const;
  void clearCredentials();

  void setServerUrl(const std::string& url);
  const std::string& getServerUrl() const { return serverUrl; }
  // Normalized base URL for API calls (adds http:// if no scheme, strips
  // trailing slashes). Empty if no server URL has been configured yet.
  std::string getBaseUrl() const;

  void setSyncStatsEnabled(bool enabled);
  bool getSyncStatsEnabled() const { return syncStatsEnabled; }
  void setSyncShelvesEnabled(bool enabled);
  bool getSyncShelvesEnabled() const { return syncShelvesEnabled; }

  // Restricts which books get matched/synced to one Grimmory shelf.
  // shelfId < 0 (the default) means "all shelves".
  void setTargetShelf(int64_t shelfId, const std::string& shelfName);
  int64_t getTargetShelfId() const { return targetShelfId; }
  const std::string& getTargetShelfName() const { return targetShelfName; }

  void setSyncBehavior(GrimmorySyncBehavior behavior);
  GrimmorySyncBehavior getSyncBehavior() const { return syncBehavior; }
};

#define GRIMMORY_STORE GrimmoryCredentialStore::getInstance()
