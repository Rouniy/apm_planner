#ifndef QUICKVIEWWIDGET_H
#define QUICKVIEWWIDGET_H

#include <QHash>
#include <QPointer>
#include <QStringList>
#include <QWidget>
#include <functional>

class QGridLayout;
class QLabel;
class QSettings;
class QuickViewWidget final : public QWidget
{
    Q_OBJECT
public:
    using Values = QHash<QString, double>;
    using Colors = QHash<QString, QString>;
    explicit QuickViewWidget(QSettings *settings, const QStringList &fields,
                             QWidget *parent = nullptr);
    QStringList selectedFields() const { return m_selected; }
    int columns() const { return m_columns; }
    bool setField(int index, const QString &field);
    bool setGridLayout(int columns, int count);
    void setUnits(std::function<QString(const QString &)> units);
    void setValues(const Values &values);
    void setWarningColors(const Colors &colors);
private:
    void rebuild();
    void refresh();
    void chooseField(int index);
    void configureLayout();
    QPointer<QSettings> m_settings;
    QStringList m_fields;
    QStringList m_selected;
    int m_columns = 2;
    QGridLayout *m_grid = nullptr;
    QVector<QWidget *> m_cells;
    QVector<QLabel *> m_labels;
    QVector<QLabel *> m_numbers;
    Values m_values;
    Colors m_colors;
    QHash<QString, QString> m_units;
};
#endif
