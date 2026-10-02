#pragma once
// Country names + search/filter parsing for the config table (design D4). Pure functions, no I/O.
#include <functional>
#include <string>
#include <vector>

namespace hunter {
namespace geo {

constexpr const char* kMixed = "Mixed";   // stored in server_country when resolved IPs disagree

/// English name for an ISO-3166 alpha-2 code ("" if the code is not in the table).
std::string countryName(const std::string& iso);
/// "DE Germany", "Unknown" (empty), "Mixed".
std::string countryLabel(const std::string& iso);
bool isValidIso(const std::string& iso);       // two upper-case letters in the table
std::string toLowerAscii(std::string s);
std::string toUpperAscii(std::string s);

/// Value of an `exit:`/`server:` term or dropdown choice matched against a stored country.
/// value (case-insensitive): ISO code, name substring (>=3 chars), "unknown", "mixed".
/// stored: ISO code, "" (unknown) or "Mixed".
bool countryValueMatches(const std::string& stored, const std::string& value);

enum class CountryMode { All, Exit, Server };

struct CountryChoice {            // dropdown next to the mode selector
    enum Kind { Any, Specific, Unknown, Mixed } kind = Any;
    std::string iso;              // for Specific
};
/// Mode All: Specific = exit OR server matches; Unknown = both unknown; Mixed = server Mixed.
/// Mode Exit: only the exit column is considered (Mixed never matches). Mode Server: only the server column.
bool passesCountryFilter(CountryMode mode, const CountryChoice& choice,
                         const std::string& exit_country, const std::string& server_country);

struct QueryTerm {
    enum Kind { Free, Exit, Server } kind = Free;
    std::string value;            // lower-case
};
/// Whitespace separated terms, AND-combined. `exit:DE` / `server:US` (value may be a name, "unknown", "mixed").
/// A free term matches the text haystack OR (ISO code equal / name substring) the country selected by `mode`.
std::vector<QueryTerm> parseSearchQuery(const std::string& text);
bool matchesSearchQuery(const std::vector<QueryTerm>& terms, const std::string& haystack_lower,
                        CountryMode mode, const std::string& exit_country, const std::string& server_country);

/// Same semantics with a caller-supplied text predicate (term is lower-case): lets the GUI search the raw
/// URI / tag without building a haystack string per record.
bool matchesSearchQuery(const std::vector<QueryTerm>& terms, const std::function<bool(const std::string&)>& text_has,
                        CountryMode mode, const std::string& exit_country, const std::string& server_country);

}  // namespace geo
}  // namespace hunter
