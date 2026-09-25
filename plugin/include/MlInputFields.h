// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <ccPointCloud.h>
#include <ccScalarField.h>
#include <QUrl>
#include <QString>
#include <cmath>
#include <limits>

namespace alis
{
    //! Stable, name-based identities survive cloud save/reopen and SF reordering.
    inline QString scalarInputKey(const QString& name)
    {
        return QStringLiteral("sf:") + QString::fromLatin1(QUrl::toPercentEncoding(name));
    }

    inline bool scalarInputName(const QString& key, QString& name)
    {
        if (!key.startsWith(QStringLiteral("sf:"))) return false;
        name = QUrl::fromPercentEncoding(key.mid(3).toLatin1());
        return !name.isEmpty() && scalarInputKey(name) == key;
    }

    inline bool potentiallyLabelDerivedField(const QString& name)
    {
        const QString compact = name.toLower().remove(QLatin1Char(' ')).remove(QLatin1Char('_'));
        for (const QString& token : {QStringLiteral("class"), QStringLiteral("label"),
             QStringLiteral("training"), QStringLiteral("prediction"), QStringLiteral("confidence"),
             QStringLiteral("cluster"), QStringLiteral("groundmask"), QStringLiteral("groundpreview"), QStringLiteral("qalvegetation"), QStringLiteral("reviewstatus")})
            if (compact.contains(token)) return true;
        return false;
    }

    struct CloudAttributeInput
    {
        const ccScalarField* scalar = nullptr;
        int rgbChannel = -1;
        int xyzChannel = -1;
        QString name;

        float value(const ccPointCloud& cloud, unsigned point) const
        {
            float result = std::numeric_limits<float>::quiet_NaN();
            if (scalar) result = static_cast<float>(scalar->getValue(point));
            else if (xyzChannel >= 0) result = static_cast<float>(cloud.getPoint(point)->u[xyzChannel]);
            else if (rgbChannel >= 0)
            {
                const auto& color = cloud.getPointColor(point);
                result = static_cast<float>(rgbChannel == 0 ? color.r : rgbChannel == 1 ? color.g : color.b);
            }
            return std::isfinite(result) ? result : std::numeric_limits<float>::quiet_NaN();
        }
    };

    inline bool resolveCloudAttribute(const ccPointCloud& cloud, const QString& key,
                                      CloudAttributeInput& input, QString& error)
    {
        input = CloudAttributeInput();
        if (key.startsWith(QStringLiteral("xyz:")))
        {
            const QString axis = key.mid(4);
            input.xyzChannel = axis == QStringLiteral("x") ? 0 : axis == QStringLiteral("y") ? 1 : axis == QStringLiteral("z") ? 2 : -1;
            if (input.xyzChannel < 0) { error = QStringLiteral("Invalid XYZ axis"); return false; }
            input.name = QStringLiteral("Raw XYZ %1").arg(axis);
            return true;
        }
        if (key.startsWith(QStringLiteral("rgb:")))
        {
            const QString channel = key.mid(4);
            input.rgbChannel = channel == QStringLiteral("red") ? 0
                : channel == QStringLiteral("green") ? 1 : channel == QStringLiteral("blue") ? 2 : -1;
            if (input.rgbChannel < 0 || !cloud.hasColors())
            {
                error = QStringLiteral("RGB channel unavailable: %1").arg(key);
                return false;
            }
            input.name = QStringLiteral("RGB %1").arg(channel);
            return true;
        }
        QString name;
        if (!scalarInputName(key, name))
        {
            error = QStringLiteral("Invalid scalar-field input key: %1").arg(key);
            return false;
        }
        const int index = cloud.getScalarFieldIndexByName(name.toUtf8().constData());
        const auto* scalar = index >= 0 ? static_cast<const ccScalarField*>(cloud.getScalarField(index)) : nullptr;
        if (!scalar || scalar->currentSize() != cloud.size())
        {
            error = QStringLiteral("Scalar field unavailable or not point-aligned: %1").arg(name);
            return false;
        }
        input.scalar = scalar;
        input.name = name;
        return true;
    }
}
