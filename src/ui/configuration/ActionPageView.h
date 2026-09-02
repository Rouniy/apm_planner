#ifndef ACTIONPAGEVIEW_H
#define ACTIONPAGEVIEW_H

#include <QWidget>
#include <QVector>

#include <functional>

class QGridLayout;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QResizeEvent;
class QScrollArea;

/** Shared Qt Widgets counterpart of Mission Planner 10's ActionPageView. */
class ActionPageView : public QWidget
{
    Q_OBJECT

public:
    using ActionCallback = std::function<void()>;

    explicit ActionPageView(const QString &title,
                            const QString &instructions,
                            QWidget *parent = nullptr);

    QPushButton *AddAction(const QString &label,
                           const QString &objectName,
                           ActionCallback callback,
                           bool enabled = true,
                           const QString &unavailableReason = QString());
    QPushButton *AddUnavailableAction(const QString &label,
                                      const QString &objectName,
                                      const QString &reason);

    QString Title() const;
    QString Instructions() const;
    QString Log() const;
    int ActionCount() const;
    int ColumnCount() const;

public slots:
    void AppendLog(const QString &line);
    void ClearLog();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void reflowActions(int availableWidth, bool force = false);

    QLabel *m_title = nullptr;
    QLabel *m_instructions = nullptr;
    QWidget *m_actionHost = nullptr;
    QGridLayout *m_actionLayout = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QScrollArea *m_actionScroll = nullptr;
    QVector<QPushButton *> m_actions;
    int m_actionCount = 0;
    int m_columnCount = 0;
};

#endif // ACTIONPAGEVIEW_H
