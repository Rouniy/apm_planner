#ifndef OSDVIDEOOVERLAYWINDOW_H
#define OSDVIDEOOVERLAYWINDOW_H

#include "tools/OsdVideoOverlayCore.h"

#include <QImage>
#include <QPointer>
#include <QWidget>

#include <atomic>
#include <functional>
#include <memory>

class HudControl;
class MjpegAviWriter;
class OsdVideoDecoder;
class QCheckBox;
class QCloseEvent;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QResizeEvent;
class QSpinBox;

/** Mission Planner 10 Tools OSD-video export workflow. */
class OsdVideoOverlayWindow final : public QWidget
{
    Q_OBJECT

public:
    struct Dependencies {
        std::function<bool(QWidget *, const QString &, const QString &,
                           const QString &)> confirm;
        std::function<QString(QWidget *)> chooseVideo;
        std::function<QString(QWidget *)> chooseTlog;
        std::function<QString(QWidget *, const QString &)> chooseOutput;
    };

    explicit OsdVideoOverlayWindow(QWidget *owner = nullptr);
    OsdVideoOverlayWindow(Dependencies dependencies,
                          QWidget *owner = nullptr);
    ~OsdVideoOverlayWindow() override;

    /** Activates the one MP10 modeless instance or creates it. */
    static OsdVideoOverlayWindow *OpenWindow(QWidget *owner = nullptr);

    bool isBusy() const { return m_busy; }
    int writtenFrames() const;
    QString statusText() const;
    void setVideoPath(const QString &path);
    void setTlogPath(const QString &path);
    void setOutputPath(const QString &path);

signals:
    void exportStarted();
    void exportFinished(bool success, const QString &message);

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void browseVideo();
    void browseTlog();
    void browseOutput();
    void startExport();
    void cancelExport();
    void decoderReady(int framesPerSecond, qint64 durationMs);
    void consumeNextFrame();
    void decoderEnded();

private:
    static Dependencies DefaultDependencies();
    void buildUi(QWidget *owner);
    void startTimelineLoad(const OsdVideoExportOptions &options);
    void timelineLoaded(quint64 generation,
                        OsdVideoTelemetryTimeline timeline,
                        OsdVideoTelemetryTimeline::LoadStatus loadStatus,
                        const QString &errorMessage);
    bool prepareWriter(const QSize &sourceSize);
    QByteArray renderFrame(const QImage &source,
                           const OsdVideoTelemetrySample &sample,
                           QImage *preview);
    void applyTelemetry(const OsdVideoTelemetrySample &sample);
    bool appendRepeatedFrames(qint64 targetFrameCount);
    void finishExport(bool success, const QString &message);
    void setBusy(bool busy);
    void updatePreview();
    OsdVideoExportOptions currentOptions() const;

    static QPointer<OsdVideoOverlayWindow> s_current;

    Dependencies m_dependencies;
    OsdVideoDecoder *m_decoder = nullptr;
    HudControl *m_hud = nullptr;
    std::unique_ptr<MjpegAviWriter> m_writer;
    OsdVideoTelemetryTimeline m_timeline;
    std::shared_ptr<std::atomic_bool> m_cancelRequested;
    quint64 m_generation = 0;
    int m_framesPerSecond = OsdVideoOverlayCore::DefaultFramesPerSecond;
    qint64 m_durationMs = 0;
    QSize m_sourceSize;
    QSize m_outputSize;
    QByteArray m_lastJpeg;
    QImage m_lastPreview;
    bool m_busy = false;
    bool m_timelineReady = false;
    bool m_closeAfterFinish = false;

    QLineEdit *m_videoPath = nullptr;
    QLineEdit *m_tlogPath = nullptr;
    QLineEdit *m_outputPath = nullptr;
    QSpinBox *m_offset = nullptr;
    QCheckBox *m_fullResolution = nullptr;
    QLabel *m_preview = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_frameCount = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_videoBrowse = nullptr;
    QPushButton *m_tlogBrowse = nullptr;
    QPushButton *m_outputBrowse = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_cancel = nullptr;
    QPushButton *m_close = nullptr;
};

#endif // OSDVIDEOOVERLAYWINDOW_H
