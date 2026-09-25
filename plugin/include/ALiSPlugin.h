// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <ccStdPluginInterface.h>

class QAction;

namespace alis
{
	class WorkspaceController;
	class WorkspaceDock;
}

class ALiSPlugin final : public QObject, public ccStdPluginInterface
{
	Q_OBJECT
	Q_INTERFACES(ccPluginInterface ccStdPluginInterface)
	Q_PLUGIN_METADATA(IID "org.alis.cloudcompare.plugin" FILE "../info.json")

public:
	explicit ALiSPlugin(QObject* parent = nullptr);
	~ALiSPlugin() override;

	void setMainAppInterface(ccMainAppInterface* app) override;
	void onNewSelection(const ccHObject::Container& selectedEntities) override;
	QList<QAction*> getActions() override;
	bool start() override;

private:
	void showWorkbench();
	void showAnnotator();
	void showSettings();
	void ensureDock();
	void syncSelection(const ccHObject::Container& selectedEntities);

	QAction* m_action = nullptr;
	QAction* m_annotatorAction = nullptr;
	QAction* m_settingsAction = nullptr;
	alis::WorkspaceDock* m_workspaceDock = nullptr;
	alis::WorkspaceController* m_workspaceController = nullptr;
};
