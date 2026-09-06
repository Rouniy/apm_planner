#include "ShapefilePolyRuntimeAudit.h"

#include "ui/MainWindow.h"
#include "ui/configuration/ConfigDeveloperToolsView.h"
#include "ui/configuration/ShapefilePolyController.h"
#include "ui/flightplanner/FlightPlannerPolygonModel.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPointF>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTreeWidget>

#include <cstring>
#include <functional>

namespace
{
bool waitFor(const std::function<bool()> &ready, int timeout = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < timeout) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return ready();
}

template<class T>
T *find(QObject *root, const char *name)
{
    return root
        ? root->findChild<T *>(QString::fromLatin1(name)) : nullptr;
}

template<class T>
T *visible(QObject *root, const char *name)
{
    if (root) {
        const auto objects = root->findChildren<T *>(
            QString::fromLatin1(name), Qt::FindChildrenRecursively);
        for (T *object : objects) {
            if (object->isVisible())
                return object;
        }
    }
    return nullptr;
}

void appendU32Be(QByteArray *bytes, quint32 value)
{
    bytes->append(char((value >> 24) & 0xff));
    bytes->append(char((value >> 16) & 0xff));
    bytes->append(char((value >> 8) & 0xff));
    bytes->append(char(value & 0xff));
}

void appendU32Le(QByteArray *bytes, quint32 value)
{
    bytes->append(char(value & 0xff));
    bytes->append(char((value >> 8) & 0xff));
    bytes->append(char((value >> 16) & 0xff));
    bytes->append(char((value >> 24) & 0xff));
}

void appendDoubleLe(QByteArray *bytes, double value)
{
    quint64 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int index = 0; index < 8; ++index)
        bytes->append(char((bits >> (8 * index)) & 0xff));
}

QByteArray pointRecord(double longitude, double latitude)
{
    QByteArray record;
    appendU32Le(&record, 1);
    appendDoubleLe(&record, longitude);
    appendDoubleLe(&record, latitude);
    return record;
}

QByteArray polyLineRecord(const QVector<QPointF> &points)
{
    QByteArray record;
    appendU32Le(&record, 3);
    double minimumX = points.first().x();
    double maximumX = minimumX;
    double minimumY = points.first().y();
    double maximumY = minimumY;
    for (const QPointF &point : points) {
        minimumX = qMin(minimumX, point.x());
        maximumX = qMax(maximumX, point.x());
        minimumY = qMin(minimumY, point.y());
        maximumY = qMax(maximumY, point.y());
    }
    appendDoubleLe(&record, minimumX);
    appendDoubleLe(&record, minimumY);
    appendDoubleLe(&record, maximumX);
    appendDoubleLe(&record, maximumY);
    appendU32Le(&record, 1);
    appendU32Le(&record, quint32(points.size()));
    appendU32Le(&record, 0);
    for (const QPointF &point : points) {
        appendDoubleLe(&record, point.x());
        appendDoubleLe(&record, point.y());
    }
    return record;
}

QByteArray shapefile(quint32 type, const QVector<QByteArray> &records)
{
    QByteArray bytes;
    appendU32Be(&bytes, 9994);
    bytes.append(20, '\0');
    appendU32Be(&bytes, 0);
    appendU32Le(&bytes, 1000);
    appendU32Le(&bytes, type);
    bytes.append(64, '\0');
    quint32 number = 1;
    for (const QByteArray &record : records) {
        appendU32Be(&bytes, number++);
        appendU32Be(&bytes, quint32(record.size() / 2));
        bytes.append(record);
    }
    const quint32 words = quint32(bytes.size() / 2);
    bytes[24] = char((words >> 24) & 0xff);
    bytes[25] = char((words >> 16) & 0xff);
    bytes[26] = char((words >> 8) & 0xff);
    bytes[27] = char(words & 0xff);
    return bytes;
}

bool write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void sendEscape(QWidget *widget)
{
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(widget, &release);
}

QDialog *chooseShapefile(ConfigDeveloperToolsView *page,
                         QPushButton *start, const QString &path)
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    start->click();
    QFileDialog *picker = nullptr;
    if (!waitFor([&] {
            picker = visible<QFileDialog>(
                page, "DeveloperShapefilePolyInputDialog");
            return picker != nullptr;
        })) {
        return nullptr;
    }
    QLineEdit *name = find<QLineEdit>(picker, "fileNameEdit");
    if (!name)
        return nullptr;
    name->setText(path);
    const QStringList selected = picker->selectedFiles();
    if (selected.size() != 1
        || QDir::cleanPath(selected.first()) != QDir::cleanPath(path)) {
        picker->reject();
        return nullptr;
    }
    if (!QMetaObject::invokeMethod(
            picker, "accept", Qt::DirectConnection)) {
        picker->reject();
        return nullptr;
    }
    QDialog *consent = nullptr;
    waitFor([&] {
        consent = visible<QDialog>(
            page, "DeveloperShapefilePolyConfirmation");
        return consent != nullptr;
    });
    return consent;
}
} // namespace

int RunShapefilePolyRuntimeAudit()
{
    int failures = 0;
    const auto expect = [&](bool condition, const char *why) {
        if (!condition) {
            ++failures;
            qCritical() << "Shapefile POLY runtime:" << why;
        }
    };

    QTemporaryDir directory;
    expect(directory.isValid(), "temporary fixture directory unavailable");
    if (!directory.isValid())
        return 1;

    const QByteArray pointBytes = shapefile(1, {pointRecord(12.5, 34.25)});
    const QVector<QPointF> firstTriangle{
        {10, 20}, {11, 20}, {11, 21}, {10, 20}};
    const QVector<QPointF> secondTriangle{
        {-2, 3}, {-1, 3}, {-1, 4}, {-2, 3}};
    const QByteArray lineBytes = shapefile(3, {
        polyLineRecord(firstTriangle), polyLineRecord(secondTriangle)});
    const QString pointPath = directory.filePath(QStringLiteral("point.shp"));
    const QString linePath = directory.filePath(QStringLiteral("lines.shp"));
    const QString firstOutput = directory.filePath(QStringLiteral("poly-1.poly"));
    const QString secondOutput = directory.filePath(QStringLiteral("poly-2.poly"));
    expect(write(pointPath, pointBytes) && write(linePath, lineBytes)
               && write(firstOutput, QByteArray("OLD-POLY")),
           "could not create isolated SHP/output fixtures");
    if (!QFileInfo::exists(pointPath) || !QFileInfo::exists(linePath))
        return 1;

    MainWindow *main = MainWindow::instance();
    QAction *action = find<QAction>(main, "actionDeveloperTools");
    expect(action, "shared Developer route missing");
    if (!action)
        return 1;
    action->trigger();
    QApplication::processEvents();
    auto *page = main->findChild<ConfigDeveloperToolsView *>();
    auto *controller = page
        ? page->findChild<ShapefilePolyController *>() : nullptr;
    auto *start = find<QPushButton>(page, "ConvertShapefileToPolyButton");
    auto *split = find<QPushButton>(page, "SplitDataFlashLogButton");
    expect(page && page->ActionCount() == 32
               && page->ImplementedActionCount() == 28,
           "Developer inventory is not 28 of 32");
    expect(controller && start && split && start->isEnabled(),
           "Shapefile POLY action/controller is unavailable offline");
    if (!controller || !start || !split)
        return 1;

    start->click();
    QFileDialog *picker = nullptr;
    expect(waitFor([&] {
        picker = visible<QFileDialog>(
            page, "DeveloperShapefilePolyInputDialog");
        return picker != nullptr;
    }), "source picker did not open");
    expect(picker && picker->fileMode() == QFileDialog::ExistingFile
               && picker->nameFilters().join(QLatin1Char(' ')).contains(
                   QStringLiteral("*.shp *.SHP"))
               && !split->isEnabled(),
           "source picker contract or shared file gate is wrong");
    if (picker)
        picker->reject();
    expect(waitFor([&] { return !controller->busy(); }),
           "picker Cancel did not release the workflow");
    expect(read(firstOutput) == QByteArray("OLD-POLY")
               && !QFileInfo::exists(secondOutput),
           "picker Cancel changed an output");

    QDialog *consent = chooseShapefile(page, start, pointPath);
    expect(consent, "Point SHP did not reach exact-plan consent");
    if (!consent)
        return 1;
    auto *buttons = consent->findChild<QDialogButtonBox *>();
    auto *cancelButton = buttons
        ? buttons->button(QDialogButtonBox::Cancel) : nullptr;
    auto *yesButton = buttons
        ? buttons->button(QDialogButtonBox::Yes) : nullptr;
    auto *tree = find<QTreeWidget>(consent, "DeveloperShapefilePolyOutputs");
    auto *summary = find<QLabel>(consent, "DeveloperShapefilePolySummary");
    expect(consent->windowTitle() == QStringLiteral("Convert Shapefile to POLY")
               && cancelButton && cancelButton->isDefault()
               && yesButton && !yesButton->isDefault(),
           "conversion consent is not default-Cancel");
    expect(tree && tree->topLevelItemCount() == 1
               && tree->topLevelItem(0)->text(0) == QStringLiteral("Replace")
               && tree->topLevelItem(0)->text(1) == QStringLiteral("1")
               && tree->topLevelItem(0)->text(2) == firstOutput,
           "Point SHP exact output plan is not visible");
    expect(summary && summary->textFormat() == Qt::PlainText
               && summary->text().contains(pointPath)
               && summary->text().contains(directory.path())
               && summary->text().contains(QStringLiteral("Points: 1"))
               && summary->text().contains(QStringLiteral("WGS84")),
           "source, directory, point count or projection is absent from consent");
    sendEscape(consent);
    expect(waitFor([&] { return !controller->busy(); }),
           "consent Escape did not cancel conversion");
    expect(read(firstOutput) == QByteArray("OLD-POLY")
               && !QFileInfo::exists(secondOutput),
           "consent Escape changed an output");

    consent = chooseShapefile(page, start, linePath);
    expect(consent, "Polyline SHP did not reach consent");
    if (!consent)
        return 1;
    tree = find<QTreeWidget>(consent, "DeveloperShapefilePolyOutputs");
    expect(tree && tree->topLevelItemCount() == 2
               && tree->topLevelItem(0)->text(0) == QStringLiteral("Replace")
               && tree->topLevelItem(0)->text(2) == firstOutput
               && tree->topLevelItem(1)->text(0) == QStringLiteral("Create")
               && tree->topLevelItem(1)->text(2) == secondOutput,
           "Polyline create/replace plan differs from exact destinations");
    const QString screenshot =
        qEnvironmentVariable("APM_SHAPEFILE_POLY_AUDIT_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        expect(consent->grab().save(screenshot),
               "conversion consent screenshot failed");
    }
    auto *convert = find<QPushButton>(
        consent, "DeveloperShapefilePolyConfirmButton");
    expect(convert && convert->text() == QStringLiteral("Convert and replace"),
           "affirmative conversion control is missing");
    if (!convert)
        return 1;
    convert->click();
    expect(waitFor([&] { return !controller->busy(); }),
           "confirmed Polyline conversion did not finish");

    const QByteArray expectedFirst(
        "#Shap to Poly - Mission Planner\r\n"
        "20\t10\r\n20\t11\r\n21\t11\r\n20\t10\r\n");
    const QByteArray expectedSecond(
        "#Shap to Poly - Mission Planner\r\n"
        "3\t-2\r\n3\t-1\r\n4\t-1\r\n3\t-2\r\n");
    expect(read(firstOutput) == expectedFirst
               && read(secondOutput) == expectedSecond,
           "published POLY bytes/order/CRLF differ from the frozen plan");
    expect(read(pointPath) == pointBytes && read(linePath) == lineBytes,
           "conversion changed a source shapefile");
    expect(!QFileInfo::exists(directory.filePath(QStringLiteral("point.shx")))
               && !QFileInfo::exists(directory.filePath(QStringLiteral("point.dbf")))
               && !QFileInfo::exists(directory.filePath(QStringLiteral("lines.shx")))
               && !QFileInfo::exists(directory.filePath(QStringLiteral("lines.dbf"))),
           "conversion created or changed optional shapefile sidecars");

    FlightPlannerPolygonModel firstPolygon;
    FlightPlannerPolygonModel secondPolygon;
    expect(firstPolygon.LoadPolygon(firstOutput)
               && secondPolygon.LoadPolygon(secondOutput),
           "published POLY files do not round-trip through Flight Planner");
    expect(firstPolygon.Count() == 3 && secondPolygon.Count() == 3,
           "closed triangles did not round-trip as three planner vertices");
    if (firstPolygon.Count() == 3) {
        const auto &points = firstPolygon.DrawnPolygon();
        expect(qAbs(points.at(0).latitude - 20.0) < 1e-12
                   && qAbs(points.at(0).longitude - 10.0) < 1e-12
                   && qAbs(points.at(2).latitude - 21.0) < 1e-12
                   && qAbs(points.at(2).longitude - 11.0) < 1e-12,
               "Flight Planner round-trip changed coordinate order");
    }
    expect(start->isEnabled() && split->isEnabled(),
           "shared Developer file gate did not recover after conversion");
    if (!screenshot.isEmpty()) {
        expect(page->grab().save(screenshot + QStringLiteral(".page.png")),
               "Developer result screenshot failed");
    }

    qInfo() << "Shapefile POLY runtime audit failures:" << failures;
    return failures ? 1 : 0;
}
