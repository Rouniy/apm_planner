#ifndef SPECTROGRAMWINDOW_H
#define SPECTROGRAMWINDOW_H

#include <QImage>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <array>
#include <atomic>
#include <functional>
#include <memory>

class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QThread;

/** Mission Planner 10 TOOLS > DataFlash Spectrogram window. */
class SpectrogramWindow final : public QWidget
{
    Q_OBJECT

public:
    struct RenderResult
    {
        bool success = false;
        bool cancelled = false;
        QString error;
        QString warning;
        std::array<QImage, 3> images;
        double startSeconds = 0.0;
        double endSeconds = 0.0;
        double maximumFrequency = 0.0;
        int sampleCount = 0;
        int windowCount = 0;
    };

    using CancelRequested = std::function<bool()>;

    struct Dependencies
    {
        std::function<QString(QWidget *parent)> chooseLog;
        std::function<RenderResult(const QString &path,
                                   const QString &sensor,
                                   int minimumDb,
                                   int maximumDb,
                                   const CancelRequested &cancel)> generate;
    };

    static constexpr int WindowWidth = 1050;
    static constexpr int WindowHeight = 820;
    static constexpr int MinimumWindowWidth = 700;
    static constexpr int MinimumWindowHeight = 500;

    explicit SpectrogramWindow(QWidget *owner = nullptr);
    SpectrogramWindow(Dependencies dependencies, QWidget *owner = nullptr);
    ~SpectrogramWindow() override;

    /** Creates a new independent modeless top-level window on every call. */
    static SpectrogramWindow *OpenWindow(QWidget *owner = nullptr);

    /** Selects an existing DataFlash log. File-picker selection renders at once. */
    void setLogPath(const QString &path, bool renderImmediately = false);
    QString logPath() const { return m_logPath; }
    QString statusText() const;
    bool isBusy() const { return m_busy; }

private:
    void buildUi(QWidget *owner);
    void pickLog();
    void beginRender();
    void finishRender(const RenderResult &result,
                      const QString &fileName,
                      const QString &sensor);
    void clearImages();
    void cancelRender();
    void stopWorker();
    void setBusy(bool busy);

    QString m_logPath;
    Dependencies m_dependencies;
    bool m_busy = false;
    std::shared_ptr<std::atomic_bool> m_cancelFlag;
    QPointer<QThread> m_thread;
    QPushButton *m_openButton = nullptr;
    QComboBox *m_sensorCombo = nullptr;
    QSpinBox *m_minimumDbSpin = nullptr;
    QSpinBox *m_maximumDbSpin = nullptr;
    QPushButton *m_redrawButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QLabel *m_status = nullptr;
    std::array<QLabel *, 3> m_imageLabels{{nullptr, nullptr, nullptr}};
};

#endif // SPECTROGRAMWINDOW_H
