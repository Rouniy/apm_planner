#ifndef PARAMETERCODEC_H
#define PARAMETERCODEC_H

#include <QVariant>

enum class ParameterType : quint8
{
    Unknown = 0,
    UInt8 = 1,
    Int8 = 2,
    UInt16 = 3,
    Int16 = 4,
    UInt32 = 5,
    Int32 = 6,
    UInt64 = 7,
    Int64 = 8,
    Real32 = 9,
    Real64 = 10
};

enum class ParameterEncoding
{
    Bytewise,
    CStyleCast
};

class ParameterCodec
{
public:
    static QVariant decodeClassic(float wireValue,
                                  ParameterType type,
                                  ParameterEncoding encoding,
                                  bool *ok = nullptr);
    static float encodeClassic(const QVariant &value,
                               ParameterType type,
                               ParameterEncoding encoding,
                               bool *ok = nullptr);
    static bool valuesEqual(const QVariant &left,
                            const QVariant &right,
                            ParameterType type);
    static bool isInteger(ParameterType type);
};

#endif
