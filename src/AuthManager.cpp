#include "AuthManager.h"

#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrl>

namespace {
const QUrl kLoginUrl(QStringLiteral("https://api.project-ebonhold.com/api/auth/login"));
const QUrl kGamesUrl(QStringLiteral("https://api.project-ebonhold.com/api/launcher/games"));

void applyCommonHeaders(QNetworkRequest &request)
{
    request.setRawHeader("User-Agent", "EbonholdLauncher/1.0");
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("X-Client-Id", "EbonholdLauncher");
    request.setRawHeader("Origin", "https://project-ebonhold.com");
    request.setRawHeader("Referer", "https://project-ebonhold.com/download");
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
    QFile file(tokenFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    m_token = QString::fromUtf8(file.readAll()).trimmed();
    if (m_token == QStringLiteral("null"))
        m_token.clear();
}

bool AuthManager::saveToken(const QString &token)
{
    const QFileInfo info(tokenFilePath());
    QDir().mkpath(info.absolutePath());

    QFile file(tokenFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;

    if (file.write(token.toUtf8()) < 0)
        return false;

    file.close();
    QFile::setPermissions(tokenFilePath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
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
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const auto networkError = reply->error();
        reply->deleteLater();

        if (status == 401 || status == 403) {
            clearToken();
            emit loginRequired();
            return;
        }

        if (networkError != QNetworkReply::NoError || status < 200 || status >= 300) {
            QString message = messageFromJson(body);
            if (message.isEmpty())
                message = QStringLiteral("Manifest request failed (HTTP %1).").arg(status);
            emit requestFailed(message);
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
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
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("User-Agent", "EbonholdLauncher/1.0");

    QJsonObject credentials;
    credentials.insert(QStringLiteral("username"), username.trimmed());
    credentials.insert(QStringLiteral("password"), password);
    credentials.insert(QStringLiteral("rememberMe"), true);

    QNetworkReply *reply = m_network.post(request, QJsonDocument(credentials).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const auto networkError = reply->error();
        reply->deleteLater();

        if (networkError != QNetworkReply::NoError || status < 200 || status >= 300) {
            QString message = messageFromJson(body);
            if (message.isEmpty())
                message = QStringLiteral("Login failed (HTTP %1).").arg(status);
            emit loginFailed(message);
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
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
