// SPDX-License-Identifier: GPL-2.0-or-later
// Headless batch adapter: the exact same native feature/terrain engines as the UI.
#include "FeatureEngine.h"
#include "GroundFilter.h"
#include "TerrainEngine.h"
#include <GenericProgressCallback.h>
#include <ccPointCloud.h>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace alis;
namespace {
void require(bool ok, const QString& message) { if (!ok) throw std::runtime_error(message.toStdString()); }
struct Progress : CCCoreLib::GenericProgressCallback {
    std::atomic<int> bucket{-1};
    void update(float percent) override {
        const int next=static_cast<int>(percent)/5;
        int old=bucket.load();
        if (next>old && bucket.compare_exchange_strong(old,next)) std::cout << "Progress " << next*5 << "%" << std::endl;
    }
    void start() override { bucket=-1; }
    void stop() override {}
    void setMethodTitle(const char* name) override { std::cout << name << std::endl; }
    void setInfo(const char*) override {}
    bool isCancelRequested() override { return false; }
};
const std::map<FeatureId, const char*> featureIds{
    {FeatureId::NeighborCount,"neighbor_count"}, {FeatureId::Density3D,"density_3d"},
    {FeatureId::EigenvaluesSum,"eigenvalues_sum"}, {FeatureId::PCA1,"pca_1"}, {FeatureId::PCA2,"pca_2"},
    {FeatureId::Linearity,"linearity"}, {FeatureId::Planarity,"planarity"}, {FeatureId::Sphericity,"sphericity"},
    {FeatureId::Anisotropy,"anisotropy"}, {FeatureId::Omnivariance,"omnivariance"},
    {FeatureId::Eigenentropy,"eigenentropy"}, {FeatureId::SurfaceVariation,"surface_variation"},
    {FeatureId::Verticality,"verticality"}, {FeatureId::NormalZAbsolute,"normal_z_absolute"},
    {FeatureId::Roughness,"roughness"}, {FeatureId::BarycenterOffsetRatio,"barycenter_offset_ratio"},
    {FeatureId::ZRange,"z_range"}, {FeatureId::ZStandardDeviation,"z_standard_deviation"},
    {FeatureId::ZAboveMinimum,"z_above_minimum"}, {FeatureId::ZRelativeToMean,"z_relative_to_mean"}
};
}
int main(int argc, char** argv)
{
    QApplication app(argc,argv);
    app.setOrganizationName("ALiSTests"); app.setApplicationName("ClassifierFeatureExporter");
    try {
        const auto args=app.arguments();
        require(args.size()>=2,"Usage: export_classifier_features <dataset-dir> [threads] [--csf-defaults]");
        QDir dir(args[1]);
        require(!QFileInfo::exists(dir.filePath("features.f32")) && !QFileInfo::exists(dir.filePath("manifest.json")),"Refusing to overwrite features/dataset manifest");
        QFile metadataFile(dir.filePath("input.json")); require(metadataFile.open(QIODevice::ReadOnly),"Read input.json");
        auto input=QJsonDocument::fromJson(metadataFile.readAll()).object();
        require(input.value("schema").toString()=="qal-native-feature-input/1.0","Input schema");
        const quint64 count=input.value("point_count").toVariant().toULongLong();
        require(count>0 && count<=std::numeric_limits<unsigned>::max(),"Point count outside CloudCompare capacity");
        const auto shift=input.value("global_shift").toArray();
        require(shift.size()==3 && input.value("global_scale").toDouble()==1,"Input shift/scale");
        ccPointCloud cloud("full-resolution-training-support");
        require(cloud.reserve(static_cast<unsigned>(count)),"Cloud allocation");
        cloud.setGlobalShift(CCVector3d(shift[0].toDouble(),shift[1].toDouble(),shift[2].toDouble()));
        QFile xyz(dir.filePath("xyz_local.f32")); require(xyz.open(QIODevice::ReadOnly) && xyz.size()==count*12,"XYZ byte count");
        while (!xyz.atEnd()) {
            const auto bytes=xyz.read(12*65536);
            require(bytes.size()%12==0,"Aligned XYZ chunk");
            for (int offset=0; offset<bytes.size(); offset+=12) {
                float p[3]; std::memcpy(p,bytes.constData()+offset,12);
                require(std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]),"Finite XYZ");
                cloud.addPoint(CCVector3(p[0],p[1],p[2]));
            }
        }
        require(cloud.size()==count,"Full input point count");
        FeatureEngine engine(&cloud);
        engine.setCacheByteLimit(std::size_t{8}*1024*1024*1024);
        Progress progress;
        SpacingSummary spacing;
        std::string error;
        require(engine.estimateSpacing(spacing,error,4096,&progress),QString::fromStdString(error));
        ScaleSuggestionOptions scales; scales.metricUnitsConfirmed=true;
        scales.targetNeighborCounts={16,64,256}; scales.minimumRadius=.08; scales.maximumRadius=.5;
        auto radii=FeatureEngine::suggestScales(spacing,scales,error);
        for (auto& radius:radii) radius=std::round(radius*1000)/1000;
        std::sort(radii.begin(),radii.end()); radii.erase(std::unique(radii.begin(),radii.end()),radii.end());
        require(!radii.empty(),QString::fromStdString(error));
        QJsonArray radiusJson;
        for (double radius:radii) { radiusJson.append(radius); std::cout << "Radius: " << radius << " m" << std::endl; }
        std::vector<FeatureRequest> requests;
        for (const auto& entry:featureIds) for (double radius:radii) requests.push_back({entry.first,radius,{}});
        ComputeOptions options; options.multiThread=true; options.maxThreadCount=args.size()>2?args[2].toInt():12;
        ComputeReport report;
        require(engine.compute(requests,report,error,&progress,options),QString::fromStdString(error));
        std::cout << "Features computed in " << report.elapsedSeconds << " seconds" << std::endl;
        // Ground is estimated WITHOUT consulting any reference class, including holdout labels.
        CSFGroundFilter groundFilter;
        GroundFilterParameters groundParameters;
        if (!args.contains("--csf-defaults")) {
            // Preserve the historical experiment on explicit legacy invocations.
            groundParameters.clothResolution=1.; groundParameters.classificationThreshold=.15;
            groundParameters.rigidness=3; groundParameters.iterations=500; groundParameters.smoothSlope=false;
        }
        std::cout << "Automatic CSF (no reference labels)" << std::endl;
        auto ground=groundFilter.run(cloud,groundParameters,GroundFilterContext());
        require(ground.succeeded(),ground.message);
        TerrainParameters terrainParameters;
        terrainParameters.gridStep=.5; terrainParameters.interpolateEmptyCells=true;
        terrainParameters.maximumInterpolationEdgeLength=10.;
        TerrainEngine terrain;
        auto terrainResult=terrain.run(cloud,ground.isGround,terrainParameters);
        require(terrainResult.succeeded(),terrainResult.message);
        QJsonArray names;
        std::vector<const std::vector<ScalarType>*> columns;
        // This is the same FeatureKey order and serialization as WorkspaceController.
        for (const auto& key:engine.cachedKeys()) {
            names.append(QStringLiteral("%1@%2").arg(featureIds.at(key.feature),QString::number(key.radius,'g',17)));
            columns.push_back(engine.cachedValues(key));
        }
        names.append("hag@0");
        QSaveFile features(dir.filePath("features.f32")); require(features.open(QIODevice::WriteOnly),"Open feature matrix");
        const std::size_t width=columns.size()+1;
        std::vector<float> chunk; chunk.reserve(8192*width);
        for (unsigned i=0; i<cloud.size(); ++i) {
            for (const auto* column:columns) chunk.push_back((*column)[i]);
            chunk.push_back(static_cast<float>(terrainResult.heightAboveGround[i]));
            if ((i+1)%8192==0 || i+1==cloud.size()) {
                const qint64 bytes=static_cast<qint64>(chunk.size()*sizeof(float));
                require(features.write(reinterpret_cast<const char*>(chunk.data()),bytes)==bytes,"Write feature rows");
                chunk.clear();
            }
        }
        require(features.commit(),"Commit feature matrix");
        auto metadata=input.value("metadata").toObject();
        metadata.insert("feature_processing",QJsonObject{{"engine","ALiS native FeatureEngine"},
            {"radii_m",radiusJson},{"spacing_nn_median_m",spacing.median}, {"spacing_sample_count",static_cast<double>(spacing.sampledPoints)},
            {"density_nn_poisson_estimate_m2",spacing.estimatedDensity2D},
            {"radius_rule","sqrt(k/(pi*density)), k=16/64/256, clamp 0.08..0.5m, round 0.001m"},
            {"seconds",report.elapsedSeconds},{"threads",options.maxThreadCount},{"source_decimated",false},
            {"absolute_xy_z_as_predictors",false},{"label_dependent_features",false}});
        metadata.insert("automatic_ground",ground.provenance.effectiveParameters);
        metadata.insert("automatic_hag",QJsonObject{{"grid_step_m",.5},{"max_interpolation_edge_m",10.},
            {"reference_classes_used",false},{"nodata_count",static_cast<double>(terrainResult.statistics.heightAboveGround.nodataCount)}});
        QJsonObject manifest{{"schema","qal-ml-dataset/1.0"},{"rows",static_cast<double>(count)},
            {"columns",static_cast<int>(width)},{"feature_names",names},{"target_domain","asprs_working"},
            {"labels_trusted",true},{"label_source",metadata.value("label_source")},{"metadata",metadata}};
        QSaveFile manifestFile(dir.filePath("manifest.json")); require(manifestFile.open(QIODevice::WriteOnly),"Open manifest");
        const auto bytes=QJsonDocument(manifest).toJson();
        require(manifestFile.write(bytes)==bytes.size() && manifestFile.commit(),"Commit manifest");
        std::cout << "READY: " << count << " points x " << width << " native features" << std::endl;
        return 0;
    } catch (const std::exception& e) { std::cerr << "ERROR: " << e.what() << std::endl; return 1; }
}
