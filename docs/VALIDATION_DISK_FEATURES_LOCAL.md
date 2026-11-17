# Disk-only feature fix — local Windows validation, 6 November 2026

Local version: **0.1.0-alpha.5.10**. This task did not publish a new GitHub release or build a Mac binary.

Installed in the existing official CloudCompare 2.13.2 x64/Qt5 host. Installed plugin SHA256:
`7aa7933970ba8fbbb5f645dc1b1592dccd3736996d7ca30980350ecea5fcb0e9`.

Verified locally:

- 25,216 grid/edge/halo assertions.
- 5,100 progressive/disk-only assertions, including actual isolated feature worker, zero additional full-cloud SFs in Disk-only mode, unchanged source classes, linked BIN round-trip/resume and native checkpoint → bounded Python LAS export.
- 368 general processing regression checks.
- 11 disk-export tests, also run against the **installed** Python modules: geometry and field values, RGB/return mapping, existing-feature reuse, partial coverage/NaN, duplicate ownership, corrupt hashes, changed geometry, missing checkpoints, invalid ASPRS labels and bounded matrix chunks. Absent RGB is not silently offered as observed model input; extra-field names are unique case-insensitively.
- Existing preparation suite: 6 cases, 5 passed and 1 LAZ-specific case skipped because that optional Python LAZ backend was absent in the local runtime. This task adds LAS export; it does not claim new LAZ backend validation.
- Installer completed with exit code 0; installed DLL and modified worker files matched the tested staging/source hashes.

The large project was saved through CloudCompare as an entire BIN scene before installation. The previous BIN was retained as a backup; no full-survey feature calculation was started. The project was then reopened using the same original filename. File paths are retained in local task outputs, not this distributable report.

These are software correctness checks, not an independent scientific classification-accuracy benchmark, a hard OS-enforced memory cap, or proof that every arbitrarily dense halo fits the selected worker budget. The current cloud itself still occupies CC RAM. See [usage and limitations](DISK_FEATURES.md).
