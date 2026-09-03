#include "ConfigOSDView.h"

#include "ConfigOSDLayoutCanvas.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QVBoxLayout>

namespace {

const QString kTuningSlotsUnavailableReason = QStringLiteral(
    "Not available in phase 1: OSD 5/6 tuning-slot MAVLink transport "
    "is not ported yet.");

} // namespace

ConfigOSDView::ConfigOSDView(QWidget *parent)
    : QWidget(parent),
      m_viewModel(new ConfigOSDViewModel(this))
{
    setObjectName(QStringLiteral("ConfigOSDView"));
    buildUi();

    connect(m_viewModel, &ConfigOSDViewModel::screensChanged,
            this, &ConfigOSDView::rebuildScreens);
    connect(m_viewModel, &ConfigOSDViewModel::selectedScreenChanged,
            this, [this](int) {
        rebuildScreens();
        syncItems();
    });
    connect(m_viewModel, &ConfigOSDViewModel::itemsChanged,
            this, &ConfigOSDView::syncItems);
    connect(m_viewModel, &ConfigOSDViewModel::itemChanged,
            this, [this](int, const QString &) { syncItems(); });
    connect(m_viewModel, &ConfigOSDViewModel::dirtyChanged,
            this, &ConfigOSDView::syncState);
    connect(m_viewModel, &ConfigOSDViewModel::stateChanged,
            this, &ConfigOSDView::syncState);
    connect(m_viewModel, &ConfigOSDViewModel::statusChanged,
            this, &ConfigOSDView::syncStatus);
    connect(m_viewModel, &ConfigOSDViewModel::writeParamsRequested,
            this, &ConfigOSDView::writeParamsRequested);

    rebuildScreens();
    syncItems();
    syncStatus();
    syncState();
}

ConfigOSDView::~ConfigOSDView() = default;

QSize ConfigOSDView::sizeHint() const
{
    return QSize(1120, 650);
}

void ConfigOSDView::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    m_viewModel->setParameterSnapshot(parameters, preferredComponent);
}

void ConfigOSDView::setConnected(bool connected)
{
    m_viewModel->setConnected(connected);
    syncStatus();
    syncState();
}

void ConfigOSDView::parameterTargetChanged()
{
    m_viewModel->parameterTargetChanged();
}

void ConfigOSDView::parameterBatchSubmitted(
    int componentId, qulonglong batchId)
{
    m_viewModel->parameterBatchSubmitted(componentId, batchId);
}

void ConfigOSDView::parameterBatchProgress(
    qulonglong batchId, int completed, int total, int succeeded, int failed)
{
    m_viewModel->parameterBatchProgress(
        batchId, completed, total, succeeded, failed);
}

void ConfigOSDView::parameterWriteFailed(
    qulonglong batchId, int componentId, const QString &name,
    const QString &reason)
{
    m_viewModel->parameterWriteFailed(
        batchId, componentId, name, reason);
}

void ConfigOSDView::parameterBatchCompleted(
    qulonglong batchId, int succeeded, int failed)
{
    m_viewModel->parameterBatchCompleted(batchId, succeeded, failed);
}

void ConfigOSDView::parameterBatchCancelled(qulonglong batchId)
{
    m_viewModel->parameterBatchCancelled(batchId);
}

void ConfigOSDView::parameterWriteSubmissionFailed(const QString &reason)
{
    m_viewModel->parameterWriteSubmissionFailed(reason);
}

void ConfigOSDView::buildUi()
{
    setStyleSheet(QStringLiteral(
        "ConfigOSDView { background: #1A201D; color: #E6EDE9; }"
        "QLabel#configOsdTitle { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QLabel#configOsdIntro { color: #C8C8C8; }"
        "QLabel#configOsdStatus { color: #34D399; }"
        "QWidget[configOsdItemRow=\"true\"] {"
        " background: #202623; border: 1px solid #303A35; }"
        "QPushButton { background: #161B18; color: #E6EDE9;"
        " border: 1px solid #303A35; padding: 5px 10px; }"
        "QPushButton:disabled { color: #68736D; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);

    m_title = new QLabel(ConfigOSDViewModel::Title(), this);
    m_title->setObjectName(QStringLiteral("configOsdTitle"));
    root->addWidget(m_title);

    m_intro = new QLabel(ConfigOSDViewModel::Intro(), this);
    m_intro->setObjectName(QStringLiteral("configOsdIntro"));
    m_intro->setWordWrap(true);
    m_intro->setTextFormat(Qt::PlainText);
    root->addWidget(m_intro);

    auto *toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("configOsdToolbar"));
    auto *toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(8);

    m_refresh = new QPushButton(tr("Refresh Params"), toolbar);
    m_refresh->setObjectName(QStringLiteral("configOsdRefreshParams"));
    toolbarLayout->addWidget(m_refresh);

    auto *screenLabel = new QLabel(tr("Screen"), toolbar);
    screenLabel->setObjectName(QStringLiteral("configOsdScreenLabel"));
    toolbarLayout->addWidget(screenLabel);

    m_screen = new QComboBox(toolbar);
    m_screen->setObjectName(QStringLiteral("configOsdScreen"));
    m_screen->setMinimumContentsLength(8);
    toolbarLayout->addWidget(m_screen);

    m_enableAll = new QPushButton(tr("Enable All"), toolbar);
    m_enableAll->setObjectName(QStringLiteral("configOsdEnableAll"));
    toolbarLayout->addWidget(m_enableAll);

    m_disableAll = new QPushButton(tr("Disable All"), toolbar);
    m_disableAll->setObjectName(QStringLiteral("configOsdDisableAll"));
    toolbarLayout->addWidget(m_disableAll);

    m_tuningSlots = new QPushButton(
        tr("OSD 5/6 Tuning Slots…"), toolbar);
    m_tuningSlots->setObjectName(QStringLiteral("configOsdTuningSlots"));
    m_tuningSlots->setEnabled(false);
    m_tuningSlots->setToolTip(kTuningSlotsUnavailableReason);
    toolbarLayout->addWidget(m_tuningSlots);
    toolbarLayout->addStretch(1);
    root->addWidget(toolbar);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("configOsdStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    root->addWidget(m_status);

    auto *editorArea = new QSplitter(Qt::Horizontal, this);
    editorArea->setObjectName(QStringLiteral("configOsdEditorArea"));
    editorArea->setChildrenCollapsible(false);

    auto *canvasScroll = new QScrollArea(editorArea);
    canvasScroll->setObjectName(QStringLiteral("configOsdCanvasScroll"));
    canvasScroll->setFrameShape(QFrame::NoFrame);
    canvasScroll->setWidgetResizable(false);
    canvasScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    canvasScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    canvasScroll->setMinimumWidth(360);
    m_canvas = new ConfigOSDLayoutCanvas(canvasScroll);
    canvasScroll->setWidget(m_canvas);
    editorArea->addWidget(canvasScroll);

    m_itemScroll = new QScrollArea(editorArea);
    m_itemScroll->setObjectName(QStringLiteral("configOsdItemsScroll"));
    m_itemScroll->setFrameShape(QFrame::NoFrame);
    m_itemScroll->setWidgetResizable(true);
    m_itemScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_itemScroll->setMinimumWidth(300);
    editorArea->addWidget(m_itemScroll);
    editorArea->setStretchFactor(0, 3);
    editorArea->setStretchFactor(1, 1);
    editorArea->setSizes({780, 300});
    root->addWidget(editorArea, 1);

    auto *actionRow = new QWidget(this);
    actionRow->setObjectName(QStringLiteral("configOsdActionRow"));
    auto *actionLayout = new QHBoxLayout(actionRow);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(8);
    actionLayout->addStretch(1);

    m_write = new QPushButton(tr("Write customization"), actionRow);
    m_write->setObjectName(QStringLiteral("configOsdWriteCustomization"));
    actionLayout->addWidget(m_write);

    m_discard = new QPushButton(tr("Discard all changes"), actionRow);
    m_discard->setObjectName(QStringLiteral("configOsdDiscardAllChanges"));
    actionLayout->addWidget(m_discard);
    root->addWidget(actionRow);

    connect(m_refresh, &QPushButton::clicked,
            this, &ConfigOSDView::requestRefresh);
    connect(m_screen, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        if (index >= 0) {
            m_viewModel->selectScreen(m_screen->itemData(index).toInt());
        }
    });
    connect(m_enableAll, &QPushButton::clicked,
            this, [this]() { m_viewModel->EnableAll(true); });
    connect(m_disableAll, &QPushButton::clicked,
            this, [this]() { m_viewModel->EnableAll(false); });
    connect(m_write, &QPushButton::clicked,
            this, &ConfigOSDView::writeChanges);
    connect(m_discard, &QPushButton::clicked,
            this, &ConfigOSDView::discardChanges);
    connect(m_canvas, &ConfigOSDLayoutCanvas::positionEdited,
            this, &ConfigOSDView::canvasPositionEdited);
}

void ConfigOSDView::rebuildScreens()
{
    const QSignalBlocker blocker(m_screen);
    const QList<int> screens = m_viewModel->Screens();
    m_screen->clear();
    for (int screen : screens) {
        m_screen->addItem(tr("Screen %1").arg(screen), screen);
    }
    const int current = m_screen->findData(m_viewModel->SelectedScreen());
    m_screen->setCurrentIndex(current);
}

void ConfigOSDView::syncItems()
{
    const QList<ConfigOSDItem> items = m_viewModel->Items();
    bool rebuild = !m_itemScroll->widget()
        || items.size() != m_itemEditors.size();
    if (!rebuild) {
        for (const ConfigOSDItem &item : items) {
            if (!m_itemEditors.contains(itemKey(item.screen, item.name))) {
                rebuild = true;
                break;
            }
        }
    }
    if (rebuild) {
        rebuildItemEditors(items);
    } else {
        syncItemEditors(items);
    }

    QVector<ConfigOSDLayoutItem> canvasItems;
    canvasItems.reserve(items.size());
    for (const ConfigOSDItem &item : items) {
        ConfigOSDLayoutItem canvasItem;
        canvasItem.key = itemKey(item.screen, item.name);
        canvasItem.caption = item.name;
        canvasItem.name = item.name;
        canvasItem.enabled = item.enabled;
        canvasItem.x = item.x;
        canvasItem.y = item.y;
        canvasItems.append(canvasItem);
    }
    m_canvas->setItems(canvasItems);
    syncState();
}

void ConfigOSDView::rebuildItemEditors(
    const QList<ConfigOSDItem> &items)
{
    QWidget *oldContent = m_itemScroll->takeWidget();
    if (oldContent) {
        oldContent->deleteLater();
    }
    m_itemEditors.clear();

    auto *content = new QWidget(m_itemScroll);
    content->setObjectName(QStringLiteral("configOsdItemsContent"));
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    if (items.isEmpty()) {
        auto *empty = new QLabel(
            tr("No OSD items are available on this screen."), content);
        empty->setObjectName(QStringLiteral("configOsdEmptyItems"));
        empty->setWordWrap(true);
        empty->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        layout->addWidget(empty);
    }

    for (const ConfigOSDItem &item : items) {
        const QString key = itemKey(item.screen, item.name);
        const QString suffix = itemObjectSuffix(item.screen, item.name);
        ItemEditors editors;
        editors.row = new QWidget(content);
        editors.row->setObjectName(
            QStringLiteral("configOsdItemRow_%1").arg(suffix));
        editors.row->setProperty("configOsdItemRow", true);
        auto *grid = new QGridLayout(editors.row);
        grid->setContentsMargins(8, 6, 8, 6);
        grid->setHorizontalSpacing(6);
        grid->setVerticalSpacing(4);

        editors.enabled = new QCheckBox(tr("Enabled"), editors.row);
        editors.enabled->setObjectName(
            QStringLiteral("configOsdItemEnabled_%1").arg(suffix));
        grid->addWidget(editors.enabled, 0, 0, 1, 4);

        auto *nameCaption = new QLabel(tr("Name"), editors.row);
        nameCaption->setObjectName(
            QStringLiteral("configOsdItemNameCaption_%1").arg(suffix));
        grid->addWidget(nameCaption, 1, 0);
        editors.name = new QLabel(item.name, editors.row);
        editors.name->setObjectName(
            QStringLiteral("configOsdItemName_%1").arg(suffix));
        editors.name->setTextInteractionFlags(Qt::TextSelectableByMouse);
        grid->addWidget(editors.name, 1, 1, 1, 3);

        auto *xCaption = new QLabel(tr("X"), editors.row);
        xCaption->setObjectName(
            QStringLiteral("configOsdItemXCaption_%1").arg(suffix));
        grid->addWidget(xCaption, 2, 0);
        editors.x = new QSpinBox(editors.row);
        editors.x->setObjectName(
            QStringLiteral("configOsdItemX_%1").arg(suffix));
        editors.x->setRange(
            0, qMax(ConfigOSDViewModel::Columns() - 1, item.x));
        grid->addWidget(editors.x, 2, 1);

        auto *yCaption = new QLabel(tr("Y"), editors.row);
        yCaption->setObjectName(
            QStringLiteral("configOsdItemYCaption_%1").arg(suffix));
        grid->addWidget(yCaption, 2, 2);
        editors.y = new QSpinBox(editors.row);
        editors.y->setObjectName(
            QStringLiteral("configOsdItemY_%1").arg(suffix));
        editors.y->setRange(
            0, qMax(ConfigOSDViewModel::Rows() - 1, item.y));
        grid->addWidget(editors.y, 2, 3);

        connect(editors.enabled, &QCheckBox::toggled,
                this, [this, screen = item.screen, name = item.name](
                    bool enabled) {
            if (!m_viewModel->setItemEnabled(screen, name, enabled)) {
                syncItems();
            }
        });
        connect(editors.x, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this, screen = item.screen, name = item.name](int x) {
            if (!m_viewModel->setItemX(screen, name, x)) {
                syncItems();
            }
        });
        connect(editors.y, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this, screen = item.screen, name = item.name](int y) {
            if (!m_viewModel->setItemY(screen, name, y)) {
                syncItems();
            }
        });

        layout->addWidget(editors.row);
        m_itemEditors.insert(key, editors);
    }
    layout->addStretch(1);
    m_itemScroll->setWidget(content);
    syncItemEditors(items);
}

void ConfigOSDView::syncItemEditors(
    const QList<ConfigOSDItem> &items)
{
    const bool canEdit = m_viewModel->CanEdit();
    for (const ConfigOSDItem &item : items) {
        const auto found = m_itemEditors.constFind(
            itemKey(item.screen, item.name));
        if (found == m_itemEditors.constEnd()) {
            continue;
        }
        const ItemEditors &editors = found.value();
        const QSignalBlocker enabledBlocker(editors.enabled);
        const QSignalBlocker xBlocker(editors.x);
        const QSignalBlocker yBlocker(editors.y);
        editors.enabled->setChecked(item.enabled);
        editors.name->setText(item.name);
        editors.x->setMaximum(
            qMax(ConfigOSDViewModel::Columns() - 1, item.x));
        editors.y->setMaximum(
            qMax(ConfigOSDViewModel::Rows() - 1, item.y));
        editors.x->setValue(item.x);
        editors.y->setValue(item.y);
        editors.enabled->setEnabled(canEdit);
        editors.x->setEnabled(canEdit);
        editors.y->setEnabled(canEdit);
    }
}

void ConfigOSDView::syncState()
{
    const bool busy = m_viewModel->Busy();
    const bool canEdit = m_viewModel->CanEdit();
    m_refresh->setEnabled(m_viewModel->Connected() && !busy);
    m_screen->setEnabled(!busy && !m_viewModel->Screens().isEmpty());
    m_enableAll->setEnabled(canEdit);
    m_disableAll->setEnabled(canEdit);
    m_canvas->setEnabled(canEdit);
    m_write->setEnabled(m_viewModel->CanWrite());
    m_discard->setEnabled(!busy && m_viewModel->SnapshotReady()
                          && m_viewModel->HasDirtyChanges());
    m_tuningSlots->setEnabled(false);
    syncItemEditors(m_viewModel->Items());
}

void ConfigOSDView::syncStatus()
{
    const QString status = !m_viewModel->Status().isEmpty()
        ? m_viewModel->Status()
        : (m_viewModel->Connected()
               ? ConfigOSDViewModel::UnreadyStatus()
               : ConfigOSDViewModel::OfflineStatus());
    m_status->setText(status);
}

void ConfigOSDView::requestRefresh()
{
    if (!m_viewModel->Connected() || m_viewModel->Busy()) {
        return;
    }
    if (m_viewModel->HasDirtyChanges()
        && QMessageBox::question(
               this, tr("Refresh Params"),
               tr("Refreshing may replace staged OSD changes if the vehicle "
                  "values changed. Continue?"),
               QMessageBox::Yes | QMessageBox::No,
               QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    emit refreshRequested(m_viewModel->ComponentId());
}

void ConfigOSDView::writeChanges()
{
    m_viewModel->WriteChanges();
}

void ConfigOSDView::discardChanges()
{
    m_viewModel->Discard();
}

void ConfigOSDView::canvasPositionEdited(
    const QString &key, int x, int y)
{
    const QList<ConfigOSDItem> items = m_viewModel->Items();
    for (const ConfigOSDItem &item : items) {
        if (itemKey(item.screen, item.name) == key) {
            if (!m_viewModel->setItemPosition(item.screen, item.name, x, y)) {
                syncItems();
            }
            return;
        }
    }
    syncItems();
}

QString ConfigOSDView::itemKey(int screen, const QString &name)
{
    return QString::number(screen) + QLatin1Char('/') + name;
}

QString ConfigOSDView::itemObjectSuffix(
    int screen, const QString &name)
{
    QString suffix = QString::number(screen) + QLatin1Char('_') + name;
    for (int index = 0; index < suffix.size(); ++index) {
        const QChar character = suffix.at(index);
        if (!character.isLetterOrNumber() && character != QLatin1Char('_')) {
            suffix[index] = QLatin1Char('_');
        }
    }
    return suffix;
}
