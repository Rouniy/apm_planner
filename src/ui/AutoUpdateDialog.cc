/*===================================================================
APM_PLANNER Open Source Ground Control Station

(c) 2014 APM_PLANNER PROJECT <http://www.diydrones.com>
(c) author: Bill Bonney <billbonney@communistech.com>

This file is part of the APM_PLANNER project

    APM_PLANNER is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    APM_PLANNER is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with APM_PLANNER. If not, see <http://www.gnu.org/licenses/>.

======================================================================*/
#include "logging.h"
#include "AutoUpdateDialog.h"
#include "ui_AutoUpdateDialog.h"
#include <QMessageBox>
#include <QDesktopServices>
#include <QFileInfo>
#include <QPushButton>

AutoUpdateDialog::AutoUpdateDialog(const QString &version, const QString &targetFilename,
                                   const QString &url, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::AutoUpdateDialog),
    m_sourceUrl(url),
    m_targetFilename(targetFilename),
    m_networkReply(NULL),
    m_targetFile(NULL),
    m_httpRequestAborted(false),
    m_writeFailed(false),
    m_redirectCount(0),
    m_skipVersion(false),
    m_skipVersionString(version)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);
    ui->progressBar->hide();
    ui->versionLabel->setText(version);

    connect(ui->skipPushButton, SIGNAL(clicked()), this, SLOT(skipClicked()));
    connect(ui->yesPushButton, SIGNAL(clicked()), this, SLOT(yesClicked()));
    connect(ui->noPushButton, SIGNAL(clicked()), this, SLOT(noClicked()));
}

AutoUpdateDialog::~AutoUpdateDialog()
{
    if (m_networkReply) {
        disconnect(m_networkReply, nullptr, this, nullptr);
        m_networkReply->abort();
        m_networkReply->deleteLater();
        m_networkReply = NULL;
    }
    if (m_targetFile) {
        m_targetFile->close();
        m_targetFile->remove();
        delete m_targetFile;
        m_targetFile = NULL;
    }
    delete ui;
}

void AutoUpdateDialog::noClicked()
{
    if (m_networkReply) {
        cancelDownload();
        return;
    }
    reject();
}

void AutoUpdateDialog::skipClicked()
{
    emit autoUpdateCancelled(m_skipVersionString);
}

void AutoUpdateDialog::yesClicked()
{
    startDownload(m_sourceUrl, m_targetFilename);
}

bool AutoUpdateDialog::skipVersion()
{
    return m_skipVersion;
}

bool AutoUpdateDialog::startDownload(const QString& url, const QString& filename)
{
    QString targetDir = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);

    const QUrl sourceUrl(url);
    if (filename.isEmpty() || filename == QStringLiteral(".")
        || filename == QStringLiteral("..")
        || filename.contains(QLatin1Char('/'))
        || filename.contains(QLatin1Char('\\'))
        || QFileInfo(filename).fileName() != filename
        || !sourceUrl.isValid()
        || sourceUrl.scheme().compare(QStringLiteral("https"),
                                      Qt::CaseInsensitive) != 0
        || sourceUrl.host().isEmpty()) {
        QMessageBox::warning(this, tr("Update Download"),
                             tr("The update manifest contains an unsafe download target."));
        return false;
    }

    if (QFile::exists(targetDir + "/" + filename)) {
        int result = QMessageBox::question(this, tr("HTTP"),
                      tr("There already exists a file called %1 in "
                         "%2. Overwrite?").arg(filename, targetDir),
                      QMessageBox::Yes|QMessageBox::No, QMessageBox::No);

        if (result == QMessageBox::No){
            return false;
        }
    }
    // Always must remove file before proceeding
    QFile::remove(targetDir + "/" + filename);

    m_targetFile = new QFile(targetDir + "/" + filename);

    if (!m_targetFile->open(QIODevice::WriteOnly)) {
        QMessageBox::information(this, tr("HTTP"),
                                 tr("Unable to save the file %1: %2.")
                                 .arg(filename).arg(m_targetFile->errorString()));
        delete m_targetFile;
        m_targetFile = NULL;
        return false;
    }

    QLOG_DEBUG() << "Start Downloading new version" << url;
    m_url = sourceUrl;
    m_redirectCount = 0;
    startFileDownloadRequest(m_url);
    return true;
}

void AutoUpdateDialog::startFileDownloadRequest(QUrl url)
{
    ui->progressBar->show();
    ui->noPushButton->setText(tr("Cancel"));
    ui->yesPushButton->setEnabled(false);
    ui->skipPushButton->setEnabled(false);

    ui->titleLabel->setText(tr("<html><head/><body><p><span style=\" font-size:18pt; font-weight:600;\">Downloading</span></p></body></html>"));
    ui->questionLabel->setText(tr(""));
    ui->statusLabel->setText(tr("Downloading %1").arg(m_targetFilename));
    m_httpRequestAborted = false;
    m_writeFailed = false;
    m_writeError.clear();
    if (m_networkReply != NULL){
        disconnect(m_networkReply, nullptr, this, nullptr);
        m_networkReply->abort();
        m_networkReply->deleteLater();
        m_networkReply = NULL;
    }
    QNetworkRequest request(url);
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
#endif
    m_networkReply = m_networkAccessManager.get(request);
    connect(m_networkReply, SIGNAL(finished()), this, SLOT(httpFinished()));
    connect(m_networkReply, SIGNAL(readyRead()), this, SLOT(httpReadyRead()));
    connect(m_networkReply, SIGNAL(downloadProgress(qint64,qint64)),
            this, SLOT(updateDataReadProgress(qint64,qint64)));
}

void AutoUpdateDialog::cancelDownload()
{
    if (!m_networkReply) {
        return;
    }
    ui->statusLabel->setText(tr("Download canceled."));
    ui->noPushButton->setText(tr("OK"));
    m_httpRequestAborted = true;
    QNetworkReply *reply = m_networkReply;
    m_networkReply = NULL;
    disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
    if (m_targetFile) {
        m_targetFile->close();
        m_targetFile->remove();
        delete m_targetFile;
        m_targetFile = NULL;
    }

}

void AutoUpdateDialog::httpFinished()
{
     auto *reply = qobject_cast<QNetworkReply *>(sender());
     if (!reply || reply != m_networkReply || !m_targetFile) {
        return;
     }

     bool result = false;
     const bool flushSucceeded = m_targetFile->flush();
     m_targetFile->close();

     QVariant redirectionTarget = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
     if (m_writeFailed || !flushSucceeded) {
         m_targetFile->remove();
         QMessageBox::information(
             this, tr("Update Download"),
             tr("Unable to write the update file: %1.")
                 .arg(m_writeError.isEmpty()
                          ? m_targetFile->errorString() : m_writeError));
     } else if (reply->error()) {
         m_targetFile->remove();
         QMessageBox::information(this, tr("HTTP"),
                                  tr("Download failed: %1.")
                                  .arg(reply->errorString()));

     } else if (!redirectionTarget.isNull()) {
         QUrl newUrl = m_url.resolved(redirectionTarget.toUrl());
         const bool safeRedirect = ++m_redirectCount <= 5
             && newUrl.scheme().compare(QStringLiteral("https"),
                                        Qt::CaseInsensitive) == 0
             && !newUrl.host().isEmpty();
         if (!safeRedirect) {
             m_targetFile->remove();
             QMessageBox::warning(this, tr("Update Download"),
                                  tr("The update download redirect is unsafe."));
         } else if (QMessageBox::question(
                        this, tr("HTTP"),
                        tr("Redirect to %1 ?").arg(newUrl.toString()),
                        QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
             m_url = newUrl;
             reply->deleteLater();
             m_networkReply = NULL;
             if (!m_targetFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                 QMessageBox::warning(
                     this, tr("Update Download"),
                     tr("Unable to reopen the update file: %1.")
                         .arg(m_targetFile->errorString()));
                 m_targetFile->remove();
                 ui->titleLabel->setText(tr("Download Failed"));
                 ui->statusLabel->setText(tr("ERROR: Download Failed!"));
                 ui->noPushButton->setText(tr("OK"));
                 delete m_targetFile;
                 m_targetFile = NULL;
                 return;
             }
             startFileDownloadRequest(m_url);
             return;
         } else {
             m_targetFile->remove();
         }
     } else {
         QString filename = m_targetFile->fileName();
         ui->statusLabel->setText(tr("Downloaded to %1.").arg(filename));
         result = true;
     }

     reply->deleteLater();
     m_networkReply = NULL;

     if (!result){
         ui->titleLabel->setText(tr("<html><head/><body><p><span style=\" font-size:18pt; font-weight:600;\">Download Failed</span></p></body></html>"));
         ui->statusLabel->setText(tr("ERROR: Download Failed!"));
     } else {
         ui->titleLabel->setText(tr("<html><head/><body><p><span style=\" font-size:18pt; font-weight:600;\">Download Complete</span></p></body></html>"));
         ui->questionLabel->setText(tr(""));

         executeDownloadedFile();

     }
     ui->noPushButton->setText(tr("OK"));

     this->raise();
     delete m_targetFile;
     m_targetFile = NULL;
}

void AutoUpdateDialog::executeDownloadedFile()
{
    QString url = m_targetFile->fileName().mid(0,m_targetFile->fileName().lastIndexOf("/"));
    QLOG_INFO() << "Opening folder for display" << url;
    QDesktopServices::openUrl(QUrl::fromLocalFile(url));
}

void AutoUpdateDialog::dmgMounted(int result, QProcess::ExitStatus exitStatus)
{
    QLOG_DEBUG() << "dmgMounted:" << result << "exitStatus:" << exitStatus;
    this->raise();
    ui->skipPushButton->setEnabled(false);
    ui->yesPushButton->setEnabled(false);
    if (result != 0){
        ui->statusLabel->setText(tr("ERROR:Failed to mount Disk Image!"));
        this->exec();
    }
    ui->statusLabel->setText(tr("Complete"));
    accept();
}

void AutoUpdateDialog::httpReadyRead()
{
    // this slot gets called every time the QNetworkReply has new data.
    // We read all of its new data and write it into the file.
    // That way we use less RAM than when reading it at the finished()
    // signal of the QNetworkReply
    if (m_targetFile && m_networkReply){
        const QByteArray data = m_networkReply->readAll();
        if (!data.isEmpty() && m_targetFile->write(data) != data.size()) {
            m_writeFailed = true;
            m_writeError = m_targetFile->errorString();
            m_networkReply->abort();
        }
    }
}

void AutoUpdateDialog::updateDataReadProgress(qint64 bytesRead, qint64 totalBytes)
{
    if (m_httpRequestAborted)
        return;
    if (totalBytes > 0) {
        ui->progressBar->setRange(0, totalBytes);
        ui->progressBar->setValue(bytesRead);
    } else {
        ui->progressBar->setRange(0, 0);
    }
}
