#ifndef MISSIONPLANNERTOOLSMENU_H
#define MISSIONPLANNERTOOLSMENU_H

#include <QHash>
#include <QList>
#include <QString>

#include <functional>

class QMenu;
class QObject;

struct MissionPlannerToolDefinition
{
    QString objectName;
    QString title;
    QString shortcut;
    bool separatorBefore = false;
};

/** Builds the MP10 top-level TOOLS inventory without legacy dock toggles. */
class MissionPlannerToolsMenu final
{
public:
    using Handler = std::function<void()>;
    using HandlerMap = QHash<QString, Handler>;

    static QList<MissionPlannerToolDefinition> Inventory();
    static void Populate(QMenu *menu, QObject *context,
                         const HandlerMap &handlers);
    static QString UnavailableReason(const QString &title);

private:
    MissionPlannerToolsMenu() = delete;
};

#endif // MISSIONPLANNERTOOLSMENU_H
