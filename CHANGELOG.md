# Changelog

> Status: Current
> Last reviewed: 2026-10-29

Notable changes to HBFSim. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/) once releases are tagged. Until
then, the default branch is the only supported version.

## [Unreleased]

### Added

- HBM thermal nodes: one lumped RC node per HBM stack fed by the bytes the
  device serves (`hbm-thermal-*`, enabled in the shipped profiles as
  telemetry), reporting peak and final temperatures, dissipated energy and the
  stack-time above the refresh-doubling and limit temperatures in summaries,
  batch receipts and stop receipts; HBM timing is unchanged.
- A package thermal network for simulation sessions (`package-thermal-*`,
  disabled in the profiles; `package-coupling-liquid-cooled` and
  `package-cascaded-hbf` overlays): a GPU node and symmetric conductances
  between the GPU, HBM and HBF stacks give every stack a time-varying
  environment, so the HBF boundary and light budget follow the neighbors'
  heat; a cascaded HBF deposits link-PHY energy in its HBM base die. Peer and
  Tiered placements couple differently through the traffic they generate.
  The stop receipt carries a `package_thermal` object; HBF stop receipts and
  summaries report `thermal_effective_boundary_c`.
- `hbf-thermal-minimum-light-budget-w`: the floor of the derived light budget
  under a package environment that no media power could hold at LTT.
- Tests: `package_thermal_network` (energy identity, RC relaxation, the
  coupled steady state against the linear system, zero-coupling identity,
  cascaded pass-through) and `package_thermal_coupling` (Tiered-shaped
  traffic heats HBM and the HBF boundary more than Peer-shaped traffic
  through the engine).

### Changed

- The package thermal network refuses an HBF or HBM ambient that differs
  from `package-thermal-ambient-c` and a non-zero
  `hbf-thermal-neighbor-heat-c`: one coolant per package, and the network
  supplies the neighbors, so the `thermal-boundary` overlays do not combine
  with the package overlays (sweep the coolant by changing all three ambient
  keys). The thermal overlay comments state the current parameter values
  (8 pJ/bit, a 0.6 s time constant, 18 W behind a 45 C boundary, about
  86 GB/s at 70 C, fail-closed boundaries from 76 C) instead of the
  superseded ones.
- The HBF thermal parameters of the shipped profiles and the library
  defaults are derived from published sources instead of unsourced
  assumptions: read energy 8 pJ/bit (measured on a hybrid-bonded 1 Tb flash
  die, ISSCC 2025), program 150 pJ/bit and erase 131 uJ/block (SLC data-sheet
  die currents over this profile family's timings, scaled onto the same
  anchor), junction-to-boundary resistance 2.0 C/W (hybrid-bonded HBM stack
  reviews plus TIM and cold plate) and heat capacity 0.3 J/C (the silicon,
  base die and mold of a 16-high stack within the 775 um height budget), so a
  stack's thermal time constant is 0.6 s instead of 7.5 s. The derivation,
  sources, ranges and what they imply for throttling are in
  `docs/reference/thermal-parameters.md`; `parameter-provenance.json` carries
  the sources per key. Thresholds, the 45 C coolant-side boundary, the 2 W
  static power and the derived light budget are unchanged in form; the
  budget behind a 45 C boundary is now 18 W per stack (5.5 W at 70 C).

### Fixed

- Installed images no longer put the same piece of every KV slot on the same
  planes. Compact and materialized initial images and static extents placed a
  stack's i-th page on plane i mod planes-per-stack, so a slot stride that is
  a multiple of stacks x planes per stack (80 MiB for a dense 70B model) left
  each layer's KV on a quarter to half of every stack's planes; each round of
  planes-per-stack pages now starts on a SplitMix64-mixed plane, the in-stack
  counterpart of the stack lane rotation. The public release missed this fix:
  decode with KV in HBF ran up to 42% slower. Persistent images move to v8
  because compact directories decode differently.

- Under the package thermal network the HBM and HBF stacks booted at their
  isolated idle (coolant plus static power times resistance) while the GPU
  booted at its steady state, so every coupled run opened with a warm-up
  transient of about a second that no powered package shows and that biased
  sub-second windows toward cooler stacks and larger light budgets. The
  network now solves its idle steady state (stacks at static power, the GPU
  at its power) and boots every node there, an HBF ceiling start keeping
  LTT; it also exists from time zero of the session instead of from the
  first batch, and the stacks' `thermal_boot_temperature_c` report the
  coupled values. A package whose idle rests above LTT boots its HBF stacks
  in Light mode; one that rests at or above STT is refused.
- `thermal_final_temperature_c` in summaries and stop receipts reported the
  hottest stack at the finish frontier but never below the boot temperature,
  so a stack booted at its light-throttle ceiling that had cooled and left
  Light mode still reported 85 C; it now reports the projected temperature.

### Removed

- The research scaffolding that only served the in-progress paper: the
  ServeLoop-driven experiment suites and their reports, the study runners,
  the paper insert, the study matrices under `configs/studies/`, the workload
  descriptors under `configs/workloads/`, the public-trace and production
  request-trace adapters, the runtime-KV critical-path adapter, the
  capacity-overflow executable and its certificate binding, and the tests,
  CMake targets and CTest registrations that ran them. The mapping-organization
  and host-zone feature tests keep their small configs under `tests/fixtures/`.
- Process documentation: dated study write-ups, review logs, experiment plans,
  figure designs and engineering logs under `docs/studies/`, `docs/reviews/`
  and the research-workflow, ASTRA, miniquick, SGLang, capacity-placement and
  simulator-performance guides.
- Evidence packages that backed no shipped configuration: the H100/Llama 2
  public anchor, the retired A100 real-inference package, the multi-GPU KV
  campaign, the GPU operator profiles and the public-trace statistics. The
  DANA A100 offload measurement and the H200 host-link measurement stay,
  because the `backing/calibrated/` overlays are fitted from them; the H200
  measurement is now registered as a provenance source.
- Study-specific overlays (`fixed-footprint-*`, `e2e-cxl-capacity-512g`,
  `timing-oracle-hbm-capacity-512g`, `dana-qwen3-30b-a3b-offload-capacity`,
  `endurance-dirty-eviction-scaled`, `mapping-read-isolation`, `persistence-*`,
  `q3-endurance-scaled`). The thermal and endurance-window overlays stay with
  study-neutral descriptions.

### Added

- `hbfsim_client.sampling`: stratified sampling of a long batch sequence for
  any frontend that drives the session protocol. A `Sampler` times every
  stratum's first batches and then a declared share, `state_only` reduces a
  skipped batch to the transactions that change device state (writes, link
  copies, peer accesses and barriers, with dependencies folded through the
  dropped reads) and ends it with a barrier at the estimated duration, and
  the report gives the stratified estimate of the window with its standard
  error and 95% interval. ServeLoop's `serveloop run --sample-rate` uses it.
- `tests/cpp/page_lru_test.cpp`, checking the page LRU below against a list
  and a map.
- `hbf-read-mshr-entries-per-stack`, `hbf-read-mshr-targets-per-entry` and
  `hbf-read-mshr-check-ns`: an opt-in page-level read MSHR at each stack's
  ingress. Same-page followers of an in-flight array read skip the scheduler
  issue, L2P lookup and page-read credit; off by default.

- `hbf-ingress-buffer-tags` (research option, needs the read MSHR): the
  ingress check also serves single-page reads of pages still in the read
  buffer or covered by the write buffer, and stages single-page writes to
  pages already in the coalescing write buffer, without the scheduler issue,
  L2P lookup or page-read credit; MSHR entries then live until evicted and a
  logical write retires only its own pages' entries. Off by default.

- The four thermal modes of OCP HBF v0.7.0 section 9.2. The RC node per HBF
  stack now drives normal operation, light throttling (media admission paced
  at the light power budget), severe throttling (the stack asserts CATTRIP,
  completes accepted commands and makes every later host command wait until
  it has cooled below LTT; TMON.CES stays set) and shutdown (the run fails
  closed with the stack, time and temperature). New keys
  `hbf-thermal-severe-c` (STT, 95 C in the shipped profiles) and
  `hbf-thermal-shutdown-c` (105 C); RTT, LTT and STT must sit on the TTTEMP
  register grid (60 C + n × 0.5 C, 60–123.5 C) in RTT < LTT < STT < shutdown
  order. The summary's `hbf_stats`, the session receipts and
  `python3 -m hbfsim list metrics` report paced and halted operations,
  CATTRIP assertions, time in each mode and the peak temperature.
  Example 7 (`examples/07_thermal_throttling.py`) shows the three
  outcomes on one read burst, and the design-space recipes gain a thermal
  recipe. `SimulationSession.final_device_totals` returns the cumulative
  device accounting of the stop receipt, whose `["hbf"]["state"]` carries
  the end-of-session thermal projection.
- `tests/python/test_thermal_llm_physics.py` (CTest `hbf_thermal_llm_physics`)
  checks the thermal model against closed forms under LLM-shaped traffic
  through the engine: media heat equals the per-operation energies, a decode
  loop heats along the RC curve, light throttling caps reads at budget over
  energy per bit and falls with the package boundary, release happens below
  RTT after the RC cooling time, every CATTRIP halt lasts the STT-to-LTT
  cooling time, programs heat an order of magnitude more than reads, and
  garbage collection adds the write amplification and the erases.
- The engine resolves `hbf-mapping-superblock-planes=all` from the HBF
  geometry, so the striped mapping overlays run through the front door and the
  reference runner without a helper script.
- `python3 -m hbfsim`, a dependency-free front door: `doctor`, `build`,
  `quickstart`, `list`, `run`, `sweep` and `show`, plus the `hbfsim` Python
  API (`run`, `sweep`, `load_summary`, `open_session`). Every run records the
  exact simulator command in its own output directory.
- `examples/`: six runnable scripts (placements, FTL mapping trade-offs, the
  read-bandwidth ceiling, custom traces, a custom Python policy, capacity-tier
  media), each registered as a CTest case.
- Onboarding documentation: getting started, concepts and glossary, extending
  guide, design-space recipes with verified commands, research workflows, and
  a Chinese README.
- Community and tooling files: Code of Conduct, CITATION.cff, issue and
  pull-request templates, CMake presets (including `werror`, which reproduces
  the CI compiler flags), a dev container, and `.editorconfig`.
- `published_config_scenario_matrix`: a CTest case that replays the smoke
  trace through every reference scenario on every published system/policy
  pair, so a profile that cannot run a scenario fails loudly.
- The CTest label `serveloop` on every test that needs the frontend checkout,
  and a `serveloop_checkout_compatible` fixture
  (`tests/python/check_serveloop_checkout.py`) that reports a missing or too
  old checkout in one line instead of twenty unrelated failures.

### Changed

- The HBF foreground read path is faster with identical results: a resource
  calendar whose idle intervals are all shorter than a request no longer
  positions a search; the mapping compute worker is chosen with one lookup
  (and none with a single worker); the device DRAM read cache keeps its lines
  in an intrusive LRU (`host/page_lru.hpp`) and the NAND bank cache its
  events in a sorted vector and its candidates in an open-addressing set, so
  a page fill or eviction allocates nothing; the device's program order lives
  in a vector indexed by block; and a plane's media-busy union appends in the
  common in-order case. A recorded long-context decode sweep (32 sequences of
  61K-79K tokens, 2 HBM + 6 HBF stacks) replays 1.4× faster, and a recorded
  prefill-plus-decode pair 1.2× faster, with every completion time, digest and
  device counter equal to the previous engine's.
- The LLM-serving frontend is ServeLoop: the renamed frontend repository
  lives at <https://github.com/FlashLabAE/ServeLoop>, and since
  ServeLoop 0.2.0a1 its Python package and console command are `serveloop`
  as well; imports, documented commands, the checkout compatibility check
  and the minimum revision `HBFSIM_SERVELOOP_MIN_REVISION` (090a65a)
  follow. HBFSim-local names were renamed with the repository: the
  `SERVELOOP_ROOT` environment variable, the `HBFSIM_SERVELOOP_SOURCE_DIR`
  CMake option (default `../ServeLoop`), the `--serveloop-root` experiment
  option, and the
  files `docs/guides/serveloop.md`, `tests/python/test_serveloop_frontend.py`,
  `tests/python/test_serveloop_persistence.py` and
  `experiments/hbf_persistence/serveloop_recovery.py`. Serialized receipt and
  manifest field names are unchanged.
- CI is organized by purpose: `quickstart` (the out-of-box path on Python
  3.10 without the frontend), `build-and-test` (GCC/Clang, Debug/Release,
  `-Werror`, tests not labelled `serveloop`), `frontend-integration` (ServeLoop
  pinned to 585c563; red until that revision is on ServeLoop's public `main`),
  `deep-verification` (component and write-amplification gates) and
  `sanitizers`; the mutation gate runs weekly and on demand (about 38 minutes).
  Steps that duplicated CTest cases (the provenance gate, `verify_foundation`)
  were dropped.
- Compiler warning options live in one place (`HBFSIM_WARNING_OPTIONS` in
  `cmake/Targets.cmake`) and apply to every target; the designated
  initializers that deliberately omit value-initialized members no longer
  trigger `-Wmissing-field-initializers`.
- The independent oracles follow the documented host mapping ledger
  (`mapping_lookup_compute` / `mapping_update_compute` on a per-stack compute
  pool, one GC mapping publication per relocated page) and the HBM
  service-group calendar; the ledger reducer resolves HBM at service-group
  granularity and bounds per-lane counters. 27 of 35 canonical cases had been
  failing silently because the cases gate stops at the first failure.
- The reference-policy profiles paired with the four topology systems place
  `flat-hbm-bytes` and `static-direct-hbm-bytes` at the cooperative write
  region's base (application HBM minus the write buffer, where application
  HBM excludes the controller-DRAM reservation), so `flat` and `direct-read`
  run on them.
- `core_system_config_*` validate profiles with `--describe-system`; the
  previous invocation read stdin to EOF and hung under harnesses that leave
  stdin open.
- `hbf_mapping_layout_control_contract`: the layout-matched control demanded
  identical array page reads from all three variants, but a bank's two decoded
  pages are served at the instant a request reaches the bank, and page-mapped
  reads reach it after per-stack mapping-DRAM issue serialization staggers
  them, so the `hot_cold` re-reads hit differently (they did at every revision
  since the study was added). The contract (schema v2) now requires identical
  demanded pages, identical media traffic between the bypass and the static
  extent, and bounds the array-read difference by the decoded-buffer hit delta.
- `hbf_persistence_long_context_contract`: the HBM-fronted cell relied on the
  address-only remapper filling KV read misses into HBM, which its default
  stopped doing on 2026-09-23. `HbmFrontedBackingRemapper` takes an explicit
  `hbf_kv_read_policy` (`direct_gpu_read_on_miss`, the unchanged default, or
  `read_allocate_hbm_cache`); the study declares the latter in its contract and
  fails closed unless every receipt reports it.
- README reorganized around users: quick start, questions to explore, how the
  simulator works, and a repository map by audience.
- Certificate schema v8 binds the `hbfsim-reference`, `ledger_probe` and
  `physical_probe` executables; the capacity-overflow binding is gone with the
  executable.
- The CTest group `Tools.cmake` (was `Studies.cmake`) holds the workload
  tools, summary observability and report tests; `workload_quality_contracts`
  (was `behavioral_workload_quality_contracts`) and the newly registered
  `simulation_transaction_completions_contract`.
- Repository URLs point at `FlashLabAE/HBFSim`.
- `import hbfsim`, and so `python3 -m hbfsim doctor`, exits with the
  interpreter's path and version on Python older than 3.10 instead of a
  `TypeError`.
- Documentation index organized by audience; all maintained documents carry
  status metadata, and links into untracked local outputs were replaced with
  plain path references.

### Fixed

- Every target compiles with `-Wall -Wextra -Wpedantic -Werror` on GCC 13 and
  Clang 18, which CI and the mutation gate both require; the gate had been
  dying at compile time. Misleading indentation in the mixed-FTL endurance
  driver, a by-value structured binding in `gap_calendar_test`, references to
  members of temporaries in the HBF contract probes and a Release-only
  maybe-uninitialized report in `hbf_mapping_organization_test` were fixed.
- `experiments/hbf_endurance/native_zone_lifetime.cpp` failed to compile on
  Linux (`std::uint64_t` is `unsigned long` there), which stopped a default
  `cmake --build`.
- `parameter_provenance_coverage`: `configs/studies/paper/lifetime-system.cfg`
  set six keys twice (a base profile followed by its overlay section); the
  duplicates are removed with the resolved configuration unchanged, and six
  engine keys used by tracked overlays are now in the provenance registry.
- `system_config_contracts` and `reference_config_roundtrip` iterate the
  published, policy-paired system profiles; `configs/systems/sglang-small.cfg`
  is a ServeLoop overlay fragment and is pinned as the only unpaired file.
- `summary_time_breakdown_contract` expected HBM refresh, precharge and
  activation columns that the channel-aggregate HBM model replaced.
- The canonical case `hbm.dual-pch-overlap` targeted one lane twice; it now
  exercises two lanes of one service group.
- The simulation-client README example reused a retained transaction id and
  was rejected by the engine.
- The `documentation_contract` and `project_structure_contract` tests pass
  again: missing status metadata and index entries, links to untracked `out/`
  results or removed files, coverage rows citing retired tests, uncataloged
  modules and configs, and two evidence packages without `__init__.py`. The
  structure gate now skips `--help` checks that need a declared-optional
  dependency (the `analysis` extra or ServeLoop) when it is not installed.
- `CONTRIBUTING.md` named a summary metric that no longer exists
  (`user_ack_throughput_GBps`; the field is `user_completion_throughput_GBps`).
