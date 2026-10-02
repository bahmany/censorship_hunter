#include "network/country_provider.h"

#include <map>
#include <mutex>

#include "geo/country_database.h"

namespace hunter {
namespace network {

struct CountryProviderRegistry::Impl {
    mutable std::mutex mu;
    std::map<std::string, Factory> factories;
    std::string default_name = "null";
};

namespace {
class NullProvider : public ICountryProvider {
public:
    std::string lookupIp(const std::string&) const override { return ""; }
    std::string version() const override { return ""; }
    std::string name() const override { return "null"; }
};

class OfflineProvider : public ICountryProvider {
public:
    explicit OfflineProvider(geo::CountryDatabase* db) : db_(db) {}
    std::string lookupIp(const std::string& ip) const override {
        if (!db_ || !db_->isLoaded()) return "";
        std::string c = db_->lookup(ip);
        return (c == geo::CountryDatabase::kUnknown || c.size() != 2) ? "" : c;
    }
    std::string version() const override { return db_ && db_->isLoaded() ? db_->dbVersion() : ""; }
    std::string name() const override { return "offline"; }
private:
    geo::CountryDatabase* db_;
};
}  // namespace

CountryProviderRegistry::CountryProviderRegistry() : impl_(std::make_shared<Impl>()) {
    impl_->factories["null"] = [] { return makeNullCountryProvider(); };
}

CountryProviderRegistry& CountryProviderRegistry::instance() {
    static CountryProviderRegistry r;
    return r;
}

void CountryProviderRegistry::registerFactory(const std::string& name, Factory f) {
    if (name.empty() || !f) return;
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->factories[name] = std::move(f);
}

std::shared_ptr<ICountryProvider> CountryProviderRegistry::create(const std::string& name) const {
    Factory f;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        auto it = impl_->factories.find(name.empty() ? impl_->default_name : name);
        if (it != impl_->factories.end()) f = it->second;
    }
    std::shared_ptr<ICountryProvider> p = f ? f() : nullptr;
    return p ? p : makeNullCountryProvider();
}

std::vector<std::string> CountryProviderRegistry::names() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    std::vector<std::string> v;
    for (auto& kv : impl_->factories) v.push_back(kv.first);
    return v;
}

void CountryProviderRegistry::setDefault(const std::string& name) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->factories.count(name)) impl_->default_name = name;
}

std::string CountryProviderRegistry::defaultName() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->default_name;
}

std::shared_ptr<ICountryProvider> makeNullCountryProvider() { return std::make_shared<NullProvider>(); }

void registerBuiltinCountryProviders() {
    auto& r = CountryProviderRegistry::instance();
    r.registerFactory("null", [] { return makeNullCountryProvider(); });
    r.registerFactory("offline", [] {
        return std::shared_ptr<ICountryProvider>(new OfflineProvider(&geo::CountryDatabase::instance()));
    });
}

}  // namespace network
}  // namespace hunter
