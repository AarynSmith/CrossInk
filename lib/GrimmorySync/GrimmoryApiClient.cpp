#include "GrimmoryApiClient.h"

#include <ArduinoJson.h>
#ifdef SIMULATOR
#include <ArduinoJsonStringCompat.h>
#endif
#include <I18n.h>
#include <Logging.h>
#ifdef SIMULATOR
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#else
#include <SecureHttpClient.h>
#endif

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "GrimmoryCredentialStore.h"

int GrimmoryApiClient::lastHttpCode = 0;

// On device, the session's Impl owns the one freeink::SecureHttpClient reused
// for every request in a sync run (see GrimmoryApiClient.h). Its destructor
// closes the connection, so GrimmoryApiSession needs no explicit teardown.
// The simulator isn't memory-constrained, so it keeps opening one HTTPClient
// per call and Impl holds nothing.
struct GrimmoryApiSession::Impl {
#ifndef SIMULATOR
  freeink::SecureHttpClient http;
#endif
};

GrimmoryApiSession::GrimmoryApiSession() : impl(std::make_unique<Impl>()) {}
GrimmoryApiSession::~GrimmoryApiSession() = default;

namespace {
// KOReaderSyncClient uses 35000/20000 before its (always fresh, always torn
// down) TLS handshake. This client also reserves MAX_RESPONSE_BODY_BYTES
// (16KB) up front for every request on top of that, so the floor is raised
// accordingly to keep real headroom for the handshake itself plus parsing.
constexpr uint32_t MIN_FREE_HEAP_FOR_TLS = 48000;
constexpr uint32_t MIN_MAX_ALLOC_HEAP_FOR_TLS = 24000;

bool insufficientHeap() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_TLS || maxAllocHeap < MIN_MAX_ALLOC_HEAP_FOR_TLS) {
    LOG_ERR("Grimmory", "Insufficient heap for TLS handshake: %u bytes free (need %u), %u max alloc (need %u)",
            freeHeap, MIN_FREE_HEAP_FOR_TLS, maxAllocHeap, MIN_MAX_ALLOC_HEAP_FOR_TLS);
    return true;
  }
  return false;
}

std::string formatIso8601(const int64_t unixSeconds) {
  const time_t t = static_cast<time_t>(unixSeconds);
  struct tm tmVal{};
  gmtime_r(&t, &tmVal);
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tmVal.tm_year + 1900, tmVal.tm_mon + 1, tmVal.tm_mday,
           tmVal.tm_hour, tmVal.tm_min, tmVal.tm_sec);
  return std::string(buf);
}

#ifdef SIMULATOR
bool isHttpsUrl(const std::string& url) { return url.rfind("https://", 0) == 0; }
#endif

// Logs a sanitized preview of a response body for diagnostics: control
// characters are replaced with spaces and the body is capped so a large
// unexpected payload (e.g. an HTML error page) doesn't flood the log.
void logResponsePreview(const char* context, const std::string& body) {
  char preview[161];
  size_t i = 0;
  for (; i < sizeof(preview) - 1 && i < body.size(); i++) {
    const char c = body[i];
    preview[i] = (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
  }
  preview[i] = '\0';
  LOG_ERR("Grimmory", "%s response body: \"%s\"%s", context, preview, body.size() >= sizeof(preview) ? "..." : "");
}

// Hard cap on how much of any single response this client will buffer,
// enforced during the read itself rather than trusting the server to honor
// pagination (see rawRequest). outBody.reserve()s this exact size upfront
// (below) so std::string's own geometric growth never has to reallocate
// mid-transfer — growing a string via repeated small appends can transiently
// need ~1.5-2x its current size (old + new buffer briefly both alive), which
// on its own was enough to exhaust heap even while the final size stayed
// under this cap.
constexpr size_t MAX_RESPONSE_BODY_BYTES = 16 * 1024;

// Performs one request against `path` (relative to the configured base URL)
// with an optional JSON body, adding the Bearer auth header when a non-empty
// token is given (login() itself passes an empty token). `extraHeaders` adds
// arbitrary additional headers (used for the koreader-sync x-auth-user/
// x-auth-key scheme, which is unrelated to the JWT bearer auth used
// everywhere else in this client — see pushKoreaderProgress). Returns the
// parsed response Error and leaves the raw response body in outBody. Reuses
// apiSession's connection across calls (see GrimmoryApiSession) instead of
// opening a fresh TLS connection per request — deliberately never calls
// SecureHttpClient::end() here, since that would close the reusable
// connection after every single call and defeat the point.
GrimmoryApiClient::Error rawRequest(GrimmoryApiSession& apiSession, const char* method, const std::string& path,
                                    const std::string& accessToken, const std::string* jsonBody, std::string& outBody,
                                    const std::vector<std::pair<std::string, std::string>>* extraHeaders = nullptr) {
  GrimmoryApiClient::lastHttpCode = 0;
  outBody.clear();

  const std::string baseUrl = GRIMMORY_STORE.getBaseUrl();
  if (baseUrl.empty()) {
    LOG_ERR("Grimmory", "No server URL configured");
    return GrimmoryApiClient::NO_CREDENTIALS;
  }
  const std::string url = baseUrl + path;

  // LOG_INF (not LOG_DBG): visible at this project's LOG_LEVEL=1 release
  // builds, where LOG_DBG (LOG_LEVEL>=2) is compiled out entirely — a prior
  // version of this line used LOG_DBG and was silently never printed.
  LOG_INF("Grimmory", "%s %s (heap: %u free / %u max-alloc)", method, url.c_str(), (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());
  if (insufficientHeap()) return GrimmoryApiClient::LOW_MEMORY;

#ifdef SIMULATOR
  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;

  if (isHttpsUrl(url)) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  if (!accessToken.empty()) {
    http.addHeader("Authorization", (std::string("Bearer ") + accessToken).c_str());
  }
  if (extraHeaders) {
    for (const auto& [name, value] : *extraHeaders) {
      http.addHeader(name.c_str(), value.c_str());
    }
  }
  if (jsonBody) {
    http.addHeader("Content-Type", "application/json");
  }

  const int httpCode = jsonBody ? http.sendRequest(method, jsonBody->c_str()) : http.sendRequest(method, "");
  GrimmoryApiClient::lastHttpCode = httpCode;
  outBody = http.getString().c_str();
  http.end();
#else
  // Reused across every call in this session (see GrimmoryApiSession); do
  // not call http.end() here or it tears down the kept-alive connection.
  freeink::SecureHttpClient& http = apiSession.impl->http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("Grimmory", "Bad URL: %s", url.c_str());
    return GrimmoryApiClient::NETWORK_ERROR;
  }
  if (!accessToken.empty()) {
    http.addHeader("Authorization", std::string("Bearer ") + accessToken);
  }
  if (extraHeaders) {
    for (const auto& [name, value] : *extraHeaders) {
      http.addHeader(name, value);
    }
  }
  if (jsonBody) {
    http.addHeader("Content-Type", "application/json");
  }

  // Reserve the full cap upfront: one allocation for the whole transfer,
  // instead of std::string growing (and transiently doubling) as append()
  // calls arrive from the read loop below.
  outBody.reserve(MAX_RESPONSE_BODY_BYTES);

  // Stream the body in via a callback instead of the simple buffered
  // sendRequest() overload, so we can refuse to grow outBody past
  // MAX_RESPONSE_BODY_BYTES no matter how large the server's response is.
  bool bodyTooLarge = false;
  const auto onData = [&outBody, &bodyTooLarge](const uint8_t* data, const size_t len) {
    if (outBody.size() + len > MAX_RESPONSE_BODY_BYTES) {
      bodyTooLarge = true;
      return false;  // abort the transfer
    }
    outBody.append(reinterpret_cast<const char*>(data), len);
    return true;
  };
  const int httpCode =
      jsonBody ? http.sendRequest(method, reinterpret_cast<const uint8_t*>(jsonBody->data()), jsonBody->size(), onData)
               : http.sendRequest(method, nullptr, 0, onData);
  GrimmoryApiClient::lastHttpCode = httpCode;

  if (bodyTooLarge) {
    LOG_ERR("Grimmory", "%s %s response exceeded %u bytes; server may not be honoring pagination", method, path.c_str(),
            (unsigned)MAX_RESPONSE_BODY_BYTES);
    return GrimmoryApiClient::SERVER_ERROR;
  }
#endif

  LOG_INF("Grimmory", "%s %s -> %d (body %u bytes, heap after: %u free / %u max-alloc)", method, path.c_str(), httpCode,
          (unsigned)outBody.size(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  if (httpCode <= 0) return GrimmoryApiClient::NETWORK_ERROR;
  if (httpCode == 200 || httpCode == 201 || httpCode == 202) return GrimmoryApiClient::OK;
  if (httpCode == 401 || httpCode == 403) return GrimmoryApiClient::AUTH_FAILED;
  if (httpCode == 404) return GrimmoryApiClient::NOT_FOUND;
  return GrimmoryApiClient::SERVER_ERROR;
}

// Returns the first of `keys` present in `object` as a non-empty string, or
// nullptr if none match. Checks .is<const char*>() explicitly for each key
// individually rather than chaining JsonVariantConst::operator| across
// multiple candidates, which does not do the intuitive per-key fallback.
const char* firstNonEmptyString(JsonObjectConst object, std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    JsonVariantConst value = object[key];
    if (value.is<const char*>()) {
      const char* str = value.as<const char*>();
      if (str && str[0] != '\0') return str;
    }
  }
  return nullptr;
}

// Reads a shelf id from either a bare number or an {"id": N} object, to
// tolerate either shape the server may send for a book's `shelves` array.
int64_t readShelfId(JsonVariantConst entry) {
  if (entry.is<JsonObjectConst>()) {
    return entry["id"] | (int64_t)0;
  }
  return entry.as<int64_t>();
}

// Strips an ISBN down to bare digits (+ trailing checksum 'X'), matching
// exactly how GrimmoryIdentifierScanner normalizes ISBNs extracted from a
// local EPUB's content.opf. The server's own isbn10/isbn13 fields can
// include hyphens (e.g. "979-8520410171"), and comparing that directly
// against a normalized local value (see resolveBookMatches's identifier
// lookup) would never match — every book would go unmatched regardless of
// whether the identifiers actually agree.
std::string normalizeIsbn(const std::string& raw) {
  std::string out;
  for (const char c : raw) {
    if (std::isdigit(static_cast<unsigned char>(c)) || c == 'x' || c == 'X') {
      out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
  }
  return out;
}

// A book object from either endpoint below carries far more than
// GrimmoryBookSummary needs — title, description (can be long HTML), author,
// file paths, cover, size, hashes, timestamps, alternativeFormats — and
// parsing all of it into a JsonDocument (ArduinoJson's tree has per-node
// overhead on top of the raw JSON) was enough to exhaust heap even for a
// modest page of real books. This filter (same technique as Epub.cpp's
// buildXLocationsJsonFilter) tells the parser to skip every field but the
// ones actually read below. `shelves` is left unfiltered (kept as-is) since
// its elements may be either bare numbers or {"id": ...} objects (see
// readShelfId) and a mismatched filter shape would silently drop whichever
// doesn't match.
void addBookFieldsToFilter(JsonObject book) {
  book["id"] = true;
  JsonObject metadata = book["metadata"].to<JsonObject>();
  metadata["isbn10"] = true;
  metadata["isbn13"] = true;
  metadata["asin"] = true;
  metadata["pageCount"] = true;
  book["shelves"] = true;
}

// GET /api/v1/books/page's envelope: {"content": [...], "page": {"totalElements": N}}.
// Matches the "one representative element" convention ArduinoJson filters
// use to describe every element of an array uniformly.
void buildBooksPageJsonFilter(JsonDocument& filter) {
  filter["page"]["totalElements"] = true;
  JsonArray content = filter["content"].to<JsonArray>();
  addBookFieldsToFilter(content.add<JsonObject>());
}

// GET /api/v1/shelves/<id>/books returns a flat JSON array at the root, no
// "content"/"page" envelope.
void buildShelfBooksJsonFilter(JsonDocument& filter) {
  JsonArray root = filter.to<JsonArray>();
  addBookFieldsToFilter(root.add<JsonObject>());
}

// Shared by getBooksPage and getShelfBooks: builds a GrimmoryBookSummary
// from one (filtered) book object and hands it to the caller's callback.
void parseBookSummary(JsonObjectConst book, const std::function<void(const GrimmoryBookSummary&)>& onBook) {
  GrimmoryBookSummary entry;
  entry.id = book["id"] | (int64_t)0;
  JsonObjectConst metadata = book["metadata"].as<JsonObjectConst>();
  entry.isbn10 = normalizeIsbn(metadata["isbn10"] | "");
  entry.isbn13 = normalizeIsbn(metadata["isbn13"] | "");
  entry.asin = metadata["asin"] | "";  // ASINs are alphanumeric, not digit-only; no normalization needed
  entry.pageCount = metadata["pageCount"] | (uint32_t)0;
  for (JsonVariantConst shelf : book["shelves"].as<JsonArrayConst>()) {
    entry.shelfIds.push_back(readShelfId(shelf));
  }
  onBook(entry);
}
}  // namespace

GrimmoryApiClient::Error GrimmoryApiClient::login(GrimmoryApiSession& apiSession, std::string& outAccessToken) {
  if (!GRIMMORY_STORE.hasCredentials()) {
    LOG_DBG("Grimmory", "No credentials configured");
    return NO_CREDENTIALS;
  }

  JsonDocument doc;
  doc["username"] = GRIMMORY_STORE.getUsername();
  doc["password"] = GRIMMORY_STORE.getPassword();
  std::string body;
  serializeJson(doc, body);

  std::string responseBody;
  const Error result = rawRequest(apiSession, "POST", "/api/v1/auth/login", "", &body, responseBody);
  if (result != OK) return result;

  JsonDocument response;
  const DeserializationError error = deserializeJson(response, responseBody);
  if (error) {
    LOG_ERR("Grimmory", "Login response JSON parse failed: %s", error.c_str());
    logResponsePreview("Login", responseBody);
    return JSON_ERROR;
  }

  const char* accessToken =
      firstNonEmptyString(response.as<JsonObjectConst>(), {"accessToken", "access_token", "token"});
  if (!accessToken) {
    JsonObjectConst nested = response["data"].as<JsonObjectConst>();
    if (nested.isNull()) nested = response["tokens"].as<JsonObjectConst>();
    if (!nested.isNull()) {
      accessToken = firstNonEmptyString(nested, {"accessToken", "access_token", "token"});
    }
  }
  if (!accessToken) {
    LOG_ERR("Grimmory", "Login response missing accessToken");
    logResponsePreview("Login", responseBody);
    return JSON_ERROR;
  }

  outAccessToken = accessToken;
  return OK;
}

GrimmoryApiClient::Error GrimmoryApiClient::getShelves(GrimmoryApiSession& apiSession, const std::string& accessToken,
                                                       std::vector<GrimmoryShelf>& out) {
  std::string responseBody;
  const Error result = rawRequest(apiSession, "GET", "/api/v1/shelves", accessToken, nullptr, responseBody);
  if (result != OK) return result;

  JsonDocument response;
  const DeserializationError error = deserializeJson(response, responseBody);
  if (error) {
    LOG_ERR("Grimmory", "Shelves response JSON parse failed: %s", error.c_str());
    return JSON_ERROR;
  }

  out.clear();
  for (JsonObjectConst shelf : response.as<JsonArrayConst>()) {
    GrimmoryShelf entry;
    entry.id = shelf["id"] | (int64_t)0;
    entry.name = shelf["name"] | "";
    out.push_back(std::move(entry));
  }
  return OK;
}

GrimmoryApiClient::Error GrimmoryApiClient::getBooksPage(
    GrimmoryApiSession& apiSession, const std::string& accessToken, const int page, const int pageSize,
    int& outTotalElements, const std::function<void(const GrimmoryBookSummary&)>& onBook) {
  char path[80];
  snprintf(path, sizeof(path), "/api/v1/books/page?sort=addedOn&page=%d&size=%d", page, pageSize);

  std::string responseBody;
  const Error result = rawRequest(apiSession, "GET", path, accessToken, nullptr, responseBody);
  if (result != OK) return result;

  LOG_INF("Grimmory", "Books page %d body: %u bytes, heap before parse: %u free / %u max-alloc", page,
          (unsigned)responseBody.size(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  JsonDocument filter;
  buildBooksPageJsonFilter(filter);
  JsonDocument response;
  const DeserializationError error =
      deserializeJson(response, responseBody, DeserializationOption::Filter(filter.as<JsonVariantConst>()));
  if (error) {
    LOG_ERR("Grimmory", "Books page response JSON parse failed: %s", error.c_str());
    logResponsePreview("Books page", responseBody);
    return JSON_ERROR;
  }

  LOG_INF("Grimmory", "Books page %d parsed OK, heap after parse: %u free / %u max-alloc", page,
          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  outTotalElements = response["page"]["totalElements"] | 0;

  for (JsonObjectConst book : response["content"].as<JsonArrayConst>()) {
    parseBookSummary(book, onBook);
  }
  return OK;
}

GrimmoryApiClient::Error GrimmoryApiClient::getShelfBooks(
    GrimmoryApiSession& apiSession, const std::string& accessToken, const int64_t shelfId,
    const std::function<void(const GrimmoryBookSummary&)>& onBook) {
  char path[48];
  snprintf(path, sizeof(path), "/api/v1/shelves/%lld/books", (long long)shelfId);

  std::string responseBody;
  const Error result = rawRequest(apiSession, "GET", path, accessToken, nullptr, responseBody);
  if (result != OK) return result;

  LOG_INF("Grimmory", "Shelf %lld books body: %u bytes, heap before parse: %u free / %u max-alloc", (long long)shelfId,
          (unsigned)responseBody.size(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  JsonDocument filter;
  buildShelfBooksJsonFilter(filter);
  JsonDocument response;
  const DeserializationError error =
      deserializeJson(response, responseBody, DeserializationOption::Filter(filter.as<JsonVariantConst>()));
  if (error) {
    LOG_ERR("Grimmory", "Shelf books response JSON parse failed: %s", error.c_str());
    logResponsePreview("Shelf books", responseBody);
    return JSON_ERROR;
  }

  LOG_INF("Grimmory", "Shelf %lld books parsed OK, heap after parse: %u free / %u max-alloc", (long long)shelfId,
          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  for (JsonObjectConst book : response.as<JsonArrayConst>()) {
    parseBookSummary(book, onBook);
  }
  return OK;
}

GrimmoryApiClient::Error GrimmoryApiClient::postReadingSession(GrimmoryApiSession& apiSession,
                                                               const std::string& accessToken,
                                                               const GrimmoryReadingSession& session) {
  const float progressDelta =
      session.endProgress > session.startProgress ? session.endProgress - session.startProgress : 0.0f;

  JsonDocument doc;
  doc["bookId"] = session.grimmoryBookId;
  doc["bookType"] = "EPUB";
  doc["startTime"] = formatIso8601(session.startTime);
  doc["endTime"] = formatIso8601(session.endTime);
  doc["durationSeconds"] = session.endTime - session.startTime;
  doc["startProgress"] = session.startProgress;
  doc["endProgress"] = session.endProgress;
  doc["progressDelta"] = progressDelta;
  doc["startLocation"] = std::to_string(session.startPage);
  doc["endLocation"] = std::to_string(session.endPage);

  std::string body;
  serializeJson(doc, body);

  std::string responseBody;
  return rawRequest(apiSession, "POST", "/api/v1/reading-sessions", accessToken, &body, responseBody);
}

GrimmoryApiClient::Error GrimmoryApiClient::getKoreaderUser(GrimmoryApiSession& apiSession,
                                                            const std::string& accessToken, GrimmoryKoreaderUser& out) {
  std::string responseBody;
  const Error result = rawRequest(apiSession, "GET", "/api/v1/koreader-users/me", accessToken, nullptr, responseBody);
  if (result != OK) return result;

  JsonDocument response;
  const DeserializationError error = deserializeJson(response, responseBody);
  if (error) {
    LOG_ERR("Grimmory", "KoReader user response JSON parse failed: %s", error.c_str());
    logResponsePreview("KoReader user", responseBody);
    return JSON_ERROR;
  }

  out.username = response["username"] | "";
  out.password = response["password"] | "";
  out.passwordMd5 = response["passwordMD5"] | "";
  out.syncEnabled = response["syncEnabled"] | false;
  out.syncWithWebReader = response["syncWithWebReader"] | false;
  return OK;
}

GrimmoryApiClient::Error GrimmoryApiClient::setKoreaderCredentials(GrimmoryApiSession& apiSession,
                                                                   const std::string& accessToken,
                                                                   const std::string& username,
                                                                   const std::string& password) {
  JsonDocument doc;
  doc["username"] = username;
  doc["password"] = password;
  std::string body;
  serializeJson(doc, body);

  std::string responseBody;
  return rawRequest(apiSession, "PUT", "/api/v1/koreader-users/me", accessToken, &body, responseBody);
}

GrimmoryApiClient::Error GrimmoryApiClient::setKoreaderSyncEnabled(GrimmoryApiSession& apiSession,
                                                                   const std::string& accessToken, const bool enabled) {
  const std::string path = std::string("/api/v1/koreader-users/me/sync?enabled=") + (enabled ? "true" : "false");
  std::string responseBody;
  return rawRequest(apiSession, "PATCH", path, accessToken, nullptr, responseBody);
}

GrimmoryApiClient::Error GrimmoryApiClient::setKoreaderSyncWithWebReader(GrimmoryApiSession& apiSession,
                                                                         const std::string& accessToken,
                                                                         const bool enabled) {
  const std::string path =
      std::string("/api/v1/koreader-users/me/sync-progress-with-grimmory?enabled=") + (enabled ? "true" : "false");
  std::string responseBody;
  return rawRequest(apiSession, "PATCH", path, accessToken, nullptr, responseBody);
}

GrimmoryApiClient::Error GrimmoryApiClient::pushKoreaderProgress(GrimmoryApiSession& apiSession,
                                                                 const std::string& koreaderUsername,
                                                                 const std::string& koreaderPasswordMd5,
                                                                 const GrimmoryKoreaderProgress& progress) {
  JsonDocument doc;
  doc["document"] = progress.document;
  doc["progress"] = progress.progress;
  doc["percentage"] = progress.percentage;
  doc["device"] = progress.device;
  doc["device_id"] = progress.deviceId;
  doc["timestamp"] = static_cast<int64_t>(time(nullptr));

  std::string body;
  serializeJson(doc, body);

  // x-auth-user/x-auth-key, NOT the JWT bearer used everywhere else in this
  // client — this is the koreader-sync protocol's own auth scheme (see
  // GrimmoryKoreaderUser). Pass an empty accessToken so rawRequest skips the
  // Authorization: Bearer header entirely.
  const std::vector<std::pair<std::string, std::string>> headers = {
      {"x-auth-user", koreaderUsername},
      {"x-auth-key", koreaderPasswordMd5},
  };

  std::string responseBody;
  return rawRequest(apiSession, "PUT", "/api/koreader/syncs/progress", "", &body, responseBody, &headers);
}

GrimmoryApiClient::Error GrimmoryApiClient::pullKoreaderProgress(GrimmoryApiSession& apiSession,
                                                                 const std::string& koreaderUsername,
                                                                 const std::string& koreaderPasswordMd5,
                                                                 const std::string& documentHash,
                                                                 GrimmoryKoreaderProgress& out) {
  // Same koreader-sync auth scheme as pushKoreaderProgress (x-auth-user/
  // x-auth-key, not the JWT bearer), so pass an empty accessToken.
  const std::vector<std::pair<std::string, std::string>> headers = {
      {"x-auth-user", koreaderUsername},
      {"x-auth-key", koreaderPasswordMd5},
  };

  std::string responseBody;
  const Error result = rawRequest(apiSession, "GET", "/api/koreader/syncs/progress/" + documentHash, "", nullptr,
                                  responseBody, &headers);
  if (result != OK) return result;

  JsonDocument response;
  const DeserializationError error = deserializeJson(response, responseBody);
  if (error) {
    LOG_ERR("Grimmory", "KoReader progress response JSON parse failed: %s", error.c_str());
    logResponsePreview("KoReader progress", responseBody);
    return JSON_ERROR;
  }

  out.document = documentHash;
  out.progress = response["progress"] | "";
  out.percentage = response["percentage"] | 0.0f;
  out.device = response["device"] | "";
  out.deviceId = response["device_id"] | "";
  return OK;
}

std::string GrimmoryApiClient::errorString(const Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_CREDENTIALS:
      return tr(STR_GRIMMORY_NO_CREDENTIALS);
    case NETWORK_ERROR:
      return tr(STR_GRIMMORY_NETWORK_ERROR);
    case AUTH_FAILED:
      return tr(STR_GRIMMORY_AUTH_REJECTED);
    case SERVER_ERROR:
      if (lastHttpCode > 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), tr(STR_GRIMMORY_HTTP_STATUS_FORMAT), lastHttpCode);
        return std::string(buf);
      }
      return tr(STR_GRIMMORY_SERVER_ERROR);
    case JSON_ERROR:
      return tr(STR_GRIMMORY_BAD_RESPONSE);
    case LOW_MEMORY:
      return tr(STR_GRIMMORY_LOW_MEMORY);
    case NOT_FOUND:
      if (lastHttpCode > 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), tr(STR_GRIMMORY_HTTP_STATUS_FORMAT), lastHttpCode);
        return std::string(buf);
      }
      return tr(STR_GRIMMORY_SERVER_ERROR);
    default:
      return tr(STR_UNKNOWN_ERROR);
  }
}
