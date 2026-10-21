# Native spatial-block panel (historical development notes)

Historical alpha.5.9-dev.1 description. The progressive destination and BIN
resume implementation in **[PROGRESSIVE_FEATURES.md](PROGRESSIVE_FEATURES.md)**
supersedes the per-block geometry output described below.

Version: `0.1.0-alpha.5.9-dev.1`, Windows / CloudCompare 2.13.2 / Qt 5.

Open **Prepare > Features > Spatial blocks** on a selected metric cloud.
The panel uses CloudCompare's native Qt style and palette. No replacement
application theme is installed.

## Implemented

- Sampled, bounded XY RGB preview and approximate sampled upper-surface hillshade.
  North is up, coordinates and scale are in metres. This is a navigation preview,
  not a scientific elevation raster.
- Exact full-resolution core counts, clickable blocks, select/deselect all.
  Core cells are half-open except the outer survey boundary: no duplicated rows.
- Selected geometric features and exact radii inherited from Prepare > Features.
  HAG is not recomputed by this geometric-only panel.
- Halo automatically equals the largest selected radius. It contributes neighbours
  but never appears in a committed output or training subset. All source points
  in each chosen core are retained; no processing decimation is performed.
- An isolated native worker per block, explicit RAM ceiling, free-memory guard,
  four worker threads and the existing bounded-neighbourhood safety checks.
- Grey pending, yellow working, green saved, red failed. Run a pilot block to
  measure timing; remaining-time ranges are estimates, not accuracy guarantees.
- Pause after committing the current block; stop the isolated worker safely;
  retry failed blocks. Within the open dialog, completed blocks are skipped.
- Original class fields, other SFs, RGB and coordinate shifts are cloned into
  each core, and requested geometric SFs are added with a model-schema mapping.
- BIN cores, source row IDs and `blocks.json` are saved to a user-chosen folder.
  Optional import of the latest or all completed blocks into CloudCompare.
- A core-only training subset action creates a separate full-resolution cloud
  and opens Classification. Class-field confirmation/trust and model selection
  remain explicit; selecting blocks never certifies labels automatically.

## Limits and next steps

This new standalone panel currently uses a cloud already loaded in CC. The
existing **Classification > Prepare data for this model** workflow continues
to support direct LAS/LAZ disk streaming. Standalone disk streaming, cross-session
resume, merging SFs back onto the full source and one-click model training/predict
from this panel are not implemented. Use the training subset in the existing
supervised workflow, with spatially disjoint train/test cores and appropriate
neighbour-context exclusion. Selecting a subset alone does not eliminate leakage.

RAM ceilings are conservative allocation checks, not an OS-enforced process cap.
Importing all cores accumulates memory; use latest-only or save-only for large data.
Collection currently scans source coordinates once per requested block; it is
bounded in additional RAM but not optimal for thousands of selected cores.
Large radii/dense blocks can be rejected with a diagnostic rather than silently
changing radii or truncating neighbourhoods.

## Verification

`ALiS_processing_tests --spatial-blocks-regression` exercises 25,216 grid boundary,
core uniqueness and halo checks, plus 20 native-dialog checks using the actual
isolated feature worker: click selection, empty-selection guard, core-only SF
publication, original labels, persisted manifest, responsive GUI event loop,
core-only training subset and unchanged source. The existing processing suite
passes 368 checks. Real-survey interactive acceptance is recorded separately;
synthetic checks are not classification accuracy validation.

This document describes the historical dev.1 design, not the current output
model. Use PROGRESSIVE_FEATURES.md for alpha.5.9 behaviour and persistence.
