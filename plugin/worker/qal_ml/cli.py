from __future__ import annotations

import argparse
import sys
from datetime import datetime, timezone
from pathlib import Path

from .classifiers import CLASSIFIER_IDS, capability_report
from .bootstrap import ALGORITHMS, capability_report as bootstrap_capability_report, cluster
from .dataset import dataset_summary, load_dataset
from .events import EventWriter
from .model import (
    TrainConfig,
    load_artifact,
    model_summary,
    predict,
    train,
)
from .repository import ensure_repository, repository_summary
from .pretrained import (
    install_curated,
    prune_obsolete,
    remove_package,
    runtime_capabilities,
    synchronize_status,
    update_installed,
)


def _positive_int(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be >= 1")
    return parsed


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="qal_ml_worker",
        description="ALiS classification and bootstrap-label worker",
    )
    parser.add_argument("--version", action="version", version="qal_ml_worker 0.5.0")
    subparsers = parser.add_subparsers(dest="command", required=True)

    inspect_parser = subparsers.add_parser("inspect", help="validate and describe a dataset or model")
    inspect_group = inspect_parser.add_mutually_exclusive_group(required=True)
    inspect_group.add_argument("--dataset", type=Path)
    inspect_group.add_argument("--model", type=Path)

    subparsers.add_parser("capabilities", help="report installed ML/DL and CPU/GPU backends")
    subparsers.add_parser("bootstrap-capabilities", help="report clustering and pretrained-provider availability")
    repository_parser = subparsers.add_parser("repository", help="initialize or inspect a local model repository")
    repository_parser.add_argument("--path", type=Path, required=True)
    pretrained_install = subparsers.add_parser("pretrained-install", help="download a curated data-only model package")
    pretrained_install.add_argument("--repository", type=Path, required=True)
    pretrained_install.add_argument("--model-id", required=True)
    pretrained_remove = subparsers.add_parser("pretrained-remove", help="remove one local pre-trained package")
    pretrained_remove.add_argument("--repository", type=Path, required=True)
    pretrained_remove.add_argument("--model-id", required=True)
    pretrained_check = subparsers.add_parser("pretrained-check", help="check official assets, versions and local runtimes")
    pretrained_check.add_argument("--repository", type=Path, required=True)
    pretrained_update = subparsers.add_parser("pretrained-update-all", help="atomically update installed managed packages")
    pretrained_update.add_argument("--repository", type=Path, required=True)
    pretrained_prune = subparsers.add_parser("pretrained-prune", help="remove obsolete ALiS-managed packages only")
    pretrained_prune.add_argument("--repository", type=Path, required=True)
    pretrained_runtime = subparsers.add_parser("pretrained-capabilities", help="inspect provider runtimes and GPU")
    pretrained_runtime.add_argument("--repository", type=Path, required=True)

    train_parser = subparsers.add_parser("train", help="train and evaluate a supervised classifier")
    train_parser.add_argument("--dataset", type=Path, required=True)
    # A separate certified cloud gives a genuinely independent validation set.
    train_parser.add_argument("--test-dataset", type=Path)
    train_parser.add_argument("--model-out", type=Path)
    train_parser.add_argument("--report-out", type=Path)
    train_parser.add_argument("--repository", type=Path)
    train_parser.add_argument("--classifier", choices=CLASSIFIER_IDS, default="random_forest")
    train_parser.add_argument("--device", choices=("auto", "cpu", "cuda"), default="auto")
    train_parser.add_argument("--block-size", type=float, default=20.0)
    train_parser.add_argument("--test-fraction", type=float, default=0.2)
    train_parser.add_argument("--seed", type=int, default=42)
    train_parser.add_argument("--trees", type=_positive_int, default=300)
    train_parser.add_argument("--max-depth", type=_positive_int)
    train_parser.add_argument("--min-samples-leaf", type=_positive_int, default=1)
    train_parser.add_argument("--max-features", choices=("sqrt", "log2", "all"), default="sqrt")
    train_parser.add_argument("--class-weight", choices=("balanced_subsample", "balanced", "none"), default="balanced_subsample")
    train_parser.add_argument("--criterion", choices=("gini", "entropy", "log_loss"), default="gini")
    train_parser.add_argument("--subsample", type=float, default=0.9)
    train_parser.add_argument("--column-sample", type=float, default=0.9)
    train_parser.add_argument("--n-jobs", type=int, default=-1)
    train_parser.add_argument("--epochs", type=_positive_int, default=30)
    train_parser.add_argument("--batch-size", type=_positive_int, default=4096)
    train_parser.add_argument("--learning-rate", type=float, default=0.001)
    train_parser.add_argument('--max-train-per-class', type=int, default=0)
    train_parser.add_argument('--spatial-buffer', type=float, default=0.)
    train_parser.add_argument('--impute-missing', action='store_true')
    train_parser.add_argument('--pointnet-radius', type=float, default=.5)
    train_parser.add_argument('--pointnet-neighbors', type=int, default=64)

    predict_parser = subparsers.add_parser("predict", help="classify a feature dataset")
    predict_parser.add_argument("--dataset", type=Path, required=True)
    predict_parser.add_argument("--model", type=Path, required=True)
    predict_parser.add_argument("--output", type=Path, required=True)
    predict_parser.add_argument("--chunk-size", type=_positive_int, default=250_000)
    predict_parser.add_argument(
        "--write-probabilities",
        action="store_true",
        help="also write the potentially very large N x C probability matrix",
    )
    cluster_parser = subparsers.add_parser("cluster", help="create reviewable unsupervised cluster pseudo-labels")
    cluster_parser.add_argument("--dataset", type=Path, required=True)
    cluster_parser.add_argument("--output", type=Path, required=True)
    cluster_parser.add_argument("--algorithm", choices=ALGORITHMS, default="minibatch_kmeans")
    cluster_parser.add_argument("--clusters", type=_positive_int, default=8)
    cluster_parser.add_argument("--seed", type=int, default=42)
    cluster_parser.add_argument("--fit-sample", type=_positive_int, default=500_000)
    cluster_parser.add_argument("--chunk-size", type=_positive_int, default=250_000)
    cluster_parser.add_argument("--device", choices=("auto", "cpu", "cuda"), default="auto")
    return parser


def run(args: argparse.Namespace) -> int:
    events = EventWriter(args.command)
    events.emit("started", "start", 0.0, f"Starting {args.command}")
    if args.command == "capabilities":
        events.emit("completed", "capabilities", 1.0, "Classifier capabilities detected", capability_report())
        return 0
    if args.command == "bootstrap-capabilities":
        events.emit("completed", "capabilities", 1.0, "Bootstrap capabilities detected", bootstrap_capability_report())
        return 0
    if args.command == "repository":
        ensure_repository(args.path)
        events.emit("completed", "repository", 1.0, "Repository ready", repository_summary(args.path))
        return 0
    if args.command == "pretrained-install":
        result = install_curated(args.repository, args.model_id, events)
        events.emit("completed", "pretrained", 1.0, "Pre-trained package installed", result)
        return 0
    if args.command == "pretrained-remove":
        result = remove_package(args.repository, args.model_id)
        events.emit("completed", "pretrained", 1.0, "Pre-trained package removed", result)
        return 0
    if args.command == "pretrained-check":
        result = synchronize_status(args.repository, events)
        events.emit("completed", "pretrained", 1.0, "Updates and runtimes checked", result)
        return 0
    if args.command == "pretrained-update-all":
        result = update_installed(args.repository, events)
        events.emit("completed", "pretrained", 1.0, "Installed pre-trained packages updated", result)
        return 0
    if args.command == "pretrained-prune":
        result = prune_obsolete(args.repository)
        events.emit("completed", "pretrained", 1.0, "Obsolete managed packages removed", result)
        return 0
    if args.command == "pretrained-capabilities":
        result = runtime_capabilities(args.repository)
        events.emit("completed", "pretrained", 1.0, "Pre-trained runtimes checked", result)
        return 0
    if args.command == "inspect":
        if args.dataset is not None:
            summary = dataset_summary(load_dataset(args.dataset))
            kind = "dataset"
        else:
            summary = model_summary(load_artifact(args.model), args.model)
            kind = "model"
        events.emit("completed", "inspect", 1.0, f"Valid {kind}", summary)
        return 0

    if args.command == "train":
        events.emit("progress", "load", 0.03, "Loading dataset")
        dataset = load_dataset(args.dataset)
        test_dataset = load_dataset(args.test_dataset) if args.test_dataset is not None else None
        max_features = None if args.max_features == "all" else args.max_features
        class_weight = None if args.class_weight == "none" else args.class_weight
        model_out = args.model_out
        if model_out is None:
            if args.repository is None:
                raise ValueError("--model-out or --repository is required")
            root = ensure_repository(args.repository)
            timestamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S_%f")
            model_out = root / "models" / args.classifier / timestamp / "model.joblib"
        config = TrainConfig(
            classifier=args.classifier,
            device=args.device,
            block_size=args.block_size,
            test_fraction=args.test_fraction,
            seed=args.seed,
            trees=args.trees,
            max_depth=args.max_depth,
            min_samples_leaf=args.min_samples_leaf,
            max_features=max_features,
            class_weight=class_weight,
            criterion=args.criterion,
            subsample=args.subsample,
            column_sample=args.column_sample,
            n_jobs=args.n_jobs,
            epochs=args.epochs,
            batch_size=args.batch_size,
            learning_rate=args.learning_rate,
            max_train_per_class=args.max_train_per_class,
            spatial_buffer=args.spatial_buffer,
            impute_missing=args.impute_missing,
            pointnet_radius=args.pointnet_radius,
            pointnet_neighbors=args.pointnet_neighbors,
            repository=str(args.repository.resolve()) if args.repository is not None else None,
        )
        train(dataset, model_out, config, events, args.report_out, test_dataset=test_dataset)
        return 0

    if args.command == "predict":
        events.emit("progress", "load", 0.03, "Loading prediction dataset")
        dataset = load_dataset(args.dataset)
        predict(
            dataset,
            args.model,
            args.output,
            chunk_size=args.chunk_size,
            write_probabilities=args.write_probabilities,
            events=events,
        )
        return 0
    if args.command == "cluster":
        events.emit("progress", "load", 0.02, "Loading feature dataset")
        dataset = load_dataset(args.dataset)
        cluster(
            dataset,
            args.output,
            algorithm=args.algorithm,
            clusters=args.clusters,
            seed=args.seed,
            fit_sample=args.fit_sample,
            chunk_size=args.chunk_size,
            events=events,
            device=args.device,
        )
        return 0
    raise AssertionError(f"unknown command: {args.command}")


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return run(args)
    except Exception as exc:
        EventWriter(args.command).emit(
            "error",
            "failed",
            1.0,
            str(exc),
            {"exception": type(exc).__name__},
        )
        return 2


if __name__ == "__main__":
    sys.exit(main())
