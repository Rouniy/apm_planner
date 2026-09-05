#ifndef PARAMETERMETADATAREGENERATIONWINDOW_H
#define PARAMETERMETADATAREGENERATIONWINDOW_H

#include "core/parameters/ParameterMetaDataRegenerationService.h"

#include <QPointer>
#include <QWidget>

#include <functional>

class QLabel;
class QCloseEvent;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;

/**
 * Modeless observer for the application-owned parameter metadata generator.
 *
 * Closing this window only detaches the presentation.  The service owns the
 * run, and only the explicit Cancel button may cancel the token currently
 * observed by this window.
 */
class ParameterMetaDataRegenerationWindow final : public QWidget
{
    Q_OBJECT

public:
    using ConfirmationCallback = std::function<bool(
        QWidget *owner, const QString &title, const QString &message)>;

    explicit ParameterMetaDataRegenerationWindow(
        ParameterMetaDataRegenerationService *service,
        QWidget *owner = nullptr,
        ConfirmationCallback confirm = ConfirmationCallback());
    ~ParameterMetaDataRegenerationWindow() override;

    ParameterMetaDataRegenerationService *service() const;
    ParameterMetaDataRegenerationService::RunToken observedToken() const
    {
        return m_observedToken;
    }

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    static constexpr int MaximumVisibleLogLines = 500;

    void requestStart();
    void requestCancel();
    void syncFromSnapshot();
    void adoptToken(
        const ParameterMetaDataRegenerationService::RunToken &token,
        bool clearPresentation);
    void showProgress(
        ParameterMetaDataRegenerationService::Phase phase,
        int completed, int total, const QString &label);
    void rebuildArtifacts(
        const QList<ParameterMetaDataRegenerationService::ArtifactReport>
            &artifacts);
    void updateArtifact(
        const ParameterMetaDataRegenerationService::ArtifactReport &artifact);
    void replaceLog(const QStringList &lines);
    void appendLogBounded(const QString &line);
    void showResult(
        const ParameterMetaDataRegenerationService::Result &result);
    void syncButtons();

    static bool defaultConfirmation(
        QWidget *owner, const QString &title, const QString &message);
    static QString phaseText(
        ParameterMetaDataRegenerationService::Phase phase);
    static QString artifactKey(
        const ParameterMetaDataRegenerationService::ArtifactReport &artifact);

    QPointer<ParameterMetaDataRegenerationService> m_service;
    ParameterMetaDataRegenerationService::RunToken m_observedToken;
    ConfirmationCallback m_confirm;
    QProgressBar *m_progress = nullptr;
    QLabel *m_phase = nullptr;
    QTableWidget *m_artifacts = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QLabel *m_result = nullptr;
    QPushButton *m_regenerate = nullptr;
    QPushButton *m_cancel = nullptr;
    QPushButton *m_close = nullptr;
};

#endif // PARAMETERMETADATAREGENERATIONWINDOW_H
