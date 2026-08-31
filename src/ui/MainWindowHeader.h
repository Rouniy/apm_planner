#ifndef MAINWINDOWHEADER_H
#define MAINWINDOWHEADER_H

#include <QPointer>
#include <QWidget>

class QAction;
class QButtonGroup;
class QComboBox;
class QLabel;
class QMenu;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QToolButton;
class UASInterface;

class MainWindowHeader final : public QWidget
{
    Q_OBJECT
public:
    explicit MainWindowHeader(QWidget *parent = nullptr);

    void setNavigationActions(QAction *data,
                              QAction *plan,
                              QAction *setup,
                              QAction *config,
                              QAction *simulation,
                              QAction *help);
    void setToolsMenu(QMenu *menu);

    void disableConnectWidget(bool disable);
    void overrideDisableConnectWidget(bool disable);
    void startAnimation();
    void stopAnimation();

signals:
    void configureLinkRequested(int linkId);

private slots:
    void refreshLinks();
    void updateCurrentLink();
    void toggleConnection();
    void rebuildVehicleList();
    void activeVehicleChanged(UASInterface *uas);

private:
    QToolButton *addNavigationButton(const QString &name,
                                     const QString &objectName,
                                     QAction *action);
    int currentLinkId() const;

    QWidget *m_navigationHost = nullptr;
    QButtonGroup *m_navigationGroup = nullptr;
    QToolButton *m_toolsButton = nullptr;
    QComboBox *m_portCombo = nullptr;
    QSpinBox *m_baudSpin = nullptr;
    QComboBox *m_vehicleCombo = nullptr;
    QPushButton *m_connectButton = nullptr;
    QLabel *m_connectionStatus = nullptr;
    QProgressBar *m_connectionProgress = nullptr;
    bool m_disableOverride = false;
};

#endif
