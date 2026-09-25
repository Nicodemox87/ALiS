// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDialog>

class QCheckBox;
class QLabel;
class QLineEdit;

namespace alis
{
	class SettingsDialog final : public QDialog
	{
		Q_OBJECT
	public:
		explicit SettingsDialog(const QString& repositoryPath, QWidget* parent = nullptr);
		QString repositoryPath() const;
		bool automaticRecommendations() const;

	private:
		void refreshCatalogSummary();
		QLineEdit* m_repositoryEdit = nullptr;
		QCheckBox* m_autoRecommendations = nullptr;
		QLabel* m_catalogStatus = nullptr;
	};
}
