#include "geo/country_query.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace hunter {
namespace geo {

namespace {
struct Entry { const char* iso; const char* name; };
const Entry kCountries[] = {
    {"AD", "Andorra"},
    {"AE", "United Arab Emirates"},
    {"AF", "Afghanistan"},
    {"AG", "Antigua and Barbuda"},
    {"AI", "Anguilla"},
    {"AL", "Albania"},
    {"AM", "Armenia"},
    {"AO", "Angola"},
    {"AQ", "Antarctica"},
    {"AR", "Argentina"},
    {"AS", "American Samoa"},
    {"AT", "Austria"},
    {"AU", "Australia"},
    {"AW", "Aruba"},
    {"AX", "Aland Islands"},
    {"AZ", "Azerbaijan"},
    {"BA", "Bosnia and Herzegovina"},
    {"BB", "Barbados"},
    {"BD", "Bangladesh"},
    {"BE", "Belgium"},
    {"BF", "Burkina Faso"},
    {"BG", "Bulgaria"},
    {"BH", "Bahrain"},
    {"BI", "Burundi"},
    {"BJ", "Benin"},
    {"BL", "Saint Barthelemy"},
    {"BM", "Bermuda"},
    {"BN", "Brunei"},
    {"BO", "Bolivia"},
    {"BQ", "Caribbean Netherlands"},
    {"BR", "Brazil"},
    {"BS", "Bahamas"},
    {"BT", "Bhutan"},
    {"BV", "Bouvet Island"},
    {"BW", "Botswana"},
    {"BY", "Belarus"},
    {"BZ", "Belize"},
    {"CA", "Canada"},
    {"CC", "Cocos Islands"},
    {"CD", "DR Congo"},
    {"CF", "Central African Republic"},
    {"CG", "Congo"},
    {"CH", "Switzerland"},
    {"CI", "Cote d'Ivoire"},
    {"CK", "Cook Islands"},
    {"CL", "Chile"},
    {"CM", "Cameroon"},
    {"CN", "China"},
    {"CO", "Colombia"},
    {"CR", "Costa Rica"},
    {"CU", "Cuba"},
    {"CV", "Cabo Verde"},
    {"CW", "Curacao"},
    {"CX", "Christmas Island"},
    {"CY", "Cyprus"},
    {"CZ", "Czechia"},
    {"DE", "Germany"},
    {"DJ", "Djibouti"},
    {"DK", "Denmark"},
    {"DM", "Dominica"},
    {"DO", "Dominican Republic"},
    {"DZ", "Algeria"},
    {"EC", "Ecuador"},
    {"EE", "Estonia"},
    {"EG", "Egypt"},
    {"EH", "Western Sahara"},
    {"ER", "Eritrea"},
    {"ES", "Spain"},
    {"ET", "Ethiopia"},
    {"FI", "Finland"},
    {"FJ", "Fiji"},
    {"FK", "Falkland Islands"},
    {"FM", "Micronesia"},
    {"FO", "Faroe Islands"},
    {"FR", "France"},
    {"GA", "Gabon"},
    {"GB", "United Kingdom"},
    {"GD", "Grenada"},
    {"GE", "Georgia"},
    {"GF", "French Guiana"},
    {"GG", "Guernsey"},
    {"GH", "Ghana"},
    {"GI", "Gibraltar"},
    {"GL", "Greenland"},
    {"GM", "Gambia"},
    {"GN", "Guinea"},
    {"GP", "Guadeloupe"},
    {"GQ", "Equatorial Guinea"},
    {"GR", "Greece"},
    {"GS", "South Georgia"},
    {"GT", "Guatemala"},
    {"GU", "Guam"},
    {"GW", "Guinea-Bissau"},
    {"GY", "Guyana"},
    {"HK", "Hong Kong"},
    {"HM", "Heard Island"},
    {"HN", "Honduras"},
    {"HR", "Croatia"},
    {"HT", "Haiti"},
    {"HU", "Hungary"},
    {"ID", "Indonesia"},
    {"IE", "Ireland"},
    {"IL", "Israel"},
    {"IM", "Isle of Man"},
    {"IN", "India"},
    {"IO", "British Indian Ocean Territory"},
    {"IQ", "Iraq"},
    {"IR", "Iran"},
    {"IS", "Iceland"},
    {"IT", "Italy"},
    {"JE", "Jersey"},
    {"JM", "Jamaica"},
    {"JO", "Jordan"},
    {"JP", "Japan"},
    {"KE", "Kenya"},
    {"KG", "Kyrgyzstan"},
    {"KH", "Cambodia"},
    {"KI", "Kiribati"},
    {"KM", "Comoros"},
    {"KN", "Saint Kitts and Nevis"},
    {"KP", "North Korea"},
    {"KR", "South Korea"},
    {"KW", "Kuwait"},
    {"KY", "Cayman Islands"},
    {"KZ", "Kazakhstan"},
    {"LA", "Laos"},
    {"LB", "Lebanon"},
    {"LC", "Saint Lucia"},
    {"LI", "Liechtenstein"},
    {"LK", "Sri Lanka"},
    {"LR", "Liberia"},
    {"LS", "Lesotho"},
    {"LT", "Lithuania"},
    {"LU", "Luxembourg"},
    {"LV", "Latvia"},
    {"LY", "Libya"},
    {"MA", "Morocco"},
    {"MC", "Monaco"},
    {"MD", "Moldova"},
    {"ME", "Montenegro"},
    {"MF", "Saint Martin"},
    {"MG", "Madagascar"},
    {"MH", "Marshall Islands"},
    {"MK", "North Macedonia"},
    {"ML", "Mali"},
    {"MM", "Myanmar"},
    {"MN", "Mongolia"},
    {"MO", "Macao"},
    {"MP", "Northern Mariana Islands"},
    {"MQ", "Martinique"},
    {"MR", "Mauritania"},
    {"MS", "Montserrat"},
    {"MT", "Malta"},
    {"MU", "Mauritius"},
    {"MV", "Maldives"},
    {"MW", "Malawi"},
    {"MX", "Mexico"},
    {"MY", "Malaysia"},
    {"MZ", "Mozambique"},
    {"NA", "Namibia"},
    {"NC", "New Caledonia"},
    {"NE", "Niger"},
    {"NF", "Norfolk Island"},
    {"NG", "Nigeria"},
    {"NI", "Nicaragua"},
    {"NL", "Netherlands"},
    {"NO", "Norway"},
    {"NP", "Nepal"},
    {"NR", "Nauru"},
    {"NU", "Niue"},
    {"NZ", "New Zealand"},
    {"OM", "Oman"},
    {"PA", "Panama"},
    {"PE", "Peru"},
    {"PF", "French Polynesia"},
    {"PG", "Papua New Guinea"},
    {"PH", "Philippines"},
    {"PK", "Pakistan"},
    {"PL", "Poland"},
    {"PM", "Saint Pierre and Miquelon"},
    {"PN", "Pitcairn"},
    {"PR", "Puerto Rico"},
    {"PS", "Palestine"},
    {"PT", "Portugal"},
    {"PW", "Palau"},
    {"PY", "Paraguay"},
    {"QA", "Qatar"},
    {"RE", "Reunion"},
    {"RO", "Romania"},
    {"RS", "Serbia"},
    {"RU", "Russia"},
    {"RW", "Rwanda"},
    {"SA", "Saudi Arabia"},
    {"SB", "Solomon Islands"},
    {"SC", "Seychelles"},
    {"SD", "Sudan"},
    {"SE", "Sweden"},
    {"SG", "Singapore"},
    {"SH", "Saint Helena"},
    {"SI", "Slovenia"},
    {"SJ", "Svalbard and Jan Mayen"},
    {"SK", "Slovakia"},
    {"SL", "Sierra Leone"},
    {"SM", "San Marino"},
    {"SN", "Senegal"},
    {"SO", "Somalia"},
    {"SR", "Suriname"},
    {"SS", "South Sudan"},
    {"ST", "Sao Tome and Principe"},
    {"SV", "El Salvador"},
    {"SX", "Sint Maarten"},
    {"SY", "Syria"},
    {"SZ", "Eswatini"},
    {"TC", "Turks and Caicos Islands"},
    {"TD", "Chad"},
    {"TF", "French Southern Territories"},
    {"TG", "Togo"},
    {"TH", "Thailand"},
    {"TJ", "Tajikistan"},
    {"TK", "Tokelau"},
    {"TL", "Timor-Leste"},
    {"TM", "Turkmenistan"},
    {"TN", "Tunisia"},
    {"TO", "Tonga"},
    {"TR", "Turkey"},
    {"TT", "Trinidad and Tobago"},
    {"TV", "Tuvalu"},
    {"TW", "Taiwan"},
    {"TZ", "Tanzania"},
    {"UA", "Ukraine"},
    {"UG", "Uganda"},
    {"UM", "US Minor Outlying Islands"},
    {"US", "United States"},
    {"UY", "Uruguay"},
    {"UZ", "Uzbekistan"},
    {"VA", "Vatican City"},
    {"VC", "Saint Vincent and the Grenadines"},
    {"VE", "Venezuela"},
    {"VG", "British Virgin Islands"},
    {"VI", "US Virgin Islands"},
    {"VN", "Vietnam"},
    {"VU", "Vanuatu"},
    {"WF", "Wallis and Futuna"},
    {"WS", "Samoa"},
    {"XK", "Kosovo"},
    {"YE", "Yemen"},
    {"YT", "Mayotte"},
    {"ZA", "South Africa"},
    {"ZM", "Zambia"},
    {"ZW", "Zimbabwe"},
};

const Entry* findIso(const std::string& iso) {
    if (iso.size() != 2) return nullptr;
    for (const auto& e : kCountries)
        if (e.iso[0] == iso[0] && e.iso[1] == iso[1]) return &e;
    return nullptr;
}
}  // namespace

std::string toLowerAscii(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string toUpperAscii(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

std::string countryName(const std::string& iso) {
    const Entry* e = findIso(iso);
    return e ? e->name : "";
}

bool isValidIso(const std::string& iso) { return findIso(iso) != nullptr; }

std::string countryLabel(const std::string& iso) {
    if (iso.empty()) return "Unknown";
    if (iso == kMixed) return "Mixed";
    std::string n = countryName(iso);
    return n.empty() ? iso : iso + " " + n;
}

bool countryValueMatches(const std::string& stored, const std::string& value_in) {
    const std::string v = toLowerAscii(value_in);
    if (v.empty()) return false;
    if (v == "unknown") return stored.empty();
    if (v == "mixed") return stored == kMixed;
    if (stored.empty() || stored == kMixed) return false;
    if (v.size() == 2) return toLowerAscii(stored) == v;
    if (v.size() < 3) return false;
    return toLowerAscii(countryName(stored)).find(v) != std::string::npos;
}

bool passesCountryFilter(CountryMode mode, const CountryChoice& c,
                         const std::string& exit_c, const std::string& server_c) {
    if (c.kind == CountryChoice::Any) return true;
    auto one = [&](const std::string& stored) {
        switch (c.kind) {
            case CountryChoice::Specific: return !stored.empty() && stored != kMixed && stored == c.iso;
            case CountryChoice::Unknown: return stored.empty();
            case CountryChoice::Mixed: return stored == kMixed;
            default: return true;
        }
    };
    switch (mode) {
        case CountryMode::Exit: return exit_c != kMixed && one(exit_c);   // Mixed is a server-hint concept only
        case CountryMode::Server: return one(server_c);
        case CountryMode::All:
            if (c.kind == CountryChoice::Unknown) return exit_c.empty() && server_c.empty();
            return one(exit_c) || one(server_c);
    }
    return true;
}

std::vector<QueryTerm> parseSearchQuery(const std::string& text) {
    std::vector<QueryTerm> out;
    std::istringstream is(text);
    std::string tok;
    while (is >> tok) {
        QueryTerm t;
        std::string low = toLowerAscii(tok);
        if (low.rfind("exit:", 0) == 0) { t.kind = QueryTerm::Exit; t.value = low.substr(5); }
        else if (low.rfind("server:", 0) == 0) { t.kind = QueryTerm::Server; t.value = low.substr(7); }
        else { t.kind = QueryTerm::Free; t.value = low; }
        if (t.value.empty()) continue;   // dangling "exit:" is ignored while the user is typing
        out.push_back(std::move(t));
    }
    return out;
}

bool matchesSearchQuery(const std::vector<QueryTerm>& terms, const std::function<bool(const std::string&)>& text_has,
                        CountryMode mode, const std::string& exit_c, const std::string& server_c) {
    for (const auto& t : terms) {
        switch (t.kind) {
            case QueryTerm::Exit:
                if (exit_c == kMixed || !countryValueMatches(exit_c, t.value)) return false;
                break;
            case QueryTerm::Server:
                if (!countryValueMatches(server_c, t.value)) return false;
                break;
            case QueryTerm::Free: {
                // A bare two-letter token that is an ISO code is a country query only (otherwise "de"
                // would match half of all addresses as plain text).
                const bool iso_only = t.value.size() == 2 && isValidIso(toUpperAscii(t.value));
                if (!iso_only && text_has(t.value)) break;
                bool ok = false;
                if (t.value == "unknown" || t.value == "mixed") {
                    ok = false;   // explicit class words need an exit:/server: prefix; never match free text
                } else if (t.value.size() >= 2) {
                    if (mode != CountryMode::Server) ok = ok || (exit_c != kMixed && countryValueMatches(exit_c, t.value));
                    if (mode != CountryMode::Exit) ok = ok || countryValueMatches(server_c, t.value);
                }
                if (!ok) return false;
                break;
            }
        }
    }
    return true;
}

bool matchesSearchQuery(const std::vector<QueryTerm>& terms, const std::string& hay,
                        CountryMode mode, const std::string& exit_c, const std::string& server_c) {
    return matchesSearchQuery(terms, [&hay](const std::string& t) { return hay.find(t) != std::string::npos; },
                              mode, exit_c, server_c);
}

}  // namespace geo
}  // namespace hunter
