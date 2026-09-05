#ifndef WARNINGENGINE_H
#define WARNINGENGINE_H

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QVector>
#include <functional>

class QTimer;

struct CustomWarning
{
    enum Conditional { NONE, LT, LTEQ, EQ, GT, GTEQ, NEQ };
    enum WarningType { SpeakAndText, Coloring };
    quint64 id = 0; // Runtime identity, never a vehicle identity or XML field.
    QString name;
    Conditional condition = NONE;
    double threshold = 0.0;
    WarningType type = SpeakAndText;
    QString color = QStringLiteral("NoColor");
    int repeatSeconds = 10;
    QString text = QStringLiteral("WARNING: {name} is {value}");
    QVector<CustomWarning> child; // MP10 permits zero or one AND child.
};

class WarningEngine final : public QObject
{
    Q_OBJECT
public:
    using Values = QHash<QString, double>;
    using Colors = QHash<QString, QString>;
    using ValueProvider = std::function<Values()>;
    using Clock = std::function<qint64()>;
    explicit WarningEngine(QString configPath, ValueProvider provider = {},
                           Clock clock = {}, QObject *parent = nullptr);
    QVector<CustomWarning> rules() const { return m_rules; }
    Colors colors() const { return m_colors; }
    QString configPath() const { return m_configPath; }
    bool dirty() const { return m_dirty; }
    bool setRules(QVector<CustomWarning> rules, QString *error = nullptr);
    bool load(QString *error = nullptr);
    bool save(QString *error = nullptr);
    static QStringList conditionNames();
    static QStringList typeNames();
    static QStringList colorNames();
    static QString formatText(const CustomWarning &rule, double value);
    void setRunning(bool running);
public slots:
    void tick();
    void resetEpoch();
signals:
    void rulesChanged();
    void warningMessage(const QString &message);
    void colorsChanged();
private:
    QString m_configPath;
    ValueProvider m_provider;
    Clock m_clock;
    QElapsedTimer m_elapsed;
    QVector<CustomWarning> m_rules;
    Colors m_colors;
    QHash<quint64, qint64> m_lastSpoken;
    quint64 m_nextId = 1;
    quint64 m_revision = 0;
    bool m_dirty = false;
    bool m_ticking = false;
    QTimer *m_timer = nullptr;
};

#endif
