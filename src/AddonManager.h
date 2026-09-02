#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QMetaType>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QUrl>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class QLockFile;
class QNetworkReply;
class QTemporaryFile;

enum class AddonStatus
{
    NotInstalled,
    Detected,
    Installed,
    UpdateAvailable
};

struct AddonInfo
{
    int id = 0;
    QString name;
    QString version;
    QString description;
    qint64 fileSizeBytes = 0;
    QString updatedAtRaw;
    QDateTime updatedAt;
    AddonStatus status = AddonStatus::NotInstalled;
    QStringList managedFolders;
};

Q_DECLARE_METATYPE(AddonInfo)

class AddonManager final : public QObject
{
    Q_OBJECT

public:
    explicit AddonManager(QObject *parent = nullptr);

    void setAuthToken(const QString &token);
    void setGameDirectory(const QString &gameDirectory);

    void refreshCatalog();
    void installAddons(const QVector<int> &ids);
    void removeAddon(int id);

    bool isBusy() const;

signals:
    void catalogReady(const QVector<AddonInfo> &addons);
    void busyChanged(bool busy);
    void operationProgress(int percent, const QString &text);
    void operationFinished(const QString &message);
    void operationFailed(const QString &message);
    void authenticationExpired();

private:
    struct PendingDownload
    {
        int id = 0;
        QString filename;
        QUrl url;
    };

    QNetworkRequest authenticatedRequest(const QUrl &url) const;
    void fail(const QString &message);
    void setBusy(bool busy);
    bool acquireInstallLock(QString *error);
    void releaseInstallLock();

    QString addonsDirectory() const;
    QString stateFilePath() const;
    QJsonObject loadState() const;
    bool saveState(const QJsonObject &state, QString *error) const;
    QStringList managedFolders(const QJsonObject &state, int addonId) const;
    bool stateIsComplete(const QJsonObject &state, int addonId) const;
    bool folderIsShared(const QJsonObject &state, int addonId, const QString &folder) const;
    bool detectedByName(const QString &name) const;
    AddonStatus determineStatus(const AddonInfo &addon, const QJsonObject &state) const;

    void requestDownloadUrls(const QVector<int> &ids);
    void startNextDownload();
    bool installArchive(const AddonInfo &addon,
                        const QString &archivePath,
                        QStringList *installedFolders,
                        QString *error);
    bool removeManagedAddon(const AddonInfo &addon, QString *error);

    static bool safeHttpsUrl(const QUrl &url);
    static bool safeFolderName(const QString &name);
    static bool removePath(const QString &path);
    static QString apiMessage(const QByteArray &body);

    QNetworkAccessManager m_network;
    QString m_authToken;
    QString m_gameDirectory;
    QVector<AddonInfo> m_catalog;
    QVector<PendingDownload> m_pendingDownloads;
    int m_downloadIndex = 0;
    bool m_busy = false;

    QNetworkReply *m_downloadReply = nullptr;
    QTemporaryFile *m_tempArchive = nullptr;
    QLockFile *m_installLock = nullptr;
};
