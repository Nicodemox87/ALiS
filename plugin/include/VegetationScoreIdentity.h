// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <ccPointCloud.h>
#include <ccScalarField.h>
#include <QCryptographicHash>
#include <QDataStream>
#include <QStringList>
#include <cmath>
#include <functional>
#include <limits>

namespace alis {
// Exact, ordered content identity; never a sample, UID, path or point-count proxy.
// Persisted with BIN. Old metadata without this digest must be recomputed once.
inline QString vegetationScoreFingerprint(const ccPointCloud& cloud, const QStringList& fields,
                                          QString& error, const std::function<bool(int)>& progress = {})
{
    if (fields.isEmpty() || !fields.contains(QStringLiteral("qAL_VegetationScore"))) {
        error = QStringLiteral("Incomplete vegetation score identity."); return {};
    }
    QVector<const CCCoreLib::ScalarField*> columns;
    for (const auto& name : fields) {
        const int index = cloud.getScalarFieldIndexByName(name.toUtf8().constData());
        if (index < 0 || cloud.getScalarField(index)->currentSize() != cloud.size()) {
            error = QStringLiteral("Missing or unaligned score input: %1").arg(name); return {};
        }
        columns << cloud.getScalarField(index);
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_5_15);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
    stream << QStringLiteral("alis-vegetation-score-content/1") << quint64(cloud.size()) << fields;
    const auto shift = cloud.getGlobalShift();
    stream << shift.x << shift.y << shift.z << cloud.getGlobalScale();
    hash.addData(bytes); bytes.clear(); stream.device()->seek(0);
    for (unsigned i = 0; i < cloud.size(); ++i) {
        const auto* p = cloud.getPoint(i);
        stream << double(p->x) << double(p->y) << double(p->z);
        for (const auto* column : columns) {
            const double value = column->getValue(i);
            // Canonicalize NaNs (BIN readers may alter their payload) and -0.
            stream << (std::isnan(value) ? std::numeric_limits<double>::quiet_NaN() : value == 0 ? 0.0 : value);
        }
        if ((i & 4095u) == 4095u || i + 1 == cloud.size()) {
            hash.addData(bytes); bytes.clear(); stream.device()->seek(0);
            if (progress && !progress(int(100.0 * (i + 1) / cloud.size()))) {
                error = QStringLiteral("Score verification canceled; previous review preserved."); return {};
            }
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}
}
