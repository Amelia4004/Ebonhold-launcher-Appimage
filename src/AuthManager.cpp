#include "AuthManager.h"

#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSharedPointer>
#include <QStandardPaths>
#include <QUrl>

namespace {
const QUrl kLoginUrl(QStringLiteral("https://api.project-ebonhold.com/api/auth/login"));
const QUrl kGamesUrl(QStringLiteral("https://api.project-ebonhold.com/api/launcher/games"));
constexpr qint64 kMaximumLoginResponseBytes = 1LL * 1024LL * 1024LL;
constexpr qint64 kMaximumManifestResponseBytes = 16LL * 1024LL * 1024LL;
constexpr qint64 kMaximumTokenBytes = 16LL * 1024LL;
constexpr int kApiTransferTimeoutMs = 30000;


void applyCommonHeaders(QNetworkRequest &request)
{
    request.setRawHeader("User-Agent", "EbonholdLauncher/1.0");
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("X-Client-Id", "EbonholdLauncher");
    request.setRawHeader("Origin", "https://project-ebonhold.com");
    request.setRawHeader("Referer", "https://project-ebonhold.com/download");
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

}

AuthManager::AuthManager(QObject *parent)
    : QObject(parent)
{
    loadToken();
}

bool AuthManager::hasToken() const
{
    return !m_token.isEmpty();
}

QString AuthManager::token() const
{
    return m_token;
}

QString AuthManager::tokenFilePath() const
{
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return QDir(configDir).filePath(QStringLiteral("token"));
}

void AuthManager::loadToken()
{
    const QString path = tokenFilePath();
    const QFileInfo info(path);
    if (!info.exists() || info.isSymLink() || info.size() > kMaximumTokenBytes)
        return;

    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    const QByteArray data = file.read(kMaximumTokenBytes + 1);
    if (data.size() > kMaximumTokenBytes)
        return;

    m_token = QString::fromUtf8(data).trimmed();
    if (m_token == QStringLiteral("null"))
        m_token.clear();
}

bool AuthManager::saveToken(const QString &token)
{
    const QByteArray data = token.toUtf8();
    if (data.isEmpty() || data.size() > kMaximumTokenBytes)
        return false;

    const QString path = tokenFilePath();
    const QString configDir = QFileInfo(path).absolutePath();

    QFileInfo directoryInfo(configDir);
    if (directoryInfo.exists() && directoryInfo.isSymLink())
        return false;

    if (!QDir().mkpath(configDir))
        return false;

    if (!QFile::setPermissions(configDir,
                               QFileDevice::ReadOwner |
                               QFileDevice::WriteOwner |
                               QFileDevice::ExeOwner)) {
        return false;
    }

    const QFileInfo tokenInfo(path);
    if (tokenInfo.isSymLink())
        return false;

    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.cancelWriting();
        return false;
    }

    if (file.write(data) != data.size()) {
        file.cancelWriting();
        return false;
    }

    if (!file.commit())
        return false;

    return QFile::setPermissions(path,
                                 QFileDevice::ReadOwner |
                                 QFileDevice::WriteOwner);
}

void AuthManager::clearToken()
{
    m_token.clear();
    QFile::remove(tokenFilePath());
}

QNetworkRequest AuthManager::authenticatedRequest(const QUrl &url) const
{
    QNetworkRequest request(url);
    applyCommonHeaders(request);
    request.setRawHeader("Authorization", QByteArray("Bearer ") + m_token.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);
    request.setTransferTimeout(kApiTransferTimeoutMs);
    return request;
}

QString AuthManager::messageFromJson(const QByteArray &data)
{
    const QJsonDocument document = QJsonDocument::fromJson(data);
    if (!document.isObject())
        return {};

    const QJsonObject object = document.object();
    QString message = object.value(QStringLiteral("message")).toString();
    if (message.isEmpty())
        message = object.value(QStringLiteral("error")).toString();
    return message;
}

void AuthManager::fetchGamesManifest()
{
    if (m_token.isEmpty()) {
        emit loginRequired();
        return;
    }

    QNetworkReply *reply = m_network.get(authenticatedRequest(kGamesUrl));
    const auto body = collectLimitedReply(reply, kMaximumManifestResponseBytes);

    connect(reply, &QNetworkReply::finished, this, [this, reply, body]() {
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool responseOk =
            finishLimitedReply(reply, body, kMaximumManifestResponseBytes);
        const auto networkError = reply->error();
        reply->deleteLater();

        if (!responseOk) {
            emit requestFailed(QStringLiteral("The games manifest response was too large."));
            return;
        }

        if (status == 401 || status == 403) {
            clearToken();
            emit loginRequired();
            return;
        }

        if (networkError != QNetworkReply::NoError || status < 200 || status >= 300) {
            QString message = messageFromJson(*body);
            if (message.isEmpty())
                message = QStringLiteral("Manifest request failed (HTTP %1).").arg(status);
            emit requestFailed(message);
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(*body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            emit requestFailed(QStringLiteral("The games manifest is not valid JSON."));
            return;
        }

        const QJsonObject root = document.object();
        if (!root.value(QStringLiteral("success")).toBool() ||
            !root.value(QStringLiteral("data")).isObject()) {
            emit requestFailed(QStringLiteral("The games API returned an unexpected response."));
            return;
        }

        emit manifestReady(root);
    });
}

void AuthManager::login(const QString &username, const QString &password)
{
    if (username.trimmed().isEmpty() || password.isEmpty()) {
        emit loginFailed(QStringLiteral("Username and password are required."));
        return;
    }

    QNetworkRequest request(kLoginUrl);
    applyCommonHeaders(request);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);
    request.setTransferTimeout(kApiTransferTimeoutMs);

    QJsonObject credentials;
    credentials.insert(QStringLiteral("username"), username.trimmed());
    credentials.insert(QStringLiteral("password"), password);
    credentials.insert(QStringLiteral("rememberMe"), true);

    QNetworkReply *reply =
        m_network.post(request,
                       QJsonDocument(credentials).toJson(QJsonDocument::Compact));
    const auto body = collectLimitedReply(reply, kMaximumLoginResponseBytes);

    connect(reply, &QNetworkReply::finished, this, [this, reply, body]() {
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool responseOk =
            finishLimitedReply(reply, body, kMaximumLoginResponseBytes);
        const auto networkError = reply->error();
        reply->deleteLater();

        if (!responseOk) {
            emit loginFailed(QStringLiteral("The login response was too large."));
            return;
        }

        if (networkError != QNetworkReply::NoError || status < 200 || status >= 300) {
            QString message = messageFromJson(*body);
            if (message.isEmpty())
                message = QStringLiteral("Login failed (HTTP %1).").arg(status);
            emit loginFailed(message);
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(*body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            emit loginFailed(QStringLiteral("The login response is not valid JSON."));
            return;
        }

        const QJsonObject root = document.object();
        if (!root.value(QStringLiteral("success")).toBool()) {
            QString message = root.value(QStringLiteral("message")).toString();
            if (message.isEmpty())
                message = QStringLiteral("Login was rejected.");
            emit loginFailed(message);
            return;
        }

        const QString token = root.value(QStringLiteral("token")).toString();
        if (token.isEmpty()) {
            emit loginFailed(QStringLiteral("Login succeeded, but the API returned no token."));
            return;
        }

        m_token = token;
        if (!saveToken(token)) {
            clearToken();
            emit loginFailed(QStringLiteral("Login succeeded, but the token could not be stored."));
            return;
        }

        emit loginSucceeded();
    });
}
