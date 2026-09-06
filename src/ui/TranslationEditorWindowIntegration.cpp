#include "MainWindow.h"
#include "TranslationEditorWindow.h"
#include <QTimer>

void MainWindow::showTranslationEditor()
{
    if (aboutToCloseFlag) return;
    if (!m_translationEditorWindow) {
        auto *editor = new TranslationEditorWindow(this);
        m_translationEditorWindow = editor;
        connect(editor, &TranslationEditorWindow::closeResolved, this, [this](bool accepted) {
            if (!m_waitingForTranslationEditorClose) return;
            m_waitingForTranslationEditorClose = false;
            if (accepted) QTimer::singleShot(0, this, [this] {
                if (!aboutToCloseFlag) close();
            });
        });
    }
    m_translationEditorWindow->show();
    m_translationEditorWindow->raise();
    m_translationEditorWindow->activateWindow();
}

bool MainWindow::requestTranslationEditorClose()
{
    const QPointer<TranslationEditorWindow> editor(m_translationEditorWindow);
    if (!editor) return true;
    m_waitingForTranslationEditorClose = true;
    if (!editor->close()) return false;
    m_waitingForTranslationEditorClose = false;
    return true;
}

void MainWindow::closeTranslationEditor()
{
    // Forced teardown is reserved for destruction, not ordinary window Close:
    // the latter waits for the editor's unsaved/busy decision above.
    m_waitingForTranslationEditorClose = false;
    if (m_translationEditorWindow) {
        m_translationEditorWindow->shutdown();
        delete m_translationEditorWindow.data();
        m_translationEditorWindow.clear();
    }
}
