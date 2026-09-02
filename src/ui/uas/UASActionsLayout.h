#ifndef UASACTIONSLAYOUT_H
#define UASACTIONSLAYOUT_H

#include "ui_UASActionsWidget.h"

#include <QHBoxLayout>

namespace UASActionsLayout
{

inline void applyMissionPlannerProportions(Ui::UASActionsWidget &ui)
{
    // Qt 5 uic does not accept Designer's comma-separated stretch syntax.
    // Apply the MP10 1:1 grid and 2:3 editor proportions explicitly.
    for (int column = 0; column < 5; ++column) {
        ui.OfficialActionsGrid->setColumnStretch(column, 1);
    }
    for (QHBoxLayout *editor : {ui.changeSpeedEditorLayout,
                                ui.changeAltitudeEditorLayout,
                                ui.loiterRadiusEditorLayout}) {
        editor->setStretch(0, 2);
        editor->setStretch(1, 3);
    }
}

} // namespace UASActionsLayout

#endif // UASACTIONSLAYOUT_H
