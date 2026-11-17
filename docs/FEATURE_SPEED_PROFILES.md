# Spatial feature speed profiles — local alpha.5.14

All modes retain the spatial grid, selected blocks, exact source core rows, safe pause/stop, full requested neighbourhood support and independent disk checkpoints. There is no source point decimation or approximate neighbour cap in these profiles.

| Profile | Fields | Radii |
|---|---|---|
| Complete / current selection | Exactly those chosen in Prepare, or explicitly resumed from the saved job | Unchanged |
| Balanced | 12 descriptors per scale | Three smallest distinct positive selected radii |
| Fast | 8 descriptors per scale | Two smallest distinct positive selected radii |

With the Montepruno selection 0.5 / 1 / 2 / 4 m, these are respectively 172, 36 and 16 fields. Fast uses neighbour count, linearity, planarity, surface variation, verticality, roughness, height range and height standard deviation. Balanced additionally includes sphericity, eigenentropy, height above neighbourhood minimum and height below neighbourhood maximum. Neither reduced profile fits quadric curvatures or calculates height percentiles. Avoiding expensive feature families and the largest neighbourhood is more meaningful than merely dropping redundant PCA output columns, because PCA computation is already shared.

Reduced profiles change the available information. They are **not equivalent to the full feature schema and are not certified classifiers**. They retain exact mathematical descriptors at the retained radii; they do not establish that a classifier using them has equal accuracy, especially for large structures, low vegetation or archaeological microtopography. Training and prediction must use the same feature/radius schema and must be validated on independent labelled data.

## Time estimate before starting

The panel shows elapsed time and a remaining/total interval in hours and minutes. Hours can exceed 24; total means the current processing pass, not all historical sessions. Selection changes update the amount of estimated work. Profile/CPU changes load the matching local calibration or schedule a new one. No fixed, invented workstation throughput is assumed.

When no compatible full-block timing is available, automatic preflight scans the loaded cloud to gather an un-decimated small spatial support region around a point in a median-population selected block. It describes up to 2,048 deterministic core queries within a 4 x 4 m core, retaining **all** points in the maximum-original-radius support envelope. A 500,000-context-point cap, conservative memory check and 60-second worker limit bound this diagnostic. Context is never truncated to make it fit: an overly dense halo reports that a full pilot is needed. The initial support scan has progress/cancellation and can itself take time on a very large loaded cloud.

Results are cached locally by ordered-coordinate fingerprint, exact feature keys/radii, CPU identifier, thread count and native algorithm tag. They are not uploaded. Temporary support/output matrices are discarded when the panel closes; preflight does not attach SFs or write production checkpoints. A small-region density-weighted extrapolation has deliberately wide uncertainty. Matching previously completed full blocks are stronger evidence and take precedence. A full pilot and subsequent actual blocks progressively improve the estimate. Density, spatial distribution, halo, disk performance, memory pressure and thermal load remain sources of error.

Native `ALIS_TIMING` diagnostics separate query traversal from load/setup/output. The preflight extrapolates query work with local XY density, point-related setup/output with estimated support population, and process startup plus the measured full-cloud support scan per pending map block. Startup time from a tiny worker is not multiplied by every source point. This remains a heuristic: adaptive leaf repetition, real disk/cache behaviour and spatial density variation require the full pilot for a stronger prediction. Timing instrumentation changes no feature values.

## Resume and output safety

Changing profile changes the feature/schema identity and selects a new independent output folder if the existing folder contains a different job. Old complete-profile journals are neither overwritten nor adopted as reduced-profile checkpoints. The profile is locked once a job/store has been created; reopen the panel to configure an independent job. Preserve both the original BIN and the old job folder to resume the complete schema later. All original current CC class fields remain untouched.

## Execution policy

Production remains **one spatial block worker at a time**, with configurable parallel CPU feature evaluation inside it. Native worker isolation, adaptive subdivision and RAM headroom are retained. More threads or multiple workers are not assumed faster. CPU/GPU and two-worker experiments are benchmark evidence only, not a production GPU adapter or a guarantee of survey-wide throughput. The GPU must pass numerical equivalence, a scalable bounded search design and realistic throughput/memory tests before being offered as an operational backend.

`scripts/benchmark_feature_profiles.py` records real-fixture timings, exact-column-subset comparisons, CPU24/CPU30/two-CPU12 measurements and an optional bounded dense CUDA / CPU12+CUDA experiment. It never changes production manifests or labels. Local Montepruno results are retained in a separate performance-evaluation folder; a small fixture is not an independent classification-accuracy study.

## Montepruno bounded experiment — 7 November 2026

Three interior spatial fixtures from the preserved real context, three repetitions each, up to 2,048 core queries with complete 4 m support, gave end-to-end native medians: Complete **1.597 s**, Balanced **0.819 s**, Fast **0.797 s**. All 27 native subset comparisons passed at the declared `0.001 × max(1, |reference|)` tolerance; observed maximum subset error was zero. This is about 2x on these small worker runs, not a promise of survey-wide gain. Small-worker startup/output costs limit this comparison.

For one Fast fixture, three-run medians were CPU24 **0.888 s**, CPU30 **0.994 s**, and two simultaneous CPU12 workers **0.948 s**. The bounded CUDA Fast prototype passed 32,768 value comparisons and exact neighbour counts, but took **7.256 s** warmed; CPU12+CUDA took **3.616 s** and also passed. Maximum CUDA difference was approximately `8.94e-8`, peak allocated CUDA memory approximately 183 MB. The GPU uses a dense experimental search, not a production spatial index. No CUDA or two-worker option is enabled: neither provided a reliable gain over one CPU24 worker in this test.

The original 172-field run was paused only after B0031 committed. Its **48 parents / 70,654,331 source rows** remain in the original `MP_block` folder, with pre-change ledgers archived under `history/before_speed_profiles_20261107`. The new reduced-schema test uses a separate job. No completed full-schema values were overwritten or silently relabelled as Fast results.
