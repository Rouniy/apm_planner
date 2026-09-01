#ifndef BACKSTAGEVIEW_H
#define BACKSTAGEVIEW_H

#include <QHash>
#include <QWidget>

class QAbstractButton;
class QButtonGroup;
class QLabel;
class QProgressBar;
class QStackedWidget;
class QVBoxLayout;

class BackstageView : public QWidget
{
    Q_OBJECT

public:
    explicit BackstageView(QWidget *parent = nullptr);

    void addGroup(const QString &title, const QString &objectName = QString());
    bool addPage(const QString &id,
                 const QString &title,
                 QWidget *page,
                 bool subPage = false,
                 const QString &badge = QString());

    QString currentPageId() const;
    QStringList pageIds() const;
    QWidget *page(const QString &id) const;
    bool isPageVisible(const QString &id) const;
    bool isGroupExpanded(const QString &id) const;

public slots:
    bool setCurrentPage(const QString &id);
    bool setPageVisible(const QString &id, bool visible);
    bool setGroupExpanded(const QString &id, bool expanded);
    void setLoading(bool loading,
                    const QString &message = QString(),
                    int progress = -1);

signals:
    void currentPageChanged(const QString &id);

private:
    struct PageEntry
    {
        QAbstractButton *button = nullptr;
        QWidget *page = nullptr;
        QString groupId;
        bool requestedVisible = true;
    };

    struct GroupEntry
    {
        QAbstractButton *button = nullptr;
        QStringList pageIds;
        bool expanded = true;
    };

    QString firstVisiblePageId() const;
    void updatePageButtonVisibility(const QString &id);
    void selectFallbackPage();

    QVBoxLayout *m_navigationLayout = nullptr;
    QButtonGroup *m_pageGroup = nullptr;
    QStackedWidget *m_pageStack = nullptr;
    QWidget *m_loadingOverlay = nullptr;
    QLabel *m_loadingLabel = nullptr;
    QProgressBar *m_loadingProgress = nullptr;
    QHash<QString, PageEntry> m_pages;
    QHash<QString, GroupEntry> m_groups;
    QStringList m_pageOrder;
    QString m_currentGroupId;
};

#endif
