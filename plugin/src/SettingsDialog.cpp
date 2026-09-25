// SPDX-License-Identifier: GPL-2.0-or-later
#include "SettingsDialog.h"
#include "ModelCatalog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QIcon>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTextBrowser>
#include <QVector>
#include <QVBoxLayout>

namespace
{
	const QString CurrentVersion = QStringLiteral("0.1.0-alpha.5.6");

	QVector<int> versionParts(const QString& value)
	{
		const QRegularExpression expression(QStringLiteral("^(\\d+)\\.(\\d+)\\.(\\d+)(?:-alpha\\.(\\d+))?"),
			QRegularExpression::CaseInsensitiveOption);
		const QRegularExpressionMatch match = expression.match(value);
		if (!match.hasMatch()) return {};
		return {match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt(),
			match.captured(4).isEmpty() ? 1000000 : match.captured(4).toInt()};
	}

	QString installerVersion(const QFileInfo& installer)
	{
		const QRegularExpression expression(QStringLiteral("^ALiS-(.+)-CloudCompare-.*-Setup\\.exe$"),
			QRegularExpression::CaseInsensitiveOption);
		const QRegularExpressionMatch match = expression.match(installer.fileName());
		return match.hasMatch() ? match.captured(1) : QString();
	}

	bool newerVersion(const QString& candidate, const QString& reference)
	{
		const QVector<int> left = versionParts(candidate);
		const QVector<int> right = versionParts(reference);
		if (left.isEmpty() || right.isEmpty()) return false;
		for (int i = 0; i < qMin(left.size(), right.size()); ++i)
		{
			if (left[i] != right[i]) return left[i] > right[i];
		}
		return false;
	}

	QString newestLocalInstaller()
	{
		QSettings settings;
		QStringList roots;
		roots << settings.value(QStringLiteral("ALiS/lastUpdateDirectory")).toString()
			  << QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
			  << QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
		QString bestPath;
		QString bestVersion = CurrentVersion;
		auto considerDirectory = [&bestPath, &bestVersion](const QString& directory)
		{
			const QDir dir(directory);
			for (const QFileInfo& candidate : dir.entryInfoList(QStringList() << QStringLiteral("ALiS-*-Setup.exe"), QDir::Files))
			{
				const QString version = installerVersion(candidate);
				if (newerVersion(version, bestVersion))
				{
					bestVersion = version;
					bestPath = candidate.absoluteFilePath();
				}
			}
		};
		for (const QString& root : roots)
		{
			if (root.isEmpty() || !QFileInfo(root).isDir()) continue;
			considerDirectory(root);
			const QDir dir(root);
			for (const QFileInfo& releaseFolder : dir.entryInfoList(QStringList() << QStringLiteral("ALiS*_RELEASE"), QDir::Dirs | QDir::NoDotAndDotDot))
				considerDirectory(releaseFolder.absoluteFilePath());
		}
		return bestPath;
	}

	bool launchInstaller(QWidget* parent, const QString& path)
	{
		const QFileInfo installer(path);
		if (!installer.isFile() || installerVersion(installer).isEmpty())
		{
			QMessageBox::warning(parent, QStringLiteral("Invalid update package"),
				QStringLiteral("Choose an ALiS Windows Setup package named ALiS-…-CloudCompare-…-Setup.exe."));
			return false;
		}
		if (QMessageBox::question(parent, QStringLiteral("Install ALiS update"),
			QStringLiteral("The installer will now open. Save your CloudCompare work, close CloudCompare, then complete the installer.\n\nPackage:\n%1")
				.arg(QDir::toNativeSeparators(path))) != QMessageBox::Yes) return false;
		QSettings settings;
		settings.setValue(QStringLiteral("ALiS/lastUpdateDirectory"), installer.absolutePath());
		// Pin the update to the host that is currently running ALiS. Inno Setup
		// otherwise reuses the last installation directory, which may be another
		// (and ABI-incompatible) CloudCompare/Qt installation on the same PC.
		const QStringList arguments{QStringLiteral("/DIR=%1").arg(QDir::toNativeSeparators(QCoreApplication::applicationDirPath()))};
		if (QProcess::startDetached(path, arguments)) return true;
		QMessageBox::critical(parent, QStringLiteral("Update launch failed"),
			QStringLiteral("Windows could not start the selected installer."));
		return false;
	}

	QTextBrowser* readOnlyPage(const QString& html)
	{
		QTextBrowser* page = new QTextBrowser;
		page->setOpenExternalLinks(true);
		page->setHtml(html);
		return page;
	}
}

namespace alis
{
	SettingsDialog::SettingsDialog(const QString& repositoryPath, QWidget* parent)
		: QDialog(parent)
	{
		setWindowTitle(QStringLiteral("ALiS Settings"));
		setWindowIcon(QIcon(QStringLiteral(":/CC/plugin/ALiS/images/settings.svg")));
		resize(720, 560);
		QVBoxLayout* root = new QVBoxLayout(this);
		QTabWidget* tabs = new QTabWidget;

		QWidget* general = new QWidget;
		QVBoxLayout* generalLayout = new QVBoxLayout(general);
		QGroupBox* storage = new QGroupBox(QStringLiteral("Repositories and reuse"));
		QFormLayout* form = new QFormLayout(storage);
		QWidget* repositoryRow = new QWidget;
		QHBoxLayout* repositoryLayout = new QHBoxLayout(repositoryRow); repositoryLayout->setContentsMargins(0, 0, 0, 0);
		m_repositoryEdit = new QLineEdit(repositoryPath);
		QPushButton* browse = new QPushButton(QStringLiteral("Choose…"));
		repositoryLayout->addWidget(m_repositoryEdit, 1); repositoryLayout->addWidget(browse);
		form->addRow(QStringLiteral("Model and dataset repository"), repositoryRow);
		m_autoRecommendations = new QCheckBox(QStringLiteral("Automatically refresh recommendations when Models is opened"));
		QSettings settings;
		m_autoRecommendations->setChecked(settings.value(QStringLiteral("ALiS/catalog/automaticRecommendations"),
			settings.value(QStringLiteral("qArchaeoLiDAR/catalog/automaticRecommendations"), true)).toBool());
		form->addRow(QString(), m_autoRecommendations);
		generalLayout->addWidget(storage);
		QLabel* note = new QLabel(QStringLiteral("The repository remains under your control. Source clouds are never overwritten; datasets, reusable models, reports and indexes are stored in separate folders."));
		note->setWordWrap(true); generalLayout->addWidget(note); generalLayout->addStretch(1);
		connect(browse, &QPushButton::clicked, this, [this]()
		{
			const QString path = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose or create ALiS repository"), m_repositoryEdit->text());
			if (!path.isEmpty()) { m_repositoryEdit->setText(path); refreshCatalogSummary(); }
		});
		tabs->addTab(general, QStringLiteral("General"));

		QWidget* catalog = new QWidget;
		QVBoxLayout* catalogLayout = new QVBoxLayout(catalog);
		m_catalogStatus = new QLabel; m_catalogStatus->setWordWrap(true); m_catalogStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
		QPushButton* rebuild = new QPushButton(QStringLiteral("Rebuild model and dataset indexes"));
		catalogLayout->addWidget(new QLabel(QStringLiteral("The intelligent catalog ranks existing models using exact feature compatibility, target domain, acquisition density/spacing, returns, validation and balanced accuracy.")));
		catalogLayout->addWidget(m_catalogStatus); catalogLayout->addWidget(rebuild); catalogLayout->addStretch(1);
		connect(rebuild, &QPushButton::clicked, this, &SettingsDialog::refreshCatalogSummary);
		tabs->addTab(catalog, QStringLiteral("Catalog"));

		tabs->addTab(readOnlyPage(QStringLiteral(
			"<h2>Credits and acknowledgements</h2>"
			"<p align='center'><img src='qrc:/CC/plugin/ALiS/images/alis_cnr_lockup.png' width='360' height='316' alt='ALiS - Archaeological LiDAR Studio / CNR-ISPC'></p>"
			"<p><b>Created by Dr Nicodemo Abate</b><br>"
			"Consiglio Nazionale delle Ricerche — Istituto di Scienze del Patrimonio Culturale (CNR-ISPC).</p>"
			"<p>For bug reports, improvements, ideas, and suggestions for changes or new implementations:<br>"
			"<a href='mailto:nicodemo.abate@cnr.it'>nicodemo.abate@cnr.it</a></p>"
			"<p>Built as one CloudCompare plugin suite. Thanks to the CloudCompare, CCCoreLib, qCSF, q3DMASC, lidR, scikit-learn, XGBoost and scientific open-source communities.</p>"
			"<hr><h3>Testing Preview</h3>"
			"<p><b>Version 0.1.0-alpha.5.6 — test and evaluation build.</b> "
			"This preview may be used for testing, scientific evaluation and workflow validation. "
			"It is not a certified production release: classifications and derived products must be reviewed by a qualified operator, and original data should be preserved.</p>"
			"<p>Distributed under the GNU GPL v2 or later; see the included license and testing notice.</p>"
			"<p>This text is project metadata and cannot be edited from the application.</p>")), QStringLiteral("Credits"));
		tabs->addTab(readOnlyPage(QStringLiteral(
			"<h2>Scientific bibliography</h2>"
			"<p>Zhang et al. (2016), <i>An Easy-to-Use Airborne LiDAR Data Filtering Method Based on Cloth Simulation</i>, Remote Sensing 8(6), 501.</p>"
			"<p>Weinmann et al. (2015), <i>Semantic point cloud interpretation based on optimal neighborhoods, relevant features and efficient classifiers</i>.</p>"
			"<p>Thomas et al. (2018), <i>Semantic Classification of 3D Point Clouds with Multiscale Spherical Neighborhoods</i>.</p>"
			"<p>Roussel et al., <i>lidR: Airborne LiDAR Data Manipulation and Visualization for Forestry Applications</i>.</p>"
			"<p>ASPRS LAS Specification 1.4 R16 / 1.5 R00; CloudCompare and q3DMASC technical documentation.</p>")), QStringLiteral("Bibliography"));
		tabs->addTab(readOnlyPage(QStringLiteral(
			"<h2>What's new — 0.1.0-alpha.5.6 Testing Preview</h2>"
			"<p>Main branding now pairs ALiS with the unchanged CNR-ISPC institutional mark below. Small toolbar and Windows icons remain ALiS-only for readability.</p>"
			"<p>Updated visual identity: vegetation, temple and LiDAR logo, coordinated navy/red icons for the workspace, annotation, classification, terrain, features and settings. Installer and shortcuts use the same identity. Processing algorithms, data and model formats are unchanged.</p>"
			"<p>Experimental PointNet local-patch classifier: raw XYZ neighbourhoods, shared point MLP and symmetric maximum pooling, train/reuse on CPU or CUDA. Radius, neighbour count, epochs, batch and learning rate are configurable. This is an ALiS adaptation of Qi et al. (CVPR 2017), not their canonical T-Net architecture or pretrained weights. Full provenance and epoch losses accompany the model. Existing five classifiers are retained.</p>"
			"<p>Windows prediction-output handle fix, including HGB. Portable vegetation scores in BIN: exact content verification before reuse; legacy scores require one recomputation. Optional isolated PyTorch CUDA runtime is detected automatically after verified setup.</p>"
			"<p>Models is now Classification: Vegetation / structures comes first, followed by Supervised workflow and Unsupervised clustering. Existing scalar-field ranges can refine either vegetation proposal without new feature computation or training. Field-selection instructions are shown in the separator.</p>"
			"<ul><li>Intelligent reusable-model catalog with compatibility scoring.</li>"
			"<li>Local experimental vegetation model: verified artifact, fixed training scales, reusable scores and post-inference threshold review.</li>"
			"<li>Feature distributions by class (P10/P25/median/P75/P90) in new training reports and an interactive range-review panel. Fast charts read existing fields; full-cloud rules produce a separate To validate status without changing ASPRS.</li>"
			"<li>Experimental adaptive vegetation/retained-point preview after Ground: density-aware scales, conservative structure protection, readable reports and non-destructive separation. NOT a certified semantic classifier.</li>"
			"<li>Optional HAG for vegetation height bands; missing HAG no longer prevents binary screening.</li>"
			"<li>Report-folder choice and manual-label audit of vegetation false positives.</li>"
			"<li>Native official CloudCompare installation and automatic workspace visibility.</li><li>Responsive multiscale features with cancellation.</li><li>Clustering from existing scalar fields and RGB. External pre-trained DL is deferred.</li><li>Separate dataset and model indexes.</li><li>Redesigned classifier cards and automatic ML engine.</li>"
			"<li>Standalone Annotation Studio within the same plugin suite.</li></ul>")), QStringLiteral("What's new"));

		QWidget* updates = new QWidget;
		QVBoxLayout* updatesLayout = new QVBoxLayout(updates);
		updatesLayout->addWidget(new QLabel(QStringLiteral("Installed version: %1 — Testing Preview").arg(CurrentVersion)));
		QLabel* updateInfo = new QLabel(QStringLiteral(
			"ALiS can find a newer Setup package already downloaded in Downloads, on the Desktop or in the last selected folder. "
			"The updater targets the CloudCompare installation that contains this running plugin; save your session and close CloudCompare when the installer asks. "
			"A signed online release channel will be connected here when the public repository is published."));
		updateInfo->setWordWrap(true); updatesLayout->addWidget(updateInfo);
		QLabel* hostPath = new QLabel(QStringLiteral("Current CloudCompare folder: %1").arg(QDir::toNativeSeparators(QCoreApplication::applicationDirPath())));
		hostPath->setTextInteractionFlags(Qt::TextSelectableByMouse); hostPath->setWordWrap(true); updatesLayout->addWidget(hostPath);
		QPushButton* findUpdate = new QPushButton(QStringLiteral("Find and install newest downloaded update"));
		findUpdate->setToolTip(QStringLiteral("Checks Downloads, Desktop and the last selected update folder for a newer official ALiS Setup package."));
		updatesLayout->addWidget(findUpdate);
		QPushButton* installUpdate = new QPushButton(QStringLiteral("Choose ALiS update package…"));
		installUpdate->setToolTip(QStringLiteral("Choose an official ALiS Windows Setup executable. The external installer performs the update; ALiS never replaces its loaded DLL directly."));
		updatesLayout->addWidget(installUpdate);
		connect(findUpdate, &QPushButton::clicked, this, [this]()
		{
			const QString path = newestLocalInstaller();
			if (path.isEmpty())
			{
				QMessageBox::information(this, QStringLiteral("ALiS is up to date"),
					QStringLiteral("No downloaded ALiS package newer than %1 was found. Use Choose ALiS update package if it is stored elsewhere.").arg(CurrentVersion));
				return;
			}
			launchInstaller(this, path);
		});
		connect(installUpdate, &QPushButton::clicked, this, [this]()
		{
			QSettings settings;
			const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Select ALiS update installer"),
				settings.value(QStringLiteral("ALiS/lastUpdateDirectory")).toString(),
				QStringLiteral("ALiS Windows Setup (ALiS-*-Setup.exe);;Applications (*.exe)"));
			if (path.isEmpty()) return;
			launchInstaller(this, path);
		});
		updatesLayout->addStretch(1);
		tabs->addTab(updates, QStringLiteral("Updates"));

		root->addWidget(tabs, 1);
		QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
		connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		root->addWidget(buttons);
		refreshCatalogSummary();
	}

	QString SettingsDialog::repositoryPath() const { return m_repositoryEdit->text().trimmed(); }
	bool SettingsDialog::automaticRecommendations() const { return m_autoRecommendations->isChecked(); }

	void SettingsDialog::refreshCatalogSummary()
	{
		const ModelCatalogSummary summary = ModelCatalog::refresh(repositoryPath(), {}, {});
		m_catalogStatus->setText(summary.error.isEmpty()
			? QStringLiteral("Repository: %1\nIndexed datasets: %2\nIndexed models: %3").arg(summary.repositoryPath).arg(summary.datasetCount).arg(summary.modelCount)
			: summary.error);
	}
}
