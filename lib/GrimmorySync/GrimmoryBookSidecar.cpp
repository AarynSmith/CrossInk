#include "GrimmoryBookSidecar.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <PersistableStore.h>

namespace {
constexpr char SIDECAR_SUFFIX[] = ".grimmory-data.json";
}

std::string GrimmoryBookSidecar::sidecarPathFor(const std::string& bookPath) {
  const size_t lastSlash = bookPath.find_last_of('/');
  const std::string dir = lastSlash == std::string::npos ? "" : bookPath.substr(0, lastSlash + 1);
  const std::string filename = lastSlash == std::string::npos ? bookPath : bookPath.substr(lastSlash + 1);

  const size_t lastDot = filename.find_last_of('.');
  const std::string stem = lastDot == std::string::npos ? filename : filename.substr(0, lastDot);

  return dir + stem + SIDECAR_SUFFIX;
}

GrimmoryBookSidecar GrimmoryBookSidecar::load(const std::string& bookPath) {
  GrimmoryBookSidecar sidecar;

  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(sidecarPathFor(bookPath).c_str(), doc)) {
    return sidecar;
  }

  sidecar.isbn10 = doc["isbn10"] | "";
  sidecar.isbn13 = doc["isbn13"] | "";
  sidecar.asin = doc["asin"] | "";
  sidecar.grimmoryBookId = doc["grimmoryBookId"] | (int64_t)-1;
  sidecar.remotePageCount = doc["remotePageCount"] | (uint32_t)0;
  sidecar.initialProgressPulled = doc["initialProgressPulled"] | false;

  for (JsonVariantConst shelf : doc["shelves"].as<JsonArrayConst>()) {
    const char* name = shelf | "";
    if (name && name[0] != '\0') sidecar.shelves.emplace_back(name);
  }

  for (JsonObjectConst session : doc["pendingSessions"].as<JsonArrayConst>()) {
    GrimmoryPendingSession pending;
    pending.durationSeconds = session["durationSeconds"] | (uint32_t)0;
    pending.startProgress = session["startProgress"] | 0.0f;
    pending.endProgress = session["endProgress"] | 0.0f;
    pending.startPage = session["startPage"] | (uint32_t)0;
    pending.endPage = session["endPage"] | (uint32_t)0;
    sidecar.pendingSessions.push_back(pending);
  }

  return sidecar;
}

bool GrimmoryBookSidecar::save(const std::string& bookPath) const {
  JsonDocument doc;
  doc["isbn10"] = isbn10;
  doc["isbn13"] = isbn13;
  doc["asin"] = asin;
  doc["grimmoryBookId"] = grimmoryBookId;
  doc["remotePageCount"] = remotePageCount;
  doc["initialProgressPulled"] = initialProgressPulled;

  JsonArray shelvesArray = doc["shelves"].to<JsonArray>();
  for (const auto& shelf : shelves) {
    shelvesArray.add(shelf);
  }

  JsonArray sessionsArray = doc["pendingSessions"].to<JsonArray>();
  for (const auto& session : pendingSessions) {
    JsonObject entry = sessionsArray.add<JsonObject>();
    entry["durationSeconds"] = session.durationSeconds;
    entry["startProgress"] = session.startProgress;
    entry["endProgress"] = session.endProgress;
    entry["startPage"] = session.startPage;
    entry["endPage"] = session.endPage;
  }

  const std::string path = sidecarPathFor(bookPath);
  LOG_INF("GRM", "Writing sidecar %s (grimmoryBookId=%lld, shelves=%u, pendingSessions=%u)", path.c_str(),
          (long long)grimmoryBookId, (unsigned)shelves.size(), (unsigned)pendingSessions.size());
  if (!PersistableStoreBase::writeDocToFile(path.c_str(), doc)) {
    LOG_ERR("GRM", "Failed to write Grimmory sidecar: %s", path.c_str());
    return false;
  }
  LOG_INF("GRM", "Wrote sidecar OK: %s", path.c_str());
  return true;
}

const std::string& GrimmoryBookSidecar::primaryIdentifier() const {
  if (!isbn13.empty()) return isbn13;
  if (!isbn10.empty()) return isbn10;
  return asin;
}

void GrimmoryBookSidecar::addPendingSession(const GrimmoryPendingSession& session) {
  if (pendingSessions.size() >= MAX_PENDING_SESSIONS) {
    LOG_ERR("GRM", "Pending Grimmory session queue full; dropping oldest entry");
    pendingSessions.erase(pendingSessions.begin());
  }
  pendingSessions.push_back(session);
}
