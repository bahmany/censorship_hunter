#pragma once

#include <string>
#include <vector>

namespace hunter {
namespace network {

/**
 * @brief Check if a URI scheme is supported for import
 * 
 * Returns true for schemes that can be processed by the subscription manager
 * and engine runners. Excludes ssr:// as nothing can run it.
 */
bool isImportableProxyUri(const std::string& uri);

} // namespace network
} // namespace hunter