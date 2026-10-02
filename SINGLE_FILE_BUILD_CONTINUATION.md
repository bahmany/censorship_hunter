# Hunter — Single-File Build: Continuation Prompt

Paste this whole file into `qwen` (or hand it to any coding agent) to pick up
exactly where a previous session left off. It died mid-task to a connection
error, not because the work was rejected — everything described as "done"
below has been verified against the actual repo state on disk.

Repo root: `/home/mohammad/Documents/projects/censorship_hunter`

## Original request (verbatim, from the project owner)

> i linux and windows and android i want the project final result in just one
> file and all engine must be inside the main binnary file and all in one
> file, do whatever need, the final deployed file must be just one file and
> all deps must be inside, and when we build final deployed and bin file,
> inside it must be latest downloaded x2ray configs (it could be more than
> 300k configs)

## Definition of done

For each of the three targets, the **only** build artifact a user needs is:

| Platform | Artifact | Notes |
|---|---|---|
| Linux x86-64 | `bin/hunter` | statically linked, no `bin/xray`/`bin/sing-box` alongside it |
| Windows x86-64 | `bin/windows/hunter.exe` | statically linked, no DLLs, no companion `.exe`s |
| Android arm64 | `bin/android/hunter.apk` | already a single file by construction (zip container) — the gap is that it doesn't yet carry the config bundle |

Each binary must, with **zero external files and zero network access on
first run**, contain:

1. The xray and sing-box engine binaries (already implemented for Linux;
   partially implemented for Windows; already implemented for Android via a
   different mechanism — see below).
2. A bundle of the latest scraped v2ray/xray proxy configs — target is
   **300,000+** URIs — downloaded fresh at *build time* from the project's
   25+ public GitHub sources, deduplicated, and baked into the binary so the
   app has working proxy candidates the instant it launches, before any live
   scraping happens.

Live scraping/refreshing from the network at runtime must continue to work
exactly as before — the embedded bundle is a head start, not a replacement.

## What already exists and is verified working (do not redo)

### New tooling
- `hunter_cpp/tools/download_configs.py` — downloads configs from all seed
  sources (`hunter_cpp/include/core/constants.h`'s `githubRepos()` /
  `antiCensorshipSources()` / `iranPrioritySources()` lists — 25+ URLs),
  dedupes, filters to `scheme://` URIs, writes one bundle file. Takes
  `--outfile`, `--timeout`, `--workers`, `--min-configs`.
- `hunter_cpp/tools/embed_configs.py` — compresses the bundle with `zstd -19`
  and emits `config_embedded.h` / `config_embedded.S` (`.incbin` assembler) /
  `config_meta.json`, mirroring the existing `embed_engines.py` pattern
  exactly. Handles the "no bundle" case by emitting a stub with
  `HUNTER_EMBED_HAS_CONFIGS 0` so the build never hard-fails.

### New runtime module
- `hunter_cpp/include/core/config_embed.h` + `hunter_cpp/src/core/config_embed.cpp`
  — mirrors `core/engine_embed.*`. Exposes `hasEmbeddedConfigs()`,
  `configs()` (decompresses once via the vendored `zstddeclib.c`, memoized
  with `std::once_flag`), `configCount()`.
- Wired into `hunter_cpp/src/orchestrator/orchestrator.cpp` (see the
  `PHASE 1b: Load embedded configs` block, right after the existing
  `HUNTER_all_cache.txt` load and before `detectCensorship()`). It loads the
  embedded bundle into `config_db_` via `addConfigs(..., "embedded")`, additive
  with whatever was already loaded from disk cache.
- `constants::CONFIG_DB_MAX_SIZE` is `600000` — already comfortably fits a
  300k+ bundle, no change needed there.

### Linux build (`hunter_cpp/CMakeLists.txt`) — COMPLETE AND CORRECT
Fully wired: `HUNTER_EMBED_CONFIGS` / `HUNTER_DOWNLOAD_CONFIGS` options,
the `download_configs.py` → `embed_configs.py` custom-command pipeline,
`config_embedded_obj` OBJECT library, and — critically — `${ZSTD_SOURCES}`,
`${HUNTER_EMBED_OBJECT}` (engines), and `${HUNTER_CONFIG_EMBED_OBJECT}`
(configs) are all added to `add_library(hunter_core STATIC ...)`, with
matching `target_include_directories`. This one file needs no further edits
for the config side. Verify it still builds (see Task 4 below) — it has
never actually been compiled since these edits landed.

## What is incomplete — this is the actual remaining work

### Task 1 — Fix `hunter_cpp/CMakeListsMingw.txt` (Windows build is broken)
The same `HUNTER_EMBED_ENGINES` / `HUNTER_EMBED_CONFIGS` custom-command
blocks were copy-pasted in (lines 32–130) and `enable_language(ASM)` was
added, but unlike the Linux file, **the generated objects and includes were
never actually attached to the `hunter_core` library**:

- `add_library(hunter_core STATIC ...)` (around line 174) only lists
  `${CORE_SOURCES} ${NETWORK_SOURCES} ${PROXY_SOURCES} ${CACHE_SOURCES}
  ${ORCHESTRATOR_SOURCES}` — it is **missing** `${ZSTD_SOURCES}`,
  `${HUNTER_EMBED_OBJECT}`, and `${HUNTER_CONFIG_EMBED_OBJECT}`.
- There is no `target_include_directories(hunter_core PUBLIC ${ZSTD_DIR}
  ${HUNTER_EMBED_INCLUDE_DIR} ${HUNTER_CONFIG_EMBED_INCLUDE_DIR})` anywhere
  in the file.

Fix by mirroring `hunter_cpp/CMakeLists.txt` lines 208–237 exactly (same
variable names already exist in the Mingw file — only the library
attachment and include-dir propagation are missing). Also confirm the
`.incbin` path in the generated `.S` resolves correctly under
`x86_64-w64-mingw32-gcc` (assembler is the same GNU `as`, should be fine,
but this has never been build-tested).

Also note `CMakeListsMingw.txt` is currently **untracked by git** (`git
status` shows `??`, not `M`) — check whether that's intentional (a
gitignored local file) or whether it should be added; don't fight this,
just be aware when diffing.

### Task 2 — Actually run and validate the config download at scale
`download_configs.py` has never been executed against the real network. Run
it standalone first, outside CMake, to sanity check before wiring it into a
full build loop:

```
python3 hunter_cpp/tools/download_configs.py \
    --outfile /tmp/configs_bundle_test.txt --timeout 30 --workers 16 --min-configs 100
wc -l /tmp/configs_bundle_test.txt
```

Confirm: how close to 300k unique configs do the 25+ sources actually yield
today (source lists drift/die over time — some of the GitHub raw URLs in
`constants.h` may 404 now). If the real yield is well under 300k:
- Don't fabricate configs to hit the number.
- Consider whether additional known-good public sources should be added to
  `constants::githubRepos()` / `antiCensorshipSources()` /
  `iranPrioritySources()` (same file used by both the live scraper and the
  build-time downloader — one source list, two consumers, keep it that way).
- Report the real achieved count back to the user rather than silently
  padding or lowering `--min-configs` to mask a shortfall.

### Task 3 — Android: bundle the config set as an APK asset
Android already achieves "single file" structurally (`hunter.apk` is a zip
containing everything, including `assets/engines/xray` and
`assets/engines/sing-box` per `hunter_android/build_apk.sh`) — **do not**
try to force the desktop `.incbin` mechanism into the NDK build; follow the
existing asset pattern instead, it's simpler and already proven for the
engine binaries.

Steps:
- Extend `hunter_android/build_apk.sh` to also produce (or accept a
  pre-built) `assets/configs.zst` — reuse `download_configs.py` +
  `embed_configs.py --no-zstd`-style output, or just zstd-compress the raw
  bundle directly with the `zstd` CLI (no need for the `.incbin`/`.S`
  generation step at all on Android — a plain compressed file in `assets/`
  is enough).
- In `hunter_android/app/src/main/cpp/` (see `android_app.cpp` /
  `jni_bridge.cpp`), add the read path: open `configs.zst` via Android's
  `AAssetManager`, decompress with the same vendored `zstddeclib.c`
  already used by `config_embed.cpp`, and feed the resulting URI list into
  `config_db_->addConfigs(..., "embedded")` at startup — same call site
  concept as the desktop orchestrator change, adapted for the
  asset-manager read instead of the `.incbin` symbol read.
- Check `hunter_android/app/build.gradle` — there's already an
  `aaptOptions`/asset no-compress rule for the engine binaries (search
  "must not be compressed" near line 78); add `configs.zst` to the same
  no-compress exclusion list if applicable (zstd output is already
  compressed, don't let the APK packer re-compress and waste time/size).

### Task 4 — First real end-to-end build + verification (Linux)
Nothing has been compiled since any of this landed. Do this first, before
touching Windows/Android further, since Linux is the fastest feedback loop
and shares the runtime code (`config_embed.cpp`, orchestrator wiring) that
all three platforms depend on:

```
cd hunter_cpp/build
cmake -DHUNTER_EMBED_ENGINES=ON -DHUNTER_EMBED_CONFIGS=ON -DHUNTER_DOWNLOAD_CONFIGS=ON ..
ninja hunter
```

Verify:
- Build succeeds (watch specifically for the `config_embedded_obj` /
  `.incbin` step and any zstd/link errors).
- `./bin/hunter` launch log shows the `[Startup] Loading embedded config
  bundle...` / `[Startup] Loaded N configs from embedded bundle` lines from
  the orchestrator change.
- `ldd bin/hunter` shows no dependency on `bin/xray`/`bin/sing-box` being
  present alongside it — move/rename `bin/xray` and `bin/sing-box` and
  confirm `hunter` still extracts and runs its own engines from the
  embedded copies (this is the existing `hunter_cpp/tests/test_embed.cpp`
  check, extend the same idea to configs).
- Final `bin/hunter` file size is reasonable (expect tens of MB, given the
  two ~10MB zstd engine blobs plus a compressed multi-hundred-thousand-line
  text bundle).

### Task 5 — Windows and Android builds, same verification bar
Once Linux is proven, apply the same `HUNTER_EMBED_ENGINES=ON
HUNTER_EMBED_CONFIGS=ON` build to the MinGW cross-build (after Task 1's fix)
and rebuild `hunter_android` via `build_apk.sh` (after Task 3's asset
wiring). For each, confirm the produced single file (`hunter.exe` /
`hunter.apk`) runs with **no companion files present** and loads the
embedded config bundle on first launch.

### Task 6 (optional but requested implicitly — "do whatever need") — one build script
Create `hunter_cpp/tools/build_single_file.sh` that runs all three builds in
sequence (or accepts a `--platform linux|windows|android|all` flag),
downloading configs once and reusing the same bundle across platforms rather
than re-downloading per target (the sources are the same regardless of
target OS — no need to hit GitHub three times for the same data). Have it
print a final summary table of the three output file paths + sizes +
embedded config counts.

## Constraints / things not to touch

- Don't touch the mass of unrelated deleted files showing in `git status`
  (Dockerfiles, docs/, deploy scripts, etc.) — that predates this task and
  is out of scope.
- Don't lower `--min-configs` or fabricate/duplicate configs just to hit
  "300k" — report the real number achieved from live sources.
- Keep the single source-of-truth for GitHub source URLs in
  `hunter_cpp/include/core/constants.h` — both the live scraper
  (`ConfigFetcher`) and the new build-time `download_configs.py` should read
  from (or stay in sync with) that same list, not diverge into two
  hardcoded lists that drift apart.
