#include "MavlinkSigningWindow.h"
#include "services/MavAuthKeyService.h"
#include "services/MavAuthKeyStore.h"

#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <utility>

namespace {
void cleanse(QString &secret)
{
    // Best effort for this owned copy. Qt/platform input methods may maintain
    // their own transient buffers; no promise of a secure-memory GUI is made.
    if (!secret.isEmpty()) {
        volatile ushort *data = reinterpret_cast<volatile ushort *>(secret.data());
        for (int i = 0; i < secret.size(); ++i) data[i] = 0;
    }
    secret.clear();
}
QString takeSecret(QLineEdit *edit)
{
    QString value = edit->text();
    edit->setText(QString()); // resets the edit's undo history as well as its text
    value.detach();
    return value;
}
bool sameConnection(const MavlinkSigningWindow::Connection &a,
                    const MavlinkSigningWindow::Connection &b)
{
    return a.identity && b.identity && a.identity == b.identity
        && a.linkId == b.linkId && a.profileId == b.profileId;
}
bool eligibleForProvision(const MavlinkSigningWindow::Connection &connection)
{
    const auto &target = connection.provisioningTarget;
    return connection.connected && !connection.required && !connection.provisioningUnconfirmed
        && connection.error.isEmpty() && connection.provisioningError.isEmpty() && target.isValid()
        && target.linkId == connection.linkId && target.profileId == connection.profileId
        && target.identity == connection.identity && target.revision == connection.revision;
}
class WrappedStatusLabel final : public QLabel
{
public:
    explicit WrappedStatusLabel(QWidget *parent) : QLabel(parent)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
    void updateMinimumHeight()
    {
        if (m_updatingHeight) return;
        m_updatingHeight = true;
        ensurePolished();
        // QLabel::heightForWidth includes the widget's explicit minimum size.
        // Remove our previous reservation before measuring shorter text, or
        // each warning can only grow the label and never release that space.
        // Raising the new minimum may synchronously resize the label, hence
        // the guard above also prevents recursive layout measurement.
        if (minimumHeight() != 0) setMinimumHeight(0);
        const int required = qMax(0, heightForWidth(qMax(1, width())));
        if (minimumHeight() != required) setMinimumHeight(required);
        m_updatingHeight = false;
    }
    void setText(const QString &text)
    {
        QLabel::setText(text);
        updateMinimumHeight();
    }
protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        updateMinimumHeight();
    }
private:
    bool m_updatingHeight = false;
};

WrappedStatusLabel *plainLabel(const QString &name, QWidget *parent)
{
    auto *label = new WrappedStatusLabel(parent);
    label->setObjectName(name);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}
QPushButton *button(const QString &name, const QString &text, QWidget *parent)
{
    auto *result = new QPushButton(text, parent);
    result->setObjectName(name);
    return result;
}
}

MavlinkSigningWindow::MavlinkSigningWindow(MavAuthKeyService *service, Connections connections,
                                         Activate activate, QWidget *parent)
    : QWidget(parent, Qt::Window), m_service(service), m_connections(std::move(connections)),
      m_activate(std::move(activate))
{
    setObjectName(QStringLiteral("MavlinkSigningWindow"));
    setWindowTitle(tr("MAVLink Signing — keys and initial provision (partial)"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(720, 720);
    auto *windowLayout = new QVBoxLayout(this);
    windowLayout->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName("SigningContentScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scroll);
    content->setObjectName("SigningContent");
    auto *layout = new QVBoxLayout(content);
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    scroll->setWidget(content);
    windowLayout->addWidget(scroll);
    auto *banner = plainLabel("SigningLocalOnlyBanner", this);
    banner->setText(tr("Use locally does not send keys to a vehicle: it selects an existing key for a disconnected connection. "
                       "Initial provision is different: it sends a secret key in cleartext over a private, dedicated physical channel, "
                       "without a vehicle acknowledgement. Rekey and disable are not available.\n"
                       "Signing authenticates; it does not encrypt telemetry. There is no recovery for a lost master password."));
    layout->addWidget(banner);

    auto *vault = new QGroupBox(tr("Encrypted local vault"), this);
    auto *vaultLayout = new QFormLayout(vault);
    m_master = new QLineEdit(vault);
    m_master->setObjectName("SigningMasterPassword");
    m_master->setEchoMode(QLineEdit::Password);
    m_master->setMaxLength(MavAuthKeyStore::MaximumPassphraseBytes);
    m_confirmation = new QLineEdit(vault);
    m_confirmation->setObjectName("SigningConfirmPassword");
    m_confirmation->setEchoMode(QLineEdit::Password);
    m_confirmation->setMaxLength(MavAuthKeyStore::MaximumPassphraseBytes);
    vaultLayout->addRow(tr("Master password:"), m_master);
    vaultLayout->addRow(tr("Confirm (create only):"), m_confirmation);
    auto *vaultButtons = new QHBoxLayout;
    m_create = button("SigningCreateVault", tr("Create new vault"), vault);
    m_unlock = button("SigningUnlockVault", tr("Unlock existing vault"), vault);
    m_lock = button("SigningLockVault", tr("Lock local vault"), vault);
    m_lock->setToolTip(tr("Locks the local key store. It does not disable vehicle signing or revoke keys already in active connections."));
    for (auto *item : {m_create, m_unlock, m_lock}) vaultButtons->addWidget(item);
    vaultLayout->addRow(vaultButtons);
    m_vaultStatus = plainLabel("SigningVaultStatus", vault);
    vaultLayout->addRow(m_vaultStatus);
    layout->addWidget(vault);

    auto *keyGroup = new QGroupBox(tr("Named signing keys"), this);
    auto *keyLayout = new QFormLayout(keyGroup);
    m_keys = new QListWidget(keyGroup);
    m_keys->setObjectName("SigningKeyList");
    m_keys->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    keyLayout->addRow(m_keys);
    m_name = new QLineEdit(keyGroup);
    m_name->setObjectName("SigningKeyName");
    m_name->setMaxLength(MavAuthKeyStore::MaximumNameBytes);
    m_seed = new QLineEdit(keyGroup);
    m_seed->setObjectName("SigningKeySeed");
    m_seed->setEchoMode(QLineEdit::Password);
    m_seed->setMaxLength(MavAuthKeyStore::MaximumSeedBytes);
    m_seed->setToolTip(tr("Use a long, unique signing seed. Spaces and letter case are significant; verify it before adding."));
    keyLayout->addRow(tr("New key name:"), m_name);
    keyLayout->addRow(tr("Seed (SHA-256 UTF-8):"), m_seed);
    m_showSeed = new QCheckBox(tr("Show seed (check for typing errors)"), keyGroup);
    m_showSeed->setObjectName("SigningShowSeed");
    connect(m_showSeed, &QCheckBox::toggled, m_seed, [this](bool show) {
        m_seed->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
    });
    keyLayout->addRow(m_showSeed);
    auto *keyButtons = new QHBoxLayout;
    m_add = button("SigningAddKey", tr("Add key"), keyGroup);
    m_delete = button("SigningDeleteKey", tr("Delete selected key…"), keyGroup);
    keyButtons->addWidget(m_add); keyButtons->addWidget(m_delete);
    keyLayout->addRow(keyButtons);
    layout->addWidget(keyGroup, 1);

    auto *connectionGroup = new QGroupBox(tr("Use an existing key locally"), this);
    auto *connectionLayout = new QVBoxLayout(connectionGroup);
    m_connection = new QComboBox(connectionGroup);
    m_connection->setObjectName("SigningConnection");
    connectionLayout->addWidget(m_connection);
    m_connectionStatus = plainLabel("SigningConnectionStatus", connectionGroup);
    m_connectionStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    connectionLayout->addWidget(m_connectionStatus);
    m_use = button("SigningUseLocally", tr("Use selected key locally"), connectionGroup);
    connectionLayout->addWidget(m_use);
    layout->addWidget(connectionGroup);

    auto *vehicleButtons = new QHBoxLayout;
    m_provision = button("SigningProvisionVehicle", tr("Initial provision vehicle…"), this);
    auto *disable = button("SigningDisableVehicle", tr("Disable vehicle signing (unavailable)"), this);
    m_provision->setEnabled(false); disable->setEnabled(false);
    m_provision->setToolTip(tr("Only for an operator-known unprovisioned vehicle on a private, dedicated channel, "
                              "with a fresh disarmed target. Sends a cleartext key once, with no acknowledgement or automatic retry."));
    disable->setToolTip(tr("Vehicle signing cannot be disabled here. Deleting or locking a local key does not change the vehicle."));
    vehicleButtons->addWidget(m_provision); vehicleButtons->addWidget(disable);
    layout->addLayout(vehicleButtons);
    m_status = plainLabel("SigningOperationStatus", this);
    layout->addWidget(m_status);

    connect(m_create, &QPushButton::clicked, this, [this] { openVault(true); });
    connect(m_unlock, &QPushButton::clicked, this, [this] { openVault(false); });
    connect(m_lock, &QPushButton::clicked, this, [this] {
        if (!m_service || m_closed) return;
        clearSecretInputs();
        const QPointer<MavlinkSigningWindow> guard(this);
        const QPointer<MavAuthKeyService> service = m_service;
        const auto token = service->lock();
        if (guard && !token) setStatus(service ? service->lastError() : tr("Vault service unavailable."));
    });
    connect(m_add, &QPushButton::clicked, this, &MavlinkSigningWindow::addKey);
    connect(m_delete, &QPushButton::clicked, this, &MavlinkSigningWindow::deleteKey);
    connect(m_use, &QPushButton::clicked, this, &MavlinkSigningWindow::useKey);
    connect(m_provision, &QPushButton::clicked, this, &MavlinkSigningWindow::provisionVehicle);
    connect(m_keys, &QListWidget::currentRowChanged, this, [this] {
        if (!m_refreshing) { ++m_selectionRevision; refresh(); }
    });
    connect(m_connection, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        if (!m_refreshing) { ++m_selectionRevision; refresh(); }
    });
    if (service) {
        connect(service, &MavAuthKeyService::stateChanged, this, &MavlinkSigningWindow::refresh);
        connect(service, &MavAuthKeyService::operationFinished, this, [this](quint64, bool ok, const QString &error) {
            ++m_selectionRevision;
            if (!m_activationPending) setStatus(ok ? tr("Local vault operation completed.") : error);
            m_activationPending = false;
            refresh();
        });
        connect(service, &QObject::destroyed, this, [this] { m_service.clear(); refresh(); });
    }
    auto *timer = new QTimer(this);
    timer->setInterval(350);
    connect(timer, &QTimer::timeout, this, &MavlinkSigningWindow::refresh);
    timer->start();
    refresh();
}

MavlinkSigningWindow::~MavlinkSigningWindow() { clearSecretInputs(); }
void MavlinkSigningWindow::setProvisioner(Activate callback)
{
    m_provisioner = std::move(callback);
    ++m_selectionRevision;
    refresh();
}
void MavlinkSigningWindow::setStatus(const QString &text)
{
    static_cast<WrappedStatusLabel *>(m_status)->setText(text);
}
void MavlinkSigningWindow::clearSecretInputs()
{
    m_showSeed->setChecked(false);
    for (auto *edit : {m_master, m_confirmation, m_seed}) {
        QString text = takeSecret(edit);
        cleanse(text);
    }
}
void MavlinkSigningWindow::closeEvent(QCloseEvent *event)
{
    m_closed = true;
    ++m_selectionRevision;
    clearSecretInputs();
    QWidget::closeEvent(event);
}

void MavlinkSigningWindow::refresh()
{
    if (m_closed || m_refreshing) return;
    m_refreshing = true;
    const QPointer<MavlinkSigningWindow> guard(this);
    const auto provider = m_connections;
    QVector<Connection> current;
    try { if (provider) current = provider(); }
    catch (...) { if (guard) setStatus(tr("Connection metadata is unavailable.")); }
    if (!guard || m_closed) return;
    const int previousIndex = m_connection->currentIndex();
    const Connection previous = previousIndex >= 0 && previousIndex < m_snapshot.size()
        ? m_snapshot.at(previousIndex) : Connection{};
    {
        const QSignalBlocker blocker(m_connection);
        bool rebuild = m_snapshot.size() != current.size();
        for (int i = 0; !rebuild && i < current.size(); ++i)
            rebuild = !sameConnection(m_snapshot.at(i), current.at(i));
        if (rebuild) m_connection->clear();
        int selected = -1;
        for (int i = 0; i < current.size(); ++i) {
            const QString text = QStringLiteral("%1 [%2]").arg(current.at(i).name, current.at(i).profileId.left(12));
            if (rebuild) m_connection->addItem(text);
            else if (m_connection->itemText(i) != text) m_connection->setItemText(i, text);
            if (sameConnection(current.at(i), previous)) selected = i;
        }
        if (rebuild) m_connection->setCurrentIndex(selected >= 0 ? selected : current.isEmpty() ? -1 : 0);
        m_snapshot = current;
    }
    const bool alive = m_service && !m_service->isShuttingDown();
    const bool busy = alive && m_service->busy();
    const bool unlocked = alive && m_service->isUnlocked();
    const QStringList names = unlocked ? m_service->keyNames() : QStringList{};
    const QString chosen = m_keys->currentItem() ? m_keys->currentItem()->text() : QString();
    {
        const QSignalBlocker blocker(m_keys);
        QStringList existing;
        for (int i = 0; i < m_keys->count(); ++i) existing.append(m_keys->item(i)->text());
        if (existing != names) {
            m_keys->clear(); m_keys->addItems(names);
            for (int i = 0; i < names.size(); ++i)
                m_keys->item(i)->setToolTip(QStringLiteral("<pre>%1</pre>").arg(
                    tr("Exact, case-sensitive name: \"%1\"").arg(names.at(i)).toHtmlEscaped()));
            const int selected = names.indexOf(chosen);
            m_keys->setCurrentRow(selected >= 0 ? selected : names.isEmpty() ? -1 : 0);
        }
    }
    m_vaultStatus->setText(!alive ? tr("Vault service unavailable.")
        : tr("%1 — %2 / %3 keys%4").arg(unlocked ? tr("Unlocked") : tr("Locked"))
            .arg(names.size()).arg(MavAuthKeyStore::MaximumKeys).arg(busy ? tr(" — working…") : QString()));
    m_create->setEnabled(alive && !busy && !unlocked);
    m_unlock->setEnabled(alive && !busy && !unlocked);
    m_lock->setEnabled(alive && !busy && unlocked);
    m_add->setEnabled(alive && !busy && unlocked && names.size() < MavAuthKeyStore::MaximumKeys);
    m_delete->setEnabled(alive && !busy && unlocked && m_keys->currentItem());
    m_master->setEnabled(alive && !busy && !unlocked);
    m_confirmation->setEnabled(alive && !busy && !unlocked);
    m_name->setEnabled(alive && !busy && unlocked);
    m_seed->setEnabled(alive && !busy && unlocked);
    m_showSeed->setEnabled(alive && !busy && unlocked);
    if (!unlocked) {
        m_showSeed->setChecked(false);
        QString oldSeed = takeSecret(m_seed);
        cleanse(oldSeed);
    }
    const int index = m_connection->currentIndex();
    const Connection selected = index >= 0 && index < current.size() ? current.at(index) : Connection{};
    const bool usable = selected.identity && selected.linkId >= 0 && !selected.profileId.isEmpty()
        && !selected.connected && selected.error.isEmpty();
    m_use->setEnabled(alive && !busy && unlocked && m_keys->currentItem() && usable && bool(m_activate));
    m_provision->setEnabled(alive && !busy && unlocked && m_keys->currentItem()
        && bool(m_provisioner) && eligibleForProvision(selected));
    m_connectionStatus->setText(index < 0 ? tr("No physical connection is available.")
        : tr("%1 | %2 | %3\nProfile: %4\nRequired fingerprint: %5\nCurrent local key: %6\n"
             "Authenticated packets (shared key): %7%8")
            .arg(selected.connected ? tr("Connected — local key selection requires disconnect") : tr("Disconnected"))
            .arg(selected.required ? tr("Signing required") : tr("Not protected"))
            .arg(selected.ready ? tr("Local key ready") : tr("Local key not ready"))
            .arg(selected.profileId, selected.fingerprint.isEmpty() ? tr("none") : selected.fingerprint,
                 selected.keyName.isEmpty() ? tr("none") : selected.keyName)
            .arg(selected.signedReceived).arg(selected.error.isEmpty() ? QString() : "\n" + selected.error));
    if (index >= 0) {
        QString provisioningStatus;
        if (selected.provisioningUnconfirmed)
            provisioningStatus = tr("PROVISIONING UNCONFIRMED: a key transfer may have been attempted. "
                "Vehicle acceptance and persistence are unknown. The local profile remains signed-only; "
                "do not retry. No rekey, disable or recovery workflow is available.");
        else if (!selected.provisioningError.isEmpty())
            provisioningStatus = tr("Initial provision unavailable: %1").arg(selected.provisioningError);
        else if (eligibleForProvision(selected))
            provisioningStatus = tr("Initial provision candidate: system %1, component %2. "
                "Fresh disarmed telemetry is unauthenticated, not proof of vehicle identity.")
                .arg(selected.provisioningTarget.systemId).arg(selected.provisioningTarget.componentId);
        if (!provisioningStatus.isEmpty())
            m_connectionStatus->setText(m_connectionStatus->text() + "\n" + provisioningStatus);
    }
    // QGroupBox/layout minimumSizeHint does not reliably propagate a wrapped
    // QLabel's height-for-width. Reserve every actual text line explicitly;
    // the surrounding scroll area handles screens shorter than the content.
    static_cast<WrappedStatusLabel *>(m_connectionStatus)->updateMinimumHeight();
    static_cast<WrappedStatusLabel *>(m_vaultStatus)->updateMinimumHeight();
    m_refreshing = false;
}

void MavlinkSigningWindow::openVault(bool create)
{
    if (!m_service || m_closed) return;
    QString master = takeSecret(m_master);
    QString confirmation = takeSecret(m_confirmation);
    if (create && master != confirmation) {
        cleanse(master); cleanse(confirmation);
        setStatus(tr("The master password and confirmation do not match."));
        return;
    }
    cleanse(confirmation);
    const QPointer<MavlinkSigningWindow> guard(this);
    const QPointer<MavAuthKeyService> service = m_service;
    const auto token = create ? service->create(master) : service->unlock(master);
    cleanse(master);
    if (guard && !token) setStatus(service ? service->lastError() : tr("Vault service unavailable."));
}

void MavlinkSigningWindow::addKey()
{
    if (!m_service || m_closed) return;
    const QString name = m_name->text();
    QString seed = takeSecret(m_seed);
    m_showSeed->setChecked(false);
    const QPointer<MavlinkSigningWindow> guard(this);
    const QPointer<MavAuthKeyService> service = m_service;
    const auto token = service->addSeed(name, seed);
    cleanse(seed);
    if (guard && !token) setStatus(service ? service->lastError() : tr("Vault service unavailable."));
}

void MavlinkSigningWindow::deleteKey()
{
    if (!m_service || m_closed || !m_keys->currentItem()) return;
    const QString name = m_keys->currentItem()->text();
    const quint64 revision = m_selectionRevision;
    auto *question = new QMessageBox(QMessageBox::Warning, tr("Delete local signing key?"),
        tr("Delete the local key named '%1'?\n\nThis does NOT disable vehicle signing or revoke existing "
           "connection sessions. Losing the only copy may prevent future access to the vehicle.").arg(name),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    question->setObjectName("SigningDeleteConfirmation");
    question->setTextFormat(Qt::PlainText);
    question->setDefaultButton(QMessageBox::Cancel);
    question->setEscapeButton(QMessageBox::Cancel);
    question->setAttribute(Qt::WA_DeleteOnClose);
    question->setWindowModality(Qt::WindowModal);
    const QPointer<MavlinkSigningWindow> guard(this);
    connect(question, &QMessageBox::finished, this, [guard, name, revision](int result) {
        if (!guard || guard->m_closed || result != QMessageBox::Yes || !guard->m_service) return;
        const QPointer<MavAuthKeyService> service = guard->m_service;
        if (service->busy() || !service->isUnlocked() || !service->keyNames().contains(name)
            || guard->m_selectionRevision != revision) {
            guard->setStatus(tr("The vault changed before deletion; review the key list again."));
            return;
        }
        const auto token = service->removeKey(name);
        if (guard && !token) guard->setStatus(service ? service->lastError() : tr("Vault service unavailable."));
    });
    question->open();
}

bool MavlinkSigningWindow::currentConnection(const Connection &expected, Connection *result, bool provisioning)
{
    if (!expected.identity || expected.linkId < 0 || expected.profileId.isEmpty()) return false;
    const QPointer<MavlinkSigningWindow> guard(this);
    const auto provider = m_connections;
    QVector<Connection> current;
    try { if (provider) current = provider(); } catch (...) { return false; }
    if (!guard || m_closed) return false;
    int matches = 0;
    for (const auto &item : current) {
        if (item.linkId != expected.linkId) continue;
        if (!sameConnection(item, expected) || !item.error.isEmpty()
            || item.revision != expected.revision
            || item.required != expected.required || item.fingerprint != expected.fingerprint
            || item.provisioningUnconfirmed != expected.provisioningUnconfirmed) return false;
        if (provisioning) {
            if (!eligibleForProvision(expected) || !eligibleForProvision(item)
                || item.provisioningTarget != expected.provisioningTarget) return false;
        } else if (item.connected) return false;
        *result = item;
        ++matches;
    }
    return matches == 1;
}

void MavlinkSigningWindow::useKey()
{
    const int index = m_connection->currentIndex();
    if (m_closed || !m_service || !m_keys->currentItem() || index < 0 || index >= m_snapshot.size()) return;
    const Connection expected = m_snapshot.at(index);
    const QString keyName = m_keys->currentItem()->text();
    const quint64 revision = ++m_selectionRevision;
    const QPointer<MavlinkSigningWindow> guard(this);
    Connection fresh;
    if (!currentConnection(expected, &fresh)) {
        if (guard) setStatus(tr("The physical connection changed or is connected; select a disconnected connection again."));
        return;
    }
    if (!guard || m_closed || revision != m_selectionRevision || !m_service) return;
    if (!fresh.required) {
        auto *question = new QMessageBox(QMessageBox::Warning, tr("Require signing for this local connection?"),
            tr("Connection: %1\nProfile: %2\nSelected local key: \"%3\"\n\n"
               "Only continue if this vehicle is already provisioned with the selected key.\n\n"
               "This physical connection will require signed traffic, including after restart. "
               "An existing unsigned vehicle will no longer connect. This partial release has no supported "
               "reset or rekey workflow. No key will be sent to the vehicle.")
                .arg(fresh.name, fresh.profileId, keyName),
            QMessageBox::Yes | QMessageBox::Cancel, this);
        question->setObjectName("SigningUseConfirmation");
        question->setTextFormat(Qt::PlainText);
        question->setDefaultButton(QMessageBox::Cancel);
        question->setEscapeButton(QMessageBox::Cancel);
        question->setAttribute(Qt::WA_DeleteOnClose);
        question->setWindowModality(Qt::WindowModal);
        connect(question, &QMessageBox::finished, this, [guard, fresh, keyName, revision](int result) {
            if (guard && !guard->m_closed && result == QMessageBox::Yes
                && guard->m_selectionRevision == revision)
                guard->beginActivation(fresh, keyName, revision);
        });
        question->open();
        return;
    }
    beginActivation(fresh, keyName, revision);
}

void MavlinkSigningWindow::beginActivation(const Connection &expected, const QString &keyName,
                                           quint64 revision)
{
    const QPointer<MavlinkSigningWindow> guard(this);
    Connection fresh;
    if (m_closed || !m_service || revision != m_selectionRevision) return;
    if (!currentConnection(expected, &fresh)) {
        if (guard) setStatus(tr("Local activation cancelled: the connection changed after confirmation."));
        return;
    }
    if (!guard || m_closed || !m_service || revision != m_selectionRevision) return;
    m_activationPending = true;
    const QPointer<MavAuthKeyService> service = m_service;
    const auto token = service->requestKey(keyName,
        [guard, expected, keyName, revision](bool ok, const QByteArray &key, const QString &error) {
            if (!guard || guard->m_closed) return;
            if (!ok) { guard->setStatus(error); return; }
            Connection current;
            if (guard->m_selectionRevision != revision || !guard->currentConnection(expected, &current)) {
                if (guard) guard->setStatus(tr("Local activation cancelled: the connection or selection changed."));
                return;
            }
            if (!guard || guard->m_closed || guard->m_selectionRevision != revision) return;
            const auto activate = guard->m_activate;
            QString activationError;
            bool activated = false;
            try { if (activate) activated = activate(current, keyName, key, &activationError); }
            catch (...) { activationError = tr("The local connection could not accept the key."); }
            if (!guard || guard->m_closed) return;
            guard->setStatus(activated ? tr("Key selected locally; this connection remains signed-only across restart. "
                                            "No key was sent to the vehicle and no vehicle setting was changed.")
                : activationError.isEmpty() ? tr("The local connection did not accept the key.") : activationError);
        });
    if (guard && !token) {
        m_activationPending = false;
        setStatus(service ? service->lastError() : tr("Vault service unavailable."));
    }
}

void MavlinkSigningWindow::provisionVehicle()
{
    const int index = m_connection->currentIndex();
    if (m_closed || !m_service || !m_provisioner || !m_keys->currentItem()
        || index < 0 || index >= m_snapshot.size()) return;
    const Connection expected = m_snapshot.at(index);
    const QString keyName = m_keys->currentItem()->text();
    const quint64 revision = ++m_selectionRevision;
    const QPointer<MavlinkSigningWindow> guard(this);
    Connection fresh;
    if (!currentConnection(expected, &fresh, true)) {
        if (guard) setStatus(tr("Initial provision cancelled: no current eligible, connected and disarmed exact target."));
        return;
    }
    if (!guard || m_closed || revision != m_selectionRevision || !m_service) return;
    auto *question = new QMessageBox(QMessageBox::Warning, tr("Send an initial signing key without acknowledgement?"),
        tr("Connection: %1\nProfile: %2\nVehicle system: %3, component: %4\nSelected key: \"%5\"\n\n"
           "Continue ONLY for an operator-known unprovisioned vehicle using a direct cable or trusted PRIVATE, DEDICATED wired connection. "
           "Fresh disarmed heartbeat data is unauthenticated and does not prove vehicle identity. "
           "Listening/broadcast links are not eligible.\n\n"
           "The secret key is sent in CLEARTEXT: anyone observing this wire can steal it. "
           "The change may affect ALL vehicle channels, not just this connection. "
           "A hidden pre-existing key may be overwritten, preventing other ground stations from connecting; this is not a supported rekey operation.\n\n"
           "There is NO ACKNOWLEDGEMENT. Submission is always UNCONFIRMED, even if signed telemetry appears. "
           "The local profile remains signed-only across restart; there is no unsigned fallback. "
           "Do not retry. This release has no retry, rekey, disable or recovery workflow.")
            .arg(fresh.name, fresh.profileId).arg(fresh.provisioningTarget.systemId)
            .arg(fresh.provisioningTarget.componentId).arg(keyName),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    question->setObjectName("SigningProvisionConfirmation");
    question->setTextFormat(Qt::PlainText);
    question->setDefaultButton(QMessageBox::Cancel);
    question->setEscapeButton(QMessageBox::Cancel);
    question->button(QMessageBox::Yes)->setText(tr("Send key once — unconfirmed"));
    question->setAttribute(Qt::WA_DeleteOnClose);
    question->setWindowModality(Qt::WindowModal);
    connect(question, &QMessageBox::finished, this, [guard, fresh, keyName, revision](int result) {
        if (guard && !guard->m_closed && result == QMessageBox::Yes
            && guard->m_selectionRevision == revision)
            guard->beginProvisioning(fresh, keyName, revision);
    });
    question->open();
}

void MavlinkSigningWindow::beginProvisioning(const Connection &expected, const QString &keyName,
                                            quint64 revision)
{
    const QPointer<MavlinkSigningWindow> guard(this);
    Connection fresh;
    if (m_closed || !m_service || !m_provisioner || revision != m_selectionRevision) return;
    if (!currentConnection(expected, &fresh, true)) {
        if (guard) setStatus(tr("Initial provision cancelled: target eligibility changed after confirmation."));
        return;
    }
    if (!guard || m_closed || !m_service || revision != m_selectionRevision) return;
    m_activationPending = true;
    const QPointer<MavAuthKeyService> service = m_service;
    const auto token = service->requestKey(keyName,
        [guard, expected, keyName, revision](bool ok, const QByteArray &key, const QString &error) {
            if (!guard || guard->m_closed) return;
            if (!ok) { guard->setStatus(error); return; }
            Connection current;
            if (guard->m_selectionRevision != revision || !guard->currentConnection(expected, &current, true)) {
                if (guard) guard->setStatus(tr("Initial provision cancelled: target, connection or selection changed."));
                return;
            }
            if (!guard || guard->m_closed || guard->m_selectionRevision != revision) return;
            const auto provisioner = guard->m_provisioner;
            QString details;
            bool submitted = false;
            try { if (provisioner) submitted = provisioner(current, keyName, key, &details); }
            catch (...) { details = tr("The provisioning handler did not finish normally; the vehicle state is unknown."); }
            if (!guard || guard->m_closed) return;
            guard->setStatus(submitted
                ? tr("SUBMITTED — UNCONFIRMED. No vehicle acknowledgement exists; acceptance and persistence are unknown. "
                     "The profile remains signed-only. Do not retry; no rekey, disable or recovery workflow is available.")
                : tr("Provisioning was not confirmed. An attempt may have occurred; do not assume the vehicle is unchanged "
                     "or fall back to unsigned traffic. %1").arg(details));
            guard->refresh();
        });
    if (guard && !token) {
        m_activationPending = false;
        setStatus(service ? service->lastError() : tr("Vault service unavailable."));
    }
}
