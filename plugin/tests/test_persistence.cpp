// SPDX-License-Identifier: GPL-2.0-or-later
// Uses CloudCompare's actual serializers, never a mock LAS/BIN implementation.
// Real-data mode writes a separate, clearly artificial test cloud; never training data.
#include "SessionModel.h"
#include <BinFilter.h>
#include <ccIOPluginInterface.h>
#include <ccPointCloud.h>
#include <ccScalarField.h>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPluginLoader>
#include <QSaveFile>
#include <QTemporaryDir>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

using namespace alis;
namespace {
int checks = 0;
void require(bool condition, const QString& message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message.toStdString());
}
const QString sessionKey = QStringLiteral("ALiS.Session.v1");
class Digest {
    QCryptographicHash hash{QCryptographicHash::Sha256};
    QByteArray buffer;
public:
    template<class T> void add(const T& value) {
        buffer.append(reinterpret_cast<const char*>(&value), sizeof(value));
        if (buffer.size() >= 65536) { hash.addData(buffer); buffer.clear(); }
    }
    void scalar(float value) {
        // Compare missingness, not implementation-specific NaN payloads.
        if (std::isnan(value)) { const quint32 nan = 0x7fc00000; add(nan); }
        else add(value);
    }
    QString result() { hash.addData(buffer); return QString::fromLatin1(hash.result().toHex()); }
};
QJsonObject snapshot(const ccPointCloud& cloud)
{
    Digest xyz, rgb;
    for (unsigned i = 0; i < cloud.size(); ++i) {
        const auto& p = *cloud.getPoint(i);
        xyz.add(p.x); xyz.add(p.y); xyz.add(p.z);
        if (cloud.hasColors()) {
            const auto& c = cloud.getPointColor(i);
            rgb.add(c.r); rgb.add(c.g); rgb.add(c.b); rgb.add(c.a);
        }
    }
    QJsonObject fields, ranges;
    for (unsigned s = 0; s < cloud.getNumberOfScalarFields(); ++s) {
        const auto* sf = static_cast<const ccScalarField*>(cloud.getScalarField(s));
        require(sf->size() == cloud.size(), QStringLiteral("Scalar alignment: %1").arg(sf->getName()));
        Digest values;
        for (unsigned i = 0; i < cloud.size(); ++i) values.scalar(sf->getValue(i));
        const QString name = QString::fromUtf8(sf->getName());
        fields.insert(name, values.result());
        ranges.insert(name, QJsonArray{sf->displayRange().start(), sf->displayRange().stop(),
            sf->saturationRange().start(), sf->saturationRange().stop(),
            sf->getColorScale() ? sf->getColorScale()->getUuid() : QString(),
            static_cast<int>(sf->getColorRampSteps())});
    }
    const auto shift = cloud.getGlobalShift();
    return QJsonObject{{"points", static_cast<double>(cloud.size())},
        {"xyz_sha256", xyz.result()}, {"rgba_sha256", rgb.result()},
        {"shift", QJsonArray{shift.x, shift.y, shift.z}}, {"scale", cloud.getGlobalScale()},
        {"fields", fields}, {"ranges", ranges},
        {"session_metadata", QString::fromUtf8(cloud.getMetaData(sessionKey).toByteArray())}};
}
ccPointCloud* singleCloud(ccHObject& root)
{
    ccPointCloud* result = nullptr;
    ccHObject::Container objects;
    root.filterChildren(objects, true, CC_TYPES::POINT_CLOUD);
    if (root.isA(CC_TYPES::POINT_CLOUD)) objects.push_back(&root);
    require(objects.size() == 1, "Exactly one cloud, no split/subsampling");
    result = static_cast<ccPointCloud*>(objects.front());
    return result;
}
void load(FileIOFilter& filter, const QString& path, ccHObject& root)
{
    FileIOFilter::LoadParameters parameters;
    parameters.alwaysDisplayLoadDialog = false;
    parameters.shiftHandlingMode = ccGlobalShiftManager::NO_DIALOG_AUTO_SHIFT;
    const auto result = filter.loadFile(path, root, parameters);
    require(result == CC_FERR_NO_ERROR, QStringLiteral("Load error %1: %2 (LAS-derived BIN needs --las-plugin for VLR metadata)").arg(result).arg(path));
}
void save(FileIOFilter& filter, ccPointCloud& cloud, const QString& path)
{
    require(!QFileInfo::exists(path), "Refusing overwrite: " + path);
    FileIOFilter::SaveParameters parameters;
    parameters.alwaysDisplaySaveDialog = false;
    require(filter.saveToFile(&cloud, path, parameters) == CC_FERR_NO_ERROR, "Save: " + path);
}
void writeJson(const QString& path, const QJsonObject& object)
{
    require(!QFileInfo::exists(path), "Refusing overwrite: " + path);
    QSaveFile file(path);
    require(file.open(QIODevice::WriteOnly), "Open report");
    const auto data = QJsonDocument(object).toJson();
    require(file.write(data) == data.size() && file.commit(), "Commit report");
}
FileIOFilter::Shared lasFilter(QPluginLoader& loader)
{
    auto* plugin = qobject_cast<ccIOPluginInterface*>(loader.instance());
    require(plugin != nullptr, "LAS plugin: " + loader.errorString());
    const auto filters = plugin->getFilters();
    require(!filters.empty(), "LAS filter exposed");
    return filters.front();
}
void annotate(ccPointCloud& cloud, bool includeDerived)
{
    ALiSSession session(cloud);
    QString error;
    require(session.initialize(error), error);
    require(session.applyManualAsprs({0, cloud.size()/2}, 2, error), error);
    require(session.applyManualAsprs({1, cloud.size()-1}, 6, error), error);
    require(session.applyManualAsprs({2}, 65, error), error); // qAL user-defined Archaeological wall
    if (includeDerived) {
        std::vector<bool> mask(cloud.size(), false); mask[0] = true;
        require(session.setGroundPreview(mask, error) && session.applyGroundPreview(error), error);
        mask[1] = true;
        require(session.setGroundPreview(mask, error), error); // Pending preview must reopen too.
        std::vector<double> hag(cloud.size(), 1.25); hag[3] = std::numeric_limits<double>::quiet_NaN();
        require(session.setHagValues(hag, QJsonObject{{"test_only", true}}, 0, error), error);
        std::vector<std::int16_t> prediction(cloud.size(), 5); prediction[4] = -1;
        require(session.setAsprsPredictions(prediction, std::vector<float>(cloud.size(), .8f),
            QJsonObject{{"test_only", true}}, 0, error), error);
    }
    cloud.setMetaData("ALiS.TestOnly", true);
    cloud.setName("ARTIFICIAL_LABELS_PERSISTENCE_TEST_DO_NOT_TRAIN");
}
void verifyReinitialize(ccPointCloud& cloud, bool includeDerived)
{
    const auto before = snapshot(cloud);
    const auto history = QJsonDocument::fromJson(cloud.getMetaData(sessionKey).toByteArray()).object().value("history").toArray();
    ALiSSession reopened(cloud);
    QString error;
    require(reopened.initialize(error), error);
    auto after = snapshot(cloud), expected = before;
    after.remove("session_metadata"); expected.remove("session_metadata");
    require(after == expected, "Reinitialization preserves every point/field/display range");
    require(reopened.historyAsJson().size() == history.size()+1, "History restored and reopen appended");
    for (int i=0; i<history.size(); ++i) require(reopened.historyAsJson()[i] == history[i], "Historical provenance unchanged");
    require(!reopened.canUndo() && !reopened.canRedo(), "Undo/redo intentionally transient");
    if (includeDerived) require(reopened.hasAppliedGround() && reopened.hasGroundPreview(), "Ground and pending preview restored");
}
std::unique_ptr<ccPointCloud> fixture()
{
    auto cloud = std::unique_ptr<ccPointCloud>(new ccPointCloud("persistence-fixture"));
    require(cloud->reserve(128) && cloud->reserveTheRGBTable(), "Fixture allocation");
    cloud->setGlobalShift(CCVector3d(-441000, -4135000, 0));
    cloud->setGlobalScale(.5); // Exercise scale as well as georeferencing.
    for (unsigned i=0; i<128; ++i) {
        cloud->addPoint(CCVector3(i*.125f, (i%11)*.25f, 300.f+i*.0625f));
        cloud->addColor(ccColor::Rgb(i, 255-i, (i*3)%256));
    }
    for (const char* name : {"Classification", "Intensity", "Return Number", "Number Of Returns"}) {
        const int index = cloud->addScalarField(name);
        require(index >= 0, "Fixture scalar allocation");
        auto* sf = static_cast<ccScalarField*>(cloud->getScalarField(index));
        for (unsigned i=0; i<cloud->size(); ++i) sf->setValue(i, 1.f+(i%3));
        sf->computeMinAndMax();
        sf->setMinDisplayed(1.5f); sf->setSaturationStop(2.5f); sf->setColorRampSteps(32);
    }
    annotate(*cloud, true);
    return cloud;
}
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setOrganizationName("ALiSTests");
    app.setApplicationName("PersistenceTests"); // Do not touch user's CC settings.
    try {
        const QStringList args = app.arguments();
        auto option = [&](const QString& flag) { const int i=args.indexOf(flag); return i>=0 && i+1<args.size() ? args[i+1] : QString(); };
        const QString input=option("--input-las"), output=option("--output-dir"), verify=option("--verify-bin");
        // CloudCompare loads its IO plugins before any file. Do the same here:
        // LasPlugin registers the QVariant stream operators for saved LAS VLRs.
        QPluginLoader registeredLas(option("--las-plugin"));
        if (!option("--las-plugin").isEmpty()) require(registeredLas.instance() != nullptr, registeredLas.errorString());
        BinFilter bin;
        if (!verify.isEmpty()) {
            QFile expectedFile(verify + ".expected.json");
            require(expectedFile.open(QIODevice::ReadOnly), "Read expected snapshot");
            const auto expected=QJsonDocument::fromJson(expectedFile.readAll()).object();
            require(!expected.isEmpty(), "Valid expected snapshot");
            ccHObject root;
            load(bin, verify, root);
            auto* cloud=singleCloud(root);
            const auto actual=snapshot(*cloud);
            require(actual == expected.value("snapshot").toObject(), "Full-cloud hashes, point order, shift, scalar fields and metadata identical");
            verifyReinitialize(*cloud, false);
            writeJson(verify + ".verified.json", QJsonObject{{"passed",true}, {"points",static_cast<double>(cloud->size())},
                {"verification","fresh process; all points, all scalar fields"}, {"checks",checks}, {"snapshot",actual}});
        } else if (!input.isEmpty()) {
            require(!output.isEmpty() && !QFileInfo::exists(output), "Use a new output directory");
            require(QDir().mkpath(output), "Create test output directory");
            QPluginLoader loader(option("--las-plugin"));
            auto las=lasFilter(loader);
            ccHObject root;
            std::cout << "Loading full LAS (no decimation)..." << std::endl;
            load(*las, input, root);
            auto* cloud=singleCloud(root);
            const auto original=snapshot(*cloud);
            annotate(*cloud, false);
            const auto annotated=snapshot(*cloud);
            require(original.value("xyz_sha256")==annotated.value("xyz_sha256"), "Annotation leaves all XYZ untouched");
            const auto originalFields=original.value("fields").toObject();
            for (auto it=originalFields.begin(); it!=originalFields.end(); ++it)
                require(annotated.value("fields").toObject().value(it.key())==it.value(), "Source field untouched: "+it.key());
            const QString path=QDir(output).filePath("ARTIFICIAL_LABELS_DO_NOT_TRAIN.bin");
            std::cout << "Saving " << cloud->size() << " points..." << std::endl;
            save(bin, *cloud, path);
            writeJson(path+".expected.json", QJsonObject{{"test_only",true}, {"source",input}, {"snapshot",annotated}, {"source_snapshot",original}});
        } else {
            QTemporaryDir temporary;
            require(temporary.isValid(), "Temporary fixture directory");
            auto cloud=fixture();
            const auto before=snapshot(*cloud);
            const QString path=temporary.filePath("roundtrip.bin");
            save(bin, *cloud, path);
            ccHObject root;
            load(bin,path,root);
            auto* reopened=singleCloud(root);
            require(snapshot(*reopened)==before, "BIN exact roundtrip (all values, NaNs, palette, shift/scale, history)");
            verifyReinitialize(*reopened,true);
            if (!option("--las-plugin").isEmpty()) {
                QPluginLoader loader(option("--las-plugin"));
                auto las=lasFilter(loader);
                for (const QString extension : {QString("las"), QString("laz")}) {
                    const QString lasPath=temporary.filePath("roundtrip."+extension);
                    save(*las,*cloud,lasPath);
                    ccHObject lasRoot;
                    load(*las,lasPath,lasRoot);
                    auto* loaded=singleCloud(lasRoot);
                    require(loaded->size()==cloud->size(), "LAS/LAZ point count");
                    const auto actual=snapshot(*loaded).value("fields").toObject();
                    const auto fields=before.value("fields").toObject();
                    for (auto it=fields.begin(); it!=fields.end(); ++it)
                        require(actual.value(it.key())==it.value(), extension+" values and order: "+it.key());
                    double maxError=0;
                    for (unsigned i=0; i<cloud->size(); ++i) {
                        const auto a=cloud->toGlobal3d(*cloud->getPoint(i)), b=loaded->toGlobal3d(*loaded->getPoint(i));
                        maxError=std::max(maxError,(a-b).norm());
                        const auto ca=cloud->getPointColor(i), cb=loaded->getPointColor(i);
                        require(ca.r==cb.r && ca.g==cb.g && ca.b==cb.b, "LAS/LAZ RGB unchanged");
                    }
                    require(maxError<=1e-4, "LAS/LAZ coordinate error <= 0.1 mm");
                    std::cout << extension.toStdString() << ": fields preserved; max XYZ error=" << maxError
                        << " m; session metadata preserved=" << loaded->hasMetaData(sessionKey) << std::endl;
                }
            }
        }
        std::cout << "Persistence checks PASSED (" << checks << ")" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << std::endl;
        return 1;
    }
}
