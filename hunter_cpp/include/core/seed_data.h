#pragma once

#include <string>
#include <vector>

namespace hunter {

/**
 * Seed data management for initial application setup and packaging.
 * 
 * This class provides functionality to:
 * - Create default source configurations when none exist
 * - Provide seed configurations for initial testing
 * - Export current state for packaging with builds
 * - Ensure the application has working defaults on first run
 */
class SeedData {
public:
    /**
     * Ensure seed data exists in the runtime directory.
     * Creates default sources, configs, and history files if they don't exist.
     * 
     * @param runtime_dir Path to the runtime directory
     * @return true if any seed files were created, false otherwise
     */
    static bool ensureSeedData(const std::string& runtime_dir);
    
    /**
     * Load seed configurations from the runtime directory.
     * Returns a vector of configuration strings for initial database population.
     * 
     * @param runtime_dir Path to the runtime directory
     * @return Vector of configuration strings
     */
    static std::vector<std::string> loadSeedConfigs(const std::string& runtime_dir);
    
    /**
     * Export current runtime data for packaging with builds.
     * Copies sources manager, history, and working configs to the export directory.
     * 
     * @param runtime_dir Path to the runtime directory
     * @param export_dir Path to the export directory for packaging
     * @return true if export was successful, false otherwise
     */
    static bool exportForPackaging(const std::string& runtime_dir, const std::string& export_dir);

private:
    // Private constructor - this is a utility class with only static methods
    SeedData() = delete;
    ~SeedData() = delete;
    SeedData(const SeedData&) = delete;
    SeedData& operator=(const SeedData&) = delete;
};

} // namespace hunter
