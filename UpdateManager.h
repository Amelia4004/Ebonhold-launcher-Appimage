#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QVector>

class QCryptographicHash;
class QNetworkReply;
class QSaveFile;

struct PatchFile
{
    int id = 0;
    QString relativePath;
    QByteArray expectedMd5;
};

struct PatchScanResult
{
    PatchFile file;
    bool exists = false;
    bool matches = false;
    QString error;
};

class UpdateManager final : public QObject
{
    Q_OBJECT

public:
    explicit UpdateManager(QObject *parent = nullptr);

    void setAuthToken(const QString &token);

    void scan(const QString &gameDirectory,
              const QJsonObject &manifest,
              const QString &gameSlug = QStringLiteral("roguelike-prod"));

    void installUpdates(const QString &gameDirectory,
                        const QVector<PatchFile> &files,
                        const QString &realmlist);

signals:
    void scanStarted(int fileCount);
    void scanFinished(const QVector<PatchScanResult> &results, const QString &realmlist);
    void scanFailed(const QString &message);

    void updateStarted(int fileCount);
    void updateFileStarted(int index, int total, const QString &relativePath);
    void updateProgress(int percent, const QString &relativePath);
    void updateFileFinished(int index, int total, const QString &relativePath);
    void updateFinished();
    void updateFailed(const QString &message);
    void authenticationExpired();

private:
    static QVector<PatchFile> collectRequiredFiles(const QJsonObject &manifest,
                                                   const QString &gameSlug,
                                                   QString *realmlist,
                                                   QString *error);
    static QVector<PatchScanResult> scanFiles(const QString &gameDirectory,
                                              const QVector<PatchFile> &files);

    void downloadNext();
    void requestDownloadUrl(const PatchFile &file);
    void startFileDownload(const PatchFile &file, const QUrl &url);
    void finishUpdate();
    void failUpdate(const QString &message);
    bool writeRealmlist(QString *error) const;
    QNetworkRequest authenticatedRequest(const QUrl &url) const;
    static QString apiMessage(const QByteArray &body);
    static bool safeHttpsUrl(const QUrl &url);

    QNetworkAccessManager m_network;
    QString m_authToken;

    QString m_gameDirectory;
    QString m_realmlist;
    QVector<PatchFile> m_updateFiles;
    int m_updateIndex = 0;
    bool m_updating = false;

    QNetworkReply *m_downloadReply = nullptr;
    QSaveFile *m_outputFile = nullptr;
    QCryptographicHash *m_downloadHash = nullptr;
};
