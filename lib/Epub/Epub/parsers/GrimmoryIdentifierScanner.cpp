#include "GrimmoryIdentifierScanner.h"

#include <Logging.h>
#include <XmlParserUtils.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {
std::string toUpper(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return out;
}

std::string trim(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) start++;
  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
  return s.substr(start, end - start);
}

// Strips everything but digits and a trailing checksum 'X' (ISBN-10), for
// classifying ISBN identifiers by digit count regardless of hyphenation.
std::string isbnDigitsOnly(const std::string& s) {
  std::string out;
  for (const char c : s) {
    if (std::isdigit(static_cast<unsigned char>(c)) || c == 'x' || c == 'X') {
      out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
  }
  return out;
}

bool startsWithCaseInsensitive(const std::string& s, const char* prefix) {
  const size_t len = std::strlen(prefix);
  if (s.size() < len) return false;
  for (size_t i = 0; i < len; ++i) {
    if (std::toupper(static_cast<unsigned char>(s[i])) != std::toupper(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}
}  // namespace

bool GrimmoryIdentifierScanner::setup() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_DBG("GIS", "Couldn't allocate memory for identifier scanner parser");
    lowMemoryFailure = true;
    return false;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  return true;
}

GrimmoryIdentifierScanner::~GrimmoryIdentifierScanner() { destroyXmlParser(parser); }

size_t GrimmoryIdentifierScanner::write(const uint8_t* buffer, const size_t size) {
  if (!parser || parseFailed) return 0;

  const uint8_t* currentBufferPos = buffer;
  auto remainingInBuffer = size;

  while (remainingInBuffer > 0) {
    void* const buf = XML_GetBuffer(parser, 1024);
    if (!buf) {
      LOG_ERR("GIS", "Couldn't allocate memory for identifier scanner buffer");
      lowMemoryFailure = true;
      destroyXmlParser(parser);
      return 0;
    }

    const auto toRead = remainingInBuffer < 1024 ? remainingInBuffer : 1024;
    memcpy(buf, currentBufferPos, toRead);

    const XML_Status parseStatus = XML_ParseBuffer(parser, static_cast<int>(toRead), remainingSize == toRead);
    if (parseStatus != XML_STATUS_OK) {
      if (!parseFailed) {
        LOG_DBG("GIS", "Identifier scan parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
                XML_ErrorString(XML_GetErrorCode(parser)));
      }
      parseFailed = true;
      destroyXmlParser(parser);
      return 0;
    }

    currentBufferPos += toRead;
    remainingInBuffer -= toRead;
    remainingSize -= toRead;
  }

  return size;
}

void GrimmoryIdentifierScanner::classifyCurrentIdentifier() {
  const std::string value = trim(currentValue);
  if (value.empty()) return;

  std::string scheme = toUpper(trim(currentScheme));
  std::string body = value;

  // EPUB3 often omits opf:scheme and instead prefixes the text itself, e.g.
  // "urn:isbn:9780062316097" or "urn:asin:B00ABCDEFG".
  if (scheme.empty()) {
    if (startsWithCaseInsensitive(body, "urn:isbn:")) {
      scheme = "ISBN";
      body = body.substr(std::strlen("urn:isbn:"));
    } else if (startsWithCaseInsensitive(body, "urn:asin:")) {
      scheme = "ASIN";
      body = body.substr(std::strlen("urn:asin:"));
    } else if (startsWithCaseInsensitive(body, "urn:mobi-asin:")) {
      scheme = "ASIN";
      body = body.substr(std::strlen("urn:mobi-asin:"));
    }
  }

  if (scheme.find("ISBN") != std::string::npos) {
    const std::string digits = isbnDigitsOnly(body);
    if (digits.size() == 13 && isbn13.empty()) {
      isbn13 = digits;
    } else if (digits.size() == 10 && isbn10.empty()) {
      isbn10 = digits;
    }
  } else if ((scheme.find("ASIN") != std::string::npos || scheme.find("AMAZON") != std::string::npos) &&
             asin.empty()) {
    asin = trim(body);
  }
}

void XMLCALL GrimmoryIdentifierScanner::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<GrimmoryIdentifierScanner*>(userData);

  if (self->state == START && (strcmp(name, "package") == 0 || strcmp(name, "opf:package") == 0)) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "metadata") == 0 || strcmp(name, "opf:metadata") == 0)) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:identifier") == 0) {
    self->state = IN_IDENTIFIER;
    self->currentScheme.clear();
    self->currentValue.clear();
    for (int i = 0; atts[i] != nullptr && atts[i + 1] != nullptr; i += 2) {
      if (strcmp(atts[i], "opf:scheme") == 0 || strcmp(atts[i], "scheme") == 0) {
        self->currentScheme = atts[i + 1];
        break;
      }
    }
    return;
  }
}

void XMLCALL GrimmoryIdentifierScanner::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<GrimmoryIdentifierScanner*>(userData);
  if (self->state == IN_IDENTIFIER) {
    self->currentValue.append(s, len);
  }
}

void XMLCALL GrimmoryIdentifierScanner::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<GrimmoryIdentifierScanner*>(userData);

  if (self->state == IN_IDENTIFIER && strcmp(name, "dc:identifier") == 0) {
    self->classifyCurrentIdentifier();
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && (strcmp(name, "metadata") == 0 || strcmp(name, "opf:metadata") == 0)) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "package") == 0 || strcmp(name, "opf:package") == 0)) {
    self->state = START;
    return;
  }
}
