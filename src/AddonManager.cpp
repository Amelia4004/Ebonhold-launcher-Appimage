#include "AddonManager.h"
#include "SafeFilesystem.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSharedPointer>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QUrl>
#include <QUrlQuery>

#include <archive.h>
#include <archive_entry.h>

namespace {
const QUrl kAddonsUrl(QStringLiteral("https://api.project-ebonhold.com/api/launcher/addons"));
const QUrl kAddonDownloadUrl(QStringLiteral("https://api.project-ebonhold.com/api/launcher/addons/download"));
constexpr qint64 kMaximumAddonDownloadBytes = 512LL * 1024LL * 1024LL;
constexpr qint64 kMaximumAddonCatalogResponseBytes = 4LL * 1024LL * 1024LL;
constexpr qint64 kMaximumAddonDownloadUrlResponseBytes = 2LL * 1024LL * 1024LL;
constexpr int kApiTransferTimeoutMs = 30000;

constexpr qint64 kMaximumAddonExtractedBytes = 1024LL * 1024LL * 1024LL;
constexpr qint64 kMaximumAddonSingleFileBytes = 256LL * 1024LL * 1024LL;
constexpr int kMaximumAddonArchiveEntries = 20000;
constexpr int kMaximumAddonArchivePathLength = 1024;
constexpr int kMaximumAddonArchiveDepth = 32;

QStringList jsonFolders(const QJsonObject &state, int addonId)
{
    QStringList folders;
    const QJsonObject addons = state.value(QStringLiteral("addons")).toObject();
    const QJsonObject record = addons.value(QString::number(addonId)).toObject();
    const QJsonArray array = record.value(QStringLiteral("folders")).toArray();
    for (const QJsonValue &value : array) {
        const QString folder = value.toString();
        if (!folder.isEmpty())
            folders.push_back(folder);
    }
    return folders;
}

QStringList pathComponents(const QString &path)
{
    return path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
}

using LimitedReplyBody = QSharedPointer<QByteArray>;

LimitedReplyBody collectLimitedReply(QNetworkReply *reply, qint64 maximumBytes)
{
    auto body = LimitedReplyBody::create();
    reply->setReadBufferSize(maximumBytes + 1);

    QObject::connect(reply, &QNetworkReply::readyRead, reply,
                     [reply, body, maximumBytes]() {
        if (reply->property("ebonholdResponseTooLarge").toBool()) {
            reply->readAll();
            return;
        }

        const QByteArray chunk = reply->readAll();
        if (static_cast<qint64>(chunk.size()) >
            maximumBytes - static_cast<qint64>(body->size())) {
            reply->setProperty("ebonholdResponseTooLarge", true);
            reply->abort();
            return;
        }

        body->append(chunk);
    });

    return body;
}

bool finishLimitedReply(QNetworkReply *reply,
                        const LimitedReplyBody &body,
                        qint64 maximumBytes)
{
    if (reply->property("ebonholdResponseTooLarge").toBool())
        return false;

    const QByteArray tail = reply->readAll();
    if (static_cast<qint64>(tail.size()) >
        maximumBytes - static_cast<qint64>(body->size())) {
        reply->setProperty("ebonholdResponseTooLarge", true);
        return false;
    }

    body->append(tail);
    return true;
}

bool archivePathIsSafe(const QString &path)
{
    if (path.isEmpty() || QDir::isAbsolutePath(path) || path.startsWith(QLatin1Char('/')) ||
        path.contains(QLatin1Char('\\')))
        return false;

    for (const QChar ch : path) {
        if (ch.unicode() < 0x20)
            return false;
    }

    const QStringList components = pathComponents(path);
    if (components.isEmpty())
        return false;

    for (const QString &component : components) {
        if (component == QStringLiteral(".") || component == QStringLiteral(".."))
            return false;
    }
    return true;
}

const AddonInfo *findAddon(const QVector<AddonInfo> &catalog, int id)
{
    for (const AddonInfo &addon : catalog) {
        if (addon.id == id)
            return &addon;
    }
    return nullptr;
}
}

AddonManager::AddonManager(QObject *parent)
    : QObject(parent)
{
}

void AddonManager::setAuthToken(const QString &token)
{
    m_authToken = token;
}

void AddonManager::setGameDirectory(const QString &gameDirectory)
{
    m_gameDirectory = QDir::cleanPath(gameDirectory);
}

bool AddonManager::isBusy() const
{
    return m_busy;
}

void AddonManager::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged(busy);
}

QNetworkRequest AddonManager::authenticatedRequest(const QUrl &url) const
{
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", QByteArray("Bearer ") + m_authToken.toUtf8());
    request.setRawHeader("User-Agent", "EbonholdLauncher/1.0");
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("X-Client-Id", "EbonholdLauncher");
    request.setRawHeader("Origin", "https://project-ebonhold.com");
    request.setRawHeader("Referer", "https://project-ebonhold.com/download");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);
    request.setTransferTimeout(kApiTransferTimeoutMs);
    return request;
}

QString AddonManager::apiMessage(const QByteArray &body)
{
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isObject())
        return {};
    const QJsonObject root = document.object();
    QString message = root.value(QStringLiteral("message")).toString();
    if (message.isEmpty())
        message = root.value(QStringLiteral("error")).toString();
    return message;
}

bool AddonManager::safeHttpsUrl(const QUrl &url)
{
    return url.isValid() && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 &&
           !url.host().isEmpty();
}

bool AddonManager::safeFolderName(const QString &name)
{
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..") ||
        name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')))
        return false;

    for (const QChar ch : name) {
        if (ch.unicode() < 0x20)
            return false;
    }
    return true;
}

QString AddonManager::addonsDirectory() const
{
    return QDir(m_gameDirectory).filePath(QStringLiteral("Interface/AddOns"));
}

QString AddonManager::stateFilePath() const
{
    return QDir(addonsDirectory()).filePath(QStringLiteral(".ebonhold-launcher-addons.json"));
}

QJsonObject AddonManager::loadState() const
{
    QString path;
    if (SafeFilesystem::resolveDestination(
            m_gameDirectory,
            QStringLiteral("Interface/AddOns/.ebonhold-launcher-addons.json"),
            false,
            &path,
            nullptr)) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
            if (parseError.error == QJsonParseError::NoError && document.isObject() &&
                document.object().value(QStringLiteral("addons")).isObject()) {
                return document.object();
            }
        }
    }

    QJsonObject root;
    root.insert(QStringLiteral("addons"), QJsonObject());
    return root;
}

bool AddonManager::saveState(const QJsonObject &state, QString *error) const
{
    QString directory;
    if (!SafeFilesystem::ensureDirectory(m_gameDirectory,
                                         QStringLiteral("Interface/AddOns"),
                                         &directory,
                                         error)) {
        return false;
    }

    QString path;
    if (!SafeFilesystem::resolveDestination(
            m_gameDirectory,
            QStringLiteral("Interface/AddOns/.ebonhold-launcher-addons.json"),
            false,
            &path,
            error)) {
        return false;
    }

    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("Could not open the AddOn state file: %1")
                     .arg(file.errorString());
        return false;
    }

    const QByteArray data = QJsonDocument(state).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size()) {
        *error = QStringLiteral("Could not save the AddOn state file: %1")
                     .arg(file.errorString());
        return false;
    }

    QString verifiedPath;
    QString pathError;
    if (!SafeFilesystem::resolveDestination(
            m_gameDirectory,
            QStringLiteral("Interface/AddOns/.ebonhold-launcher-addons.json"),
            false,
            &verifiedPath,
            &pathError) ||
        QDir::cleanPath(verifiedPath) != QDir::cleanPath(path)) {
        file.cancelWriting();
        *error = QStringLiteral("The AddOn state path changed while it was being written.");
        return false;
    }

    if (!file.commit()) {
        *error = QStringLiteral("Could not save the AddOn state file: %1")
                     .arg(file.errorString());
        return false;
    }

    return true;
}

QStringList AddonManager::managedFolders(const QJsonObject &state, int addonId) const
{
    QStringList result;
    for (const QString &folder : jsonFolders(state, addonId)) {
        if (safeFolderName(folder))
            result.push_back(folder);
    }
    return result;
}

bool AddonManager::stateIsComplete(const QJsonObject &state, int addonId) const
{
    const QStringList folders = managedFolders(state, addonId);
    if (folders.isEmpty())
        return false;

    for (const QString &folder : folders) {
        const QFileInfo info(QDir(addonsDirectory()).filePath(folder));
        if (!info.isDir() || info.isSymLink())
            return false;
        QDir directory(info.absoluteFilePath());
        if (directory.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                QDir::Name).isEmpty())
            return false;
    }
    return true;
}

bool AddonManager::folderIsShared(const QJsonObject &state, int addonId, const QString &folder) const
{
    const QJsonObject addons = state.value(QStringLiteral("addons")).toObject();
    for (auto it = addons.constBegin(); it != addons.constEnd(); ++it) {
        if (it.key() == QString::number(addonId) || !it.value().isObject())
            continue;
        for (const QJsonValue &value : it.value().toObject().value(QStringLiteral("folders")).toArray()) {
            if (value.toString() == folder)
                return true;
        }
    }
    return false;
}

bool AddonManager::detectedByName(const QString &name) const
{
    QDir directory(addonsDirectory());
    if (!directory.exists())
        return false;

    const QFileInfoList entries = directory.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot,
                                                           QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (!entry.isSymLink() &&
            entry.fileName().compare(name, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

AddonStatus AddonManager::determineStatus(const AddonInfo &addon, const QJsonObject &state) const
{
    const QJsonObject addons = state.value(QStringLiteral("addons")).toObject();
    const QJsonObject local = addons.value(QString::number(addon.id)).toObject();
    if (!local.isEmpty()) {
        if (!stateIsComplete(state, addon.id))
            return AddonStatus::UpdateAvailable;

        const QString localUpdated = local.value(QStringLiteral("updated_at")).toString();
        if (localUpdated == addon.updatedAtRaw)
            return AddonStatus::Installed;

        const QDateTime localTime = QDateTime::fromString(localUpdated, Qt::ISODateWithMs);
        if (localTime.isValid() && addon.updatedAt.isValid() && addon.updatedAt <= localTime)
            return AddonStatus::Installed;

        return AddonStatus::UpdateAvailable;
    }

    return detectedByName(addon.name) ? AddonStatus::Detected : AddonStatus::NotInstalled;
}

bool AddonManager::acquireInstallLock(QString *error)
{
    QString directory;
    if (!SafeFilesystem::ensureDirectory(m_gameDirectory,
                                         QStringLiteral("Interface/AddOns"),
                                         &directory,
                                         error)) {
        return false;
    }

    QString lockPath;
    if (!SafeFilesystem::resolveDestination(
            m_gameDirectory,
            QStringLiteral("Interface/AddOns/.ebonhold-launcher.lock"),
            false,
            &lockPath,
            error)) {
        return false;
    }

    releaseInstallLock();
    m_installLock = new QLockFile(lockPath);
    m_installLock->setStaleLockTime(30000);
    if (!m_installLock->tryLock(0)) {
        delete m_installLock;
        m_installLock = nullptr;
        *error = QStringLiteral("Another Ebonhold launcher is currently modifying AddOns.");
        return false;
    }

    return true;
}

void AddonManager::releaseInstallLock()
{
    if (!m_installLock)
        return;
    m_installLock->unlock();
    delete m_installLock;
    m_installLock = nullptr;
}

void AddonManager::refreshCatalog()
{
    if (m_busy) {
        emit operationFailed(QStringLiteral("Another optional-content operation is already running."));
        return;
    }
    if (m_gameDirectory.isEmpty() || !QDir(m_gameDirectory).exists()) {
        emit operationFailed(QStringLiteral("Select a valid WoW directory first."));
        return;
    }
    if (m_authToken.isEmpty()) {
        emit authenticationExpired();
        return;
    }

    setBusy(true);
    emit operationProgress(0, QStringLiteral("Loading official AddOns..."));
    QNetworkReply *reply = m_network.get(authenticatedRequest(kAddonsUrl));
    const auto body = collectLimitedReply(reply, kMaximumAddonCatalogResponseBytes);

    connect(reply, &QNetworkReply::finished, this, [this, reply, body]() {
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool responseOk =
            finishLimitedReply(reply, body, kMaximumAddonCatalogResponseBytes);
        const auto networkError = reply->error();
        reply->deleteLater();

        if (!responseOk) {
            fail(QStringLiteral("The AddOn catalog response was too large."));
            return;
        }
        if (status == 401 || status == 403) {
            setBusy(false);
            emit authenticationExpired();
            return;
        }
        if (networkError != QNetworkReply::NoError || status < 200 || status >= 300) {
            QString message = apiMessage(*body);
            if (message.isEmpty())
                message = QStringLiteral("Could not retrieve the AddOn catalog (HTTP %1).").arg(status);
            fail(message);
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(*body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            fail(QStringLiteral("The AddOn catalog is not valid JSON."));
            return;
        }

        const QJsonObject root = document.object();
        const QJsonArray array = root.value(QStringLiteral("addons")).toArray();
        if (!root.value(QStringLiteral("success")).toBool() || array.isEmpty()) {
            fail(QStringLiteral("The AddOn API returned an unexpected response."));
            return;
        }

        const QJsonObject state = loadState();
        QVector<AddonInfo> catalog;
        catalog.reserve(array.size());

        for (const QJsonValue &value : array) {
            if (!value.isObject())
                continue;
            const QJsonObject object = value.toObject();
            AddonInfo addon;
            addon.id = object.value(QStringLiteral("id")).toInt();
            addon.name = object.value(QStringLiteral("name")).toString();
            addon.version = object.value(QStringLiteral("version")).toString();
            addon.description = object.value(QStringLiteral("description")).toString();
            addon.fileSizeBytes = object.value(QStringLiteral("file_size_bytes")).toString().toLongLong();
            if (addon.fileSizeBytes <= 0)
                addon.fileSizeBytes =
                    static_cast<qint64>(object.value(QStringLiteral("file_size_bytes")).toDouble());
            addon.updatedAtRaw = object.value(QStringLiteral("updated_at")).toString();
            addon.updatedAt = QDateTime::fromString(addon.updatedAtRaw, Qt::ISODateWithMs);
            addon.managedFolders = managedFolders(state, addon.id);
            if (addon.id <= 0 || addon.name.isEmpty())
                continue;
            addon.status = determineStatus(addon, state);
            catalog.push_back(addon);
        }

        m_catalog = catalog;
        setBusy(false);
        emit operationProgress(100, QStringLiteral("AddOn catalog loaded."));
        emit catalogReady(m_catalog);
    });
}

void AddonManager::installAddons(const QVector<int> &ids)
{
    if (m_busy) {
        emit operationFailed(QStringLiteral("Another optional-content operation is already running."));
        return;
    }
    if (ids.isEmpty())
        return;
    if (m_authToken.isEmpty()) {
        emit authenticationExpired();
        return;
    }
    if (m_catalog.isEmpty()) {
        emit operationFailed(QStringLiteral("Load the AddOn catalog before installing AddOns."));
        return;
    }

    QVector<int> uniqueIds;
    for (int id : ids) {
        if (!findAddon(m_catalog, id)) {
            emit operationFailed(QStringLiteral("Unknown AddOn ID %1.").arg(id));
            return;
        }
        if (!uniqueIds.contains(id))
            uniqueIds.push_back(id);
    }

    QString lockError;
    if (!acquireInstallLock(&lockError)) {
        emit operationFailed(lockError);
        return;
    }

    setBusy(true);
    requestDownloadUrls(uniqueIds);
}

void AddonManager::requestDownloadUrls(const QVector<int> &ids)
{
    QStringList values;
    for (int id : ids)
        values.push_back(QString::number(id));

    QUrl url = kAddonDownloadUrl;
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("addon_ids"), values.join(QLatin1Char(',')));
    url.setQuery(query);
    emit operationProgress(0, QStringLiteral("Requesting AddOn downloads..."));

    QNetworkReply *reply = m_network.get(authenticatedRequest(url));
    const auto body = collectLimitedReply(reply, kMaximumAddonDownloadUrlResponseBytes);

    connect(reply, &QNetworkReply::finished, this, [this, reply, ids, body]() {
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool responseOk =
            finishLimitedReply(reply, body, kMaximumAddonDownloadUrlResponseBytes);
        const auto networkError = reply->error();
        reply->deleteLater();

        if (!responseOk) {
            fail(QStringLiteral("The AddOn download response was too large."));
            return;
        }
        if (status == 401 || status == 403) {
            setBusy(false);
            emit authenticationExpired();
            return;
        }
        if (networkError != QNetworkReply::NoError || status < 200 || status >= 300) {
            QString message = apiMessage(*body);
            if (message.isEmpty())
                message = QStringLiteral("Could not retrieve AddOn download URLs (HTTP %1).").arg(status);
            fail(message);
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(*body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            fail(QStringLiteral("The AddOn download response is not valid JSON."));
            return;
        }

        const QJsonObject root = document.object();
        const QJsonArray files = root.value(QStringLiteral("files")).toArray();
        if (!root.value(QStringLiteral("success")).toBool() || files.isEmpty()) {
            fail(QStringLiteral("The AddOn API returned no download files."));
            return;
        }

        QVector<PendingDownload> pending;
        QVector<int> responseIds;
        for (const QJsonValue &value : files) {
            if (!value.isObject()) {
                fail(QStringLiteral("The AddOn API returned an invalid download entry."));
                return;
            }

            const QJsonObject object = value.toObject();
            PendingDownload item;
            item.id = object.value(QStringLiteral("file_id")).toInt();
            item.filename = object.value(QStringLiteral("filename")).toString();
            item.url = QUrl(object.value(QStringLiteral("url")).toString());

            if (!ids.contains(item.id) || responseIds.contains(item.id) ||
                !item.filename.startsWith(QStringLiteral("Interface/AddOns/")) ||
                !item.filename.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive) ||
                !safeHttpsUrl(item.url) || !findAddon(m_catalog, item.id)) {
                fail(QStringLiteral("The AddOn API returned an invalid or unexpected download."));
                return;
            }

            responseIds.push_back(item.id);
            pending.push_back(item);
        }

        for (int id : ids) {
            if (!responseIds.contains(id)) {
                fail(QStringLiteral("No download was returned for AddOn ID %1.").arg(id));
                return;
            }
        }

        m_pendingDownloads = pending;
        m_downloadIndex = 0;
        startNextDownload();
    });
}

void AddonManager::startNextDownload()
{
    if (!m_busy)
        return;

    if (m_downloadIndex >= m_pendingDownloads.size()) {
        m_pendingDownloads.clear();
        releaseInstallLock();
        setBusy(false);
        emit operationProgress(100, QStringLiteral("AddOn installation complete."));
        emit operationFinished(QStringLiteral("Selected AddOns were installed successfully."));
        refreshCatalog();
        return;
    }

    const PendingDownload item = m_pendingDownloads.at(m_downloadIndex);
    const AddonInfo *addon = findAddon(m_catalog, item.id);
    if (!addon) {
        fail(QStringLiteral("The AddOn catalog changed during installation."));
        return;
    }

    delete m_tempArchive;
    m_tempArchive = new QTemporaryFile(QDir::temp().filePath(QStringLiteral("ebonhold-addon-XXXXXX.zip")), this);
    m_tempArchive->setAutoRemove(true);
    if (!m_tempArchive->open()) {
        fail(QStringLiteral("Could not create a temporary AddOn archive."));
        return;
    }

    QNetworkRequest request(item.url);
    request.setRawHeader("User-Agent", "EbonholdLauncher/1.0");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(60000);
    m_downloadReply = m_network.get(request);

    const QString addonName = addon->name;
    emit operationProgress(0, QStringLiteral("Downloading %1...").arg(addonName));

    connect(m_downloadReply, &QNetworkReply::readyRead, this, [this]() {
        if (!m_downloadReply || !m_tempArchive)
            return;
        const QByteArray chunk = m_downloadReply->readAll();
        if (m_tempArchive->size() + chunk.size() > kMaximumAddonDownloadBytes) {
            m_downloadReply->abort();
            return;
        }
        if (m_tempArchive->write(chunk) != chunk.size())
            m_downloadReply->abort();
    });

    connect(m_downloadReply, &QNetworkReply::downloadProgress, this,
            [this, addonName](qint64 received, qint64 total) {
                int percent = 0;
                if (total > 0)
                    percent = qBound(0, static_cast<int>((received * 100) / total), 100);
                emit operationProgress(percent,
                                       QStringLiteral("Downloading %1...").arg(addonName));
            });

    connect(m_downloadReply, &QNetworkReply::finished, this, [this, item, addonName]() {
        if (!m_downloadReply)
            return;

        QNetworkReply *reply = m_downloadReply;
        m_downloadReply = nullptr;
        const auto networkError = reply->error();
        const QString networkMessage = reply->errorString();

        if (m_tempArchive && reply->bytesAvailable() > 0) {
            const QByteArray tail = reply->readAll();
            if (m_tempArchive->size() + tail.size() <= kMaximumAddonDownloadBytes)
                m_tempArchive->write(tail);
        }
        reply->deleteLater();

        if (networkError != QNetworkReply::NoError) {
            fail(QStringLiteral("Download failed for %1: %2").arg(addonName, networkMessage));
            return;
        }
        if (!m_tempArchive || m_tempArchive->size() <= 0 ||
            m_tempArchive->size() > kMaximumAddonDownloadBytes) {
            fail(QStringLiteral("The downloaded archive for %1 is empty or too large.").arg(addonName));
            return;
        }

        m_tempArchive->flush();
        const AddonInfo *addon = findAddon(m_catalog, item.id);
        if (!addon) {
            fail(QStringLiteral("The AddOn catalog changed during installation."));
            return;
        }

        QStringList installedFolders;
        QString error;
        emit operationProgress(100, QStringLiteral("Installing %1...").arg(addonName));
        if (!installArchive(*addon, m_tempArchive->fileName(), &installedFolders, &error)) {
            fail(error);
            return;
        }

        delete m_tempArchive;
        m_tempArchive = nullptr;
        ++m_downloadIndex;
        startNextDownload();
    });
}

bool AddonManager::installArchive(const AddonInfo &addon,
                                   const QString &archivePath,
                                   QStringList *installedFolders,
                                   QString *error)
{
    QString addonsDir;
    if (!SafeFilesystem::ensureDirectory(m_gameDirectory,
                                         QStringLiteral("Interface/AddOns"),
                                         &addonsDir,
                                         error)) {
        return false;
    }

    QTemporaryDir staging(QDir(addonsDir).filePath(QStringLiteral(".ebonhold-stage-XXXXXX")));
    if (!staging.isValid()) {
        *error = QStringLiteral("Could not create the AddOn staging directory.");
        return false;
    }

    struct archive *archive = archive_read_new();
    archive_read_support_filter_all(archive);
    archive_read_support_format_zip(archive);

    if (archive_read_open_filename(archive,
                                   QFile::encodeName(archivePath).constData(),
                                   10240) != ARCHIVE_OK) {
        *error = QStringLiteral("Could not open the AddOn archive for %1: %2")
                     .arg(addon.name,
                          QString::fromUtf8(archive_error_string(archive)));
        archive_read_free(archive);
        return false;
    }

    QStringList topLevels;
    bool ok = true;
    int entryCount = 0;
    qint64 totalExtractedBytes = 0;
    struct archive_entry *entry = nullptr;

    while (true) {
        const int headerResult = archive_read_next_header(archive, &entry);
        if (headerResult == ARCHIVE_EOF)
            break;
        if (headerResult != ARCHIVE_OK) {
            *error = QStringLiteral("Could not read the archive for %1: %2")
                         .arg(addon.name,
                              QString::fromUtf8(archive_error_string(archive)));
            ok = false;
            break;
        }

        ++entryCount;
        if (entryCount > kMaximumAddonArchiveEntries) {
            *error = QStringLiteral("The archive for %1 contains too many files.")
                         .arg(addon.name);
            ok = false;
            break;
        }

        const char *rawPath = archive_entry_pathname_utf8(entry);
        if (!rawPath)
            rawPath = archive_entry_pathname(entry);
        const QString path = QString::fromUtf8(rawPath ? rawPath : "");

        if (path.size() > kMaximumAddonArchivePathLength ||
            !archivePathIsSafe(path)) {
            *error = QStringLiteral("The archive for %1 contains an unsafe path.")
                         .arg(addon.name);
            ok = false;
            break;
        }

        const QStringList components = pathComponents(path);
        if (components.size() > kMaximumAddonArchiveDepth) {
            *error = QStringLiteral("The archive for %1 contains an excessively deep path.")
                         .arg(addon.name);
            ok = false;
            break;
        }

        const auto fileType = archive_entry_filetype(entry);
        if (archive_entry_symlink(entry) || archive_entry_hardlink(entry) ||
            (fileType != AE_IFREG && fileType != AE_IFDIR)) {
            *error = QStringLiteral(
                         "The archive for %1 contains an unsupported link or special file.")
                         .arg(addon.name);
            ok = false;
            break;
        }

        if (components.isEmpty())
            continue;

        const la_int64_t declaredSize =
            fileType == AE_IFREG ? archive_entry_size(entry) : 0;
        if (declaredSize > kMaximumAddonSingleFileBytes) {
            *error = QStringLiteral("The archive for %1 contains a file that is too large.")
                         .arg(addon.name);
            ok = false;
            break;
        }

        // Only AddOn directories belong in Interface/AddOns.
        // Ignore loose files like README files.
        if (components.size() == 1 && fileType == AE_IFREG) {
            archive_read_data_skip(archive);
            continue;
        }

        const QString topLevel = components.first();
        if (!safeFolderName(topLevel)) {
            *error = QStringLiteral("The archive for %1 contains an invalid top-level folder.")
                         .arg(addon.name);
            ok = false;
            break;
        }

        if (!topLevels.contains(topLevel))
            topLevels.push_back(topLevel);

        const QString cleaned = QDir::cleanPath(path);

        if (fileType == AE_IFDIR) {
            QString createdDirectory;
            QString pathError;
            if (!SafeFilesystem::ensureDirectory(staging.path(),
                                                 cleaned,
                                                 &createdDirectory,
                                                 &pathError)) {
                *error = QStringLiteral("Could not safely create a directory for %1: %2")
                             .arg(addon.name, pathError);
                ok = false;
                break;
            }
            archive_read_data_skip(archive);
            continue;
        }

        if (declaredSize > 0 &&
            totalExtractedBytes > kMaximumAddonExtractedBytes - declaredSize) {
            *error = QStringLiteral("The archive for %1 expands beyond the allowed size.")
                         .arg(addon.name);
            ok = false;
            break;
        }

        QString destination;
        QString pathError;
        if (!SafeFilesystem::resolveDestination(staging.path(),
                                                cleaned,
                                                true,
                                                &destination,
                                                &pathError)) {
            *error = QStringLiteral("Could not safely extract %1: %2")
                         .arg(addon.name, pathError);
            ok = false;
            break;
        }

        QSaveFile output(destination);
        output.setDirectWriteFallback(false);
        if (!output.open(QIODevice::WriteOnly)) {
            *error = QStringLiteral("Could not create an extracted file for %1: %2")
                         .arg(addon.name, output.errorString());
            ok = false;
            break;
        }

        qint64 fileExtractedBytes = 0;
        char buffer[64 * 1024];
        while (true) {
            const la_ssize_t bytesRead =
                archive_read_data(archive, buffer, sizeof(buffer));
            if (bytesRead == 0)
                break;
            if (bytesRead < 0) {
                *error = QStringLiteral("Could not extract %1: %2")
                             .arg(addon.name,
                                  QString::fromUtf8(archive_error_string(archive)));
                ok = false;
                break;
            }

            const qint64 chunkSize = static_cast<qint64>(bytesRead);
            if (fileExtractedBytes > kMaximumAddonSingleFileBytes - chunkSize ||
                totalExtractedBytes > kMaximumAddonExtractedBytes - chunkSize) {
                *error = QStringLiteral("The archive for %1 expands beyond the allowed size.")
                             .arg(addon.name);
                ok = false;
                break;
            }

            if (output.write(buffer, chunkSize) != chunkSize) {
                *error = QStringLiteral("Could not write an extracted file for %1: %2")
                             .arg(addon.name, output.errorString());
                ok = false;
                break;
            }

            fileExtractedBytes += chunkSize;
            totalExtractedBytes += chunkSize;
        }

        if (!ok) {
            output.cancelWriting();
            break;
        }

        if (!output.commit()) {
            *error = QStringLiteral("Could not finish an extracted file for %1: %2")
                         .arg(addon.name, output.errorString());
            ok = false;
            break;
        }
    }

    archive_read_close(archive);
    archive_read_free(archive);

    if (!ok)
        return false;

    if (topLevels.isEmpty()) {
        *error = QStringLiteral("The archive for %1 contains no AddOn folders.")
                     .arg(addon.name);
        return false;
    }

    for (const QString &folder : topLevels) {
        if (!QFileInfo(QDir(staging.path()).filePath(folder)).isDir()) {
            *error = QStringLiteral("The archive for %1 has an invalid layout.")
                         .arg(addon.name);
            return false;
        }
    }

    QString verifiedAddonsDir;
    if (!SafeFilesystem::ensureDirectory(m_gameDirectory,
                                         QStringLiteral("Interface/AddOns"),
                                         &verifiedAddonsDir,
                                         error) ||
        QDir::cleanPath(verifiedAddonsDir) != QDir::cleanPath(addonsDir)) {
        if (error && error->isEmpty())
            *error = QStringLiteral("The AddOn directory changed during installation.");
        return false;
    }

    QJsonObject state = loadState();
    const QStringList previousFolders = managedFolders(state, addon.id);

    for (const QString &folder : topLevels) {
        if (folderIsShared(state, addon.id, folder)) {
            *error = QStringLiteral("Cannot safely replace shared AddOn folder '%1'.")
                         .arg(folder);
            return false;
        }

        const QFileInfo targetInfo(QDir(addonsDir).filePath(folder));
        if (targetInfo.isSymLink()) {
            *error = QStringLiteral("Refusing to replace symlinked AddOn folder '%1'.")
                         .arg(folder);
            return false;
        }
    }

    QTemporaryDir backup(QDir(addonsDir).filePath(QStringLiteral(".ebonhold-backup-XXXXXX")));
    if (!backup.isValid()) {
        *error = QStringLiteral("Could not create an AddOn rollback directory.");
        return false;
    }

    QStringList backedUp;
    QStringList newlyInstalled;

    auto rollback = [&]() {
        for (const QString &folder : newlyInstalled)
            removePath(QDir(addonsDir).filePath(folder));

        for (const QString &folder : backedUp) {
            const QString source = QDir(backup.path()).filePath(folder);
            const QString target = QDir(addonsDir).filePath(folder);
            if (QFileInfo::exists(source) || QFileInfo(source).isSymLink())
                QDir().rename(source, target);
        }
    };

    auto backupTarget = [&](const QString &folder) -> bool {
        const QString target = QDir(addonsDir).filePath(folder);
        const QFileInfo targetInfo(target);
        if (!targetInfo.exists() && !targetInfo.isSymLink())
            return true;
        if (targetInfo.isSymLink())
            return false;
        if (!QDir().rename(target, QDir(backup.path()).filePath(folder)))
            return false;
        backedUp.push_back(folder);
        return true;
    };

    for (const QString &folder : topLevels) {
        if (!backupTarget(folder)) {
            rollback();
            *error = QStringLiteral(
                         "Could not safely prepare existing AddOn folder '%1' for replacement.")
                         .arg(folder);
            return false;
        }
    }

    for (const QString &folder : previousFolders) {
        if (topLevels.contains(folder) || folderIsShared(state, addon.id, folder))
            continue;
        if (!backupTarget(folder)) {
            rollback();
            *error = QStringLiteral("Could not safely prepare old AddOn folder '%1' for cleanup.")
                         .arg(folder);
            return false;
        }
    }

    if (!SafeFilesystem::ensureDirectory(m_gameDirectory,
                                         QStringLiteral("Interface/AddOns"),
                                         &verifiedAddonsDir,
                                         error) ||
        QDir::cleanPath(verifiedAddonsDir) != QDir::cleanPath(addonsDir)) {
        rollback();
        if (error && error->isEmpty())
            *error = QStringLiteral("The AddOn directory changed during installation.");
        return false;
    }

    for (const QString &folder : topLevels) {
        const QString source = QDir(staging.path()).filePath(folder);
        const QString target = QDir(addonsDir).filePath(folder);
        if (!QDir().rename(source, target)) {
            rollback();
            *error = QStringLiteral("Could not install AddOn folder '%1'.").arg(folder);
            return false;
        }
        newlyInstalled.push_back(folder);
    }

    QJsonObject addons = state.value(QStringLiteral("addons")).toObject();
    QJsonObject record;
    record.insert(QStringLiteral("name"), addon.name);
    record.insert(QStringLiteral("updated_at"), addon.updatedAtRaw);

    QJsonArray foldersArray;
    for (const QString &folder : topLevels)
        foldersArray.push_back(folder);

    record.insert(QStringLiteral("folders"), foldersArray);
    addons.insert(QString::number(addon.id), record);
    state.insert(QStringLiteral("addons"), addons);

    if (!saveState(state, error)) {
        rollback();
        return false;
    }

    *installedFolders = topLevels;
    return true;
}

bool AddonManager::removePath(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink())
        return true;
    if (info.isSymLink() || info.isFile())
        return QFile::remove(path);
    return QDir(path).removeRecursively();
}

bool AddonManager::removeManagedAddon(const AddonInfo &addon, QString *error)
{
    QJsonObject state = loadState();
    QJsonObject addons = state.value(QStringLiteral("addons")).toObject();
    const QJsonObject record = addons.value(QString::number(addon.id)).toObject();
    if (record.isEmpty()) {
        *error = QStringLiteral("%1 is not managed by this launcher.").arg(addon.name);
        return false;
    }

    const QStringList folders = managedFolders(state, addon.id);

    QString addonsDir;
    if (!SafeFilesystem::ensureDirectory(m_gameDirectory,
                                         QStringLiteral("Interface/AddOns"),
                                         &addonsDir,
                                         error)) {
        return false;
    }

    QTemporaryDir backup(QDir(addonsDir).filePath(QStringLiteral(".ebonhold-remove-XXXXXX")));
    if (!backup.isValid()) {
        *error = QStringLiteral("Could not create an AddOn rollback directory.");
        return false;
    }

    QStringList moved;
    for (const QString &folder : folders) {
        if (folderIsShared(state, addon.id, folder))
            continue;

        const QString source = QDir(addonsDir).filePath(folder);
        const QFileInfo sourceInfo(source);
        if (!sourceInfo.exists() && !sourceInfo.isSymLink())
            continue;

        if (sourceInfo.isSymLink()) {
            *error = QStringLiteral("Refusing to remove symlinked AddOn folder '%1'.")
                         .arg(folder);
            return false;
        }

        if (!QDir().rename(source, QDir(backup.path()).filePath(folder))) {
            for (const QString &restore : moved)
                QDir().rename(QDir(backup.path()).filePath(restore),
                              QDir(addonsDir).filePath(restore));
            *error = QStringLiteral("Could not remove AddOn folder '%1'.").arg(folder);
            return false;
        }

        moved.push_back(folder);
    }

    addons.remove(QString::number(addon.id));
    state.insert(QStringLiteral("addons"), addons);

    if (!saveState(state, error)) {
        for (const QString &restore : moved)
            QDir().rename(QDir(backup.path()).filePath(restore),
                          QDir(addonsDir).filePath(restore));
        return false;
    }

    return true;
}

void AddonManager::removeAddon(int id)
{
    if (m_busy) {
        emit operationFailed(QStringLiteral("Another optional-content operation is already running."));
        return;
    }
    const AddonInfo *addon = findAddon(m_catalog, id);
    if (!addon) {
        emit operationFailed(QStringLiteral("Unknown AddOn ID %1.").arg(id));
        return;
    }

    QString lockError;
    if (!acquireInstallLock(&lockError)) {
        emit operationFailed(lockError);
        return;
    }

    setBusy(true);
    QString error;
    const QString name = addon->name;
    if (!removeManagedAddon(*addon, &error)) {
        fail(error);
        return;
    }
    releaseInstallLock();
    setBusy(false);
    emit operationFinished(QStringLiteral("%1 was removed.").arg(name));
    refreshCatalog();
}

void AddonManager::fail(const QString &message)
{
    if (m_downloadReply) {
        m_downloadReply->disconnect(this);
        m_downloadReply->abort();
        m_downloadReply->deleteLater();
        m_downloadReply = nullptr;
    }
    delete m_tempArchive;
    m_tempArchive = nullptr;
    m_pendingDownloads.clear();
    m_downloadIndex = 0;
    releaseInstallLock();
    setBusy(false);
    emit operationFailed(message);
}
