#pragma once
// Country-provider interface + factory registry (design D4 / A->C contract).
// Default provider is "null" (everything Unknown). Providers are registered EXPLICITLY at startup
// (registerBuiltinCountryProviders) - never from static initializers, and creating a provider never
// starts background work.
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hunter {
namespace network {

class ICountryProvider {
public:
    virtual ~ICountryProvider() = default;
    /// ISO-3166 alpha-2 for a literal public IP, "" if unknown/private/unmapped.
    virtual std::string lookupIp(const std::string& ip) const = 0;
    /// Identifies the data snapshot ("" for the null provider). A change invalidates offline-derived entries.
    virtual std::string version() const = 0;
    virtual std::string name() const = 0;
};

class CountryProviderRegistry {
public:
    using Factory = std::function<std::shared_ptr<ICountryProvider>()>;
    static CountryProviderRegistry& instance();
    /// Replaces an existing factory of the same name. Factories must not start threads or I/O.
    void registerFactory(const std::string& name, Factory f);
    /// Unknown name => the null provider (never nullptr).
    std::shared_ptr<ICountryProvider> create(const std::string& name) const;
    std::vector<std::string> names() const;
    /// Name used by create("") .
    void setDefault(const std::string& name);
    std::string defaultName() const;
private:
    CountryProviderRegistry();
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

std::shared_ptr<ICountryProvider> makeNullCountryProvider();
/// Registers "null" and "offline" (DB-IP HCGEO1 via CountryDatabase::instance()). Idempotent.
void registerBuiltinCountryProviders();

}  // namespace network
}  // namespace hunter
