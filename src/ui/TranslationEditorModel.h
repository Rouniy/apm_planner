#ifndef TRANSLATIONEDITORMODEL_H
#define TRANSLATIONEDITORMODEL_H

#include "services/ResxTranslationService.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QVector>

/** Editable, full-project data model for the MP10 Translation / RESX Editor. */
class TranslationEditorModel final : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column {
        FileColumn = 0,
        InternalKeyColumn,
        SourceTextColumn,
        TranslationColumn,
        ExistingColumn,
        ColumnCount
    };
    Q_ENUM(Column)

    enum DataRole {
        RelativePathRole = Qt::UserRole + 1,
        KeyRole,
        SourceTextRole,
        TranslationRole,
        CommentRole,
        ExistingRole,
        MissingRole,
        WillExportRole,
        ModifiedRole
    };
    Q_ENUM(DataRole)

    explicit TranslationEditorModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index,
                  int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool setData(const QModelIndex &index, const QVariant &value,
                 int role = Qt::EditRole) override;
    QHash<int, QByteArray> roleNames() const override;

    void loadProject(const ResxTranslationProject &project);
    void clearProject();
    bool hasProject() const noexcept { return m_hasProject; }
    QString sourceRoot() const { return m_sourceRoot; }
    QString culture() const { return m_culture; }
    QStringList warnings() const { return m_warnings; }
    int resourceFileCount() const noexcept { return m_resourceFileCount; }
    int totalCount() const noexcept { return m_rows.size(); }
    int missingCount() const noexcept { return m_missingCount; }
    int translatedCount() const noexcept { return m_translatedCount; }
    int modifiedCount() const noexcept { return m_modifiedCount; }
    bool hasUnsavedChanges() const noexcept { return m_modifiedCount != 0; }

    ResxTranslationEntry entryAt(int row) const;
    QVector<ResxTranslationEntry> snapshot() const;
    bool acceptChanges();
    bool revertAll();
    int importTranslations(const ResxTranslationService::ImportResult &result);
    int importTranslations(
        const QHash<TranslationIdentity, QString> &values,
        const QVector<TranslationIdentity> &order = {});

signals:
    void projectChanged();
    void countsChanged();

private:
    struct Row {
        ResxTranslationEntry entry;
        QString acceptedTranslation;
    };
    struct RowState {
        bool missing = false;
        bool willExport = false;
        bool modified = false;
    };

    static RowState state(const Row &row);
    static QString normalizedPath(const QString &path);
    static QString foldedPath(const QString &path);
    static QString baseName(const QString &path);
    static QString resourceName(const QString &sourceRelativePath);
    void rebuildAliasIndex();
    void addAlias(const QString &key, const QString &alias, int row);
    int uniqueImportRow(const TranslationIdentity &identity) const;
    bool setTranslation(int row, const QString &translation,
                        bool emitChange);
    void recount();

    QVector<Row> m_rows;
    // Exact resource keys remain case-sensitive. Each folded path alias keeps
    // at most two rows because import only needs to distinguish unique from
    // ambiguous without an O(imports * rows) scan.
    QHash<QString, QHash<QString, QVector<int>>> m_aliasesByKey;
    QString m_sourceRoot;
    QString m_culture;
    QStringList m_warnings;
    int m_resourceFileCount = 0;
    int m_missingCount = 0;
    int m_translatedCount = 0;
    int m_modifiedCount = 0;
    bool m_hasProject = false;
};

/** Search / Missing / Will-export projection; the source model remains full. */
class TranslationEditorFilterModel final : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit TranslationEditorFilterModel(QObject *parent = nullptr);

    QString searchText() const { return m_searchText; }
    bool missingOnly() const noexcept { return m_missingOnly; }
    bool exportOnly() const noexcept { return m_exportOnly; }
    int visibleCount() const { return rowCount(); }

public slots:
    void setSearchText(const QString &text);
    void setMissingOnly(bool enabled);
    void setExportOnly(bool enabled);

signals:
    void filterChanged();
    void visibleCountChanged(int count);

protected:
    bool filterAcceptsRow(int sourceRow,
                          const QModelIndex &sourceParent) const override;

private:
    void refreshFilter();

    QString m_searchText;
    bool m_missingOnly = false;
    bool m_exportOnly = false;
};

#endif // TRANSLATIONEDITORMODEL_H
