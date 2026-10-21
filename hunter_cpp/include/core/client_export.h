#pragma once

#include <string>
#include <vector>

namespace hunter {
namespace client_export {

/**
 * @brief Normalize a single proxy URI for import into v2rayN and compatible clients.
 *
 * Trim whitespace/CR/LF/BOM/zero-width chars, lowercase the scheme, map known
 * aliases (hy2->hysteria2, socks5->socks), drop unsupported schemes, clean
 * fragments and encode remarks. Returns the empty string when the URI must be
 * dropped.
 */
std::string normalizeUri(const std::string& uri);

/**
 * @brief Build a share/export text from a list of URIs.
 *
 * Normalizes each URI (deduplicated, sorted), joins with "\r\n" on Windows
 * and "\n" elsewhere, and ends with a trailing newline. No comment/header
 * lines are included.
 */
std::string buildShareText(const std::vector<std::string>& uris);

} // namespace client_export
} // namespace hunter
