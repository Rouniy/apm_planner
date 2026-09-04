#ifndef EXTERNALGUIDEDWINDOW_H
#define EXTERNALGUIDEDWINDOW_H

#include "comm/GuidedTargetService.h"
#include "tools/ExternalGuidedFile.h"

#include <QPointer>
#include <QWidget>

#include <functional>

class QCloseEvent;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QThread;
class QTimer;
class VehicleTargetManager;

/** Mission Planner 10 TOOLS > External Guided modeless window. */
class ExternalGuidedWindow final : public QWidget
{
    Q_OBJECT

public:
    using FileReader =
        std::function<ExternalGuidedFileResult(const QString &path)>;
    using FileChooser = std::function<QString(QWidget *parent)>;
    using DangerousConfirmation = std::function<bool(
        QWidget *parent, const QString &title, const QString &message)>;

    struct Dependencies
    {
        VehicleTargetManager *targetManager = nullptr;
        GuidedTargetService *guidedService = nullptr;
        FileReader readFile;
        FileChooser chooseFile;
        DangerousConfirmation confirmStart;
        int updateIntervalMs = 1000;
    };

    static constexpr int WindowWidth = 620;
    static constexpr int WindowHeight = 390;
    static constexpr int MinimumWindowWidth = 560;
    static constexpr int MinimumWindowHeight = 350;
    static constexpr int MaximumPathCharacters = 32768;

    explicit ExternalGuidedWindow(QWidget *owner = nullptr);
    ExternalGuidedWindow(Dependencies dependencies,
                         QWidget *owner = nullptr);
    ~ExternalGuidedWindow() override;

    /** Creates a fresh independent modeless window on every call. */
    static ExternalGuidedWindow *OpenWindow(QWidget *owner = nullptr);

    QString filePath() const;
    QString statusText() const;
    QString lastAcceptedText() const;
    bool isStarting() const noexcept { return m_starting; }
    bool isRunning() const noexcept { return m_session.isValid(); }

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    enum class ReadPurpose {
        InitialValidation,
        PostConfirmation,
        PeriodicUpdate
    };

    static Dependencies DefaultDependencies();
    void buildUi(QWidget *owner);
    void connectDependencies();
    void syncUi();
    void refreshTargetDescription();
    void toggleSession();
    void beginStart();
    void beginRead(ReadPurpose purpose, const QString &path);
    void finishRead(ReadPurpose purpose,
                    const ExternalGuidedFileResult &result,
                    quint64 generation);
    void handleInitialRead(const ExternalGuidedFileResult &result);
    void handlePostConfirmationRead(
        const ExternalGuidedFileResult &result);
    void handlePeriodicRead(const ExternalGuidedFileResult &result);
    void scheduleNextRead();
    void requestStop(const QString &status);
    void finishLocalSession(const QString &status);
    void abortStart(const QString &status);
    void cancelRead();
    void shutdown();
    bool sessionMatches(
        const GuidedTargetService::SessionToken &session) const;
    QString targetDescription(const VehicleTargetLease &target) const;
    QString startFailureText(GuidedTargetService::RequestResult result) const;
    static GuidedTargetService::Target guidedTarget(
        const ExternalGuidedWaypoint &waypoint);
    static QString waypointText(const GuidedTargetService::Target &target);

    QPointer<VehicleTargetManager> m_targetManager;
    QPointer<GuidedTargetService> m_guidedService;
    FileReader m_readFile;
    FileChooser m_chooseFile;
    DangerousConfirmation m_confirmStart;
    int m_updateIntervalMs = 1000;
    quint64 m_operationGeneration = 0;
    bool m_starting = false;
    bool m_stopping = false;
    bool m_closing = false;
    QString m_activePath;
    VehicleTargetLease m_pendingLease;
    GuidedTargetService::SessionToken m_session;
    QPointer<QThread> m_readThread;
    QTimer *m_updateTimer = nullptr;
    QLineEdit *m_filePath = nullptr;
    QPushButton *m_browse = nullptr;
    QPushButton *m_toggle = nullptr;
    QLabel *m_target = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_lastAccepted = nullptr;
};

#endif // EXTERNALGUIDEDWINDOW_H
