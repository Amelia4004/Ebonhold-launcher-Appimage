#pragma once

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>

namespace SafeFilesystem {

inline void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

inline bool validateRelativePath(const QString &relativePath,
                                 QStringList *components,
                                 QString *error)
{
    if (relativePath.isEmpty() || QDir::isAbsolutePath(relativePath) ||
        relativePath.contains(QLatin1Char('\\'))) {
        setError(error, QStringLiteral("Unsafe path."));
        return false;
    }

    const QString cleaned = QDir::cleanPath(relativePath);
    if (cleaned == QStringLiteral(".") || cleaned == QStringLiteral("..") ||
        cleaned.startsWith(QStringLiteral("../"))) {
        setError(error, QStringLiteral("Unsafe path."));
        return false;
    }

    for (const QChar ch : relativePath) {
        if (ch.unicode() < 0x20) {
            setError(error, QStringLiteral("Unsafe path."));
            return false;
        }
    }

    *components = cleaned.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (components->isEmpty()) {
        setError(error, QStringLiteral("Unsafe path."));
        return false;
    }

    for (const QString &component : *components) {
        if (component == QStringLiteral(".") || component == QStringLiteral("..") ||
            component.isEmpty()) {
            setError(error, QStringLiteral("Unsafe path."));
            return false;
        }
    }

    return true;
}

inline bool canonicalRoot(const QString &rootDirectory,
                          QString *rootCanonical,
                          QString *error)
{
    const QFileInfo rootInfo(rootDirectory);
    if (!rootInfo.exists() || !rootInfo.isDir()) {
        setError(error, QStringLiteral("The selected game directory does not exist."));
        return false;
    }

    if (rootInfo.isSymLink()) {
        setError(error, QStringLiteral("Refusing a symlinked game directory."));
        return false;
    }

    *rootCanonical = rootInfo.canonicalFilePath();
    if (rootCanonical->isEmpty()) {
        setError(error, QStringLiteral("Could not resolve the selected game directory."));
        return false;
    }

    return true;
}

inline bool isInsideRoot(const QString &rootCanonical, const QString &candidate)
{
    const QString root = QDir::cleanPath(rootCanonical);
    const QString path = QDir::cleanPath(candidate);
    return path == root || path.startsWith(root + QDir::separator());
}

inline bool walkDirectories(const QString &rootCanonical,
                            const QStringList &components,
                            int count,
                            bool createMissing,
                            QString *directory,
                            QString *error)
{
    QString current = rootCanonical;

    for (int i = 0; i < count; ++i) {
        const QString next = QDir(current).filePath(components.at(i));
        QFileInfo info(next);

        if (info.isSymLink()) {
            setError(error,
                     QStringLiteral("Refusing path with symlinked directory '%1'.")
                         .arg(components.at(i)));
            return false;
        }

        if (!info.exists()) {
            if (!createMissing) {
                for (int j = i; j < count; ++j)
                    current = QDir(current).filePath(components.at(j));

                if (!isInsideRoot(rootCanonical, current)) {
                    setError(error, QStringLiteral("Path escapes the selected game directory."));
                    return false;
                }

                *directory = QDir::cleanPath(current);
                return true;
            }

            QDir parent(current);
            if (!parent.mkdir(components.at(i))) {
                setError(error,
                         QStringLiteral("Could not create directory '%1'.")
                             .arg(components.at(i)));
                return false;
            }

            info.setFile(next);
            if (info.isSymLink() || !info.exists() || !info.isDir()) {
                setError(error,
                         QStringLiteral("Directory '%1' changed while preparing the path.")
                             .arg(components.at(i)));
                return false;
            }
        } else if (!info.isDir()) {
            setError(error,
                     QStringLiteral("Path component '%1' is not a directory.")
                         .arg(components.at(i)));
            return false;
        }

        const QString canonical = info.canonicalFilePath();
        if (canonical.isEmpty() || !isInsideRoot(rootCanonical, canonical)) {
            setError(error, QStringLiteral("Path escapes the selected game directory."));
            return false;
        }

        current = canonical;
    }

    *directory = QDir::cleanPath(current);
    return true;
}

inline bool resolveDestination(const QString &rootDirectory,
                               const QString &relativePath,
                               bool createParentDirectories,
                               QString *destination,
                               QString *error = nullptr)
{
    QStringList components;
    if (!validateRelativePath(relativePath, &components, error))
        return false;

    QString rootCanonical;
    if (!canonicalRoot(rootDirectory, &rootCanonical, error))
        return false;

    QString parent;
    if (!walkDirectories(rootCanonical,
                         components,
                         components.size() - 1,
                         createParentDirectories,
                         &parent,
                         error)) {
        return false;
    }

    const QString candidate = QDir(parent).filePath(components.constLast());
    const QFileInfo finalInfo(candidate);

    if (finalInfo.isSymLink()) {
        setError(error, QStringLiteral("Refusing a symlinked destination."));
        return false;
    }

    QString resolved = QDir::cleanPath(candidate);
    if (finalInfo.exists()) {
        const QString canonical = finalInfo.canonicalFilePath();
        if (canonical.isEmpty() || !isInsideRoot(rootCanonical, canonical)) {
            setError(error, QStringLiteral("Destination escapes the selected game directory."));
            return false;
        }
        resolved = canonical;
    } else if (!isInsideRoot(rootCanonical, resolved)) {
        setError(error, QStringLiteral("Destination escapes the selected game directory."));
        return false;
    }

    *destination = resolved;
    return true;
}

inline bool ensureDirectory(const QString &rootDirectory,
                            const QString &relativePath,
                            QString *directory,
                            QString *error = nullptr)
{
    QStringList components;
    if (!validateRelativePath(relativePath, &components, error))
        return false;

    QString rootCanonical;
    if (!canonicalRoot(rootDirectory, &rootCanonical, error))
        return false;

    return walkDirectories(rootCanonical,
                           components,
                           components.size(),
                           true,
                           directory,
                           error);
}

} // namespace SafeFilesystem
