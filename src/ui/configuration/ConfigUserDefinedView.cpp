#include "ConfigUserDefinedView.h"

#include "ConfigFriendlyParamsView.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

ConfigUserDefinedView::ConfigUserDefinedView(
    const ParameterMetaDataCatalog &catalog, QWidget *parent,
    bool enforceMetadataRanges)
    : QWidget(parent),
      m_viewModel(new ConfigUserDefinedViewModel(this)),
      m_editor(new ConfigFriendlyParamsView(
          false, catalog, this, enforceMetadataRanges))
{
    setObjectName(QStringLiteral("ConfigUserDefinedView"));
    setStyleSheet(QStringLiteral(
        "ConfigUserDefinedView { background: #1A201D; color: #E6EDE9; }"
        "QLabel#userParamsTitle { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "ConfigUserDefinedView QPushButton { padding: 5px 12px; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(0);

    auto *title = new QLabel(tr("User Params"), this);
    title->setObjectName(QStringLiteral("userParamsTitle"));
    root->addWidget(title);

    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 8, 0, 12);
    toolbar->setSpacing(8);
    auto *modify = new QPushButton(tr("Modify"), this);
    modify->setObjectName(QStringLiteral("modifyButton"));
    auto *refresh = new QPushButton(tr("Refresh Params"), this);
    refresh->setObjectName(QStringLiteral("refreshUserParamsButton"));
    toolbar->addWidget(modify);
    toolbar->addWidget(refresh);
    toolbar->addStretch(1);
    root->addLayout(toolbar);

    m_editor->setObjectName(QStringLiteral("userParamsEditor"));
    m_editor->setEmbeddedMode(true);
    m_editor->setUnavailableMessage(
        tr("No selected parameters are available on this vehicle."));
    root->addWidget(m_editor, 1);

    connect(modify, &QPushButton::clicked,
            this, &ConfigUserDefinedView::showModifyDialog);
    connect(refresh, &QPushButton::clicked, this, [this]() {
        const int componentId = m_editor->selectedComponent();
        emit refreshRequested(componentId > 0 ? componentId : 1);
    });
    connect(m_editor, &ConfigFriendlyParamsView::refreshRequested,
            this, &ConfigUserDefinedView::refreshRequested);
    connect(m_editor, &ConfigFriendlyParamsView::writeRequested,
            this, &ConfigUserDefinedView::writeRequested);
    connect(m_viewModel, &ConfigUserDefinedViewModel::optionsChanged,
            this, &ConfigUserDefinedView::applyConfiguredOptions);
    applyConfiguredOptions();
}

void ConfigUserDefinedView::setCatalog(
    const ParameterMetaDataCatalog &catalog, bool enforceMetadataRanges)
{
    m_editor->setCatalog(catalog, enforceMetadataRanges);
    m_editor->setUnavailableMessage(
        tr("No selected parameters are available on this vehicle."));
}

void ConfigUserDefinedView::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    m_editor->setParameterSnapshot(parameters, preferredComponent);
}

void ConfigUserDefinedView::ApplyOptions(const QString &raw)
{
    m_viewModel->ApplyOptions(raw);
}

int ConfigUserDefinedView::visibleParameterCount() const
{
    return m_editor->visibleParameterCount();
}

void ConfigUserDefinedView::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    m_editor->parameterChanged(componentId, name, value);
}

void ConfigUserDefinedView::parameterWriteFailed(
    int componentId, const QString &name, const QString &reason)
{
    m_editor->parameterWriteFailed(componentId, name, reason);
}

void ConfigUserDefinedView::parameterWriteFailed(
    int componentId, const QString &name, const QVariant &attemptedValue,
    const QString &reason)
{
    m_editor->parameterWriteFailed(componentId, name, attemptedValue, reason);
}

void ConfigUserDefinedView::showModifyDialog()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("userParamsModifyDialog"));
    dialog.setWindowTitle(tr("Params"));
    dialog.resize(360, 320);
    dialog.setStyleSheet(QStringLiteral(
        "QDialog#userParamsModifyDialog { background: #434445; }"));

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);
    auto *prompt = new QLabel(
        tr("Enter Param Names (comma or newline separated)"), &dialog);
    prompt->setObjectName(QStringLiteral("userParamsPrompt"));
    prompt->setWordWrap(true);
    layout->addWidget(prompt);
    auto *editor = new QPlainTextEdit(&dialog);
    editor->setObjectName(QStringLiteral("userParamsTextEdit"));
    editor->setPlainText(m_viewModel->OptionsText());
    editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    editor->setMinimumHeight(220);
    layout->addWidget(editor, 1);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        Qt::Horizontal, &dialog);
    buttons->setObjectName(QStringLiteral("userParamsDialogButtons"));
    buttons->button(QDialogButtonBox::Ok)->setDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted,
            &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected,
            &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) {
        ApplyOptions(editor->toPlainText());
    }
}

void ConfigUserDefinedView::applyConfiguredOptions()
{
    m_editor->setCustomParameterNames(m_viewModel->Options());
}
