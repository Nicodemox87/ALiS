# Prepare data for this model — alpha.5.7

Open **Classification > Supervised workflow > 7. Classify a cloud > Prepare data for this model**.

1. Select a saved ALiS `model.joblib` from a trusted author. Joblib is executable input, not a safe interchange format.
2. Choose the selected CloudCompare cloud, or LAS/LAZ directly on disk. Loaded mode snapshots current global coordinates and required scalar fields/RGB without cloning the cloud. All points are used, including hidden points. Disk mode never loads the full cloud in CC.
3. Choose a NEW job folder, maximum block side (default 50 m) and additional worker memory estimate (default 2 GiB). Confirm metric units. Inspect model requirements.
4. Prepare / resume. Feature identities, order and radii come from the saved model, never automatic scale suggestions. Existing arbitrary SF inputs are required as named. Geometric SF reuse requires explicit provenance confirmation. Supplied `qAL_HAG` is retained.
5. Classify prepared blocks. The saved estimator and preprocessing are reused without retraining; supported neural backends retain their configured device. GPU acceleration does not apply to the native geometric feature engine.
6. Review `result/REPORT.html`, `result/result.json`, class distributions and mean confidence. These are prediction diagnostics, NOT accuracy estimates. LAS input produces `classified.las`, preserving record order and non-class attributes. Invalid predictions retain the original LAS class.
7. For a loaded cloud, **Show in CC** adds Derived Classification and Confidence. Original, Working and Trusted labels are untouched. Explicit review/Apply remains separate. A RAM preflight may leave outputs on disk instead of allocating display fields. On reopening the dialog, repeat Prepare / resume and Classify (completed blocks are reused) to re-verify the snapshot before import.

## Bounded processing and reproducibility

The disk index is built in 100,000-point batches. Each square core is expanded by at least the largest model radius (plus 2 mm numeric margin); PointNet uses its saved neighbourhood radius. Actual context counts are checked before allocation. Dense blocks subdivide recursively, never reduce radii or decimate points. The native process is capped at one million context points per block and isolated from CloudCompare. Estimates are conservative, not an operating-system hard memory quota; model size and backend allocations can vary.

Each point belongs to exactly one half-open core. Halo rows provide context and are omitted when merging output by original point ID. No LAS duplicates are introduced. Interrupted block computation can be resumed in the same folder. Changes to source content, model, settings or checksummed block files are rejected. Incomplete predictions are retained under an `incomplete_prediction_*` folder before retry. Stop is immediate during native blocks and checked at prediction block boundaries; source hashing/model loading may take time.

Disk space includes the indexed input, per-block coordinates and features, predictions, and output LAS. Each stage checks available space; very large clouds may require many tens of GB. Temporary context is retained intentionally for restart and reproducibility. Keep the job folder until validation is complete.

## HAG, legacy models and limits

Fixed-radius halo processing is local. CSF and terrain interpolation are not necessarily local: a finite halo is NOT proof of equivalence to global terrain. Prefer supplying a consistent global HAG. Missing HAG requires explicit opt-in and a complete saved CSF/terrain recipe. For legacy models use the explicit recipe editor only after checking training settings. Defaults are suggestions, not inferred provenance. An initial 20 m terrain halo is used and checked against cloth/interpolation support; validate seams before scientific interpretation. Ground/class labels are never silently used as predictor inputs.

LAS RGB normalization cannot be inferred safely from its 16-bit channels; RGB models currently require the CC snapshot path (explicit 0–255 display values). Waveform formats 4/5/9/10 are rejected until payload-preserving export is available. Only ASPRS-target models are supported by this blocked classifier. Unknown or incomplete feature identities fail explicitly. Dataset CRS is retained in LAS metadata, but metric units still require confirmation.

## Validation (28–29 September 2026)

- Five synthetic integration tests: disk LAS/LAZ and CC snapshot, fixed-radius seam equivalence, exact point ownership, saved-feature reuse, cancellation/resume, checksum rejection, non-class attribute preservation, unchanged source classifications, unit/budget/missing-HAG rejection.
- Native session integration: 92 checks passed, including streamed import, preservation of Working/Trusted, and rejection of truncated outputs.
- Processing/workspace regression with UI checks: 375 checks passed.
- Real LiDAR tile: 585,001 points, four 10 m cores, 41 model inputs. Random Forest labels agree 100% with whole-tile inference; finite masks match, largest feature difference 2.33e-10. Both paths use the SAME global HAG. This validates blocking, not classifier accuracy or tiled-ground equivalence.
- PointNet: 28,283 points in a 5 m spatial crop, four blocks, CUDA inference. Blocked and whole-crop labels agree 100%; no point decimation. This is a regression check, not independent model validation.

No new full-survey scientific classification is implied by these software regression tests.
