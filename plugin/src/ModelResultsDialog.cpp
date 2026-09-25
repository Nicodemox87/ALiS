// SPDX-License-Identifier: GPL-2.0-or-later
#include "ModelResultsDialog.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace
{
	QJsonObject readReport(const QString& path, QString* error = nullptr)
	{
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly))
		{
			if (error) *error = file.errorString();
			return {};
		}
		QJsonParseError parseError;
		const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
		if (parseError.error != QJsonParseError::NoError || !document.isObject())
		{
			if (error) *error = parseError.errorString();
			return {};
		}
		return document.object();
	}

	QString number(double value, int decimals = 4)
	{
		return QString::number(value, 'f', decimals);
	}

	QTableWidget* table(const QStringList& headers, const char* objectName)
	{
		auto* result = new QTableWidget;
		result->setObjectName(QString::fromLatin1(objectName));
		result->setColumnCount(headers.size());
		result->setHorizontalHeaderLabels(headers);
		result->setEditTriggers(QAbstractItemView::NoEditTriggers);
		result->setSelectionBehavior(QAbstractItemView::SelectRows);
		result->setAlternatingRowColors(true);
		result->horizontalHeader()->setStretchLastSection(true);
		return result;
	}

	void setItem(QTableWidget* target, int row, int column, const QString& text)
	{
		target->setItem(row, column, new QTableWidgetItem(text));
	}

	QString htmlReportForJson(const QString& reportPath)
	{
		QString result = reportPath;
		if (result.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive))
			result.chop(4);
		result += QStringLiteral("html");
		return result;
	}
}

namespace alis
{
	QString ModelResultsDialog::reportPathForModel(const QString& modelPath)
	{
		if (modelPath.trimmed().isEmpty()) return {};
		const QFileInfo model(modelPath);
		const QString candidate = model.fileName() == QStringLiteral("model.joblib")
			? QDir(model.absolutePath()).filePath(QStringLiteral("report.json"))
			: model.absoluteFilePath() + QStringLiteral(".report.json");
		return QFileInfo::exists(candidate) ? candidate : QString();
	}

	ModelResultsDialog::ModelResultsDialog(const QStringList& reportPaths, QWidget* parent)
		: QDialog(parent)
	{
		setWindowTitle(QStringLiteral("ALiS — Model results"));
		setObjectName(QStringLiteral("ALiS.ModelResults"));
		resize(980, 720);
		for (const QString& path : reportPaths)
			if (QFileInfo(path).isFile() && !m_reportPaths.contains(QFileInfo(path).absoluteFilePath()))
				m_reportPaths << QFileInfo(path).absoluteFilePath();

		auto* layout = new QVBoxLayout(this);
		auto* title = new QLabel(QStringLiteral("<b>Training and validation results</b><br>Use these results before prediction. A high global score can still hide a poorly learned archaeological or rare class."));
		title->setWordWrap(true);
		title->setStyleSheet(QStringLiteral("QLabel { color:#172554; background:#eff6ff; border:1px solid #bfdbfe; padding:10px; border-radius:5px; }"));
		layout->addWidget(title);

		auto* selectorRow = new QHBoxLayout;
		selectorRow->addWidget(new QLabel(QStringLiteral("Result:")));
		m_reportSelector = new QComboBox;
		m_reportSelector->setObjectName(QStringLiteral("ALiS.ModelResults.Selector"));
		for (const QString& path : m_reportPaths)
		{
			const QJsonObject report = readReport(path);
			const QString classifier = report.value(QStringLiteral("classifier_id")).toString(QFileInfo(path).dir().dirName());
			m_reportSelector->addItem(QStringLiteral("%1 — %2").arg(classifier, QDir::toNativeSeparators(QFileInfo(path).absolutePath())), path);
		}
		selectorRow->addWidget(m_reportSelector, 1);
		layout->addLayout(selectorRow);
		m_status = new QLabel;
		m_status->setWordWrap(true);
		layout->addWidget(m_status);

		m_tabs = new QTabWidget;
		m_tabs->setObjectName(QStringLiteral("ALiS.ModelResults.Tabs"));
		m_overview = new QLabel;
		m_overview->setWordWrap(true);
		m_overview->setAlignment(Qt::AlignLeft | Qt::AlignTop);
		m_overview->setTextInteractionFlags(Qt::TextSelectableByMouse);
		auto* overviewPage = new QWidget;
		auto* overviewLayout = new QVBoxLayout(overviewPage);
		overviewLayout->addWidget(m_overview);
		overviewLayout->addStretch(1);
		m_tabs->addTab(overviewPage, QStringLiteral("Overview"));
		m_comparison = table({QStringLiteral("Classifier"), QStringLiteral("Balanced accuracy"), QStringLiteral("Accuracy"), QStringLiteral("Kappa"), QStringLiteral("Test points"), QStringLiteral("Status")}, "ALiS.ModelResults.Comparison");
		m_tabs->addTab(m_comparison, QStringLiteral("Comparison"));
		m_perClass = table({QStringLiteral("ASPRS"), QStringLiteral("Class"), QStringLiteral("Precision"), QStringLiteral("Recall"), QStringLiteral("F1"), QStringLiteral("IoU"), QStringLiteral("Support")}, "ALiS.ModelResults.PerClass");
		m_tabs->addTab(m_perClass, QStringLiteral("Per class"));
		m_confusion = table({}, "ALiS.ModelResults.ConfusionMatrix");
		m_tabs->addTab(m_confusion, QStringLiteral("Confusion matrix"));
		m_learning = table({QStringLiteral("Epoch"), QStringLiteral("Training loss")}, "ALiS.ModelResults.Learning");
		m_tabs->addTab(m_learning, QStringLiteral("Learning"));
		m_importance = table({QStringLiteral("Feature"), QStringLiteral("Importance")}, "ALiS.ModelResults.Importance");
		m_tabs->addTab(m_importance, QStringLiteral("Features"));
		m_technical = new QPlainTextEdit;
		m_technical->setReadOnly(true);
		m_technical->setObjectName(QStringLiteral("ALiS.ModelResults.Technical"));
		m_tabs->addTab(m_technical, QStringLiteral("Technical details"));
		layout->addWidget(m_tabs, 1);

		auto* explanation = new QLabel(QStringLiteral("Note: Random Forest, Extra Trees and boosting do not have neural epochs. Generic p-values are not valid outputs for these predictive models; use spatial validation, confusion matrix, per-class confidence and repeated experiments for uncertainty."));
		explanation->setWordWrap(true);
		explanation->setStyleSheet(QStringLiteral("QLabel { color:#78350f; background:#fffbeb; border:1px solid #fde68a; padding:7px; }"));
		layout->addWidget(explanation);
		auto* buttons = new QHBoxLayout;
		auto* openHtml = new QPushButton(QStringLiteral("Open complete HTML report"));
		auto* openFolder = new QPushButton(QStringLiteral("Open result folder"));
		auto* close = new QPushButton(QStringLiteral("Close"));
		buttons->addWidget(openHtml); buttons->addWidget(openFolder); buttons->addStretch(1); buttons->addWidget(close);
		layout->addLayout(buttons);
		connect(close, &QPushButton::clicked, this, &QDialog::accept);
		connect(openHtml, &QPushButton::clicked, this, [this]()
		{
			const QString path = htmlReportForJson(currentReportPath());
			if (!QFileInfo::exists(path)) QMessageBox::warning(this, windowTitle(), QStringLiteral("HTML report not found."));
			else QDesktopServices::openUrl(QUrl::fromLocalFile(path));
		});
		connect(openFolder, &QPushButton::clicked, this, [this]()
		{
			if (!currentReportPath().isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(currentReportPath()).absolutePath()));
		});
		connect(m_reportSelector, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ModelResultsDialog::loadReport);
		rebuildComparison();
		if (m_reportPaths.isEmpty()) clearReport(QStringLiteral("No readable report.json was found beside the selected model."));
		else loadReport(0);
	}

	QString ModelResultsDialog::currentReportPath() const
	{
		return m_reportSelector ? m_reportSelector->currentData().toString() : QString();
	}

	void ModelResultsDialog::clearReport(const QString& message)
	{
		m_status->setText(message);
		m_status->setStyleSheet(QStringLiteral("QLabel { color:#991b1b; background:#fee2e2; padding:7px; }"));
		m_overview->setText(message);
		for (QTableWidget* target : {m_perClass, m_confusion, m_learning, m_importance}) target->setRowCount(0);
		m_technical->clear();
	}

	void ModelResultsDialog::rebuildComparison()
	{
		m_comparison->setRowCount(0);
		for (const QString& path : m_reportPaths)
		{
			const QJsonObject report = readReport(path);
			if (report.isEmpty()) continue;
			const QJsonObject training = report.value(QStringLiteral("training")).toObject();
			const QJsonObject metrics = training.value(QStringLiteral("metrics")).toObject();
			const QJsonObject split = training.value(QStringLiteral("split")).toObject();
			const int row = m_comparison->rowCount(); m_comparison->insertRow(row);
			setItem(m_comparison, row, 0, report.value(QStringLiteral("classifier_id")).toString());
			setItem(m_comparison, row, 1, number(metrics.value(QStringLiteral("balanced_accuracy")).toDouble()));
			setItem(m_comparison, row, 2, number(metrics.value(QStringLiteral("accuracy")).toDouble()));
			setItem(m_comparison, row, 3, number(metrics.value(QStringLiteral("cohen_kappa")).toDouble()));
			setItem(m_comparison, row, 4, QString::number(split.value(QStringLiteral("test_rows")).toInt()));
			setItem(m_comparison, row, 5, metrics.value(QStringLiteral("validation_complete")).toBool() ? QStringLiteral("Complete") : QStringLiteral("Warnings"));
		}
		m_comparison->resizeColumnsToContents();
	}

	void ModelResultsDialog::loadReport(int index)
	{
		if (index < 0 || index >= m_reportPaths.size()) return;
		QString error;
		const QJsonObject report = readReport(m_reportPaths[index], &error);
		if (report.isEmpty()) { clearReport(QStringLiteral("Cannot read report: %1").arg(error)); return; }
		const QJsonObject training = report.value(QStringLiteral("training")).toObject();
		const QJsonObject metrics = training.value(QStringLiteral("metrics")).toObject();
		const QJsonObject split = training.value(QStringLiteral("split")).toObject();
		const QJsonObject config = training.value(QStringLiteral("configuration")).toObject();
		const bool complete = metrics.value(QStringLiteral("validation_complete")).toBool();
		m_status->setText(complete ? QStringLiteral("✓ Validation contains every trained class.") : QStringLiteral("⚠ Validation is incomplete: read the warnings before using this model."));
		m_status->setStyleSheet(complete ? QStringLiteral("QLabel { color:#14532d; background:#dcfce7; padding:7px; }") : QStringLiteral("QLabel { color:#92400e; background:#fef3c7; padding:7px; }"));
		QStringList warnings;
		for (const QJsonValue value : report.value(QStringLiteral("warnings")).toArray()) warnings << value.toString();
		const QString strategy = split.value(QStringLiteral("strategy")).toString();
		QString overviewText = QStringLiteral(
			"<h2>%1</h2><p><b>Device:</b> %2 · <b>Validation:</b> %3</p>"
			"<p><b>Balanced accuracy:</b> %4 · <b>Accuracy:</b> %5 · <b>Cohen kappa:</b> %6</p>"
			"<p><b>Training points:</b> %7 · <b>Test points:</b> %8 · <b>Features:</b> %9</p>"
			"<p><b>Training time:</b> %10 s · <b>Evaluation time:</b> %11 s</p>"
			"<p><b>How to read it:</b> balanced accuracy gives every class the same weight; recall shows how much of a real class is found; precision shows how often a predicted class is right; F1 balances both. Read support first for rare classes.</p>"
			"<p><b>Next step:</b> inspect low-recall classes and off-diagonal confusion cells. If acceptable, run Predict; then visually inspect Derived Classification and Confidence before Apply.</p>%12");
		overviewText = overviewText.arg(report.value(QStringLiteral("classifier_id")).toString())
			.arg(config.value(QStringLiteral("resolved_device")).toString(QStringLiteral("cpu")))
			.arg(strategy)
			.arg(number(metrics.value(QStringLiteral("balanced_accuracy")).toDouble()))
			.arg(number(metrics.value(QStringLiteral("accuracy")).toDouble()))
			.arg(number(metrics.value(QStringLiteral("cohen_kappa")).toDouble()))
			.arg(QString::number(split.value(QStringLiteral("train_rows")).toInt()))
			.arg(QString::number(split.value(QStringLiteral("test_rows")).toInt()))
			.arg(QString::number(report.value(QStringLiteral("feature_schema")).toObject().value(QStringLiteral("count")).toInt()))
			.arg(number(training.value(QStringLiteral("training_seconds")).toDouble(), 2))
			.arg(number(training.value(QStringLiteral("evaluation_seconds")).toDouble(), 2))
			.arg(warnings.isEmpty() ? QString() : QStringLiteral("<p><b>Warnings:</b><br>%1</p>").arg(warnings.join(QStringLiteral("<br>"))));
		m_overview->setText(overviewText);

		const QJsonArray classes = metrics.value(QStringLiteral("per_class")).toArray();
		m_perClass->setRowCount(classes.size());
		for (int row = 0; row < classes.size(); ++row)
		{
			const QJsonObject item = classes[row].toObject();
			setItem(m_perClass, row, 0, QString::number(item.value(QStringLiteral("class")).toInt()));
			setItem(m_perClass, row, 1, item.value(QStringLiteral("name")).toString());
			setItem(m_perClass, row, 2, number(item.value(QStringLiteral("precision")).toDouble()));
			setItem(m_perClass, row, 3, number(item.value(QStringLiteral("recall")).toDouble()));
			setItem(m_perClass, row, 4, number(item.value(QStringLiteral("f1")).toDouble()));
			setItem(m_perClass, row, 5, number(item.value(QStringLiteral("iou")).toDouble()));
			setItem(m_perClass, row, 6, QString::number(item.value(QStringLiteral("support")).toInt()));
		}
		m_perClass->resizeColumnsToContents();

		const QJsonArray labels = metrics.value(QStringLiteral("labels")).toArray();
		QStringList headers;
		for (const QJsonValue& label : labels) headers << QStringLiteral("Pred %1").arg(label.toInt());
		m_confusion->clear(); m_confusion->setColumnCount(headers.size()); m_confusion->setHorizontalHeaderLabels(headers);
		m_confusion->setRowCount(labels.size());
		QStringList vertical;
		const QJsonArray matrix = metrics.value(QStringLiteral("confusion_matrix")).toArray();
		for (int row = 0; row < labels.size(); ++row)
		{
			vertical << QStringLiteral("True %1").arg(labels[row].toInt());
			const QJsonArray values = matrix.at(row).toArray();
			for (int column = 0; column < values.size(); ++column) setItem(m_confusion, row, column, QString::number(values[column].toInt()));
		}
		m_confusion->setVerticalHeaderLabels(vertical); m_confusion->resizeColumnsToContents();

		const QJsonArray history = training.value(QStringLiteral("learning_history")).toArray();
		m_learning->setRowCount(history.size());
		for (int row = 0; row < history.size(); ++row)
		{
			const QJsonObject item = history[row].toObject();
			setItem(m_learning, row, 0, QString::number(item.value(QStringLiteral("epoch")).toInt()));
			setItem(m_learning, row, 1, number(item.value(QStringLiteral("training_loss")).toDouble(), 6));
		}
		if (history.isEmpty())
		{
			m_learning->setRowCount(1); setItem(m_learning, 0, 0, QStringLiteral("—"));
			setItem(m_learning, 0, 1, QStringLiteral("Not applicable: this model is not trained in neural epochs."));
		}

		const QJsonArray names = report.value(QStringLiteral("feature_schema")).toObject().value(QStringLiteral("names")).toArray();
		const QJsonArray importances = training.value(QStringLiteral("feature_importances")).toArray();
		QVector<QPair<QString, double>> ranking;
		for (int i = 0; i < std::min(names.size(), importances.size()); ++i) ranking.push_back(qMakePair(names[i].toString(), importances[i].toDouble()));
		std::sort(ranking.begin(), ranking.end(), [](const QPair<QString, double>& a, const QPair<QString, double>& b) { return a.second > b.second; });
		m_importance->setRowCount(ranking.isEmpty() ? 1 : ranking.size());
		if (ranking.isEmpty()) { setItem(m_importance, 0, 0, QStringLiteral("Not exposed by this classifier")); setItem(m_importance, 0, 1, QStringLiteral("—")); }
		for (int row = 0; row < ranking.size(); ++row) { setItem(m_importance, row, 0, ranking[row].first); setItem(m_importance, row, 1, number(ranking[row].second, 6)); }
		m_importance->resizeColumnsToContents();
		m_technical->setPlainText(QString::fromUtf8(QJsonDocument(report).toJson(QJsonDocument::Indented)));
	}
}
