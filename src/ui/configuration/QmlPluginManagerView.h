#ifndef QMLPLUGINMANAGERVIEW_H
#define QMLPLUGINMANAGERVIEW_H

#include <QPointer>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QmlPluginManager;

/** User-facing manager for the supported trusted QML extension system. */
class QmlPluginManagerView final : public QWidget
{
    Q_OBJECT

public:
    explicit QmlPluginManagerView(QmlPluginManager *manager,
                                  QWidget *parent = nullptr);

public slots:
    void Refresh();
    void Reload();
    void OpenSelected();
    void OpenUserFolder();

private:
    QString selectedPluginId() const;

    QPointer<QmlPluginManager> m_manager;
    QLabel *m_directory = nullptr;
    QTableWidget *m_plugins = nullptr;
    QPushButton *m_reload = nullptr;
    QPushButton *m_open = nullptr;
    QPushButton *m_openFolder = nullptr;
    QPlainTextEdit *m_diagnostics = nullptr;
};

#endif // QMLPLUGINMANAGERVIEW_H
