# Adaptive geometric features — local Windows validation, 7 November 2026

Local version: **0.1.0-alpha.5.11**. No new GitHub release or Mac binary was published in this task.

## Changes

- PC/cloud-based initial grid suggestion; existing grids retain their identity.
- Automatic RAM reserve with a fresh budget before each processing leaf; optional manual ceiling remains available.
- Full-resolution quadtree subdivision of exact core/context counts, not decimation or automatic radius reduction.
- Bounded disk spools and four-way streamed checkpoint merging; no full parent feature matrix in the GUI.
- Parent checkpoint format and identity unchanged: existing alpha.5.10 completed cores remain reusable.
- Strict geometry/order/schema and SHA256 checks when recovering a disk ledger, including a BIN saved before processing.
- Median-point-count pilot; provisional sub-block ETA, then density-aware parent timings with uncertainty intervals and elapsed/remaining/run-total time.
- Per-parent processing seconds, adaptive leaf counts and context-point evaluations in `blocks.json`.

## Executed software tests

- **25,216** existing grid/edge/halo assertions.
- **8,455** new assertions: actual isolated native worker, adaptive-vs-monolithic feature values and source-row ordering, internal edge/full halo equivalence, bounded merge, no halo duplicates, monotonic progress, cancellation preserving prior atomic output, worker memory-pressure subdivision, pathological halo termination, zero new disk-mode full-cloud SFs, streamed commit, old-BIN disk-ledger recovery, changed identity/checksum rejection, automatic RAM reserve and density-aware time estimation.
- **1,135,067** additional assertions compare all **172** fields (43 descriptors × the exact requested 0.5 / 1 / 2 / 4 m radii) between a monolithic native-worker run and forced adaptive subdivision, on 6,561 synthetic curved-surface points. Source row order/coverage and NaN agreement are checked; finite values use an explicit `0.001 × max(1, |reference|)` tolerance for floating-point origin differences. This is an algorithm-equivalence fixture, not a real-survey accuracy benchmark.
- **5,100** existing progressive/current/copy/disk assertions: BIN round-trip, pause/stop, skipped completed cores, source labels unchanged, no disk-mode cloud clone/SF arrays and native-checkpoint → Python unified-LAS integration.
- **368** general processing regression assertions.
- Native Release build and native-plugin installer completed successfully. Installer exit code **0**. Installed DLL matched tested staging, SHA256:
  `8e199c3f8d82706435b801c4836c516717d5872f7f060fd92523b33d80658ba6`.

Test logs are local build outputs: `adaptive-native-final-tests.log`, `adaptive-all-fields-tests.log`, `adaptive-processing-tests.log`, `adaptive-native-final-build.log`, `adaptive-installer-build.log`, `adaptive-feature-install.log`.

The user's entire scene was saved through CloudCompare before restart. The prior BIN and the prior disk-job ledgers were retained. The target run uses the unchanged 100 m map grid and the same 172 feature keys/radii (0.5, 1, 2 and 4 m), allowing already-completed parent checkpoints to be retained rather than regenerated.

The installed host was reopened and the existing job resumed on the 235,740,172-point working cloud. All **37** prior completed parents were verified and retained under the same job ID and recorded checksums. The first representative unfinished parent contained **4,059,677** core / **4,607,419** context points; the live UI confirmed automatic 100 → 50 → 25 m subdivision. Its first native worker processed **527,341** context points with all 172 fields and four CPU threads, under an automatic budget of approximately 4 GiB (about 405 MiB observed worker working set at this early stage). This is a start-of-run observation, not a peak-memory measurement or completion claim for the remaining survey. The run was left active in CloudCompare; time estimates await useful completed sub-block timings.

These are correctness/regression tests, not a claim of independently measured classification accuracy, an OS-enforced memory quota, guaranteed wall-clock duration, or proof that an arbitrarily dense fixed halo can always fit. The already-loaded CC geometry still occupies memory. An interrupted parent is not marked complete; its uncommitted internal leaves may be recomputed on resume. See [usage and limitations](DISK_FEATURES.md).
