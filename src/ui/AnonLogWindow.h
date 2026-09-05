#ifndef ANONLOGWINDOW_H
#define ANONLOGWINDOW_H

#include "Loghandling/LogAnonymizeService.h"

#include <QPointer>
#include <QWidget>

#include <functional>

class QCloseEvent;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

/**
 * Modeless observer/editor for the application-owned log anonymizer.
 *
 * Closing the window only detaches its presentation.  The explicit Cancel
 * button is the sole UI path which cancels the exact service token observed by
 * this window.
 */
class AnonLogWindow final : public QWidget
{
    Q_OBJECT

public:
    using ConfirmationCallback = std::function<bool(
        QWidget *owner, const QString &title, const QString &message)>;

    explicit AnonLogWindow(
        LogAnonymizeService *service,
        QWidget *owner = nullptr,
        ConfirmationCallback confirm = ConfirmationCallback());
    ~AnonLogWindow() override;

    LogAnonymizeService *service() const { return m_service.data(); }
    quint64 observedToken() const { return m_observedToken; }

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    struct Request
    {
        QString inputPath;
        QString outputPath;
        QString latitudeText;
        QString longitudeText;
        LogAnonymizeOptions options;
    };

    void chooseInput();
    void chooseOutput();
    void requestStart();
    void requestCancel();
    void syncFromService();
    void syncButtons();
    void adoptCurrentJob(quint64 token);
    bool collectRequest(Request *request, QString *error) const;
    bool fieldsStillMatch(const Request &request) const;
    void showLocalError(const QString &error);

    static bool defaultConfirmation(
        QWidget *owner, const QString &title, const QString &message);
    static QString fileFilter();
    static QString suggestedOutputPath(const QString &inputPath);
    static QString formatBytes(qint64 bytes);
    static QString offsetText(double value);

    QPointer<LogAnonymizeService> m_service;
    ConfirmationCallback m_confirm;
    quint64 m_observedToken = 0;
    QString m_localStatus;

    QLineEdit *m_input = nullptr;
    QLineEdit *m_output = nullptr;
    QLineEdit *m_latitude = nullptr;
    QLineEdit *m_longitude = nullptr;
    QPushButton *m_browseInput = nullptr;
    QPushButton *m_browseOutput = nullptr;
    QLabel *m_status = nullptr;
    QProgressBar *m_progress = nullptr;
    QPlainTextEdit *m_result = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_cancel = nullptr;
    QPushButton *m_close = nullptr;
};

#endif // ANONLOGWINDOW_H
