#ifndef CONFIGELEVATIONSOURCESVIEW_H
#define CONFIGELEVATIONSOURCESVIEW_H

#include <QList>
#include <QStringList>
#include <QWidget>

class ConfigElevationSourcesViewModel;
class BackstagePage;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;

class ConfigElevationSourcesView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigElevationSourcesView(QWidget *parent = nullptr);
    ConfigElevationSourcesView(ConfigElevationSourcesViewModel *viewModel,
                               QWidget *parent = nullptr);

    ConfigElevationSourcesViewModel *viewModel() const { return m_viewModel; }

private slots:
    void browse();
    void clearSaved();
    void syncDirectory(const QString &path);
    void syncStatus(const QString &status);
    void syncNativeStatus(const QString &status);
    void syncProgress(int value, int maximum);
    void syncBusy(bool busy);
    void syncElevationRows();
    void syncRasterRows();

private:
    void buildUi();
    static void configureTable(QTableWidget *table,
                               const QStringList &headers,
                               const QList<int> &widths);

    ConfigElevationSourcesViewModel *m_viewModel = nullptr;
    QLineEdit *m_directoryEdit = nullptr;
    QPushButton *m_browseButton = nullptr;
    QPushButton *m_saveButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QTableWidget *m_elevationTable = nullptr;
    QTableWidget *m_rasterTable = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_nativeStatusLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
};

BackstagePage configElevationSourcesBackstagePage();

#endif
