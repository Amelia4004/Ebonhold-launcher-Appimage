#include "UpdateManager.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QUrl>
#include <QUrlQuery>
#include <QtConcurrent/QtConcurrentRun>

namespace {
const QUrl kPatchDownloadBase(QStringLiteral("https://api.project-ebonhold.com/api/launcher/download"));

QByteArray decodeManifestMd5(const QString &encoded)
{
    const QByteArray raw = QByteArray::fromBase64(encoded.toUtf8());
    if (raw.size() != 16)
        return {};
    return raw.toHex().toLower();
}

bool optionIsRequired(const QJsonObject &file)
{
    const QJsonValue option = file.value(QStringLiteral("option_slug"));
    return option.isUndefined() || option.isNull();
}

bool safeRelativePath(const QString &path)
{
    if (path.isEmpty() || QDir::isAbsolutePath(path))
        return false;

    const QString cleaned = QDir::cleanPath(path);
    if (cleaned == QStringLiteral(".") || cleaned == QStringLiteral("..") ||
        cleaned.startsWith(QStringLiteral("../")))
        return false;

    for (const QChar ch : path) {
        if (ch.unicode() < 0x20 || ch == QChar('|'))
            return false;
    }
    return true;
}

bool destinationInsideRoot(const QString &rootDirectory,
                           const QString &relativePath,
                           QString *destination)
{
    const QDir root(rootDirectory);
    const QString rootPath = QDir::cleanPath(root.absolutePath());
    const QString candidate = QDir::cleanPath(root.absoluteFilePath(relativePath));
    const QString prefix = rootPath + QDir::separator();

    if (candidate != rootPath && !candidate.startsWith(prefix))
        return false;

    *destination = candidate;
    return true;
}

void appendFiles(const QJsonArray &array, QVector<PatchFile> *files, QString *error)
{
    for (const QJsonValue &value : array) {
        if (!value.isObject())
            continue;

        const QJsonObject object = value.toObject();
        if (!optionIsRequired(object))
            continue;

        const QString path = object.value(QStringLiteral("file_path_from_game_root")).toString();
        if (path.compare(QStringLiteral("Data/enUS/realmlist.wtf"), Qt::CaseSensitive) == 0)
            continue;

        const int id = object.value(QStringLiteral("id")).toInt();
        const QByteArray md5 = decodeManifestMd5(object.value(QStringLiteral("file_hash")).toString());

        if (id <= 0 || !safeRelativePath(path) || md5.size() != 32) {
            *error = QStringLiteral("Invalid or unsafe manifest entry: %1").arg(path);
            return;
        }

        files->push_back(PatchFile{id, QDir::cleanPath(path), md5});
    }
}
}

UpdateManager::UpdateManager(QObject *parent)
    : QObject(parent)
{
}

void UpdateManager::setAuthToken(const QString &token)
{
    m_authToken = token;
}


std::optional<PatchFile> UpdateManager::findManifestFile(const QJsonObject &manifest,
                                                         const QString &relativePath,
                                                         const QString &gameSlug)
{
    const QString wantedPath = QDir::cleanPath(relativePath);
    if (!safeRelativePath(wantedPath))
        return std::nullopt;

    auto findInArray = [&wantedPath](const QJsonArray &array) -> std::optional<PatchFile> {
        for (const QJsonValue &value : array) {
            if (!value.isObject())
                continue;

            const QJsonObject object = value.toObject();
            const QString path = QDir::cleanPath(
                object.value(QStringLiteral("file_path_from_game_root")).toString());
            if (path.compare(wantedPath, Qt::CaseSensitive) != 0)
                continue;

            const int id = object.value(QStringLiteral("id")).toInt();
            const QByteArray md5 =
                decodeManifestMd5(object.value(QStringLiteral("file_hash")).toString());
            if (id <= 0 || md5.size() != 32)
                return std::nullopt;

            return PatchFile{id, path, md5};
        }

        return std::nullopt;
    };

    const QJsonObject data = manifest.value(QStringLiteral("data")).toObject();
    const QJsonArray games = data.value(QStringLiteral("games")).toArray();
    for (const QJsonValue &value : games) {
        const QJsonObject game = value.toObject();
        if (game.value(QStringLiteral("slug")).toString() != gameSlug)
            continue;

        if (const auto found = findInArray(game.value(QStringLiteral("files")).toArray()))
            return found;
        break;
    }

    return findInArray(data.value(QStringLiteral("common")).toObject()
                           .value(QStringLiteral("files")).toArray());
}

QVector<PatchFile> UpdateManager::collectRequiredFiles(const QJsonObject &manifest,
                                                       const QString &gameSlug,
                                                       QString *realmlist,
                                                       QString *error)
{
    QVector<PatchFile> files;

    const QJsonObject data = manifest.value(QStringLiteral("data")).toObject();
    const QJsonObject common = data.value(QStringLiteral("common")).toObject();
    appendFiles(common.value(QStringLiteral("files")).toArray(), &files, error);
    if (!error->isEmpty())
        return {};

    const QJsonArray games = data.value(QStringLiteral("games")).toArray();
    QJsonObject selectedGame;
    for (const QJsonValue &value : games) {
        const QJsonObject game = value.toObject();
        if (game.value(QStringLiteral("slug")).toString() == gameSlug) {
            selectedGame = game;
            break;
        }
    }

    if (selectedGame.isEmpty()) {
        *error = QStringLiteral("Game '%1' was not found in the API manifest.").arg(gameSlug);
        return {};
    }

    *realmlist = selectedGame.value(QStringLiteral("realmlist")).toString();
    appendFiles(selectedGame.value(QStringLiteral("files")).toArray(), &files, error);
    if (!error->isEmpty())
        return {};

    return files;
}

QVector<PatchScanResult> UpdateManager::scanFiles(const QString &gameDirectory,
                                                  const QVector<PatchFile> &files)
{
    QVector<PatchScanResult> results;
    results.reserve(files.size());

    for (const PatchFile &patch : files) {
        PatchScanResult result;
        result.file = patch;

        QString destination;
        if (!destinationInsideRoot(gameDirectory, patch.relativePath, &destination)) {
            result.error = QStringLiteral("Unsafe destination path.");
            results.push_back(result);
            continue;
        }

        QFile file(destination);
        result.exists = file.exists();
        if (!result.exists) {
            results.push_back(result);
            continue;
        }

        if (!file.open(QIODevice::ReadOnly)) {
            result.error = QStringLiteral("Could not open file for hashing.");
            results.push_back(result);
            continue;
        }

        QCryptographicHash hash(QCryptographicHash::Md5);
        if (!hash.addData(&file)) {
            result.error = QStringLiteral("Could not hash file.");
            results.push_back(result);
            continue;
        }

        result.matches = (hash.result().toHex().toLower() == patch.expectedMd5);
        results.push_back(result);
    }

    return results;
}

void UpdateManager::scan(const QString &gameDirectory,
                         const QJsonObject &manifest,
                         const QString &gameSlug)
{
    if (m_updating) {
        emit scanFailed(QStringLiteral("An update is already running."));
        return;
    }

    QString realmlist;
    QString error;
    const QVector<PatchFile> files = collectRequiredFiles(manifest, gameSlug, &realmlist, &error);

    if (!error.isEmpty()) {
        emit scanFailed(error);
        return;
    }

    if (files.isEmpty()) {
        emit scanFailed(QStringLiteral("The manifest contains no required files for this game."));
        return;
    }

    emit scanStarted(files.size());

    auto *watcher = new QFutureWatcher<QVector<PatchScanResult>>(this);
    connect(watcher, &QFutureWatcher<QVector<PatchScanResult>>::finished, this,
            [this, watcher, realmlist]() {
                const QVector<PatchScanResult> results = watcher->result();
                watcher->deleteLater();
                emit scanFinished(results, realmlist);
            });

    watcher->setFuture(QtConcurrent::run(&UpdateManager::scanFiles, gameDirectory, files));
}

QNetworkRequest UpdateManager::authenticatedRequest(const QUrl &url) const
{
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", QByteArray("Bearer ") + m_authToken.toUtf8());
    request.setRawHeader("User-Agent", "EbonholdLauncher/1.0");
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("X-Client-Id", "EbonholdLauncher");
    request.setRawHeader("Origin", "https://project-ebonhold.com");
    request.setRawHeader("Referer", "https://project-ebonhold.com/download");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return request;
}

QString UpdateManager::apiMessage(const QByteArray &body)
{
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isObject())
        return {};

    const QJsonObject object = document.object();
    QString message = object.value(QStringLiteral("message")).toString();
    if (message.isEmpty())
        message = object.value(QStringLiteral("error")).toString();
    return message;
}

bool UpdateManager::safeHttpsUrl(const QUrl &url)
{
    return url.isValid() && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 &&
           !url.host().isEmpty();
}

void UpdateManager::installUpdates(const QString &gameDirectory,
                                   const QVector<PatchFile> &files,
                                   const QString &realmlist)
{
    if (m_updating) {
        emit updateFailed(QStringLiteral("An update is already running."));
        return;
    }

    if (m_authToken.isEmpty()) {
        emit authenticationExpired();
        return;
    }

    if (files.isEmpty()) {
        m_gameDirectory = QDir::cleanPath(gameDirectory);
        m_realmlist = realmlist;
        QString error;
        if (!writeRealmlist(&error)) {
            emit updateFailed(error);
            return;
        }
        emit updateFinished();
        return;
    }

    m_gameDirectory = QDir::cleanPath(gameDirectory);
    m_realmlist = realmlist;
    m_updateFiles = files;
    m_updateIndex = 0;
    m_updating = true;

    emit updateStarted(m_updateFiles.size());
    downloadNext();
}

void UpdateManager::downloadNext()
{
    if (!m_updating)
        return;

    if (m_updateIndex >= m_updateFiles.size()) {
        finishUpdate();
        return;
    }

    const PatchFile &file = m_updateFiles.at(m_updateIndex);
    emit updateFileStarted(m_updateIndex + 1, m_updateFiles.size(), file.relativePath);
    requestDownloadUrl(file);
}

void UpdateManager::requestDownloadUrl(const PatchFile &file)
{
    QUrl url = kPatchDownloadBase;
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("file_ids"), QString::number(file.id));
    url.setQuery(query);

    QNetworkReply *reply = m_network.get(authenticatedRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, file]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const auto networkError = reply->error();
        reply->deleteLater();

        if (!m_updating)
            return;

        if (status == 401 || status == 403) {
            m_updating = false;
            emit authenticationExpired();
            return;
        }

        if (networkError != QNetworkReply::NoError || status < 200 || status >= 300) {
            QString message = apiMessage(body);
            if (message.isEmpty())
                message = QStringLiteral("Could not get a download URL for %1 (HTTP %2).")
                              .arg(file.relativePath)
                              .arg(status);
            failUpdate(message);
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            failUpdate(QStringLiteral("Invalid download response for %1.").arg(file.relativePath));
            return;
        }

        const QJsonObject root = document.object();
        QString downloadUrl;
        const QJsonArray responseFiles = root.value(QStringLiteral("files")).toArray();
        if (!responseFiles.isEmpty() && responseFiles.first().isObject())
            downloadUrl = responseFiles.first().toObject().value(QStringLiteral("url")).toString();
        if (downloadUrl.isEmpty())
            downloadUrl = root.value(QStringLiteral("url")).toString();

        const QUrl parsedUrl(downloadUrl);
        if (!safeHttpsUrl(parsedUrl)) {
            failUpdate(QStringLiteral("The API returned no safe HTTPS download URL for %1.")
                           .arg(file.relativePath));
            return;
        }

        startFileDownload(file, parsedUrl);
    });
}

void UpdateManager::startFileDownload(const PatchFile &file, const QUrl &url)
{
    QString destination;
    if (!destinationInsideRoot(m_gameDirectory, file.relativePath, &destination)) {
        failUpdate(QStringLiteral("Unsafe destination path for %1.").arg(file.relativePath));
        return;
    }

    const QFileInfo destinationInfo(destination);
    if (!QDir().mkpath(destinationInfo.absolutePath())) {
        failUpdate(QStringLiteral("Could not create the directory for %1.").arg(file.relativePath));
        return;
    }

    delete m_outputFile;
    m_outputFile = new QSaveFile(destination, this);
    m_outputFile->setDirectWriteFallback(false);
    if (!m_outputFile->open(QIODevice::WriteOnly)) {
        const QString error = m_outputFile->errorString();
        delete m_outputFile;
        m_outputFile = nullptr;
        failUpdate(QStringLiteral("Could not create temporary output for %1: %2")
                       .arg(file.relativePath, error));
        return;
    }

    delete m_downloadHash;
    m_downloadHash = new QCryptographicHash(QCryptographicHash::Md5);

    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "EbonholdLauncher/1.0");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    m_downloadReply = m_network.get(request);

    connect(m_downloadReply, &QNetworkReply::readyRead, this, [this, file]() {
        if (!m_downloadReply || !m_outputFile || !m_downloadHash)
            return;

        const QByteArray chunk = m_downloadReply->readAll();
        if (chunk.isEmpty())
            return;

        m_downloadHash->addData(chunk);
        if (m_outputFile->write(chunk) != chunk.size()) {
            const QString error = m_outputFile->errorString();
            m_downloadReply->abort();
            failUpdate(QStringLiteral("Could not write %1: %2").arg(file.relativePath, error));
        }
    });

    connect(m_downloadReply, &QNetworkReply::downloadProgress, this,
            [this, file](qint64 received, qint64 total) {
                if (total <= 0) {
                    const int base = (m_updateIndex * 100) / qMax(1, m_updateFiles.size());
                    emit updateProgress(base, file.relativePath);
                    return;
                }

                const double fileFraction = static_cast<double>(received) / static_cast<double>(total);
                const double overall = (static_cast<double>(m_updateIndex) + fileFraction) /
                                       static_cast<double>(qMax(1, m_updateFiles.size()));
                emit updateProgress(qBound(0, static_cast<int>(overall * 100.0), 100), file.relativePath);
            });

    connect(m_downloadReply, &QNetworkReply::finished, this, [this, file]() {
        if (!m_downloadReply)
            return;

        QNetworkReply *reply = m_downloadReply;
        m_downloadReply = nullptr;

        if (!m_updating) {
            reply->deleteLater();
            return;
        }

        const auto networkError = reply->error();
        const QString networkMessage = reply->errorString();

        if (m_outputFile && reply->bytesAvailable() > 0) {
            const QByteArray chunk = reply->readAll();
            m_downloadHash->addData(chunk);
            if (m_outputFile->write(chunk) != chunk.size()) {
                reply->deleteLater();
                failUpdate(QStringLiteral("Could not finish writing %1.").arg(file.relativePath));
                return;
            }
        }

        reply->deleteLater();

        if (networkError != QNetworkReply::NoError) {
            failUpdate(QStringLiteral("Download failed for %1: %2")
                           .arg(file.relativePath, networkMessage));
            return;
        }

        const QByteArray actualMd5 = m_downloadHash->result().toHex().toLower();
        if (actualMd5 != file.expectedMd5) {
            failUpdate(QStringLiteral("Checksum mismatch for %1. The existing file was left untouched.")
                           .arg(file.relativePath));
            return;
        }

        if (!m_outputFile->commit()) {
            const QString error = m_outputFile->errorString();
            failUpdate(QStringLiteral("Could not replace %1: %2").arg(file.relativePath, error));
            return;
        }

        delete m_outputFile;
        m_outputFile = nullptr;
        delete m_downloadHash;
        m_downloadHash = nullptr;

        emit updateFileFinished(m_updateIndex + 1, m_updateFiles.size(), file.relativePath);
        ++m_updateIndex;
        downloadNext();
    });
}

bool UpdateManager::writeRealmlist(QString *error) const
{
    if (m_realmlist.trimmed().isEmpty())
        return true;

    const QString relativePath = QStringLiteral("Data/enUS/realmlist.wtf");
    QString destination;
    if (!destinationInsideRoot(m_gameDirectory, relativePath, &destination)) {
        *error = QStringLiteral("Unsafe realmlist destination path.");
        return false;
    }

    const QFileInfo info(destination);
    if (!QDir().mkpath(info.absolutePath())) {
        *error = QStringLiteral("Could not create Data/enUS for realmlist.wtf.");
        return false;
    }

    const QByteArray wanted = QStringLiteral("set realmlist %1\n").arg(m_realmlist.trimmed()).toUtf8();

    QFile existing(destination);
    if (existing.open(QIODevice::ReadOnly) && existing.readAll() == wanted)
        return true;

    QSaveFile output(destination);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("Could not write realmlist.wtf: %1").arg(output.errorString());
        return false;
    }

    if (output.write(wanted) != wanted.size() || !output.commit()) {
        *error = QStringLiteral("Could not update realmlist.wtf: %1").arg(output.errorString());
        return false;
    }

    return true;
}

void UpdateManager::finishUpdate()
{
    QString error;
    if (!writeRealmlist(&error)) {
        failUpdate(error);
        return;
    }

    m_updating = false;
    m_updateFiles.clear();
    emit updateProgress(100, QString());
    emit updateFinished();
}

void UpdateManager::failUpdate(const QString &message)
{
    if (m_downloadReply) {
        m_downloadReply->disconnect(this);
        m_downloadReply->abort();
        m_downloadReply->deleteLater();
        m_downloadReply = nullptr;
    }

    if (m_outputFile) {
        m_outputFile->cancelWriting();
        delete m_outputFile;
        m_outputFile = nullptr;
    }

    delete m_downloadHash;
    m_downloadHash = nullptr;

    m_updating = false;
    emit updateFailed(message);
}
