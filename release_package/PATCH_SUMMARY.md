# Patch Summary – Config Management & UI Freeze Hardening

## 1. IP-based deduplication (database & validation)
- **Files:** `src/network/continuous_validator.cpp`, `src/orchestrator/orchestrator.cpp`
- **What:** Configs are now deduplicated by normalized endpoint (IP/IPv6 literal) instead of raw URI hash.
- **Why:** Prevent duplicate configs that only differ by parameters or UUIDs but share the same endpoint.
- **How:** `endpointKeyForUri` extracts address/host; `looksLikeLiteralIp` ensures we only collapse literal IPs, not hostnames.

## 2. Stricter “live” criteria
- **Files:** `src/network/continuous_validator.cpp`
- **What:** A config is considered “live” only if it passes ping, speed test, and has `download_speed_kbps > 0`.
- **Why:** Avoid marking Telegram-only or ping-only configs as usable for general traffic.
- **How:** `isUsableResult` now requires `result.success && !result.telegram_only && result.download_speed_kbps > 0.0f`.

## 3. GitHub sources refresh every hour after first success
- **Files:** `src/orchestrator/orchestrator.cpp`
- **What:** If at least one working config exists and the last successful GitHub refresh is older than 1 hour, a background refresh fetches new configs from GitHub.
- **Why:** Keep configs fresh without overwhelming the network on first run.
- **How:** In `runCycle`, read `last_success_ts` from the GitHub worker; if `now - last_success_ts >= 3600.0`, call `fetchGithubConfigs` and add to `ConfigDatabase`.

## 4. UI: “Copy All” everywhere configs are shown
- **Files:** `src/win32/imgui_app.cpp`
- **What:** Added “Copy All Healthy” on Home, “Copy All Live/Visible” on Configs page, “Copy All Port URIs” on Ports tab, and “Copy All Sources” on Sources tab.
- **Why:** One-click copy of all displayed/live configs.
- **How:** New helper `DedupeStringsPreserveOrder` and `JoinUniqueLinesText` ensure clipboard output is clean and deduplicated.

## 5. UI: Source normalization & deduplication
- **Files:** `src/win32/imgui_app.cpp`
- **What:** “Normalize Sources” button removes empty lines and duplicate URLs; “Unique sources: X” counter.
- **Why:** Cleaner source management and better UX.
- **How:** `ApplyAdvancedSettings` now dedupes before saving; `SyncBuffersFromConfig` writes deduped values to UI.

## 6. UI Freeze Hardening (already done in previous session)
- **Files:** `src/win32/imgui_app.cpp`, `src/security/dpi_evasion.cpp`
- **What:** All bypass operations run in detached threads with real-time log publishing; UI never blocks.
- **How:** `RunEdgeBypassAsync` uses `publish_edge_progress` lambda to push logs to UI without holding UI mutex.

## 7. Testing
- All unit tests pass (22/22).
- Build succeeds.
- Binaries copied to `release_package/`.

## Usage Notes
- “Copy All” buttons respect the current filter (e.g., “Live” toggle on Configs page).
- Deduplication only collapses literal IPs; hostnames remain distinct.
- GitHub refresh respects the 1‑hour interval and only runs if working configs exist.
- If any UI freeze or unexpected behavior appears, report the exact page/action and I’ll harden that path.
