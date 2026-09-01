#include "ConfigUserDefinedViewModel.h"

#include <QRegularExpression>
#include <QSettings>
#include <QSet>

namespace {
const QString kUserParamsKey = QStringLiteral("UserParams");
}

ConfigUserDefinedViewModel::ConfigUserDefinedViewModel(QObject *parent)
    : QObject(parent)
{
    QSettings settings;
    m_options = settings.contains(kUserParamsKey)
        ? ParseOptions(settings.value(kUserParamsKey).toString())
        : DefaultOptions();
}

QStringList ConfigUserDefinedViewModel::DefaultOptions()
{
    return {
        QStringLiteral("CH6_OPT"), QStringLiteral("CH7_OPT"),
        QStringLiteral("CH8_OPT"), QStringLiteral("CH9_OPT"),
        QStringLiteral("CH10_OPT"), QStringLiteral("CH11_OPT"),
        QStringLiteral("CH12_OPT"), QStringLiteral("CH13_OPT"),
        QStringLiteral("CH14_OPT"), QStringLiteral("CH15_OPT"),
        QStringLiteral("CH16_OPT"), QStringLiteral("RC6_OPTION"),
        QStringLiteral("RC7_OPTION"), QStringLiteral("RC8_OPTION"),
        QStringLiteral("RC9_OPTION"), QStringLiteral("RC10_OPTION"),
        QStringLiteral("RC11_OPTION"), QStringLiteral("RC12_OPTION"),
        QStringLiteral("RC13_OPTION"), QStringLiteral("RC14_OPTION"),
        QStringLiteral("RC15_OPTION"), QStringLiteral("RC16_OPTION")
    };
}

QStringList ConfigUserDefinedViewModel::ParseOptions(const QString &raw)
{
    const QStringList tokens = raw.split(
        QRegularExpression(QStringLiteral("[,\\r\\n\\s]+")),
        Qt::SkipEmptyParts);
    static const QRegularExpression validName(
        QStringLiteral("^[A-Z0-9_]{1,16}$"));
    QStringList options;
    QSet<QString> seen;
    for (const QString &token : tokens) {
        const QString name = token.trimmed().toUpper();
        if (!validName.match(name).hasMatch() || seen.contains(name)) {
            continue;
        }
        seen.insert(name);
        options.append(name);
    }
    return options;
}

QStringList ConfigUserDefinedViewModel::Options() const
{
    return m_options;
}

QString ConfigUserDefinedViewModel::OptionsText() const
{
    return m_options.join(QStringLiteral("\r\n"));
}

void ConfigUserDefinedViewModel::ApplyOptions(const QString &raw)
{
    const QStringList parsed = ParseOptions(raw);
    QSettings settings;
    const QString serialized = parsed.join(QLatin1Char(','));
    if (!settings.contains(kUserParamsKey)
        || settings.value(kUserParamsKey).toString() != serialized) {
        settings.setValue(kUserParamsKey, serialized);
    }
    if (m_options != parsed) {
        m_options = parsed;
        emit optionsChanged();
    }
}
