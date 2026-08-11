#pragma once
#include <Print.h>

#include <string>

#include "expat.h"

// Lightweight, standalone OPF <dc:identifier> scanner used only to recover
// ISBN-10/ISBN-13/ASIN identifiers for Grimmory sync book matching.
// Deliberately independent of ContentOpfParser/BookMetadataCache: it does not
// write to or change the cached BookMetadata/book.bin format in any way, it
// only reads dc:identifier text out of content.opf.
class GrimmoryIdentifierScanner final : public Print {
  enum ParserState {
    START,
    IN_PACKAGE,
    IN_METADATA,
    IN_IDENTIFIER,
  };

  size_t remainingSize;
  XML_Parser parser = nullptr;
  ParserState state = START;
  bool parseFailed = false;
  bool lowMemoryFailure = false;
  std::string currentScheme;
  std::string currentValue;

  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

  void classifyCurrentIdentifier();

 public:
  std::string isbn10;
  std::string isbn13;
  std::string asin;

  explicit GrimmoryIdentifierScanner(const size_t xmlSize) : remainingSize(xmlSize) {}
  ~GrimmoryIdentifierScanner() override;

  bool setup();
  bool failedForLowMemory() const { return lowMemoryFailure; }

  size_t write(uint8_t data) override { return write(&data, 1); }
  size_t write(const uint8_t* buffer, size_t size) override;
};
