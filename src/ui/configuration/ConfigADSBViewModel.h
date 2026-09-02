#ifndef CONFIGADSBVIEWMODEL_H
#define CONFIGADSBVIEWMODEL_H

#include "ParamField.h"
#include "comm/AdsbIdentificationClient.h"
#include "core/parameters/ParameterStore.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>

/*
 * Mission Planner 10 SETUP > ADSB page state
 * (ViewModels/GCSViews/ConfigurationView/ConfigADSBViewModel.cs).
 *
 * Owns the page texts, the search rule, the parameter filter and the
 * identification workflow on top of a caller-owned AdsbIdentificationClient.
 * The widget layer (ConfigADSBView) owns dialogs and editors.
 */
class ConfigADSBViewModel final : public QObject
{
    Q_OBJECT

public:
    using Field = AdsbIdentificationClient::Field;

    static const QString Title;          // "ADSB"
    static const QString Intro;          // "ADS-B receiver / avoidance. Populated on connect."
    static const int SearchMinimumLength; // 2: shorter terms show everything

    explicit ConfigADSBViewModel(QObject *parent = nullptr);

    // --- MP10 static rules ---
    // StartsWith("ADSB_") || StartsWith("AVD_"), OrdinalIgnoreCase.
    static bool IsAdsbParameter(const QString &name);
    // ADSB_OPTIONS, ADSB_RF_CAPABLE, ADSB_RF_SELECT are bitmask editors in MP10.
    static bool IsForcedBitmaskParameter(const QString &name);
    // The ADS-B subset of a committed snapshot, ordered by name (case-insensitive).
    static QList<ConfigFriendlyParameterValue> FilterSnapshot(
        const QList<ParameterRecord> &records);
    // Trimmed search term, or empty when shorter than SearchMinimumLength.
    static QString SearchTerm(const QString &text);
    // MP10 Write(): every field as {name, value} maps for one ParameterService
    // batch; names containing "ENABLE" come last, and each group is ordered by
    // name case-insensitively so the three editor sections cannot reorder it.
    static QVariantList OrderedWriteChanges(const QList<ParamField> &fields);

    // --- state ---
    QString status() const { return m_status; }
    QString flightId() const { return m_flightId; }
    QString aircraftRegistration() const { return m_aircraftRegistration; }
    QString search() const { return m_search; }
    QString searchTerm() const { return SearchTerm(m_search); }
    bool isConnected() const { return m_connected; }
    bool isActive() const { return m_active; }
    bool isIdentificationBusy() const;
    bool isIdentificationAvailable() const; // connected and bound to a current lease
    bool hasPendingBulkWrite() const { return m_bulkWritePending; }
    qulonglong pendingBulkBatchId() const { return m_bulkBatchId; }

    AdsbIdentificationClient *client() const { return m_client.data(); }
    void setClient(AdsbIdentificationClient *client);

public slots:
    void setConnected(bool connected);
    void setSearch(const QString &search);
    void setFlightId(const QString &flightId);
    void setAircraftRegistration(const QString &registration);

    // MP10 RequestIdentification: GET registration then flight id.
    bool requestIdentification();
    // MP10 SaveFlightId / SaveAircraftRegistration, split so the view can run
    // the modal clear confirmation between the steps without this object
    // being on the stack: canSave() reports "offline" when the transport is
    // not available; declineClear() reports "... was not changed."; save()
    // performs the write (the caller has already confirmed an empty value).
    bool canSave();
    void declineClear(Field field);
    bool save(Field field, const QString &text);
    void cancelIdentification();

    // Bulk "Write Params" lifecycle (see ConfigADSBView slots of the same names).
    void beginBulkWrite(int parameterCount);
    void bulkWriteSubmitted(qulonglong batchId);
    void bulkWriteCompleted(qulonglong batchId, int succeeded, int failed);
    void bulkWriteCancelled(qulonglong batchId);
    void bulkWriteSubmissionFailed(const QString &reason);

    // Page lifecycle: activation reads the identification once the page is
    // usable; deactivation and target changes cancel the running write.
    void activate();
    void deactivate();
    void parameterTargetChanged();

signals:
    void statusChanged(const QString &status);
    void flightIdChanged(const QString &flightId);
    void aircraftRegistrationChanged(const QString &registration);
    void searchTermChanged(const QString &term);
    void connectedChanged(bool connected);
    void identificationBusyChanged(bool busy);

private:
    void setStatus(const QString &status);
    bool replyIsCurrent(qulonglong generation) const;
    static QString fieldLabel(Field field);

    void onFlightIdReceived(qulonglong generation, const QString &flightId);
    void onRegistrationReceived(qulonglong generation, const QString &registration);
    void onSaveStarted(Field field, qulonglong generation);
    void onSaveCompleted(Field field, qulonglong generation);
    void onSaveFailed(Field field, const QString &reason, bool cancelled);
    void onLeaseInvalidated(qulonglong generation);

    QPointer<AdsbIdentificationClient> m_client;
    QString m_status;
    QString m_flightId;
    QString m_aircraftRegistration;
    QString m_search;
    bool m_connected = false;
    bool m_active = false;
    bool m_bulkWritePending = false; // between beginBulkWrite and the outcome
    qulonglong m_bulkBatchId = 0;    // 0 until the owner reports the batch id
};

#endif
