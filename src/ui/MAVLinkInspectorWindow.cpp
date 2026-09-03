#include "MAVLinkInspectorWindow.h"

#include <QVBoxLayout>
#include <QWidget>

MAVLinkInspectorWindow::MAVLinkInspectorWindow(QWidget *inspectorView,
                                               QWidget *owner)
    : QWidget(owner, Qt::Window)
    , m_inspectorView(inspectorView)
{
    setObjectName(QStringLiteral("MAVLinkInspectorWindow"));
    setWindowTitle(tr("Mavlink Inspector"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(640, 520);

    if (owner) {
        move(owner->frameGeometry().center() - rect().center());
    }

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    if (m_inspectorView) {
        layout->addWidget(m_inspectorView);
    }
}

QWidget *MAVLinkInspectorWindow::inspectorView() const
{
    return m_inspectorView;
}
