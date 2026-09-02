#include <QApplication>
#include <QColor>
#include <QFile>
#include <QLineEdit>
#include <QPalette>
#include <QTableWidget>
#include <QTest>
#include <QWidget>

class MissionPlannerStyleTest final : public QObject
{
    Q_OBJECT

private slots:
    void emeraldPaletteMatchesMissionPlanner10();
    void representativeWidgetsUseDarkEmeraldSurfaces();
};

namespace {
QString loadEmeraldStyle()
{
    QFile file(QStringLiteral(APM_TEST_SOURCE_DIR
                              "/files/styles/style-outdoor.css"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}
}

void MissionPlannerStyleTest::emeraldPaletteMatchesMissionPlanner10()
{
    const QString style = loadEmeraldStyle();
    QVERIFY2(!style.isEmpty(), "The default desktop stylesheet is unreadable");

    // These are the authoritative Emerald resources in MP10's
    // Theme/Palettes/Emerald.axaml. Keep every token explicit so an accidental
    // fallback to the former white outdoor theme is caught immediately.
    const QStringList colors{
        QStringLiteral("#34D399"), QStringLiteral("#10B981"),
        QStringLiteral("#1A201D"), QStringLiteral("#121614"),
        QStringLiteral("#202623"), QStringLiteral("#161B18"),
        QStringLiteral("#2A322D"), QStringLiteral("#0D1210"),
        QStringLiteral("#06251A"), QStringLiteral("#065F46"),
        QStringLiteral("#E6EDE9")
    };
    for (const QString &color : colors) {
        QVERIFY2(style.contains(color, Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("Missing MP10 color %1")
                                .arg(color)));
    }
    QVERIFY(!style.contains(QStringLiteral("#F6F6F6"),
                            Qt::CaseInsensitive));
    QVERIFY(style.contains(QStringLiteral("QWidget#FdMap")));
    QVERIFY(style.contains(QStringLiteral("QTableView#WpGrid")));
}

void MissionPlannerStyleTest::representativeWidgetsUseDarkEmeraldSurfaces()
{
    const QString style = loadEmeraldStyle();
    QVERIFY(!style.isEmpty());
    QVERIFY(QApplication::setStyle(QStringLiteral("Fusion")) != nullptr);
    qApp->setStyleSheet(style);

    QWidget surface;
    QLineEdit input(&surface);
    QTableWidget table(&surface);
    surface.ensurePolished();
    input.ensurePolished();
    table.ensurePolished();

    QCOMPARE(surface.palette().color(QPalette::Window), QColor("#1A201D"));
    QCOMPARE(surface.palette().color(QPalette::WindowText), QColor("#E6EDE9"));
    QCOMPARE(input.palette().color(QPalette::Base), QColor("#161B18"));
    QCOMPARE(input.palette().color(QPalette::Text), QColor("#E6EDE9"));
    QCOMPARE(table.palette().color(QPalette::Base), QColor("#161B18"));
    QCOMPARE(table.palette().color(QPalette::AlternateBase), QColor("#202623"));

    qApp->setStyleSheet(QString());
}

QTEST_MAIN(MissionPlannerStyleTest)
#include "test_missionplannerstyle.moc"
