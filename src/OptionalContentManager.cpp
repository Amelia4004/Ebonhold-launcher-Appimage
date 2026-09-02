#include "OptionalContentManager.h"
#include "SafeFilesystem.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

OptionalContentManager::OptionalContentManager(QObject *parent)
    : QObject(parent),
      m_updater(new UpdateManager(this))
{
    connect(m_updater, &UpdateManager::updateProgress, this,
            [this](int percent, const QString &relativePath) {
                emit operationProgress(percent,
                                       relativePath.isEmpty()
                                           ? QStringLiteral("Installing HD patch...")
                                           : QStringLiteral("Downloading %1...").arg(relativePath));
            });
    connect(m_updater, &UpdateManager::updateFinished, this, [this]() {
        const QString title = titleForKind(m_activeKind);
        setBusy(false);
        emit operationFinished(QStringLiteral("%1 installed successfully.").arg(title));
        refresh();
    });
    connect(m_updater, &UpdateManager::updateFailed, this, [this](const QString &message) {
        setBusy(false);
        emit operationFailed(message);
    });
    connect(m_updater, &UpdateManager::authenticationExpired, this, [this]() {
        setBusy(false);
        emit authenticationExpired();
    });
}

void OptionalContentManager::setAuthToken(const QString &token)
{
    m_authToken = token;
    m_updater->setAuthToken(token);
}

void OptionalContentManager::setGameDirectory(const QString &gameDirectory)
{
    m_gameDirectory = QDir::cleanPath(gameDirectory);
}

void OptionalContentManager::setManifest(const QJsonObject &manifest)
{
    m_manifest = manifest;
}

bool OptionalContentManager::isBusy() const
{
    return m_busy;
}

void OptionalContentManager::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged(busy);
}

QString OptionalContentManager::pathForKind(HdPatchKind kind)
{
    switch (kind) {
    case HdPatchKind::CharacterWorld:
        return QStringLiteral("Data/patch-H.MPQ");
    case HdPatchKind::Creatures:
        return QStringLiteral("Data/patch-G.MPQ");
    }
    return {};
}

QString OptionalContentManager::titleForKind(HdPatchKind kind)
{
    switch (kind) {
    case HdPatchKind::CharacterWorld:
        return QStringLiteral("Character & World HD Patch");
    case HdPatchKind::Creatures:
        return QStringLiteral("Creatures HD Patch");
    }
    return QStringLiteral("HD Patch");
}

QString OptionalContentManager::descriptionForKind(HdPatchKind kind)
{
    switch (kind) {
    case HdPatchKind::CharacterWorld:
        return QStringLiteral("Required for the optional Character and World HD Patch.");
    case HdPatchKind::Creatures:
        return QStringLiteral("Required for the optional Creatures HD Patch.");
    }
    return {};
}

std::optional<PatchFile> OptionalContentManager::fileForKind(HdPatchKind kind) const
{
    return UpdateManager::findManifestFile(m_manifest, pathForKind(kind));
}

QString OptionalContentManager::destinationForKind(HdPatchKind kind) const
{
    QString destination;
    if (!SafeFilesystem::resolveDestination(m_gameDirectory,
                                            pathForKind(kind),
                                            false,
                                            &destination,
                                            nullptr)) {
        return {};
    }
    return destination;
}

HdPatchInfo OptionalContentManager::scanOne(const QString &gameDirectory,
                                             const QJsonObject &manifest,
                                             HdPatchKind kind)
{
    HdPatchInfo info;
    info.kind = kind;
    info.title = titleForKind(kind);
    info.description = descriptionForKind(kind);
    info.relativePath = pathForKind(kind);

    const auto manifestFile = UpdateManager::findManifestFile(manifest, info.relativePath);
    if (!manifestFile) {
        info.status = HdPatchStatus::Unavailable;
        info.error = QStringLiteral("This optional file is not present in the current manifest.");
        return info;
    }

    info.file = *manifestFile;
    QString destination;
    QString pathError;
    if (!SafeFilesystem::resolveDestination(gameDirectory,
                                            info.relativePath,
                                            false,
                                            &destination,
                                            &pathError)) {
        info.status = HdPatchStatus::Error;
        info.error = pathError.isEmpty()
                         ? QStringLiteral("Unsafe HD patch destination.")
                         : pathError;
        return info;
    }

    QFile file(destination);
    if (!file.exists()) {
        info.status = HdPatchStatus::NotInstalled;
        return info;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        info.status = HdPatchStatus::Error;
        info.error = QStringLiteral("Could not open the installed file for verification.");
        return info;
    }

    QCryptographicHash hash(QCryptographicHash::Md5);
    if (!hash.addData(&file)) {
        info.status = HdPatchStatus::Error;
        info.error = QStringLiteral("Could not calculate the installed file checksum.");
        return info;
    }

    const QByteArray actual = hash.result().toHex().toLower();
    info.status = actual == info.file.expectedMd5
                      ? HdPatchStatus::Installed
                      : HdPatchStatus::UpdateAvailable;
    return info;
}

void OptionalContentManager::refresh()
{
    if (m_busy) {
        emit operationFailed(QStringLiteral("Another optional-content operation is already running."));
        return;
    }
    if (m_gameDirectory.isEmpty() || !QDir(m_gameDirectory).exists()) {
        emit operationFailed(QStringLiteral("Select a valid WoW directory first."));
        return;
    }
    if (m_manifest.isEmpty()) {
        emit operationFailed(QStringLiteral("No current Ebonhold manifest is available."));
        return;
    }

    setBusy(true);
    emit operationProgress(0, QStringLiteral("Checking HD patches..."));

    const QString gameDirectory = m_gameDirectory;
    const QJsonObject manifest = m_manifest;
    auto *watcher = new QFutureWatcher<QVector<HdPatchInfo>>(this);
    connect(watcher, &QFutureWatcher<QVector<HdPatchInfo>>::finished, this,
            [this, watcher]() {
                const QVector<HdPatchInfo> patches = watcher->result();
                watcher->deleteLater();
                setBusy(false);
                emit operationProgress(100, QStringLiteral("HD patch status checked."));
                emit patchesReady(patches);
            });

    watcher->setFuture(QtConcurrent::run([gameDirectory, manifest]() {
        QVector<HdPatchInfo> result;
        result.push_back(scanOne(gameDirectory, manifest, HdPatchKind::CharacterWorld));
        result.push_back(scanOne(gameDirectory, manifest, HdPatchKind::Creatures));
        return result;
    }));
}

void OptionalContentManager::install(HdPatchKind kind)
{
    if (m_busy) {
        emit operationFailed(QStringLiteral("Another optional-content operation is already running."));
        return;
    }
    if (m_authToken.isEmpty()) {
        emit authenticationExpired();
        return;
    }
    const auto file = fileForKind(kind);
    if (!file) {
        emit operationFailed(QStringLiteral("%1 is not available in the current manifest.")
                                 .arg(titleForKind(kind)));
        return;
    }

    m_activeKind = kind;
    setBusy(true);
    emit operationProgress(0, QStringLiteral("Preparing %1...").arg(titleForKind(kind)));
    m_updater->setAuthToken(m_authToken);
    m_updater->installUpdates(m_gameDirectory, QVector<PatchFile>{*file}, QString());
}

void OptionalContentManager::remove(HdPatchKind kind)
{
    if (m_busy) {
        emit operationFailed(QStringLiteral("Another optional-content operation is already running."));
        return;
    }

    QString destination;
    QString pathError;
    if (!SafeFilesystem::resolveDestination(m_gameDirectory,
                                            pathForKind(kind),
                                            false,
                                            &destination,
                                            &pathError)) {
        emit operationFailed(pathError.isEmpty()
                                 ? QStringLiteral("Unsafe HD patch destination.")
                                 : pathError);
        return;
    }

    const QFileInfo info(destination);
    if (!info.exists()) {
        emit operationFinished(QStringLiteral("%1 is already not installed.").arg(titleForKind(kind)));
        refresh();
        return;
    }
    if (!info.isFile() || info.isSymLink()) {
        emit operationFailed(QStringLiteral("Refusing to remove an unsafe HD patch path."));
        return;
    }

    setBusy(true);
    const bool removed = QFile::remove(destination);
    setBusy(false);
    if (!removed) {
        emit operationFailed(QStringLiteral("Could not remove %1.").arg(destination));
        return;
    }

    emit operationFinished(QStringLiteral("%1 was removed.").arg(titleForKind(kind)));
    refresh();
}
