#ifndef MAVLINKSERIALTCPBRIDGEWINDOW_H
#define MAVLINKSERIALTCPBRIDGEWINDOW_H

#include <QPointer>
#include <QWidget>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDialog;
class QLabel;
class QPushButton;
class QSpinBox;
class MavlinkSerialTcpBridgeService;

/**
 * Modeless owner of one explicitly confirmed MAVLink SERIAL_CONTROL bridge.
 *
 * Transport and exact-target policy stay in the application-owned service.
 * This window stops only the operation ID that it admitted; closing a second
 * observer can never stop another window's bridge.
 */
class MavlinkSerialTcpBridgeWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit MavlinkSerialTcpBridgeWindow(QWidget *parent = nullptr);
    ~MavlinkSerialTcpBridgeWindow() override;

    void setService(MavlinkSerialTcpBridgeService *service);
    bool ownsOperation() const noexcept;
    quint64 ownedOperationId() const noexcept { return m_ownedOperationId; }
    bool isClosing() const noexcept { return m_closing; }

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildUi();
    void toggleBridge();
    void startBridge();
    void stopOwnedBridge(const QString &reason);
    void dismissPrompt();
    void refresh();
    QString selectedDeviceLabel() const;
    void setStatus(const QString &status);

    QPointer<MavlinkSerialTcpBridgeService> m_service;
    QPointer<QDialog> m_prompt;
    quint64 m_bindingRevision = 0;
    quint64 m_promptRevision = 0;
    quint64 m_ownedOperationId = 0;
    bool m_preparing = false;
    bool m_closing = false;
    bool m_refreshing = false;

    QLabel *m_target = nullptr;
    QComboBox *m_device = nullptr;
    QComboBox *m_baud = nullptr;
    QSpinBox *m_listenPort = nullptr;
    QCheckBox *m_allowRemote = nullptr;
    QPushButton *m_toggle = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_tcpToVehicle = nullptr;
    QLabel *m_vehicleToTcp = nullptr;
    QLabel *m_dropped = nullptr;
};

#endif // MAVLINKSERIALTCPBRIDGEWINDOW_H
