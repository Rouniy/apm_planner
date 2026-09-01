#include "dataselectionscreen.h"
#include <QCheckBox>
#include <QVBoxLayout>
#include <logging.h>
#include <GraphTreeWidgetItem.h>

QTreeWidgetItem *DataSelectionScreen::findOrCreateGroup(const QString &groupName)
{
    const auto existing = m_groupItems.constFind(groupName);
    if (existing != m_groupItems.constEnd()) {
        return existing.value();
    }

    auto *group = new GraphTreeWidgetItem(QStringList() << groupName);
    int insertionIndex = 0;
    while (insertionIndex < ui.treeWidget->topLevelItemCount()
           && QString::localeAwareCompare(
                  ui.treeWidget->topLevelItem(insertionIndex)->text(0),
                  groupName) < 0) {
        ++insertionIndex;
    }
    ui.treeWidget->insertTopLevelItem(insertionIndex, group);
    m_groupItems.insert(groupName, group);
    return group;
}

DataSelectionScreen::DataSelectionScreen(QWidget *parent) : QWidget(parent)
{
	ui.setupUi(this);
    connect(ui.treeWidget, SIGNAL(itemChanged(QTreeWidgetItem*,int)), this, SLOT(onItemChanged(QTreeWidgetItem*,int)));
    connect(ui.clearPushButton,SIGNAL(clicked()),this,SLOT(clearSelectionButtonClicked()));
    ui.treeWidget->setHeaderHidden(true);
}

DataSelectionScreen::~DataSelectionScreen()
{
}

void DataSelectionScreen::clearSelectionButtonClicked()
{
    QList<QTreeWidgetItem*> items = ui.treeWidget->findItems("",Qt::MatchContains | Qt::MatchRecursive);
    for (auto &item : items)
    {
        if (item->checkState(0) == Qt::Checked)
        {
            item->setCheckState(0,Qt::Unchecked);
            // ^^ this will trigger the disabling of the graph automatically
        }
    }
    m_enabledList.clear();
}

void DataSelectionScreen::enableItem(const QString &name)
{
    handleItem(name, Qt::Checked);
}

void DataSelectionScreen::disableItem(const QString &name)
{
    handleItem(name, Qt::Unchecked);
}

QList<QString> DataSelectionScreen::disableAllItems()
{
    // Store enabled List for returning
    QList<QString> tempList(m_enabledList);
    // Clear all selected items - its like perssing the clear button
    clearSelectionButtonClicked();
    return tempList;
}

void DataSelectionScreen::enableItemList(QList<QString> &itemList)
{
    for(const auto &name : itemList)
    {
        handleItem(name, Qt::Checked);
    }
}

void DataSelectionScreen::addItem(QString name)
{
    if (name.contains(":"))
    {
        QString sysid = name.mid(0,name.indexOf(":"));
        name = name.mid(name.indexOf(":")+1);
        m_nameToSysId[name] = sysid;
    }
    if (name.contains("."))
    {
        //Splitting name a every '.'
        QStringList parts = name.split('.');
        QString groupname;
        QString shortname;

        if(parts.size() == 2)
        {
            // name format is like "ATTITUDE.Pitch" -> groupname = ATTITUDE, shortname = Pitch
            groupname = parts[0];
            shortname = parts[1];
        }
        else if(parts.size() == 3)
        {
            // name format is like "BATTERY_STATUS.voltages.0" -> groupname = BATTERY_STATUS, shortname = voltages.0
            groupname = parts[0];
            shortname = parts[1] + "." + parts[2];
        }
        else
        {
            QLOG_INFO() << "Name schema mismatch dropping item.";
            return;
        }

        QTreeWidgetItem *group = findOrCreateGroup(groupname);
        auto *child = new GraphTreeWidgetItem(QStringList() << shortname);
        child->setFlags(child->flags() | Qt::ItemIsUserCheckable);
        child->setCheckState(0,Qt::Unchecked);
        group->addChild(child);
    }
}

void DataSelectionScreen::addItems(const QMap<QString, QStringList> &fmtMap)
{
    for (auto iter = fmtMap.constBegin(); iter != fmtMap.constEnd(); ++iter)
    {
        QString tempName {iter.key()};
        QStringList parts {tempName.split('.')};
        QTreeWidgetItem *pParentItem {nullptr};

        if(parts.size() > 2 || parts.size() == 0)
        {
            QLOG_INFO() << "Name schema mismatch dropping item.";
            return;
        }

        pParentItem = findOrCreateGroup(parts[0]);

        if(parts.size() == 2)
        {
            // we have an index so add another level to the tree
            GraphTreeWidgetItem *indexItem = new GraphTreeWidgetItem(QStringList() << parts[1]);    // index
            pParentItem->addChild(indexItem);
            // now the index item is the parent of all following items
            pParentItem = indexItem;
        }

        // now add all the leaves
        for (const auto &shortName : iter.value())
        {
            GraphTreeWidgetItem *pShortName = new GraphTreeWidgetItem(QStringList() << shortName);
            pShortName->setFlags(pShortName->flags() | Qt::ItemIsUserCheckable);
            pShortName->setCheckState(0,Qt::Unchecked);
            pParentItem->addChild(pShortName);
        }
    }
}

void DataSelectionScreen::onItemChanged(QTreeWidgetItem* item, int column)
{
    // Can be 2 level (groupname and shortname) or 3 level (groupname, index, shortname)
    Q_UNUSED(column)
    if (!item->parent())
    {
        // this is groupname - nothing to do
        return;
    }
    // construct the name -> groupname.index.shortname or groupname.shortname
    QString name;
    auto level1 = item->parent();
    if(level1->parent())
    {
        // here we have 3 level so we take 1st part (groupname)
        name.append(level1->parent()->text(0));
        name.append('.');
    }

    name.append(level1->text(0));   // this is index (3 level) or groupname (2 level)
    name.append('.');
    name.append(item->text(0));     // this is always shortname

    if (item->checkState(0) == Qt::Checked)
    {
        if (!m_enabledList.contains(name))
        {
            m_enabledList.append(name);
            emit itemEnabled(name);
        }
    }
    else
    {
        if (m_enabledList.contains(name))
        {
            m_enabledList.removeOne(name);
            emit itemDisabled(name);
        }
    }
}

void DataSelectionScreen::clear()
{
    ui.treeWidget->clear();
    m_groupItems.clear();
    m_nameToSysId.clear();
    m_enabledList.clear();
}

void DataSelectionScreen::handleItem(const QString &name, Qt::CheckState checkState)
{
    // we expect "groupName.indexName:idx.valueName" or "groupName.valueName" in name
    QStringList parts {name.split('.')};

    // use reference or pointer to avoid copying
    QString &groupName {parts.first()};
    QString &valueName {parts.last()};
    QString *pindexName {nullptr};

    // we only have an index if the name has 3 parts
    if (parts.size() == 3)
    {
        pindexName = &parts[1];
    }

    QList<QTreeWidgetItem*> treeItems = ui.treeWidget->findItems(valueName, Qt::MatchExactly | Qt::MatchRecursive, 0);
    if (treeItems.size() == 0)
    {
        return; // haven't found anything
    }

    // iterate result
    for (auto &item : treeItems )
    {
        QTreeWidgetItem *pItem {item->parent()};
        if (pItem)  // do we have a parent
        {
            if ((pindexName != nullptr) && (pItem->text(0) == *pindexName)) // if we have an index name...
            {
                pItem = pItem->parent(); // ... we use it to find the group which is one level up
            }

            if (pItem->text(0) == groupName)  // if parent matches group we found it
            {
                if (item->checkState(0) != checkState)
                {
                    item->setCheckState(0, checkState); // enable / disable it
                    ui.treeWidget->scrollToItem(item);
                }
                return;
            }
        }
    }
    QLOG_ERROR() << "No item found in DataSelectionScreen::handelItem:" << name;
}
