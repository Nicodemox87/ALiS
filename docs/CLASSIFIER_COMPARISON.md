# Classifier comparison protocol

For interactive testing, use ALiS **Classification**, select trusted training labels and the input fields, choose the model(s), set the training/test split and select an output repository. Review the generated HTML/JSON reports and per-class confusion matrix before applying a saved model to another cloud. Check class semantics and the exact feature schema, including scales.

The Python worker also provides a development batch comparison entry point, `python -m qal_ml.comparison --help`, with `plugin/worker` on PYTHONPATH. It requires an exported dataset bundle and is not a replacement for the interactive workflow. Some historical output filenames in that helper refer to the first development site; they do not mean survey data or validated models are bundled.

Use the same held-out point membership when comparing algorithms. Keep spatial blocks independent, record buffers, seeds, class balance, train caps, imputation and missing-field policy. Fit preprocessing on training data only. Report per-class precision, recall, F1 and support; mark classes absent from the test set. Accuracy measured on training points or the same survey is not evidence of independent-site generalisation.

Source LiDAR data, unpublished study reports and pretrained weights are deliberately not distributed. This public testing release makes no universal classification-accuracy claim. See [runtime instructions](RUNTIME.md), [release limitations](../RELEASE_NOTES.md) and [security guidance](../SECURITY.md).
