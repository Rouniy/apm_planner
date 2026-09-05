#include "ConfigFFTWindow.h"

#include "configuration/ConfigFFTView.h"
#include "configuration/ConfigFFTViewModel.h"

#include <QCloseEvent>
#include <QVBoxLayout>

ConfigFFTWindow::ConfigFFTWindow(
    ConfigFFTViewModel *viewModel, QWidget *owner)
    : QWidget(owner, Qt::Window)
{
    setObjectName(QStringLiteral("ConfigFFTWindow"));
    setWindowTitle(tr("FFT Log Analysis"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(1000, 700);
    if (owner) {
        move(owner->frameGeometry().center() - rect().center());
    }
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_view = new ConfigFFTView(viewModel, this);
    layout->addWidget(m_view);
}

ConfigFFTWindow::~ConfigFFTWindow()
{
    if (m_view) {
        disconnect(m_view.data(), nullptr, this, nullptr);
        if (m_view->viewModel()) {
            m_view->viewModel()->shutdown();
        }
    }
}

ConfigFFTViewModel *ConfigFFTWindow::viewModel() const
{
    return m_view ? m_view->viewModel() : nullptr;
}

ConfigFFTView *ConfigFFTWindow::view() const
{
    return m_view.data();
}

void ConfigFFTWindow::closeEvent(QCloseEvent *event)
{
    if (viewModel()) {
        viewModel()->shutdown();
    }
    QWidget::closeEvent(event);
}
