#ifndef CONFIGADSBVIEW_H
#define CONFIGADSBVIEW_H

#include "ConfigADSBViewModel.h"
#include "core/parameters/ParameterMetaData.h"
#include "core/parameters/ParameterStore.h"

#include <QList>
#include <QVariant>
#include <QVariantList>
#include <QWidget>

class ConfigFriendlyParamsView;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;

/*
 * Mission Planner 10 SETUP > ADSB page
 * (GCSViews/ConfigurationView/ConfigADSBView.axaml).
 *
 * Title, toolbar (Write Params, Refresh Params, Find), identification row
 * (Flight Identification / Aircraft Registration with Save), intro text and
 * the editors for the ADSB and AVD parameter families. The editors are three
 * embedded ConfigFriendlyParamsView instances without custom mode so the
 * metadata-backed numeric, combo and bitmask editors are kept: Standard-level
 * parameters, Advanced-level parameters, and a custom-name section for
 * parameters that have no catalog title (MP10 shows those with their raw
 * name).
 */
class ConfigADSBView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigADSBView(AdsbIdentificationClient *client,
                            const ParameterMetaDataCatalog &catalog = ParameterMetaDataCatalog(),
                            QWidget *parent = nullptr, bool enforceMetadataRanges = true);
    ~ConfigADSBView() override;

    ConfigADSBViewModel *viewModel() const { return m_viewModel; }
    ConfigFriendlyParamsView *standardEditor() const { return m_standardEditor; }
    ConfigFriendlyParamsView *advancedEditor() const { return m_advancedEditor; }
    ConfigFriendlyParamsView *otherEditor() const { return m_otherEditor; }

    void setCatalog(const ParameterMetaDataCatalog &catalog, bool enforceMetadataRanges = true);
    // Committed ParameterStore snapshot; only the ADSB and AVD families are shown.
    void setParameterSnapshot(const QList<ParameterRecord> &records,
                              int preferredComponent = 1);
    void setConnected(bool connected);
    bool isConnected() const;
    int selectedComponent() const;
    int visibleParameterCount() const;
    QStringList shownParameterNames() const;

public slots:
    void activate();
    void deactivate();
    void parameterTargetChanged();
    void parameterChanged(int componentId, const QString &name, const QVariant &value);
    void parameterWriteFailed(int componentId, const QString &name, const QString &reason);
    void parameterWriteFailed(int componentId, const QString &name,
                              const QVariant &attemptedValue, const QString &reason);
    // Bulk "Write Params" outcome, reported by the owner of the parameter
    // batch (QGCUASParamManager / ParameterService): submission id, completion
    // counts, cancellation or a submission failure. Foreign batch ids are
    // ignored. MP10 statuses: "Parameters successfully saved." / "write failed".
    void parameterBatchSubmitted(int componentId, qulonglong batchId);
    void parameterBatchCompleted(qulonglong batchId, int succeeded, int failed);
    void parameterBatchCancelled(qulonglong batchId);
    void parameterWriteSubmissionFailed(const QString &reason);
    // Toolbar actions (also wired to the buttons).
    void refreshParams();
    void writeParams();
    void saveFlightId();
    void saveAircraftRegistration();

signals:
    // "Refresh Params": re-read the parameter list of this component.
    void refreshRequested(int componentId);
    // A single edited field (forwarded from the embedded editors).
    void writeRequested(int componentId, const QString &name, const QVariant &value);
    // "Write Params": every ADS-B field as one batch, ENABLE parameters last.
    void writeParamsRequested(int componentId, QVariantList changes);

private:
    void buildUi(const ParameterMetaDataCatalog &catalog, bool enforceMetadataRanges);
    void applySearchTerm(const QString &term);
    void syncIdentificationControls();
    // Runs the MP10 save flow for one field: availability, the modal clear
    // confirmation (this object may be deleted while it is open), then the
    // client write. Every step after the modal re-checks a QPointer guard.
    void saveIdentification(ConfigADSBViewModel::Field field, const QString &text);
    QStringList uncataloguedNames(const QList<ConfigFriendlyParameterValue> &values) const;

    ConfigADSBViewModel *m_viewModel = nullptr;
    ParameterMetaDataCatalog m_catalog;
    bool m_enforceMetadataRanges = true;
    int m_preferredComponent = 1;

    QLabel *m_title = nullptr;
    QPushButton *m_writeButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QLineEdit *m_search = nullptr;
    QLabel *m_status = nullptr;
    QLineEdit *m_flightIdEdit = nullptr;
    QPushButton *m_saveFlightIdButton = nullptr;
    QLineEdit *m_aircraftRegistrationEdit = nullptr;
    QPushButton *m_saveAircraftRegistrationButton = nullptr;
    QLabel *m_intro = nullptr;
    QScrollArea *m_scroll = nullptr;
    ConfigFriendlyParamsView *m_standardEditor = nullptr;
    ConfigFriendlyParamsView *m_advancedEditor = nullptr;
    ConfigFriendlyParamsView *m_otherEditor = nullptr;
};

#endif
