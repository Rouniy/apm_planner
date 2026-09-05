#include "ConfigFFTView.h"

#include "ui/qcustomplot.h"

#include <QAction>
#include <QCheckBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPen>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QColor seriesColor(int index)
{
    static const QColor colors[] = {
        QColor(QStringLiteral("red")),
        QColor(QStringLiteral("green")),
        QColor(QStringLiteral("blue")),
        QColor(QStringLiteral("black")),
        QColor(QStringLiteral("violet")),
        QColor(QStringLiteral("orange")),
    };
    return colors[index % (sizeof(colors) / sizeof(colors[0]))];
}

int decimalsFor(const ParamField &field)
{
    if (field.increment >= 1.0) {
        return 0;
    }
    int decimals = 0;
    double increment = field.increment;
    while (decimals < 6
           && std::abs(increment - std::round(increment)) > 1.0e-9) {
        increment *= 10.0;
        ++decimals;
    }
    return decimals;
}
} // namespace

ConfigFFTView::ConfigFFTView(
    ConfigFFTViewModel *viewModel, QWidget *parent)
    : QWidget(parent)
    , m_viewModel(viewModel ? viewModel : new ConfigFFTViewModel)
{
    setObjectName(QStringLiteral("ConfigFFTView"));
    if (m_viewModel && !m_viewModel->parent()) {
        m_viewModel->setParent(this);
    }
    buildUi();
    if (m_viewModel) {
        connect(m_viewModel, &ConfigFFTViewModel::fieldsChanged,
                this, &ConfigFFTView::rebuildFields);
        connect(m_viewModel, &ConfigFFTViewModel::stateChanged,
                this, &ConfigFFTView::syncState);
        connect(m_viewModel, &ConfigFFTViewModel::analysisResultChanged,
                this, &ConfigFFTView::syncPlot);
        connect(m_viewModel, &ConfigFFTViewModel::writeRequested,
                this, &ConfigFFTView::writeRequested);
        connect(m_viewModel, &ConfigFFTViewModel::refreshRequested,
                this, &ConfigFFTView::refreshRequested);
    }
    rebuildFields();
    syncPlot();
    syncState();
}

ConfigFFTView::~ConfigFFTView()
{
    if (m_viewModel) {
        disconnect(m_viewModel.data(), nullptr, this, nullptr);
        m_viewModel->shutdown();
    }
}

void ConfigFFTView::setCatalog(
    const ParameterMetaDataCatalog &catalog, bool enforceMetadataRanges)
{
    if (m_viewModel) {
        m_viewModel->setCatalog(catalog, enforceMetadataRanges);
    }
}

void ConfigFFTView::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent, bool completeSnapshot)
{
    if (m_viewModel) {
        m_viewModel->setParameterSnapshot(
            parameters, preferredComponent, completeSnapshot);
    }
}

void ConfigFFTView::setParameterContext(
    bool connected, bool heartbeatFresh, bool armed)
{
    if (m_viewModel) {
        m_viewModel->setParameterContext(connected, heartbeatFresh, armed);
    }
}

void ConfigFFTView::setConnected(bool connected)
{
    if (m_viewModel) {
        m_viewModel->setConnected(connected);
    }
}

void ConfigFFTView::setHeartbeatFresh(bool fresh)
{
    if (m_viewModel) {
        m_viewModel->setHeartbeatFresh(fresh);
    }
}

void ConfigFFTView::setArmed(bool armed)
{
    if (m_viewModel) {
        m_viewModel->setArmed(armed);
    }
}

void ConfigFFTView::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    if (m_viewModel) {
        m_viewModel->parameterChanged(componentId, name, value);
    }
}

void ConfigFFTView::parameterWriteSubmitted(
    quint64 requestId, qulonglong batchId)
{
    if (m_viewModel) {
        m_viewModel->parameterWriteSubmitted(requestId, batchId);
    }
}

void ConfigFFTView::parameterWriteSubmissionFailed(
    quint64 requestId, const QString &reason)
{
    if (m_viewModel) {
        m_viewModel->parameterWriteSubmissionFailed(requestId, reason);
    }
}

void ConfigFFTView::parameterWriteFailed(
    qulonglong batchId, int componentId, const QString &name,
    const QString &reason)
{
    if (m_viewModel) {
        m_viewModel->parameterWriteFailed(
            batchId, componentId, name, reason);
    }
}

void ConfigFFTView::parameterWriteCancelled(
    qulonglong batchId, int componentId, const QString &name)
{
    if (m_viewModel) {
        m_viewModel->parameterWriteCancelled(batchId, componentId, name);
    }
}

void ConfigFFTView::parameterBatchCompleted(
    qulonglong batchId, int succeeded, int failed)
{
    if (m_viewModel) {
        m_viewModel->parameterBatchCompleted(batchId, succeeded, failed);
    }
}

void ConfigFFTView::refreshFailed(const QString &reason)
{
    if (m_viewModel) {
        m_viewModel->refreshFailed(reason);
    }
}

void ConfigFFTView::refreshCanceled()
{
    if (m_viewModel) {
        m_viewModel->refreshCanceled();
    }
}

void ConfigFFTView::buildUi()
{
    setStyleSheet(QStringLiteral(
        "ConfigFFTView { background: #1A201D; color: #E6EDE9; }"
        "QLabel#fftTitle { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QLabel#fftInfo { color: #C8C8C8; }"
        "QLabel#fftParameterStatus, QLabel#fftStatus { color: #34D399; }"
        "QPushButton, QSpinBox, QDoubleSpinBox, QToolButton {"
        " background: #161B18; color: #E6EDE9; border: 1px solid #303A35;"
        " padding: 4px; }"
        "QPushButton:disabled, QSpinBox:disabled,"
        " QDoubleSpinBox:disabled, QToolButton:disabled { color: #68736D; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(8);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("fftTitle"));
    root->addWidget(m_title);
    m_info = new QLabel(this);
    m_info->setObjectName(QStringLiteral("fftInfo"));
    m_info->setWordWrap(true);
    root->addWidget(m_info);

    m_fieldsHost = new QWidget(this);
    m_fieldsHost->setObjectName(QStringLiteral("fftParameterFields"));
    auto *fieldLayout = new QGridLayout(m_fieldsHost);
    fieldLayout->setContentsMargins(0, 0, 0, 0);
    fieldLayout->setHorizontalSpacing(8);
    fieldLayout->setVerticalSpacing(6);
    fieldLayout->setColumnMinimumWidth(0, 220);
    fieldLayout->setColumnStretch(1, 1);
    root->addWidget(m_fieldsHost);

    m_parameterStatus = new QLabel(this);
    m_parameterStatus->setObjectName(QStringLiteral("fftParameterStatus"));
    m_parameterStatus->setWordWrap(true);
    root->addWidget(m_parameterStatus);

    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);
    m_refresh = new QPushButton(tr("Refresh Params"), this);
    m_refresh->setObjectName(QStringLiteral("fftRefreshParams"));
    toolbar->addWidget(m_refresh);
    m_fft = new QPushButton(tr("FFT"), this);
    m_fft->setObjectName(QStringLiteral("fftAnalyze"));
    toolbar->addWidget(m_fft);
    m_cancel = new QPushButton(tr("Cancel FFT"), this);
    m_cancel->setObjectName(QStringLiteral("fftCancel"));
    toolbar->addWidget(m_cancel);
    toolbar->addWidget(new QLabel(tr("Bins"), this));
    m_bins = new QSpinBox(this);
    m_bins->setObjectName(QStringLiteral("fftBins"));
    m_bins->setRange(4, 14);
    toolbar->addWidget(m_bins);
    toolbar->addWidget(new QLabel(tr("Start Freq"), this));
    m_startFrequency = new QDoubleSpinBox(this);
    m_startFrequency->setObjectName(QStringLiteral("fftStartFrequency"));
    m_startFrequency->setRange(0.0, 1000.0);
    m_startFrequency->setDecimals(1);
    m_startFrequency->setSuffix(tr(" Hz"));
    toolbar->addWidget(m_startFrequency);
    m_magnitude = new QCheckBox(tr("Magnitude"), this);
    m_magnitude->setObjectName(QStringLiteral("fftMagnitude"));
    toolbar->addWidget(m_magnitude);
    toolbar->addStretch(1);
    root->addLayout(toolbar);

    m_fftStatus = new QLabel(this);
    m_fftStatus->setObjectName(QStringLiteral("fftStatus"));
    m_fftStatus->setWordWrap(true);
    root->addWidget(m_fftStatus);
    m_plotTitle = new QLabel(tr("FFT spectrum"), this);
    m_plotTitle->setObjectName(QStringLiteral("fftPlotTitle"));
    m_plotTitle->setAlignment(Qt::AlignCenter);
    root->addWidget(m_plotTitle);
    m_plot = new QCustomPlot(this);
    m_plot->setObjectName(QStringLiteral("fftPlot"));
    m_plot->xAxis->setLabel(tr("Frequency (Hz)"));
    m_plot->yAxis->setLabel(tr("Amplitude (dB)"));
    m_plot->legend->setVisible(true);
    m_plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    root->addWidget(m_plot, 1);

    connect(m_refresh, &QPushButton::clicked, this, [this]() {
        if (m_viewModel) {
            m_viewModel->refreshParameters();
        }
    });
    connect(m_fft, &QPushButton::clicked,
            this, &ConfigFFTView::chooseLogFile);
    connect(m_cancel, &QPushButton::clicked, this, [this]() {
        if (m_viewModel) {
            m_viewModel->cancelAnalysis();
        }
    });
    connect(m_bins, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int value) {
        if (m_viewModel) {
            m_viewModel->setBins(value);
        }
    });
    connect(m_startFrequency,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double value) {
        if (m_viewModel) {
            m_viewModel->setStartFrequencyHz(value);
        }
    });
    connect(m_magnitude, &QCheckBox::toggled,
            this, [this](bool checked) {
        if (m_viewModel) {
            m_viewModel->setMagnitude(checked);
        }
    });
}

void ConfigFFTView::rebuildFields()
{
    auto *layout = qobject_cast<QGridLayout *>(m_fieldsHost->layout());
    while (QLayoutItem *item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_fieldEditors.clear();
    if (!m_viewModel) {
        return;
    }
    const QList<ParamField> fields = m_viewModel->fields();
    m_fieldEditors.resize(fields.size());
    for (int index = 0; index < fields.size(); ++index) {
        const ParamField &field = fields.at(index);
        FieldEditors editors;
        editors.label = new QLabel(field.label, m_fieldsHost);
        editors.label->setObjectName(
            QStringLiteral("fftParameterLabel_%1").arg(field.name));
        editors.label->setToolTip(field.description);
        editors.label->setWordWrap(true);
        layout->addWidget(editors.label, index, 0);

        if (field.editorKind == ParamField::EditorKind::Bitmask) {
            editors.bitmask = new QToolButton(m_fieldsHost);
            editors.bitmask->setObjectName(
                QStringLiteral("fftParameterBitmask_%1").arg(field.name));
            editors.bitmask->setPopupMode(QToolButton::InstantPopup);
            auto *menu = new QMenu(editors.bitmask);
            for (const BitOption &option : field.bitOptions) {
                QAction *const action = menu->addAction(
                    QStringLiteral("%1: %2").arg(option.bit).arg(option.label));
                action->setCheckable(true);
                action->setData(option.bit);
                connect(action, &QAction::toggled, this,
                        [this, index](bool) { stageBitmask(index); });
                editors.bitActions.append(action);
            }
            editors.bitmask->setMenu(menu);
            layout->addWidget(editors.bitmask, index, 1);
        } else {
            editors.numeric = new QDoubleSpinBox(m_fieldsHost);
            editors.numeric->setObjectName(
                QStringLiteral("fftParameterNumeric_%1").arg(field.name));
            editors.numeric->setRange(field.minimum, field.maximum);
            editors.numeric->setSingleStep(field.increment);
            editors.numeric->setDecimals(decimalsFor(field));
            editors.numeric->setValue(field.value.toDouble());
            connect(editors.numeric, &QDoubleSpinBox::editingFinished,
                    this, [this, index]() {
                if (!m_viewModel || index >= m_fieldEditors.size()
                    || !m_fieldEditors.at(index).numeric) {
                    return;
                }
                const QList<ParamField> current = m_viewModel->fields();
                if (index < current.size()) {
                    m_viewModel->stageParameterValue(
                        current.at(index).name,
                        m_fieldEditors.at(index).numeric->value());
                }
            });
            layout->addWidget(editors.numeric, index, 1);
        }
        editors.units = new QLabel(field.units, m_fieldsHost);
        layout->addWidget(editors.units, index, 2);
        editors.status = new QLabel(field.status, m_fieldsHost);
        editors.status->setObjectName(
            QStringLiteral("fftParameterState_%1").arg(field.name));
        layout->addWidget(editors.status, index, 3);
        m_fieldEditors[index] = editors;
    }
    syncState();
}

void ConfigFFTView::syncState()
{
    if (!m_viewModel) {
        m_refresh->setEnabled(false);
        m_fft->setEnabled(false);
        m_cancel->setEnabled(false);
        return;
    }
    m_title->setText(m_viewModel->title());
    m_info->setText(m_viewModel->info());
    m_parameterStatus->setText(m_viewModel->parameterStatus());
    m_fftStatus->setText(m_viewModel->analysisStatus());
    m_refresh->setEnabled(m_viewModel->canRefreshParameters());
    m_fft->setEnabled(m_viewModel->canAnalyze());
    m_cancel->setEnabled(m_viewModel->analysisBusy());
    m_bins->setEnabled(!m_viewModel->analysisBusy());
    m_startFrequency->setEnabled(!m_viewModel->analysisBusy());
    m_magnitude->setEnabled(!m_viewModel->analysisBusy());
    {
        const QSignalBlocker blocker(m_bins);
        m_bins->setValue(m_viewModel->bins());
    }
    {
        const QSignalBlocker blocker(m_startFrequency);
        m_startFrequency->setValue(m_viewModel->startFrequencyHz());
    }
    {
        const QSignalBlocker blocker(m_magnitude);
        m_magnitude->setChecked(m_viewModel->magnitude());
    }

    const QList<ParamField> fields = m_viewModel->fields();
    for (int index = 0;
         index < fields.size() && index < m_fieldEditors.size(); ++index) {
        const ParamField &field = fields.at(index);
        FieldEditors &editors = m_fieldEditors[index];
        const bool enabled = m_viewModel->canEditParameter(field.name);
        if (editors.numeric) {
            const QSignalBlocker blocker(editors.numeric);
            editors.numeric->setValue(field.value.toDouble());
            editors.numeric->setEnabled(enabled);
        }
        if (editors.bitmask) {
            const qulonglong mask = field.value.toULongLong();
            editors.bitmask->setEnabled(enabled);
            for (QAction *action : editors.bitActions) {
                const QSignalBlocker blocker(action);
                const int bit = action->data().toInt();
                action->setChecked(bit >= 0 && bit < 64
                    && (mask & (qulonglong(1) << bit)) != 0);
            }
            editors.bitmask->setText(bitmaskSummary(field));
        }
        if (editors.status) {
            editors.status->setText(field.status);
        }
    }
}

void ConfigFFTView::syncPlot()
{
    m_plot->clearGraphs();
    if (!m_viewModel) {
        m_plot->replot();
        return;
    }
    const DataFlashFftAnalyzer::Result result =
        m_viewModel->analysisResult();
    m_plot->yAxis->setLabel(m_viewModel->magnitude()
        ? tr("Magnitude") : tr("Amplitude (dB)"));
    m_plotTitle->setText(result.series.isEmpty()
        ? tr("FFT spectrum")
        : tr("FFT: %1 - %2 Hz input")
              .arg(result.source)
              .arg(result.sampleRateHz, 0, 'f', 1));
    for (int index = 0; index < result.series.size(); ++index) {
        const DataFlashFftAnalyzer::Series &series = result.series.at(index);
        const int count = qMin(series.frequenciesHz.size(),
                               series.values.size());
        QVector<double> frequencies = series.frequenciesHz.mid(0, count);
        QVector<double> values = series.values.mid(0, count);
        QCPGraph *const graph = m_plot->addGraph();
        graph->setName(series.label);
        graph->setPen(QPen(seriesColor(index), 2.0));
        graph->setData(frequencies, values, true);
    }
    if (!result.series.isEmpty()) {
        m_plot->rescaleAxes();
        if (!m_viewModel->magnitude()) {
            // FFT2's exact-zero floor is about -6466 dB. Keep the computed
            // graph data intact, but start with a useful 160 dB dynamic range;
            // ordinary plot zoom/pan can still inspect values below it.
            double peak = -std::numeric_limits<double>::infinity();
            for (const auto &series : result.series) {
                for (double value : series.values) {
                    if (std::isfinite(value)) peak = std::max(peak, value);
                }
            }
            if (!std::isfinite(peak) || peak < -6000.0) peak = 0.0;
            m_plot->yAxis->setRange(peak - 160.0, peak + 10.0);
        }
    } else {
        m_plot->xAxis->setRange(0.0, 100.0);
        m_plot->yAxis->setRange(-100.0, 10.0);
    }
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void ConfigFFTView::chooseLogFile()
{
    QPointer<ConfigFFTView> guardedThis(this);
    QPointer<ConfigFFTViewModel> guardedModel(m_viewModel);
    if (!guardedModel || !guardedModel->canAnalyze()) {
        return;
    }
    QString start = QStandardPaths::writableLocation(
        QStandardPaths::DocumentsLocation);
    auto *picker = new QFileDialog(
        this, tr("Open dataflash log"), start,
        tr("Dataflash log (*.bin *.BIN);;All files (*)"));
    picker->setObjectName(QStringLiteral("fftLogPicker"));
    picker->setAcceptMode(QFileDialog::AcceptOpen);
    picker->setFileMode(QFileDialog::ExistingFile);
    QPointer<QFileDialog> guardedPicker(picker);
    const int result = picker->exec();
    if (!guardedThis || !guardedModel || !guardedPicker) {
        if (guardedPicker) {
            delete guardedPicker.data();
        }
        return;
    }
    const QString path = result == QDialog::Accepted
        && !guardedPicker->selectedFiles().isEmpty()
        ? guardedPicker->selectedFiles().constFirst() : QString();
    delete guardedPicker.data();
    if (!guardedThis || !guardedModel || path.isEmpty()) {
        return;
    }
    guardedModel->analyzeFile(path);
}

void ConfigFFTView::stageBitmask(int fieldIndex)
{
    if (!m_viewModel || fieldIndex < 0
        || fieldIndex >= m_fieldEditors.size()) {
        return;
    }
    qulonglong value = 0;
    for (QAction *action : m_fieldEditors.at(fieldIndex).bitActions) {
        const int bit = action->data().toInt();
        if (action->isChecked() && bit >= 0 && bit < 64) {
            value |= qulonglong(1) << bit;
        }
    }
    const QList<ParamField> fields = m_viewModel->fields();
    if (fieldIndex < fields.size()) {
        m_viewModel->stageParameterValue(fields.at(fieldIndex).name, value);
    }
}

QString ConfigFFTView::bitmaskSummary(const ParamField &field)
{
    const qulonglong mask = field.value.toULongLong();
    QStringList enabled;
    for (const BitOption &option : field.bitOptions) {
        if (option.bit >= 0 && option.bit < 64
            && (mask & (qulonglong(1) << option.bit)) != 0) {
            enabled.append(QStringLiteral("%1: %2")
                               .arg(option.bit).arg(option.label));
        }
    }
    if (enabled.isEmpty()) {
        return tr("(none)");
    }
    return enabled.size() <= 3
        ? enabled.join(QStringLiteral(", "))
        : tr("%1 bits set").arg(enabled.size());
}
