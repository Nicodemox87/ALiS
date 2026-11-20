# ALiS alpha.5.21 — local macOS port

Work started 18 November 2026 from public source commit
`09caf704bba1cedd5926e600b208a70cdb5b4a72` on branch `codex/macos-alpha-5.21`.
Build validation completed locally on 18 November 2026; Mac release publication prepared on 20 November 2026.

## Baseline and constraints

- Apple M4 Max / arm64, macOS 27.0.1; existing ALiS alpha.5.9 is preserved.
- CloudCompare 2.13.2 pinned at `49dbbb662f296c7780aae717897c85b3cb3764ed`.
- Local tools: CMake 4.4.4, Ninja 1.13.2, Qt 5.15.19, Eigen 5.0.1,
  CGAL 6.2.1, GMP 6.3.0, MPFR 4.2.2, LASzip 3.5.0, Python 3.12.15, libomp 23.1.3.
- CPU runtime; no CUDA or unverified Metal acceleration claims.
- No full-survey processing, private data publication, or changes to Windows assets.

## Port changes

- Mac release metadata follows alpha.5.21 (Mac build 1).
- Recovery/semantic UI regression uses Qt offscreen on macOS, as the existing
  painted UI tests do; the Windows font fixture remains Windows-only.
- The app declares light appearance independently of macOS appearance, preserving
  the user's readability preference on fresh installations.
- The bundle includes the alpha.5.21 release notes, complete Italian guide and
  disk-feature/speed-profile documentation.

- CGAL is enabled in the Mac host: DFM Delaunay filling requires it. The
  first native run exposed the missing backend; the regression is retained.
- Failed tests no longer automatically attach LLDB (which can block unattended
  builds); explicit `ALIS_DEBUG_NATIVE_TESTS=1` retains optional debugging.

## Validation

Native arm64 compilation and all 11 CTest suites passed on 18 November 2026
after enabling CGAL and correcting the Mac recovery test backend. The first
run exposed two failures (missing Delaunay backend and minimal-backend UI
crash); neither test was removed or weakened. The 16 updater unit tests passed.
The installer payload was extracted, compared byte-for-byte against staging,
relocated and tested: 47 Python regression tests, 46 passed and 1 skipped
(CUDA unavailable). The six CPU classifier backends were exercised. Native
features, terrain processing and annotation are covered by the CTest suites.
These are functional tests, not a real-survey classifier accuracy benchmark.

## Interactive acceptance and private fixture

The new arm64 app launched with light appearance on this Mac. GUI import and
display were checked on a declared 12,028-point spatial crop, with labels
unchanged and both original-index fields retained. No full-survey run was
performed. Private datasets and their detailed results are not distributed.

Coordinates were imported with a preserved global shift. No EPSG was assigned.
The annotation metric-unit guard correctly refused to proceed while units were
unconfirmed; no real-data annotation or classifier accuracy is claimed here.

The crop contains GPS no-data values equal to -FLT_MAX. Interactive import
exposed undefined int64 conversion in CloudCompare's automatic GPS shift.
The Mac host patch bounds that conversion and keeps out-of-range values
unshifted, with a warning. It does not replace GPS values or alter labels.
`tools/test_macos_las_gps.py` exercises the actual native importer/exporter:
no-data sentinels, normal GPS times and leading zeros all round-trip with exact
GPS values, unchanged classes, unchanged coordinates and both extra fields.
The full 11 native suites also pass after this correction.

## Delivery and limits

The local package is `output/mac-installer/ALiS-0.1.0-alpha.5.21-macOS-arm64-Setup.pkg`;
its final hash and size are recorded in the adjacent `ALiS-macOS-arm64.json`.
Requires Apple Silicon and macOS 27.0 or later with the current bundled runtime.
The bundle uses ad-hoc signing; it is not Apple notarized. Existing installed
alpha.5.9 was preserved. Building and testing this package does not establish
that it has been installed in /Applications. GitHub distribution includes software and corresponding sources only; no
private datasets, models or detailed survey results are included. Private fixtures and logs are excluded from sources.
Logs are in the ignored `output/port-logs` directory. Windows results do not
establish Mac acceptance.
