// SPDX-License-Identifier: GPL-2.0-or-later
// Explicit, non-GUI scientific/performance runner for the derived cloud2 tile.
// This is intentionally not a fast default unit test.

#include "FeatureEngine.h"
#include "GroundFilter.h"
#include "TerrainEngine.h"
#include "WorkspaceLogic.h"

#include <ccPointCloud.h>

#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSaveFile>
#include <QSysInfo>
#include <QTextStream>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{
	constexpr char FixtureMagic[8] = {'Q', 'A', 'L', 'F', 'X', 'Y', 'Z', '1'};
	constexpr std::uint32_t FixtureVersion = 1;
	constexpr std::uint32_t LittleEndianTag = 0x01020304;
	constexpr std::uint32_t UnitMetre = 1;
	constexpr qint64 FixtureHeaderBytes = 96;

	struct FixtureMetadata
	{
		std::uint64_t pointCount = 0;
		std::int32_t epsg = 0;
		std::uint32_t unitCode = 0;
		std::array<double, 3> globalShift = {{0.0, 0.0, 0.0}};
		double globalScale = 1.0;
		QString sourceSha256;
		QString fixtureSha256;
	};

	struct CsvRow
	{
		QString phase;
		QString item;
		QString radius;
		QString elapsedMilliseconds;
		QString totalCount;
		QString validCount;
		QString invalidCount;
		QString minimum;
		QString maximum;
		QString mean;
		QString standardDeviation;
		QString sha256;
	};

	QString number(double value, int precision = 15)
	{
		return std::isfinite(value) ? QLocale::c().toString(value, 'g', precision) : QString();
	}

	QJsonValue jsonNumber(double value)
	{
		return std::isfinite(value) ? QJsonValue(value) : QJsonValue(QJsonValue::Null);
	}

	QString unsignedNumber(std::uint64_t value)
	{
		return QString::number(static_cast<qulonglong>(value));
	}

	bool readExact(QFile& file, void* destination, qint64 byteCount, QString& error)
	{
		char* output = static_cast<char*>(destination);
		qint64 total = 0;
		while (total < byteCount)
		{
			const qint64 count = file.read(output + total, byteCount - total);
			if (count <= 0)
			{
				error = QStringLiteral("Unexpected end of fixture at byte %1.").arg(file.pos());
				return false;
			}
			total += count;
		}
		return true;
	}

	template <typename T>
	bool readPrimitive(QFile& file, T& value, QString& error)
	{
		return readExact(file, &value, static_cast<qint64>(sizeof(T)), error);
	}

	QString sha256File(const QString& path, QString& error)
	{
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly))
		{
			error = file.errorString();
			return QString();
		}
		QCryptographicHash hash(QCryptographicHash::Sha256);
		while (!file.atEnd())
		{
			const QByteArray block = file.read(8 * 1024 * 1024);
			if (block.isEmpty() && file.error() != QFile::NoError)
			{
				error = file.errorString();
				return QString();
			}
			hash.addData(block);
		}
		return QString::fromLatin1(hash.result().toHex().toUpper());
	}

	template <typename T>
	QString rawVectorSha256(const std::vector<T>& values)
	{
		QCryptographicHash hash(QCryptographicHash::Sha256);
		if (!values.empty())
		{
			const std::size_t byteCount = values.size() * sizeof(T);
			const char* data = reinterpret_cast<const char*>(values.data());
			std::size_t offset = 0;
			while (offset < byteCount)
			{
				const int chunk = static_cast<int>(std::min<std::size_t>(byteCount - offset, 32 * 1024 * 1024));
				hash.addData(data + offset, chunk);
				offset += static_cast<std::size_t>(chunk);
			}
		}
		return QString::fromLatin1(hash.result().toHex().toUpper());
	}

	QString maskSha256(const std::vector<bool>& mask)
	{
		QByteArray packed(static_cast<int>((mask.size() + 7) / 8), '\0');
		for (std::size_t index = 0; index < mask.size(); ++index)
		{
			if (mask[index])
			{
				packed[static_cast<int>(index / 8)] = static_cast<char>(
					static_cast<unsigned char>(packed[static_cast<int>(index / 8)]) | (1u << (index % 8)));
			}
		}
		return QString::fromLatin1(QCryptographicHash::hash(packed, QCryptographicHash::Sha256).toHex().toUpper());
	}

	bool loadFixture(const QString& path,
	                 std::unique_ptr<ccPointCloud>& cloud,
	                 FixtureMetadata& metadata,
	                 QString& error)
	{
		if (QSysInfo::ByteOrder != QSysInfo::LittleEndian)
		{
			error = QStringLiteral("The v1 fixture runner currently requires a little-endian host.");
			return false;
		}
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly))
		{
			error = file.errorString();
			return false;
		}
		char magic[8] = {};
		std::uint32_t version = 0;
		std::uint32_t endianTag = 0;
		std::array<unsigned char, 32> sourceHash = {{0}};
		if (!readExact(file, magic, 8, error)
		    || !readPrimitive(file, version, error)
		    || !readPrimitive(file, endianTag, error)
		    || !readPrimitive(file, metadata.pointCount, error)
		    || !readPrimitive(file, metadata.epsg, error)
		    || !readPrimitive(file, metadata.unitCode, error)
		    || !readPrimitive(file, metadata.globalShift[0], error)
		    || !readPrimitive(file, metadata.globalShift[1], error)
		    || !readPrimitive(file, metadata.globalShift[2], error)
		    || !readPrimitive(file, metadata.globalScale, error)
		    || !readExact(file, sourceHash.data(), static_cast<qint64>(sourceHash.size()), error))
		{
			return false;
		}
		if (std::memcmp(magic, FixtureMagic, 8) != 0 || version != FixtureVersion || endianTag != LittleEndianTag)
		{
			error = QStringLiteral("Unsupported or corrupt ALiS fixture header.");
			return false;
		}
		if (metadata.unitCode != UnitMetre || metadata.epsg <= 0)
		{
			error = QStringLiteral("Fixture does not provide an EPSG CRS with explicitly confirmed metre units.");
			return false;
		}
		if (metadata.pointCount == 0 || metadata.pointCount > std::numeric_limits<unsigned>::max())
		{
			error = QStringLiteral("Fixture point count is empty or exceeds ccPointCloud capacity.");
			return false;
		}
		if (!std::isfinite(metadata.globalScale) || metadata.globalScale <= 0.0
		    || !std::all_of(metadata.globalShift.begin(), metadata.globalShift.end(),
		                   [](double value) { return std::isfinite(value); }))
		{
			error = QStringLiteral("Fixture contains an invalid Global Shift or Scale.");
			return false;
		}
		const std::uint64_t payloadBytes = metadata.pointCount * 3u * sizeof(float);
		if (payloadBytes > static_cast<std::uint64_t>(std::numeric_limits<qint64>::max())
		    || file.size() != FixtureHeaderBytes + static_cast<qint64>(payloadBytes))
		{
			error = QStringLiteral("Fixture size does not match its point count.");
			return false;
		}

		metadata.sourceSha256 = QString::fromLatin1(
			reinterpret_cast<const char*>(sourceHash.data()), static_cast<int>(sourceHash.size())).toLatin1().toHex().toUpper();
		metadata.fixtureSha256 = sha256File(path, error);
		if (metadata.fixtureSha256.isEmpty())
		{
			return false;
		}

		cloud.reset(new ccPointCloud(QStringLiteral("cloud2 real-tile E2E fixture")));
		if (!cloud->reserveThePointsTable(static_cast<unsigned>(metadata.pointCount)))
		{
			error = QStringLiteral("Not enough memory to reserve fixture points.");
			return false;
		}
		constexpr std::size_t RecordsPerBlock = 65536;
		std::vector<float> xyz(RecordsPerBlock * 3);
		std::uint64_t remaining = metadata.pointCount;
		while (remaining)
		{
			const std::size_t records = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, RecordsPerBlock));
			if (!readExact(file, xyz.data(), static_cast<qint64>(records * 3 * sizeof(float)), error))
			{
				cloud.reset();
				return false;
			}
			for (std::size_t index = 0; index < records; ++index)
			{
				const float x = xyz[index * 3];
				const float y = xyz[index * 3 + 1];
				const float z = xyz[index * 3 + 2];
				if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
				{
					error = QStringLiteral("Fixture contains a non-finite point.");
					cloud.reset();
					return false;
				}
				cloud->addPoint(CCVector3(x, y, z));
			}
			remaining -= records;
		}
		cloud->setGlobalShift(CCVector3d(metadata.globalShift[0], metadata.globalShift[1], metadata.globalShift[2]));
		cloud->setGlobalScale(metadata.globalScale);
		return cloud->size() == metadata.pointCount;
	}

	QJsonObject spacingToJson(const alis::SpacingSummary& spacing)
	{
		QJsonObject json;
		json.insert(QStringLiteral("valid"), spacing.valid);
		json.insert(QStringLiteral("requestedSamples"), static_cast<double>(spacing.requestedSamples));
		json.insert(QStringLiteral("sampledPoints"), static_cast<double>(spacing.sampledPoints));
		json.insert(QStringLiteral("positiveSpacingCount"), static_cast<double>(spacing.positiveSpacingCount));
		json.insert(QStringLiteral("duplicateSamples"), static_cast<double>(spacing.duplicateSamples));
		json.insert(QStringLiteral("unresolvedSamples"), static_cast<double>(spacing.unresolvedSamples));
		json.insert(QStringLiteral("minimum"), jsonNumber(spacing.minimum));
		json.insert(QStringLiteral("percentile10"), jsonNumber(spacing.percentile10));
		json.insert(QStringLiteral("percentile25"), jsonNumber(spacing.percentile25));
		json.insert(QStringLiteral("median"), jsonNumber(spacing.median));
		json.insert(QStringLiteral("percentile75"), jsonNumber(spacing.percentile75));
		json.insert(QStringLiteral("percentile90"), jsonNumber(spacing.percentile90));
		json.insert(QStringLiteral("maximum"), jsonNumber(spacing.maximum));
		json.insert(QStringLiteral("medianAbsoluteDeviation"), jsonNumber(spacing.medianAbsoluteDeviation));
		json.insert(QStringLiteral("estimatedDensity2D"), jsonNumber(spacing.estimatedDensity2D));
		json.insert(QStringLiteral("extentX"), jsonNumber(spacing.extentX));
		json.insert(QStringLiteral("extentY"), jsonNumber(spacing.extentY));
		json.insert(QStringLiteral("extentZ"), jsonNumber(spacing.extentZ));
		return json;
	}

	QJsonObject featureStatisticsToJson(const alis::FeatureStatistics& statistics)
	{
		QJsonObject json;
		json.insert(QStringLiteral("totalCount"), static_cast<double>(statistics.totalCount));
		json.insert(QStringLiteral("validCount"), static_cast<double>(statistics.validCount));
		json.insert(QStringLiteral("invalidCount"), static_cast<double>(statistics.invalidCount));
		json.insert(QStringLiteral("minimum"), jsonNumber(statistics.minimum));
		json.insert(QStringLiteral("maximum"), jsonNumber(statistics.maximum));
		json.insert(QStringLiteral("mean"), jsonNumber(statistics.mean));
		json.insert(QStringLiteral("variance"), jsonNumber(statistics.variance));
		json.insert(QStringLiteral("standardDeviation"), jsonNumber(statistics.standardDeviation));
		json.insert(QStringLiteral("percentile05"), jsonNumber(statistics.percentile05));
		json.insert(QStringLiteral("percentile25"), jsonNumber(statistics.percentile25));
		json.insert(QStringLiteral("median"), jsonNumber(statistics.median));
		json.insert(QStringLiteral("percentile75"), jsonNumber(statistics.percentile75));
		json.insert(QStringLiteral("percentile95"), jsonNumber(statistics.percentile95));
		json.insert(QStringLiteral("quantileSampleCount"), static_cast<double>(statistics.quantileSampleCount));
		json.insert(QStringLiteral("quantilesApproximate"), statistics.quantilesApproximate);
		return json;
	}

	QJsonObject terrainValueStatisticsToJson(const alis::TerrainValueStatistics& statistics)
	{
		QJsonObject json;
		json.insert(QStringLiteral("validCount"), static_cast<double>(statistics.validCount));
		json.insert(QStringLiteral("nodataCount"), static_cast<double>(statistics.nodataCount));
		json.insert(QStringLiteral("minimum"), jsonNumber(statistics.minimum));
		json.insert(QStringLiteral("maximum"), jsonNumber(statistics.maximum));
		json.insert(QStringLiteral("mean"), jsonNumber(statistics.mean));
		json.insert(QStringLiteral("standardDeviation"), jsonNumber(statistics.standardDeviation));
		return json;
	}

	QString csvEscape(const QString& value)
	{
		QString escaped = value;
		escaped.replace('"', QStringLiteral("\"\""));
		return QStringLiteral("\"") + escaped + QStringLiteral("\"");
	}

	bool writeResults(const QString& outputDirectory,
	                  const QJsonObject& report,
	                  const std::vector<CsvRow>& rows,
	                  QString& error)
	{
		if (!QDir().mkpath(outputDirectory))
		{
			error = QStringLiteral("Could not create output directory: %1").arg(outputDirectory);
			return false;
		}
		QSaveFile jsonFile(QDir(outputDirectory).filePath(QStringLiteral("real_tile_e2e.json")));
		if (!jsonFile.open(QIODevice::WriteOnly)
		    || jsonFile.write(QJsonDocument(report).toJson(QJsonDocument::Indented)) < 0
		    || !jsonFile.commit())
		{
			error = QStringLiteral("Could not atomically write JSON report: %1").arg(jsonFile.errorString());
			return false;
		}

		QSaveFile csvFile(QDir(outputDirectory).filePath(QStringLiteral("real_tile_e2e.csv")));
		if (!csvFile.open(QIODevice::WriteOnly | QIODevice::Text))
		{
			error = QStringLiteral("Could not write CSV report: %1").arg(csvFile.errorString());
			return false;
		}
		QTextStream stream(&csvFile);
		stream.setCodec("UTF-8");
		stream << "phase,item,radius,elapsed_ms,total_count,valid_count,invalid_or_nodata_count,minimum,maximum,mean,stddev,sha256\n";
		for (const CsvRow& row : rows)
		{
			stream << csvEscape(row.phase) << ',' << csvEscape(row.item) << ',' << csvEscape(row.radius) << ','
			       << csvEscape(row.elapsedMilliseconds) << ',' << csvEscape(row.totalCount) << ','
			       << csvEscape(row.validCount) << ',' << csvEscape(row.invalidCount) << ','
			       << csvEscape(row.minimum) << ',' << csvEscape(row.maximum) << ',' << csvEscape(row.mean) << ','
			       << csvEscape(row.standardDeviation) << ',' << csvEscape(row.sha256) << '\n';
		}
		stream.flush();
		if (stream.status() != QTextStream::Ok || !csvFile.commit())
		{
			error = QStringLiteral("Could not atomically commit CSV report: %1").arg(csvFile.errorString());
			return false;
		}
		return true;
	}

	QString argumentValue(const QStringList& arguments, const QString& name)
	{
		const int index = arguments.indexOf(name);
		return index >= 0 && index + 1 < arguments.size() ? arguments[index + 1] : QString();
	}
}

int main(int argc, char** argv)
{
	if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
	{
		qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("minimal"));
	}
	QApplication application(argc, argv);
	application.setApplicationName(QStringLiteral("ALiS real-tile E2E"));
	application.setQuitOnLastWindowClosed(false);

	const QStringList arguments = application.arguments();
	const QString fixturePath = argumentValue(arguments, QStringLiteral("--fixture"));
	const QString outputDirectory = argumentValue(arguments, QStringLiteral("--output-dir"));
	if (fixturePath.isEmpty() || outputDirectory.isEmpty())
	{
		std::cerr << "Usage: ALiS_real_tile_e2e --fixture <file.qalf> --output-dir <directory>\n";
		return EXIT_FAILURE;
	}

	QElapsedTimer totalTimer;
	totalTimer.start();
	QJsonObject report;
	report.insert(QStringLiteral("schema"), QStringLiteral("ALiS.real-tile-e2e/v1"));
	report.insert(QStringLiteral("startedUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
	report.insert(QStringLiteral("status"), QStringLiteral("running"));
	QJsonObject software;
	software.insert(QStringLiteral("qtRuntime"), QString::fromLatin1(qVersion()));
	software.insert(QStringLiteral("cloudCompareBaseline"),
	                QStringLiteral("v2.13.2@49dbbb662f296c7780aae717897c85b3cb3764ed"));
	software.insert(QStringLiteral("scalarTypeBytes"), static_cast<int>(sizeof(ScalarType)));
	software.insert(QStringLiteral("runner"), QStringLiteral("ALiS_real_tile_e2e"));
	report.insert(QStringLiteral("software"), software);

	std::vector<CsvRow> csvRows;
	auto finishFailure = [&](const QString& stage, const QString& message) -> int
	{
		report.insert(QStringLiteral("status"), QStringLiteral("failed"));
		report.insert(QStringLiteral("failedStage"), stage);
		report.insert(QStringLiteral("error"), message);
		report.insert(QStringLiteral("elapsedMilliseconds"), static_cast<double>(totalTimer.elapsed()));
		report.insert(QStringLiteral("finishedUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
		QString writeError;
		if (!writeResults(outputDirectory, report, csvRows, writeError))
		{
			std::cerr << "E2E failed at " << stage.toStdString() << ": " << message.toStdString()
			          << "; report write also failed: " << writeError.toStdString() << '\n';
		}
		else
		{
			std::cerr << "E2E failed at " << stage.toStdString() << ": " << message.toStdString() << '\n';
		}
		return EXIT_FAILURE;
	};

	QElapsedTimer phaseTimer;
	phaseTimer.start();
	FixtureMetadata fixture;
	std::unique_ptr<ccPointCloud> cloud;
	QString error;
	if (!loadFixture(fixturePath, cloud, fixture, error))
	{
		return finishFailure(QStringLiteral("fixture.load"), error);
	}
	const qint64 loadMilliseconds = phaseTimer.elapsed();
	QJsonObject fixtureJson;
	fixtureJson.insert(QStringLiteral("path"), QFileInfo(fixturePath).absoluteFilePath());
	fixtureJson.insert(QStringLiteral("fixtureSha256"), fixture.fixtureSha256);
	fixtureJson.insert(QStringLiteral("sourceLasSha256"), fixture.sourceSha256);
	fixtureJson.insert(QStringLiteral("pointCount"), static_cast<double>(fixture.pointCount));
	fixtureJson.insert(QStringLiteral("epsg"), fixture.epsg);
	fixtureJson.insert(QStringLiteral("linearUnit"), QStringLiteral("metre"));
	fixtureJson.insert(QStringLiteral("globalScale"), fixture.globalScale);
	QJsonArray shift;
	for (double value : fixture.globalShift) shift.append(value);
	fixtureJson.insert(QStringLiteral("globalShift"), shift);
	fixtureJson.insert(QStringLiteral("loadMilliseconds"), static_cast<double>(loadMilliseconds));
	report.insert(QStringLiteral("fixture"), fixtureJson);
	csvRows.push_back({QStringLiteral("fixture"), QStringLiteral("load"), QString(), QString::number(loadMilliseconds),
	                   unsignedNumber(fixture.pointCount), unsignedNumber(fixture.pointCount), QStringLiteral("0"),
	                   QString(), QString(), QString(), QString(), fixture.fixtureSha256});

	alis::FeatureEngine featureEngine(cloud.get());
	alis::SpacingSummary spacing;
	std::string featureError;
	phaseTimer.restart();
	if (!featureEngine.estimateSpacing(spacing, featureError, 4096, nullptr))
	{
		return finishFailure(QStringLiteral("spacing.estimate"), QString::fromStdString(featureError));
	}
	const qint64 spacingMilliseconds = phaseTimer.elapsed();
	QJsonObject spacingJson = spacingToJson(spacing);
	spacingJson.insert(QStringLiteral("elapsedMilliseconds"), static_cast<double>(spacingMilliseconds));
	spacingJson.insert(QStringLiteral("method"),
	                   QStringLiteral("deterministic occupied-octree-cell nearest-neighbour sample"));
	report.insert(QStringLiteral("spacing"), spacingJson);
	csvRows.push_back({QStringLiteral("spacing"), QStringLiteral("nearest-neighbour estimate"), QString(),
	                   QString::number(spacingMilliseconds), unsignedNumber(spacing.sampledPoints),
	                   unsignedNumber(spacing.positiveSpacingCount), unsignedNumber(spacing.unresolvedSamples),
	                   number(spacing.minimum), number(spacing.maximum), number(spacing.median),
	                   number(spacing.medianAbsoluteDeviation), QString()});

	const alis::GroundParameters preset = alis::groundPresetParameters(
		alis::GroundPreset::ArchaeologicalSite, spacing.median);
	alis::GroundFilterParameters groundParameters;
	groundParameters.smoothSlope = preset.slopeProcessing;
	groundParameters.timeStep = preset.timeStep;
	groundParameters.classificationThreshold = preset.classificationThreshold;
	groundParameters.clothResolution = preset.clothResolution;
	groundParameters.rigidness = preset.rigidness;
	groundParameters.iterations = preset.iterations;
	alis::CSFGroundFilter groundFilter;
	const alis::GroundFilterValidation groundValidation = groundFilter.validate(*cloud, groundParameters);
	if (!groundValidation.valid)
	{
		return finishFailure(QStringLiteral("ground.validate"), groundValidation.message);
	}
	alis::GroundFilterContext groundContext;
	const alis::GroundFilterResult ground = groundFilter.run(*cloud, groundParameters, groundContext);
	if (!ground.succeeded())
	{
		return finishFailure(QStringLiteral("ground.csf"), ground.message);
	}
	if (ground.groundPointCount == 0 || ground.offGroundPointCount == 0)
	{
		return finishFailure(QStringLiteral("ground.sanity"),
		                     QStringLiteral("CSF returned a trivial all-Ground or all-Non-ground mask."));
	}
	const QString groundMaskHash = maskSha256(ground.isGround);
	QJsonObject groundJson;
	groundJson.insert(QStringLiteral("algorithmId"), ground.provenance.algorithmId);
	groundJson.insert(QStringLiteral("implementation"), ground.provenance.implementation);
	groundJson.insert(QStringLiteral("status"), QStringLiteral("success"));
	groundJson.insert(QStringLiteral("message"), ground.message);
	groundJson.insert(QStringLiteral("parameters"), ground.provenance.effectiveParameters);
	groundJson.insert(QStringLiteral("parameterDerivation"),
	                  QStringLiteral("ArchaeologicalSite preset evaluated at measured median spacing"));
	groundJson.insert(QStringLiteral("elapsedMilliseconds"), static_cast<double>(ground.provenance.elapsedMilliseconds));
	groundJson.insert(QStringLiteral("groundPointCount"), static_cast<double>(ground.groundPointCount));
	groundJson.insert(QStringLiteral("offGroundPointCount"), static_cast<double>(ground.offGroundPointCount));
	groundJson.insert(QStringLiteral("groundFraction"),
	                  static_cast<double>(ground.groundPointCount) / static_cast<double>(fixture.pointCount));
	groundJson.insert(QStringLiteral("maskSha256BitPacked"), groundMaskHash);
	report.insert(QStringLiteral("ground"), groundJson);
	csvRows.push_back({QStringLiteral("ground"), QStringLiteral("CSF Ground mask"), QString(),
	                   QString::number(ground.provenance.elapsedMilliseconds), unsignedNumber(fixture.pointCount),
	                   unsignedNumber(ground.groundPointCount), unsignedNumber(ground.offGroundPointCount),
	                   QStringLiteral("0"), QStringLiteral("1"),
	                   number(static_cast<double>(ground.groundPointCount) / static_cast<double>(fixture.pointCount)),
	                   QString(), groundMaskHash});

	alis::TerrainParameters terrainParameters;
	terrainParameters.gridStep = std::max(0.10, std::min(0.50, 8.0 * spacing.median));
	terrainParameters.interpolateEmptyCells = true;
	terrainParameters.maximumInterpolationEdgeLength = 8.0 * terrainParameters.gridStep;
	alis::TerrainEngine terrainEngine;
	const alis::TerrainValidation terrainValidation =
		terrainEngine.validate(*cloud, ground.isGround, terrainParameters);
	if (!terrainValidation.valid)
	{
		return finishFailure(QStringLiteral("terrain.validate"), terrainValidation.message);
	}
	alis::TerrainResult terrain = terrainEngine.run(*cloud, ground.isGround, terrainParameters);
	if (!terrain.succeeded())
	{
		return finishFailure(QStringLiteral("terrain.dtm_hag"), terrain.message);
	}
	if (!terrain.grid.isValid() || terrain.statistics.heightAboveGround.validCount == 0)
	{
		return finishFailure(QStringLiteral("terrain.sanity"),
		                     QStringLiteral("DTM grid is invalid or HAG has no finite samples."));
	}
	const QString dtmHash = rawVectorSha256(terrain.grid.heightsLocal);
	const QString hagHash = rawVectorSha256(terrain.heightAboveGround);
	QJsonObject terrainJson;
	terrainJson.insert(QStringLiteral("algorithmId"), terrain.provenance.algorithmId);
	terrainJson.insert(QStringLiteral("implementation"), terrain.provenance.implementation);
	terrainJson.insert(QStringLiteral("status"), QStringLiteral("success"));
	terrainJson.insert(QStringLiteral("message"), terrain.message);
	terrainJson.insert(QStringLiteral("parameters"), terrain.provenance.effectiveParameters);
	terrainJson.insert(QStringLiteral("gridStepDerivation"),
	                   QStringLiteral("clamp(8 x measured median spacing, 0.10 m, 0.50 m)"));
	terrainJson.insert(QStringLiteral("elapsedMilliseconds"), static_cast<double>(terrain.provenance.elapsedMilliseconds));
	terrainJson.insert(QStringLiteral("gridWidth"), static_cast<double>(terrain.grid.width));
	terrainJson.insert(QStringLiteral("gridHeight"), static_cast<double>(terrain.grid.height));
	terrainJson.insert(QStringLiteral("totalCellCount"), static_cast<double>(terrain.statistics.totalCellCount));
	terrainJson.insert(QStringLiteral("observedCellCount"), static_cast<double>(terrain.statistics.observedCellCount));
	terrainJson.insert(QStringLiteral("interpolatedCellCount"), static_cast<double>(terrain.statistics.interpolatedCellCount));
	terrainJson.insert(QStringLiteral("nodataCellCount"), static_cast<double>(terrain.statistics.nodataCellCount));
	terrainJson.insert(QStringLiteral("negativeHagCount"),
	                   static_cast<double>(terrain.statistics.negativeHeightAboveGroundCount));
	terrainJson.insert(QStringLiteral("dtmElevation"), terrainValueStatisticsToJson(terrain.statistics.dtmElevation));
	terrainJson.insert(QStringLiteral("dtmSha256RawFloat64LittleEndian"), dtmHash);
	terrainJson.insert(QStringLiteral("heightAboveGround"),
	                   terrainValueStatisticsToJson(terrain.statistics.heightAboveGround));
	terrainJson.insert(QStringLiteral("hagSha256RawFloat64LittleEndian"), hagHash);
	report.insert(QStringLiteral("terrain"), terrainJson);
	csvRows.push_back({QStringLiteral("terrain"), QStringLiteral("DTM elevation"), number(terrain.grid.step),
	                   QString::number(terrain.provenance.elapsedMilliseconds),
	                   unsignedNumber(terrain.statistics.totalCellCount),
	                   unsignedNumber(terrain.statistics.dtmElevation.validCount),
	                   unsignedNumber(terrain.statistics.dtmElevation.nodataCount),
	                   number(terrain.statistics.dtmElevation.minimum), number(terrain.statistics.dtmElevation.maximum),
	                   number(terrain.statistics.dtmElevation.mean),
	                   number(terrain.statistics.dtmElevation.standardDeviation), dtmHash});
	csvRows.push_back({QStringLiteral("terrain"), QStringLiteral("HAG"), number(terrain.grid.step),
	                   QString::number(terrain.provenance.elapsedMilliseconds), unsignedNumber(fixture.pointCount),
	                   unsignedNumber(terrain.statistics.heightAboveGround.validCount),
	                   unsignedNumber(terrain.statistics.heightAboveGround.nodataCount),
	                   number(terrain.statistics.heightAboveGround.minimum),
	                   number(terrain.statistics.heightAboveGround.maximum),
	                   number(terrain.statistics.heightAboveGround.mean),
	                   number(terrain.statistics.heightAboveGround.standardDeviation), hagHash});

	alis::ScaleSuggestionOptions scaleOptions;
	scaleOptions.metricUnitsConfirmed = true;
	scaleOptions.targetNeighborCounts = {16, 64, 256};
	const std::vector<double> radii = alis::FeatureEngine::suggestScales(spacing, scaleOptions, featureError);
	if (radii.size() != 3)
	{
		return finishFailure(QStringLiteral("features.scales"),
		                     QStringLiteral("Expected three distinct density-aware scales: %1")
		                         .arg(QString::fromStdString(featureError)));
	}
	QJsonArray scalesJson;
	for (double radius : radii) scalesJson.append(radius);
	QJsonObject scaleDerivation;
	scaleDerivation.insert(QStringLiteral("radiiMetres"), scalesJson);
	QJsonArray targetCounts;
	for (unsigned count : scaleOptions.targetNeighborCounts) targetCounts.append(static_cast<int>(count));
	scaleDerivation.insert(QStringLiteral("targetNeighborCounts"), targetCounts);
	scaleDerivation.insert(QStringLiteral("formula"), QStringLiteral("sqrt(targetCount / (pi x estimatedDensity2D))"));
	scaleDerivation.insert(QStringLiteral("metricUnitsConfirmedFromFixtureCrs"), true);
	report.insert(QStringLiteral("scaleSelection"), scaleDerivation);

	const std::array<alis::FeatureId, 3> featureIds = {{
		alis::FeatureId::Planarity,
		alis::FeatureId::Roughness,
		alis::FeatureId::Verticality,
	}};
	std::vector<alis::FeatureRequest> requests;
	for (double radius : radii)
	{
		for (alis::FeatureId feature : featureIds)
		{
			alis::FeatureRequest request;
			request.feature = feature;
			request.radius = radius;
			requests.push_back(request);
		}
	}
	alis::ComputeReport computeReport;
	alis::ComputeOptions computeOptions;
	computeOptions.multiThread = true;
	if (!featureEngine.compute(requests, computeReport, featureError, nullptr, computeOptions))
	{
		return finishFailure(QStringLiteral("features.compute"), QString::fromStdString(featureError));
	}
	QJsonObject computeJson;
	computeJson.insert(QStringLiteral("pointCount"), static_cast<double>(computeReport.pointCount));
	computeJson.insert(QStringLiteral("requestedFields"), static_cast<double>(computeReport.requestedFields));
	computeJson.insert(QStringLiteral("computedFields"), static_cast<double>(computeReport.computedFields));
	computeJson.insert(QStringLiteral("reusedFields"), static_cast<double>(computeReport.reusedFields));
	computeJson.insert(QStringLiteral("processedScales"), static_cast<double>(computeReport.processedScales));
	computeJson.insert(QStringLiteral("octreeBuilt"), computeReport.octreeBuilt);
	computeJson.insert(QStringLiteral("octreeReused"), computeReport.octreeReused);
	computeJson.insert(QStringLiteral("elapsedSeconds"), computeReport.elapsedSeconds);
	computeJson.insert(QStringLiteral("sharedPcaContract"),
	                   QStringLiteral("one maximum-radius query per point and at most one covariance eigendecomposition per point/radius"));
	csvRows.push_back({QStringLiteral("features"), QStringLiteral("compute all requested fields"), QString(),
	                   number(computeReport.elapsedSeconds * 1000.0), unsignedNumber(computeReport.pointCount),
	                   QString::number(computeReport.computedFields), QString::number(computeReport.reusedFields),
	                   QString(), QString(), QString(), QString(), QString()});

	QJsonArray featureFields;
	phaseTimer.restart();
	for (double radius : radii)
	{
		for (alis::FeatureId feature : featureIds)
		{
			alis::FeatureKey key;
			key.feature = feature;
			key.radius = radius;
			alis::FeatureStatistics statistics;
			if (!featureEngine.statistics(key, statistics, featureError))
			{
				return finishFailure(QStringLiteral("features.statistics"), QString::fromStdString(featureError));
			}
			const std::vector<ScalarType>* values = featureEngine.cachedValues(key);
			if (!values || values->size() != fixture.pointCount)
			{
				return finishFailure(QStringLiteral("features.cache"),
				                     QStringLiteral("Cached feature array is missing or point-index misaligned."));
			}
			const QString valuesHash = rawVectorSha256(*values);
			QJsonObject fieldJson;
			fieldJson.insert(QStringLiteral("feature"), QString::fromLatin1(alis::featureName(feature)));
			fieldJson.insert(QStringLiteral("radiusMetres"), radius);
			fieldJson.insert(QStringLiteral("statistics"), featureStatisticsToJson(statistics));
			fieldJson.insert(QStringLiteral("valuesSha256RawScalarTypeLittleEndian"), valuesHash);
			fieldJson.insert(QStringLiteral("cached"), true);
			featureFields.append(fieldJson);
			csvRows.push_back({QStringLiteral("feature-statistics"),
			                   QString::fromLatin1(alis::featureName(feature)), number(radius), QString(),
			                   unsignedNumber(statistics.totalCount), unsignedNumber(statistics.validCount),
			                   unsignedNumber(statistics.invalidCount), number(statistics.minimum), number(statistics.maximum),
			                   number(statistics.mean), number(statistics.standardDeviation), valuesHash});
		}
	}
	computeJson.insert(QStringLiteral("statisticsElapsedMilliseconds"), static_cast<double>(phaseTimer.elapsed()));
	computeJson.insert(QStringLiteral("fields"), featureFields);

	QJsonArray materialized;
	phaseTimer.restart();
	for (alis::FeatureId feature : featureIds)
	{
		alis::FeatureKey key;
		key.feature = feature;
		key.radius = radii[1];
		const std::string fieldName = featureEngine.defaultScalarFieldName(key);
		const int index = featureEngine.materialize(
			key, fieldName, alis::MaterializePolicy::FailIfExists, featureError);
		if (index < 0)
		{
			return finishFailure(QStringLiteral("features.materialize"), QString::fromStdString(featureError));
		}
		QJsonObject materializedField;
		materializedField.insert(QStringLiteral("feature"), QString::fromLatin1(alis::featureName(feature)));
		materializedField.insert(QStringLiteral("radiusMetres"), radii[1]);
		materializedField.insert(QStringLiteral("scalarFieldName"), QString::fromStdString(fieldName));
		materializedField.insert(QStringLiteral("scalarFieldIndex"), index);
		materialized.append(materializedField);
	}
	computeJson.insert(QStringLiteral("materializationElapsedMilliseconds"), static_cast<double>(phaseTimer.elapsed()));
	computeJson.insert(QStringLiteral("materializedMiddleScale"), materialized);
	computeJson.insert(QStringLiteral("cacheBytes"), static_cast<double>(featureEngine.cacheBytes()));
	report.insert(QStringLiteral("features"), computeJson);

	report.insert(QStringLiteral("status"), QStringLiteral("success"));
	report.insert(QStringLiteral("elapsedMilliseconds"), static_cast<double>(totalTimer.elapsed()));
	report.insert(QStringLiteral("finishedUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
	report.insert(QStringLiteral("interpretation"),
	              QStringLiteral("Exploratory engineering output; not a validated archaeological classification."));
	if (!writeResults(outputDirectory, report, csvRows, error))
	{
		std::cerr << error.toStdString() << '\n';
		return EXIT_FAILURE;
	}
	std::cout << "ALiS real-tile E2E completed successfully\n"
	          << "JSON: " << QDir(outputDirectory).filePath(QStringLiteral("real_tile_e2e.json")).toStdString() << '\n'
	          << "CSV:  " << QDir(outputDirectory).filePath(QStringLiteral("real_tile_e2e.csv")).toStdString() << '\n';
	return EXIT_SUCCESS;
}
