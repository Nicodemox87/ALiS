// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDialog>
#include <QStringList>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QTableWidget;
class QTabWidget;
class QWidget;

namespace alis
{
	class ModelResultsDialog final : public QDialog
	{
	public:
		explicit ModelResultsDialog(const QStringList& reportPaths, QWidget* parent = nullptr);

		static QString reportPathForModel(const QString& modelPath);

	private:
		void rebuildComparison();
		void loadReport(int index);
		void clearReport(const QString& message);
		QString currentReportPath() const;

		QStringList m_reportPaths;
		QComboBox* m_reportSelector = nullptr;
		QLabel* m_status = nullptr;
		QLabel* m_overview = nullptr;
		QTableWidget* m_comparison = nullptr;
		QTableWidget* m_perClass = nullptr;
		QTableWidget* m_confusion = nullptr;
		QTableWidget* m_learning = nullptr;
		QTableWidget* m_importance = nullptr;
		QPlainTextEdit* m_technical = nullptr;
		QTabWidget* m_tabs = nullptr;
	};
}
