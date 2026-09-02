#include "ConfigADSBView.h"

#include "ConfigFriendlyParamsView.h"
#include "ConfigFriendlyParamsViewModel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

ConfigADSBView::ConfigADSBView(AdsbIdentificationClient *client,
                               const ParameterMetaDataCatalog &catalog, QWidget *parent,
                               bool enforceMetadataRanges)
    : QWidget(parent),
      m_viewModel(new ConfigADSBViewModel(this)),
      m_catalog(catalog),
      m_enforceMetadataRanges(enforceMetadataRanges)
{
    setObjectName(QStringLiteral("ConfigADSBView"));
    buildUi(catalog, enforceMetadataRanges);
    m_viewModel->setClient(client);
    syncIdentificationControls();
}

ConfigADSBView::~ConfigADSBView()
{
    // Never leave a write cadence running for a page that no longer exists.
    m_viewModel->cancelIdentification();
}

void ConfigADSBView::buildUi(const ParameterMetaDataCatalog &catalog,
                             bool enforceMetadataRanges)
{
    setStyleSheet(QStringLiteral(
        "ConfigADSBView { background: #1A201D; color: #E6EDE9; }"
        "QLabel#adsbTitle { color: #E8E8E8; font-size: 16px; font-weight: bold; }"
        "QLabel#adsbStatus { color: #34D399; }"
        "QLabel#adsbIntro, QLabel#findLabel, QLabel#flightIdLabel,"
        " QLabel#aircraftRegistrationLabel { color: #BBC5BF; }"
        "ConfigADSBView QLineEdit { background: #161B18; color: #E6EDE9;"
        " border: 1px solid #2A322D; padding: 4px; }"
        "ConfigADSBView QPushButton { padding: 5px 12px; }"));

    // MP10: Grid rows Auto,Auto,Auto,* with margin 16.
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(0);

    m_title = new QLabel(ConfigADSBViewModel::Title, this);
    m_title->setObjectName(QStringLiteral("adsbTitle"));
    root->addWidget(m_title);

    // Row 1: StackPanel spacing 8, top margin 6.
    auto *rows = new QVBoxLayout;
    rows->setContentsMargins(0, 6, 0, 0);
    rows->setSpacing(8);

    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);
    m_writeButton = new QPushButton(tr("Write Params"), this);
    m_writeButton->setObjectName(QStringLiteral("writeParamsButton"));
    toolbar->addWidget(m_writeButton);
    m_refreshButton = new QPushButton(tr("Refresh Params"), this);
    m_refreshButton->setObjectName(QStringLiteral("refreshParamsButton"));
    toolbar->addWidget(m_refreshButton);
    auto *findLabel = new QLabel(tr("Find"), this);
    findLabel->setObjectName(QStringLiteral("findLabel"));
    findLabel->setContentsMargins(8, 0, 0, 0);
    toolbar->addWidget(findLabel, 0, Qt::AlignVCenter);
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("adsbSearch"));
    m_search->setFixedWidth(220);
    m_search->setPlaceholderText(tr("search (min 2 chars)"));
    toolbar->addWidget(m_search);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("adsbStatus"));
    m_status->setTextFormat(Qt::PlainText);
    toolbar->addWidget(m_status, 1, Qt::AlignVCenter);
    rows->addLayout(toolbar);

    auto *identification = new QHBoxLayout;
    identification->setSpacing(8);
    auto *flightIdLabel = new QLabel(tr("Flight Identification"), this);
    flightIdLabel->setObjectName(QStringLiteral("flightIdLabel"));
    identification->addWidget(flightIdLabel, 0, Qt::AlignVCenter);
    m_flightIdEdit = new QLineEdit(this);
    m_flightIdEdit->setObjectName(QStringLiteral("flightIdEdit"));
    m_flightIdEdit->setFixedWidth(160);
    m_flightIdEdit->setMaxLength(AdsbIdentificationClient::DeviceTextLength);
    identification->addWidget(m_flightIdEdit);
    m_saveFlightIdButton = new QPushButton(tr("Save"), this);
    m_saveFlightIdButton->setObjectName(QStringLiteral("saveFlightIdButton"));
    identification->addWidget(m_saveFlightIdButton);
    auto *registrationLabel = new QLabel(tr("Aircraft Registration"), this);
    registrationLabel->setObjectName(QStringLiteral("aircraftRegistrationLabel"));
    registrationLabel->setContentsMargins(12, 0, 0, 0);
    identification->addWidget(registrationLabel, 0, Qt::AlignVCenter);
    m_aircraftRegistrationEdit = new QLineEdit(this);
    m_aircraftRegistrationEdit->setObjectName(QStringLiteral("aircraftRegistrationEdit"));
    m_aircraftRegistrationEdit->setFixedWidth(160);
    m_aircraftRegistrationEdit->setMaxLength(AdsbIdentificationClient::DeviceTextLength);
    identification->addWidget(m_aircraftRegistrationEdit);
    m_saveAircraftRegistrationButton = new QPushButton(tr("Save"), this);
    m_saveAircraftRegistrationButton->setObjectName(
        QStringLiteral("saveAircraftRegistrationButton"));
    identification->addWidget(m_saveAircraftRegistrationButton);
    identification->addStretch(1);
    rows->addLayout(identification);
    root->addLayout(rows);

    // Row 2: intro, top margin 10.
    m_intro = new QLabel(ConfigADSBViewModel::Intro, this);
    m_intro->setObjectName(QStringLiteral("adsbIntro"));
    m_intro->setWordWrap(true);
    m_intro->setContentsMargins(0, 10, 0, 0);
    root->addWidget(m_intro);

    // Row 3: scrollable field list, top margin 10.
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("adsbFieldsScroll"));
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    auto *content = new QWidget(m_scroll);
    content->setObjectName(QStringLiteral("adsbFieldsContent"));
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 10, 0, 0);
    contentLayout->setSpacing(0);
    m_standardEditor = new ConfigFriendlyParamsView(false, catalog, content, enforceMetadataRanges);
    m_standardEditor->setObjectName(QStringLiteral("adsbStandardParams"));
    m_advancedEditor = new ConfigFriendlyParamsView(true, catalog, content, enforceMetadataRanges);
    m_advancedEditor->setObjectName(QStringLiteral("adsbAdvancedParams"));
    m_otherEditor = new ConfigFriendlyParamsView(true, catalog, content, enforceMetadataRanges);
    m_otherEditor->setObjectName(QStringLiteral("adsbOtherParams"));
    for (ConfigFriendlyParamsView *editor : {m_standardEditor, m_advancedEditor, m_otherEditor}) {
        editor->setEmbeddedMode(true);
        // MP10 shows an empty list; the intro already says "Populated on connect."
        editor->setUnavailableMessage(QStringLiteral(" "));
        editor->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        contentLayout->addWidget(editor);
        connect(editor, &ConfigFriendlyParamsView::writeRequested, this,
                &ConfigADSBView::writeRequested);
        connect(editor, &ConfigFriendlyParamsView::refreshRequested, this,
                &ConfigADSBView::refreshRequested);
    }
    contentLayout->addStretch(1);
    m_scroll->setWidget(content);
    root->addWidget(m_scroll, 1);

    connect(m_writeButton, &QPushButton::clicked, this, &ConfigADSBView::writeParams);
    connect(m_refreshButton, &QPushButton::clicked, this, &ConfigADSBView::refreshParams);
    connect(m_saveFlightIdButton, &QPushButton::clicked, this, &ConfigADSBView::saveFlightId);
    connect(m_saveAircraftRegistrationButton, &QPushButton::clicked, this,
            &ConfigADSBView::saveAircraftRegistration);
    connect(m_search, &QLineEdit::textChanged, m_viewModel, &ConfigADSBViewModel::setSearch);
    connect(m_viewModel, &ConfigADSBViewModel::searchTermChanged, this,
            &ConfigADSBView::applySearchTerm);
    connect(m_viewModel, &ConfigADSBViewModel::statusChanged, m_status, &QLabel::setText);
    connect(m_viewModel, &ConfigADSBViewModel::flightIdChanged, this,
            [this](const QString &flightId) {
                const QSignalBlocker blocker(m_flightIdEdit);
                m_flightIdEdit->setText(flightId);
            });
    connect(m_viewModel, &ConfigADSBViewModel::aircraftRegistrationChanged, this,
            [this](const QString &registration) {
                const QSignalBlocker blocker(m_aircraftRegistrationEdit);
                m_aircraftRegistrationEdit->setText(registration);
            });
    connect(m_viewModel, &ConfigADSBViewModel::identificationBusyChanged, this,
            [this](bool) { syncIdentificationControls(); });
    connect(m_viewModel, &ConfigADSBViewModel::connectedChanged, this,
            [this](bool) { syncIdentificationControls(); });
}

void ConfigADSBView::setCatalog(const ParameterMetaDataCatalog &catalog,
                                bool enforceMetadataRanges)
{
    m_catalog = catalog;
    m_enforceMetadataRanges = enforceMetadataRanges;
    m_standardEditor->setCatalog(catalog, enforceMetadataRanges);
    m_advancedEditor->setCatalog(catalog, enforceMetadataRanges);
    m_otherEditor->setCatalog(catalog, enforceMetadataRanges);
}

QStringList ConfigADSBView::uncataloguedNames(
    const QList<ConfigFriendlyParameterValue> &values) const
{
    QStringList names;
    for (const ConfigFriendlyParameterValue &value : values) {
        const bool described = m_catalog.isValid() && m_catalog.contains(value.name) &&
                               !m_catalog.value(value.name).title.isEmpty();
        if (!described && !names.contains(value.name, Qt::CaseInsensitive)) {
            names.append(value.name);
        }
    }
    return names;
}

void ConfigADSBView::setParameterSnapshot(const QList<ParameterRecord> &records,
                                          int preferredComponent)
{
    m_preferredComponent = preferredComponent;
    const QList<ConfigFriendlyParameterValue> values =
        ConfigADSBViewModel::FilterSnapshot(records);
    m_standardEditor->setParameterSnapshot(values, preferredComponent);
    m_advancedEditor->setParameterSnapshot(values, preferredComponent);
    m_otherEditor->setCustomParameterNames(uncataloguedNames(values));
    m_otherEditor->setParameterSnapshot(values, preferredComponent);
    applySearchTerm(m_viewModel->searchTerm());
}

void ConfigADSBView::setConnected(bool connected)
{
    m_viewModel->setConnected(connected);
}

bool ConfigADSBView::isConnected() const
{
    return m_viewModel->isConnected();
}

int ConfigADSBView::selectedComponent() const
{
    const int component = m_standardEditor->selectedComponent();
    return component > 0 ? component : m_preferredComponent;
}

int ConfigADSBView::visibleParameterCount() const
{
    return m_standardEditor->visibleParameterCount() +
           m_advancedEditor->visibleParameterCount() +
           m_otherEditor->visibleParameterCount();
}

QStringList ConfigADSBView::shownParameterNames() const
{
    QStringList names;
    for (ConfigFriendlyParamsView *editor : {m_standardEditor, m_advancedEditor, m_otherEditor}) {
        const QList<ParamField> fields = editor->viewModel()->fields();
        for (const ParamField &field : fields) {
            names.append(field.name);
        }
    }
    return names;
}

void ConfigADSBView::activate()
{
    m_viewModel->activate();
}

void ConfigADSBView::deactivate()
{
    m_viewModel->deactivate();
}

void ConfigADSBView::parameterTargetChanged()
{
    m_viewModel->parameterTargetChanged();
    setParameterSnapshot({}, m_preferredComponent);
}

void ConfigADSBView::parameterChanged(int componentId, const QString &name,
                                      const QVariant &value)
{
    m_standardEditor->parameterChanged(componentId, name, value);
    m_advancedEditor->parameterChanged(componentId, name, value);
    m_otherEditor->parameterChanged(componentId, name, value);
}

void ConfigADSBView::parameterWriteFailed(int componentId, const QString &name,
                                          const QString &reason)
{
    m_standardEditor->parameterWriteFailed(componentId, name, reason);
    m_advancedEditor->parameterWriteFailed(componentId, name, reason);
    m_otherEditor->parameterWriteFailed(componentId, name, reason);
}

void ConfigADSBView::parameterWriteFailed(int componentId, const QString &name,
                                          const QVariant &attemptedValue,
                                          const QString &reason)
{
    m_standardEditor->parameterWriteFailed(componentId, name, attemptedValue, reason);
    m_advancedEditor->parameterWriteFailed(componentId, name, attemptedValue, reason);
    m_otherEditor->parameterWriteFailed(componentId, name, attemptedValue, reason);
}

void ConfigADSBView::refreshParams()
{
    emit refreshRequested(selectedComponent());
}

void ConfigADSBView::writeParams()
{
    if (!m_viewModel->isConnected()) {
        m_viewModel->bulkWriteSubmissionFailed(QString()); // no-op unless pending
        m_status->setText(tr("offline")); // MP10 Write(): BaseStream not open
        return;
    }
    if (m_viewModel->hasPendingBulkWrite()) {
        return; // one batch at a time; the owner reports its outcome
    }
    QList<ParamField> fields;
    for (ConfigFriendlyParamsView *editor : {m_standardEditor, m_advancedEditor, m_otherEditor}) {
        fields += editor->viewModel()->fields();
    }
    const QVariantList changes = ConfigADSBViewModel::OrderedWriteChanges(fields);
    if (changes.isEmpty()) {
        m_status->setText(tr("Parameters successfully saved.")); // nothing to write, like MP10
        return;
    }
    m_viewModel->beginBulkWrite(changes.size());
    emit writeParamsRequested(selectedComponent(), changes);
}

void ConfigADSBView::parameterBatchSubmitted(int componentId, qulonglong batchId)
{
    Q_UNUSED(componentId)
    m_viewModel->bulkWriteSubmitted(batchId);
}

void ConfigADSBView::parameterBatchCompleted(qulonglong batchId, int succeeded, int failed)
{
    m_viewModel->bulkWriteCompleted(batchId, succeeded, failed);
}

void ConfigADSBView::parameterBatchCancelled(qulonglong batchId)
{
    m_viewModel->bulkWriteCancelled(batchId);
}

void ConfigADSBView::parameterWriteSubmissionFailed(const QString &reason)
{
    m_viewModel->bulkWriteSubmissionFailed(reason);
}

void ConfigADSBView::saveIdentification(ConfigADSBViewModel::Field field, const QString &text)
{
    using Field = ConfigADSBViewModel::Field;
    if (!m_viewModel->canSave()) {
        return; // "offline"
    }
    if (AdsbIdentificationClient::NeedsClearConfirmation(text)) {
        // The page (and with it the view model and its client binding) may be
        // deleted while the modal box is open: nothing below touches members
        // without re-checking the guard.
        const QPointer<ConfigADSBView> guard(this);
        const QString title = field == Field::FlightId
                                  ? tr("Clear Flight Identification")
                                  : tr("Clear Aircraft Registration");
        const QString question =
            field == Field::FlightId
                ? tr("The Flight Identification field is empty. Send an empty value and "
                     "clear the device setting?")
                : tr("The Aircraft Registration field is empty. Send an empty value and "
                     "clear the device setting?");
        // Heap-allocated and parented: deleting the page deletes the box and
        // QDialog::exec() returns Rejected instead of touching a dead object
        // (a stack QMessageBox::question box would be destroyed twice).
        auto *box = new QMessageBox(QMessageBox::Question, title, question,
                                    QMessageBox::Yes | QMessageBox::No, this);
        box->setObjectName(QStringLiteral("adsbClearConfirmation"));
        box->setDefaultButton(QMessageBox::No);
        box->setAttribute(Qt::WA_DeleteOnClose);
        const int answer = box->exec();
        if (!guard) {
            return;
        }
        if (answer != QMessageBox::Yes) {
            m_viewModel->declineClear(field);
            return;
        }
        if (!m_viewModel->canSave()) {
            return; // the vehicle went away while the box was open
        }
    }
    m_viewModel->save(field, text);
}

void ConfigADSBView::saveFlightId()
{
    saveIdentification(ConfigADSBViewModel::Field::FlightId, m_flightIdEdit->text());
}

void ConfigADSBView::saveAircraftRegistration()
{
    saveIdentification(ConfigADSBViewModel::Field::Registration,
                       m_aircraftRegistrationEdit->text());
}

void ConfigADSBView::applySearchTerm(const QString &term)
{
    m_standardEditor->viewModel()->setSearch(term);
    m_advancedEditor->viewModel()->setSearch(term);
    m_otherEditor->viewModel()->setSearch(term);
}

void ConfigADSBView::syncIdentificationControls()
{
    // MP10 keeps the controls enabled and answers "offline"; only a running
    // write blocks the two Save buttons here so the cadence is not doubled.
    const bool busy = m_viewModel->isIdentificationBusy();
    m_saveFlightIdButton->setEnabled(!busy);
    m_saveAircraftRegistrationButton->setEnabled(!busy);
}
