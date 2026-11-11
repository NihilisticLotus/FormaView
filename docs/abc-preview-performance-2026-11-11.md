# ABC preview performance investigation — 2026-11-11

Measured on `main`, starting commit `8eba7361ad51b262624ad7b4057dae197101954e`.
No commit, push, application package, installation or signing was performed.
The pre-existing untracked `.agents/` and `tools/runtime/installer_task_history.py`
were left untouched.

Phase 2 now sustains approximately 30 sample updates/s on this asset with the
production QML inspector present and the complete moving model kept visible.
Both complete-timeline runs presented updates 1–239 without skipping a sample;
one final sample needed a separately measured 3.84 ms drain after playback ended.
Presentation intervals still reach about 51 ms, so this is not a locked 30 FPS
claim. Phase 1 measurements below remain as historical evidence.

## Input and machine

- Input: `B:\Model\AA.ABC`, read only; 528,687,173 bytes, modified
  `2026-11-11T06:31:57Z`. No output or cache was created beside it.
- SHA-256 before and after:
  `4a92a439ccd7971c197256d026d9c7bc9a760781ceb2df7c5c6978e8387931f0`.
- Houdini 20.5.487 Ogawa archive: 240 merged samples at 24 Hz, 67 polygon
  meshes, 90,085 points, 89,942 polygons, 359,794 corners, 179,910 triangles.
- Windows 11 Enterprise 26100; Ryzen 9 9950X3D; 203,595,677,696 bytes RAM.
  Installed GPUs include RTX 5090 D (driver 32.0.16.1742) and AMD integrated
  graphics. The probe selects D3D11; adapter identity was not independently
  captured, so the results should not be treated as a verified RTX-only run.
- MSVC 14.44, Qt 6.12.0, Alembic 1.8.12; optimized Release test executables.

## Path and cause

1. `readModelDocument` opens a `ModelAbcSource`, holding a read-only input
   lease/hash and indexing the archive. Subsequent samples use the same archive.
2. `tickAbc` advances real time and chooses the nearest merged sample. Its
   16 ms timer was functioning. Sampling runs in a `std::async` worker; archive
   reads are serialized, with existing digest-based mesh/topology reuse.
3. Every decoded sample becomes an immutable `ModelDocument`. It transforms
   positions/normals, builds corner-domain data and runs the common geometry
   finalizer. Another worker builds the full triangle-order render frame.
4. The 20 ms session poll used to collect both stages. When reads missed the
   latest request, their documents were cached but never landed. Advancing
   playback repeatedly superseded them: the clock moved while the image stayed
   on an old sample. Baseline run 1 performed 150 reads and recorded 144
   superseded reads but displayed only three updated samples in six seconds.
5. ABC frames were also marked GPU-cacheable like static inspection states.
   `Renderer::saveActive` moved active resources into the resident LRU for each
   new sample. Cache misses then allocated buffers, textures, bindings and
   pipelines again. GPU execution itself was around 1.7 ms, far smaller than
   the CPU read/build and scheduling costs.

The first 60 sequential samples measured document read p50/p95 of
28.11/33.28 ms and frame build 15.91/16.54 ms. A separate baseline stage probe
measured archive decode 13.49/13.94 ms and geometry finalization 5.44/5.65 ms.
These microbenchmarks run serially and must not be summed as a pipelined
playback frame interval.

## Phase 1 changes

| Files | Behavior |
|---|---|
| `src/host/model_session.cpp`, `.hpp` | Forward playback can land the newest completed sample between the presented position/explicit seek boundary and the current clock. Explicit paused seeks remain exact. Preparing frames are allowed to finish; useful in-flight prefetch reads are retained. Backward/explicit seek boundaries, cancellation, generation isolation and end/loop behavior remain enforced. |
| `src/host/model_abc.cpp`, `.hpp` | Bounded cache query for completed samples within that playback window. |
| `src/host/model_session.cpp` | ABC read/frame workers queue a GUI wake on completion. Future readiness is still checked; the existing poll remains the fallback if a wake arrives before async thread exit. Worker futures join during session destruction; queued calls use the session QObject context. |
| `src/host/model_frame.cpp` | ABC frames stream through the existing dynamic resource path instead of moving resources into the static GPU LRU. CPU sample/frame cache limits are unchanged. |
| `src/host/model_session.cpp`, `src/formats/geometry_cache.cpp` | Skip face-normal calculation when all three corner normals already exist; reserve the decoded position array before filling it. Missing normals still use the existing fallback. |
| `tests/model_abc_tests.cpp`, `tests/model_render_tests.cpp` | Catch-up window, fast requests, playback seek, queued-wakeup teardown, streaming policy, changing vertex count, backward reuse and GPU release regressions. |
| `tests/model_abc_performance.cpp`, `src/host/CMakeLists.txt` | Opt-in real-asset benchmark with actual submitted payload identities at `frameSwapped`, CPU stage timing, renderer observations, sample order, misses and GUI heartbeat. |

No timer speed increase, triangle reduction, lower render resolution, normal/UV
omission or all-frame preload was introduced.

## Phase 1 before and after

Identical 1280×720 `ModelView`, MSAA 4, D3D11, ordinary shaded mode. Playback
speed is 1.25 to request the 24 Hz source at 30 sample updates per wall second.
Each row is six seconds, with 180 expected updates. Two runs use the same
session; returning to sample 0 precedes run 2. OS caches were not flushed.

| Measurement | Baseline 1 | Baseline 2 | Final 1 | Final 2 |
|---|---:|---:|---:|---:|
| Presented sample updates | 3 | 9 | 173 | 164 |
| Effective updates/second | 0.50 | 1.50 | 28.83 | 27.33 |
| Missed updates | 177 | 171 | 7 | 16 |
| Interval p50, ms | 40.80 | 263.28 | 31.48 | 37.11 |
| Interval p95, ms | 5145.00 | 2463.03 | 49.90 | 43.37 |
| Longest interval, ms | 5145.00 | 2463.03 | 56.36 | 57.16 |
| Renderer prepare p95, ms | 7.91 | 10.85 | 4.53 | 4.64 |
| Peak renderer-owned GPU bytes | 97,169,984 | 97,169,984 | 32,390,112 | 32,390,112 |
| GUI heartbeat longest gap, ms | not recorded | not recorded | 4.94 | 4.48 |

Baseline interval statistics contain very few samples and should be read with
their update counts. Presented updates count distinct submitted sample payloads
at swap, not timer ticks or repeated redraws; expected-minus-presented is a
missed-update proxy, not a physical monitor presentation counter.

Intermediate checks isolated the effects: scheduling alone produced 143/131
updates; avoiding unnecessary normal work gave 148/141; queued wakeups gave
171/157; streaming resources gave 172/164. These are independent noisy runs,
not additive gains. The final source was rebuilt and measured again for the
table above.

Final document read p50/p95 is 26.82/32.05 ms; frame build is 16.13/16.58 ms.
This change primarily removes starvation, stage waits and resource churn;
it does not claim a dramatic CPU decode speedup.

Compact machine-readable results, raw presentation intervals and sample order
are in `abc-preview-performance-2026-11-11.json`. Full local outputs and build
logs are under `out/abc-performance/` and `out/abc-*.log` (ignored build data).

## Phase 1 verification and reproduction

Development build only:

```powershell
cmake --build out/msvc --config Release --target formaview_model_abc_performance formaview_model_abc_tests formaview_model_render_tests formaview_model_state_tests --parallel 12
$env:PATH = 'E:/Qt/6.12.0/msvc2022_64/bin;E:/GameUnpackProject/tools/DirectXTex-exr-source/build-exr/vcpkg_installed/x64-windows/bin;' + $env:PATH
./out/msvc/Release/formaview_model_abc_performance.exe B:/Model/AA.ABC E:/CodexProject/FormaView/out/abc-performance/recheck.json
./out/msvc/Release/formaview_model_abc_tests.exe E:/CodexProject/FormaView/out/abc-performance/recheck-tests B:/Model/AA.ABC
./out/msvc/Release/formaview_model_render_tests.exe E:/CodexProject/FormaView/out/abc-performance/recheck-render.json
```

Observed results:

- ABC session regressions pass, including real-file random seeks checked against
  direct archive reads for positions, topology, UVs, authored normals and counts.
  Existing empty/varying topology, static mesh reuse, cancellation, archive
  switch, malformed samples and unsupported schemas also pass.
- Fast synthetic playback produced 80 distinct publications in the 650 ms
  starvation guard; its ordering and explicit seek checks pass. This is not a
  portable real-asset throughput assertion.
- D3D11 material/readback regressions: 64 assertions pass, including new
  streaming resize/backseek/clear verification.
- Broader model-state/independent export regressions: 5,084 assertions pass.
- Existing ASan `formaview_professional_tests abc` target rebuilt and passed
  against generated workspace fixtures. This covers archive decoding, not the
  Qt host worker lifecycle under ASan. No UBSan host run was performed.
- `git diff --check` passes. Input size, modification time and SHA-256 remain
  unchanged after the test runs.

## Phase 1 remaining boundaries (before follow-up)

The observed result is approximately 27–29 updates/s with occasional 43–57 ms
intervals, not a stable locked 30 FPS. Full corner-domain documents and complete
triangle-order VBOs are still rebuilt/uploaded for changed samples: about
32.4 MB per update, and 5.3–5.6 GB uploaded per measured six-second run. Future
work can separate static attributes/topology from deforming data, but needs
additional contracts for changing topology, UVs, normals and transforms.

The benchmark uses the actual model/session/renderer but excludes the complete
QML inspector, OS input, physical display scanout and Houdini measurement.
No full product end-to-end 30 FPS claim or cross-machine guarantee is made.
Byte/count cache bounds remain in place; no unbounded prefetch was added.

## Phase 2 changes

The remaining work had three distinct costs: rebuilding all triangle vertices,
uploading all vertex attributes, and recreating the inspector's per-mesh readout
objects whenever its QVariantList model changed. A separate timing error also
accumulated across playback ticks: `QElapsedTimer::restart()` returned integral
milliseconds, discarding a fraction on every tick. In a six-second trace the
clock requested only sample 175 despite a nominal target of 180. Keeping
nanosecond elapsed time removes that drift without changing the timer interval
or requested playback speed.

- `model_frame.cpp` builds an ABC deformation payload containing positions and
  normals (24 bytes per render vertex). An immutable base holds UVs, colour,
  coverage, draw ranges, guides and source-corner mapping. Its signature checks
  archive identity, display space, topology/UV content digests, domain sizes,
  visibility, isolation and UV selection. Missing digests disable reuse;
  changing topology or UVs rebuilds the base. UV inspection, heat/collision,
  selected-node guides and session texture overrides use the ordinary builder.
- `render_item.cpp` binds a 36-byte static attribute stream and the 24-byte
  deformation stream to the existing shader. Subsequent compatible frames
  upload only deformation. Buffer resize, empty frames, mode transitions and
  rejected payload recovery invalidate the appropriate resources. A malformed
  deformation cannot permanently reject a later valid payload sharing its base.
  The combined vertex budget remains 256 MiB.
- `geometry_cache.cpp` memoizes validated, expanded UV arrays by content digest
  with a 64 MiB archive-wide limit. Memo hits still validate the current topology
  domain. A constant UV array with an incompatible later point count is rejected.
- `ModelInspector.qml` uses the mesh count for its Repeater and binds each row
  to the current indexed metadata. The actual 67 readout objects survive sample
  changes. This preserves all per-sample values while avoiding object recreation.
- The 96 MiB/four-entry CPU frame cache conservatively charges the shared base
  to every entry, as well as deformation and signature storage. The existing
  256 MiB/64-sample document cache and two-sample prefetch window remain bounded.

The first 60 sequential reads built the static base once. In the final
six-second QML probe, document read p50/p95 was 19.27/24.33 ms. Ordinary complete
frame building remained 15.83/16.25 ms; the streaming builder measured
3.86/4.19 ms including its first base build. Archive decode measured 6.18/8.94 ms.
These serial microbenchmarks are not a sum of the pipelined playback interval.

## Phase 2 measurement definitions

The probe now supports the actual `ModelPane`, complete `ModelInspector` scene
tab and `CacheTimeline`. It uses the same fixed 1280×720 colour buffer and MSAA4,
in a 1600×824 QML window. All 67 meshes and 179,910 triangles remain submitted.
No geometry, normals, UVs or render quality are removed.

Default-camera six-second runs preserve the earlier camera policy for comparison.
Screenshot inspection showed that the animated model moves out of that view
late in the timeline. Additional full-timeline runs therefore follow the bounds
of **all** current geometry in the probe, keeping the model visible. This is
solely a benchmark camera; production camera behavior is unchanged. These runs
are reported separately and were visually checked in
`out/abc-performance/v2-visible-qml.png`.

`noninitial_updates` counts distinct submitted samples at swap, excluding the
already displayed sample 0. The old `presented_updates`/`effective_fps` counters
can include sample 0; JSON retains them only for historical comparison.
At the fixed measurement deadline, playback pauses; any final in-flight sample
then drains separately. `skipped_sample_indices` checks the complete presentation
order through that paused request. Drain time is never added to the fixed-window
update count. A frame still in flight at the boundary is distinguished from a
skipped sample. Physical display scanout is not measured.

Each configuration ran twice in one session, seeking back to zero first, with
warm OS caches. Final measurements ran without parallel builds or tests.

## Phase 2 results

Six-second comparison, default camera; update counts below exclude sample 0:

| Measurement | Phase 1 QML 1 / 2 | Phase 2 isolated 1 / 2 | Phase 2 QML 1 / 2 |
|---|---:|---:|---:|
| Noninitial updates | 105 / 85 | 180 / 180 | 179 / 180 |
| Updates/s | 17.46 / 14.10 | 30.00 / 30.00 | 29.83 / 29.99 |
| Interval p95, ms | 72.06 / 78.22 | 49.64 / 44.21 | 47.20 / 47.05 |
| Longest interval, ms | 76.41 / 90.01 | 50.35 / 50.91 | 50.86 / 50.42 |
| Renderer prepare p95, ms | 4.82 / 5.07 | 1.95 / 1.96 | 1.98 / 2.05 |
| GUI heartbeat longest gap, ms | 42.53 / 46.36 | see JSON | 9.83 / 10.06 |

The phase 2 QML first run presented sample 180 after a further 1.62 ms drain;
neither run skipped any requested sample. Phase 1 did not have a drain trace,
so its old missed-update proxy cannot prove which frames were skipped versus
still in flight.

Complete timeline, model kept visible; 239 noninitial updates requested:

| Measurement | Isolated 1 | Isolated 2 | Full QML 1 | Full QML 2 |
|---|---:|---:|---:|---:|
| Noninitial updates before stop | 239 | 239 | 239 | 238 |
| Updates/s before stop | 29.99 | 29.99 | 29.99 | 29.87 |
| Updates presented including final drain | 239 | 239 | 239 | 239 |
| Skipped sample indices | 0 | 0 | 0 | 0 |
| Interval p95, ms | 49.71 | 48.76 | 47.15 | 45.15 |
| Longest interval, ms | 62.40 | 68.30 | 50.90 | 50.23 |
| Final drain, ms | 0.79 | 0.71 | 0.63 | 3.84 |
| GUI heartbeat longest gap, ms | see JSON | see JSON | 10.02 | 10.73 |

Both QML runs retained all 67 original inspector readout objects. Default-camera
full-timeline results are also preserved in JSON: both configurations presented
every update through sample 239 after draining the final request.

For 539,730 render vertices, deformation upload is **12,953,520 bytes/update**,
down from 32,383,800 bytes for the full VBO: **60% fewer vertex bytes**. Total
six-second uploads are now 2.319/2.332 GB with QML, compared with phase 1 isolated
5.603/5.312 GB despite its lower update count. Peak renderer-owned GPU allocation
remains 32,390,112 bytes: splitting the buffer improves transfer cost, not total
resident vertex storage. This is renderer accounting, not total process VRAM or
peak process RAM. Full raw renderer observations and sample intervals remain in
the local benchmark outputs; compact results, source hashes and machine details
are in the report JSON under `phase2`.

## Phase 2 verification and reproduction

Fresh Release and MSVC host ASan builds passed:

- ABC session tests with generated fixed/varying/empty topology, stable and
  animated UVs/normals, invalid cached-UV domains, exact seek, backward seek
  during playback, cancellation, archive switches, queued completion teardown
  and the real asset. Random real samples compare decoded positions, topology,
  UVs, normals and counts against fresh archive reads. Every checked split
  render vertex matches the ordinary builder's positions, normals and shading
  attributes exactly.
- 69 actual GPU assertions in both Release and ASan, including deformation-only
  uploads, resize/backseek/clear, malformed deformation followed by recovery on
  the same base, ordinary/split layout transitions and matching UV/normal/colour/
  coverage pixel readback. Empty-frame allocation accounting returns to the
  176-byte uniform allocation; renderer teardown is also checked.
- 5,084 broader model-state and independent export assertions.
- Actual desktop D3D11 workflow tests `abcGeometryCachePreview` and
  `abcCurrentFrameExport`: 4 QtTest passes including setup/cleanup, zero failures
  or skips. These exercise timeline controls, labels, varying topology, archive
  switches and generated-fixture current-frame export through the complete app.
- `git diff --check` passes. The real input size, modification time and SHA-256
  were checked again and remain unchanged. No output was written beside it.

Relevant logs are `out/abc-v2-regression.log`, `out/abc-v2-render.log`,
`out/abc-v2-model-state.log`, `out/abc-v2-asan-host.log`,
`out/abc-v2-asan-render.log` and `out/abc-v2-desktop-tests.txt`.

```powershell
./out/msvc/Release/formaview_model_abc_performance.exe B:/Model/AA.ABC E:/CodexProject/FormaView/out/abc-performance/recheck-isolated.json --full --follow
./out/msvc/Release/formaview_model_abc_performance.exe B:/Model/AA.ABC E:/CodexProject/FormaView/out/abc-performance/recheck-qml.json --qml --full --follow
```

Host ASan uses the isolated `out/abc-host-asan` build, `FORMAVIEW_ASAN=ON`,
RelWithDebInfo and the existing ASan Alembic dependency. Map imported
RelWithDebInfo dependencies to Release and include the MSVC ASan runtime,
Qt and dependency DLL directories in PATH. It covers the changed host/archive
code and lifecycle tests; external Qt/OpenEXR/Imath/D3D runtimes are not
instrumented. No UBSan run was performed.

## Phase 2 limits (short traces)

The asset now delivers approximately 30 updates/s without sample loss in these
complete-timeline traces. It still has uneven presentation spacing: QML p95 is
45–47 ms, and an isolated visible run reached 68.30 ms. Strict 33.33 ms pacing,
physical monitor presentation and cross-machine 30 FPS are not established.
The 16 ms request cadence, asynchronous completion and Qt/D3D11 presentation
remain possible contributors; these traces do not isolate one cause.

The performance harness includes the actual pane/inspector/timeline, not all
of Main/Desktop, history activity or OS input. Separate desktop integration
tests validate correctness, not sustained throughput with every application
service active. The tested adapter was not independently identified by this
probe. These measurements do not include Houdini or a direct Houdini comparison.

## Phase 3: continuous-loop stability

The fixed acceptance run completed **230 natural loops in 30.50 minutes** in one Release process, with the actual production pane, complete 67-row inspector and timeline. A probe-only camera followed all current geometry. The first 200 loops had no control changes; loop 200 paused for two seconds and resumed, followed by four playing seeks starting at loop 210. All 67 original inspector row objects survived.

The conclusion is sustained near-30-update/s throughput without progressive slowdown in this run. It does **not** establish lossless playback: clean loops contain 33 requested-but-not-presented samples out of 54228 unique requests (0.06%). Requested sample zero was not presented in loops 14, 26, 44, 122. 12 source samples were never requested by the playback clock and are counted separately. There were 0 nonmonotonic sample transitions in clean loops.

Loop zero is excluded from clean comparisons because sample zero was already visible at setup. FPS counts distinct payload updates at frameSwapped; the inclusive 240-sample clip spans 239 source intervals, so a natural loop can average slightly above 30. No counter adjustment is used. Intervened loops are excluded from the following comparison.

| Group (zero-based loops) | Loops | Median updates/s | P50 ms | P95 ms | P99 ms | Max ms | Requested omissions |
|---|---:|---:|---:|---:|---:|---:|---:|
| early_1_19 | 19 | 30.12 | 31.40 | 49.79 | 54.84 | 71.30 | 2 |
| warmed_20_49 | 30 | 30.10 | 31.40 | 49.72 | 54.90 | 73.21 | 6 |
| late_170_199 | 30 | 30.10 | 31.41 | 49.81 | 56.09 | 76.21 | 6 |
| post_controls_215_229 | 15 | 30.12 | 31.37 | 49.60 | 55.04 | 69.00 | 0 |

Across all clean loops, P95/P99/max intervals were 49.78/55.05/78.63 ms. There were 0 intervals over 100 ms and 0 over 250 ms. Intentional pause and seek effects are retained in the raw data and analyzed separately. 345 full clean 30-second sliding windows have median FPS 30.10, range 29.97–30.17.

| Resource group | Private commit median MiB | Private WS median MiB | CPU median, one-core % | Process GPU local median MiB | Handles median |
|---|---:|---:|---:|---:|---:|
| warmed_20_49 | 748.25 | 514.00 | 145.02 | 196.74 | 1163 |
| late_170_199 | 752.38 | 518.18 | 146.95 | 196.74 | 1165 |
| post_controls_215_229 | 767.04 | 527.79 | 146.94 | 196.93 | 1166 |

CPU 100% means one logical core; this machine exposes 32. Renderer-owned GPU bytes, process-local GPU use, process memory, caches and handles are separate measurements. File-handle snapshots: 122, file count range 212–212, minimum object-type coverage 99.83%, maximum worker duration 4.20 ms. Closed handles can race the read-only type query. One-second GUI resource sampling peaked at 0.16 ms.

Memory is not perfectly flat. The observer retains finite loop/window summaries until the selected run ends, while event, heartbeat and resource records stream to disk; recent windows and frame metadata use bounded retention. Private-byte drift therefore cannot be interpreted as a product leak or proof of zero leaks from this run alone. Cache/queue bounds and the early/late comparison are retained in JSON.

222 asynchronous published first-frame deformation/signature SHA-256 comparisons had 0 mismatches. Payload/sample-time/count checks had 0 mismatches. These CPU checks do not prove that every loop displayed sample zero; the presentation omissions above are a separate result. Detailed pause/resume and seek outcomes, including target presentation times, are in phase3.summary.action_results.

This stage adds opt-in long-run observation plus read-only cache/worker telemetry; it adds no further playback behavior optimization. The protocol and source/binary hashes were frozen before acceptance. Final verification compares that manifest, the input size/mtime/SHA-256, git diff whitespace and a fresh real-asset ABC regression run. The historical phase 2 ASan/GPU/desktop checks remain identified as phase 2 evidence; the 30-minute run itself uses Release.

Reproduction (Qt/dependency DLL paths as in phase 2):

```powershell
./out/msvc/Release/formaview_model_abc_performance.exe B:/Model/AA.ABC E:/CodexProject/FormaView/out/abc-performance/recheck-soak.json --qml --follow --soak=230
```

All new run outputs are under out/abc-performance/. Raw event/loop/window/resource/heartbeat streams, final JSON, screenshot, fixed protocol, analysis and frozen manifest use the soak-acceptance-v1 prefix. frameSwapped measures submitted payload presentation, not physical monitor scanout. The harness does not include the rest of Main/Desktop/history or all background services, and the adapter is not independently identified. No cross-machine or cross-asset guarantee follows. No commit, push, packaging or installation was performed. B:/Model/AA.ABC was read-only.

Additional acceptance details:

- GUI heartbeat P50/P95/P99/max: 15.34/16.87/18.03/27.51 ms. This is observed timer service spacing, not a 1 ms deadline guarantee.
- Request-to-swap P50/P95/P99/max: 11.35/15.20/16.93/46.07 ms.
- Sample zero was absent from 13 clean loop presentation orders: 14, 22, 26, 44, 73, 79, 82, 114, 122, 151, 195, 197, 226. Four were requested omissions; the other nine were never requested by the clock. There are 12 never-requested samples overall, including three sample-239 boundaries.
- After the first ten seconds, document cache stayed at 261985184 bytes / 8 samples; frame cache 50980858 bytes / 1 entry; UV memo 5756704 bytes; renderer-owned GPU 32390112 bytes / 1 resident base. Active archive reads and frame jobs each stayed within 0–1; pending-frame samples were 0. Final drain has active_reads=0, frame_jobs=0, pending_frame=false.
- Process private commit ranged 718.80–792.57 MiB after warmup. Warmed-to-late medians increased 4.12 MiB; after the seeks the median was 767.04 MiB, with final drained commit 755.21 MiB. This allocation/observer drift limits any leak conclusion; it did not accompany growing product cache or worker counts.
- Total handles ranged 1163–1177; process-local GPU ranged 196.74–197.35 MiB.

| Playing seek target | Target presented after action, ms | First presentation after action |
|---|---:|---:|
| 173 | 42.18 | 173 |
| 12 | 40.89 | 218 |
| 221 | 37.68 | 221 |
| 47 | 38.45 | 47 |

Pause lasted 2023.71 ms. One already-in-flight sample zero presented 7.21 ms after pause, then no more payload updates until resume. Resume's first update arrived 46.68 ms later. All four seek targets presented. The backward seek to 12 first swapped already-in-flight sample 218, 3.96 ms after the action; target 12 followed at 40.89 ms. This trace does not establish immediate cancellation of a previously submitted frame.

No progressive degradation, payload mismatch, growing cache/worker queue or unexpected >100 ms clean-loop interval was reproduced, so this stage applies no further playback fix. Rare omissions and uneven pacing remain explicitly open limitations.

Final regression initially **failed** with `FAIL Timeout: stable stream open` (exit 1; out/abc-performance/soak-final-regression.log). The invocation used a relative fixture directory. tests/model_abc_tests.cpp passes that path unchanged to ModelSession, while ModelAbcSource uses InputLease and src/core/file_access_win.cpp:56 rejects nonabsolute local-drive paths. The test waits only for settled state, so an open error becomes a 60-second timeout. Changing only the fixture-directory argument to an absolute path on the same binary produced **PASS model ABC session (1 real samples)** (exit 0, approximately 3 seconds; out/abc-performance/soak-final-regression-absolute.log). This was an invalid test invocation, with no evidence of a long-run cache/thread regression. Both attempts and fixtures remain preserved. No product change or repeat 230-loop run was needed; the frozen source/binary manifest still matches all 15 files. The final input SHA-256, size and modification time also match.

Full final worktree state (pre-existing .agents/ and tools/runtime/installer_task_history.py were preserved):

```text
 M include/formaview/geometry_cache.hpp
 M include/formaview/render_item.hpp
 M src/formats/geometry_cache.cpp
 M src/host/CMakeLists.txt
 M src/host/ModelInspector.qml
 M src/host/model_abc.cpp
 M src/host/model_abc.hpp
 M src/host/model_document.hpp
 M src/host/model_frame.cpp
 M src/host/model_frame_cache.cpp
 M src/host/model_session.cpp
 M src/host/model_session.hpp
 M src/render/render_item.cpp
 M tests/model_abc_tests.cpp
 M tests/model_render_tests.cpp
?? .agents/
?? docs/abc-preview-performance-2026-11-11.json
?? docs/abc-preview-performance-2026-11-11.md
?? tests/model_abc_performance.cpp
?? tests/model_abc_stability.hpp
?? tools/runtime/installer_task_history.py
```
