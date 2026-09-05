#include "QuickViewWidget.h"

#include <QAction>
#include <QColor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFontMetrics>
#include <QGridLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>
#include <cmath>

namespace {
const QStringList defaults{QStringLiteral("alt"), QStringLiteral("groundspeed"),
    QStringLiteral("current"), QStringLiteral("airspeed"),
    QStringLiteral("verticalspeed"), QStringLiteral("DistToHome")};
const QStringList quickPalette{QStringLiteral("#D197F8"), QStringLiteral("#FE842E"),
    QStringLiteral("#FF605B"), QStringLiteral("#00FF53"),
    QStringLiteral("#FEFE56"), QStringLiteral("#00FFFC")};
class Cell final : public QWidget {
public:
    explicit Cell(QWidget *parent) : QWidget(parent) {}
    std::function<void()> activate;
    void setLabels(QLabel *description, QLabel *number) {
        m_description = description;
        m_number = number;
        description->installEventFilter(this);
        number->installEventFilter(this);
    }
    void setTextColors(const QString &description, const QString &number) {
        m_descriptionColor = description;
        m_numberColor = number;
        updateTextSizes();
    }
    void updateTextSizes() {
        if (!m_description || !m_number || !layout()) return;
        const QRect available = contentsRect().marginsRemoved(layout()->contentsMargins());
        // MP10 QuickView.Render scales the description with cell height and the
        // number with the remaining height. Fit both axes, including long field
        // names/units and negative coordinates, without changing their contents.
        // Use actual label rectangles: layout rounding/minimums need not match
        // an arithmetic 1:4 split, especially immediately after a grid rebuild.
        fitFont(m_description, qMax(9, int(available.height() * 0.16)), m_descriptionColor);
        fitFont(m_number, qMax(10, int(m_number->contentsRect().height() * 0.9)), m_numberColor);
    }
protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if ((watched == m_description || watched == m_number) && event->type() == QEvent::Resize)
            updateTextSizes();
        return QWidget::eventFilter(watched, event);
    }
    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        updateTextSizes();
    }
    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && activate) {
            const auto callback = activate;
            callback();
        }
    }
private:
    static void fitFont(QLabel *label, int maximumPixels, const QString &color) {
        const int width = qMax(1, label->contentsRect().width() - 2);
        const int height = qMax(1, label->contentsRect().height());
        QFont font = label->font();
        int low = 1;
        int high = maximumPixels;
        int best = 1;
        while (low <= high) {
            const int pixels = low + (high - low) / 2;
            font.setPixelSize(pixels);
            const QFontMetrics metrics(font);
            const int textWidth = qMax(metrics.horizontalAdvance(label->text()),
                                       metrics.boundingRect(label->text()).width());
            if (textWidth <= width && metrics.height() <= height) {
                best = pixels;
                low = pixels + 1;
            } else {
                high = pixels - 1;
            }
        }
        // The production theme sets QWidget { font-size: 11px; }. A plain
        // setFont() loses to that rule on repolish, including warning updates.
        // Keep the computed size in the same local rule as its foreground color.
        const QString style = QStringLiteral("background: transparent; color: %1; font-size: %2px;")
            .arg(color).arg(best);
        if (label->styleSheet() != style) label->setStyleSheet(style);
    }
    QLabel *m_description = nullptr;
    QLabel *m_number = nullptr;
    QString m_descriptionColor = QStringLiteral("#ffffff");
    QString m_numberColor = QStringLiteral("#ffffff");
};
}

QuickViewWidget::QuickViewWidget(QSettings *settings, const QStringList &fields,
                               QWidget *parent)
    : QWidget(parent), m_settings(settings), m_fields(fields)
{
    setObjectName(QStringLiteral("QuickHost"));
    m_grid = new QGridLayout(this);
    m_grid->setObjectName(QStringLiteral("QuickGrid"));
    m_grid->setContentsMargins(4, 4, 4, 4);
    m_grid->setSpacing(4);
    m_columns = qBound(1, settings ? settings->value("quickViewColumns", 2).toInt() : 2, 6);
    const int count = qBound(1, settings ? settings->value("quickViewCount", 6).toInt() : 6, 12);
    for (int i = 0; i < count; ++i) {
        const QString saved = settings ? settings->value(
            QStringLiteral("quickView%1").arg(i + 1)).toString() : QString();
        m_selected.append(saved.isEmpty() ? defaults[i % defaults.size()] : saved);
    }
    auto *layoutAction = new QAction(tr("QuickView Layout"), this);
    connect(layoutAction, &QAction::triggered, this, &QuickViewWidget::configureLayout);
    addAction(layoutAction);
    setContextMenuPolicy(Qt::ActionsContextMenu);
    rebuild();
}

bool QuickViewWidget::setField(int index, const QString &field)
{
    if (index < 0 || index >= m_selected.size() || !m_fields.contains(field)) return false;
    m_selected[index] = field;
    if (m_settings) m_settings->setValue(QStringLiteral("quickView%1").arg(index + 1), field);
    refresh();
    return true;
}

bool QuickViewWidget::setGridLayout(int columns, int count)
{
    if (columns < 1 || columns > 6 || count < 1 || count > 12) return false;
    m_columns = columns;
    while (m_selected.size() > count) m_selected.removeLast();
    while (m_selected.size() < count) {
        const int i = m_selected.size();
        const QString saved = m_settings ? m_settings->value(
            QStringLiteral("quickView%1").arg(i + 1)).toString() : QString();
        m_selected.append(saved.isEmpty() ? defaults[i % defaults.size()] : saved);
    }
    if (m_settings) {
        m_settings->setValue(QStringLiteral("quickViewColumns"), columns);
        m_settings->setValue(QStringLiteral("quickViewCount"), count);
    }
    rebuild();
    return true;
}

void QuickViewWidget::setUnits(std::function<QString(const QString &)> units)
{
    QPointer<QuickViewWidget> guard(this);
    const auto fields = m_fields;
    QHash<QString, QString> labels;
    for (const auto &field : fields) {
        const QString unit = units ? units(field) : QString();
        if (!guard) return;
        labels.insert(field, unit);
    }
    m_units = std::move(labels);
    refresh();
}
void QuickViewWidget::setValues(const Values &values) { m_values = values; refresh(); }
void QuickViewWidget::setWarningColors(const Colors &colors) { m_colors = colors; refresh(); }

void QuickViewWidget::rebuild()
{
    while (auto *item = m_grid->takeAt(0)) { delete item->widget(); delete item; }
    for (int row = 0; row < m_grid->rowCount(); ++row) m_grid->setRowStretch(row, 0);
    for (int col = 0; col < m_grid->columnCount(); ++col) m_grid->setColumnStretch(col, 0);
    m_cells.clear(); m_labels.clear(); m_numbers.clear();
    for (int i = 0; i < m_selected.size(); ++i) {
        auto *cell = new Cell(this);
        cell->setObjectName(QStringLiteral("QuickCell_%1").arg(i));
        cell->setAttribute(Qt::WA_StyledBackground);
        cell->activate = [this, i]() { chooseField(i); };
        cell->setToolTip(tr("Double-click to select a telemetry field. Right-click for layout."));
        auto *layout = new QVBoxLayout(cell);
        layout->setContentsMargins(4, 2, 4, 2);
        layout->setSpacing(2);
        auto *label = new QLabel(cell);
        label->setObjectName(QStringLiteral("QuickDescription_%1").arg(i));
        label->setAlignment(Qt::AlignCenter);
        label->setTextFormat(Qt::PlainText);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto *number = new QLabel(cell);
        number->setObjectName(QStringLiteral("QuickNumber_%1").arg(i));
        number->setAlignment(Qt::AlignCenter);
        number->setTextFormat(Qt::PlainText);
        number->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        number->setAttribute(Qt::WA_TransparentForMouseEvents);
        // Ignore font-derived minimum hints: larger text must not prevent the
        // splitter/grid from becoming small again after a window enlargement.
        layout->addWidget(label, 1);
        layout->addWidget(number, 4);
        cell->setLabels(label, number);
        m_grid->addWidget(cell, i / m_columns, i % m_columns);
        m_grid->setRowStretch(i / m_columns, 1);
        m_grid->setColumnStretch(i % m_columns, 1);
        m_cells.append(cell); m_labels.append(label); m_numbers.append(number);
    }
    refresh();
}

void QuickViewWidget::refresh()
{
    for (int i = 0; i < m_cells.size(); ++i) {
        const QString field = m_selected[i];
        const QString unit = m_units.value(field);
        const auto value = m_values.constFind(field);
        const bool valid = value != m_values.cend() && std::isfinite(value.value());
        const QString help = tr("Double-click to select a telemetry field. Right-click for layout.");
        m_cells[i]->setToolTip(!valid && field == QStringLiteral("DistToHome")
            ? tr("Distance needs a fresh vehicle position and HOME_POSITION from the selected vehicle. A connection alone is not enough.\n%1").arg(help)
            : help);
        m_labels[i]->setText(field + (unit.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(unit)));
        m_numbers[i]->setText(valid ? QString::number(value.value(), 'f',
            field == QStringLiteral("lat") || field == QStringLiteral("lng") ? 7 : 2) : QStringLiteral("—"));
        const QString named = valid ? m_colors.value(field) : QString();
        const QColor background(named);
        const bool colored = !named.isEmpty() && named != QStringLiteral("NoColor") && background.isValid();
        const QString foreground = colored
            ? ((background.red() + background.green() + background.blue()) / 3 > 128
                ? QStringLiteral("#000000") : QStringLiteral("#ffffff"))
            : quickPalette[i % quickPalette.size()];
        m_cells[i]->setProperty("warningColor", colored ? named : QStringLiteral("NoColor"));
        m_cells[i]->setStyleSheet(QStringLiteral("QWidget#QuickCell_%1 { background-color: %2; }")
            .arg(i).arg(colored ? background.name() : QStringLiteral("transparent")));
        static_cast<Cell *>(m_cells[i])->setTextColors(
            colored ? foreground : QStringLiteral("#ffffff"), foreground);
    }
}

void QuickViewWidget::chooseField(int index)
{
    if (index < 0 || index >= m_selected.size()) return;
    const QString original = m_selected[index];
    QPointer<QuickViewWidget> guard(this);
    bool ok = false;
    const QString field = QInputDialog::getItem(this, tr("Quick field"),
        tr("Telemetry field (canonical units)"), m_fields,
        qMax(0, m_fields.indexOf(original)), true, &ok);
    if (guard && ok && index < m_selected.size() && m_selected[index] == original)
        setField(index, field);
}

void QuickViewWidget::configureLayout()
{
    // No application modal state is retained across the confirmation boundary.
    QPointer<QuickViewWidget> guard(this);
    QPointer<QDialog> dialog = new QDialog(this);
    dialog->setWindowTitle(tr("QuickView Layout"));
    auto *layout = new QFormLayout(dialog);
    auto *columns = new QSpinBox(dialog); columns->setRange(1, 6); columns->setValue(m_columns);
    auto *rows = new QSpinBox(dialog); rows->setRange(1, 12);
    rows->setValue((m_selected.size() + m_columns - 1) / m_columns);
    layout->addRow(tr("Columns (1..6)"), columns);
    layout->addRow(tr("Rows (maximum 12 cells)"), rows);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    const auto validate = [=]() { buttons->button(QDialogButtonBox::Ok)->setEnabled(columns->value() * rows->value() <= 12); };
    connect(columns, QOverload<int>::of(&QSpinBox::valueChanged), dialog, validate);
    connect(rows, QOverload<int>::of(&QSpinBox::valueChanged), dialog, validate);
    validate();
    if (dialog->exec() == QDialog::Accepted && guard && dialog)
        setGridLayout(columns->value(), columns->value() * rows->value());
    delete dialog.data();
}
