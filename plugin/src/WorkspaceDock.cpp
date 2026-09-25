// SPDX-License-Identifier: GPL-2.0-or-later
#include "WorkspaceDock.h"

#include "ModelCatalog.h"
#include "WorkspaceLogic.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDesktopServices>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QPalette>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QTabWidget>
#include <QTabBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace
{
	using alis::WorkspaceDock;
	class PrecisionSpinBox final : public QDoubleSpinBox
	{
	protected:
		QString textFromValue(double value) const override
		{
			QString text=locale().toString(value,'f',decimals());
			if(text.contains(locale().decimalPoint())){while(text.endsWith(locale().zeroDigit()))text.chop(1);if(text.endsWith(locale().decimalPoint()))text.chop(1);}
			return text;
		}
	};

	template <typename T>
	T* named(T* widget, const char* objectName)
	{
		widget->setObjectName(QString::fromLatin1(objectName));
		return widget;
	}

	QLabel* valueLabel(const char* objectName, const QString& initial = QStringLiteral("\u2014"))
	{
		QLabel* label = named(new QLabel(initial), objectName);
		label->setWordWrap(true);
		label->setTextInteractionFlags(Qt::TextSelectableByMouse);
		return label;
	}

	QPushButton* commandButton(const QString& text, const char* objectName)
	{
		QPushButton* button = named(new QPushButton(text), objectName);
		button->setMinimumHeight(30);
		return button;
	}

	QWidget* helpLabel(const QString& text, const QString& help)
	{
		QWidget* row = new QWidget;
		QHBoxLayout* layout = new QHBoxLayout(row);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(4);
		QLabel* label = new QLabel(text);
		QToolButton* info = new QToolButton;
		info->setText(QStringLiteral("i"));
		info->setAutoRaise(true);
		info->setFixedSize(18, 18);
		info->setCursor(Qt::WhatsThisCursor);
		info->setStyleSheet(QStringLiteral("QToolButton { border:1px solid #64748b; border-radius:8px; color:#334155; font-weight:700; }"));
		const QString richHelp = QStringLiteral("<qt><div style='min-width:300px; max-width:430px'><b>%1</b><br><br>%2</div></qt>").arg(text, help);
		label->setToolTip(richHelp);
		info->setToolTip(richHelp);
		info->setAccessibleName(QStringLiteral("Informazioni: %1").arg(text));
		layout->addWidget(label);
		layout->addWidget(info);
		layout->addStretch(1);
		return row;
	}

	void explain(QWidget* widget, const QString& title, const QString& help)
	{
		if (widget)
		{
			widget->setToolTip(QStringLiteral("<qt><div style='min-width:300px; max-width:430px'><b>%1</b><br><br>%2</div></qt>").arg(title, help));
		}
	}

	QVBoxLayout* addScrollableTab(QTabWidget* tabs,
	                              const QString& title,
	                              const char* tabObjectName,
	                              const char* contentObjectName)
	{
		QScrollArea* scroll = named(new QScrollArea, tabObjectName);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);

		QWidget* content = named(new QWidget, contentObjectName);
		QVBoxLayout* layout = new QVBoxLayout(content);
		layout->setContentsMargins(10, 10, 10, 10);
		layout->setSpacing(10);
		scroll->setWidget(content);
		tabs->addTab(scroll, title);
		return layout;
	}

	QFormLayout* formLayout(QGroupBox* group)
	{
		QFormLayout* form = new QFormLayout(group);
		form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
		form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
		form->setHorizontalSpacing(12);
		form->setVerticalSpacing(7);
		return form;
	}

	void addLevels(QComboBox* combo)
	{
		combo->addItem(QStringLiteral("Low"), 0);
		combo->addItem(QStringLiteral("Medium"), 1);
		combo->addItem(QStringLiteral("High"), 2);
		combo->setCurrentIndex(1);
	}

	void configureLengthSpin(QDoubleSpinBox* spin, double value)
	{
		spin->setDecimals(12);
		spin->setRange(0.001, 1000000.0);
		spin->setSingleStep(0.05);
		spin->setValue(value);
		spin->setSuffix(QStringLiteral(" m"));
	}

	double comboDouble(const QComboBox* combo)
	{
		bool ok = false;
		const double value = combo ? combo->currentData().toDouble(&ok) : 0.0;
		return ok ? value : 0.0;
	}
}

namespace alis
{
	WorkspaceDock::WorkspaceDock(QWidget* parent)
		: QDockWidget(QStringLiteral("ALiS Workspace"), parent)
	{
		setObjectName(QStringLiteral("ALiS.WorkspaceDock"));
		setWindowIcon(QIcon(QStringLiteral(":/CC/plugin/ALiS/images/icon.svg")));
		setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
		setMinimumWidth(430);

		QWidget* content = named(new QWidget, "ALiS.Workspace.Content");
		QVBoxLayout* root = new QVBoxLayout(content);
		root->setContentsMargins(8, 8, 8, 8);
		root->setSpacing(8);

		QLabel* title = named(
			new QLabel(QStringLiteral("<b>Archaeological LiDAR Studio</b>"
			                          "<br><span style='color:#64748b'>Original, Working and Derived data remain separate</span>")),
			"ALiS.Workspace.Title");
		title->setWordWrap(true);
		QHBoxLayout* brandRow = new QHBoxLayout;
		QLabel* brandIcon = named(new QLabel, "ALiS.Workspace.BrandIcon");
		brandIcon->setPixmap(QIcon(QStringLiteral(":/CC/plugin/ALiS/images/icon.svg")).pixmap(40, 40));
		brandIcon->setFixedSize(44, 44);
		brandIcon->setAccessibleName(QStringLiteral("ALiS: vegetation, heritage and LiDAR"));
		brandRow->addWidget(brandIcon);
		brandRow->addWidget(title, 1);
		root->addLayout(brandRow);

		m_tabs = named(new QTabWidget, "ALiS.Workspace.Tabs");
		m_tabs->setDocumentMode(true);
		buildSessionTab();
		buildPrepareArea();
		buildAnnotationArea();
		buildModelsTab();
		buildHistoryTab();
		root->addWidget(m_displayPanel);
		root->addWidget(m_tabs, 1);

		m_statusValue = valueLabel("ALiS.Workspace.Status", QStringLiteral("Select a point cloud"));
		root->addWidget(m_statusValue);

		QWidget* progressRow = named(new QWidget, "ALiS.Workspace.Progress.Row");
		QHBoxLayout* progressLayout = new QHBoxLayout(progressRow);
		progressLayout->setContentsMargins(0, 0, 0, 0);
		progressLayout->setSpacing(6);
		m_progressOperationValue = valueLabel("ALiS.Workspace.Progress.Operation", QStringLiteral("Idle"));
		m_progressOperationValue->setMinimumWidth(90);
		m_progressBar = named(new QProgressBar, "ALiS.Workspace.Progress.Bar");
		m_progressBar->setTextVisible(true);
		m_cancelButton = commandButton(QStringLiteral("Cancel"), "ALiS.Workspace.Progress.Cancel");
		m_cancelButton->setEnabled(false);
		connect(m_cancelButton, &QPushButton::clicked, this, &WorkspaceDock::cancelProcessingRequested);
		progressLayout->addWidget(m_progressOperationValue);
		progressLayout->addWidget(m_progressBar, 1);
		progressLayout->addWidget(m_cancelButton);
		root->addWidget(progressRow);

		setWidget(content);
		setScaleRadii({0.10, 0.25, 0.50, 1.00, 2.00, 5.00, 10.00});
		clearSession();
		clearProgress();
	}

	void WorkspaceDock::buildPrepareArea()
	{
		QWidget* page=named(new QWidget,"ALiS.Workspace.Area.Prepare");
		auto* layout=new QVBoxLayout(page);layout->setContentsMargins(8,8,8,8);layout->setSpacing(8);
		QLabel* intro=new QLabel(QStringLiteral("<b>Prepare the cloud</b><br><span style='color:#64748b'>Pre-processing → Terrain → Features. Continue in Classification to separate and review classes.</span>"));
		intro->setWordWrap(true);layout->addWidget(intro);
		m_prepareTabs=named(new QTabWidget,"ALiS.Workspace.Prepare.Tabs");m_prepareTabs->setDocumentMode(true);layout->addWidget(m_prepareTabs,1);
		m_tabs->addTab(page,QIcon(QStringLiteral(":/CC/plugin/ALiS/images/prepare.svg")),QStringLiteral("Prepare"));
		buildPreprocessingTab();buildTerrainTab();buildFeaturesTab();
		m_prepareTabs->setTabIcon(0, QIcon(QStringLiteral(":/CC/plugin/ALiS/images/prepare.svg")));
		m_prepareTabs->setTabIcon(1, QIcon(QStringLiteral(":/CC/plugin/ALiS/images/terrain.svg")));
		m_prepareTabs->setTabIcon(2, QIcon(QStringLiteral(":/CC/plugin/ALiS/images/features.svg")));
	}

	void WorkspaceDock::buildAnnotationArea()
	{
		QWidget* page=named(new QWidget,"ALiS.Workspace.Area.Annotate");
		auto* layout=new QVBoxLayout(page);layout->setContentsMargins(8,8,8,8);layout->setSpacing(8);
		QLabel* intro=new QLabel(QStringLiteral("<b>Annotate and validate</b><br><span style='color:#64748b'>Annotation Studio is also available as an independent ALiS Annotator action in the CloudCompare toolbar.</span>"));
		intro->setWordWrap(true);layout->addWidget(intro);
		m_annotationTabs=named(new QTabWidget,"ALiS.Workspace.Annotate.Tabs");m_annotationTabs->setDocumentMode(true);layout->addWidget(m_annotationTabs,1);
		m_tabs->addTab(page,QIcon(QStringLiteral(":/CC/plugin/ALiS/images/annotator.svg")),QStringLiteral("Annotate"));
		buildAnnotationTab();buildLabelsTab();
	}

	void WorkspaceDock::showAnnotationWorkspace()
	{
		if(m_tabs)m_tabs->setCurrentIndex(2);
		if(m_annotationTabs)m_annotationTabs->setCurrentIndex(0);
	}

	void WorkspaceDock::showPrepareFeatures()
	{
		if(m_tabs)m_tabs->setCurrentIndex(1);
		if(m_prepareTabs)m_prepareTabs->setCurrentIndex(2);
	}

	void WorkspaceDock::buildPreprocessingTab()
	{
		QVBoxLayout* layout = addScrollableTab(
			m_prepareTabs,
			QStringLiteral("Pre-processing"),
			"ALiS.Workspace.Tab.Preprocessing",
			"ALiS.Workspace.Tab.Preprocessing.Content");

		QLabel* introduction = new QLabel(QStringLiteral(
			"<b>Optional cleaning before Ground and feature computation</b><br>"
			"These commands open the original CloudCompare 2.13.2 tools and use CCCoreLib. "
			"They create a new derived cloud, hide the source and preserve the source unchanged."));
		introduction->setWordWrap(true);
		introduction->setStyleSheet(QStringLiteral("QLabel { color:#334155; background:#e0f2fe; padding:8px; border-radius:4px; }"));
		layout->addWidget(introduction);

		QGroupBox* cleaning = named(new QGroupBox(QStringLiteral("Noise, outliers and point reduction")),
		                                  "ALiS.Workspace.Preprocessing.Cleaning");
		QGridLayout* grid = new QGridLayout(cleaning);
		grid->setHorizontalSpacing(8);
		grid->setVerticalSpacing(8);

		QPushButton* sor = commandButton(QStringLiteral("SOR — statistical outliers…"),
		                                 "ALiS.Workspace.Preprocessing.SOR");
		sor->setIcon(QIcon(QStringLiteral(":/CC/images/ccSORFilter.png")));
		sor->setIconSize(QSize(22, 22));
		explain(sor, QStringLiteral("CloudCompare SOR filter"), QStringLiteral(
			"Runs CloudCompare Tools > Clean > SOR filter. For every point, CCCoreLib compares the mean K-neighbour distance with the global distribution and keeps points within the chosen sigma threshold. "
			"Useful for sparse isolated outliers; it can remove real edges or rare structures when K is too high or sigma too low. The native defaults are K=6 and 1 sigma."));

		QPushButton* noise = commandButton(QStringLiteral("Noise — local surface…"),
		                                   "ALiS.Workspace.Preprocessing.Noise");
		// CloudCompare 2.13.2 has no dedicated icon on actionNoiseFilter; use its own generic filter artwork.
		noise->setIcon(QIcon(QStringLiteral(":/CC/images/ccBilateralFilter.png")));
		noise->setIconSize(QSize(22, 22));
		explain(noise, QStringLiteral("CloudCompare Noise filter"), QStringLiteral(
			"Runs CloudCompare Tools > Clean > Noise filter. CCCoreLib fits an approximate local surface and rejects points by point-to-surface distance. "
			"Choose radius or KNN, relative sigma or absolute error, and whether isolated points are removed. Prefer it when noise is measured relative to a surface; inspect roofs, walls, vegetation and archaeological edges before accepting the derived cloud."));

		QPushButton* subsample = commandButton(QStringLiteral("Subsample / thin cloud…"),
		                                       "ALiS.Workspace.Preprocessing.Subsample");
		subsample->setIcon(QIcon(QStringLiteral(":/CC/images/ccSampleCloud.png")));
		subsample->setIconSize(QSize(22, 22));
		explain(subsample, QStringLiteral("CloudCompare subsampling"), QStringLiteral(
			"Runs the native CloudCompare subsampling dialog. Spatial sampling enforces a minimum distance; octree sampling keeps a representative point per cell; random sampling targets a count. "
			"Use a derived copy for fast previews. Subsampling changes point support and therefore spacing, density, suggested feature radii and model compatibility: analyse the new cloud again before processing."));

		grid->addWidget(sor, 0, 0);
		grid->addWidget(noise, 0, 1);
		grid->addWidget(subsample, 1, 0, 1, 2);
		layout->addWidget(cleaning);

		QLabel* workflow = new QLabel(QStringLiteral(
			"Suggested order: inspect the raw cloud → use SOR or Noise only if needed → optionally subsample a preview copy → select the new cloud in the DB Tree → refresh Session statistics → continue with Terrain. "
			"Do not run SOR and Noise blindly in sequence: each filter can remove valid low-density geometry."));
		workflow->setWordWrap(true);
		workflow->setStyleSheet(QStringLiteral("QLabel { color:#475569; background:#f8fafc; border:1px solid #cbd5e1; padding:8px; border-radius:4px; }"));
		layout->addWidget(workflow);
		layout->addStretch(1);

		for (QAbstractButton* button : {sor, noise, subsample}) registerCommandButton(button);
		connect(sor, &QPushButton::clicked, this, [this]() { Q_EMIT preprocessingActionRequested(QStringLiteral("host.sor")); });
		connect(noise, &QPushButton::clicked, this, [this]() { Q_EMIT preprocessingActionRequested(QStringLiteral("host.noise")); });
		connect(subsample, &QPushButton::clicked, this, [this]() { Q_EMIT preprocessingActionRequested(QStringLiteral("host.subsample")); });
	}

	void WorkspaceDock::buildSessionTab()
	{
		QVBoxLayout* layout = addScrollableTab(
			m_tabs,
			QStringLiteral("Session"),
			"ALiS.Workspace.Tab.Session",
			"ALiS.Workspace.Tab.Session.Content");
		m_tabs->setTabIcon(m_tabs->count()-1,QIcon(QStringLiteral(":/CC/plugin/ALiS/images/session.svg")));

		QGroupBox* sessionGroup = named(new QGroupBox(QStringLiteral("Selected Session")),
		                                     "ALiS.Workspace.Session.Group");
		QFormLayout* form = formLayout(sessionGroup);
		m_cloudNameValue = valueLabel("ALiS.Workspace.Session.CloudName");
		m_entityUidValue = valueLabel("ALiS.Workspace.Session.EntityUid");
		m_pointCountValue = valueLabel("ALiS.Workspace.Session.PointCount");
		m_unitsValue = valueLabel("ALiS.Workspace.Session.Units");
		m_metricUnitsCheck = named(new QCheckBox(QStringLiteral("Coordinates are metres")),
		                                "ALiS.Workspace.Session.MetricUnitsConfirmed");
		m_metricUnitsCheck->setToolTip(QStringLiteral("Required for metre-based Ground, DTM and feature radii when CRS metadata is unavailable. The decision is remembered for this cloud fingerprint and is also preserved in CloudCompare BIN metadata. It does not assign or infer an EPSG code."));
		m_dirtyValue = valueLabel("ALiS.Workspace.Session.Dirty");
		form->addRow(QStringLiteral("Cloud"), m_cloudNameValue);
		form->addRow(QStringLiteral("Entity UID"), m_entityUidValue);
		form->addRow(QStringLiteral("Points"), m_pointCountValue);
		form->addRow(QStringLiteral("Scale units"), m_unitsValue);
		form->addRow(QStringLiteral("Unit confirmation"), m_metricUnitsCheck);
		form->addRow(QStringLiteral("Session state"), m_dirtyValue);
		connect(m_metricUnitsCheck, &QCheckBox::toggled, this, [this](bool confirmed)
		{
			m_metricUnitsConfirmed = confirmed;
			updateCommandAvailability();
			Q_EMIT metricUnitsConfirmationChanged(confirmed);
		});
		layout->addWidget(sessionGroup);
		QGroupBox* infoGroup=named(new QGroupBox(QStringLiteral("Point cloud info")),"ALiS.Workspace.CloudInfo");
		auto* infoLayout=new QVBoxLayout(infoGroup);
		m_cloudProfileValue=valueLabel("ALiS.Workspace.CloudInfo.Text",QStringLiteral("Select a point cloud"));
		m_cloudProfileValue->setTextFormat(Qt::PlainText); infoLayout->addWidget(m_cloudProfileValue);
		auto* analyseInfo=commandButton(QStringLiteral("Analyse / refresh spacing, returns and classes"),"ALiS.Workspace.CloudInfo.Refresh");
		explain(analyseInfo,QStringLiteral("Cloud statistics"),QStringLiteral("The first analysis is cached for this CloudCompare entity, so deselecting and selecting it again is immediate. This button forces a fresh scan after external edits. Class/return histograms scan all points; nearest-neighbour statistics use bounded sampled queries against the full cloud. The cloud itself is NOT subsampled. Missing LAS fields remain unknown."));
		infoLayout->addWidget(analyseInfo);registerCommandButton(analyseInfo);connect(analyseInfo,&QPushButton::clicked,this,&WorkspaceDock::cloudProfileRequested);
		layout->addWidget(infoGroup);

		QGroupBox* viewGroup = named(new QGroupBox(QStringLiteral("Display")),
		                                          "ALiS.Workspace.Visualization.Group");
		m_displayPanel = viewGroup;
		QFormLayout* viewForm = formLayout(viewGroup);
		m_visualizationCombo = named(new QComboBox, "ALiS.Workspace.Visualization.Mode");
		m_visualizationCombo->addItem(QStringLiteral("RGB / Original"), QStringLiteral("original"));
		m_visualizationCombo->addItem(QStringLiteral("Ground"), QStringLiteral("ground"));
		m_visualizationCombo->addItem(QStringLiteral("Non-ground"), QStringLiteral("non_ground"));
		m_visualizationCombo->addItem(QStringLiteral("Height Above Ground"), QStringLiteral("hag"));
		m_visualizationCombo->addItem(QStringLiteral("ASPRS Working"), QStringLiteral("asprs"));
		m_visualizationCombo->addItem(QStringLiteral("ASPRS Prediction (Derived)"), QStringLiteral("prediction"));
		m_visualizationCombo->addItem(QStringLiteral("Prediction Confidence"), QStringLiteral("prediction_confidence"));
		QWidget* modeRow = new QWidget;
		QHBoxLayout* modeLayout = new QHBoxLayout(modeRow);
		modeLayout->setContentsMargins(0, 0, 0, 0);
		QPushButton* refreshFields = commandButton(QStringLiteral("Refresh"), "ALiS.Display.Refresh");
		modeLayout->addWidget(m_visualizationCombo, 1);
		modeLayout->addWidget(refreshFields);
		viewForm->addRow(helpLabel(QStringLiteral("Show"), QStringLiteral("RGB, Ground or any scalar field on the selected cloud. Refresh picks up fields created by other CloudCompare tools. Changing colour never changes point values.")), modeRow);
		connect(refreshFields, &QPushButton::clicked, this, &WorkspaceDock::displayRefreshRequested);
		m_displayPaletteCombo = named(new QComboBox, "ALiS.Display.Palette");
		m_displayEditButton = commandButton(QStringLiteral("Edit…"), "ALiS.Display.EditPalette");
		QWidget* paletteRow = new QWidget;
		QHBoxLayout* paletteLayout = new QHBoxLayout(paletteRow);
		paletteLayout->setContentsMargins(0, 0, 0, 0);
		paletteLayout->addWidget(m_displayPaletteCombo, 1);
		paletteLayout->addWidget(m_displayEditButton);
		viewForm->addRow(helpLabel(QStringLiteral("Palette"), QStringLiteral("All CloudCompare palettes, including custom scales. Edit opens the native colour-scale editor. RGB does not use a scalar palette.")), paletteRow);
		m_displayMinSpin = named(new PrecisionSpinBox, "ALiS.Display.Minimum");
		m_displayMaxSpin = named(new PrecisionSpinBox, "ALiS.Display.Maximum");
		for (QDoubleSpinBox* spin : {m_displayMinSpin, m_displayMaxSpin})
		{
			spin->setDecimals(6);
			spin->setRange(-1e30, 1e30);
			spin->setKeyboardTracking(false);
			spin->setMinimumWidth(80);
		}
		m_displayAutoButton = commandButton(QStringLiteral("Auto"), "ALiS.Display.AutoRange");
		QWidget* rangeRow = new QWidget;
		QHBoxLayout* rangeLayout = new QHBoxLayout(rangeRow);
		rangeLayout->setContentsMargins(0, 0, 0, 0);
		rangeLayout->addWidget(m_displayMinSpin);
		rangeLayout->addWidget(new QLabel(QStringLiteral("to")));
		rangeLayout->addWidget(m_displayMaxSpin);
		rangeLayout->addWidget(m_displayAutoButton);
		viewForm->addRow(helpLabel(QStringLiteral("Colour range"), QStringLiteral("Endpoints of colour saturation, not a point filter. Values outside this interval keep endpoint colours. Auto restores the field's native range; no points are removed or reclassified.")), rangeRow);
		m_displayLegendCheck = named(new QCheckBox(QStringLiteral("Show colour scale legend")), "ALiS.Display.ShowLegend");
		m_displayLegendCheck->setToolTip(QStringLiteral("Shows or hides CloudCompare's scalar-field colour scale at the side of the 3D view. This changes only the display, never the data."));
		viewForm->addRow(helpLabel(QStringLiteral("Legend"), QStringLiteral("Toggle the scalar-field colour scale shown at the side of the CloudCompare 3D view. The setting follows the selected cloud and remains unchanged when switching between scalar fields.")), m_displayLegendCheck);
		connect(m_displayPaletteCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { Q_EMIT displayPaletteRequested(m_displayPaletteCombo->currentData().toString()); });
		for (QDoubleSpinBox* spin : {m_displayMinSpin, m_displayMaxSpin})
			connect(spin, &QDoubleSpinBox::editingFinished, this, [this]() { Q_EMIT displayRangeRequested(m_displayMinSpin->value(), m_displayMaxSpin->value()); });
		connect(m_displayAutoButton, &QPushButton::clicked, this, &WorkspaceDock::displayRangeResetRequested);
		connect(m_displayEditButton, &QPushButton::clicked, this, &WorkspaceDock::displayPaletteEditorRequested);
		connect(m_displayLegendCheck, &QCheckBox::toggled, this, &WorkspaceDock::displayLegendVisibilityRequested);
		connect(m_visualizationCombo,
		        qOverload<int>(&QComboBox::currentIndexChanged),
		        this,
		        [this](int)
		        {
			        Q_EMIT visualizationRequested(m_visualizationCombo->currentData().toString());
		        });
		setDisplayRange(false, 0, 1, 0, 1);

		QLabel* note = named(
			new QLabel(QStringLiteral("One classification field is used: standard ASPRS classes plus ALiS archaeological classes in the LAS user-defined range 64–75. Ground is always class 2.")),
			"ALiS.Workspace.Session.IndependenceNote");
		note->setWordWrap(true);
		note->setStyleSheet(QStringLiteral("QLabel { color:#475569; background:#f1f5f9; padding:8px; border-radius:4px; }"));
		layout->addWidget(note);
		QLabel* saveNote = named(new QLabel(QStringLiteral(
			"Resume work: select the cloud in the DB Tree and use File > Save > CloudCompare BIN. "
			"Reopen the BIN to recover labels, scalar fields and History.")), "ALiS.Workspace.Session.SaveNote");
		saveNote->setWordWrap(true);
		explain(saveNote, QStringLiteral("Saving and reopening"), QStringLiteral(
			"BIN stores all points, applied labels, Ground/HAG fields, materialized features, display ranges and processing history. "
			"Unapplied selections, Studio cameras/sections, feature caches and Undo/Redo are not saved. "
			"LAS/LAZ is an exchange format: explicitly map Working to Classification and include qAL extra fields; "
			"it does not store the project history. Never overwrite the source LAS. "
			"After reopening, confirm metre units if needed. Features published as SF retain their feature/radius identities and can be reused by Models without recomputation."));
		layout->addWidget(saveNote);
		layout->addStretch(1);
	}

	void WorkspaceDock::buildTerrainTab()
	{
		QVBoxLayout* layout = addScrollableTab(
			m_prepareTabs,
			QStringLiteral("Terrain"),
			"ALiS.Workspace.Tab.Terrain",
			"ALiS.Workspace.Tab.Terrain.Content");

		QGroupBox* methodGroup = named(new QGroupBox(QStringLiteral("1. Ground method")),
		                                    "ALiS.Workspace.Terrain.MethodGroup");
		QFormLayout* methodForm = formLayout(methodGroup);
		m_groundAlgorithmCombo = named(new QComboBox, "ALiS.Workspace.Terrain.Algorithm");
		m_groundAlgorithmCombo->addItem(QStringLiteral("CloudCompare CSF (native)"), QStringLiteral("csf.cloudcompare.v2.13.2"));
		m_groundAlgorithmCombo->addItem(QStringLiteral("PMF raster (Zhang / inspired by lidR)"), QStringLiteral("pmf.lidr_zhang.fast_raster.v1"));
		methodForm->addRow(helpLabel(QStringLiteral("Algorithm"),
			QStringLiteral("<b>CSF</b> simulates a cloth over the inverted cloud and is generally the best first choice. "
			               "<b>PMF</b> progressively opens an elevation surface: it is useful as an independent comparison, but is more sensitive to window/threshold tuning.")),
			m_groundAlgorithmCombo);
		explain(m_groundAlgorithmCombo, QStringLiteral("Ground algorithm"),
			QStringLiteral("Start with CloudCompare CSF. Compare PMF when shrubs, walls or terraces are being absorbed into Ground. Always use Preview before Apply."));
		QLabel* methodNote = new QLabel(QStringLiteral("CSF: same engine as CloudCompare. PMF: raster approximation, NOT the point-based lidR implementation. Neither uses reference classes or removes source points."));
		methodNote->setWordWrap(true);
		methodForm->addRow(methodNote);
		layout->addWidget(methodGroup);
		QGroupBox* recipeGroup=named(new QGroupBox(QStringLiteral("Final — Save or reuse this processing recipe (optional)")),"ALiS.Workspace.Presets");
		auto* recipeForm=formLayout(recipeGroup);
		QLabel* recipePurpose = new QLabel(QStringLiteral(
			"A recipe is a reusable setup, not another filter. It remembers the qCSF/PMF settings, context controls, DTM grid step, selected geometric features and their metric radii. "
			"“Multiscale” refers to the feature radii: Ground still runs one qCSF cloth or one PMF sequence per Preview."));
		recipePurpose->setWordWrap(true);
		recipePurpose->setStyleSheet(QStringLiteral("QLabel { color:#334155; background:#fef3c7; padding:8px; border-radius:4px; }"));
		recipeForm->addRow(recipePurpose);
		m_savedPresetCombo=named(new QComboBox,"ALiS.Workspace.Presets.Library");m_savedPresetCombo->addItem(QStringLiteral("Choose a saved preset…"),QString());
		recipeForm->addRow(helpLabel(QStringLiteral("Library"),QStringLiteral("A recipe stores the ground algorithm/base parameters, context controls, PMF windows/cell, DTM step, selected features and all radii. Loading restores exact values. Adapt to this cloud is a separate, explicit action.")),m_savedPresetCombo);
		auto* saveRecipe=commandButton(QStringLiteral("Save as new…"),"ALiS.Workspace.Presets.Save");
		auto* importRecipe=commandButton(QStringLiteral("Import JSON…"),"ALiS.Workspace.Presets.Import");
		auto* adaptRecipe=commandButton(QStringLiteral("Suggest / adapt to this cloud"),"ALiS.Workspace.Presets.Adapt");
		recipeForm->addRow(saveRecipe,importRecipe);recipeForm->addRow(adaptRecipe);
		m_presetMinRadius=named(new PrecisionSpinBox,"ALiS.Workspace.Presets.MinRadius");configureLengthSpin(m_presetMinRadius,.1);
		m_presetMaxRadius=named(new PrecisionSpinBox,"ALiS.Workspace.Presets.MaxRadius");configureLengthSpin(m_presetMaxRadius,3.);
		m_presetMinRadius->setMaximum(1000);m_presetMaxRadius->setMaximum(1000);
		recipeForm->addRow(helpLabel(QStringLiteral("Suggested radius minimum"),QStringLiteral("Lower bound for feature scales, not cloth resolution. The 16/64/256 neighbour targets are heuristics, not paper-validated optimal scales for this survey.")),m_presetMinRadius);
		recipeForm->addRow(helpLabel(QStringLiteral("Suggested radius maximum"),QStringLiteral("Upper feature radius bound; also limited to one quarter of the shorter cloud extent. Large radii are expensive on dense UAV clouds. Adapting never tightens CSF height tolerance.")),m_presetMaxRadius);
		m_presetUseReturns=named(new QCheckBox(QStringLiteral("Use reliable return metadata in scale suggestions")),"ALiS.Workspace.Presets.Returns");m_presetUseReturns->setChecked(true);
		explain(m_presetUseReturns,QStringLiteral("Return-aware support"),QStringLiteral("Only used when at least 90% of paired return indices/counts are valid. A bounded last-return fraction adjusts the effective support density, making suggested radii coarser on multi-return scenes. Last returns are NOT ground. This heuristic can be disabled; missing metadata is never invented."));recipeForm->addRow(m_presetUseReturns);
		m_processingSuggestion=valueLabel("ALiS.Workspace.Presets.Explanation",QStringLiteral("Loading restores exact saved values. Suggest / adapt is a separate action based on the current cloud. No automatic union of Ground masks is performed."));
		recipeForm->addRow(m_processingSuggestion);
		for(auto* button:{saveRecipe,importRecipe,adaptRecipe})registerCommandButton(button);
		connect(saveRecipe,&QPushButton::clicked,this,&WorkspaceDock::saveProcessingPresetRequested);
		connect(importRecipe,&QPushButton::clicked,this,&WorkspaceDock::importProcessingPresetRequested);
		connect(adaptRecipe,&QPushButton::clicked,this,&WorkspaceDock::adaptProcessingPresetRequested);
		connect(m_savedPresetCombo,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int){QString path=m_savedPresetCombo->currentData().toString();if(!path.isEmpty())Q_EMIT loadProcessingPresetRequested(path);});

		QWidget* simplePage = named(new QWidget, "ALiS.Workspace.Terrain.Simple");
		QFormLayout* simpleForm = new QFormLayout(simplePage);
		simpleForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
		m_groundPresetCombo = named(new QComboBox, "ALiS.Workspace.Terrain.Preset");
		m_groundPresetCombo->addItems({QStringLiteral("Flat / open terrain"),
		                               QStringLiteral("Archaeology / subtle relief"),
		                               QStringLiteral("Terraces / scarps"),
		                               QStringLiteral("Hilly / steep terrain"),
		                               QStringLiteral("Rocky / irregular surface"),
		                               QStringLiteral("Forest"),
		                               QStringLiteral("Dense shrubs / undergrowth"),
		                               QStringLiteral("CloudCompare reference (2 m / 0.5 m)"),
		                               QStringLiteral("Custom / modified")});
		m_groundPresetCombo->setCurrentIndex(7);
		m_terrainComplexityCombo = named(new QComboBox, "ALiS.Workspace.Terrain.Complexity");
		m_microreliefCombo = named(new QComboBox, "ALiS.Workspace.Terrain.PreserveMicrorelief");
		m_vegetationCombo = named(new QComboBox, "ALiS.Workspace.Terrain.VegetationDensity");
		addLevels(m_terrainComplexityCombo);
		addLevels(m_microreliefCombo);
		addLevels(m_vegetationCombo);
		simpleForm->addRow(helpLabel(QStringLiteral("Preset"),
			QStringLiteral("CloudCompare reference restores its GUI initial values: cloth 2 m, threshold 0.5 m, Relief, 500 iterations, slope off. Other presets are editable starting points, not validated universal settings. Total-cloud density is often dominated by canopy, so it must not automatically tighten the vertical tolerance.")), m_groundPresetCombo);
		simpleForm->addRow(helpLabel(QStringLiteral("Terrain complexity"),
			QStringLiteral("Low selects Flat; Medium keeps the loaded preset's terrain type; High selects Steep slope and slope recovery. Computed from the loaded base each time, never cumulatively. Vegetation alone is not evidence of steep or flat ground.")), m_terrainComplexityCombo);
		simpleForm->addRow(helpLabel(QStringLiteral("Preserve microrelief"),
			QStringLiteral("High refines the base cloth by 25%; Low coarsens it by 40%; Medium keeps the base. This does NOT tighten the height threshold. Inspect walls and microrelief separately: fine cloth may also follow low objects.")), m_microreliefCombo);
		simpleForm->addRow(helpLabel(QStringLiteral("Vegetation density"),
			QStringLiteral("High coarsens the base cloth by 25% to test sparse ground support; Low refines it by 20%. Neither changes threshold, rigidity nor slope recovery. If ground is missing under trees, first compare the 0.5 m reference threshold. Last returns are not necessarily ground.")), m_vegetationCombo);
		m_groundPresetGuidance = valueLabel("ALiS.Workspace.Terrain.PresetGuidance");
		m_groundPresetGuidance->setStyleSheet(QStringLiteral("QLabel { color:#334155; background:#e0f2fe; padding:8px; border-radius:4px; }"));
		simpleForm->addRow(m_groundPresetGuidance);
		QLabel* simpleNote = new QLabel(QStringLiteral("Workflow: choose a preset, run Preview, inspect Ground and Non-ground, then change only one control at a time."));
		simpleNote->setWordWrap(true);
		simpleForm->addRow(simpleNote);
		layout->addWidget(simplePage);

		QWidget* advancedPage = named(new QWidget, "ALiS.Workspace.Terrain.Advanced");
		QVBoxLayout* advancedLayout = new QVBoxLayout(advancedPage);
		m_csfParametersGroup = named(new QGroupBox(QStringLiteral("CloudCompare qCSF parameters")),
		                                  "ALiS.Workspace.Terrain.CsfParameters");
		QFormLayout* advancedForm = formLayout(m_csfParametersGroup);
		m_clothResolutionSpin = named(new PrecisionSpinBox, "ALiS.Workspace.Terrain.ClothResolution");
		m_classificationThresholdSpin = named(new PrecisionSpinBox, "ALiS.Workspace.Terrain.ClassificationThreshold");
		m_timeStepSpin = named(new PrecisionSpinBox, "ALiS.Workspace.Terrain.TimeStep");
		configureLengthSpin(m_clothResolutionSpin, 2.0);
		configureLengthSpin(m_classificationThresholdSpin, 0.5);
		m_timeStepSpin->setDecimals(12);
		m_timeStepSpin->setRange(0.001, 10.0);
		m_timeStepSpin->setSingleStep(0.05);
		m_timeStepSpin->setValue(0.65);
		m_csfSceneCombo = named(new QComboBox, "ALiS.Workspace.Terrain.Scene");
		m_csfSceneCombo->addItem(QStringLiteral("Steep slope — flexible (1)"),1);
		m_csfSceneCombo->addItem(QStringLiteral("Relief — medium (2)"),2);
		m_csfSceneCombo->addItem(QStringLiteral("Flat — rigid (3)"),3);
		m_csfSceneCombo->setCurrentIndex(1);
		m_iterationsSpin = named(new QSpinBox, "ALiS.Workspace.Terrain.Iterations");
		m_iterationsSpin->setRange(1, 100000);
		m_iterationsSpin->setValue(500);
		m_slopeProcessingCheck = named(new QCheckBox(QStringLiteral("Enable slope post-processing")),
		                               "ALiS.Workspace.Terrain.SlopeProcessing");
		advancedForm->addRow(helpLabel(QStringLiteral("Cloth resolution"),
			QStringLiteral("Grid size of the simulated cloth, in metres. Smaller values preserve finer terrain but can follow bushes, roofs and walls. Larger values reject objects more strongly but can smooth archaeological microrelief.")), m_clothResolutionSpin);
		advancedForm->addRow(helpLabel(QStringLiteral("Classification threshold"),
			QStringLiteral("Absolute point-to-cloth distance accepted as Ground. CloudCompare starts at 0.5 m. A 0.1 m band can reject real ground below vegetation or on rough slopes. More points per square metre do NOT justify a smaller vertical tolerance. First compare 0.5 m, then change one parameter at a time. Larger values can admit vegetation.")), m_classificationThresholdSpin);
		advancedForm->addRow(helpLabel(QStringLiteral("Time step"),
			QStringLiteral("Internal CSF simulation step. CloudCompare normally uses 0.65. Change it only to diagnose instability; it is not a terrain-scale parameter.")), m_timeStepSpin);
		advancedForm->addRow(helpLabel(QStringLiteral("Terrain type (CloudCompare)"),
			QStringLiteral("Same mapping as native CSF: Steep slope=1, Relief=2, Flat=3. Choose the terrain below vegetation, not the canopy shape. Flat/rigid can bridge depressions and omit ground; flexible cloth can follow objects.")), m_csfSceneCombo);
		advancedForm->addRow(helpLabel(QStringLiteral("Iterations"),
			QStringLiteral("Maximum CSF simulation iterations. 500 matches CloudCompare's default. More iterations usually cost time without fixing a wrong resolution or threshold.")), m_iterationsSpin);
		advancedForm->addRow(helpLabel(QStringLiteral("Slope post-processing"),
			QStringLiteral("Post-processing for ground omitted on slopes. Enable when there are genuine scarps or slopes, including below vegetation; inspect possible false ground. Vegetation controls never disable it. With Global Scale other than 1, native CSF's internal fixed thresholds are in local units.")), m_slopeProcessingCheck);
		advancedLayout->addWidget(m_csfParametersGroup);

		m_pmfParametersGroup = named(new QGroupBox(QStringLiteral("Progressive Morphological Filter parameters")),
		                                  "ALiS.Workspace.Terrain.PmfParameters");
		QFormLayout* pmfForm = formLayout(m_pmfParametersGroup);
		m_pmfWindowsEdit = named(new QLineEdit(QStringLiteral("3, 6, 12, 20")),
		                         "ALiS.Workspace.Terrain.PmfWindows");
		m_pmfThresholdsEdit = named(new QLineEdit(QStringLiteral("0.5, 0.8, 1.4, 2.0")),
		                            "ALiS.Workspace.Terrain.PmfThresholds");
		m_pmfWindowsEdit->setToolTip(QStringLiteral("Increasing full window widths in metres, separated by commas."));
		m_pmfThresholdsEdit->setToolTip(QStringLiteral("One height threshold in metres for each window."));
		m_pmfPresetCombo = named(new QComboBox, "ALiS.Workspace.Terrain.PmfPreset");
		m_pmfPresetCombo->addItem(QStringLiteral("Balanced / recover ground (starting point)"));
		m_pmfPresetCombo->addItem(QStringLiteral("lidR example ws/th (not identical engine)"));
		m_pmfPresetCombo->addItem(QStringLiteral("Complex / steep terrain"));
		m_pmfPresetCombo->addItem(QStringLiteral("Detailed low relief (inspect omissions)"));
		m_pmfPresetCombo->addItem(QStringLiteral("Custom / modified"));
		m_pmfCellSizeSpin = named(new PrecisionSpinBox,"ALiS.Workspace.Terrain.PmfCellSize");
		configureLengthSpin(m_pmfCellSizeSpin,.5); m_pmfCellSizeSpin->setMinimum(0); m_pmfCellSizeSpin->setSpecialValueText(QStringLiteral("Auto (first window / 6)"));
		pmfForm->addRow(helpLabel(QStringLiteral("Raster cell"),QStringLiteral("Internal raster step; not point decimation. 0 selects first window/6. Coarse cells merge slope elevations and can omit ground; fine cells cost RAM and contain more gaps. The 8-million-cell safety limit can coarsen this step; actual cell size is logged. Window support is rounded to whole cells.")),m_pmfCellSizeSpin);
		pmfForm->addRow(helpLabel(QStringLiteral("PMF preset"),
			QStringLiteral("Balanced uses a 0.5 m initial tolerance to avoid the old overly strict 0.08 m default. Steep tolerates more relief. Detailed is stricter and can omit ground. The lidR example only copies its documented ws/th values: our raster algorithm has different neighbourhood support.")), m_pmfPresetCombo);
		pmfForm->addRow(helpLabel(QStringLiteral("Window widths ws (m)"),
			QStringLiteral("Increasing neighbourhood widths, in metres. Each step removes objects smaller than its window. The number of widths must equal the number of thresholds.")), m_pmfWindowsEdit);
		pmfForm->addRow(helpLabel(QStringLiteral("Height thresholds th (m)"),
			QStringLiteral("Maximum elevation difference admitted at each window. Lower early thresholds reject low shrubs; values that are too low can remove stones, banks and archaeological microrelief.")), m_pmfThresholdsEdit);
		QLabel* pmfNote = new QLabel(QStringLiteral("PMF is a comparison filter in this build. Preview the same tile with CSF and PMF before choosing."));
		pmfNote->setWordWrap(true);
		pmfForm->addRow(pmfNote);
		m_pmfParametersGroup->setVisible(false);
		advancedLayout->addWidget(m_pmfParametersGroup);
		advancedLayout->addStretch(1);
		layout->addWidget(advancedPage);

		connect(m_groundAlgorithmCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
		        [this, simplePage](int)
		        {
			        const QString id = m_groundAlgorithmCombo->currentData().toString();
			        const bool csf = id.startsWith(QStringLiteral("csf."));
			        simplePage->setVisible(csf);
			        m_csfParametersGroup->setVisible(csf);
			        m_pmfParametersGroup->setVisible(!csf);
			        Q_EMIT groundAlgorithmChanged(id);
		        });

		connect(m_groundPresetCombo,
		        qOverload<int>(&QComboBox::currentIndexChanged),
		        this,
		        [this](int index)
		        {
			        static const QStringList guidance = {
				        QStringLiteral("Flat/open: 2 m cloth, 0.5 m band, rigid. Inspect banks and depressions."),
				        QStringLiteral("Archaeology: 1 m cloth, 0.5 m band, Relief. Check low structures separately; not an archaeology classifier."),
				        QStringLiteral("Terraces/scarps: medium rigidity with slope recovery. Inspect terrace edges for omission."),
				        QStringLiteral("Hilly/steep: flexible cloth and slope recovery. Use only for genuine terrain slopes; vegetation leakage risk is higher."),
				        QStringLiteral("Rocky: 0.5 m cloth, 0.5 m band, flexible + slope. Inspect rocks and small structures."),
				        QStringLiteral("Forest: start from native reference; choose the terrain type below the canopy. No density-derived tightening."),
				        QStringLiteral("Dense vegetation: native reference first. Sparse last returns are not necessarily ground; inspect false positives."),
				        QStringLiteral("Native GUI reference: 2 m cloth, 0.5 m band, Relief (2), 500 iterations, 0.65 time step, slope off."),
				        QStringLiteral("Modified settings. All effective parameters are shown below; save a named multiscale preset to reuse them.")
			        };
			        if (m_groundPresetGuidance && index >= 0 && index < guidance.size()) m_groundPresetGuidance->setText(guidance.at(index));
			        Q_EMIT groundPresetChanged(index);
		        });
		m_groundPresetGuidance->setText(QStringLiteral("Native GUI reference: 2 m cloth, 0.5 m band, Relief (2), 500 iterations, slope off."));
		for (QComboBox* combo : {m_terrainComplexityCombo, m_microreliefCombo, m_vegetationCombo})
		{
			connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { emitSimpleGroundControls(); });
		}
		connect(m_clothResolutionSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) { emitAdvancedGroundParameters(); });
		connect(m_classificationThresholdSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) { emitAdvancedGroundParameters(); });
		connect(m_timeStepSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double) { emitAdvancedGroundParameters(); });
		connect(m_csfSceneCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { emitAdvancedGroundParameters(); });
		connect(m_iterationsSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { emitAdvancedGroundParameters(); });
		connect(m_slopeProcessingCheck, &QCheckBox::toggled, this, [this](bool) { emitAdvancedGroundParameters(); });
		connect(m_pmfWindowsEdit, &QLineEdit::editingFinished, this, &WorkspaceDock::emitPmfParameters);
		connect(m_pmfThresholdsEdit, &QLineEdit::editingFinished, this, &WorkspaceDock::emitPmfParameters);
		connect(m_pmfCellSizeSpin,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this](double){emitPmfParameters();});
		connect(m_pmfPresetCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index)
		{
			if(index==4) return;
			const QSignalBlocker cellBlock(m_pmfCellSizeSpin);
			m_pmfCellSizeSpin->setValue(.5);
			if (index == 0)
			{
				m_pmfWindowsEdit->setText(QStringLiteral("3, 6, 12, 20"));
				m_pmfThresholdsEdit->setText(QStringLiteral("0.5, 0.8, 1.4, 2.0"));
			}
			else if (index == 1)
			{
				m_pmfWindowsEdit->setText(QStringLiteral("3, 6, 9, 12"));
				m_pmfThresholdsEdit->setText(QStringLiteral("0.10, 0.57, 1.03, 1.50"));
			}
			else if(index==2)
			{
				m_pmfWindowsEdit->setText(QStringLiteral("3, 6, 12, 20"));
				m_pmfThresholdsEdit->setText(QStringLiteral("0.5, 1.2, 2.0, 3.0"));
			}
			else { m_pmfWindowsEdit->setText(QStringLiteral("1, 2, 4, 8")); m_pmfThresholdsEdit->setText(QStringLiteral("0.2, 0.3, 0.5, 0.8")); m_pmfCellSizeSpin->setValue(.25); }
			Q_EMIT pmfParametersChanged(m_pmfWindowsEdit->text(),m_pmfThresholdsEdit->text(),m_pmfCellSizeSpin->value());
		});

		QGroupBox* actionsGroup = named(new QGroupBox(QStringLiteral("Ground workflow")),
		                                     "ALiS.Workspace.Terrain.Actions");
		QGridLayout* actions = new QGridLayout(actionsGroup);
		m_groundPreviewButton = commandButton(QStringLiteral("Ground Preview"), "ALiS.Workspace.Terrain.Preview");
		m_groundApplyButton = commandButton(QStringLiteral("Apply"), "ALiS.Workspace.Terrain.Apply");
		m_groundDiscardButton = commandButton(QStringLiteral("Discard"), "ALiS.Workspace.Terrain.Discard");
		m_groundRestoreButton = commandButton(QStringLiteral("Restore"), "ALiS.Workspace.Terrain.Restore");
		m_computeDtmButton = commandButton(QStringLiteral("Compute DTM"), "ALiS.Workspace.Terrain.ComputeDtm");
		m_computeHagButton = commandButton(QStringLiteral("Compute HAG"), "ALiS.Workspace.Terrain.ComputeHag");
		m_openDemButton = commandButton(QStringLiteral("DEM / DSM — CloudCompare Rasterize…"), "ALiS.Workspace.Terrain.OpenDem");
		m_openDemButton->setIcon(QIcon(QStringLiteral(":/CC/images/ccGrid.png")));
		m_openDemButton->setIconSize(QSize(22, 22));
		m_dtmGridStepSpin = named(new PrecisionSpinBox, "ALiS.Workspace.Terrain.DtmGridStep");
		configureLengthSpin(m_dtmGridStepSpin, 0.25);
		m_dtmFillEmptyCheck = named(new QCheckBox(QStringLiteral("Fill empty DTM cells with Delaunay interpolation")),
		                                  "ALiS.Workspace.Terrain.DtmFillEmpty");
		m_dtmFillEmptyCheck->setChecked(false);
		m_dtmMaxEdgeSpin = named(new PrecisionSpinBox, "ALiS.Workspace.Terrain.DtmMaxEdge");
		configureLengthSpin(m_dtmMaxEdgeSpin, 0.75);
		m_dtmMaxEdgeSpin->setMinimum(0.0);
		m_dtmMaxEdgeSpin->setSpecialValueText(QStringLiteral("No limit"));
		m_dtmMaxEdgeSpin->setEnabled(false);
		actions->addWidget(m_groundPreviewButton, 0, 0, 1, 2);
		actions->addWidget(m_groundApplyButton, 1, 0);
		actions->addWidget(m_groundDiscardButton, 1, 1);
		actions->addWidget(m_groundRestoreButton, 2, 0, 1, 2);
		actions->addWidget(helpLabel(QStringLiteral("DTM grid step"),
			QStringLiteral("Raster cell size used for the derived terrain model. A good first value is roughly 2–5 times point spacing, but never finer than the effective Ground support.")), 3, 0);
		actions->addWidget(m_dtmGridStepSpin, 3, 1);
		actions->addWidget(m_dtmFillEmptyCheck, 4, 0, 1, 2);
		actions->addWidget(helpLabel(QStringLiteral("Delaunay max edge"),
			QStringLiteral("Maximum triangle edge used to interpolate empty DTM cells. A finite limit avoids bridging wide gaps, buildings or disconnected terrain support; 0 removes the limit. Cells outside supported triangles remain NODATA.")), 5, 0);
		actions->addWidget(m_dtmMaxEdgeSpin, 5, 1);
		actions->addWidget(m_computeDtmButton, 6, 0);
		actions->addWidget(m_computeHagButton, 6, 1);
		actions->addWidget(m_openDemButton, 7, 0, 1, 2);
		layout->addWidget(actionsGroup);
		explain(m_groundPreviewButton, QStringLiteral("Ground Preview"), QStringLiteral("Computes a reversible Ground/non-ground result. Use this before Apply; it does not overwrite ASPRS labels."));
		explain(m_groundApplyButton, QStringLiteral("Apply"), QStringLiteral("Accepts the current preview in the working session. Original imported values remain restorable."));
		explain(m_groundDiscardButton, QStringLiteral("Discard"), QStringLiteral("Removes the current preview and returns to the last applied Ground state."));
		explain(m_groundRestoreButton, QStringLiteral("Restore"), QStringLiteral("Restores the original Ground state from the imported cloud."));
		explain(m_computeDtmButton, QStringLiteral("Compute DTM"), QStringLiteral("Builds a minimum-Z terrain model from the accepted Ground mask. Empty cells are left as NODATA unless the explicit Delaunay option is enabled. HAG is derived only from this DTM."));
		explain(m_computeHagButton, QStringLiteral("Compute HAG"), QStringLiteral("Computes Height Above Ground. Requires an accepted Ground result and a DTM."));
		explain(m_openDemButton, QStringLiteral("DEM / DSM with CloudCompare Rasterize"), QStringLiteral("Opens CloudCompare's native Rasterize tool on the selected cloud. Use all points and choose minimum, average or maximum Z according to the required elevation/surface model; the native dialog also offers Delaunay, Kriging and other empty-cell strategies plus cloud, mesh and raster export. This is deliberately separate from the Ground-only DTM used for HAG."));
		explain(m_dtmFillEmptyCheck, QStringLiteral("Fill DTM voids"), QStringLiteral("Uses the same ccRasterGrid Delaunay interpolation as CloudCompare. It can increase HAG coverage, but interpolation is an estimate and may bridge genuine gaps. The chosen option and maximum edge are recorded in History."));

		for (QAbstractButton* button : {m_groundPreviewButton,
		                                m_groundApplyButton,
		                                m_groundDiscardButton,
		                                m_groundRestoreButton,
		                                m_computeDtmButton,
		                                m_computeHagButton,
		                                m_openDemButton})
		{
			registerCommandButton(button);
		}
		connect(m_groundPreviewButton, &QPushButton::clicked, this, &WorkspaceDock::groundPreviewRequested);
		connect(m_groundApplyButton, &QPushButton::clicked, this, &WorkspaceDock::groundApplyRequested);
		connect(m_groundDiscardButton, &QPushButton::clicked, this, &WorkspaceDock::groundDiscardRequested);
		connect(m_groundRestoreButton, &QPushButton::clicked, this, &WorkspaceDock::groundRestoreRequested);
		connect(m_dtmFillEmptyCheck, &QCheckBox::toggled, this, [this](bool enabled) { m_dtmMaxEdgeSpin->setEnabled(enabled && !m_busy); });
		connect(m_computeDtmButton, &QPushButton::clicked, this, [this]() {
			Q_EMIT computeDtmRequested(m_dtmGridStepSpin->value(), m_dtmFillEmptyCheck->isChecked(), m_dtmMaxEdgeSpin->value());
		});
		connect(m_computeHagButton, &QPushButton::clicked, this, &WorkspaceDock::computeHagRequested);
		connect(m_openDemButton, &QPushButton::clicked, this, [this]() { Q_EMIT preprocessingActionRequested(QStringLiteral("host.rasterize")); });

		m_groundSummaryValue = valueLabel("ALiS.Workspace.Terrain.Summary", QStringLiteral("No Ground result"));
		layout->addWidget(m_groundSummaryValue);
		layout->addWidget(recipeGroup);
		layout->addStretch(1);
	}

	void WorkspaceDock::buildVegetationTab()
	{
		QVBoxLayout* layout = addScrollableTab(m_modelsTabs,
			QStringLiteral("Vegetation / structures"),
			"ALiS.Workspace.Tab.Vegetation", "ALiS.Workspace.Tab.Vegetation.Content");
		auto* intro = new QLabel(QStringLiteral("<b>Vegetation / structures — experimental</b><br>Apply Ground in Terrain first. Use adaptive geometry or a trusted local model below, then inspect proposals with feature intervals. Retained points are not automatically Building."));
		intro->setWordWrap(true);
		layout->addWidget(intro);
		auto* fieldGuide=new QLabel(QStringLiteral("<b>Choose SFs / fields:</b> open Feature distributions / range review → select a field in the left list → set minimum / maximum and enable Use range for this feature → Apply all enabled ranges.<br>Then use Refine vegetation using saved feature ranges below. All enabled ranges combine with AND. The trained model keeps its fixed input schema; these extra fields refine its proposals afterwards."));
		fieldGuide->setWordWrap(true);layout->addWidget(fieldGuide);
		auto* rangeReview=commandButton(QStringLiteral("Feature distributions / range review…"),"ALiS.Workspace.Features.RangeReview");
		rangeReview->setToolTip(QStringLiteral("Fast charts from existing scalar fields. Drag a range on each plot; values outside any enabled range become To validate. Full-point application, no ASPRS overwrite. Create missing geometric fields in Features first."));
		registerCommandButton(rangeReview);layout->addWidget(rangeReview);
		connect(rangeReview,&QPushButton::clicked,this,&WorkspaceDock::featureRangeReviewRequested);
		auto* vegetationGroup = named(new QGroupBox(QStringLiteral("After Ground — vegetation / retained points")), "ALiS.Workspace.Terrain.Vegetation");
		auto* vegetationLayout = new QVBoxLayout(vegetationGroup);
		auto* vegetationNote = new QLabel(QStringLiteral("<b>1. Apply Ground → 2. Preview → 3. Review / separate</b><br>DTM/HAG is optional for binary separation and needed for vegetation height bands.<br>"
			"Conservative geometric proposals, not certified classification. Ground stays unchanged. Uncertain points are retained, including possible structures below vegetation."));
		vegetationNote->setWordWrap(true); vegetationLayout->addWidget(vegetationNote);
		auto* vegetationMode=named(new QComboBox,"ALiS.Workspace.Terrain.Vegetation.Mode");
		vegetationMode->addItems({QStringLiteral("Adaptive geometry — exploratory"),QStringLiteral("Verified local model — experimental")});
		vegetationLayout->addWidget(vegetationMode);
		auto* geometryControls=new QWidget;auto* geometryLayout=new QVBoxLayout(geometryControls);geometryLayout->setContentsMargins(0,0,0,0);
		auto* vegetationForm = new QFormLayout;
		const auto infoCaption=helpLabel;
		auto* vegetationPreset = named(new QComboBox, "ALiS.Workspace.Terrain.Vegetation.Preset");
		vegetationPreset->addItems({QStringLiteral("Preserve structures (recommended)"), QStringLiteral("Balanced exploration"), QStringLiteral("Vegetation recall — review carefully")});
		auto* vegetationRadius = named(new PrecisionSpinBox, "ALiS.Workspace.Terrain.Vegetation.MaxRadius");
		vegetationRadius->setRange(.2,3); vegetationRadius->setDecimals(2); vegetationRadius->setValue(1);
		auto* vegetationNeighbors = named(new QSpinBox, "ALiS.Workspace.Terrain.Vegetation.MinimumNeighbors");
		vegetationNeighbors->setRange(8,64); vegetationNeighbors->setValue(12);
		auto* vegetationLow = named(new PrecisionSpinBox, "ALiS.Workspace.Terrain.Vegetation.LowHeight");
		vegetationLow->setRange(.05,20); vegetationLow->setDecimals(2); vegetationLow->setValue(.5);
		auto* vegetationMedium = named(new PrecisionSpinBox, "ALiS.Workspace.Terrain.Vegetation.MediumHeight");
		vegetationMedium->setRange(.1,100); vegetationMedium->setDecimals(2); vegetationMedium->setValue(2);
		vegetationForm->addRow(helpLabel(QStringLiteral("Adaptive preset"), QStringLiteral("All presets measure cloud spacing and derive three spherical radii, then use robust quantiles of non-ground geometric features within guarded ranges. These are ALiS starting heuristics inspired by multiscale literature, NOT published universal thresholds. Preserve structures raises the vegetation evidence required. No preset can recover surfaces the laser did not sample.")),vegetationPreset);
		vegetationForm->addRow(helpLabel(QStringLiteral("Maximum radius (m)"), QStringLiteral("Upper bound, not a forced radius. Density suggests the first radius for about 16 neighbours under a planar-support approximation; the other radii are 2x and 4x. Actual neighbour counts are checked at every point. Small-scale planar evidence protects surfaces even when larger spheres include a canopy. Values are radii, whereas 3DMASC parameter scales are diameters.")),vegetationRadius);
		vegetationForm->addRow(helpLabel(QStringLiteral("Minimum neighbours / scale"),QStringLiteral("A point needs at least two supported scales. Sparse, occluded and ambiguous points remain Uncertain; reducing this value increases unstable decisions. The original cloud is never subsampled.")),vegetationNeighbors);
		vegetationForm->addRow(helpLabel(QStringLiteral("Low vegetation upper HAG (m)"),QStringLiteral("Only points already proposed as vegetation are divided into ASPRS 3/4/5 by HAG. Default height breaks 0.5 and 2 m are editable project conventions, not mandatory ASPRS limits. Height alone never makes a point vegetation.")),vegetationLow);
		vegetationForm->addRow(QStringLiteral("Medium vegetation upper HAG (m)"),vegetationMedium);
		auto* vegetationTarget=named(new QSpinBox,"ALiS.Workspace.Terrain.Vegetation.TargetNeighbors");vegetationTarget->setRange(8,128);vegetationTarget->setValue(16);
		auto* vegetationFine=named(new PrecisionSpinBox,"ALiS.Workspace.Terrain.Vegetation.FineLimit");vegetationFine->setRange(.05,.5);vegetationFine->setDecimals(3);vegetationFine->setValue(.25);
		vegetationForm->addRow(infoCaption(QStringLiteral("Target support for initial scale"),QStringLiteral("Initial estimate r=sqrt(k/(pi*density)). This assumes locally homogeneous planar sampling. It is NOT guaranteed support on vertical walls or in canopy. Actual neighbours are checked per point. Larger k suggests larger radii only within the physical limits below.")),vegetationTarget);
		vegetationForm->addRow(infoCaption(QStringLiteral("Fine-scale radius limit (m)"),QStringLiteral("Physical protection against mixing a small wall with surrounding leaves. Effective radii: r, 2r, 4r; r is limited by this value and maximum radius/4. If support remains insufficient, points stay uncertain: the radius is not expanded without bounds. These settings apply ONLY to adaptive geometry, never to a trained model.")),vegetationFine);
		geometryLayout->addLayout(vegetationForm);vegetationLayout->addWidget(geometryControls);
		auto* localControls=new QWidget;auto* localLayout=new QVBoxLayout(localControls);localLayout->setContentsMargins(0,0,0,0);
		auto* localModel=named(new QLineEdit,"ALiS.Workspace.Terrain.Vegetation.ModelPath");
		const QString studyModel=QDir(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)).filePath(QStringLiteral("qArchaeoLiDAR_Classifier_Tests_PzArmerina/Vegetation_geometry_study_20260906/experimental_model.joblib"));
		localModel->setText(QSettings().value("ALiS/vegetation/localModel",QFileInfo::exists(studyModel)?studyModel:QString()).toString());
		auto* localBrowse=commandButton(QStringLiteral("Select local model…"),"ALiS.Workspace.Terrain.Vegetation.SelectModel");
		auto* modelRow=new QHBoxLayout;modelRow->addWidget(localModel);modelRow->addWidget(localBrowse);localLayout->addLayout(modelRow);
		auto* modelInfo=named(new QLabel,"ALiS.Workspace.Terrain.Vegetation.ModelInfo");modelInfo->setWordWrap(true);localLayout->addWidget(modelInfo);
		auto* trustModel=named(new QCheckBox(QStringLiteral("I trust this local model and its source (joblib can execute code)")),"ALiS.Workspace.Terrain.Vegetation.TrustModel");localLayout->addWidget(trustModel);
		auto* scoreThreshold=named(new PrecisionSpinBox,"ALiS.Workspace.Terrain.Vegetation.ScoreThreshold");scoreThreshold->setDecimals(12);scoreThreshold->setRange(0,1.000001);scoreThreshold->setValue(.986473083496);
		auto* scoreForm=new QFormLayout;scoreForm->addRow(infoCaption(QStringLiteral("Vegetation score threshold"),QStringLiteral("Downstream score threshold, not a calibrated probability. Raising it usually retains more vegetation but reduces structural false positives. Lowering it can remove walls/ruins. Filter existing scores without rerunning inference. A threshold above 1 retains everything. Ground is always protected; retained does NOT mean Building.")),scoreThreshold);localLayout->addLayout(scoreForm);
		auto* modelRun=commandButton(QStringLiteral("Compute compatible features and run model"),"ALiS.Workspace.Terrain.Vegetation.RunModel");
		auto* scoreApply=commandButton(QStringLiteral("Apply threshold to existing scores — no new prediction"),"ALiS.Workspace.Terrain.Vegetation.FilterScore");
		registerCommandButton(modelRun);registerCommandButton(scoreApply);localLayout->addWidget(modelRun);localLayout->addWidget(scoreApply);
		localControls->hide();vegetationLayout->addWidget(localControls);
		auto refreshModel=[localModel,modelInfo,scoreThreshold,trustModel](){
			trustModel->setChecked(false);QFile file(localModel->text()+QStringLiteral(".json"));QJsonObject meta;
			if(file.open(QIODevice::ReadOnly))meta=QJsonDocument::fromJson(file.readAll()).object();
			if(meta.value("schema").toString()!=QStringLiteral("alis-vegetation-local-model/1")){modelInfo->setText(QStringLiteral("Choose a local model with its .joblib.json manifest. No model is downloaded automatically."));return;}
			QStringList scales;for(const auto& r:meta.value("radii_m").toArray())scales<<QString::number(r.toDouble(),'g',6);
			const auto validation=meta.value("validation").toObject();const auto test=validation.value("test").toObject();
			scoreThreshold->setValue(meta.value("threshold").toDouble(.986473));
			modelInfo->setText(QStringLiteral("%1\nFixed training radii: %2 m. Minimum support: 12 neighbours.\nRecorded test: precision %3%, recall %4%, Building error %5%; worst block %6%. These are NOT accuracy estimates for the selected cloud.\nExperimental: only one surveyed site. Fine/coarse adaptive controls are intentionally disabled for this model.")
				.arg(meta.value("name").toString(),scales.join(" / ")).arg(100*test.value("vegetation_precision").toDouble(),0,'f',2).arg(100*test.value("vegetation_recall").toDouble(),0,'f',2).arg(100*test.value("building_false_vegetation_rate").toDouble(),0,'f',3).arg(100*validation.value("worst_block_building_error").toDouble(),0,'f',2));
		};
		connect(localModel,&QLineEdit::textChanged,this,[refreshModel](){refreshModel();});refreshModel();
		connect(localBrowse,&QPushButton::clicked,this,[this,localModel](){auto p=QFileDialog::getOpenFileName(this,QStringLiteral("Trusted local vegetation model"),localModel->text(),QStringLiteral("ALiS vegetation model (*.joblib)"));if(!p.isEmpty())localModel->setText(p);});
		auto* vegetationOutput = named(new QLineEdit, "ALiS.Workspace.Terrain.Vegetation.Output");
		vegetationOutput->setText(QSettings().value(QStringLiteral("ALiS/vegetation/output"), QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(QStringLiteral("ALiS/vegetation-tests"))).toString());
		auto* vegetationBrowse = commandButton(QStringLiteral("Report folder…"),"ALiS.Workspace.Terrain.Vegetation.Browse");
		auto* vegetationOpen = commandButton(QStringLiteral("Open reports"),"ALiS.Workspace.Terrain.Vegetation.OpenReports");
		auto* vegetationPaths = new QHBoxLayout; vegetationPaths->addWidget(vegetationOutput);vegetationPaths->addWidget(vegetationBrowse);vegetationPaths->addWidget(vegetationOpen);vegetationLayout->addLayout(vegetationPaths);
		connect(vegetationBrowse,&QPushButton::clicked,this,[this,vegetationOutput](){auto path=QFileDialog::getExistingDirectory(this,QStringLiteral("Vegetation reports"),vegetationOutput->text());if(!path.isEmpty())vegetationOutput->setText(path);});
		connect(vegetationOpen,&QPushButton::clicked,this,[vegetationOutput](){QDesktopServices::openUrl(QUrl::fromLocalFile(vegetationOutput->text()));});
		auto* vegetationRun=commandButton(QStringLiteral("Adapt to this cloud and preview"),"ALiS.Workspace.Terrain.Vegetation.Run");
		auto* vegetationExtract=commandButton(QStringLiteral("Create vegetation / retained clouds"),"ALiS.Workspace.Terrain.Vegetation.Extract");
		registerCommandButton(vegetationRun); registerCommandButton(vegetationExtract);
		vegetationLayout->addWidget(vegetationRun);vegetationLayout->addWidget(vegetationExtract);
		connect(vegetationMode,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[geometryControls,localControls,vegetationRun](int i){geometryControls->setVisible(i==0);localControls->setVisible(i==1);vegetationRun->setVisible(i==0);});
		connect(modelRun,&QPushButton::clicked,this,[this,localModel,vegetationOutput,trustModel](){
			QSettings().setValue("ALiS/vegetation/localModel",localModel->text());QSettings().setValue("ALiS/vegetation/output",vegetationOutput->text());
			Q_EMIT vegetationModelRequested(localModel->text(),vegetationOutput->text(),m_pythonExecutableEdit->text(),m_workerScriptEdit->text(),trustModel->isChecked());
		});
		connect(scoreApply,&QPushButton::clicked,this,[this,scoreThreshold](){Q_EMIT vegetationThresholdRequested(scoreThreshold->value());});
		connect(vegetationRun,&QPushButton::clicked,this,[this,vegetationPreset,vegetationRadius,vegetationNeighbors,vegetationLow,vegetationMedium,vegetationOutput,vegetationTarget,vegetationFine](){
			QSettings().setValue(QStringLiteral("ALiS/vegetation/output"),vegetationOutput->text());
			Q_EMIT vegetationPreviewRequested(vegetationPreset->currentIndex(),vegetationRadius->value(),vegetationNeighbors->value(),vegetationLow->value(),vegetationMedium->value(),vegetationOutput->text(),vegetationTarget->value(),vegetationFine->value());
		});
		connect(vegetationExtract,&QPushButton::clicked,this,&WorkspaceDock::vegetationExtractRequested);
		auto* refine=commandButton(QStringLiteral("Refine vegetation using saved feature ranges"),"ALiS.Workspace.Vegetation.RefineFeatures");
		explain(refine,QStringLiteral("Use your computed features"),QStringLiteral("Create features at any desired scales in Features. Open Feature distributions / range review, choose the existing scalar fields and apply ranges describing vegetation. This button checks those ranges on ALL points and moves vegetation proposals outside them to uncertain / To validate. Ground, ASPRS and model scores stay unchanged. It works with either adaptive geometry or the local model; no new feature computation or training. It can only retain additional points, not add vegetation. To broaden a previously refined proposal, rerun the preview or reapply the model score threshold first."));
		registerCommandButton(refine);vegetationLayout->insertWidget(vegetationLayout->count()-1,refine);
		connect(refine,&QPushButton::clicked,this,[this,vegetationOutput](){Q_EMIT vegetationFeatureRefinementRequested(vegetationOutput->text());});
		auto* vegetationLegend=new QLabel(QStringLiteral("Preview codes: 0 uncertain · 1 vegetation · 2 other surface · 3 protected ground · 4 excluded (noise/water). Other surface is NOT automatically a building. Extraction keeps 0/2/3/4 together; save the resulting clouds with CloudCompare Save."));
		vegetationLegend->setWordWrap(true);vegetationLayout->addWidget(vegetationLegend);layout->addWidget(vegetationGroup);
		layout->addStretch(1);
	}

	void WorkspaceDock::buildFeaturesTab()
	{
		QVBoxLayout* layout = addScrollableTab(
			m_prepareTabs,
			QStringLiteral("Features"),
			"ALiS.Workspace.Tab.Features",
			"ALiS.Workspace.Tab.Features.Content");
		QGroupBox* scalesGroup = named(new QGroupBox(QStringLiteral("Scale Manager")),
		                                    "ALiS.Workspace.Features.ScaleManager");
		QVBoxLayout* scalesLayout = new QVBoxLayout(scalesGroup);
		m_scaleList = named(new QListWidget, "ALiS.Workspace.Features.ScaleList");
		m_scaleList->setSelectionMode(QAbstractItemView::ExtendedSelection);
		m_scaleList->setMaximumHeight(145);
		scalesLayout->addWidget(m_scaleList);
		QHBoxLayout* scaleEdit = new QHBoxLayout;
		m_newScaleSpin = named(new PrecisionSpinBox, "ALiS.Workspace.Features.NewScale");
		configureLengthSpin(m_newScaleSpin, 0.50);
		m_addScaleButton = commandButton(QStringLiteral("Add"), "ALiS.Workspace.Features.AddScale");
		m_removeScaleButton = commandButton(QStringLiteral("Remove"), "ALiS.Workspace.Features.RemoveScale");
		scaleEdit->addWidget(m_newScaleSpin, 1);
		scaleEdit->addWidget(m_addScaleButton);
		scaleEdit->addWidget(m_removeScaleButton);
		scalesLayout->addLayout(scaleEdit);
		QHBoxLayout* scaleCommands = new QHBoxLayout;
		m_suggestScalesButton = commandButton(QStringLiteral("Suggest Scales"), "ALiS.Workspace.Features.SuggestScales");
		m_saveScalePresetButton = commandButton(QStringLiteral("Save multiscale preset…"), "ALiS.Workspace.Features.SaveScalePreset");
		explain(m_suggestScalesButton,QStringLiteral("Adaptive scales"),QStringLiteral("Same 16/64/256-neighbour policy, radius bounds and reliable-return option as Terrain. This button changes only feature radii. All values remain editable."));
		scaleCommands->addWidget(m_suggestScalesButton);
		scaleCommands->addWidget(m_saveScalePresetButton);
		scalesLayout->addLayout(scaleCommands);
		m_scaleExplanationValue = valueLabel("ALiS.Workspace.Features.ScaleExplanation",
		                                          QStringLiteral("Scale suggestions require confirmed metric units and point spacing."));
		scalesLayout->addWidget(m_scaleExplanationValue);
		layout->addWidget(scalesGroup);

		connect(m_addScaleButton, &QPushButton::clicked, this, [this]()
		{
			QVector<double> radii = scaleRadii();
			radii.push_back(m_newScaleSpin->value());
			setScaleRadii(radii);
			Q_EMIT scaleRadiiChanged(scaleRadii());
		});
		connect(m_removeScaleButton, &QPushButton::clicked, this, [this]()
		{
			const QList<QListWidgetItem*> selected = m_scaleList->selectedItems();
			for (QListWidgetItem* item : selected)
			{
				delete m_scaleList->takeItem(m_scaleList->row(item));
			}
			updateScaleEditors();
			Q_EMIT scaleRadiiChanged(scaleRadii());
		});
		connect(m_suggestScalesButton, &QPushButton::clicked, this, &WorkspaceDock::suggestScalesRequested);
		connect(m_saveScalePresetButton, &QPushButton::clicked, this, &WorkspaceDock::saveProcessingPresetRequested);
		connect(m_scaleList, &QListWidget::itemChanged, this, [this](QListWidgetItem* item)
		{
			if (m_updatingScales || !item)
			{
				return;
			}
			bool ok = false;
			const double value = item->text().toDouble(&ok);
			if (!ok || !std::isfinite(value) || value <= 0.0)
			{
				const QSignalBlocker blocker(m_scaleList);
				item->setText(QString::number(item->data(Qt::UserRole).toDouble(), 'g', 8));
				return;
			}
			item->setData(Qt::UserRole, value);
			setScaleRadii(scaleRadii());
			Q_EMIT scaleRadiiChanged(scaleRadii());
		});
		registerCommandButton(m_suggestScalesButton);

		QGroupBox* selectionGroup = named(new QGroupBox(QStringLiteral("Requested geometric features")),
		                                       "ALiS.Workspace.Features.Selection");
		QVBoxLayout* selectionLayout = new QVBoxLayout(selectionGroup);
		m_featureList = named(new QListWidget, "ALiS.Workspace.Features.FeatureList");
		const QList<QPair<QString, QString>> features = {
			{QStringLiteral("neighbor_count"), QStringLiteral("Neighbour count")},
			{QStringLiteral("density_2d"), QStringLiteral("Point density (2D)")},
			{QStringLiteral("density_3d"), QStringLiteral("Point density (3D)")},
			{QStringLiteral("eigenvalue_1"), QStringLiteral("Eigenvalue 1")},
			{QStringLiteral("eigenvalue_2"), QStringLiteral("Eigenvalue 2")},
			{QStringLiteral("eigenvalue_3"), QStringLiteral("Eigenvalue 3")},
			{QStringLiteral("eigenvalues_sum"), QStringLiteral("Sum of eigenvalues")},
			{QStringLiteral("pca_1"), QStringLiteral("PCA 1")},
			{QStringLiteral("pca_2"), QStringLiteral("PCA 2")},
			{QStringLiteral("roughness"), QStringLiteral("Roughness")},
			{QStringLiteral("signed_roughness"), QStringLiteral("Signed roughness")},
			{QStringLiteral("mean_curvature"), QStringLiteral("Mean curvature")},
			{QStringLiteral("gaussian_curvature"), QStringLiteral("Gaussian curvature")},
			{QStringLiteral("normal_x"), QStringLiteral("Normal X")},
			{QStringLiteral("normal_y"), QStringLiteral("Normal Y")},
			{QStringLiteral("normal_z"), QStringLiteral("Normal Z / orientation")},
			{QStringLiteral("normal_z_absolute"), QStringLiteral("Absolute Normal Z")},
			{QStringLiteral("dip"), QStringLiteral("Dip")},
			{QStringLiteral("dip_direction"), QStringLiteral("Dip direction")},
			{QStringLiteral("verticality"), QStringLiteral("Verticality")},
			{QStringLiteral("planarity"), QStringLiteral("Planarity")},
			{QStringLiteral("linearity"), QStringLiteral("Linearity")},
			{QStringLiteral("sphericity"), QStringLiteral("Sphericity")},
			{QStringLiteral("anisotropy"), QStringLiteral("Anisotropy")},
			{QStringLiteral("omnivariance"), QStringLiteral("Omnivariance")},
			{QStringLiteral("eigenentropy"), QStringLiteral("Eigenentropy")},
			{QStringLiteral("surface_variation"), QStringLiteral("Surface variation")},
			{QStringLiteral("normal_change_rate"), QStringLiteral("Normal change rate")},
			{QStringLiteral("moment_order_1"), QStringLiteral("First-order moment")},
			{QStringLiteral("barycenter_offset_ratio"), QStringLiteral("Barycenter offset ratio (q3DMASC ANISO)")},
			{QStringLiteral("hag"), QStringLiteral("Height Above Ground")},
			{QStringLiteral("z_minimum"), QStringLiteral("Local Z minimum")},
			{QStringLiteral("z_maximum"), QStringLiteral("Local Z maximum")},
			{QStringLiteral("z_range"), QStringLiteral("Local Z range / relief")},
			{QStringLiteral("z_mean"), QStringLiteral("Local Z mean")},
			{QStringLiteral("z_standard_deviation"), QStringLiteral("Local Z standard deviation")},
			{QStringLiteral("z_above_minimum"), QStringLiteral("Z above local minimum")},
			{QStringLiteral("z_below_maximum"), QStringLiteral("Z below local maximum")},
			{QStringLiteral("z_relative_to_mean"), QStringLiteral("Z relative to local mean")},
			{QStringLiteral("z_percentile_10"), QStringLiteral("Local Z percentile 10")},
			{QStringLiteral("z_percentile_25"), QStringLiteral("Local Z percentile 25")},
			{QStringLiteral("z_median"), QStringLiteral("Local Z median")},
			{QStringLiteral("z_percentile_75"), QStringLiteral("Local Z percentile 75")},
			{QStringLiteral("z_percentile_90"), QStringLiteral("Local Z percentile 90")}
		};
		for (const auto& feature : features)
		{
			QListWidgetItem* item = new QListWidgetItem(feature.second, m_featureList);
			item->setData(Qt::UserRole, feature.first);
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
			const bool initial = feature.first == QStringLiteral("roughness")
			                  || feature.first == QStringLiteral("verticality")
			                  || feature.first == QStringLiteral("planarity");
			item->setCheckState(initial ? Qt::Checked : Qt::Unchecked);
		}
		m_featureList->setMinimumHeight(300);
		QHBoxLayout* selectCommands = new QHBoxLayout;
		for (bool select : {true, false})
		{
			QPushButton* button = commandButton(select ? QStringLiteral("Select all") : QStringLiteral("Deselect all"), select ? "ALiS.Features.SelectAll" : "ALiS.Features.SelectNone");
			selectCommands->addWidget(button);
			connect(button, &QPushButton::clicked, this, [this, select]()
			{
				{ const QSignalBlocker blocker(m_featureList); for (int i = 0; i < m_featureList->count(); ++i) m_featureList->item(i)->setCheckState(select ? Qt::Checked : Qt::Unchecked); }
				Q_EMIT featureSelectionChanged(selectedFeatureIds());
				updateCommandAvailability();
			});
			registerCommandButton(button);
		}
		selectionLayout->addLayout(selectCommands);
		selectionLayout->addWidget(m_featureList);
		m_computeFeaturesButton = commandButton(QStringLiteral("Compute Selected Features"),
		                                        "ALiS.Workspace.Features.ComputeSelected");
		selectionLayout->addWidget(m_computeFeaturesButton);
		layout->addWidget(selectionGroup);
		connect(m_featureList, &QListWidget::itemChanged, this, [this](QListWidgetItem*)
		{
			Q_EMIT featureSelectionChanged(selectedFeatureIds());
			updateCommandAvailability();
		});
		connect(m_computeFeaturesButton, &QPushButton::clicked, this, [this]()
		{
			Q_EMIT computeFeaturesRequested(selectedFeatureIds(), scaleRadii());
		});
		registerCommandButton(m_computeFeaturesButton);

		QLabel* publishedNote = new QLabel(QStringLiteral("Each feature/radius is automatically published as a Scalar Field. Use Display above for field, palette and range. CloudCompare BIN preserves these fields."));
		publishedNote->setWordWrap(true); layout->addWidget(publishedNote);
		layout->addStretch(1);
	}

	void WorkspaceDock::buildModelsTab()
	{
		QWidget* page = named(new QWidget, "ALiS.Workspace.Area.Models");
		auto* pageLayout = new QVBoxLayout(page);
		pageLayout->setContentsMargins(8, 8, 8, 8);
		pageLayout->setSpacing(8);
		auto* intro = new QLabel(QStringLiteral(
			"<b>Classification lab</b><br><span style='color:#64748b'>Create reviewable starter labels, or use the established supervised workflow to train, validate and reuse models.</span>"));
		intro->setWordWrap(true);
		pageLayout->addWidget(intro);
		m_modelsTabs = named(new QTabWidget, "ALiS.Workspace.Models.Tabs");
		m_modelsTabs->setDocumentMode(true);
		pageLayout->addWidget(m_modelsTabs, 1);
		m_tabs->addTab(page, QIcon(QStringLiteral(":/CC/plugin/ALiS/images/models.svg")), QStringLiteral("Classification"));

		QVBoxLayout* bootstrapLayout = addScrollableTab(
			m_modelsTabs, QStringLiteral("Unsupervised clustering"),
			"ALiS.Workspace.Models.Bootstrap", "ALiS.Workspace.Models.Bootstrap.Content");
		auto* bootstrapIntro = new QLabel(QStringLiteral(
			"<b>Start without certified labels</b><br>Explore groups using existing scalar fields, RGB and optional geometric features, then review them in Annotation Studio. "
			"Review cluster IDs, assign meaningful classes in Annotation Studio, then reuse validated labels for training."));
		bootstrapIntro->setWordWrap(true);
		bootstrapIntro->setStyleSheet(QStringLiteral("QLabel { color:#172554; background:#eff6ff; border:1px solid #bfdbfe; padding:9px; border-radius:5px; }"));
		bootstrapLayout->addWidget(bootstrapIntro);
		auto* clusteringLayout = bootstrapLayout;

		auto* bootstrapSetup = named(new QGroupBox(QStringLiteral("A. Unsupervised clustering — method and effort")), "ALiS.Workspace.Models.Bootstrap.Setup");
		auto* bootstrapForm = formLayout(bootstrapSetup);
		m_bootstrapEngineCombo = named(new QComboBox, "ALiS.Workspace.Models.Bootstrap.Engine");
		m_bootstrapEngineCombo->addItem(QStringLiteral("Explore — MiniBatch K-Means (fast)"), QStringLiteral("minibatch_kmeans"));
		m_bootstrapEngineCombo->addItem(QStringLiteral("Explore — BIRCH (balanced)"), QStringLiteral("birch"));
		m_bootstrapEngineCombo->addItem(QStringLiteral("Explore — Gaussian Mixture (thorough)"), QStringLiteral("gaussian_mixture"));
		m_bootstrapQualityCombo = named(new QComboBox, "ALiS.Workspace.Models.Bootstrap.Quality");
		m_bootstrapQualityCombo->addItem(QStringLiteral("Fast preview"), 0);
		m_bootstrapQualityCombo->addItem(QStringLiteral("Balanced"), 1);
		m_bootstrapQualityCombo->addItem(QStringLiteral("Thorough"), 2);
		m_bootstrapQualityCombo->setCurrentIndex(1);
		m_bootstrapClusterSpin = named(new QSpinBox, "ALiS.Workspace.Models.Bootstrap.ClusterCount");
		m_bootstrapClusterSpin->setRange(2, 64); m_bootstrapClusterSpin->setValue(8);
		m_bootstrapFitSampleSpin = named(new QSpinBox, "ALiS.Workspace.Models.Bootstrap.FitSample");
		m_bootstrapFitSampleSpin->setRange(10000, 5000000); m_bootstrapFitSampleSpin->setSingleStep(100000); m_bootstrapFitSampleSpin->setValue(500000);
		m_bootstrapChunkSpin = named(new QSpinBox, "ALiS.Workspace.Models.Bootstrap.ChunkSize");
		m_bootstrapChunkSpin->setRange(1000, 2000000); m_bootstrapChunkSpin->setSingleStep(50000); m_bootstrapChunkSpin->setValue(250000);
		bootstrapForm->addRow(helpLabel(QStringLiteral("Method"), QStringLiteral("Group points by similarity in the checked scalar fields and RGB channels. Each channel is standardized so its numeric range does not dominate. Cluster IDs acquire a semantic meaning after your review and class assignment.")), m_bootstrapEngineCombo);
		bootstrapForm->addRow(helpLabel(QStringLiteral("Effort"), QStringLiteral("Adjusts fitting sample and streaming chunk size. It does not change the scientific meaning of cluster IDs.")), m_bootstrapQualityCombo);
		bootstrapForm->addRow(QStringLiteral("Requested groups"), m_bootstrapClusterSpin);
		bootstrapForm->addRow(QStringLiteral("Maximum fitting sample"), m_bootstrapFitSampleSpin);
		bootstrapForm->addRow(QStringLiteral("Streaming chunk"), m_bootstrapChunkSpin);
		clusteringLayout->addWidget(bootstrapSetup);

		auto* bootstrapFeatures = named(new QGroupBox(QStringLiteral("B. Choose input fields — existing SF, RGB and features")), "ALiS.Workspace.Models.Bootstrap.Features");
		auto* bootstrapFeaturesLayout = new QVBoxLayout(bootstrapFeatures);
		m_bootstrapFeatureList = named(new QListWidget, "ALiS.Workspace.Models.Bootstrap.FeatureList");
		m_bootstrapFeatureList->setMinimumHeight(180);
		bootstrapFeaturesLayout->addWidget(m_bootstrapFeatureList);
		auto* fieldNote = new QLabel(QStringLiteral("Select existing fields below. RGB and scalar values are read directly; geometric features are optional. For vegetation versus structures, try HAG, roughness/planarity and return information when available."));
		fieldNote->setWordWrap(true);
		bootstrapFeaturesLayout->addWidget(fieldNote);
		auto* fieldButtons = new QHBoxLayout;
		auto* selectSafe = commandButton(QStringLiteral("Select measurements"), "ALiS.Workspace.Models.Bootstrap.SelectMeasurements");
		auto* selectRgb = commandButton(QStringLiteral("RGB only"), "ALiS.Workspace.Models.Bootstrap.SelectRGB");
		auto* clearFields = commandButton(QStringLiteral("Deselect all"), "ALiS.Workspace.Models.Bootstrap.DeselectAll");
		auto* refreshFields = commandButton(QStringLiteral("Refresh fields"), "ALiS.Workspace.Models.Bootstrap.RefreshFields");
		fieldButtons->addWidget(selectSafe); fieldButtons->addWidget(selectRgb);
		fieldButtons->addWidget(clearFields); fieldButtons->addWidget(refreshFields);
		bootstrapFeaturesLayout->addLayout(fieldButtons);
		connect(selectSafe, &QPushButton::clicked, this, [this]()
		{
			const QSignalBlocker blocker(m_bootstrapFeatureList);
			for (int i = 0; i < m_bootstrapFeatureList->count(); ++i)
			{
				auto* item = m_bootstrapFeatureList->item(i);
				item->setCheckState(item->data(Qt::UserRole + 1).toBool() ? Qt::Unchecked : Qt::Checked);
			}
			updateCommandAvailability();
		});
		connect(selectRgb, &QPushButton::clicked, this, [this]()
		{
			const QSignalBlocker blocker(m_bootstrapFeatureList);
			for (int i = 0; i < m_bootstrapFeatureList->count(); ++i)
			{
				auto* item = m_bootstrapFeatureList->item(i);
				item->setCheckState(item->data(Qt::UserRole).toString().startsWith(QStringLiteral("rgb:")) ? Qt::Checked : Qt::Unchecked);
			}
			updateCommandAvailability();
		});
		connect(clearFields, &QPushButton::clicked, this, [this]()
		{
			const QSignalBlocker blocker(m_bootstrapFeatureList);
			for (int i = 0; i < m_bootstrapFeatureList->count(); ++i) m_bootstrapFeatureList->item(i)->setCheckState(Qt::Unchecked);
			updateCommandAvailability();
		});
		connect(refreshFields, &QPushButton::clicked, this, [this]() { Q_EMIT displayRefreshRequested(); });

		auto* prepareFeatures = commandButton(QStringLiteral("Optional: calculate geometric features…"), "ALiS.Workspace.Models.Bootstrap.OpenFeatures");
		connect(prepareFeatures, &QPushButton::clicked, this, &WorkspaceDock::showPrepareFeatures);
		bootstrapFeaturesLayout->addWidget(prepareFeatures);
		clusteringLayout->addWidget(bootstrapFeatures);

		auto* bootstrapActions = named(new QGroupBox(QStringLiteral("C. Run and review clusters")), "ALiS.Workspace.Models.Bootstrap.Actions");
		auto* bootstrapActionsLayout = new QGridLayout(bootstrapActions);
		auto* checkBackend = commandButton(QStringLiteral("Check engines and GPU"), "ALiS.Workspace.Models.Bootstrap.Check");
		m_bootstrapRunButton = commandButton(QStringLiteral("Create Derived cluster labels"), "ALiS.Workspace.Models.Bootstrap.Run");
		auto* reviewBootstrap = commandButton(QStringLiteral("Review in Annotation Studio"), "ALiS.Workspace.Models.Bootstrap.Review");
		m_bootstrapStatusValue = valueLabel("ALiS.Workspace.Models.Bootstrap.Status", QStringLiteral("Ready to inspect available engines."));
		bootstrapActionsLayout->addWidget(checkBackend, 0, 0);
		bootstrapActionsLayout->addWidget(m_bootstrapRunButton, 0, 1);
		bootstrapActionsLayout->addWidget(reviewBootstrap, 1, 0, 1, 2);
		bootstrapActionsLayout->addWidget(m_bootstrapStatusValue, 2, 0, 1, 2);
		clusteringLayout->addWidget(bootstrapActions);
		clusteringLayout->addStretch(1);

		connect(checkBackend, &QPushButton::clicked, this, [this]()
		{
			Q_EMIT bootstrapCapabilitiesRequested(m_pythonExecutableEdit ? m_pythonExecutableEdit->text().trimmed() : QString(),
				m_workerScriptEdit ? m_workerScriptEdit->text().trimmed() : QString());
		});
		connect(m_bootstrapRunButton, &QPushButton::clicked, this, [this]()
		{
			Q_EMIT bootstrapLabelsRequested(selectedBootstrapFeatureKeys(), m_pythonExecutableEdit->text().trimmed(),
				m_workerScriptEdit->text().trimmed(), m_modelRepositoryEdit->text().trimmed(),
				m_bootstrapEngineCombo->currentData().toString(), m_bootstrapClusterSpin->value(),
				m_bootstrapFitSampleSpin->value(), m_bootstrapChunkSpin->value(), 42);
		});
		connect(reviewBootstrap, &QPushButton::clicked, this, [this]()
		{
			showAnnotationWorkspace();
			Q_EMIT annotationActionRequested(QStringLiteral("studio"));
		});
		connect(m_bootstrapFeatureList, &QListWidget::itemChanged, this, [this](QListWidgetItem*) { updateCommandAvailability(); });
		connect(m_bootstrapEngineCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { updateCommandAvailability(); });
		connect(m_bootstrapQualityCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int quality)
		{
			const int samples[] = {100000, 500000, 1500000};
			const int chunks[] = {500000, 250000, 100000};
			m_bootstrapFitSampleSpin->setValue(samples[quality]);
			m_bootstrapChunkSpin->setValue(chunks[quality]);
		});
		registerCommandButton(prepareFeatures);
		registerCommandButton(m_bootstrapRunButton);
		registerCommandButton(reviewBootstrap);
		refreshPretrainedCatalog();

		QVBoxLayout* layout = addScrollableTab(
			m_modelsTabs, QStringLiteral("Supervised workflow"),
			"ALiS.Workspace.Models.Supervised", "ALiS.Workspace.Models.Supervised.Content");
		QGroupBox* flow=named(new QGroupBox(QStringLiteral("Model tutorial — choose a workflow")),"ALiS.Workspace.Models.Flow");
		auto* flowLayout=new QGridLayout(flow);
		const QStringList stages={QStringLiteral("1  Features"),QStringLiteral("2  Annotate"),QStringLiteral("3  Train / validate"),QStringLiteral("4  Predict / review")};
		const QStringList tips={QStringLiteral("Compute the same feature names, radii and HAG definition for every cloud. Published SFs are selectable in Display and survive BIN save/reopen."),QStringLiteral("Create labels for at least two ASPRS classes in several spatially separated areas. Apply the verified labels in Annotate. Imported or automatic classes are not automatically Trusted."),QStringLiteral("Export the selected schema, choose classifier/device, then Train. Spatial blocks separate train and test. Read per-class support, precision/recall/F1 and confusion matrix; a class absent from test is NOT validated."),QStringLiteral("For reuse: load a trusted model, prepare matching features, then Predict. Inspect Derived classes and confidence. Apply is a separate, undoable promotion to Working; it never confirms new training labels.")};
		for(int i=0;i<4;++i)
		{
			auto* button=commandButton(stages[i],qPrintable(QStringLiteral("ALiS.Workspace.Models.Flow.%1").arg(i)));
			button->setIcon(style()->standardIcon(i==0?QStyle::SP_FileDialogDetailedView:i==1?QStyle::SP_FileDialogContentsView:i==2?QStyle::SP_ComputerIcon:QStyle::SP_DialogApplyButton));
			explain(button,stages[i],tips[i]);flowLayout->addWidget(button,i/2,i%2);
			connect(button,&QPushButton::clicked,this,[this,i]()
			{
				if(i==0) showPrepareFeatures();
				else if(i==1) showAnnotationWorkspace();
				else if(i==2 && !m_modelCardChecks.isEmpty()) m_modelCardChecks.front()->setFocus();
				else m_modelPathEdit->setFocus();
			});
		}
		flowLayout->addWidget(helpLabel(QStringLiteral("New model: 1 → 2 → 3 → 4"),QStringLiteral("Features → manually trusted labels → spatial validation → predictions → visual review → optional Apply. No step deletes source points.")),2,0,1,2);
		flowLayout->addWidget(helpLabel(QStringLiteral("Saved model: load → 1 → 4"),QStringLiteral("No retraining required. Load only trusted .joblib files: they are executable Python serialization. Keep feature keys/order/radii compatible with the saved model. Reports from its training describe only its original validation scope.")),3,0,1,2);
		layout->addWidget(flow);

		QLabel* tutorial=named(new QLabel(QStringLiteral(
			"<b>Before you start</b><br>Original LAS/LAZ values are preserved. Preparation creates reusable Scalar Fields; manual labels are <b>Trusted</b>; predictions remain <b>Derived</b> until you explicitly apply them.<br><br>"
			"<b>To create and validate a new model</b><br>"
			"<b>1.</b> In <b>Prepare → Features</b>, compute one reproducible multiscale schema, including HAG when appropriate. "
			"<b>2.</b> In <b>Annotate</b>, label representative examples of every target ASPRS class in several spatially separated areas. "
			"<b>3.</b> Export the dataset, choose a classifier and train with spatial blocks. Check class support, confusion matrix, precision, recall and F1: a class missing from the test blocks is not validated. "
			"<b>4.</b> Predict, inspect class and confidence Scalar Fields in CloudCompare, then Apply only predictions above a justified threshold.<br><br>"
			"<b>To reuse a saved model</b><br>Load a trusted <code>.joblib</code>, recreate its exact feature keys, radii and HAG convention, then Predict → review → Apply. Retraining is unnecessary when the incoming cloud is compatible with the model's documented density, sensor and landscape domain.<br><br>"
			"<b>CPU/GPU</b><br>Random Forest, Extra Trees and Histogram Gradient Boosting use CPU. XGBoost and Multiscale MLP can use CUDA when compatible GPU libraries are installed. Ground, DTM and geometric feature computation currently use CPU.")),
			"ALiS.Workspace.Models.Tutorial.Text");
		tutorial->setWordWrap(true);
		tutorial->setTextInteractionFlags(Qt::TextSelectableByMouse);
		tutorial->setStyleSheet(QStringLiteral("QLabel { color:#172554; background:#eff6ff; border:1px solid #bfdbfe; padding:10px; border-radius:5px; }"));
		layout->addWidget(tutorial);

		QLabel* video=named(new QLabel(QStringLiteral("Video tutorial: will be added after the workflow is frozen.")),
			"ALiS.Workspace.Models.VideoPlaceholder");
		video->setStyleSheet(QStringLiteral("QLabel { color:#64748b; padding:4px; }"));
		layout->addWidget(video);

		QLabel* safety = named(new QLabel(QStringLiteral(
			"All classifiers in this supervised tab train only from Manual/Trusted labels. "
			"ML/DL outputs remain Derived until an explicit thresholded Apply.")),
			"ALiS.Workspace.Models.SafetyNote");
		safety->setWordWrap(true);
		safety->setStyleSheet(QStringLiteral("QLabel { color:#1e3a8a; background:#dbeafe; padding:8px; border-radius:4px; }"));
		layout->addWidget(safety);

		QGroupBox* progress = named(new QGroupBox(QStringLiteral("Where am I? — guided workflow")),
			"ALiS.Workspace.Models.WorkflowStatus");
		auto* progressLayout = new QVBoxLayout(progress);
		m_modelWorkflowLabels.clear();
		for (int step = 0; step < 5; ++step)
		{
			auto* label = valueLabel(qPrintable(QStringLiteral("ALiS.Workspace.Models.WorkflowStatus.%1").arg(step)));
			m_modelWorkflowLabels << label;
			progressLayout->addWidget(label);
		}
		layout->addWidget(progress);

		QGroupBox* inputs = named(new QGroupBox(QStringLiteral("1. Available features (cache or saved SF)")),
		                              "ALiS.Workspace.Models.Inputs");
		QVBoxLayout* inputLayout = new QVBoxLayout(inputs);
		m_modelFeatureList = named(new QListWidget, "ALiS.Workspace.Models.FeatureList");
		m_modelFeatureList->setMinimumHeight(190);
		inputLayout->addWidget(m_modelFeatureList);
		m_trustedTrainingValue = valueLabel("ALiS.Workspace.Models.TrustedCount",
		                                         QStringLiteral("0 Manual/Trusted ASPRS points"));
		explain(m_trustedTrainingValue, QStringLiteral("Which labels train ASPRS?"), QStringLiteral(
			"Only points explicitly confirmed in the ASPRS domain and still matching qAL_ASPRS_TrainingClass are eligible. "
			"Archaeology labels and automatic Ground/predictions do not confirm an ASPRS class. "
			"Older saved clouds keep their labels, but mixed-domain qAL_TrainingLabel is ambiguous: select and reapply "
			"the correct ASPRS class in Annotate to confirm it, even if its value is unchanged. "
			"Artificial persistence-test clouds are excluded. Save your work as BIN."));
		inputLayout->addWidget(m_trustedTrainingValue);
		m_selectLoadedTrainingButton = commandButton(QStringLiteral("Select loaded training set…"),
			"ALiS.Workspace.Models.SelectLoadedTraining");
		m_loadTrainingButton = commandButton(QStringLiteral("Load LAS/LAZ training set…"),
			"ALiS.Workspace.Models.LoadTraining");
		const QString trainingHelp = QStringLiteral(
			"Choose the Scalar Field that contains the certified ASPRS classes (LAS Classification is the default). "
			"The selected values become Working plus Manual/Trusted labels; source data are preserved and the operation is undoable.");
		explain(m_selectLoadedTrainingButton, QStringLiteral("Already loaded cloud"), QStringLiteral(
			"Select a point cloud in CloudCompare's DB Tree, then choose its class field. ") + trainingHelp);
		explain(m_loadTrainingButton, QStringLiteral("Load certified LAS/LAZ"), QStringLiteral(
			"Open a new LAS/LAZ directly from Models, then choose its class field. ") + trainingHelp);
		connect(m_selectLoadedTrainingButton, &QPushButton::clicked, this, [this]() { Q_EMIT selectTrainingSetRequested(false, false); });
		connect(m_loadTrainingButton, &QPushButton::clicked, this, [this]() { Q_EMIT selectTrainingSetRequested(true, false); });
		registerCommandButton(m_selectLoadedTrainingButton);
		registerCommandButton(m_loadTrainingButton);
		QHBoxLayout* trainingButtons = new QHBoxLayout;
		trainingButtons->addWidget(m_selectLoadedTrainingButton);
		trainingButtons->addWidget(m_loadTrainingButton);
		inputLayout->addLayout(trainingButtons);
		m_trainingCloudValue = valueLabel("ALiS.Workspace.Models.TrainingCloud", QStringLiteral("Training: not selected"));
		inputLayout->addWidget(m_trainingCloudValue);
		layout->addWidget(inputs);

		QGroupBox* validation = named(new QGroupBox(QStringLiteral("2. Validation dataset")),
			"ALiS.Workspace.Models.Validation");
		QFormLayout* validationForm = formLayout(validation);
		m_validationModeCombo = named(new QComboBox, "ALiS.Workspace.Models.ValidationMode");
		m_validationModeCombo->addItem(QStringLiteral("Internal spatial hold-out (%)"), QStringLiteral("internal_spatial"));
		m_validationModeCombo->addItem(QStringLiteral("Independent external test cloud"), QStringLiteral("external_cloud"));
		validationForm->addRow(helpLabel(QStringLiteral("Strategy"), QStringLiteral(
			"Internal: one certified cloud is divided into spatially separated train/test blocks. External: all eligible training points fit the model and a different certified cloud is used only for testing.")), m_validationModeCombo);
		QWidget* testButtonsWidget = new QWidget;
		QHBoxLayout* testButtons = new QHBoxLayout(testButtonsWidget);
		testButtons->setContentsMargins(0, 0, 0, 0);
		m_selectLoadedTestButton = commandButton(QStringLiteral("Select loaded test…"), "ALiS.Workspace.Models.SelectLoadedTest");
		m_loadTestButton = commandButton(QStringLiteral("Load LAS/LAZ test…"), "ALiS.Workspace.Models.LoadTest");
		connect(m_selectLoadedTestButton, &QPushButton::clicked, this, [this]() { Q_EMIT selectTrainingSetRequested(false, true); });
		connect(m_loadTestButton, &QPushButton::clicked, this, [this]() { Q_EMIT selectTrainingSetRequested(true, true); });
		registerCommandButton(m_selectLoadedTestButton); registerCommandButton(m_loadTestButton);
		testButtons->addWidget(m_selectLoadedTestButton); testButtons->addWidget(m_loadTestButton);
		validationForm->addRow(QStringLiteral("External test"), testButtonsWidget);
		m_testCloudValue = valueLabel("ALiS.Workspace.Models.TestCloud", QStringLiteral("Test: internal split"));
		validationForm->addRow(m_testCloudValue);
		layout->addWidget(validation);

		// The Python bridge is an implementation detail. Keep its automatically resolved
		// paths available to the controller without exposing them in the normal workflow.
		m_pythonExecutableEdit = named(new QLineEdit(this), "ALiS.Workspace.Models.Python");
		m_workerScriptEdit = named(new QLineEdit(this), "ALiS.Workspace.Models.WorkerScript");
		m_pythonExecutableEdit->hide();
		m_workerScriptEdit->hide();

		QGroupBox* library = named(new QGroupBox(QStringLiteral("3. Model library")),
		                               "ALiS.Workspace.Models.Library");
		QGridLayout* libraryLayout = new QGridLayout(library);
		m_modelPathEdit = named(new QLineEdit, "ALiS.Workspace.Models.ModelPath");
		m_modelPathEdit->setPlaceholderText(QStringLiteral("Optional saved model for prediction without retraining"));
		m_modelRepositoryEdit = named(new QLineEdit, "ALiS.Workspace.Models.Repository");
		m_modelRepositoryEdit->setPlaceholderText(QStringLiteral("Repository for datasets, models and technical reports"));
		QPushButton* browseModel = commandButton(QStringLiteral("Load…"), "ALiS.Workspace.Models.BrowseModel");
		QPushButton* browseRepository = commandButton(QStringLiteral("Choose…"), "ALiS.Workspace.Models.BrowseRepository");
		libraryLayout->addWidget(helpLabel(QStringLiteral("Saved model"), QStringLiteral("Load a previously validated .joblib package and classify a compatible cloud without training again.")), 0, 0);
		libraryLayout->addWidget(m_modelPathEdit, 0, 1); libraryLayout->addWidget(browseModel, 0, 2);
		libraryLayout->addWidget(helpLabel(QStringLiteral("Repository"), QStringLiteral("Permanent library of certified datasets, reusable models, metadata, metrics, plots and reports.")), 1, 0);
		libraryLayout->addWidget(m_modelRepositoryEdit, 1, 1); libraryLayout->addWidget(browseRepository, 1, 2);
		QLabel* engineStatus = new QLabel(QStringLiteral("ML engine: configured automatically · GPU selected by each compatible classifier"));
		engineStatus->setStyleSheet(QStringLiteral("QLabel { color:#475569; padding:3px; }"));
		libraryLayout->addWidget(engineStatus, 2, 0, 1, 3);
		m_modelCatalogSummary = new QLabel(QStringLiteral("Catalog not analysed yet."));
		m_modelCatalogSummary->setWordWrap(true);
		m_modelCatalogTree = new QTreeWidget;
		m_modelCatalogTree->setObjectName(QStringLiteral("ALiS.Workspace.Models.SmartCatalog"));
		m_modelCatalogTree->setHeaderLabels({QStringLiteral("Classifier"), QStringLiteral("Match"), QStringLiteral("Score"), QStringLiteral("Reason")});
		m_modelCatalogTree->setRootIsDecorated(false); m_modelCatalogTree->setAlternatingRowColors(true);
		m_modelCatalogTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
		m_modelCatalogTree->setMinimumHeight(130); m_modelCatalogTree->header()->setStretchLastSection(true);
		m_refreshCatalogButton = commandButton(QStringLiteral("Refresh intelligent catalog"), "ALiS.Workspace.Models.RefreshCatalog");
		m_useCatalogModelButton = commandButton(QStringLiteral("Use selected model"), "ALiS.Workspace.Models.UseCatalogModel");
		QPushButton* selectCompatibleModels = commandButton(QStringLiteral("Select all compatible"), "ALiS.Workspace.Models.SelectCompatibleModels");
		QHBoxLayout* catalogButtons = new QHBoxLayout;
		catalogButtons->addWidget(m_refreshCatalogButton); catalogButtons->addWidget(m_useCatalogModelButton); catalogButtons->addWidget(selectCompatibleModels); catalogButtons->addStretch(1);
		libraryLayout->addWidget(m_modelCatalogSummary, 3, 0, 1, 3);
		libraryLayout->addWidget(m_modelCatalogTree, 4, 0, 1, 3);
		libraryLayout->addLayout(catalogButtons, 5, 0, 1, 3);
		connect(browseModel, &QPushButton::clicked, this, [this]()
		{
			const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Load trusted classifier model"),
				m_modelPathEdit->text(), QStringLiteral("Joblib model (*.joblib);;All files (*)"));
			if (!path.isEmpty()) m_modelPathEdit->setText(path);
		});
		connect(browseRepository, &QPushButton::clicked, this, [this]()
		{
			const QString path = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose or create ALiS repository"),
				m_modelRepositoryEdit->text());
			if (!path.isEmpty()) m_modelRepositoryEdit->setText(path);
		});
		connect(m_modelPathEdit, &QLineEdit::textChanged, this, [this](const QString& path)
		{
			m_modelAvailable = QFileInfo::exists(path);
			updateCommandAvailability();
		});
		connect(m_modelRepositoryEdit, &QLineEdit::editingFinished, this, [this]()
		{
			QSettings().setValue(QStringLiteral("ALiS/modelRepository"), m_modelRepositoryEdit->text().trimmed());
			refreshModelCatalog();
			refreshPretrainedCatalog();
		});
		connect(m_refreshCatalogButton, &QPushButton::clicked, this, &WorkspaceDock::refreshModelCatalog);
		connect(m_useCatalogModelButton, &QPushButton::clicked, this, &WorkspaceDock::useSelectedCatalogModel);
		connect(selectCompatibleModels, &QPushButton::clicked, this, [this]()
		{
			m_modelCatalogTree->clearSelection();
			for (int row = 0; row < m_modelCatalogTree->topLevelItemCount(); ++row)
			{
				QTreeWidgetItem* item = m_modelCatalogTree->topLevelItem(row);
				if (item->data(0, Qt::UserRole + 1).toBool()) item->setSelected(true);
			}
		});
		connect(m_modelCatalogTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem*, int) { useSelectedCatalogModel(); });
		connect(m_modelCatalogTree, &QTreeWidget::itemSelectionChanged, this, &WorkspaceDock::updateCommandAvailability);
		layout->addWidget(library);

		QGroupBox* classifiers = named(new QGroupBox(QStringLiteral("4. Choose one or more classifiers")),
		                                  "ALiS.Workspace.Models.Classifiers");
		QVBoxLayout* classifierLayout = new QVBoxLayout(classifiers);
		QLabel* classifierIntro = new QLabel(QStringLiteral(
			"Activate any combination. Each selected classifier keeps its own settings and is trained as a separate, reusable model. "
			"Multiple selections share the validation strategy and seed. PointNet uses raw XYZ instead of selected features; its spatial guard is shared by the comparison batch. Check identical point-ID hashes in reports: nonfinite-row handling can change membership."));
		classifierIntro->setWordWrap(true);
		classifierIntro->setStyleSheet(QStringLiteral("QLabel { color:#334155; padding:3px; }"));
		classifierLayout->addWidget(classifierIntro);
		QHBoxLayout* classifierSelection = new QHBoxLayout;
		QPushButton* selectAllModels = commandButton(QStringLiteral("Select all"), "ALiS.Workspace.Models.SelectAllClassifiers");
		QPushButton* clearAllModels = commandButton(QStringLiteral("Deselect all"), "ALiS.Workspace.Models.ClearAllClassifiers");
		classifierSelection->addWidget(selectAllModels); classifierSelection->addWidget(clearAllModels); classifierSelection->addStretch(1);
		classifierLayout->addLayout(classifierSelection);
		m_modelCardChecks.clear(); m_modelCardDevices.clear(); m_modelCardPrimaryParameters.clear();
		m_modelCardBatchSizes.clear(); m_modelCardLearningRates.clear(); m_modelCardMaxDepths.clear();
		m_modelCardMinimumLeaves.clear(); m_modelCardMaxFeatures.clear(); m_modelCardClassWeights.clear();
		m_modelCardCriteria.clear(); m_modelCardSubsamples.clear(); m_modelCardColumnSamples.clear();
		const QStringList classifierIds = {QStringLiteral("random_forest"), QStringLiteral("extra_trees"),
			QStringLiteral("hist_gradient_boosting"), QStringLiteral("xgboost"), QStringLiteral("multiscale_mlp"), QStringLiteral("pointnet")};
		const QStringList classifierNames = {QStringLiteral("Random Forest"), QStringLiteral("Extra Trees"),
			QStringLiteral("Histogram Gradient Boosting"), QStringLiteral("XGBoost"), QStringLiteral("Multiscale MLP"), QStringLiteral("PointNet · local 3D patches")};
		const QStringList classifierKinds = {QStringLiteral("ML · robust baseline · CPU"), QStringLiteral("ML · randomized trees · CPU"),
			QStringLiteral("ML · boosting · CPU"), QStringLiteral("ML · boosting · CPU / CUDA"), QStringLiteral("DL · engineered multiscale features · CPU / CUDA"), QStringLiteral("DL · raw XYZ neighbourhoods · CPU / CUDA")};
		const QStringList classifierDescriptions = {
			QStringLiteral("Recommended first baseline: stable, interpretable feature importance and good performance on mixed LiDAR features."),
			QStringLiteral("More randomized than RF; often fast and useful as an independent tree-ensemble comparison."),
			QStringLiteral("Efficient boosting for tabular geometric features; iterations are trained sequentially."),
			QStringLiteral("Powerful gradient boosting. Auto prefers CUDA when the installed XGBoost build and GPU support it."),
			QStringLiteral("Experimental neural network over the selected multiscale features. It is not a raw point-cloud network."),
			QStringLiteral("Experimental PointNet-style local-patch classifier (Qi et al., CVPR 2017). Learns from raw XYZ neighbours using a shared point MLP and symmetric maximum pooling. No computed features required: selected RGB/SFs are ignored by this model only. This ALiS variant has no T-Net and is not a pretrained canonical PointNet. Coordinates must be in metres. Radius controls context; nearest-K caps the neighbourhood size. Training uses an automatic 2-radius spatial guard; no test labels enter the patches. Save and reuse it like other supervised models. CPU builds neighbourhoods; CUDA trains and predicts when available. Batch size counts patches, not individual points. Results are experimental: compare held-out per-class metrics, not training loss alone.")};
		QGridLayout* modelGrid = new QGridLayout;
		modelGrid->setContentsMargins(0, 0, 0, 0);
		modelGrid->setHorizontalSpacing(10);
		modelGrid->setVerticalSpacing(10);
		modelGrid->setColumnStretch(0, 1);
		modelGrid->setColumnStretch(1, 1);
		for (int i = 0; i < classifierIds.size(); ++i)
		{
			QGroupBox* card = new QGroupBox(classifierNames[i]);
			card->setObjectName(QStringLiteral("ALiS.Workspace.Models.Card.%1").arg(classifierIds[i]));
			QVBoxLayout* cardLayout = new QVBoxLayout(card);
			cardLayout->setContentsMargins(12, 10, 12, 10); cardLayout->setSpacing(7);
			QHBoxLayout* header = new QHBoxLayout;
			QCheckBox* enabled = new QCheckBox(QStringLiteral("Include in training / comparison"));
			enabled->setObjectName(QStringLiteral("ALiS.Workspace.Models.Card.%1.Enabled").arg(classifierIds[i]));
			enabled->setProperty("classifierId", classifierIds[i]);
			enabled->setChecked(i == 0);
			explain(enabled, classifierNames[i], classifierDescriptions[i]);
			QLabel* kind = new QLabel(classifierKinds[i]);
			kind->setStyleSheet(QStringLiteral("QLabel { color:#64748b; font-size:10px; }"));
			QToolButton* info = new QToolButton;
			info->setText(QStringLiteral("i")); info->setAutoRaise(true); info->setFixedSize(20, 20);
			info->setToolTip(QStringLiteral("<qt><b>%1</b><br><br>%2</qt>").arg(classifierNames[i], classifierDescriptions[i]));
			header->addWidget(enabled); header->addStretch(1); header->addWidget(info);
			cardLayout->addLayout(header);
			cardLayout->addWidget(kind);
			QFormLayout* settings = new QFormLayout;
			settings->setContentsMargins(0, 0, 0, 0);
			settings->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
			QComboBox* profile = new QComboBox(card);
			profile->setObjectName(QStringLiteral("ALiS.Workspace.Models.Card.%1.Profile").arg(classifierIds[i]));
			profile->addItem(QStringLiteral("Ultra low (fastest)"), 0);
			profile->addItem(QStringLiteral("Fast preview"), 1);
			profile->addItem(QStringLiteral("Standard"), 2);
			profile->addItem(QStringLiteral("High accuracy"), 3);
			profile->addItem(QStringLiteral("Ultra high"), 4);
			profile->setCurrentIndex(2);
			profile->setMinimumWidth(150);
			QComboBox* device = new QComboBox(card);
			device->setObjectName(QStringLiteral("ALiS.Workspace.Models.Card.%1.Device").arg(classifierIds[i]));
			if (i >= 3)
			{
				device->addItem(QStringLiteral("Auto (prefer GPU)"), QStringLiteral("auto"));
				device->addItem(QStringLiteral("CPU"), QStringLiteral("cpu"));
				device->addItem(QStringLiteral("CUDA GPU"), QStringLiteral("cuda"));
			}
			else device->addItem(QStringLiteral("CPU"), QStringLiteral("cpu"));
			device->setMinimumWidth(150);
			QSpinBox* primary = new QSpinBox(card);
			primary->setObjectName(QStringLiteral("ALiS.Workspace.Models.Card.%1.Primary").arg(classifierIds[i]));
			primary->setRange(i >= 4 ? 1 : 10, i >= 4 ? 1000 : 5000); primary->setValue(i >= 4 ? 30 : 300);
			primary->setMinimumWidth(150);
			QSpinBox* batch = new QSpinBox(card); batch->setRange(32, 1000000); batch->setValue(4096);
			if (i == 5) batch->setRange(8, 1024);
			QDoubleSpinBox* patchRadius = nullptr; QSpinBox* patchNeighbors = nullptr;
			if (i == 5)
			{
				patchRadius = new QDoubleSpinBox(card); patchRadius->setRange(0.01, 50.0); patchRadius->setDecimals(3); patchRadius->setValue(0.5);
				patchRadius->setObjectName(QStringLiteral("ALiS.Workspace.Models.PointNet.Radius"));
				patchNeighbors = new QSpinBox(card); patchNeighbors->setRange(8, 512); patchNeighbors->setValue(64);
				patchNeighbors->setObjectName(QStringLiteral("ALiS.Workspace.Models.PointNet.Neighbors"));
				explain(patchRadius, QStringLiteral("PointNet neighbourhood radius"), classifierDescriptions[i]);
				explain(patchNeighbors, QStringLiteral("Maximum neighbours per patch"), QStringLiteral("Uses the nearest K points inside the radius. Sparse patches are masked. Larger K costs more RAM/VRAM and time. 64 is a starting point, not a universal optimum. No labels are used to choose neighbours."));
				settings->addRow(QStringLiteral("Patch radius (m)"), patchRadius);
				settings->addRow(QStringLiteral("Maximum neighbours"), patchNeighbors);
			}
			PrecisionSpinBox* learning = new PrecisionSpinBox; learning->setParent(card);
			learning->setRange(0.000001, 1.0); learning->setDecimals(6); learning->setValue(0.001);
			QSpinBox* maxDepth = new QSpinBox(card); maxDepth->setRange(0, 100); maxDepth->setSpecialValueText(QStringLiteral("Unlimited"));
			maxDepth->setObjectName(QStringLiteral("ALiS.Workspace.Models.Card.%1.MaxDepth").arg(classifierIds[i]));
			QSpinBox* minimumLeaf = new QSpinBox(card); minimumLeaf->setRange(1, 10000); minimumLeaf->setValue(i == 2 ? 20 : 1);
			minimumLeaf->setObjectName(QStringLiteral("ALiS.Workspace.Models.Card.%1.MinimumLeaf").arg(classifierIds[i]));
			QComboBox* maxFeatures = new QComboBox(card); maxFeatures->addItem(QStringLiteral("Square root"), QStringLiteral("sqrt")); maxFeatures->addItem(QStringLiteral("log2"), QStringLiteral("log2")); maxFeatures->addItem(QStringLiteral("All features"), QStringLiteral("all"));
			QComboBox* classWeight = new QComboBox(card); classWeight->addItem(QStringLiteral("Balanced per tree"), QStringLiteral("balanced_subsample")); classWeight->addItem(QStringLiteral("Balanced globally"), QStringLiteral("balanced")); classWeight->addItem(QStringLiteral("None"), QStringLiteral("none"));
			if (i >= 2) classWeight->setCurrentIndex(1);
			QComboBox* criterion = new QComboBox(card); criterion->addItem(QStringLiteral("Gini"), QStringLiteral("gini")); criterion->addItem(QStringLiteral("Entropy"), QStringLiteral("entropy")); criterion->addItem(QStringLiteral("Log loss"), QStringLiteral("log_loss"));
			PrecisionSpinBox* subsample = new PrecisionSpinBox; subsample->setParent(card); subsample->setRange(0.1, 1.0); subsample->setDecimals(2); subsample->setSingleStep(0.05); subsample->setValue(0.9);
			PrecisionSpinBox* columnSample = new PrecisionSpinBox; columnSample->setParent(card); columnSample->setRange(0.1, 1.0); columnSample->setDecimals(2); columnSample->setSingleStep(0.05); columnSample->setValue(0.9);
			QCheckBox* imputeMissing = new QCheckBox(QStringLiteral("Impute missing feature values"), card);
			imputeMissing->setObjectName(QStringLiteral("ALiS.Workspace.Models.Card.%1.ImputeMissing").arg(classifierIds[i]));
			imputeMissing->setChecked(true);
			explain(imputeMissing, QStringLiteral("Missing-feature handling"), QStringLiteral("Replaces missing geometric-feature values using medians learned only from the training partition and adds missingness indicators. Keep enabled when multiscale neighbourhoods leave NaN values, especially at cloud boundaries. The previous strict mode discarded every row containing even one NaN."));
			settings->addRow(QStringLiteral("Tuning profile"), profile);
			settings->addRow(QStringLiteral("Compute device"), device);
			settings->addRow(i >= 4 ? QStringLiteral("Epochs") : (i == 2 ? QStringLiteral("Iterations") : (i == 3 ? QStringLiteral("Boosting rounds") : QStringLiteral("Trees"))), primary);
			settings->addRow(QStringLiteral("Maximum depth (0 = unlimited)"), maxDepth);
			settings->addRow(QStringLiteral("Minimum samples per leaf"), minimumLeaf);
			settings->addRow(QStringLiteral("Features per split"), maxFeatures);
			settings->addRow(QStringLiteral("Class weighting"), classWeight);
			settings->addRow(QStringLiteral("Split criterion"), criterion);
			settings->addRow(QStringLiteral("Learning rate"), learning);
			settings->addRow(QStringLiteral("Row subsample"), subsample);
			settings->addRow(QStringLiteral("Feature subsample"), columnSample);
			settings->addRow(QStringLiteral("Missing values"), imputeMissing);
			settings->addRow(QStringLiteral("Batch size"), batch);
			maxDepth->setVisible(i < 4); minimumLeaf->setVisible(i <= 2); maxFeatures->setVisible(i <= 1);
			classWeight->setVisible(i < 4); criterion->setVisible(i <= 1); learning->setVisible(i >= 2);
			subsample->setVisible(i == 3); columnSample->setVisible(i == 3); batch->setVisible(i >= 4);
			imputeMissing->setVisible(i != 5);
			for (QWidget* widget : QList<QWidget*>{maxDepth, minimumLeaf, maxFeatures, classWeight, criterion, learning, subsample, columnSample, batch, imputeMissing})
				if (QWidget* label = settings->labelForField(widget)) label->setVisible(!widget->isHidden());
			cardLayout->addLayout(settings);
			auto applyProfile = [primary, batch, learning, maxDepth, minimumLeaf, maxFeatures, classWeight, criterion, subsample, columnSample, imputeMissing, i](int preset)
			{
				if (i >= 4)
				{
					const int epochs[] = {5, 10, 30, 80, 150};
					const int batches[] = {16384, 8192, 4096, 2048, 1024};
					const double rates[] = {0.002, 0.0015, 0.001, 0.0005, 0.00025};
					primary->setValue(epochs[preset]); batch->setValue(batches[preset]); learning->setValue(rates[preset]);
					if (i == 5) { const int patchBatches[] = {256, 256, 128, 64, 32}; batch->setValue(patchBatches[preset]); }
				}
				else
				{
					const int estimators[] = {50, 100, 300, 700, 1200};
					const int depths[] = {10, 16, 0, 0, 0};
					const int leaves[] = {8, 4, 1, 1, 1};
					primary->setValue(estimators[preset]); maxDepth->setValue(depths[preset]); minimumLeaf->setValue(i == 2 ? std::max(5, leaves[preset] * 5) : leaves[preset]);
					maxFeatures->setCurrentIndex(preset == 4 ? 2 : 0);
					classWeight->setCurrentIndex(i >= 2 ? 1 : 0); criterion->setCurrentIndex(0);
					const double rates[] = {0.15, 0.10, 0.05, 0.03, 0.02}; learning->setValue(rates[preset]);
					const double sampling[] = {0.65, 0.75, 0.90, 0.95, 1.0}; subsample->setValue(sampling[preset]); columnSample->setValue(sampling[preset]);
				}
				imputeMissing->setChecked(i != 5);
			};
			connect(profile, QOverload<int>::of(&QComboBox::currentIndexChanged), this, applyProfile); applyProfile(2);
			m_modelCardChecks.push_back(enabled); m_modelCardDevices.push_back(device);
			m_modelCardPrimaryParameters.push_back(primary); m_modelCardBatchSizes.push_back(batch); m_modelCardLearningRates.push_back(learning);
			m_modelCardMaxDepths.push_back(maxDepth); m_modelCardMinimumLeaves.push_back(minimumLeaf); m_modelCardMaxFeatures.push_back(maxFeatures);
			m_modelCardClassWeights.push_back(classWeight); m_modelCardCriteria.push_back(criterion); m_modelCardSubsamples.push_back(subsample); m_modelCardColumnSamples.push_back(columnSample);
			m_modelCardImputeMissing.push_back(imputeMissing);
			auto setCardEnabled = [this, profile, device, primary, batch, learning, maxDepth, minimumLeaf, maxFeatures, classWeight, criterion, subsample, columnSample, imputeMissing, i](bool checked)
			{
				for (QWidget* widget : QList<QWidget*>{profile, device, primary, batch, learning, maxDepth, minimumLeaf, maxFeatures, classWeight, criterion, subsample, columnSample, imputeMissing}) widget->setEnabled(checked);
				if (m_trainModelButton) updateCommandAvailability();
			};
			connect(enabled, &QCheckBox::toggled, this, setCardEnabled); setCardEnabled(enabled->isChecked());
			if (patchRadius) { patchRadius->setEnabled(enabled->isChecked()); patchNeighbors->setEnabled(enabled->isChecked());
				connect(enabled, &QCheckBox::toggled, patchRadius, &QWidget::setEnabled);
				connect(enabled, &QCheckBox::toggled, patchNeighbors, &QWidget::setEnabled); }
			// The CloudCompare dock can be narrow in logical pixels on high-DPI displays.
			// Full-width cards preserve the reference design without collapsing controls.
			modelGrid->addWidget(card, i, 0, 1, 2);
		}
		classifierLayout->addLayout(modelGrid);
		connect(selectAllModels, &QPushButton::clicked, this, [this]() { for (QAbstractButton* check : m_modelCardChecks) check->setChecked(true); });
		connect(clearAllModels, &QPushButton::clicked, this, [this]() { for (QAbstractButton* check : m_modelCardChecks) check->setChecked(false); });
		layout->addWidget(classifiers);

		QGroupBox* parameters = named(new QGroupBox(QStringLiteral("5. Shared validation and prediction parameters")),
		                                  "ALiS.Workspace.Models.Parameters");
		QFormLayout* parameterForm = formLayout(parameters);
		m_modelBlockSizeSpin = named(new PrecisionSpinBox, "ALiS.Workspace.Models.BlockSize");
		configureLengthSpin(m_modelBlockSizeSpin, 20.0);
		m_modelTrainFractionSpin = named(new PrecisionSpinBox, "ALiS.Workspace.Models.TrainFraction");
		m_modelTrainFractionSpin->setRange(50.0, 100.0); m_modelTrainFractionSpin->setDecimals(0);
		m_modelTrainFractionSpin->setValue(80.0); m_modelTrainFractionSpin->setSuffix(QStringLiteral(" %"));
		m_modelTrainFractionSpin->setReadOnly(true); m_modelTrainFractionSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
		m_modelTestFractionSpin = named(new PrecisionSpinBox, "ALiS.Workspace.Models.TestFraction");
		m_modelTestFractionSpin->setRange(5.0, 50.0); m_modelTestFractionSpin->setDecimals(0);
		m_modelTestFractionSpin->setSingleStep(5.0); m_modelTestFractionSpin->setValue(20.0);
		m_modelTestFractionSpin->setSuffix(QStringLiteral(" %"));
		m_modelSeedSpin = named(new QSpinBox, "ALiS.Workspace.Models.Seed");
		m_modelSeedSpin->setRange(0, 2147483647); m_modelSeedSpin->setValue(42);
		m_predictionChunkSpin = named(new QSpinBox, "ALiS.Workspace.Models.ChunkSize");
		m_predictionChunkSpin->setRange(1000, 10000000); m_predictionChunkSpin->setSingleStep(50000);
		m_predictionChunkSpin->setValue(250000);
		m_predictionConfidenceSpin = named(new PrecisionSpinBox, "ALiS.Workspace.Models.ConfidenceThreshold");
		m_predictionConfidenceSpin->setRange(0.0, 1.0); m_predictionConfidenceSpin->setDecimals(2);
		m_predictionConfidenceSpin->setSingleStep(0.05); m_predictionConfidenceSpin->setValue(0.0);
		m_predictionConfidenceFilterCheck = named(new QCheckBox(QStringLiteral("Filter prediction view using threshold")), "ALiS.Workspace.Models.ConfidenceViewFilter");
		parameterForm->addRow(helpLabel(QStringLiteral("Spatial validation block"),QStringLiteral("Whole XY blocks are held out. Use blocks larger than local object/feature support and label every class in several separated blocks. Millions of nearby points are not millions of independent tests.")), m_modelBlockSizeSpin);
		parameterForm->addRow(helpLabel(QStringLiteral("Training percentage"), QStringLiteral("Internal mode: automatically equals 100 minus the test percentage. External mode: 100% of the training cloud is used for fitting.")), m_modelTrainFractionSpin);
		parameterForm->addRow(helpLabel(QStringLiteral("Test percentage"), QStringLiteral("Internal mode only. The remaining percentage is used for training; splitting is spatial, not a random point shuffle.")), m_modelTestFractionSpin);
		connect(m_modelTestFractionSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double value)
		{
			if (m_validationModeCombo->currentData().toString() != QStringLiteral("external_cloud")) m_modelTrainFractionSpin->setValue(100.0 - value);
		});
		connect(m_validationModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int)
		{
			const bool external = m_validationModeCombo->currentData().toString() == QStringLiteral("external_cloud");
			m_selectLoadedTestButton->setEnabled(external && m_hasSession && !m_busy);
			m_loadTestButton->setEnabled(external && m_hasSession && !m_busy);
			if (external)
			{
				m_modelTestFractionSpin->setProperty("internalValue", m_modelTestFractionSpin->value());
				m_modelTestFractionSpin->setMaximum(100.0); m_modelTestFractionSpin->setValue(100.0);
				m_modelTrainFractionSpin->setValue(100.0);
			}
			else
			{
				m_modelTestFractionSpin->setMaximum(50.0);
				m_modelTestFractionSpin->setValue(m_modelTestFractionSpin->property("internalValue").isValid() ? m_modelTestFractionSpin->property("internalValue").toDouble() : 20.0);
				m_modelTrainFractionSpin->setValue(100.0 - m_modelTestFractionSpin->value());
				m_testCloudValue->setText(QStringLiteral("Test: internal spatial split"));
			}
			m_modelTestFractionSpin->setEnabled(!external && m_hasSession && !m_busy);
		});
		m_selectLoadedTestButton->setEnabled(false); m_loadTestButton->setEnabled(false);
		parameterForm->addRow(QStringLiteral("Random seed"), m_modelSeedSpin);
		parameterForm->addRow(QStringLiteral("Prediction chunk"), m_predictionChunkSpin);
		parameterForm->addRow(helpLabel(QStringLiteral("Review/apply confidence ≥"),QStringLiteral("Predict always writes a class and confidence for every valid point. This downstream threshold never reruns the classifier. Enable the view filter to hide lower-confidence points; Apply uses the same threshold when copying to Working. Scores are not calibrated probabilities of correctness.")), m_predictionConfidenceSpin);
		parameterForm->addRow(QStringLiteral("Downstream display filter"), m_predictionConfidenceFilterCheck);
		connect(m_predictionConfidenceFilterCheck, &QCheckBox::toggled, this, [this](bool enabled) { Q_EMIT predictionConfidenceFilterRequested(enabled, m_predictionConfidenceSpin->value()); });
		connect(m_predictionConfidenceSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double value)
		{
			if (m_predictionConfidenceFilterCheck->isChecked()) Q_EMIT predictionConfidenceFilterRequested(true, value);
		});
		layout->addWidget(parameters);

		QGroupBox* actions = named(new QGroupBox(QStringLiteral("6. Train, compare and inspect")),
		                               "ALiS.Workspace.Models.Actions");
		QGridLayout* actionLayout = new QGridLayout(actions);
		m_exportDatasetButton = commandButton(QStringLiteral("Export Dataset"), "ALiS.Workspace.Models.Export");
		m_trainModelButton = commandButton(QStringLiteral("Train / compare selected models"), "ALiS.Workspace.Models.Train");
		m_predictModelButton = commandButton(QStringLiteral("Predict (Derived)"), "ALiS.Workspace.Models.Predict");
		m_applyPredictionsButton = commandButton(QStringLiteral("Apply predictions to ASPRS Working"), "ALiS.Workspace.Models.Apply");
		m_viewModelResultsButton = commandButton(QStringLiteral("View statistics and report"), "ALiS.Workspace.Models.ViewResults");
		m_viewPredictionStatisticsButton = commandButton(QStringLiteral("View prediction statistics"), "ALiS.Workspace.Models.ViewPredictionStatistics");
		actionLayout->addWidget(m_exportDatasetButton, 0, 0);
		actionLayout->addWidget(m_trainModelButton, 0, 1);
		actionLayout->addWidget(m_viewModelResultsButton, 1, 0);
		actionLayout->addWidget(m_viewPredictionStatisticsButton, 1, 1);
		m_exportDatasetButton->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));m_trainModelButton->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));m_predictModelButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));m_applyPredictionsButton->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));
		explain(m_exportDatasetButton,QStringLiteral("Export dataset"),QStringLiteral("Saves aligned features, point IDs, coordinates and eligible training labels in the repository. Does not train or modify the input cloud."));
		explain(m_trainModelButton,QStringLiteral("Train and compare selected models"),QStringLiteral("Runs every activated classifier with the same selected features, spatial validation settings and random seed. Each model and technical report is saved separately in the repository."));
		explain(m_predictModelButton,QStringLiteral("Predict"),QStringLiteral("Applies the saved model and its preprocessing. Output is Derived Classification and Confidence; original/Working classes remain unchanged until Apply."));
		explain(m_applyPredictionsButton,QStringLiteral("Review, then Apply"),QStringLiteral("Explicitly promotes predictions above the confidence threshold into Working with Undo available. Never delete noise automatically or promote predictions to Trusted training without manual review."));
		m_modelDatasetValue = valueLabel("ALiS.Workspace.Models.Dataset", QStringLiteral("No exported dataset"));
		m_modelSummaryValue = valueLabel("ALiS.Workspace.Models.Summary", QStringLiteral("No model run"));
		actionLayout->addWidget(m_modelDatasetValue, 2, 0, 1, 2);
		actionLayout->addWidget(m_modelSummaryValue, 3, 0, 1, 2);
		layout->addWidget(actions);

		QGroupBox* classify = named(new QGroupBox(QStringLiteral("7. Classify a cloud with a trained model")),
			"ALiS.Workspace.Models.Classification");
		auto* classifyLayout = new QGridLayout(classify);
		auto* classifyIntro = new QLabel(QStringLiteral(
			"Choose a model in section 3, then explicitly choose the target cloud. The target must contain the same feature schema used during training. Predict creates Derived fields only; visual review and Apply remain separate."));
		classifyIntro->setWordWrap(true);
		classifyIntro->setStyleSheet(QStringLiteral("QLabel { color:#172554; background:#eff6ff; padding:8px; border-radius:4px; }"));
		classifyLayout->addWidget(classifyIntro, 0, 0, 1, 2);
		m_selectLoadedClassificationButton = commandButton(QStringLiteral("Select loaded cloud…"), "ALiS.Workspace.Models.SelectClassificationCloud");
		m_loadClassificationButton = commandButton(QStringLiteral("Load LAS/LAZ cloud…"), "ALiS.Workspace.Models.LoadClassificationCloud");
		classifyLayout->addWidget(m_selectLoadedClassificationButton, 1, 0);
		classifyLayout->addWidget(m_loadClassificationButton, 1, 1);
		m_classificationCloudValue = valueLabel("ALiS.Workspace.Models.ClassificationCloud", QStringLiteral("Target: not selected"));
		classifyLayout->addWidget(m_classificationCloudValue, 2, 0, 1, 2);
		classifyLayout->addWidget(m_predictModelButton, 3, 0);
		classifyLayout->addWidget(m_applyPredictionsButton, 3, 1);
		m_comparePredictionsButton = commandButton(QStringLiteral("Predict / compare selected catalog models"), "ALiS.Workspace.Models.ComparePredictions");
		classifyLayout->addWidget(m_comparePredictionsButton, 4, 0, 1, 2);
		auto* classifySteps = new QLabel(QStringLiteral("Predict always creates all Derived predictions. Confidence is a downstream view/apply threshold: changing it does not rerun Predict. Ctrl-select catalog models (or Select all compatible) for a target-cloud comparison."));
		classifySteps->setWordWrap(true);
		classifySteps->setStyleSheet(QStringLiteral("QLabel { color:#475569; padding:4px; }"));
		classifyLayout->addWidget(classifySteps, 5, 0, 1, 2);
		layout->addWidget(classify);
		for (QAbstractButton* button : {m_exportDatasetButton, m_trainModelButton, m_predictModelButton, m_applyPredictionsButton, m_viewModelResultsButton, m_viewPredictionStatisticsButton, m_comparePredictionsButton})
		{
			registerCommandButton(button);
		}
		connect(m_modelFeatureList, &QListWidget::itemChanged, this, [this](QListWidgetItem*) { updateCommandAvailability(); });
		connect(m_exportDatasetButton, &QPushButton::clicked, this, [this]()
		{
			Q_EMIT exportModelDatasetRequested(selectedModelFeatureKeys(), m_modelRepositoryEdit->text().trimmed());
		});
		connect(m_trainModelButton, &QPushButton::clicked, this, [this]()
		{
			Q_EMIT trainModelRequested(selectedModelFeatureKeys(), m_pythonExecutableEdit->text().trimmed(),
				m_workerScriptEdit->text().trimmed(), m_modelPathEdit->text().trimmed(), m_modelRepositoryEdit->text().trimmed(),
				selectedModelSpecifications(),
				m_modelBlockSizeSpin->value(), m_modelTestFractionSpin->value() / 100.0, m_modelSeedSpin->value(),
				m_validationModeCombo->currentData().toString());
		});
		connect(m_predictModelButton, &QPushButton::clicked, this, [this]()
		{
			Q_EMIT predictModelRequested(selectedModelFeatureKeys(), m_pythonExecutableEdit->text().trimmed(),
				m_workerScriptEdit->text().trimmed(), m_modelPathEdit->text().trimmed(),
				m_modelRepositoryEdit->text().trimmed(), m_predictionChunkSpin->value());
		});
		connect(m_applyPredictionsButton, &QPushButton::clicked, this, [this]()
		{
			Q_EMIT applyModelPredictionsRequested(m_predictionConfidenceSpin->value());
		});
		connect(m_comparePredictionsButton, &QPushButton::clicked, this, [this]()
		{
			Q_EMIT compareModelPredictionsRequested(selectedModelFeatureKeys(), m_pythonExecutableEdit->text().trimmed(),
				m_workerScriptEdit->text().trimmed(), selectedCatalogModelPaths(), m_modelRepositoryEdit->text().trimmed(), m_predictionChunkSpin->value());
		});
		connect(m_viewPredictionStatisticsButton, &QPushButton::clicked, this, &WorkspaceDock::showPredictionStatisticsRequested);
		connect(m_selectLoadedClassificationButton, &QPushButton::clicked, this, [this]()
		{
			Q_EMIT selectClassificationCloudRequested(false);
		});
		connect(m_loadClassificationButton, &QPushButton::clicked, this, [this]()
		{
			Q_EMIT selectClassificationCloudRequested(true);
		});
		registerCommandButton(m_selectLoadedClassificationButton);
		registerCommandButton(m_loadClassificationButton);
		connect(m_viewModelResultsButton, &QPushButton::clicked, this, [this]()
		{
			Q_EMIT showModelResultsRequested(m_modelPathEdit->text().trimmed());
		});
		updateModelWorkflowGuide();
		layout->addStretch(1);
		// Keep the established supervised workflow as the primary entry point.
		m_modelsTabs->tabBar()->moveTab(1, 0);
		buildVegetationTab();
		m_modelsTabs->tabBar()->moveTab(2, 0);
		m_modelsTabs->setCurrentIndex(0);
	}

	void WorkspaceDock::buildAnnotationTab()
	{
		QVBoxLayout* layout = addScrollableTab(
			m_annotationTabs,
			QStringLiteral("Studio"),
			"ALiS.Workspace.Tab.Annotate",
			"ALiS.Workspace.Tab.Annotate.Content");

		QLabel* intro = new QLabel(QStringLiteral(
			"Annotation Studio shows the SAME cloud in four configurable views, including rotatable 3D. Ctrl adds selections; Live sync links pan/zoom in real time while preserving view orientations. Draw section (Top) places a custom vertical plane; X/Y/Z presets remain available. "
			"Move a live section, select with rectangle/polygon/lasso/wand, and assign trusted labels directly. "
			"Only display samples are reduced; selections use original point IDs."));
		intro->setWordWrap(true);
		intro->setStyleSheet(QStringLiteral("QLabel { color:#334155; background:#e0f2fe; padding:8px; border-radius:4px; }"));
		layout->addWidget(intro);
		QPushButton* studio = commandButton(QStringLiteral("Open Annotation Studio"), "ALiS.Workspace.Annotate.OpenStudio");
		studio->setMinimumHeight(38);
		explain(studio, QStringLiteral("Annotation Studio"), QStringLiteral("Floating plugin window with four configurable OpenGL views, including orbit 3D, sharing one source cloud and selection. Live sync continuously links centre/scale from whichever panel you navigate, retaining independent orientations. Each panel also has a view menu, Fit and one-shot Sync. Ctrl adds for one gesture; Navigate rotates 3D or pans 2D. Live metric slab, exact labels and Undo/Redo. Selections pass through the slab, not only frontmost pixels. The native CloudCompare viewport remains available. Confirm metric units in Session first."));
		connect(studio, &QPushButton::clicked, this, [this]() { Q_EMIT annotationActionRequested(QStringLiteral("studio")); });
		registerCommandButton(studio);
		layout->addWidget(studio);

		QGroupBox* viewsGroup = named(new QGroupBox(QStringLiteral("Optional: native CloudCompare view")),
			"ALiS.Workspace.Annotate.Views");
		QGridLayout* views = new QGridLayout(viewsGroup);
		const QList<QPair<QString, QString>> viewActions = {
			{QStringLiteral("Top (map)"), QStringLiteral("view.top")},
			{QStringLiteral("Front"), QStringLiteral("view.front")},
			{QStringLiteral("Right"), QStringLiteral("view.right")},
			{QStringLiteral("3D / ISO"), QStringLiteral("view.iso")}
		};
		for (int i = 0; i < viewActions.size(); ++i)
		{
			QPushButton* button = commandButton(viewActions.at(i).first,
				qPrintable(QStringLiteral("ALiS.Workspace.Annotate.View.%1").arg(i)));
			explain(button, viewActions.at(i).first,
				QStringLiteral("Changes the active CloudCompare viewport only; point coordinates and labels are not modified."));
			connect(button, &QPushButton::clicked, this, [this, action = viewActions.at(i).second]() { Q_EMIT annotationActionRequested(action); });
			registerCommandButton(button);
			views->addWidget(button, i / 2, i % 2);
		}
		layout->addWidget(viewsGroup);

		QGroupBox* selectionGroup = named(new QGroupBox(QStringLiteral("2. Isolate or select points")),
			"ALiS.Workspace.Annotate.Selection");
		QVBoxLayout* selection = new QVBoxLayout(selectionGroup);
		QPushButton* segment = commandButton(QStringLiteral("Polygon / rectangle selection"), "ALiS.Workspace.Annotate.Segment");
		QPushButton* crossSection = commandButton(QStringLiteral("Section / clipping box"), "ALiS.Workspace.Annotate.CrossSection");
		explain(segment, QStringLiteral("Polygon / rectangle selection"),
			QStringLiteral("Launches CloudCompare's native Segment tool (shortcut T). Draw in the active view, then keep the inside or outside subset."));
		explain(crossSection, QStringLiteral("Section / clipping box"),
			QStringLiteral("Launches CloudCompare's Cross Section tool. Set the box thickness in metres inside that tool and move it interactively along X, Y or Z."));
		connect(segment, &QPushButton::clicked, this, [this]() { Q_EMIT annotationActionRequested(QStringLiteral("host.segment")); });
		connect(crossSection, &QPushButton::clicked, this, [this]() { Q_EMIT annotationActionRequested(QStringLiteral("host.cross_section")); });
		registerCommandButton(segment);
		registerCommandButton(crossSection);
		selection->addWidget(segment);
		selection->addWidget(crossSection);

		layout->addWidget(selectionGroup);

		QGroupBox* labelGroup = named(new QGroupBox(QStringLiteral("3. Assign a trusted label")),
			"ALiS.Workspace.Annotate.Assign");
		QVBoxLayout* labelLayout = new QVBoxLayout(labelGroup);
		QLabel* labelNote = new QLabel(QStringLiteral("After segmentation, keep only the intended subset visible. Label application is deliberately separated from selection so accidental clicks cannot modify classes."));
		labelNote->setWordWrap(true);
		QPushButton* goLabels = commandButton(QStringLiteral("Open Quick labels"), "ALiS.Workspace.Annotate.GoLabels");
		explain(goLabels, QStringLiteral("Open Quick labels"), QStringLiteral("Opens the class-assignment panel. Labels apply to the current visible subset and are recorded in Undo/Redo history."));
		connect(goLabels, &QPushButton::clicked, this, [this]()
		{
			showAnnotationWorkspace();
			if (m_annotationTabs) m_annotationTabs->setCurrentIndex(1);
		});
		registerCommandButton(goLabels);
		labelLayout->addWidget(labelNote);
		labelLayout->addWidget(goLabels);
		layout->addWidget(labelGroup);
		layout->addStretch(1);
	}

	void WorkspaceDock::buildLabelsTab()
	{
		QVBoxLayout* layout = addScrollableTab(
			m_annotationTabs,
			QStringLiteral("Quick labels"),
			"ALiS.Workspace.Tab.Labels",
			"ALiS.Workspace.Tab.Labels.Content");

		QGroupBox* scopeGroup = named(new QGroupBox(QStringLiteral("Selection scope")),
		                                   "ALiS.Workspace.Labels.Scope");
		QVBoxLayout* scopeLayout = new QVBoxLayout(scopeGroup);
		QLabel* scopeNote = new QLabel(QStringLiteral("Labels are applied only to the current visible subset produced by CloudCompare selection/segmentation tools."));
		scopeNote->setWordWrap(true);
		m_visibleSubsetValue = valueLabel("ALiS.Workspace.Labels.VisibleSubset", QStringLiteral("No visible subset"));
		scopeLayout->addWidget(scopeNote);
		scopeLayout->addWidget(m_visibleSubsetValue);
		layout->addWidget(scopeGroup);

		QGroupBox* classificationGroup = named(new QGroupBox(QStringLiteral("Manual / Trusted annotation")),
		                                            "ALiS.Workspace.Labels.Classification");
		QFormLayout* classificationForm = formLayout(classificationGroup);
		m_asprsCombo = named(new QComboBox, "ALiS.Workspace.Labels.AsprsClass");
		for (const AspClass& value : asprsClasses())
		{
			if (!value.reserved)
			{
				m_asprsCombo->addItem(
					QStringLiteral("%1 - %2").arg(value.code).arg(QString::fromUtf8(value.name)),
					static_cast<int>(value.code));
			}
		}
		m_asprsCombo->insertSeparator(m_asprsCombo->count());
		for (const AspClass& value : archaeologyAsprsClasses())
			m_asprsCombo->addItem(QStringLiteral("qAL Other %1 - %2").arg(value.code).arg(QString::fromUtf8(value.name)), static_cast<int>(value.code));
		m_asprsCombo->setCurrentIndex(m_asprsCombo->findData(2));
		classificationForm->addRow(QStringLiteral("ASPRS / qAL Other"), m_asprsCombo);
		QLabel* trusted = new QLabel(QStringLiteral("Training label: Manual / Trusted"));
		trusted->setStyleSheet(QStringLiteral("QLabel { color:#166534; font-weight:600; }"));
		classificationForm->addRow(trusted);
		m_applyLabelsButton = commandButton(QStringLiteral("Apply Label to Visible Subset"),
		                                    "ALiS.Workspace.Labels.Apply");
		classificationForm->addRow(m_applyLabelsButton);
		layout->addWidget(classificationGroup);

		QGroupBox* editGroup = named(new QGroupBox(QStringLiteral("Restore and history")),
		                                  "ALiS.Workspace.Labels.EditActions");
		QGridLayout* editLayout = new QGridLayout(editGroup);
		m_restoreOriginalButton = commandButton(QStringLiteral("Restore Original ASPRS"),
		                                        "ALiS.Workspace.Labels.RestoreOriginal");
		m_undoButton = commandButton(QStringLiteral("Undo"), "ALiS.Workspace.Labels.Undo");
		m_redoButton = commandButton(QStringLiteral("Redo"), "ALiS.Workspace.Labels.Redo");
		editLayout->addWidget(m_restoreOriginalButton, 0, 0, 1, 2);
		editLayout->addWidget(m_undoButton, 1, 0);
		editLayout->addWidget(m_redoButton, 1, 1);
		layout->addWidget(editGroup);

		for (QAbstractButton* button : {m_applyLabelsButton,
		                                m_restoreOriginalButton,
		                                m_undoButton,
		                                m_redoButton})
		{
			registerCommandButton(button);
		}
		connect(m_applyLabelsButton, &QPushButton::clicked, this, [this]()
		{
			Q_EMIT applyLabelsRequested(m_asprsCombo->currentData().toInt(), -1);
		});
		connect(m_restoreOriginalButton, &QPushButton::clicked, this, &WorkspaceDock::restoreOriginalLabelsRequested);
		connect(m_undoButton, &QPushButton::clicked, this, &WorkspaceDock::undoRequested);
		connect(m_redoButton, &QPushButton::clicked, this, &WorkspaceDock::redoRequested);
		layout->addStretch(1);
	}

	void WorkspaceDock::buildHistoryTab()
	{
		QVBoxLayout* layout = addScrollableTab(
			m_tabs,
			QStringLiteral("History"),
			"ALiS.Workspace.Tab.History",
			"ALiS.Workspace.Tab.History.Content");
		m_tabs->setTabIcon(m_tabs->count()-1,QIcon(QStringLiteral(":/CC/plugin/ALiS/images/history.svg")));
		QLabel* note = new QLabel(QStringLiteral("Processing provenance: operation, timestamp, parameters, affected points and outputs."));
		note->setWordWrap(true);
		layout->addWidget(note);
		m_historyTree = named(new QTreeWidget, "ALiS.Workspace.History.Tree");
		m_historyTree->setColumnCount(3);
		m_historyTree->setHeaderLabels({QStringLiteral("Time"), QStringLiteral("Operation"), QStringLiteral("Details")});
		m_historyTree->setRootIsDecorated(false);
		m_historyTree->setAlternatingRowColors(true);
		m_historyTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
		m_historyTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
		m_historyTree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
		layout->addWidget(m_historyTree, 1);
	}

	QVector<double> WorkspaceDock::scaleRadii() const
	{
		QVector<double> radii;
		if (!m_scaleList)
		{
			return radii;
		}
		for (int i = 0; i < m_scaleList->count(); ++i)
		{
			bool ok = false;
			const double value = m_scaleList->item(i)->data(Qt::UserRole).toDouble(&ok);
			if (ok && std::isfinite(value) && value > 0.0)
			{
				radii.push_back(value);
			}
		}
		return radii;
	}

	QStringList WorkspaceDock::selectedFeatureIds() const
	{
		QStringList result;
		if (!m_featureList)
		{
			return result;
		}
		for (int i = 0; i < m_featureList->count(); ++i)
		{
			const QListWidgetItem* item = m_featureList->item(i);
			if (item->checkState() == Qt::Checked)
			{
				result.push_back(item->data(Qt::UserRole).toString());
			}
		}
		return result;
	}

	QStringList WorkspaceDock::selectedModelFeatureKeys() const
	{
		QStringList result;
		if (!m_modelFeatureList) return result;
		for (int i = 0; i < m_modelFeatureList->count(); ++i)
		{
			const QListWidgetItem* item = m_modelFeatureList->item(i);
			if (item->checkState() == Qt::Checked)
			{
				result.push_back(item->data(Qt::UserRole).toString());
			}
		}
		return result;
	}

	QStringList WorkspaceDock::selectedBootstrapFeatureKeys() const
	{
		QStringList result;
		if (!m_bootstrapFeatureList) return result;
		for (int i = 0; i < m_bootstrapFeatureList->count(); ++i)
		{
			const QListWidgetItem* item = m_bootstrapFeatureList->item(i);
			if (item->checkState() == Qt::Checked) result.push_back(item->data(Qt::UserRole).toString());
		}
		return result;
	}

	QStringList WorkspaceDock::selectedModelSpecifications() const
	{
		QStringList result;
		const int count = std::min({m_modelCardChecks.size(), m_modelCardDevices.size(),
			m_modelCardPrimaryParameters.size(), m_modelCardBatchSizes.size(), m_modelCardLearningRates.size(),
			m_modelCardMaxDepths.size(), m_modelCardMinimumLeaves.size(), m_modelCardMaxFeatures.size(),
			m_modelCardClassWeights.size(), m_modelCardCriteria.size(), m_modelCardSubsamples.size(), m_modelCardColumnSamples.size(),
			m_modelCardImputeMissing.size()});
		for (int i = 0; i < count; ++i)
		{
			if (!m_modelCardChecks[i] || !m_modelCardChecks[i]->isChecked()) continue;
			QStringList specification{
				m_modelCardChecks[i]->property("classifierId").toString(), m_modelCardDevices[i]->currentData().toString(),
				QString::number(m_modelCardPrimaryParameters[i]->value()), QString::number(m_modelCardBatchSizes[i]->value()),
				QString::number(m_modelCardLearningRates[i]->value(), 'g', 17), QString::number(m_modelCardMaxDepths[i]->value()),
				QString::number(m_modelCardMinimumLeaves[i]->value()), m_modelCardMaxFeatures[i]->currentData().toString(),
				m_modelCardClassWeights[i]->currentData().toString(), m_modelCardCriteria[i]->currentData().toString(),
				QString::number(m_modelCardSubsamples[i]->value(), 'g', 17), QString::number(m_modelCardColumnSamples[i]->value(), 'g', 17),
				m_modelCardImputeMissing[i]->isChecked() ? QStringLiteral("1") : QStringLiteral("0")
			};
			if (m_modelCardChecks[i]->property("classifierId").toString() == QStringLiteral("pointnet"))
			{
				const auto* radius = findChild<QDoubleSpinBox*>(QStringLiteral("ALiS.Workspace.Models.PointNet.Radius"));
				const auto* neighbors = findChild<QSpinBox*>(QStringLiteral("ALiS.Workspace.Models.PointNet.Neighbors"));
				specification << QString::number(radius->value(), 'g', 17) << QString::number(neighbors->value());
			}
			result.push_back(specification.join(QLatin1Char('|')));
		}
		return result;
	}

	QStringList WorkspaceDock::selectedCatalogModelPaths() const
	{
		QStringList paths;
		if (!m_modelCatalogTree) return paths;
		for (QTreeWidgetItem* item : m_modelCatalogTree->selectedItems())
			if (item->data(0, Qt::UserRole + 1).toBool() && !item->data(0, Qt::UserRole).toString().isEmpty()) paths << item->data(0, Qt::UserRole).toString();
		paths.removeDuplicates();
		return paths;
	}

	QString WorkspaceDock::modelRepositoryPath() const
	{
		return m_modelRepositoryEdit ? m_modelRepositoryEdit->text().trimmed() : QString();
	}

	void WorkspaceDock::setSessionInfo(const QString& cloudName,
	                                   quint64 entityUid,
	                                   quint64 pointCount,
	                                   const QString& unitDescription,
	                                   bool dirty)
	{
		m_hasSession = true;
		m_cloudNameValue->setText(cloudName);
		m_entityUidValue->setText(QString::number(entityUid));
		m_pointCountValue->setText(QString::number(pointCount));
		m_unitsValue->setText(unitDescription);
		setSessionDirty(dirty);
		setStatus(QStringLiteral("Session ready"), StatusTone::Ready);
		updateCommandAvailability();
	}

	void WorkspaceDock::clearSession()
	{
		m_hasSession = false;
		m_metricUnitsConfirmed = false;
		m_metricUnitsLockedByMetadata = false;
		if (m_metricUnitsCheck)
		{
			const QSignalBlocker blocker(m_metricUnitsCheck);
			m_metricUnitsCheck->setChecked(false);
			m_metricUnitsCheck->setEnabled(false);
		}
		m_previewAvailable = false;
		m_groundApplied = false;
		m_dtmAvailable = false;
		m_hagAvailable = false;
		m_computedFeatureAvailable = false;
		m_modelAvailable = false;
		m_predictionsAvailable = false;
		m_classificationTargetSelected = false;
		m_trustedTrainingPoints = 0;
		m_cachedFeatureIds.clear();
		m_cachedFeatureNames.clear();
		m_cachedFeatureRadii.clear();
		m_cloudNameValue->setText(QStringLiteral("Select a point cloud"));
		m_entityUidValue->setText(QStringLiteral("\u2014"));
		m_pointCountValue->setText(QStringLiteral("\u2014"));
		m_unitsValue->setText(QStringLiteral("Unknown"));
		m_dirtyValue->setText(QStringLiteral("No active Session"));
		setDisplayLegendState(false, false);
		m_visibleSubsetValue->setText(QStringLiteral("No visible subset"));
		m_groundSummaryValue->setText(QStringLiteral("No Ground result"));
		m_modelFeatureList->clear();
		m_trustedTrainingValue->setText(QStringLiteral("0 Manual/Trusted ASPRS points"));
		m_modelDatasetValue->setText(QStringLiteral("No exported dataset"));
		m_modelSummaryValue->setText(QStringLiteral("No model run"));
		if (m_predictionConfidenceFilterCheck) { const QSignalBlocker blocker(m_predictionConfidenceFilterCheck); m_predictionConfidenceFilterCheck->setChecked(false); }
		setPredictionStatistics(QString());
		if (m_classificationCloudValue) m_classificationCloudValue->setText(QStringLiteral("Target: not selected"));
		setStatus(QStringLiteral("Select a single point cloud"), StatusTone::Neutral);
		updateCommandAvailability();
	}

	void WorkspaceDock::setMetricUnitConfirmation(bool confirmed, bool lockedByMetadata)
	{
		m_metricUnitsConfirmed = confirmed;
		m_metricUnitsLockedByMetadata = lockedByMetadata;
		const QSignalBlocker blocker(m_metricUnitsCheck);
		m_metricUnitsCheck->setChecked(confirmed);
		m_metricUnitsCheck->setEnabled(m_hasSession && !lockedByMetadata && !m_busy);
		m_metricUnitsCheck->setText(lockedByMetadata
			? QStringLiteral("Coordinates are metres (from CRS metadata)")
			: QStringLiteral("Coordinates are metres (confirm once and remember)"));
		updateCommandAvailability();
	}

	void WorkspaceDock::setSessionDirty(bool dirty)
	{
		m_dirtyValue->setText(dirty ? QStringLiteral("Modified") : QStringLiteral("Clean"));
		m_dirtyValue->setStyleSheet(dirty
			? QStringLiteral("QLabel { color:#92400e; font-weight:600; }")
			: QStringLiteral("QLabel { color:#166534; }") );
	}

	void WorkspaceDock::setStatus(const QString& text, StatusTone tone)
	{
		QString foreground = QStringLiteral("#475569");
		QString background = QStringLiteral("#f1f5f9");
		if (tone == StatusTone::Ready)
		{
			foreground = QStringLiteral("#166534");
			background = QStringLiteral("#dcfce7");
		}
		else if (tone == StatusTone::Warning)
		{
			foreground = QStringLiteral("#92400e");
			background = QStringLiteral("#fef3c7");
		}
		else if (tone == StatusTone::Error)
		{
			foreground = QStringLiteral("#991b1b");
			background = QStringLiteral("#fee2e2");
		}
		m_statusValue->setText(text);
		m_statusValue->setStyleSheet(
			QStringLiteral("QLabel { color:%1; background:%2; border-radius:4px; padding:6px; }")
				.arg(foreground, background));
	}

	void WorkspaceDock::setBusy(bool busy)
	{
		m_busy = busy;
		// Native CSF pumps UI events: recipes must not mutate during a run.
		m_tabs->setEnabled(!busy);
		updateCommandAvailability();
	}

	void WorkspaceDock::setProgress(const QString& operation, int value, int maximum, bool cancellable)
	{
		m_progressOperationValue->setText(operation);
		if (maximum <= 0)
		{
			m_progressBar->setRange(0, 0);
		}
		else
		{
			m_progressBar->setRange(0, maximum);
			m_progressBar->setValue(std::max(0, std::min(value, maximum)));
		}
		m_progressBar->setVisible(true);
		m_progressOperationValue->setVisible(true);
		m_cancelButton->setVisible(cancellable);
		m_cancelButton->setEnabled(cancellable && m_busy);
	}

	void WorkspaceDock::clearProgress()
	{
		m_progressOperationValue->setText(QStringLiteral("Idle"));
		m_progressBar->setRange(0, 100);
		m_progressBar->setValue(0);
		m_cancelButton->setEnabled(false);
		m_cancelButton->setVisible(false);
	}

	void WorkspaceDock::setGroundState(bool previewAvailable,
	                                   bool groundApplied,
	                                   bool dtmAvailable,
	                                   bool hagAvailable,
	                                   const QString& summary)
	{
		m_previewAvailable = previewAvailable;
		m_groundApplied = groundApplied;
		m_dtmAvailable = dtmAvailable;
		m_hagAvailable = hagAvailable;
		m_groundSummaryValue->setText(summary.isEmpty() ? QStringLiteral("No Ground result") : summary);
		updateCommandAvailability();
	}

	void WorkspaceDock::setAdvancedGroundParameters(double clothResolution,
	                                                double classificationThreshold,
	                                                double timeStep,
	                                                int rigidness,
	                                                int iterations,
	                                                bool slopeProcessing)
	{
		const QSignalBlocker clothBlocker(m_clothResolutionSpin);
		const QSignalBlocker thresholdBlocker(m_classificationThresholdSpin);
		const QSignalBlocker timeStepBlocker(m_timeStepSpin);
		const QSignalBlocker rigidnessBlocker(m_csfSceneCombo);
		const QSignalBlocker iterationsBlocker(m_iterationsSpin);
		const QSignalBlocker slopeBlocker(m_slopeProcessingCheck);
		m_clothResolutionSpin->setValue(clothResolution);
		m_classificationThresholdSpin->setValue(classificationThreshold);
		m_timeStepSpin->setValue(timeStep);
		m_csfSceneCombo->setCurrentIndex(m_csfSceneCombo->findData(rigidness));
		m_iterationsSpin->setValue(iterations);
		m_slopeProcessingCheck->setChecked(slopeProcessing);
	}

	void WorkspaceDock::setPmfParameters(const QString& windowSizes, const QString& heightThresholds, double cellSize)
	{
		const QSignalBlocker windowsBlocker(m_pmfWindowsEdit);
		const QSignalBlocker thresholdsBlocker(m_pmfThresholdsEdit);
		const QSignalBlocker cellBlocker(m_pmfCellSizeSpin);
		m_pmfWindowsEdit->setText(windowSizes);
		m_pmfThresholdsEdit->setText(heightThresholds);
		m_pmfCellSizeSpin->setValue(cellSize);
	}

	void WorkspaceDock::setScaleRadii(const QVector<double>& radii)
	{
		QVector<double> cleaned;
		for (double value : radii)
		{
			if (std::isfinite(value) && value > 0.0)
			{
				cleaned.push_back(value);
			}
		}
		std::sort(cleaned.begin(), cleaned.end());
		cleaned.erase(std::unique(cleaned.begin(), cleaned.end(), [](double a, double b)
		{
			return qFuzzyCompare(1.0 + a, 1.0 + b);
		}), cleaned.end());

		m_updatingScales = true;
		const QSignalBlocker blocker(m_scaleList);
		m_scaleList->clear();
		for (double value : cleaned)
		{
			QListWidgetItem* item = new QListWidgetItem(QString::number(value, 'g', 8), m_scaleList);
			item->setData(Qt::UserRole, value);
			item->setFlags(item->flags() | Qt::ItemIsEditable);
		}
		m_updatingScales = false;
		updateScaleEditors();
	}

	void WorkspaceDock::showScaleSuggestion(const QVector<double>& radii, const QString& explanation)
	{
		setScaleRadii(radii);
		m_scaleExplanationValue->setText(explanation);
	}

	void WorkspaceDock::setModelFeatureChoices(const QStringList& keys, const QStringList& displayNames)
	{
		QSet<QString> previousSelection;
		for (const QString& key : selectedModelFeatureKeys()) previousSelection.insert(key);
		const bool firstPopulation = m_modelFeatureList->count() == 0;
		{
			const QSignalBlocker blocker(m_modelFeatureList);
			m_modelFeatureList->clear();
			for (int i = 0; i < keys.size(); ++i)
			{
				QListWidgetItem* item = new QListWidgetItem(i < displayNames.size() ? displayNames.at(i) : keys.at(i), m_modelFeatureList);
				item->setData(Qt::UserRole, keys.at(i));
				item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
				item->setCheckState((firstPopulation || previousSelection.contains(keys.at(i))) ? Qt::Checked : Qt::Unchecked);
			}
		}
		updateCommandAvailability();
		QSettings settings;
		if (settings.value(QStringLiteral("ALiS/catalog/automaticRecommendations"),
			settings.value(QStringLiteral("qArchaeoLiDAR/catalog/automaticRecommendations"), true)).toBool()) refreshModelCatalog();
	}


	void WorkspaceDock::setBootstrapFeatureChoices(const QStringList& keys, const QStringList& displayNames,
	                                               const QStringList& excludedByDefault)
	{
		if (!m_bootstrapFeatureList) return;
		QSet<QString> previous;
		for (const QString& key : selectedBootstrapFeatureKeys()) previous.insert(key);
		const bool initial = m_bootstrapFeatureList->count() == 0;
		{
			const QSignalBlocker blocker(m_bootstrapFeatureList);
			m_bootstrapFeatureList->clear();
			for (int i = 0; i < keys.size(); ++i)
			{
				const QString key = keys.at(i);
				const bool categorical = excludedByDefault.contains(key);
				auto* item = new QListWidgetItem(i < displayNames.size() ? displayNames.at(i) : key, m_bootstrapFeatureList);
				item->setData(Qt::UserRole, key);
				item->setData(Qt::UserRole + 1, categorical);
				item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
				item->setCheckState((initial ? !categorical : previous.contains(key)) ? Qt::Checked : Qt::Unchecked);
				item->setToolTip(categorical
					? QStringLiteral("Class, identifier or prior result: unchecked by default. Selecting it makes groups depend on previous labels or IDs; use intentionally when inspecting an existing classification.")
					: QStringLiteral("Uses values already present in the selected cloud. No geometric feature recomputation is needed. Selected channels are standardized before clustering."));
			}
		}
		updateCommandAvailability();
	}

	void WorkspaceDock::setModelWorkerPaths(const QString& pythonExecutable,
	                                       const QString& workerScript,
	                                       const QString& modelPath,
	                                       const QString& repositoryPath)
	{
		if (!pythonExecutable.isEmpty()) m_pythonExecutableEdit->setText(pythonExecutable);
		if (!workerScript.isEmpty()) m_workerScriptEdit->setText(workerScript);
		if (!modelPath.isEmpty()) m_modelPathEdit->setText(modelPath);
		if (!repositoryPath.isEmpty()) m_modelRepositoryEdit->setText(repositoryPath);
		refreshPretrainedCatalog();
	}

	void WorkspaceDock::setModelRepositoryPath(const QString& path)
	{
		if (!m_modelRepositoryEdit || path.trimmed().isEmpty()) return;
		m_modelRepositoryEdit->setText(QDir::toNativeSeparators(path.trimmed()));
		refreshModelCatalog();
		refreshPretrainedCatalog();
	}

	void WorkspaceDock::setCatalogCloudProfile(const QJsonObject& profile)
	{
		m_catalogCloudProfile = profile;
		refreshPretrainedCatalog();
	}

	void WorkspaceDock::refreshModelCatalog()
	{
		if (!m_modelCatalogTree || !m_modelCatalogSummary) return;
		m_modelCatalogTree->clear();
		const ModelCatalogSummary catalog = ModelCatalog::refresh(modelRepositoryPath(), m_catalogCloudProfile, selectedModelFeatureKeys());
		if (!catalog.error.isEmpty())
		{
			m_modelCatalogSummary->setText(catalog.error);
			m_useCatalogModelButton->setEnabled(false);
			return;
		}
		int reusable = 0;
		for (const ModelRecommendation& recommendation : catalog.recommendations)
		{
			QString classifier = recommendation.classifierId; classifier.replace(QLatin1Char('_'), QLatin1Char(' '));
			QTreeWidgetItem* item = new QTreeWidgetItem(m_modelCatalogTree, {classifier,
				recommendation.status, QString::number(recommendation.score, 'f', 1), recommendation.explanation});
			item->setData(0, Qt::UserRole, recommendation.modelPath);
			item->setData(0, Qt::UserRole + 1, recommendation.compatible);
			item->setToolTip(3, recommendation.explanation);
			if (recommendation.compatible) ++reusable;
			else item->setDisabled(true);
		}
		m_modelCatalogTree->resizeColumnToContents(0); m_modelCatalogTree->resizeColumnToContents(1); m_modelCatalogTree->resizeColumnToContents(2);
		for (int row = 0; row < m_modelCatalogTree->topLevelItemCount(); ++row)
		{
			QTreeWidgetItem* item = m_modelCatalogTree->topLevelItem(row);
			if (item->data(0, Qt::UserRole + 1).toBool()) { m_modelCatalogTree->setCurrentItem(item); break; }
		}
		m_modelCatalogSummary->setText(QStringLiteral("%1 datasets · %2 models · %3 reusable for the current feature schema. Double-click a compatible model to load it.")
			.arg(catalog.datasetCount).arg(catalog.modelCount).arg(reusable));
		m_useCatalogModelButton->setEnabled(reusable > 0);
	}

	void WorkspaceDock::useSelectedCatalogModel()
	{
		QTreeWidgetItem* item = m_modelCatalogTree ? m_modelCatalogTree->currentItem() : nullptr;
		if (!item || !item->data(0, Qt::UserRole + 1).toBool()) return;
		const QString path = item->data(0, Qt::UserRole).toString();
		if (!path.isEmpty())
		{
			m_modelPathEdit->setText(path);
			m_modelSummaryValue->setText(QStringLiteral("Reusable catalog model selected: %1").arg(item->text(0)));
			updateCommandAvailability();
		}
	}

	void WorkspaceDock::refreshPretrainedCatalog()
	{
		// External pre-trained models are deferred. No catalog or downloads are exposed.
	}

	void WorkspaceDock::setModelState(quint64 trustedTrainingPoints,
	                                  const QString& datasetPath,
	                                  bool modelAvailable,
	                                  bool predictionsAvailable,
	                                  const QString& summary)
	{
		m_trustedTrainingPoints = trustedTrainingPoints;
		Q_UNUSED(modelAvailable);
		m_modelAvailable = QFileInfo(m_modelPathEdit->text()).isFile();
		m_predictionsAvailable = predictionsAvailable;
		m_trustedTrainingValue->setText(QStringLiteral("%1 Manual/Trusted ASPRS points").arg(trustedTrainingPoints));
		m_modelDatasetValue->setText(datasetPath.isEmpty() ? QStringLiteral("No exported dataset") : datasetPath);
		m_modelSummaryValue->setText(summary.isEmpty()
			? (predictionsAvailable ? QStringLiteral("Derived predictions ready for review") : QStringLiteral("No model run"))
			: summary);
		updateModelWorkflowGuide();
		updateCommandAvailability();
	}

	void WorkspaceDock::setTrainingSetState(const QString& trainingCloud, const QString& testCloud)
	{
		m_trainingCloudValue->setText(trainingCloud.isEmpty()
			? QStringLiteral("Training: not selected") : QStringLiteral("Training: %1").arg(trainingCloud));
		if (m_validationModeCombo->currentData().toString() == QStringLiteral("external_cloud"))
			m_testCloudValue->setText(testCloud.isEmpty()
				? QStringLiteral("Test: not selected") : QStringLiteral("Test: %1").arg(testCloud));
		updateModelWorkflowGuide();
	}

	void WorkspaceDock::setClassificationTargetState(const QString& cloudName)
	{
		m_classificationTargetSelected = !cloudName.isEmpty();
		m_classificationCloudValue->setText(m_classificationTargetSelected
			? QStringLiteral("Target: %1").arg(cloudName) : QStringLiteral("Target: not selected"));
		updateModelWorkflowGuide();
		updateCommandAvailability();
	}

	void WorkspaceDock::setDisplayFields(const QStringList& names, const QString& activeField, bool hasVisibilityMask)
	{
		const QSignalBlocker blocker(m_visualizationCombo);
		const QString previous = m_visualizationCombo->currentData().toString();
		for (int i = m_visualizationCombo->count() - 1; i >= 0; --i)
			if (m_visualizationCombo->itemData(i).toString().startsWith(QStringLiteral("sf:")))
				m_visualizationCombo->removeItem(i);
		for (const QString& name : names) m_visualizationCombo->addItem(QStringLiteral("SF: %1").arg(name), QStringLiteral("sf:%1").arg(name));
		const bool keepFilter = hasVisibilityMask && (previous == QStringLiteral("ground") || previous == QStringLiteral("non_ground"));
		const QString mode = keepFilter ? previous : (activeField.isEmpty() ? QStringLiteral("original") : QStringLiteral("sf:%1").arg(activeField));
		const int index = m_visualizationCombo->findData(mode);
		m_visualizationCombo->setCurrentIndex(index >= 0 ? index : 0);
	}

	void WorkspaceDock::setDisplayPalettes(const QStringList& ids, const QStringList& names, const QString& activeId)
	{
		const QSignalBlocker blocker(m_displayPaletteCombo);
		m_displayPaletteCombo->clear();
		for (int i = 0; i < ids.size(); ++i) m_displayPaletteCombo->addItem(names.value(i, ids[i]), ids[i]);
		m_displayPaletteCombo->setCurrentIndex(m_displayPaletteCombo->findData(activeId));
		m_displayPaletteCombo->setEnabled(!activeId.isEmpty());
		m_displayEditButton->setEnabled(!activeId.isEmpty());
	}

	void WorkspaceDock::setDisplayRange(bool enabled, double minimum, double maximum, double start, double stop)
	{
		const QSignalBlocker a(m_displayMinSpin), b(m_displayMaxSpin);
		m_displayMinSpin->setRange(minimum, maximum);
		m_displayMaxSpin->setRange(minimum, maximum);
		m_displayMinSpin->setValue(start);
		m_displayMaxSpin->setValue(stop);
		m_displayMinSpin->setEnabled(enabled);
		m_displayMaxSpin->setEnabled(enabled);
		m_displayAutoButton->setEnabled(enabled);
		const QString hint = enabled ? QStringLiteral("Colour saturation endpoints in scalar-field units. Values outside remain visible in endpoint colours.")
			: QStringLiteral("Select a scalar field and a relative linear palette to edit the range. Absolute palettes are configured with Edit; logarithmic ranges are managed in CloudCompare.");
		m_displayMinSpin->setToolTip(hint);
		m_displayMaxSpin->setToolTip(hint);
	}

	void WorkspaceDock::setDisplayLegendState(bool enabled, bool visible)
	{
		const QSignalBlocker blocker(m_displayLegendCheck);
		m_displayLegendCheck->setEnabled(enabled);
		m_displayLegendCheck->setChecked(enabled && visible);
	}

	void WorkspaceDock::setVisibleSubset(const QString& description, quint64 pointCount)
	{
		m_visibleSubsetValue->setText(QStringLiteral("%1 (%2 points)").arg(description).arg(pointCount));
	}

	void WorkspaceDock::setUndoRedoAvailable(bool undoAvailable, bool redoAvailable)
	{
		m_undoAvailable = undoAvailable;
		m_redoAvailable = redoAvailable;
		updateCommandAvailability();
	}

	void WorkspaceDock::appendHistoryEntry(const QString& timestamp,
	                                       const QString& operation,
	                                       const QString& details)
	{
		new QTreeWidgetItem(m_historyTree, {timestamp, operation, details});
		m_historyTree->scrollToBottom();
	}

	void WorkspaceDock::clearHistory()
	{
		m_historyTree->clear();
	}

	void WorkspaceDock::registerCommandButton(QAbstractButton* button)
	{
		if (button && !m_commandButtons.contains(button))
		{
			m_commandButtons.push_back(button);
		}
	}

	void WorkspaceDock::emitSimpleGroundControls()
	{
		Q_EMIT simpleGroundControlsChanged(m_terrainComplexityCombo->currentData().toInt(),
		                                   m_microreliefCombo->currentData().toInt(),
		                                   m_vegetationCombo->currentData().toInt());
	}

	void WorkspaceDock::emitAdvancedGroundParameters()
	{
		{const QSignalBlocker block(m_groundPresetCombo);m_groundPresetCombo->setCurrentIndex(8);}
		Q_EMIT advancedGroundParametersChanged(m_clothResolutionSpin->value(),
		                                      m_classificationThresholdSpin->value(),
		                                      m_timeStepSpin->value(),
		                                      m_csfSceneCombo->currentData().toInt(),
		                                      m_iterationsSpin->value(),
		                                      m_slopeProcessingCheck->isChecked());
	}

	void WorkspaceDock::emitPmfParameters()
	{
		{const QSignalBlocker block(m_pmfPresetCombo);m_pmfPresetCombo->setCurrentIndex(4);}
		Q_EMIT pmfParametersChanged(m_pmfWindowsEdit->text().trimmed(),
		                            m_pmfThresholdsEdit->text().trimmed(),m_pmfCellSizeSpin->value());
	}

	void WorkspaceDock::resetSimpleGroundControls()
	{
		for(auto* combo:{m_terrainComplexityCombo,m_microreliefCombo,m_vegetationCombo}) { const QSignalBlocker block(combo); combo->setCurrentIndex(1); }
	}

	void WorkspaceDock::updateScaleEditors()
	{
		m_removeScaleButton->setEnabled(m_scaleList->count() > 0 && !m_busy);
		m_saveScalePresetButton->setEnabled(m_scaleList->count() > 0 && !m_busy);
	}

	ProcessingPreset WorkspaceDock::processingPreset() const
	{
		ProcessingPreset p;p.algorithm=m_groundAlgorithmCombo->currentData().toString();
		p.base.clothResolution=m_clothResolutionSpin->value();p.base.classificationThreshold=m_classificationThresholdSpin->value();
		p.base.timeStep=m_timeStepSpin->value();p.base.rigidness=m_csfSceneCombo->currentData().toInt();p.base.iterations=m_iterationsSpin->value();p.base.slopeProcessing=m_slopeProcessingCheck->isChecked();
		p.complexity=m_terrainComplexityCombo->currentIndex();p.microrelief=m_microreliefCombo->currentIndex();p.vegetation=m_vegetationCombo->currentIndex();
		auto parse=[](QString text){QVector<double> values;for(auto v:text.split(QRegularExpression("[,;\\s]+"),Qt::SkipEmptyParts)){bool ok=false;double n=v.toDouble(&ok);values<<(ok?n:-1.);}return values;};
		p.windows=parse(m_pmfWindowsEdit->text());p.thresholds=parse(m_pmfThresholdsEdit->text());p.cellSize=m_pmfCellSizeSpin->value();p.dtmStep=m_dtmGridStepSpin->value();
		p.features=selectedFeatureIds();p.radii=scaleRadii();p.minimumRadius=m_presetMinRadius->value();p.maximumRadius=m_presetMaxRadius->value();p.useReturns=m_presetUseReturns->isChecked();return p;
	}
	void WorkspaceDock::setProcessingPreset(const ProcessingPreset& p)
	{
		m_groundAlgorithmCombo->setCurrentIndex(m_groundAlgorithmCombo->findData(p.algorithm));
		for(auto* combo:{m_groundPresetCombo,m_pmfPresetCombo,m_terrainComplexityCombo,m_microreliefCombo,m_vegetationCombo})combo->blockSignals(true);
		m_groundPresetCombo->setCurrentIndex(8);m_pmfPresetCombo->setCurrentIndex(4);m_terrainComplexityCombo->setCurrentIndex(p.complexity);m_microreliefCombo->setCurrentIndex(p.microrelief);m_vegetationCombo->setCurrentIndex(p.vegetation);
		for(auto* combo:{m_groundPresetCombo,m_pmfPresetCombo,m_terrainComplexityCombo,m_microreliefCombo,m_vegetationCombo})combo->blockSignals(false);
		GroundParameters effective=applySimpleGroundControls(p.base,static_cast<SimpleLevel>(p.complexity),static_cast<SimpleLevel>(p.microrelief),static_cast<SimpleLevel>(p.vegetation),0.);
		setAdvancedGroundParameters(effective.clothResolution,effective.classificationThreshold,effective.timeStep,effective.rigidness,effective.iterations,effective.slopeProcessing);
		auto text=[](const QVector<double>& values){QStringList a;for(double v:values)a<<QString::number(v,'g',17);return a.join(", ");};
		setPmfParameters(text(p.windows),text(p.thresholds),p.cellSize);m_dtmGridStepSpin->setValue(p.dtmStep);setScaleRadii(p.radii);
		m_presetMinRadius->setValue(p.minimumRadius);m_presetMaxRadius->setValue(p.maximumRadius);m_presetUseReturns->setChecked(p.useReturns);
		{const QSignalBlocker block(m_featureList);for(int i=0;i<m_featureList->count();++i){auto* item=m_featureList->item(i);item->setCheckState(p.features.contains(item->data(Qt::UserRole).toString())?Qt::Checked:Qt::Unchecked);}}
		m_groundPresetGuidance->setText(QStringLiteral("Loaded: %1. Ground settings, context and feature scales restored.").arg(p.name));updateCommandAvailability();
	}
	void WorkspaceDock::setPresetLibrary(const QStringList& paths,const QStringList& names)
	{
		const QSignalBlocker block(m_savedPresetCombo);m_savedPresetCombo->clear();m_savedPresetCombo->addItem(QStringLiteral("Choose a saved preset…"),QString());for(int i=0;i<paths.size();++i)m_savedPresetCombo->addItem(names.value(i),paths[i]);
	}
	void WorkspaceDock::setCloudProfileText(const QString& text){m_cloudProfileValue->setText(text);}
	void WorkspaceDock::setProcessingSuggestion(const QString& text){m_processingSuggestion->setText(text);}
	void WorkspaceDock::setPredictionStatistics(const QString& text)
	{
		if (m_viewPredictionStatisticsButton) m_viewPredictionStatisticsButton->setToolTip(text.isEmpty() ? QStringLiteral("Run Predict first.") : text);
	}

	void WorkspaceDock::setBootstrapStatus(const QString& text, bool myriaAvailable)
	{
		Q_UNUSED(myriaAvailable);
		if (m_bootstrapStatusValue) m_bootstrapStatusValue->setText(text);
		updateCommandAvailability();
	}

	void WorkspaceDock::updateModelWorkflowGuide()
	{
		if (m_modelWorkflowLabels.size() != 5) return;
		bool onlyPointNet = !selectedModelSpecifications().isEmpty();
		for (const QString& specification : selectedModelSpecifications()) onlyPointNet = onlyPointNet && specification.startsWith(QStringLiteral("pointnet|"));
		const bool features = onlyPointNet || (m_modelFeatureList && !selectedModelFeatureKeys().isEmpty());
		const bool labels = m_trustedTrainingPoints > 1;
		const bool external = m_validationModeCombo
			&& m_validationModeCombo->currentData().toString() == QStringLiteral("external_cloud");
		const bool validation = labels && (!external || (m_testCloudValue && !m_testCloudValue->text().contains(QStringLiteral("not selected"))));
		const bool states[] = {features, labels, validation, m_modelAvailable, m_predictionsAvailable};
		const QStringList readyText = {
			onlyPointNet ? QStringLiteral("Raw XYZ ready for PointNet; no feature calculation required") : QStringLiteral("Feature schema selected (%1 fields)").arg(features ? selectedModelFeatureKeys().size() : 0),
			QStringLiteral("Training labels ready (%1 trusted points)").arg(m_trustedTrainingPoints),
			external ? QStringLiteral("Independent test cloud selected") : QStringLiteral("Internal spatial validation configured"),
			QStringLiteral("Reusable trained model available"),
			QStringLiteral("Derived predictions ready for visual review")};
		const QStringList waitingText = {
			QStringLiteral("Select or compute the feature Scalar Fields"),
			QStringLiteral("Select a certified set or annotate at least two classes"),
			external ? QStringLiteral("Select a separate certified test cloud") : QStringLiteral("Choose training labels first"),
			QStringLiteral("Train a classifier or load a compatible saved model"),
			m_classificationTargetSelected
				? QStringLiteral("Target selected — run Predict, then inspect class and confidence")
				: QStringLiteral("Choose the cloud to classify in section 7")};
		for (int index = 0; index < 5; ++index)
		{
			m_modelWorkflowLabels[index]->setText(QStringLiteral("%1  %2. %3")
				.arg(states[index] ? QStringLiteral("✓") : QStringLiteral("○"))
				.arg(index + 1)
				.arg(states[index] ? readyText[index] : waitingText[index]));
			m_modelWorkflowLabels[index]->setStyleSheet(states[index]
				? QStringLiteral("QLabel { color:#14532d; background:#dcfce7; padding:6px; border-radius:3px; }")
				: QStringLiteral("QLabel { color:#475569; background:#f1f5f9; padding:6px; border-radius:3px; }"));
		}
	}

	void WorkspaceDock::updateCommandAvailability()
	{
		updateModelWorkflowGuide();
		const bool ready = m_hasSession && !m_busy;
		for (QAbstractButton* button : m_commandButtons)
		{
			button->setEnabled(ready);
		}
		const bool externalValidation = m_validationModeCombo
			&& m_validationModeCombo->currentData().toString() == QStringLiteral("external_cloud");
		if (m_selectLoadedTrainingButton) m_selectLoadedTrainingButton->setEnabled(ready);
		if (m_loadTrainingButton) m_loadTrainingButton->setEnabled(!m_busy);
		if (m_selectLoadedTestButton) m_selectLoadedTestButton->setEnabled(ready && externalValidation);
		if (m_loadTestButton) m_loadTestButton->setEnabled(!m_busy && externalValidation);
		if (m_selectLoadedClassificationButton) m_selectLoadedClassificationButton->setEnabled(ready);
		if (m_loadClassificationButton) m_loadClassificationButton->setEnabled(!m_busy);
		if (m_modelTrainFractionSpin) m_modelTrainFractionSpin->setEnabled(ready && !externalValidation);
		if (m_modelTestFractionSpin) m_modelTestFractionSpin->setEnabled(ready && !externalValidation);

		m_groundPreviewButton->setEnabled(ready && m_metricUnitsConfirmed);
		m_groundApplyButton->setEnabled(ready && m_previewAvailable);
		m_groundDiscardButton->setEnabled(ready && m_previewAvailable);
		m_groundRestoreButton->setEnabled(ready && m_groundApplied);
		m_computeDtmButton->setEnabled(ready && m_groundApplied && m_metricUnitsConfirmed);
		m_computeHagButton->setEnabled(ready && m_dtmAvailable);
		const bool modelFeaturesSelected = !selectedModelFeatureKeys().isEmpty();
		bool onlyPointNet = !selectedModelSpecifications().isEmpty();
		for (const QString& specification : selectedModelSpecifications())
			onlyPointNet = onlyPointNet && specification.startsWith(QStringLiteral("pointnet|"));
		m_exportDatasetButton->setEnabled(ready && modelFeaturesSelected && m_metricUnitsConfirmed);
		m_trainModelButton->setEnabled(ready && (modelFeaturesSelected || onlyPointNet) && m_metricUnitsConfirmed
			&& m_trustedTrainingPoints > 1 && !selectedModelSpecifications().isEmpty());
		m_predictModelButton->setEnabled(ready && m_metricUnitsConfirmed && m_modelAvailable && m_classificationTargetSelected);
		m_applyPredictionsButton->setEnabled(ready && m_predictionsAvailable);
		m_viewPredictionStatisticsButton->setEnabled(!m_busy && m_predictionsAvailable);
		m_comparePredictionsButton->setEnabled(ready && m_metricUnitsConfirmed
			&& m_classificationTargetSelected && !selectedCatalogModelPaths().isEmpty());
		m_viewModelResultsButton->setEnabled(!m_busy && m_modelAvailable);
		if (m_bootstrapRunButton)
		{
			m_bootstrapRunButton->setEnabled(ready && !selectedBootstrapFeatureKeys().isEmpty());
			m_bootstrapRunButton->setToolTip(QStringLiteral("Groups points by the checked existing fields and/or RGB. Review cluster IDs in Annotation Studio and assign classes."));
		}
		m_computeFeaturesButton->setEnabled(ready && m_metricUnitsConfirmed && !selectedFeatureIds().isEmpty());
		m_suggestScalesButton->setEnabled(ready && m_metricUnitsConfirmed);
		m_metricUnitsCheck->setEnabled(m_hasSession && !m_metricUnitsLockedByMetadata && !m_busy);
		m_undoButton->setEnabled(ready && m_undoAvailable);
		m_redoButton->setEnabled(ready && m_redoAvailable);
		m_visualizationCombo->setEnabled(m_hasSession && !m_busy);
		m_displayPanel->setEnabled(ready);
		m_cancelButton->setEnabled(m_busy && m_cancelButton->isVisible());
		updateScaleEditors();
	}
}
