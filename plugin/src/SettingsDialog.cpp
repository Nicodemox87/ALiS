// SPDX-License-Identifier: GPL-2.0-or-later
#include "SettingsDialog.h"
#include "MacUpdates.h"
#include "WindowsUpdates.h"
#include "ReleaseVersion.h"
#include "ModelCatalog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QUrl>
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
	const QString CurrentVersion = QStringLiteral(ALIS_RELEASE_VERSION);

	QVector<int> versionParts(const QString& value)
	{
		return alis::windowsVersionParts(value);
	}

	QString installerVersion(const QFileInfo& installer)
	{
		return alis::windowsInstallerVersion(installer.fileName());
	}

	bool newerVersion(const QString& candidate, const QString& reference)
	{
		return alis::newerWindowsVersion(candidate,reference);
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
			for (const QFileInfo& candidate : dir.entryInfoList(QStringList() << QStringLiteral("ALiS-*-Setup*.exe"), QDir::Files))
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
				QStringLiteral("Choose an ALiS Windows plugin-only Setup.exe or Windows-x64-Complete-Setup-r1.exe package."));
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
		const QString key=installer.fileName().contains(QStringLiteral("-Complete-Setup-"),Qt::CaseInsensitive)?QStringLiteral("/HOSTDIR=%1"):QStringLiteral("/DIR=%1");
		const QStringList arguments{key.arg(QDir::toNativeSeparators(QCoreApplication::applicationDirPath()))};
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
			"<h3>AI-assisted development disclosure</h3>"
			"<p>ALiS was developed with assistance from generative AI tools (OpenAI ChatGPT/Codex), including support for source code, debugging, tests, documentation and visual assets. Scientific direction, requirements and release decisions remain with Dr Nicodemo Abate. AI is not an author or a certifying authority. Automated tests do not replace expert review; users must validate scientific outputs.</p>"
			"<p>This development-provenance disclosure is distinct from the ML/DL classifiers available in ALiS. It does not change software or third-party licences, imply institutional endorsement, or constitute a claim of legal conformity. See the distributed AI_DISCLOSURE.md for scope and legal references.</p>"
			"<hr><h3>Testing Preview</h3>"
			"<p><b>Version 0.1.0-alpha.5.9 — Testing Preview.</b> "
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
			"<h2>What's new — 0.1.0-alpha.5.9</h2><p>Automatic, optional update checks at startup on Windows and macOS (at most once daily). No silent installation. AI-assisted development disclosure. Progressive spatial features update either the current cloud or one full-cloud copy. Pending rows are NaN; no geometry copies per block. Compact feature checkpoints, geometry-order fingerprint, BIN save/reopen with progress and exact schema resume. Explicit training subsets remain available.</p>"
			"<p><b>Prepare data for this model:</b> exact model feature schema, loaded CC snapshots or LAS/LAZ on disk; adaptive bounded blocks, radius-preserving halo, checkpoints and isolated native processing. Derived predictions and confidence can be returned to CC without replacing source labels. Tiled terrain requires explicit review.</p>"
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
			"Download new packages from the official ALiS GitHub releases page."));
		updateInfo->setWordWrap(true); updatesLayout->addWidget(updateInfo);
		QLabel* hostPath = new QLabel(QStringLiteral("Current CloudCompare folder: %1").arg(QDir::toNativeSeparators(QCoreApplication::applicationDirPath())));
		hostPath->setTextInteractionFlags(Qt::TextSelectableByMouse); hostPath->setWordWrap(true); updatesLayout->addWidget(hostPath);
		QPushButton* findUpdate = new QPushButton(QStringLiteral("Find and install newest downloaded update"));
		findUpdate->setToolTip(QStringLiteral("Checks Downloads, Desktop and the last selected update folder for a newer official ALiS Setup package."));
		updatesLayout->addWidget(findUpdate);
		QPushButton* installUpdate = new QPushButton(QStringLiteral("Choose ALiS update package…"));
		installUpdate->setToolTip(QStringLiteral("Choose an official ALiS Windows Setup executable. The external installer performs the update; ALiS never replaces its loaded DLL directly."));
		updatesLayout->addWidget(installUpdate);
#ifndef Q_OS_WIN
		updateInfo->setText(QStringLiteral("macOS/Linux updates use a matching platform package. Save your session and close CloudCompare before updating. Windows Setup executables are not supported on this platform."));
		findUpdate->hide();installUpdate->hide();
		auto* releases=new QPushButton(QStringLiteral("Open ALiS releases and platform instructions"));
		updatesLayout->addWidget(releases);
		connect(releases,&QPushButton::clicked,this,[](){QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/Nicodemox87/ALiS/releases")));});
#endif
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
				QStringLiteral("ALiS Windows Setup (ALiS-*-Setup*.exe);;Applications (*.exe)"));
			if (path.isEmpty()) return;
			launchInstaller(this, path);
		});
#ifdef Q_OS_MACOS
        updateInfo->setText(QStringLiteral("Complete Apple Silicon installation with bundled Python and classifiers. Updates are checked on the official GitHub release channel, including testing previews. Downloaded packages are verified before installation. Save your work and quit ALiS before opening an update installer."));
        updatesLayout->addWidget(new QLabel(QStringLiteral("Mac package: %1").arg(macInstalledVersion())));
        auto* autoUpdate = new QCheckBox(QStringLiteral("Automatically check GitHub for updates once a day"));
        autoUpdate->setChecked(QSettings().value("ALiS/updates/automatic", true).toBool());
        updatesLayout->addWidget(autoUpdate);
        connect(autoUpdate, &QCheckBox::toggled, this, [](bool checked){QSettings().setValue("ALiS/updates/automatic", checked);});
        auto* checkUpdate = new QPushButton(QStringLiteral("Check GitHub for Mac updates now"));
        updatesLayout->addWidget(checkUpdate);
        connect(checkUpdate, &QPushButton::clicked, this, [this](){checkMacUpdates(this);});
#endif
#ifdef Q_OS_WIN
        auto* automaticWindowsUpdate = new QCheckBox(QStringLiteral("Check for updates at startup (at most once a day)"));
        automaticWindowsUpdate->setChecked(QSettings().value("ALiS/updates/automatic", true).toBool());
        automaticWindowsUpdate->setToolTip(QStringLiteral("Contacts the official public GitHub release API. No clouds, models, paths or project data are sent. Includes testing previews. No automatic download or installation."));
        updatesLayout->addWidget(automaticWindowsUpdate);
        connect(automaticWindowsUpdate,&QCheckBox::toggled,this,[](bool enabled){QSettings().setValue("ALiS/updates/automatic",enabled);});
        auto* checkWindowsUpdate = new QPushButton(QStringLiteral("Check GitHub for Windows updates now"));
        updatesLayout->addWidget(checkWindowsUpdate);
        connect(checkWindowsUpdate,&QPushButton::clicked,this,[this](){checkWindowsUpdates(this);});
#endif
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
