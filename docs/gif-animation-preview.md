# GIF animation preview and frame export

FormaView treats an animated GIF as a time-based image while keeping the
ordinary image inspection controls available. The image pane provides
play/pause, previous/next frame, loop control, a frame scrubber, and the
current frame count and delay. Seeking is decoded on the image worker; a newer
seek cancels and supersedes older work, so an obsolete result cannot replace
the requested frame.

Preview and export share `GifDecoder`. It scans frame metadata once, asks Qt's
GIF handler to decode each isolated frame's palette/LZW pixels, maps the four
interlace passes when present, and composes on an explicit ARGB32 canvas.
Disposal 0/1 keeps the canvas;
disposal 2 clears only the preceding frame rectangle to the logical screen's
global background color; disposal 3 restores the canvas saved before that
frame. A local palette cannot redefine the global background. For transparent
GIFs, the canvas background is transparent when the first frame's transparent
index equals the logical background index; absence of a global table also
defaults to transparency. Transparent patch pixels preserve existing canvas
pixels.

Only the presented frame, current patch, composition canvas and optional
disposal-3 snapshot are retained, plus bounded frame metadata. Forward playback
reuses the decoder; backward seeks and loop boundaries restart decoding to
preserve compositing state. A GIF is rejected when it has no readable frames,
exceeds 512 MiB input or 64 MiB compressed data for one frame, exceeds 10,000
frames, exceeds 16,384 pixels on an axis, exceeds a 16,777,216-pixel canvas,
or exceeds four billion canvas pixels across the animation. Invalid frame
rectangles, truncated blocks and reserved disposal methods fail with a
readable error. GIF plain-text rendering extensions are explicitly unsupported.
The source file is never rewritten.

“Export frame PNGs” asks for an output directory, file-name prefix, and an
existing-file policy. Names use stable zero-based numbering padded to at least
four digits. Export reads and writes one composed frame at a time through an
existing checked `OutputTransaction` publication, preserves alpha, reports progress and errors, and can be
cancelled. Skip, rename, and explicitly confirmed overwrite policies are
available. Name allocation and commit use the same input protection and
conflict rules as ordinary exports, including atomic non-overwriting
publication for skip/rename. The source GIF has a read-only input lease during
export and is never rewritten. Opening or clearing the image cancels an active
frame export; PNGs already committed remain available.

Automated regression coverage uses repository-embedded single-, two-, and
three-frame GIFs. The three-frame fixture has 30/70/110 ms delays, a finite
loop count, transparency, local updates, and disposal. Tests assert composed
frame pixels, delay and loop metadata, finite source-loop completion,
loop-on/loop-off tail behavior, rapid
seek replacement, forty repeated seeks with old presentation storage released,
stable padded PNG names and alpha, skip/rename/overwrite policies, malformed
input, and output-path failure. A generated 2,000-frame fixture verifies actual
mid-stream cancellation, partial readable PNG output and temporary cleanup.
Natural playback crosses twelve loop boundaries, pauses and resumes, switches
to a still image and reopens; weak references verify old CPU presentation
frames are released. Data-driven tests check disposal 0/1/2/3 with white, blue
and transparent backgrounds. Hand-authored local-rectangle fixtures deliberately
use a conflicting local background color (magenta); exact pixels across every
preview frame, backward seeks and exported PNG confirm that the global blue
background and unaffected regions survive correctly. Separate GIF87a fixtures
cover global/local tables, no graphic-control extension and interlaced rows.
Reserved disposal, excessive canvas dimensions and truncation are rejected.
A destination hard-linked to the source is rejected even under overwrite;
source bytes are checked unchanged.

The original nonwhite disposal-2 failure is covered by
`nonwhiteDisposalBackgroundPreviewAndExport`: opaque red background followed by
a transparent local-palette frame. Before the fix, Qt 6.12 returned opaque
white at (1,1); the regression failed (`out/gif-disposal-red.txt`). It now asserts
opaque red for both preview and export. The
[GIF89a specification](https://www.w3.org/Graphics/GIF/spec-gif89a.txt)
defines the background index against the global table and disposal 2 against
the background. In the
[Qt 6.12 handler](https://raw.githubusercontent.com/qt/qtbase/v6.12.0/src/plugins/imageformats/gif/qgifhandler.cpp),
disposal with a transparency index fills `0x00ffffff`; an initially opaque
RGB32 canvas presents those bits as white. Its background lookup can also use
the active local palette. Isolated pixel decoding plus explicit composition
avoids both paths. These fixture expectations come from the stated background
policy and exact pixel maps, rather than another library's rendered output.
The four-row interlace fixture also caught Qt's short-image progressive row
replication issue (`out/gif-palette-tests.txt`). Decoding rows in stream order
and mapping the four specified passes avoids that issue without retaining an
additional image.

Playback follows the GIF's embedded loop count until the user changes the
Loop control. Turning Loop off overrides the file and stops at the next end;
turning it on is an explicit infinite-loop override for the current session.
Opening another file clears that override and returns to the new file's loop
semantics.

The Release application and its QML cache are built as the UI
syntax/integration gate. The headless protocol below measures CPU/session
behavior. Interactive appearance, scene-graph rendering and sustained GPU
behavior remain manual checks. No package, installer, commit, push, or release
is produced by this work.

Compositing verification before stability work (2026-11-12): see `out/gif-final-build.txt` and
`out/gif-final-tests.txt` for the final Release application/GIF target build
and headless CTest run. Both targets built successfully; CTest passed 1/1 in
12.09 seconds. Detailed QtTest output is saved automatically to
`out/msvc/src/host/gif-animation-tests.txt`: 20 body/data executions plus init
and cleanup, total 22 passed, zero failed or skipped. The original nonwhite
regression failed before the fix and passes now in preview and PNG export.
The suite includes a real active streaming cancellation rather than only
cancellation before work begins. `git diff --check` passed. No GUI was opened,
and no packaging, commit or push was performed.

## Headless stability protocol

`formaview_gif_stability_tests OUTPUT_DIRECTORY SECONDS` is an opt-in Release
probe (60..7200 seconds), separate from the short default CTest regression.
It creates synthetic 512x384 GIFs and uses two production `ImageSession`
instances in a `QCoreApplication`, with the host's 32 MiB Qt image-allocation
limit. It never constructs a window, QML engine, image view or graphics context.
One session naturally plays a four-frame 20 ms animation. The other repeatedly
exports a 512-frame animation, alternating full completion, explicit cancel,
cancel by clear, and cancel by opening another file. Disposal 1/2/3, an opaque
blue logical background, local rectangles and transparency occur in the frames.

After 60 seconds of warmup in a 30-minute run, playback controls execute every
ten seconds: pause/resume, a burst of 129 seeks (latest result must win), loop
off to the tail, clear/reopen, exactly three finite source passes, switching
to a still image, and exporting from the playing session itself. Both normal
and cancelled PNG batches are checked in full against four explicit pixel
maps, then removed from their isolated test directories. Frame numbering,
cancel status, temporary-file cleanup, source SHA-256 preservation, bounded
weak references and final release after clear are asserted.

Memory and process handles are sampled every second. For 1800 seconds, baseline
is the median of seconds 60..180 and tail is the median of seconds 1680..1800.
Fixed gates recorded before execution in `protocol.json` are: private-memory
tail growth <=16 MiB, peak growth <=64 MiB, post-warmup least-squares slope
<=0.5 MiB/minute, and handle peak <=baseline median +16. Cancellation must
settle within 2000 ms and a control/seek within 5000 ms. Minimum natural
playback and successful/cancelled export counts prevent idle completion from
passing. Windows private bytes are the allocation measure; working set is
reported separately and no working-set trim is performed.

Evidence is streamed to `samples.csv`, `events.jsonl` and `summary.json`;
`protocol.json` freezes gates/input hashes, and the 30-minute run's
`manifest.json` freezes the relevant source and executable hashes. The
independent `tools/runtime/gif_stability_report.py EVIDENCE_DIRECTORY` audits
the streams, elapsed duration, memory trend, counts, cleanup and hashes, and
writes `analysis.json`. A short preflight is not reported as long-run acceptance.

`formaview_gif_seek_stress_tests INPUT.gif OUTPUT.json` is a separate Windows
probe for the generated 512-frame input. It checks 84 completed forward/backward
seeks, 520 additional burst requests, deep seeks queued behind a real active
export followed by cancellation, eight active decode clear/reopens and eight
active decoder destructions. It verifies final frame pixels, partial PNG
readability, memory/handles, obsolete-result suppression and input preservation.

The stability work also reproduced an export-progress race: if a consumer
delays the poll between the initial atomic progress read and the terminal
snapshot, the worker can finish all files while the displayed count stays at
an earlier value. `completedExportPublishesFinalProgressAfterDelayedPoll`
failed with 2/128 despite all 128 PNGs having committed
(`out/gif-progress-red.txt`). Poll now re-reads the final progress after observing
the synchronized terminal state and before releasing shared progress storage.
The regression holds the event consumer until the last PNG commits, then
asserts the full final count.

The Release deep-seek probe passed in 8.47 seconds
(`out/gif-deep-seek-final.json`): maximum completed seek 257.71 ms, cancellation
10.44 ms, active destructor 1.64 ms, measured private-memory peak growth
7.34 MiB, final growth 2.79 MiB, and zero handle growth. Source hash was unchanged.

An AddressSanitizer Release build also passed all 23 QtTest executions and the
same deep-seek/active teardown probe (`out/gif-asan-regression.txt`,
`out/gif-asan-deep-seek-final.json`). Both captured sanitizer error streams are
empty. The sanitizer run used `ASAN_OPTIONS=quarantine_size_mb=16:halt_on_error=1`;
its allocator/quarantine memory is separate from the ordinary Release process
memory acceptance. The deep probe's maximum seek was 514.06 ms, cancellation
9.15 ms and active destructor 4.89 ms, with zero handle growth. This checks
instrumented application memory access, not GPU allocation or an unbounded
leak-free lifetime guarantee.

GIF test targets are now conditional on the image-conversion target. A host
configured with professional/raster image support disabled configures
successfully and registers no GIF tests (`out/gif-thin-configure.txt`,
`out/gif-thin-tests.txt`); it no longer links tests against absent ImageSession
implementation.

## Stability verification results — 2026-11-12

The frozen Release run `out/gif-soak-30m-v1` passed after 1800 seconds of
measurement and 1.44 seconds of drain/cleanup. Independent audit
`out/gif-soak-30m-v1/analysis.json` also passed and confirmed all frozen source,
executable and input SHA-256 hashes unchanged.

| Measurement | Observed | Fixed gate |
| --- | ---: | ---: |
| Natural playback wraps / presentations | 15,245 / 61,630 | >=900 / >=7,200 |
| Complete exports / cancelled exports | 219 / 658 | >=20 complete; >=10 for each cancel kind |
| Explicit / clear / open cancels | 220 / 219 / 219 | >=10 each |
| Fully pixel-verified PNGs, including partial batches | 118,491 | Every committed test PNG |
| Private memory baseline / tail median | 21.18 / 21.88 MiB | Compare 60..180 s with 1680..1800 s |
| Private memory tail / measured peak growth | +0.70 / +5.86 MiB | <=16 / <=64 MiB |
| Private memory post-warmup slope | +0.00773 MiB/min | <=0.5 MiB/min |
| Working-set baseline / tail median | 36.37 / 36.64 MiB | Reported separately |
| Peak handle growth over baseline median | +15 | <=16 |
| Main-run maximum cancellation / seek-burst completion | 20.84 / 11.71 ms | <=2000 / <=5000 ms |
| Memory samples / maximum sample interval | 1,790 / 2.36 s | Continuous one-second sampling target; no >=5 s gap |

The run exercised 25 pause/resumes, 25 rapid seek bursts (3,225 requests), 25
loop-off tail stops, 25 exactly-three-pass finite source runs, 25 clear/reopens,
25 still-image switches, and 24 exports from the playing session itself.
At most one of the last 128 presented-frame weak references remained live at
sampling points; all were expired after final clear. All temporary transaction
files and isolated batch directories were gone after validation. Test input
hashes remained unchanged. The ordinary Release process showed bounded memory
variation within the frozen gates; this is a 30-minute, synthetic-workload
result, not a guarantee for every GIF or indefinite lifetime.

The independent report uses the usual midpoint median for even sample counts;
the runner uses the upper middle observation. This explains the approximately
0.012 MiB baseline difference in `summary.json`; both calculations pass the
same fixed gates. The table uses the independent report's values.

Final Release application and GIF regression builds passed
(`out/gif-stability-final-build.txt`). The final headless CTest run passed 1/1
in 12.71 seconds (`out/gif-stability-final-ctest.txt`), with QtTest total 23
passed, zero failed/skipped. The pre-fix progress failure, ordinary Release
deep-seek result, sanitizer results and thin-host configuration check are
recorded above. `git diff --check` passed. No GUI, packaging, installer,
commit, push or publication was performed.

Reproduce from an already configured build, with the Qt and OpenEXR runtime
directories on `PATH`, using a fresh evidence directory:

```powershell
formaview_gif_stability_tests.exe E:/CodexProject/FormaView/out/gif-soak-new 1800
py -3.12 tools/runtime/gif_stability_report.py E:/CodexProject/FormaView/out/gif-soak-new
formaview_gif_seek_stress_tests.exe E:/CodexProject/FormaView/out/gif-soak-new/export.gif E:/CodexProject/FormaView/out/gif-seek-new.json
```
