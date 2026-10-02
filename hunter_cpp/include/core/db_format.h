#pragma once
// TSV row (de)serialization for ConfigDatabase V4 / live cache V3 (design D2) and
// parsing of the legacy V1-V3 layouts. Pure logic, no file I/O.
#include <string>
#include <vector>

#include "core/models.h"

namespace hunter {

extern const char* const kDbHeaderV4;     // "#HUNTER_CONFIG_DB_V4"
extern const char* const kLiveHeaderV3;   // "#HUNTER_LIVE_CACHE_V3"
constexpr size_t kMaxRowBytes = 256 * 1024;
constexpr size_t kLegacyColumns = 14;

const std::vector<std::string>& v4ExtensionColumns();
std::string v4ColumnsLine();               // "#COLUMNS\t<ext names>"

std::string escapeTsv(const std::string& s);
bool unescapeTsv(const std::string& s, std::string* out);  // false on malformed escape
std::vector<std::string> splitTabs(const std::string& line);

// One V4 row (no trailing newline). `now` is only used to store the derived stability.
std::string serializeRecordV4(const ConfigHealthRecord& rec, double now, const HealthThresholds& th);
// Strict parse + validation. Recomputes the endpoint key from the URI and compares.
bool parseRecordV4(const std::string& line, const HealthThresholds& th, ConfigHealthRecord* out,
                   std::string* err);

// Legacy layouts: layout 3 = 14 columns (DB V3 / live V2), 2 = 12 (DB V2 / live V1), 1 = 11 (DB V1).
// Produces a record with Unknown health, empty ring, needs_retest=true; old alive/latency/
// totals are retained as hints only.
bool parseRecordLegacy(int layout, const std::vector<std::string>& fields, const HealthThresholds& th,
                       ConfigHealthRecord* out);

// Fill derived legacy fields (alive, latency_ms, consecutive_fails, last_alive_time) from evidence.
void syncLegacyFromEvidence(ConfigHealthRecord* rec);
// Initialise static evidence attributes + key fields from the URI.
void initRecordIdentity(ConfigHealthRecord* rec);

}  // namespace hunter
