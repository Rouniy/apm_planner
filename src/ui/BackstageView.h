#ifndef BACKSTAGEVIEW_H
#define BACKSTAGEVIEW_H

#include <QHash>
#include <QString>
#include <QWidget>

#include <functional>

class QAbstractButton;
class QButtonGroup;
class QStackedWidget;
class QVBoxLayout;
class ConfigParamLoadingView;

class BackstagePage
{
public:
    using Factory = std::function<QWidget *(QWidget *parent)>;

    QString id;
    QString header;
    QString badge;
    Factory factory;
    std::function<bool()> visibleWhen;
    bool isSub = false;
    bool isAdvanced = false;
    bool requiresConnection = false;
    bool allowsPartialParameters = false;
};

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
    bool addPage(const BackstagePage &page);

    QString currentPageId() const;
    QStringList pageIds() const;
    QWidget *page(const QString &id) const;
    BackstagePage pageDefinition(const QString &id) const;
    bool isPageCreated(const QString &id) const;
    bool isPageVisible(const QString &id) const;
    bool automaticSelectionEnabled() const;
    bool isGroupExpanded(const QString &id) const;
    bool isGroupVisible(const QString &id) const;
    static bool shouldShowParameterLoading(bool connected,
                                           bool parametersReady,
                                           bool allowsPartialParameters);

public slots:
    void setAutomaticSelectionEnabled(bool enabled);
    bool restoreInitialPage(const QString &preferredPage = QString());
    bool setCurrentPage(const QString &id);
    bool setPageVisible(const QString &id, bool visible);
    bool setPagePresentation(const QString &id, const QString &header,
                             const QString &badge = QString());
    bool setGroupExpanded(const QString &id, bool expanded);
    bool setGroupVisible(const QString &id, bool visible);
    bool resetPage(const QString &id);
    void refreshVisibility();
    void setParameterLoadingState(bool visible, int received, int reported,
                                  bool loadingCancelled = false,
                                  const QString &failure = QString());
    void setParameterLoadingRequesting();
    void setParameterLoadingStopping();

signals:
    void currentPageChanged(const QString &id);
    void pageActivated(const QString &id, QWidget *page);
    void pageDeactivated(const QString &id, QWidget *page);
    void stopLoadingRequested();
    void retryLoadingRequested();

private:
    struct PageEntry
    {
        QAbstractButton *button = nullptr;
        QWidget *page = nullptr;
        BackstagePage definition;
        QString groupId;
        bool requestedVisible = true;
    };

    struct GroupEntry
    {
        QAbstractButton *button = nullptr;
        QStringList pageIds;
        QString title;
        bool expanded = true;
    };

    QString firstVisiblePageId() const;
    bool addPageEntry(const BackstagePage &definition, QWidget *page);
    QWidget *ensurePageCreated(const QString &id);
    void updateGroupButtonPresentation(GroupEntry &group);
    void updatePageButtonVisibility(const QString &id);
    void selectFallbackPage();

    QVBoxLayout *m_navigationLayout = nullptr;
    QButtonGroup *m_pageGroup = nullptr;
    QStackedWidget *m_pageStack = nullptr;
    QWidget *m_loadingOverlay = nullptr;
    ConfigParamLoadingView *m_parameterLoadingView = nullptr;
    QHash<QString, PageEntry> m_pages;
    QHash<QString, GroupEntry> m_groups;
    QStringList m_pageOrder;
    QString m_currentGroupId;
    QString m_currentPageId;
    bool m_automaticSelectionEnabled = true;
};

#endif
