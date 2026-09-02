#ifndef MAINWINDOWHEADER_H
#define MAINWINDOWHEADER_H

#include <QPointer>
#include <QWidget>

class QAction;
class QButtonGroup;
class QCheckBox;
class QComboBox;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
class QEnterEvent;
#endif
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

    bool autoHideEnabled() const;
    static constexpr int headerHeightFor(bool autoHide, bool hovered) noexcept
    {
        return autoHide && !hovered ? 7 : 64;
    }

    void setNavigationActions(QAction *data,
                              QAction *plan,
                              QAction *setup,
                              QAction *config,
                              QAction *simulation,
                              QAction *help);
    void setToolsMenu(QMenu *menu);
    void setConnectionOptionsAction(QAction *action);

    void disableConnectWidget(bool disable);
    void overrideDisableConnectWidget(bool disable);
    void startAnimation();
    void stopAnimation();

public slots:
    void setAutoHideEnabled(bool enabled);
    void toggleConnection();
    void setDefaultBaudRate(int baud);

signals:
    void configureLinkRequested(int linkId);
    void autoHideEnabledChanged(bool enabled);
    void fullScreenRequested();

protected:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    void enterEvent(QEnterEvent *event) override;
#else
    void enterEvent(QEvent *event) override;
#endif
    void leaveEvent(QEvent *event) override;

private slots:
    void refreshLinks();
    void updateCurrentLink();
    void applyBaudRate();
    void rebuildTargetList();
    void syncCurrentTarget();
    void selectTarget(int index);
    void activeVehicleChanged(UASInterface *uas);

private:
    QToolButton *addNavigationButton(const QString &name,
                                     const QString &objectName,
                                     QAction *action);
    int currentLinkId() const;

    QWidget *m_navigationHost = nullptr;
    QWidget *m_connectionPanel = nullptr;
    QButtonGroup *m_navigationGroup = nullptr;
    QToolButton *m_toolsButton = nullptr;
    QComboBox *m_portCombo = nullptr;
    QSpinBox *m_baudSpin = nullptr;
    QComboBox *m_targetCombo = nullptr;
    QCheckBox *m_autoConnectCheckBox = nullptr;
    QPushButton *m_connectButton = nullptr;
    QLabel *m_connectionStatus = nullptr;
    QProgressBar *m_connectionProgress = nullptr;
    QAction *m_autoHideAction = nullptr;
    QAction *m_connectionOptionsAction = nullptr;
    bool m_disableOverride = false;
    bool m_autoHideEnabled = false;
    bool m_headerHovered = false;

    void updateHeaderHeight();
};

#endif
