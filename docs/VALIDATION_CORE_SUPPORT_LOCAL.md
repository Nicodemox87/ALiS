# Core/support feature optimization — local alpha.5.13

7 November 2026. Native Windows development/validation only; no GitHub release or Mac binary was published in this task.

## Audited primary implementations

- [3DMASC documentation](https://lidar.univ-rennes.fr/node/11): distinct core points and complete support clouds; optional spatial/random subsampling is explicitly selectable, not mandatory. Its SC scale denotes sphere **diameter**. ALiS retains the user's requested **radii** 0.5/1/2/4 m, not silently converted diameters.
- [q3DMASC source](https://github.com/dgirardeau/q3DMASC), locally audited commit `4eec64593580d62029ff477868f2e9bdd7e07002`: `q3DMASCTools.cpp`, `Tools::PrepareFeatures`, particularly the scale sort, one maximum-radius octree query, smaller-radius prefixes and separate core query loop; `NeighborhoodFeature.cpp` delegates geometric operations to CCCoreLib `Neighbourhood`.
- CloudCompare stable host source `49dbbb662f296c7780aae717897c85b3cb3764ed`, CCCoreLib `a8ce4270dfa41d5817842447b27f36094f9dfb51`: `DgmOctree::findNeighborsInASphereStartingFromCell`, `GeometricalAnalysisTools`, `Neighbourhood::computeQuadric`, `GenericProgressCallback::NormalizedProgress::steps`.
- [CloudCompare geometric-feature documentation](https://cloudcompare.org/doc/wiki/index.php?title=Compute_geometric_features) describes the classic feature tools, but its note about independent calculations is not assumed to describe every later development build: [the current changelog](https://github.com/CloudCompare/CloudCompare/blob/master/CHANGELOG.md) also documents later traversal-sharing improvements. The installed stable host ABI was not replaced.

ALiS already shared one maximum-radius octree extraction and one PCA per point/radius. Therefore merely replacing it with repeated classic CC feature commands would not automatically accelerate all 172 fields. Copying feature semantics blindly would also be unsafe: self-point conventions, height statistics and scale units need explicit matching. No assumption was made that 3DMASC's fast examples compute identical features on every original point at the same physical radii.

## Implemented improvements, retaining full-resolution blocks

1. Separate query rows from support rows. Each adaptive leaf writes sorted, unique context-row query indices for its exact half-open core. The isolated native worker retains the complete context octree and evaluates only the core. Halo outputs remain NaN in the private scratch matrix and never enter the parent checkpoint. Empty-query octree cells are skipped. Every requested source core point remains described exactly once.
2. Partial cache scopes are explicit and cannot satisfy later complete-cloud feature requests. Cache replacement is committed only after a successful computation; invalid query masks are rejected.
3. Scales reuse one cumulative, distance-ordered Welford prefix for covariance, centroid and height range rather than recomputing each prefix. Per-cell geometry/quantile buffers retain capacity between queries.
4. The five fixed, linearly interpolated height quantiles use exact multi-rank selection. No histogram approximation or neighbour truncation was introduced.
5. Quadric normal equations accumulate only one symmetric triangle and mirror it before the unchanged solver, following the symmetry principle used in CCCoreLib. Projected first moments reuse the existing centroid/eigenvalues; nearly degenerate eigenspaces keep the direct summation path.
6. Progress uses CCCoreLib's batched `steps` API rather than a shared mutex operation for every point. Cancellation remains checked per query. Feature-column metadata is resolved before output loops rather than converting Qt strings per value.
7. Adaptive capacity separates bounded per-thread search scratch from per-point field/context cost. It retains a two-field-copy allowance, fixed headroom, exact counts and the native independent guard. A large radius/context is not changed automatically to make it fit.
8. A Jacobi failure on numerically near-diagonal repeated-eigenvalue cases is retried after clearing only scale-relative round-off-sized off-diagonal terms. This fixes a failure exposed when query order changes; valid first-pass PCA results are unchanged. This is a numerical robustness improvement, not a different physical feature definition.
9. New parent completions record their engine version outside the geometry/schema identity. Older verified checkpoints are retained, with unrecorded provenance left empty rather than invented.

The spatial grid, selection, per-parent checkpointing, safe stop/pause, disk-only mode and unified export remain available. No point-cloud decimation, neighbour cap/truncation, class edits or automatic feature-count reduction was applied.

## Numerical and persistence validation

- **2,061** feature safety checks: query/support masking, NaN support rows, full-cloud cache recovery after a partial scope, invalid mask rejection, exact quantile ranks versus sorting, memory and neighbourhood safeguards, cancellation and existing updater checks.
- **25,216** grid/edge/halo checks.
- **8,483** adaptive/native/streamed merge/recovery/RAM/CPU/ETA checks, including the revised capacity bound.
- **1,135,067** comparisons of all 172 fields between monolithic and forced adaptive processing at 0.5/1/2/4 m, retaining source order, full halo, point ownership and NaN patterns.
- **5,100** progressive SF / BIN resume / pause-stop / current-copy-disk-only checks.
- Dense real-data fixture: 16,001 un-decimated source points, all 172 native fields. Paired alpha.5.12 versus optimized worker runs compared **2,752,172** output values each; no values failed the declared `0.001 × max(1, |reference|)` tolerance, and neighbour counts matched exactly. Maximum observed absolute difference was approximately `0.0004883` (a rounding-sensitive projected-moment value), not a claim of bit identity.
- A separate 7,525-core-query / 16,001-support-point fixture checked **1,294,300** values against the full-query reference, with all support-only output rows NaN and no neighbour-count mismatch. Its query fraction is intentionally smaller than a normal 100 m production core and is not used to promise survey-wide speedup.

Feature mathematical definitions, keys, radii and checkpoint format remain compatible. Numerical retry and changed summation paths mean exact bit-level identity across versions is not promised. Degenerate eigenvector directions have no unique physical solution. These are software-equivalence tests, not an independent classification-accuracy study.

## Measured performance and GPU decision

Same i9-13950HX workstation and 24 requested CPU threads. After the production parent had finished and paused, three alternating-order pairs on the same 16,001-point fixture gave medians:

| Native implementation | Elapsed time |
|---|---:|
| alpha.5.12, all 172 fields / all queries | 3.011 s |
| Optimized, all 172 fields / all queries | 2.001 s |
| Optimized, 7,525 core queries / complete 16,001-point support | 1.608 s (one run) |

The math/output optimization alone measured approximately **1.50×** throughput, or about one-third less elapsed time, versus the already-24-thread worker. Larger safe contexts and avoiding halo queries are additional scene-dependent benefits, not multiplied into an unmeasured promised speedup. Early runs overlapping compilation/production were excluded from these final paired medians.

A separate PyTorch/CUDA all-172-field prototype was actually run on the RTX 5000 Ada Laptop GPU. On all 16,001 fixture points, the dense float64 version took about **14.0 s** including search/transfers/output download and passed the full 2,752,172-value comparison. A sorted-prefix mixed-precision variant took about **5.75 s**, but **105 values failed** the declared equivalence contract (PCA degeneracy/NaN patterns and projected moments). Thus neither prototype is enabled in production. These GPU measurements used different thermal/background load conditions, so they are exploratory, not a hardware verdict. The dense GPU search is not a scalable spatial index. CUDA initialization and warm-up were recorded separately. A future production GPU adapter needs an exact, bounded spatial index, fused reductions and independent numerical validation; switching a checkbox would not make this prototype faster or correct.

Local scripts, raw requests, comparison JSON and timing logs are retained under `F:\2026_ERIHS_MontePruno\MP_block\performance_evaluation_20261107`. Native build/test logs are under repository `output/core-query-*.log`.

## Production preservation

The running alpha.5.12 pass was paused **after** parent B0032 completed, not killed mid-parent. That parent retained 4,059,677 core points after 4 adaptive leaves / 5,342,370 context evaluations and about 1,795 seconds including collection/commit. There are now **38** completed parents, covering **36,495,606** source rows; **47** nonempty parents remain. The 35 empty map cells are not errors. Pre-update ledgers were archived under `MP_block/history/before_core_queries_20261107_1449`. All older completed hashes and the original 74,650,796,108-byte BIN remain preserved. Deployment and resumed-worker observations are recorded in the local monitoring note; remaining cores are not declared complete by this report.

### Verified local deployment

After the user confirmed the new Windows administrator prompt, the alpha.5.13 installer completed with exit code 0 on 7 November 2026 at 17:09 Europe/Rome. The installed `plugins/ALIS_PLUGIN.dll` SHA-256 is `428e23ea9aa0c28eec60b23f27ef7a61fe93efa59cdd87536cbca55ae5cdf39b`; `ALiS_prepare_block.exe` is `d402744607a325953b4eaa209e38f58af9f01239a04216a79808ed3f9a88e2bb`. The same original BIN was reopened once. This installation verification alone does not establish that the remaining parents have run or completed.

The original BIN finished loading at 17:18:59; its LoD finished at 17:20:16. The saved job schema was explicitly resumed, with all 85 nonempty cells selected, 100 m grid, full 4 m halo, automatic RAM, 24 CPU threads and disk-only destination. Existing disk checkpoints were verified and the newer 38-parent ledger was adopted without recomputing B0032. At 17:30:29 the native worker was processing B0040: **4,674,444 support points, 3,951,675 exact core queries, 172 requested fields**, and a **10,811,680,768-byte** worker allowance. Three CPU-time samples rose from 261.28 to 617.92 to 928.20 seconds; input reads had completed and progress writes advanced. The UI showed 38 completed / 0 failed parents. This confirms actual production use of the optimized core/support path, not completion of the remaining 47 parents or a measured survey-wide speedup. The existing 15-minute monitor remains active.
