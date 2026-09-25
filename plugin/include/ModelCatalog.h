// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace alis
{
	struct ModelRecommendation
	{
		QString artifactId;
		QString classifierId;
		QString modelPath;
		QString status;
		QString explanation;
		double score = 0.0;
		bool compatible = false;
	};

	struct ModelCatalogSummary
	{
		QString repositoryPath;
		QString error;
		int datasetCount = 0;
		int modelCount = 0;
		QList<ModelRecommendation> recommendations;
	};

	struct PretrainedModelEntry
	{
		QString id;
		QString name;
		QString provider;
		QString category;
		QString domain;
		QString task;
		QString taxonomy;
		QString output;
		QString pipeline;
		QString license;
		QString sourceUrl;
		QString manifestPath;
		QString checkpointPath;
		QString adapterId;
		QStringList requiredDimensions;
		bool installed = false;
		bool checkpointReady = false;
		bool adapterReady = false;
		bool oneClickInstall = false;
		int suitabilityScore = -1;
		QString suitability;
		QString suitabilityReason;
		bool inputCompatible = false;
		QString inputReason;
		QString compatibility;
		QString installedVersion;
		QString latestVersion;
		QString updateStatus;
		QString updateReason;
		QString runtimeReason;
		QString blockerCode;
		QString setupGuide;
		QString action;
		bool providerKnown = false;
		bool runtimeDetected = false;
		bool adapterImplemented = false;
		bool updateAvailable = false;
		bool obsolete = false;
	};

	//! Rebuilds the persistent indexes and ranks reusable models for one cloud.
	class ModelCatalog
	{
	public:
		static ModelCatalogSummary refresh(const QString& repositoryPath,
		                                   const QJsonObject& cloudProfile,
		                                   const QStringList& featureNames,
		                                   const QString& targetDomain = QStringLiteral("asprs_working"));
		//! Lists curated official sources plus locally registered packages.
		static QList<PretrainedModelEntry> pretrainedModels(const QString& repositoryPath,
		                                                       const QJsonObject& cloudProfile = {});
		//! Validates and registers a data-only package manifest. No provider code is executed.
		static bool registerPretrainedManifest(const QString& repositoryPath,
		                                      const QString& sourceManifest,
		                                      QString& installedManifest,
		                                      QString& error);
	};
}
