#ifndef PREFLIGHTCHECKLISTWIDGET_H
#define PREFLIGHTCHECKLISTWIDGET_H

#include <QModelIndex>
#include <QVector>
#include <QWidget>

class QLabel;
class QCheckBox;
class PreflightChecklistModel;

class PreflightChecklistWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit PreflightChecklistWidget(PreflightChecklistModel *model,
                                      QWidget *parent = nullptr);

    PreflightChecklistModel *model() const;

private:
    struct RowWidgets
    {
        QWidget *container = nullptr;
        QLabel *description = nullptr;
        QLabel *value = nullptr;
        QCheckBox *checkBox = nullptr;
    };

    void buildRows(QWidget *rowParent);
    void syncRows(const QModelIndex &topLeft,
                  const QModelIndex &bottomRight);
    void syncRow(int row);

    PreflightChecklistModel *m_model = nullptr;
    QVector<RowWidgets> m_rows;
};

#endif // PREFLIGHTCHECKLISTWIDGET_H
