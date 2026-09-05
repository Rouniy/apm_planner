#ifndef MAVLINKSIGNINGWINDOW_H
#define MAVLINKSIGNINGWINDOW_H

#include <QByteArray>
#include <QPointer>
#include <QVector>
#include <QWidget>
#include <functional>

class MavAuthKeyService;
class QCloseEvent;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

/** Modeless local-only key manager. Never provisions or disables vehicle signing. */
class MavlinkSigningWindow final : public QWidget
{
    Q_OBJECT
public:
    struct Connection {
        int linkId = -1;
        QString profileId, name;
        QPointer<QObject> identity;
        quint64 revision = 0;
        bool connected = false, required = false, ready = false;
        QString fingerprint, keyName, error;
        quint64 signedReceived = 0;
    };
    using Connections = std::function<QVector<Connection>()>;
    using Activate = std::function<bool(const Connection &, const QString &,
                                        const QByteArray &, QString *)>;
    explicit MavlinkSigningWindow(MavAuthKeyService *service, Connections connections,
                                  Activate activate, QWidget *parent = nullptr);
    ~MavlinkSigningWindow() override;
    bool isClosing() const { return m_closed; }
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    void refresh();
    void openVault(bool create);
    void addKey();
    void deleteKey();
    void useKey();
    void beginActivation(const Connection &expected, const QString &keyName, quint64 revision);
    void clearSecretInputs();
    void setStatus(const QString &text);
    bool currentConnection(const Connection &expected, Connection *current);
    QPointer<MavAuthKeyService> m_service;
    Connections m_connections;
    Activate m_activate;
    QVector<Connection> m_snapshot;
    quint64 m_selectionRevision = 0;
    bool m_closed = false;
    bool m_refreshing = false;
    bool m_activationPending = false;
    QLineEdit *m_master = nullptr;
    QLineEdit *m_confirmation = nullptr;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_seed = nullptr;
    QCheckBox *m_showSeed = nullptr;
    QListWidget *m_keys = nullptr;
    QComboBox *m_connection = nullptr;
    QLabel *m_vaultStatus = nullptr;
    QLabel *m_connectionStatus = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_create = nullptr;
    QPushButton *m_unlock = nullptr;
    QPushButton *m_lock = nullptr;
    QPushButton *m_add = nullptr;
    QPushButton *m_delete = nullptr;
    QPushButton *m_use = nullptr;
};

#endif
