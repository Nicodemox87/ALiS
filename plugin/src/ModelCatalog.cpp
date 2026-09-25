// SPDX-License-Identifier: GPL-2.0-or-later
#include "ModelCatalog.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHash>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>
#include <cmath>

namespace
{
	QJsonObject readObject(const QString& path)
	{
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly)) return {};
		const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
		return document.isObject() ? document.object() : QJsonObject{};
	}

	QHash<QString, QJsonObject> indexedEntries(const QString& path)
	{
		QHash<QString, QJsonObject> result;
		const QJsonObject root = readObject(path);
		for (const QJsonValue& value : root.value(QStringLiteral("entries")).toArray())
		{
			const QJsonObject object = value.toObject();
			const QString id = object.value(QStringLiteral("id")).toString();
			if (!id.isEmpty()) result.insert(id, object);
		}
		return result;
	}

	bool writeArray(const QString& path, const QJsonArray& value)
	{
		QSaveFile file(path);
		return file.open(QIODevice::WriteOnly)
			&& file.write(QJsonDocument(value).toJson(QJsonDocument::Indented)) >= 0
			&& file.commit();
	}

	QStringList stringArray(const QJsonValue& value)
	{
		QStringList result;
		for (const QJsonValue& item : value.toArray()) result.push_back(item.toString());
		return result;
	}

	double similarity(double current, double trained)
	{
		if (!(current > 0.0) || !(trained > 0.0)) return -1.0;
		return std::exp(-std::abs(std::log(current / trained)));
	}

	double number(const QJsonObject& object, const QStringList& keys)
	{
		for (const QString& key : keys)
		{
			const double value = object.value(key).toDouble(-1.0);
			if (value >= 0.0 && std::isfinite(value)) return value;
		}
		return -1.0;
	}

	QJsonObject trainingProfile(const QJsonObject& model)
	{
		QJsonObject profile = model.value(QStringLiteral("training_profile")).toObject();
		if (!profile.isEmpty()) return profile;
		const QJsonObject metadata = model.value(QStringLiteral("training_metadata")).toObject();
		const QJsonObject source = metadata.value(QStringLiteral("source_cloud")).toObject();
		return source.value(QStringLiteral("cloud_profile")).toObject();
	}

	alis::PretrainedModelEntry pretrainedFromJson(const QJsonObject& object, const QString& manifestPath)
	{
		alis::PretrainedModelEntry entry;
		entry.id = object.value(QStringLiteral("id")).toString();
		entry.name = object.value(QStringLiteral("name")).toString(entry.id);
		entry.provider = object.value(QStringLiteral("provider")).toString();
		entry.category = object.value(QStringLiteral("category")).toString(QStringLiteral("Semantic classification"));
		entry.domain = object.value(QStringLiteral("domain")).toString();
		entry.task = object.value(QStringLiteral("task")).toString();
		entry.taxonomy = object.value(QStringLiteral("taxonomy")).toString();
		entry.output = object.value(QStringLiteral("output")).toString(QStringLiteral("Per-point semantic labels"));
		entry.pipeline = object.value(QStringLiteral("pipeline")).toString();
		entry.license = object.value(QStringLiteral("license")).toString();
		entry.sourceUrl = object.value(QStringLiteral("source_url")).toString();
		entry.adapterId = object.value(QStringLiteral("adapter")).toString();
		entry.requiredDimensions = stringArray(object.value(QStringLiteral("required_dimensions")));
		entry.manifestPath = QDir::toNativeSeparators(manifestPath);
		QString checkpoint = object.value(QStringLiteral("checkpoint")).toString();
		if (!checkpoint.isEmpty() && QFileInfo(checkpoint).isRelative()) checkpoint = QFileInfo(manifestPath).dir().filePath(checkpoint);
		entry.checkpointPath = QDir::toNativeSeparators(checkpoint);
		entry.installed = !manifestPath.isEmpty();
		entry.checkpointReady = QFileInfo::exists(checkpoint);
		// Adapters are deliberately allow-listed. Inference adapters are added only after end-to-end validation.
		entry.adapterReady = false;
		entry.oneClickInstall = object.value(QStringLiteral("one_click_install")).toBool(false);
		entry.installedVersion = object.value(QStringLiteral("catalog_version")).toString();
		entry.latestVersion = entry.installedVersion;
		entry.obsolete = object.value(QStringLiteral("obsolete")).toBool(false);
		entry.compatibility = !entry.installed ? QStringLiteral("Available online")
			: (!entry.checkpointReady ? QStringLiteral("Checkpoint missing") : QStringLiteral("Provider adapter pending"));
		return entry;
	}

	QJsonObject curatedPretrained(const QString& id, const QString& name, const QString& provider,
		const QString& task, const QString& taxonomy, const QString& license, const QString& url,
		const QString& adapter, const QStringList& dimensions, const QString& category,
		const QString& domain, const QString& output, const QString& pipeline, bool oneClick)
	{
		QJsonArray required; for (const QString& dimension : dimensions) required.append(dimension);
		return QJsonObject{{QStringLiteral("schema"), QStringLiteral("alis-pretrained-model/1.0")},
			{QStringLiteral("id"), id}, {QStringLiteral("name"), name}, {QStringLiteral("provider"), provider},
			{QStringLiteral("task"), task}, {QStringLiteral("taxonomy"), taxonomy},
			{QStringLiteral("category"), category}, {QStringLiteral("domain"), domain},
			{QStringLiteral("output"), output}, {QStringLiteral("pipeline"), pipeline},
			{QStringLiteral("license"), license}, {QStringLiteral("source_url"), url},
			{QStringLiteral("adapter"), adapter}, {QStringLiteral("required_dimensions"), required},
			{QStringLiteral("one_click_install"), oneClick},
			{QStringLiteral("catalog_version"), QStringLiteral("2026.09.04.1")},
			{QStringLiteral("obsolete"), false}};
	}

	QStringList normalizedFields(const QJsonObject& profile)
	{
		QStringList result;
		for (const QJsonValue& value : profile.value(QStringLiteral("scalarFields")).toArray())
		{
			QString field = value.toString().toLower();
			field.remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
			result << field;
		}
		return result;
	}

	void evaluateSuitability(alis::PretrainedModelEntry& entry, const QJsonObject& profile)
	{
		if (profile.isEmpty() || profile.value(QStringLiteral("points")).toDouble() <= 0)
		{
			entry.suitability = QStringLiteral("Unknown");
			entry.suitabilityReason = QStringLiteral("Select a cloud to evaluate acquisition domain and input channels.");
			entry.suitabilityScore = -1;
			return;
		}
		const QStringList fields = normalizedFields(profile);
		auto hasAny = [&fields](const QStringList& aliases)
		{
			for (const QString& field : fields) for (const QString& alias : aliases) if (field.contains(alias)) return true;
			return false;
		};
		const bool hasReturns = profile.value(QStringLiteral("validReturnFraction")).toDouble() > 0.50;
		const bool hasIntensity = hasAny({QStringLiteral("intensity"), QStringLiteral("reflectance"), QStringLiteral("amplitude")});
		const bool hasRgb = profile.value(QStringLiteral("hasColors")).toBool();
		const bool hasNir = hasAny({QStringLiteral("nir"), QStringLiteral("nearinfrared"), QStringLiteral("infrared")});
		const bool georeferenced = !profile.value(QStringLiteral("projection")).toString().isEmpty();
		const double density = profile.value(QStringLiteral("nnDensity2D")).toDouble(profile.value(QStringLiteral("bboxDensity")).toDouble());
		int score = 50;
		QStringList reasons;
		if (entry.domain == QStringLiteral("als"))
		{
			score = 72;
			if (georeferenced) { score += 8; reasons << QStringLiteral("georeferenced survey"); }
			if (hasReturns) { score += 8; reasons << QStringLiteral("usable return metadata"); }
			if (density > 0.0 && density < 1.0) { score -= 18; reasons << QStringLiteral("very sparse for this ALS model"); }
			else if (density >= 1.0 && density <= 150.0) { score += 5; reasons << QStringLiteral("plausible ALS density"); }
		}
		else if (entry.domain == QStringLiteral("forest"))
		{
			score = 62;
			reasons << QStringLiteral("appropriate only after Ground/non-ground and vegetation isolation");
			if (hasReturns) score += 8;
		}
		else if (entry.domain == QStringLiteral("archaeology_2_5d"))
		{
			score = georeferenced ? 78 : 64;
			reasons << QStringLiteral("requires a DTM-derived raster workflow, not raw-point inference");
		}
		else if (entry.domain == QStringLiteral("foundation"))
		{
			score = 58;
			reasons << QStringLiteral("transfer encoder; fine-tuning or an audited task head is required");
		}
		else if (entry.domain == QStringLiteral("mobile_mapping"))
		{
			score = 32;
			reasons << QStringLiteral("trained on street-level mobile mapping, not airborne terrain");
			if (hasIntensity || hasRgb) score += 5;
		}
		else if (entry.domain == QStringLiteral("indoor"))
		{
			score = 18;
			reasons << QStringLiteral("indoor taxonomy and geometry do not match an airborne survey");
		}
		else if (entry.domain == QStringLiteral("automotive"))
		{
			score = 8;
			reasons << QStringLiteral("sensor-centred spinning-LiDAR range image required; georeferenced ALS is incompatible");
		}
		if (entry.id == QStringLiteral("ignf-fractal-randlanet-7cl"))
		{
			if (hasRgb) score += 4; else { score -= 10; reasons << QStringLiteral("RGB absent"); }
			if (!hasIntensity) { score -= 10; reasons << QStringLiteral("reflectance/intensity absent"); }
			if (!hasNir) { score -= 25; reasons << QStringLiteral("required NIR channel absent"); }
		}
		if ((entry.id.startsWith(QStringLiteral("spt-")) || entry.id.startsWith(QStringLiteral("ezsp-"))) && !hasIntensity)
		{ score -= 12; reasons << QStringLiteral("intensity is missing"); }
		entry.suitabilityScore = std::max(0, std::min(100, score));
		entry.suitability = entry.suitabilityScore >= 75 ? QStringLiteral("Recommended")
			: (entry.suitabilityScore >= 45 ? QStringLiteral("Review") : QStringLiteral("Not suitable"));
		entry.suitabilityReason = reasons.isEmpty() ? QStringLiteral("No decisive acquisition metadata available.") : reasons.join(QStringLiteral("; "));

		QStringList missingInputs;
		for (const QString& required : entry.requiredDimensions)
		{
			const QString key = required.toLower();
			if ((key.contains(QStringLiteral("echo")) || key.contains(QStringLiteral("return"))) && !hasReturns)
				missingInputs << required;
			else if ((key.contains(QStringLiteral("intensity")) || key.contains(QStringLiteral("reflectance"))) && !hasIntensity)
				missingInputs << required;
			else if ((key == QStringLiteral("rgb") || key.contains(QStringLiteral("colour"))) && !hasRgb)
				missingInputs << required;
			else if ((key == QStringLiteral("nir") || key.contains(QStringLiteral("infrared"))) && !hasNir)
				missingInputs << required;
			else if (key.contains(QStringLiteral("sensor-centred")) || key.contains(QStringLiteral("scan geometry")))
				missingInputs << required;
			else if (key.contains(QStringLiteral("dtm-derived")))
				missingInputs << required;
			else if (key.contains(QStringLiteral("normals")) && !hasAny({QStringLiteral("normal"), QStringLiteral("nx"), QStringLiteral("ny"), QStringLiteral("nz")}))
				missingInputs << required;
		}
		entry.inputCompatible = missingInputs.isEmpty();
		entry.inputReason = entry.inputCompatible
			? QStringLiteral("All directly verifiable input channels are present; semantic meaning still requires preflight review.")
			: QStringLiteral("Missing or incompatible input: %1").arg(missingInputs.join(QStringLiteral(", ")));
	}
}

namespace alis
{
	ModelCatalogSummary ModelCatalog::refresh(const QString& repositoryPath,
	                                         const QJsonObject& cloudProfile,
	                                         const QStringList& featureNames,
	                                         const QString& targetDomain)
	{
		ModelCatalogSummary result;
		result.repositoryPath = QDir::fromNativeSeparators(repositoryPath.trimmed());
		if (result.repositoryPath.isEmpty()) { result.error = QStringLiteral("Choose a model repository."); return result; }
		QDir root(result.repositoryPath);
		if (!root.exists() && !QDir().mkpath(result.repositoryPath))
		{
			result.error = QStringLiteral("Cannot create repository: %1").arg(result.repositoryPath); return result;
		}
		for (const QString& folder : {QStringLiteral("datasets"), QStringLiteral("models"), QStringLiteral("runs"), QStringLiteral("catalog")})
			root.mkpath(folder);

		QJsonArray modelIndex;
		QDirIterator modelFiles(root.filePath(QStringLiteral("models")), {QStringLiteral("manifest.json")}, QDir::Files, QDirIterator::Subdirectories);
		while (modelFiles.hasNext())
		{
			const QString manifestPath = modelFiles.next();
			QJsonObject model = readObject(manifestPath);
			if (model.value(QStringLiteral("schema")).toString() != QStringLiteral("qal-model-entry/1.0")) continue;
			model.insert(QStringLiteral("manifest"), QDir::toNativeSeparators(manifestPath));
			modelIndex.append(model);
		}
		// Keep legacy/custom-output models that were registered outside models/**.
		const QString existingPath = root.filePath(QStringLiteral("catalog/models.json"));
		QFile existingFile(existingPath);
		if (existingFile.open(QIODevice::ReadOnly))
		{
			const QJsonArray existing = QJsonDocument::fromJson(existingFile.readAll()).array();
			for (const QJsonValue& value : existing)
			{
				const QJsonObject candidate = value.toObject();
				bool present = false;
				for (const QJsonValue& indexed : modelIndex)
					if (indexed.toObject().value(QStringLiteral("artifact_id")) == candidate.value(QStringLiteral("artifact_id"))) { present = true; break; }
				if (!present) modelIndex.append(candidate);
			}
			existingFile.close();
		}

		QJsonArray datasetIndex;
		QDirIterator datasetFiles(root.filePath(QStringLiteral("datasets")), {QStringLiteral("manifest.json")}, QDir::Files, QDirIterator::Subdirectories);
		while (datasetFiles.hasNext())
		{
			const QString manifestPath = datasetFiles.next();
			const QJsonObject manifest = readObject(manifestPath);
			if (manifest.value(QStringLiteral("schema")).toString() != QStringLiteral("qal-ml-dataset/1.0")) continue;
			datasetIndex.append(QJsonObject{{QStringLiteral("manifest"), QDir::toNativeSeparators(manifestPath)},
				{QStringLiteral("created_utc"), manifest.value(QStringLiteral("created_utc"))},
				{QStringLiteral("target_domain"), manifest.value(QStringLiteral("target_domain"))},
				{QStringLiteral("point_count"), manifest.value(QStringLiteral("point_count"))},
				{QStringLiteral("feature_names"), manifest.value(QStringLiteral("feature_names"))},
				{QStringLiteral("source"), manifest.value(QStringLiteral("source"))}});
		}
		result.datasetCount = datasetIndex.size();
		result.modelCount = modelIndex.size();
		if (!writeArray(root.filePath(QStringLiteral("catalog/models.json")), modelIndex)
			|| !writeArray(root.filePath(QStringLiteral("catalog/datasets.json")), datasetIndex))
		{
			result.error = QStringLiteral("The catalog indexes could not be updated."); return result;
		}

		for (const QJsonValue& value : modelIndex)
		{
			const QJsonObject model = value.toObject();
			ModelRecommendation item;
			item.artifactId = model.value(QStringLiteral("artifact_id")).toString();
			item.classifierId = model.value(QStringLiteral("classifier_id")).toString(QStringLiteral("unknown"));
			item.modelPath = QDir::toNativeSeparators(model.value(QStringLiteral("model")).toString());
			QStringList reasons;
			const QStringList trainedFeatures = stringArray(model.value(QStringLiteral("feature_names")));
			const bool fileReady = QFileInfo::exists(item.modelPath);
			const bool featureMatch = model.value(QStringLiteral("classifier_id")).toString() == QStringLiteral("pointnet")
				|| featureNames.isEmpty() || trainedFeatures == featureNames;
			const bool domainMatch = model.value(QStringLiteral("target_domain")).toString() == targetDomain;
			item.compatible = fileReady && featureMatch && domainMatch;
			if (!fileReady) reasons << QStringLiteral("model file missing");
			if (!featureMatch) reasons << QStringLiteral("feature set or order differs");
			if (!domainMatch) reasons << QStringLiteral("classification domain differs");

			const QJsonObject trained = trainingProfile(model);
			double domainSum = 0.0; int domainCount = 0;
			for (const QPair<QStringList, QStringList>& pair : {
				QPair<QStringList, QStringList>{{QStringLiteral("nnMedian"), QStringLiteral("nominalSpacing")}, {QStringLiteral("nnMedian"), QStringLiteral("nominalSpacing")}},
				QPair<QStringList, QStringList>{{QStringLiteral("nnDensity2D"), QStringLiteral("bboxDensity")}, {QStringLiteral("nnDensity2D"), QStringLiteral("bboxDensity")}}})
			{
				const double match = similarity(number(cloudProfile, pair.first), number(trained, pair.second));
				if (match >= 0.0) { domainSum += match; ++domainCount; }
			}
			const double currentReturns = number(cloudProfile, {QStringLiteral("validReturnFraction")});
			const double trainedReturns = number(trained, {QStringLiteral("validReturnFraction")});
			if (currentReturns >= 0.0 && trainedReturns >= 0.0)
			{
				domainSum += std::max(0.0, 1.0 - std::abs(currentReturns - trainedReturns)); ++domainCount;
			}
			const double domainScore = domainCount ? domainSum / domainCount : 0.45;
			const double quality = std::max(0.0, std::min(1.0, model.value(QStringLiteral("balanced_accuracy")).toDouble(0.5)));
			item.score = item.compatible ? 100.0 * (0.55 * quality + 0.45 * domainScore) : 0.0;
			if (!item.compatible) item.status = QStringLiteral("Incompatible");
			else if (domainCount >= 2 && domainScore >= 0.70) item.status = QStringLiteral("Ready");
			else { item.status = QStringLiteral("Review"); reasons << (domainCount < 2 ? QStringLiteral("training cloud profile incomplete") : QStringLiteral("acquisition differs from training")); }
			if (model.value(QStringLiteral("validation_strategy")).toString().contains(QStringLiteral("external"))) reasons << QStringLiteral("externally validated");
			item.explanation = reasons.isEmpty() ? QStringLiteral("Features, domain and acquisition profile are compatible.") : reasons.join(QStringLiteral("; "));
			result.recommendations.push_back(item);
		}
		std::sort(result.recommendations.begin(), result.recommendations.end(), [](const ModelRecommendation& a, const ModelRecommendation& b)
		{
			if (a.compatible != b.compatible) return a.compatible > b.compatible;
			return a.score > b.score;
		});
		return result;
	}

	QList<PretrainedModelEntry> ModelCatalog::pretrainedModels(const QString& repositoryPath, const QJsonObject& cloudProfile)
	{
		Q_UNUSED(repositoryPath);
		Q_UNUSED(cloudProfile);
		return {}; // External pre-trained DL is deferred; supervised catalog is unchanged.
	}

	bool ModelCatalog::registerPretrainedManifest(const QString& repositoryPath, const QString& sourceManifest,
		QString& installedManifest, QString& error)
	{
		const QString rootPath = QDir::fromNativeSeparators(repositoryPath.trimmed());
		if (rootPath.isEmpty()) { error = QStringLiteral("Choose an ALiS repository first."); return false; }
		const QJsonObject object = readObject(sourceManifest);
		if (object.value(QStringLiteral("schema")).toString() != QStringLiteral("alis-pretrained-model/1.0"))
		{ error = QStringLiteral("Unsupported manifest schema. Expected alis-pretrained-model/1.0."); return false; }
		QString id = object.value(QStringLiteral("id")).toString().trimmed();
		id.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
		if (id.isEmpty() || object.value(QStringLiteral("name")).toString().trimmed().isEmpty()
			|| object.value(QStringLiteral("provider")).toString().trimmed().isEmpty()
			|| object.value(QStringLiteral("source_url")).toString().trimmed().isEmpty()
			|| object.value(QStringLiteral("license")).toString().trimmed().isEmpty())
		{ error = QStringLiteral("Manifest must declare id, name, provider, source_url and license."); return false; }
		QJsonObject normalized = object;
		const QString checkpoint = object.value(QStringLiteral("checkpoint")).toString();
		if (!checkpoint.isEmpty())
		{
			const QString resolved = QFileInfo(checkpoint).isRelative() ? QFileInfo(sourceManifest).dir().filePath(checkpoint) : checkpoint;
			normalized.insert(QStringLiteral("checkpoint"), QDir::toNativeSeparators(QFileInfo(resolved).absoluteFilePath()));
		}
		normalized.insert(QStringLiteral("registered_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
		const QString folder = QDir(rootPath).filePath(QStringLiteral("models/pretrained/%1").arg(id));
		if (!QDir().mkpath(folder)) { error = QStringLiteral("Cannot create pretrained package folder."); return false; }
		installedManifest = QDir(folder).filePath(QStringLiteral("manifest.json"));
		QSaveFile output(installedManifest);
		if (!output.open(QIODevice::WriteOnly) || output.write(QJsonDocument(normalized).toJson(QJsonDocument::Indented)) < 0 || !output.commit())
		{ error = QStringLiteral("Cannot register the package manifest."); return false; }
		installedManifest = QDir::toNativeSeparators(installedManifest);
		return true;
	}
}
