#include "ParameterCodec.h"

#include <QtGlobal>

#include <cmath>
#include <cstring>
#include <limits>

namespace {
template<typename T>
T clampedCast(float value)
{
    const double numeric = static_cast<double>(value);
    const double minimum = static_cast<double>(std::numeric_limits<T>::lowest());
    const double maximum = static_cast<double>(std::numeric_limits<T>::max());
    return static_cast<T>(qBound(minimum, numeric, maximum));
}

quint32 floatBits(float value)
{
    quint32 bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "Classic MAVLink parameters are 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float bitsAsFloat(quint32 bits)
{
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void setResult(bool success, bool *ok)
{
    if (ok) {
        *ok = success;
    }
}
}

QVariant ParameterCodec::decodeClassic(float wireValue,
                                       ParameterType type,
                                       ParameterEncoding encoding,
                                       bool *ok)
{
    if (type == ParameterType::UInt64 || type == ParameterType::Int64
        || type == ParameterType::Real64 || type == ParameterType::Unknown) {
        setResult(false, ok);
        return QVariant();
    }

    setResult(true, ok);
    if (type == ParameterType::Real32) {
        return QVariant::fromValue(wireValue);
    }

    if (encoding == ParameterEncoding::CStyleCast) {
        switch (type) {
        case ParameterType::UInt8:  return QVariant::fromValue(static_cast<quint32>(clampedCast<quint8>(wireValue)));
        case ParameterType::Int8:   return QVariant::fromValue(static_cast<qint32>(clampedCast<qint8>(wireValue)));
        case ParameterType::UInt16: return QVariant::fromValue(static_cast<quint32>(clampedCast<quint16>(wireValue)));
        case ParameterType::Int16:  return QVariant::fromValue(static_cast<qint32>(clampedCast<qint16>(wireValue)));
        case ParameterType::UInt32: return QVariant::fromValue(clampedCast<quint32>(wireValue));
        case ParameterType::Int32:  return QVariant::fromValue(clampedCast<qint32>(wireValue));
        default: break;
        }
    } else {
        const quint32 bits = floatBits(wireValue);
        switch (type) {
        case ParameterType::UInt8:
            return QVariant::fromValue(static_cast<quint32>(bits & 0xffU));
        case ParameterType::Int8:
            return QVariant::fromValue(static_cast<qint32>(static_cast<qint8>(bits & 0xffU)));
        case ParameterType::UInt16:
            return QVariant::fromValue(static_cast<quint32>(bits & 0xffffU));
        case ParameterType::Int16:
            return QVariant::fromValue(static_cast<qint32>(static_cast<qint16>(bits & 0xffffU)));
        case ParameterType::UInt32:
            return QVariant::fromValue(bits);
        case ParameterType::Int32: {
            qint32 signedBits = 0;
            std::memcpy(&signedBits, &bits, sizeof(signedBits));
            return QVariant::fromValue(signedBits);
        }
        default: break;
        }
    }

    setResult(false, ok);
    return QVariant();
}

float ParameterCodec::encodeClassic(const QVariant &value,
                                    ParameterType type,
                                    ParameterEncoding encoding,
                                    bool *ok)
{
    bool converted = false;
    if (type == ParameterType::Real32) {
        const float result = value.toFloat(&converted);
        setResult(converted && std::isfinite(result), ok);
        return converted ? result : 0.0f;
    }
    if (type == ParameterType::UInt64 || type == ParameterType::Int64
        || type == ParameterType::Real64 || type == ParameterType::Unknown) {
        setResult(false, ok);
        return 0.0f;
    }

    quint32 bits = 0;
    if (type == ParameterType::UInt8 || type == ParameterType::UInt16
        || type == ParameterType::UInt32) {
        const qulonglong numeric = value.toULongLong(&converted);
        const qulonglong maximum = type == ParameterType::UInt8 ? 0xffULL
            : type == ParameterType::UInt16 ? 0xffffULL : 0xffffffffULL;
        if (!converted || numeric > maximum) {
            setResult(false, ok);
            return 0.0f;
        }
        bits = static_cast<quint32>(numeric);
    } else {
        const qlonglong numeric = value.toLongLong(&converted);
        const qlonglong minimum = type == ParameterType::Int8 ? -128LL
            : type == ParameterType::Int16 ? -32768LL : -2147483648LL;
        const qlonglong maximum = type == ParameterType::Int8 ? 127LL
            : type == ParameterType::Int16 ? 32767LL : 2147483647LL;
        if (!converted || numeric < minimum || numeric > maximum) {
            setResult(false, ok);
            return 0.0f;
        }
        bits = static_cast<quint32>(static_cast<qint32>(numeric));
        if (type == ParameterType::Int8) {
            bits &= 0xffU;
        } else if (type == ParameterType::Int16) {
            bits &= 0xffffU;
        }
    }

    setResult(true, ok);
    if (encoding == ParameterEncoding::CStyleCast) {
        if (type == ParameterType::UInt8 || type == ParameterType::UInt16
            || type == ParameterType::UInt32) {
            return static_cast<float>(bits);
        }
        qint32 signedValue = 0;
        if (type == ParameterType::Int8) {
            signedValue = static_cast<qint8>(bits & 0xffU);
        } else if (type == ParameterType::Int16) {
            signedValue = static_cast<qint16>(bits & 0xffffU);
        } else {
            std::memcpy(&signedValue, &bits, sizeof(signedValue));
        }
        return static_cast<float>(signedValue);
    }
    return bitsAsFloat(bits);
}

bool ParameterCodec::valuesEqual(const QVariant &left,
                                 const QVariant &right,
                                 ParameterType type)
{
    if (isInteger(type)) {
        if (type == ParameterType::UInt8 || type == ParameterType::UInt16
            || type == ParameterType::UInt32 || type == ParameterType::UInt64) {
            bool leftOk = false;
            bool rightOk = false;
            const qulonglong leftValue = left.toULongLong(&leftOk);
            const qulonglong rightValue = right.toULongLong(&rightOk);
            return leftOk && rightOk && leftValue == rightValue;
        }
        bool leftOk = false;
        bool rightOk = false;
        const qlonglong leftValue = left.toLongLong(&leftOk);
        const qlonglong rightValue = right.toLongLong(&rightOk);
        return leftOk && rightOk && leftValue == rightValue;
    }

    bool leftOk = false;
    bool rightOk = false;
    const double leftValue = left.toDouble(&leftOk);
    const double rightValue = right.toDouble(&rightOk);
    if (!leftOk || !rightOk) {
        return false;
    }
    if (std::isnan(leftValue) || std::isnan(rightValue)) {
        return std::isnan(leftValue) && std::isnan(rightValue);
    }
    return leftValue == rightValue
        || std::abs(leftValue - rightValue) <= std::numeric_limits<float>::epsilon();
}

bool ParameterCodec::isInteger(ParameterType type)
{
    return type >= ParameterType::UInt8 && type <= ParameterType::Int64;
}
