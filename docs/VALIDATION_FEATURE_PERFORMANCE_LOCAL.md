# Feature throughput — local Windows evaluation, 7 November 2026

Local version: **0.1.0-alpha.5.12**. No GitHub publication or Mac binary update in this task.

## Scope and implementation

The user authorized stopping the Montepruno feature pass to evaluate and increase resource utilization. Stop safely retained all **37** completed parent checkpoints; the unfinished parent remains pending. The original BIN, source geometry, classes, 172 feature keys, radii 0.5/1/2/4 m, 4 m halo and 100 m reporting grid were not changed.

- CPU thread control added to Spatial blocks, persisted in application settings. Hardware-aware default: at most 24 threads and at least two logical processors of headroom where possible; explicit requests accepted up to 32. On this 32-logical-processor workstation the default is 24. Generic feature-engine callers keep their conservative four-thread default unless they opt in.
- Automatic worker ceiling raised from 8 to 24 GiB, capped at 50% of current free RAM and retaining at least 2 GiB and 40%. Refreshed at each adaptive leaf. Manual ceiling up to 64 GiB is still bounded by current free RAM, retaining at least 2 GiB and 25%.
- Worker preflight continues to account for per-thread neighbourhood scratch and field arrays. A ceiling is not an OS hard quota or RAM forced into use. Existing CPU/RAM pressure safeguards and adaptive fallback remain in place.
- CPU thread count recorded outside the geometry/schema configuration, in `blocks.json`. Completed checkpoint identity and format remain compatible. No concurrent multi-parent scheduler or production GPU feature adapter was introduced.
- The destination selector can shrink inside the scrollable settings panel rather than forcing horizontal clipping.

## Hardware and reproducible microbenchmarks

Intel Core i9-13950HX, 24 cores / 32 logical processors; approximately 128 GB installed RAM; NVIDIA RTX 5000 Ada Generation Laptop GPU, 16 GB VRAM; managed PyTorch 2.10.0+cu128 reports CUDA available. Existing display/other processes were using approximately 9 GB VRAM at the first observation: a future backend must budget *free*, not nominal, VRAM.

Artifacts are local under `F:\2026_ERIHS_MontePruno\MP_block\performance_evaluation_20261107`. A preserved in-flight worker context supplied an un-decimated, **16,001-point spatial crop** (approximately 5.08 m side), tested with all 172 native fields. This crop is a performance fixture with its own boundary context, **not** a substitute for full-survey production output. Native output arrays were exactly equal, including NaN positions, across tested thread counts, memory ceilings and two simultaneous four-thread workers.

| Configuration | Measured elapsed time |
|---|---:|
| Original installed worker, 4 threads / 8 GiB ceiling (one run) | 6.418 s |
| Same worker, 4 threads / 16 GiB ceiling (one run) | 6.413 s |
| Expanded worker, 8 threads (median of 3 rotated-order runs) | 4.325 s |
| Expanded worker, 12 threads (median of 3) | 3.546 s |
| Expanded worker, 16 threads (median of 3) | 3.171 s |
| Expanded worker, 24 threads (median of 3) | 2.714 s |
| Two concurrent 4-thread workers, two identical fixture jobs | 7.154 s total |

The larger memory ceiling alone did not accelerate a crop already fitting in memory. The 24-thread crop result is approximately 2.36 times the first four-thread observation, **not a promised full-survey speedup**. Warm cache, thermal state, GUI/storage contention and density affect timings; parts of this exploratory session overlapped local compilation. A representative full parent remains necessary to calibrate the operational ETA. The two-worker result measures throughput on duplicated fixture jobs, not a deployed parallel block scheduler.

## CUDA feasibility experiment — limited coverage

A separate prototype queried **64** actual source points against the preserved full worker context, retaining all radius-4 neighbours (maximum 22,058 per query; 936,185 pairs), then processed the nested 0.5/1/2/4 m neighbourhoods. CPU exact-radius search was followed by packed GPU float64 covariance/eigenvalues, means and three height quantiles. This is **not** native parity for all 172 fields and does not implement curvature, all moments or GPU neighbour search.

- CPU local statistics with four Python threads: **0.148 s**.
- Warm GPU statistics: **0.040–0.045 s**; peak allocated CUDA memory approximately **185 MiB**.
- CPU radius search: **0.183 s**; CPU packing: **0.050 s**; upload **0.017 s**, download **0.001 s**.
- Including query work and transfers, excluding one-time index construction: CPU **0.331 s**, hybrid **0.291 s** (approximately 1.14 times throughput, not the 3.7-times arithmetic-only ratio).
- CPU/GPU prototype values agreed within `atol=1e-8, rtol=1e-6`, same NaN pattern; maximum absolute difference approximately `4.8e-14`. This compares the prototype's two implementations, not the entire native engine.

Conclusion: GPU is viable, but merely moving PCA to CUDA leaves neighbour search/packing as bottlenecks. A future adapter needs bounded GPU radius search/spatial indexing, batched reductions, exact neighbour ownership and staged validation for all requested fields, with CPU fallback on memory/numerical failure. No neighbour truncation or unannounced sampling is acceptable. NVIDIA recommends profiling end-to-end work and minimizing/batching CPU–GPU transfers: https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html . Open3D documents exact-radius search primitives, but was not installed or integrated in this evaluation: https://www.open3d.org/docs/latest/python_api/open3d.core.nns.NearestNeighborSearch.html .

## Regression execution

Native Release build passed. Spatial regression passed: **25,216** grid/edge/halo checks; **8,479** adaptive/native/merge/recovery/RAM/CPU/ETA checks; **1,135,067** all-172-field monolithic/adaptive assertions; **5,100** progressive SF/BIN/pause-stop/disk-only assertions. The rotated CPU-scaling experiment additionally checked complete native array equality in each of 12 runs.

Additional feature-safety regression: **22** checks passed. General processing regression: **368** checks passed. Build/test logs: `output/performance-final-build.log`, `output/performance-spatial-tests.log`, `output/performance-feature-safety-tests.log`, `output/performance-processing-tests.log`.

The native-plugin installer completed with exit code **0** in the official CloudCompare 2.13.2 Qt5 installation. Tested staging and installed SHA256 hashes matched:

- `plugins/ALIS_PLUGIN.dll`: `f19a23e81b68b00e431acee29f5c7abe4ae9ad4e63dd9c456a7a998bf431c48a`.
- `ALiS_prepare_block.exe`: `06296a7ba6eebd822443c927b966970181f65d04513b35600a20b321f3942829`.

The existing BIN was reopened without changing its 74,650,796,108-byte saved file or discarding any completed parent journals. Operational restart observations are recorded in the job monitoring note; installation and tests alone do not prove that the remaining production cores have finished.
