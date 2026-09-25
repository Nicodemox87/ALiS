from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from sklearn.model_selection import GroupShuffleSplit


@dataclass(frozen=True)
class SpatialSplit:
    train_indices: np.ndarray
    test_indices: np.ndarray
    block_ids: np.ndarray
    train_blocks: np.ndarray
    test_blocks: np.ndarray
    block_size: float


def spatial_block_split(
    xy: np.ndarray,
    labels: np.ndarray,
    *,
    block_size: float,
    test_fraction: float,
    seed: int,
    candidates: int = 96,
    buffer_distance: float = 0.0,
) -> SpatialSplit:
    if not np.isfinite(block_size) or block_size <= 0:
        raise ValueError("block_size must be positive and finite")
    if not 0.05 <= test_fraction <= 0.5:
        raise ValueError("test_fraction must be in [0.05,0.5]")
    if not np.isfinite(buffer_distance) or buffer_distance < 0:
        raise ValueError("buffer_distance must be finite and nonnegative")
    if xy.ndim != 2 or xy.shape[1] != 2 or len(xy) != len(labels):
        raise ValueError("xy/labels dimensions do not match")
    if not np.all(np.isfinite(xy)):
        raise ValueError("spatial split requires finite xy coordinates")

    origin = np.min(xy, axis=0)
    block_xy = np.floor((xy - origin) / block_size).astype(np.int64)
    _, block_ids = np.unique(block_xy, axis=0, return_inverse=True)
    unique_blocks = np.unique(block_ids)
    if len(unique_blocks) < 2:
        raise ValueError("spatial split requires at least two occupied blocks")

    all_classes = np.unique(labels)
    # Score candidate partitions on the compact block/class histogram. Repeating
    # group expansion + label sorting over millions of rows per candidate is O(KN)
    # overhead; the candidate sequence and point-count objective remain identical.
    encoded = np.searchsorted(all_classes, labels)
    histogram = np.bincount(block_ids*len(all_classes)+encoded,
        minlength=len(unique_blocks)*len(all_classes)).reshape(len(unique_blocks),len(all_classes))
    splitter = GroupShuffleSplit(
        n_splits=max(8, int(candidates)),
        test_size=test_fraction,
        random_state=int(seed),
    )
    best: tuple[float, np.ndarray, np.ndarray] | None = None
    target = len(labels) * test_fraction
    for train, test in splitter.split(unique_blocks, groups=unique_blocks):
        if len(train) == 0 or len(test) == 0:
            continue
        train_counts=histogram[train].sum(axis=0)
        test_counts=histogram[test].sum(axis=0)
        missing_train = np.count_nonzero(train_counts == 0)
        if missing_train:
            continue
        missing_test = np.count_nonzero(test_counts == 0)
        size_error = abs(int(test_counts.sum()) - target) / len(labels)
        score = missing_test * 10.0 + size_error
        if best is None or score < best[0]:
            best = (score, train, test)
    if best is None:
        raise ValueError(
            "no leakage-free spatial split keeps every class in training; "
            "use a smaller block size or add trusted labels in more blocks"
        )

    train = np.flatnonzero(np.isin(block_ids,unique_blocks[best[1]]))
    test = np.flatnonzero(np.isin(block_ids,unique_blocks[best[2]]))
    if buffer_distance > 0:
        # Exclude training points near the entire held-out cell, not just sampled
        # test points. This is a geometric guard; reference labels are never features.
        cells = np.unique(block_xy[test], axis=0)
        keep = np.ones(len(train), dtype=bool)
        for start in range(0, len(train), 500_000):
            coords = xy[train[start:start+500_000]]
            near = np.zeros(len(coords), dtype=bool)
            for cell in cells:
                low = origin + cell * block_size
                high = low + block_size
                delta = np.maximum(np.maximum(low-coords, coords-high), 0.)
                near |= np.sum(delta*delta, axis=1) <= buffer_distance**2
            keep[start:start+len(coords)] = ~near
        train = train[keep]
        if len(train) == 0 or len(np.setdiff1d(all_classes, np.unique(labels[train]))):
            raise ValueError('Spatial buffer removes every training example of a class; add spatially separate labels')
    train_blocks = np.unique(block_ids[train])
    test_blocks = np.unique(block_ids[test])
    if np.intersect1d(train_blocks, test_blocks).size:
        raise AssertionError("internal error: spatial block leakage")
    return SpatialSplit(
        train_indices=train,
        test_indices=test,
        block_ids=block_ids,
        train_blocks=train_blocks,
        test_blocks=test_blocks,
        block_size=float(block_size),
    )
