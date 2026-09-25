from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import joblib
import numpy as np


WORKER_ROOT = Path(__file__).resolve().parents[1]
ENTRYPOINT = WORKER_ROOT / "qal_ml_worker.py"
sys.path.insert(0, str(WORKER_ROOT))
from qal_ml import pretrained


def write_dataset(path: Path, *, trusted: bool = True, target_domain: str = "asprs_working") -> int:
    rng = np.random.default_rng(12345)
    classes = np.asarray([2, 3, 5, 6, 9, 14], dtype=np.uint8)
    rows: list[list[float]] = []
    xy: list[list[float]] = []
    labels: list[int] = []
    # Every occupied block contains every class, so spatial holdout evaluates all classes.
    for bx in range(8):
        for by in range(6):
            for class_index, class_value in enumerate(classes):
                for repeat in range(3):
                    rows.append(
                        [
                            class_index * 2.0 + rng.normal(0, 0.08),
                            (class_index % 3) * 1.5 + rng.normal(0, 0.08),
                            class_index / 5.0 + rng.normal(0, 0.03),
                        ]
                    )
                    xy.append([bx * 20.0 + repeat, by * 20.0 + class_index * 0.1])
                    labels.append(int(class_value))
    features = np.asarray(rows, dtype="<f4")
    coordinates = np.asarray(xy, dtype="<f8")
    label_array = np.asarray(labels, dtype="u1")
    trusted_mask = np.ones(len(features), dtype="u1")
    trusted_mask[0] = 0
    features[7, 1] = np.nan  # Must be dropped in training and preserved as NODATA at predict.
    path.mkdir()
    features.tofile(path / "features.f32")
    coordinates.tofile(path / "xy.f64")
    np.linspace(100.0, 110.0, len(features), dtype="<f8").tofile(path / "z.f64")
    label_array.tofile(path / "labels.u8")
    trusted_mask.tofile(path / "trusted.u8")
    np.arange(len(features), dtype="<u8").tofile(path / "point_ids.u64")
    manifest = {
        "schema": "qal-ml-dataset/1.0",
        "rows": len(features),
        "columns": 3,
        "features": [
            {"column": 0, "key": "Planarity@0.5m"},
            {"column": 1, "key": "Roughness@0.5m"},
            {"column": 2, "key": "HAG"},
        ],
        "labels_trusted": trusted,
        "label_source": "unit-test-manual",
        "target_domain": target_domain,
        "metadata": {"fixture": "synthetic-spatial-v1"},
        "files": {
            "features": {"path": "features.f32", "dtype": "<f4", "order": "C"},
            "xy": {"path": "xy.f64", "dtype": "<f8", "columns": 2},
            "z": {"path": "z.f64", "dtype": "<f8"},
            "labels": {"path": "labels.u8", "dtype": "u1"},
            "trusted": {"path": "trusted.u8", "dtype": "u1", "manual_trusted_value": 1},
            "point_ids": {"path": "point_ids.u64", "dtype": "<u8"}
        },
    }
    (path / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    return len(features)


class WorkerEndToEndTest(unittest.TestCase):
    maxDiff = None

    def run_cli(self, *args: object, expected_code: int = 0) -> list[dict]:
        completed = subprocess.run(
            [sys.executable, str(ENTRYPOINT), *(str(arg) for arg in args)],
            cwd=WORKER_ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(completed.returncode, expected_code, completed.stderr + completed.stdout)
        events = [json.loads(line) for line in completed.stdout.splitlines() if line.strip()]
        self.assertTrue(events)
        self.assertTrue(all(event["schema"] == "qal-ml-event/1.0" for event in events))
        return events

    def test_train_inspect_predict_and_determinism(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dataset = root / "training.qalml"
            n = write_dataset(dataset)
            inspect_events = self.run_cli("inspect", "--dataset", dataset)
            summary = inspect_events[-1]["data"]
            self.assertEqual(summary["point_count"], n)
            self.assertEqual(summary["nonfinite_feature_rows"], 1)
            self.assertEqual(summary["trusted_row_count"], n - 1)
            self.assertTrue(summary["z_geometry_present"])
            self.assertEqual([item["class"] for item in summary["classes"]], [2, 3, 5, 6, 9, 14])

            models = [root / "forest-a.joblib", root / "forest-b.joblib"]
            for model in models:
                events = self.run_cli(
                    "train",
                    "--dataset",
                    dataset,
                    "--model-out",
                    model,
                    "--block-size",
                    20,
                    "--test-fraction",
                    0.25,
                    "--trees",
                    40,
                    "--seed",
                    77,
                    "--n-jobs",
                    1,
                )
                metric_event = next(event for event in events if event["event"] == "metric")
                self.assertGreater(metric_event["data"]["balanced_accuracy"], 0.95)

            artifact = joblib.load(models[0])
            self.assertEqual(artifact["schema"], "qal-ml-model/2.0")
            self.assertEqual(artifact["classifier_id"], "random_forest")
            self.assertEqual(artifact["target_domain"], "asprs_working")
            self.assertEqual(artifact["training"]["split"]["overlapping_block_count"], 0)
            self.assertEqual(artifact["training"]["split"]["discarded_nonfinite_rows"], 1)
            self.assertEqual(artifact["training"]["split"]["nontrusted_rows"], 1)
            self.assertEqual(artifact["class_mapping"]["probability_column_to_asprs"], [2, 3, 5, 6, 9, 14])
            self.assertEqual(len(artifact["training"]["metrics"]["per_class"]), 6)
            report = json.loads(Path(str(models[0]) + ".report.json").read_text(encoding="utf-8"))
            self.assertEqual(report["schema"], "qal-ml-training-report/1.0")
            self.assertNotIn("estimator", report)
            self.assertEqual(report["training"]["split"]["missing_test_classes"], [])
            self.assertTrue(report["training"]["metrics"]["validation_complete"])
            self.assertTrue(all("iou" in row for row in report["training"]["metrics"]["per_class"]))
            self.assertTrue((models[0].parent / "forest-a.report.html").exists())
            self.assertTrue((models[0].parent / "forest-a.metrics.csv").exists())
            self.assertTrue((models[0].parent / "forest-a.confusion_matrix.svg").exists())

            outputs = [root / "prediction-a.qalml", root / "prediction-b.qalml"]
            for model, output in zip(models, outputs):
                output.mkdir()  # CloudCompare creates an empty job directory before launch.
                self.run_cli(
                    "predict",
                    "--dataset",
                    dataset,
                    "--model",
                    model,
                    "--output",
                    output,
                    "--chunk-size",
                    137,
                )
                result = json.loads((output / "result.json").read_text(encoding="utf-8"))
                self.assertEqual(result["invalid_nonfinite_row_count"], 1)
                self.assertFalse((output / "probabilities.f32").exists())
                predicted = np.fromfile(output / "predictions.i16", dtype="<i2")
                confidence = np.fromfile(output / "confidence.f32", dtype="<f4")
                uncertainty = np.fromfile(output / "uncertainty.f32", dtype="<f4")
                self.assertEqual(predicted[7], -1)
                self.assertTrue(np.isnan(confidence[7]))
                self.assertTrue(np.isnan(uncertainty[7]))
                self.assertTrue(set(np.unique(predicted[predicted >= 0])).issubset({2, 3, 5, 6, 9, 14}))
                np.testing.assert_allclose(confidence[predicted >= 0] + uncertainty[predicted >= 0], 1.0)

            np.testing.assert_array_equal(
                np.fromfile(outputs[0] / "predictions.i16", dtype="<i2"),
                np.fromfile(outputs[1] / "predictions.i16", dtype="<i2"),
            )
            np.testing.assert_array_equal(
                np.fromfile(outputs[0] / "confidence.f32", dtype="<f4"),
                np.fromfile(outputs[1] / "confidence.f32", dtype="<f4"),
            )
            model_inspect = self.run_cli("inspect", "--model", models[0])[-1]["data"]
            self.assertEqual(model_inspect["feature_schema"]["count"], 3)

    def test_classifier_catalog_repository_and_extra_trees(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capabilities = self.run_cli("capabilities")[-1]["data"]
            self.assertEqual(capabilities["schema"], "qal-ml-capabilities/1.0")
            self.assertIn("random_forest", [item["id"] for item in capabilities["classifiers"]])
            repository = root / "repository"
            summary = self.run_cli("repository", "--path", repository)[-1]["data"]
            self.assertEqual(summary["model_count"], 0)
            dataset = repository / "datasets" / "training.qalml"
            write_dataset(dataset)
            events = self.run_cli(
                "train", "--dataset", dataset, "--repository", repository,
                "--classifier", "extra_trees", "--device", "cpu",
                "--trees", 20, "--n-jobs", 1,
            )
            completed = events[-1]["data"]
            model = Path(completed["model"])
            self.assertTrue(model.exists())
            artifact = joblib.load(model)
            self.assertEqual(artifact["classifier_id"], "extra_trees")
            self.assertEqual(artifact["training"]["configuration"]["resolved_device"], "cpu")
            catalog = json.loads((repository / "catalog" / "models.json").read_text(encoding="utf-8"))
            self.assertEqual(len(catalog), 1)
            self.assertEqual(catalog[0]["artifact_id"], artifact["artifact_id"])

    def test_bootstrap_clusters_are_derived_and_aligned(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dataset = root / "bootstrap.qalml"
            n = write_dataset(dataset)
            capabilities = self.run_cli("bootstrap-capabilities")[-1]["data"]
            self.assertEqual(capabilities["schema"], "alis-bootstrap-capabilities/1.0")
            self.assertEqual(capabilities["pretrained"], [])
            output = root / "clusters"
            events = self.run_cli(
                "cluster", "--dataset", dataset, "--output", output,
                "--algorithm", "minibatch_kmeans", "--clusters", 6,
                "--fit-sample", 200, "--chunk-size", 137, "--seed", 77,
            )
            self.assertEqual(events[-1]["event"], "completed")
            result = json.loads((output / "result.json").read_text(encoding="utf-8"))
            self.assertEqual(result["schema"], "alis-bootstrap-result/1.0")
            self.assertIn("not ASPRS", result["semantics"])
            clusters = np.fromfile(output / "clusters.i32", dtype="<i4")
            confidence = np.fromfile(output / "confidence.f32", dtype="<f4")
            self.assertEqual(len(clusters), n)
            self.assertEqual(len(confidence), n)
            self.assertTrue(np.all((clusters >= 0) & (clusters < 6)))
            self.assertTrue(np.all(np.isfinite(confidence)))
            self.assertTrue(np.all((confidence >= 0.0) & (confidence <= 1.0)))

    def test_independent_external_test_dataset(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            training = root / "training.qalml"
            external = root / "external-test.qalml"
            write_dataset(training)
            write_dataset(external)
            model = root / "external-validation.joblib"
            events = self.run_cli(
                "train", "--dataset", training, "--test-dataset", external,
                "--model-out", model, "--trees", 20, "--n-jobs", 1,
            )
            metric_event = next(event for event in events if event["event"] == "metric")
            self.assertGreater(metric_event["data"]["balanced_accuracy"], 0.95)
            artifact = joblib.load(model)
            split = artifact["training"]["split"]
            self.assertEqual(split["strategy"], "external-certified-dataset")
            self.assertIsNone(split["requested_test_fraction"])
            self.assertEqual(split["train_rows_before_sampling"], split["trusted_rows_used"])
            self.assertEqual(
                artifact["provenance"]["test_dataset_sha256"],
                split["test_dataset_sha256"],
            )

    def test_curated_package_install_contract_and_removal(self) -> None:
        class Events:
            def __init__(self) -> None:
                self.messages: list[str] = []

            def emit(self, _event: str, _stage: str, _progress: float, message: str, _data=None) -> None:
                self.messages.append(message)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            checkpoint = root / "fixture.ckpt"
            configuration = root / "fixture.yaml"
            checkpoint.write_bytes(b"trusted-fixture-weights")
            configuration.write_text("model: fixture\n", encoding="utf-8")
            model_id = "fixture-curated"
            pretrained.CURATED[model_id] = {
                "name": "Fixture",
                "provider": "ALiS tests",
                "task": "semantic segmentation",
                "taxonomy": "fixture",
                "license": "CC0-1.0",
                "source_url": "https://example.invalid/model-card",
                "adapter": "fixture",
                "required_dimensions": ["XYZ"],
                "assets": {"model.ckpt": checkpoint.as_uri(), "config.yaml": configuration.as_uri()},
                "checkpoint": "model.ckpt",
                "configuration": "config.yaml",
            }
            try:
                installed = pretrained.install_curated(root / "repository", model_id, Events())
                manifest = Path(installed["manifest"])
                self.assertTrue(manifest.is_file())
                data = json.loads(manifest.read_text(encoding="utf-8"))
                self.assertEqual(data["schema"], "alis-pretrained-model/1.0")
                self.assertEqual(data["managed_by"], "ALiS")
                self.assertEqual(data["catalog_version"], pretrained.CATALOG_VERSION)
                self.assertEqual(len(data["assets"]), 2)
                self.assertEqual(len(data["assets"][0]["sha256"]), 64)
                status = pretrained.check_updates(root / "repository")
                fixture_status = next(item for item in status["entries"] if item["id"] == model_id)
                self.assertEqual(fixture_status["status"], "current")
                runtime = pretrained.runtime_capabilities(root / "repository")
                fixture_runtime = next(item for item in runtime["entries"] if item["id"] == model_id)
                self.assertFalse(fixture_runtime["provider_known"])
                self.assertFalse(fixture_runtime["adapter_implemented"])
                self.assertFalse(fixture_runtime["runtime_ready"])
                self.assertEqual(fixture_runtime["blocker_code"], "provider_not_integrated")
                self.assertTrue(fixture_runtime["setup_guide"])

                checkpoint.write_bytes(b"updated-trusted-fixture-weights")
                status = pretrained.check_updates(root / "repository")
                fixture_status = next(item for item in status["entries"] if item["id"] == model_id)
                self.assertEqual(fixture_status["status"], "update_available")
                updated = pretrained.update_installed(root / "repository", Events())
                self.assertIn(model_id, updated["updated"])
                self.assertEqual((manifest.parent / "model.ckpt").read_bytes(), checkpoint.read_bytes())

                unmanaged = root / "repository" / "models" / "pretrained" / "user-package"
                unmanaged.mkdir()
                (unmanaged / "manifest.json").write_text('{"id":"user-package"}', encoding="utf-8")
                prune = pretrained.prune_obsolete(root / "repository")
                self.assertNotIn("user-package", prune["removed"])
                self.assertTrue(unmanaged.is_dir())
                removed = self.run_cli(
                    "pretrained-remove", "--repository", root / "repository", "--model-id", model_id
                )[-1]["data"]
                self.assertTrue(removed["removed"])
                self.assertFalse(manifest.parent.exists())
            finally:
                pretrained.CURATED.pop(model_id, None)
            events = self.run_cli(
                "pretrained-remove", "--repository", root / "repository", "--model-id", "../escape", expected_code=2
            )
            self.assertIn("invalid pre-trained model id", events[-1]["message"])

    def test_optional_gpu_and_dl_backends_when_available(self) -> None:
        capabilities = self.run_cli("capabilities")[-1]["data"]
        by_id = {item["id"]: item for item in capabilities["classifiers"]}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dataset = root / "training.qalml"
            write_dataset(dataset)
            if by_id["xgboost"]["available"] and "cuda" in by_id["xgboost"]["devices"]:
                model = root / "xgboost.joblib"
                self.run_cli(
                    "train", "--dataset", dataset, "--model-out", model,
                    "--classifier", "xgboost", "--device", "cuda", "--trees", 8, "--n-jobs", 1,
                )
                artifact = joblib.load(model)
                self.assertEqual(artifact["classifier_id"], "xgboost")
                self.assertEqual(artifact["training"]["configuration"]["resolved_device"], "cuda")
                output = root / "xgb-prediction.qalml"
                self.run_cli("predict", "--dataset", dataset, "--model", model, "--output", output)
                values = np.fromfile(output / "predictions.i16", dtype="<i2")
                self.assertTrue(set(np.unique(values[values >= 0])).issubset({2, 3, 5, 6, 9, 14}))
            if by_id["multiscale_mlp"]["available"]:
                model = root / "mlp.joblib"
                self.run_cli(
                    "train", "--dataset", dataset, "--model-out", model,
                    "--classifier", "multiscale_mlp", "--device", "cpu",
                    "--epochs", 3, "--batch-size", 256, "--learning-rate", 0.003,
                )
                summary = self.run_cli("inspect", "--model", model)[-1]["data"]
                self.assertEqual(summary["classifier_id"], "multiscale_mlp")
                self.assertEqual(summary["training"]["configuration"]["resolved_device"], "cpu")

    def test_all_classifiers_finalize_mapped_outputs(self) -> None:
        available = self.run_cli("capabilities")[-1]["data"]["classifiers"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dataset = root / "mapped.qalml"
            count = write_dataset(dataset)
            for spec in available:
                if not spec["available"]:
                    continue
                classifier = spec["id"]
                with self.subTest(classifier=classifier):
                    model = root / (classifier + ".joblib")
                    self.run_cli("train", "--dataset", dataset, "--model-out", model,
                                 "--classifier", classifier, "--device", "cpu", "--trees", 8,
                                 "--epochs", 2, "--n-jobs", 1, "--batch-size", 128)
                    for probabilities in (False, True):
                        output = root / (classifier + str(probabilities) + ".qalml")
                        args = ["predict", "--dataset", dataset, "--model", model, "--output", output]
                        if probabilities:
                            args.append("--write-probabilities")
                        self.run_cli(*args)
                        self.assertEqual(np.fromfile(output / "predictions.i16", dtype="<i2").size, count)
                        self.assertEqual((output / "probabilities.f32").exists(), probabilities)
                        # Final directory and all files must be unlocked for callers.
                        moved = output.with_name(output.name + ".moved")
                        output.rename(moved)
                        moved.rename(output)

    def test_rejects_untrusted_labels_and_target_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            untrusted = root / "untrusted.qalml"
            write_dataset(untrusted, trusted=False)
            events = self.run_cli(
                "train",
                "--dataset",
                untrusted,
                "--model-out",
                root / "never.joblib",
                "--trees",
                5,
                expected_code=2,
            )
            self.assertEqual(events[-1]["event"], "error")
            self.assertIn("not marked trusted", events[-1]["message"])

            training = root / "training.qalml"
            write_dataset(training)
            model = root / "forest.joblib"
            self.run_cli("train", "--dataset", training, "--model-out", model, "--trees", 5, "--n-jobs", 1)
            archaeology = root / "archaeology.qalml"
            write_dataset(archaeology, target_domain="archaeology_class")
            events = self.run_cli(
                "predict",
                "--dataset",
                archaeology,
                "--model",
                model,
                "--output",
                root / "never-output.qalml",
                expected_code=2,
            )
            self.assertIn("target_domain mismatch", events[-1]["message"])


if __name__ == "__main__":
    unittest.main()
