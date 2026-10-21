# Progressive block features - alpha.5.9

Open **Prepare > Features > Spatial blocks**, choose geometric features/radii
before opening the panel, then choose **Feature destination**:

- **Current cloud (add/update SFs)**: no geometry copy. The selected original
  rows receive the new SF values as each core completes.
- **One new full-cloud copy**: makes one complete copy, once. Every subsequent
  core updates that same entity; the original geometry, fields and metadata
  remain unchanged. To resume later, select the destination copy in the DB Tree.

New job-owned fields are initialised to NaN across the full cloud. Unprocessed
points never masquerade as zero. Existing classification fields and ordinary SFs
are not overwritten. Changing the feature/radius recipe creates independent
job-owned fields rather than silently mixing incompatible results.

Select some cores, run, add others and run again: completed cores are skipped.
Pause finishes the current core. Stop cancels the isolated worker before the
short commit step, retaining already committed cores. Halo points supply
neighbourhood context only; each source row belongs to one core.

## Save and resume

Press **Save cloud + progress (BIN)**, or save the cloud with CC's BIN exporter.
This saves full geometry once, feature fields and progressive metadata together.
The original LAS/LAZ file is never overwritten automatically. A staged BIN and
atomic final copy protect an existing output if the dedicated save fails.

Reopen the BIN, select its cloud and reopen Spatial blocks. Exact matching uses
all ordered global XYZ coordinates (SHA-256), point count, bounds, block size,
feature IDs, exact radii and algorithm-schema version. Entity IDs and filenames
are not used as proof of row alignment. If current UI parameters differ, choose
**Resume saved settings**, or start an independent new recipe. Completed cores
are restored from cloud metadata; legitimate NaNs inside a completed core do
not make it pending. Geometry reordering/subsetting invalidates that identity.

Keep owned progressive SFs unedited. Their values are not rehashed on reopening;
manual edits/deleting fields can invalidate results and should start a new job.
Grid/recipe/destination are frozen after allocation in an open job; close and
reopen to start another configuration. Labels may still be edited outside the
modal panel without changing the geometric descriptor recipe.

## Disk and memory

No per-block BIN, XYZ, RGB or old-field copies are produced. Temporary halo XYZ
and worker matrices are deleted after each block. Persistent `.alsf` checkpoints
contain row indices and NEW feature values only, plus a small header; `blocks.json`
records provenance. This is a feature journal, not a standalone point cloud and
not an automatic recovery loader. Save BIN to resume after closing CC; without
a saved BIN, the journal alone is not currently replayed into a fresh cloud.

In-memory SF arrays still require approximately `4 * N * K` bytes for N source
rows and K new float fields. A full-cloud copy also needs geometry and original
attributes. The panel displays SF size and checks free RAM for the destination,
worker ceiling and reserve before allocation. Block workers are bounded but
cannot eliminate the final SF allocation. If it does not fit, use the existing
model-driven disk-streaming workflow or reduce the feature schema; standalone
disk-backed full-cloud SFs are future work. Import/export of LAS extra bytes does
not substitute for BIN's saved job metadata.

Training subset creation remains an explicit separate action and may duplicate
the selected geometry only when requested. It now copies from the progressive
destination, so computed SFs are included. Model training and label certification
remain in Classification; spatial leakage precautions still apply.

## Validation

Real isolated-worker regression: 25,216 grid/edge/halo assertions and 5,079
progressive checks, including NaN pending rows, unchanged classes, absence of
per-core BIN files, partial BIN round-trip, exact schema reuse with no duplicate
SFs, pause/stop and a single complete copy for multiple blocks. This is software
correctness evidence, not classification accuracy validation.

Research testing preview. See the matching release's CI evidence and limitations;
these checks do not establish scientific accuracy or certify production use.
