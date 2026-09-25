// SPDX-License-Identifier: GPL-2.0-or-later
#include "ALiSPlugin.h"

#include "WorkspaceController.h"
#include "WorkspaceDock.h"
#include "SettingsDialog.h"

#include <ccMainAppInterface.h>
#include <ccPointCloud.h>

#include <QAction>
#include <QMainWindow>
#include <QSettings>
#include <QStandardPaths>
#include <QDir>
#include <QTimer>
#include <QToolBar>
#include <QApplication>

ALiSPlugin::ALiSPlugin(QObject* parent)
	: QObject(parent)
	, ccStdPluginInterface(QStringLiteral(":/CC/plugin/ALiS/info.json"))
{
}

ALiSPlugin::~ALiSPlugin()
{
	delete m_workspaceController;
	m_workspaceController = nullptr;
	delete m_workspaceDock;
	m_workspaceDock = nullptr;
}

void ALiSPlugin::setMainAppInterface(ccMainAppInterface* app)
{
	ccStdPluginInterface::setMainAppInterface(app);
	if (m_app)
	{
		// CloudCompare creates plugin actions and restores its layout after this
		// callback. Defer opening until that is complete; an ordinary CC launch
		// must expose ALiS without a special launcher or environment variable.
		auto* startup = new QTimer(this);
		startup->setInterval(250);
		connect(startup, &QTimer::timeout, this, [this, startup]() {
			if (!m_app || !m_app->getMainWindow() || !m_app->getMainWindow()->isVisible()
				|| QApplication::activeModalWidget()) return;
			startup->stop();
			startup->deleteLater();
			QSettings settings;
			const bool openOnStartup = qEnvironmentVariableIsSet("ALIS_AUTOSTART")
				? qEnvironmentVariableIntValue("ALIS_AUTOSTART") == 1
				: settings.value(QStringLiteral("ALiS/workspace/openOnStartup"), true).toBool();
			if (openOnStartup)
			{
				if (QToolBar* toolbar = m_app->getMainWindow()->findChild<QToolBar*>(QStringLiteral("ALiS")))
					toolbar->show();
				showWorkbench();
			}
		});
		startup->start();
	}
}

QList<QAction*> ALiSPlugin::getActions()
{
	if (!m_action)
	{
		m_action = new QAction(QIcon(QStringLiteral(":/CC/plugin/ALiS/images/workspace.svg")), getName(), this);
		m_action->setToolTip(QStringLiteral("Open the ALiS workspace"));
		connect(m_action, &QAction::triggered, this, &ALiSPlugin::showWorkbench);
	}
	if (!m_annotatorAction)
	{
		m_annotatorAction = new QAction(QIcon(QStringLiteral(":/CC/plugin/ALiS/images/annotator.svg")), QStringLiteral("ALiS Annotator"), this);
		m_annotatorAction->setToolTip(QStringLiteral("Open Annotation Studio as an independent tool"));
		connect(m_annotatorAction, &QAction::triggered, this, &ALiSPlugin::showAnnotator);
	}
	if (!m_settingsAction)
	{
		m_settingsAction = new QAction(QIcon(QStringLiteral(":/CC/plugin/ALiS/images/settings.svg")), QStringLiteral("ALiS Settings"), this);
		m_settingsAction->setToolTip(QStringLiteral("Repositories, catalog, credits, bibliography, changes and updates"));
		connect(m_settingsAction, &QAction::triggered, this, &ALiSPlugin::showSettings);
	}
	return {m_action, m_annotatorAction, m_settingsAction};
}

bool ALiSPlugin::start()
{
	if (!m_app)
	{
		return false;
	}

	showWorkbench();
	return true;
}

void ALiSPlugin::onNewSelection(const ccHObject::Container& selectedEntities)
{
	syncSelection(selectedEntities);
}

void ALiSPlugin::showWorkbench()
{
	if (!m_app)
	{
		return;
	}

	ensureDock();
	syncSelection(m_app->getSelectedEntities());
	m_workspaceDock->show();
	m_workspaceDock->raise();
	dispToConsole(QStringLiteral("[ALiS] Archaeological LiDAR Studio opened."));
}

void ALiSPlugin::showAnnotator()
{
	if (!m_app) return;
	ensureDock();
	syncSelection(m_app->getSelectedEntities());
	m_workspaceDock->showAnnotationWorkspace();
	m_workspaceDock->show();
	m_workspaceDock->raise();
	if (!m_workspaceController->selectedCloud())
	{
		dispToConsole(QStringLiteral("[ALiS Annotator] Select one point cloud in the DB Tree."), ccMainAppInterface::WRN_CONSOLE_MESSAGE);
		return;
	}
	m_workspaceController->openAnnotationStudio();
	dispToConsole(QStringLiteral("[ALiS Annotator] Standalone Annotation Studio requested."));
}

void ALiSPlugin::showSettings()
{
	if (!m_app) return;
	ensureDock();
	QSettings settings;
	QString fallback = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
		.filePath(QStringLiteral("ALiS/repository"));
	const QString legacyFallback = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
		.filePath(QStringLiteral("qArchaeoLiDAR/repository"));
	if (!QDir(fallback).exists() && QDir(legacyFallback).exists()) fallback = legacyFallback;
	QString repository = settings.value(QStringLiteral("ALiS/modelRepository")).toString();
	if (repository.isEmpty()) repository = settings.value(QStringLiteral("qArchaeoLiDAR/modelRepository"), fallback).toString();
	alis::SettingsDialog dialog(repository, m_app->getMainWindow());
	if (dialog.exec() != QDialog::Accepted) return;
	settings.setValue(QStringLiteral("ALiS/modelRepository"), dialog.repositoryPath());
	settings.setValue(QStringLiteral("ALiS/catalog/automaticRecommendations"), dialog.automaticRecommendations());
	m_workspaceDock->setModelRepositoryPath(dialog.repositoryPath());
	dispToConsole(QStringLiteral("[ALiS] Settings saved; intelligent catalog refreshed."));
}

void ALiSPlugin::ensureDock()
{
	if (m_workspaceDock || !m_app)
	{
		return;
	}

	QMainWindow* mainWindow = m_app->getMainWindow();
	m_workspaceDock = new alis::WorkspaceDock(mainWindow);
	mainWindow->addDockWidget(Qt::RightDockWidgetArea, m_workspaceDock);
	m_workspaceController = new alis::WorkspaceController(m_app, m_workspaceDock, this);
	connect(m_workspaceDock, &QObject::destroyed, this, [this]() { m_workspaceDock = nullptr; });
}

void ALiSPlugin::syncSelection(const ccHObject::Container& selectedEntities)
{
	if (!m_workspaceController)
	{
		return;
	}

	ccPointCloud* cloud = nullptr;
	if (selectedEntities.size() == 1 && selectedEntities.front()->isA(CC_TYPES::POINT_CLOUD))
	{
		cloud = static_cast<ccPointCloud*>(selectedEntities.front());
	}
	m_workspaceController->selectCloud(cloud);
}
