#include "OptionalContentDialog.h"
#include "SafeFilesystem.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QDir>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

namespace {
QWidget *buttonContainer(QWidget *parent, const QList<QPushButton *> &buttons)
{
    auto *container = new QWidget(parent);
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    for (QPushButton *button : buttons)
        layout->addWidget(button);
    layout->addStretch();
    return container;
}
}

OptionalContentDialog::OptionalContentDialog(const QString &gameDirectory,
                                             const QString &authToken,
                                             const QJsonObject &manifest,
                                             QWidget *parent)
    : QDialog(parent),
      m_gameDirectory(QDir::cleanPath(gameDirectory)),
      m_addons(new AddonManager(this)),
      m_hdPatches(new OptionalContentManager(this))
{
    m_addons->setGameDirectory(m_gameDirectory);
    m_addons->setAuthToken(authToken);
    m_hdPatches->setGameDirectory(m_gameDirectory);
    m_hdPatches->setAuthToken(authToken);
    m_hdPatches->setManifest(manifest);

    buildUi();

    connect(m_addons, &AddonManager::catalogReady,
            this, &OptionalContentDialog::rebuildAddonTable);
    connect(m_hdPatches, &OptionalContentManager::patchesReady,
            this, &OptionalContentDialog::rebuildHdTable);

    connect(m_addons, &AddonManager::busyChanged, this,
            [this](bool) { updateControls(); });
    connect(m_hdPatches, &OptionalContentManager::busyChanged, this,
            [this](bool) { updateControls(); });

    auto progressHandler = [this](int percent, const QString &text) {
        m_progress->setRange(0, 100);
        m_progress->setValue(percent);
        m_operationText->setText(text);
    };
    connect(m_addons, &AddonManager::operationProgress, this, progressHandler);
    connect(m_hdPatches, &OptionalContentManager::operationProgress, this, progressHandler);

    auto finishedHandler = [this](const QString &message) {
        m_progress->setRange(0, 100);
        m_progress->setValue(100);
        m_operationText->setText(message);
        updateControls();
    };
    connect(m_addons, &AddonManager::operationFinished, this, finishedHandler);
    connect(m_hdPatches, &OptionalContentManager::operationFinished, this, finishedHandler);

    connect(m_addons, &AddonManager::operationFailed,
            this, &OptionalContentDialog::showOperationError);
    connect(m_hdPatches, &OptionalContentManager::operationFailed,
            this, &OptionalContentDialog::showOperationError);

    auto expired = [this]() {
        emit authenticationExpired();
        reject();
    };
    connect(m_addons, &AddonManager::authenticationExpired, this, expired);
    connect(m_hdPatches, &OptionalContentManager::authenticationExpired, this, expired);

    connect(m_refreshButton, &QPushButton::clicked,
            this, &OptionalContentDialog::refreshAll);
    connect(m_updateAllButton, &QPushButton::clicked, this, [this]() {
        QVector<int> updates;
        for (const AddonInfo &addon : m_currentAddons) {
            if (addon.status == AddonStatus::UpdateAvailable)
                updates.push_back(addon.id);
        }
        if (!updates.isEmpty())
            m_addons->installAddons(updates);
    });
    connect(m_openAddonFolderButton, &QPushButton::clicked,
            this, &OptionalContentDialog::openAddonFolder);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::accept);

    refreshAll();
}

void OptionalContentDialog::buildUi()
{
    setWindowTitle(QStringLiteral("Optional Content"));
    resize(920, 620);

    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(
        QStringLiteral("Optional content is installed only when you explicitly choose it here. "
                       "Normal updates and Full Repair do not install or remove these files."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *tabs = new QTabWidget(this);

    auto *addonsPage = new QWidget(tabs);
    auto *addonsLayout = new QVBoxLayout(addonsPage);
    auto *addonsInfo = new QLabel(
        QStringLiteral("Officially offered Ebonhold AddOns. The launcher tracks only AddOns installed or adopted through this manager."),
        addonsPage);
    addonsInfo->setWordWrap(true);
    addonsLayout->addWidget(addonsInfo);

    m_addonTable = new QTableWidget(0, 4, addonsPage);
    m_addonTable->setHorizontalHeaderLabels(
        {QStringLiteral("AddOn"), QStringLiteral("Description"),
         QStringLiteral("Status"), QStringLiteral("Actions")});
    m_addonTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_addonTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_addonTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_addonTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_addonTable->verticalHeader()->setVisible(false);
    m_addonTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_addonTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_addonTable->setWordWrap(true);
    addonsLayout->addWidget(m_addonTable, 1);

    auto *addonButtons = new QHBoxLayout;
    m_updateAllButton = new QPushButton(QStringLiteral("Update All"), addonsPage);
    m_openAddonFolderButton = new QPushButton(QStringLiteral("Open AddOn Folder"), addonsPage);
    addonButtons->addWidget(m_updateAllButton);
    addonButtons->addWidget(m_openAddonFolderButton);
    addonButtons->addStretch();
    addonsLayout->addLayout(addonButtons);
    tabs->addTab(addonsPage, QStringLiteral("Official AddOns"));

    auto *hdPage = new QWidget(tabs);
    auto *hdLayout = new QVBoxLayout(hdPage);
    auto *hdInfo = new QLabel(
        QStringLiteral("HD patches are optional game files from the normal Ebonhold manifest. "
                       "They are checksum-verified using the same download path as regular patches."),
        hdPage);
    hdInfo->setWordWrap(true);
    hdLayout->addWidget(hdInfo);

    m_hdTable = new QTableWidget(0, 4, hdPage);
    m_hdTable->setHorizontalHeaderLabels(
        {QStringLiteral("HD Patch"), QStringLiteral("File"),
         QStringLiteral("Status"), QStringLiteral("Actions")});
    m_hdTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_hdTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_hdTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_hdTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_hdTable->verticalHeader()->setVisible(false);
    m_hdTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_hdTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_hdTable->setWordWrap(true);
    hdLayout->addWidget(m_hdTable, 1);
    tabs->addTab(hdPage, QStringLiteral("HD Patches"));

    layout->addWidget(tabs, 1);

    m_operationText = new QLabel(QStringLiteral("Ready"), this);
    m_operationText->setWordWrap(true);
    layout->addWidget(m_operationText);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    layout->addWidget(m_progress);

    auto *bottom = new QHBoxLayout;
    m_refreshButton = new QPushButton(QStringLiteral("Refresh"), this);
    m_closeButton = new QPushButton(QStringLiteral("Close"), this);
    bottom->addWidget(m_refreshButton);
    bottom->addStretch();
    bottom->addWidget(m_closeButton);
    layout->addLayout(bottom);
}

QString OptionalContentDialog::addonStatusText(AddonStatus status)
{
    switch (status) {
    case AddonStatus::NotInstalled:
        return QStringLiteral("Not installed");
    case AddonStatus::Detected:
        return QStringLiteral("Detected (not managed)");
    case AddonStatus::Installed:
        return QStringLiteral("Up to date");
    case AddonStatus::UpdateAvailable:
        return QStringLiteral("Update available");
    }
    return QStringLiteral("Unknown");
}

QString OptionalContentDialog::hdStatusText(HdPatchStatus status)
{
    switch (status) {
    case HdPatchStatus::Unavailable:
        return QStringLiteral("Unavailable");
    case HdPatchStatus::NotInstalled:
        return QStringLiteral("Not installed");
    case HdPatchStatus::Installed:
        return QStringLiteral("Installed");
    case HdPatchStatus::UpdateAvailable:
        return QStringLiteral("Update available");
    case HdPatchStatus::Error:
        return QStringLiteral("Verification error");
    }
    return QStringLiteral("Unknown");
}

QString OptionalContentDialog::formatBytes(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KiB").arg(bytes / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MiB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

void OptionalContentDialog::rebuildAddonTable(const QVector<AddonInfo> &addons)
{
    m_currentAddons = addons;
    m_addonTable->setRowCount(0);

    for (const AddonInfo &addon : addons) {
        const int row = m_addonTable->rowCount();
        m_addonTable->insertRow(row);

        auto *name = new QTableWidgetItem(addon.name);
        QString details = addon.description;
        if (addon.fileSizeBytes > 0)
            details += QStringLiteral("\n%1").arg(formatBytes(addon.fileSizeBytes));
        if (addon.updatedAt.isValid())
            details += QStringLiteral(" · Updated %1")
                           .arg(addon.updatedAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        auto *description = new QTableWidgetItem(details);
        auto *status = new QTableWidgetItem(addonStatusText(addon.status));

        m_addonTable->setItem(row, 0, name);
        m_addonTable->setItem(row, 1, description);
        m_addonTable->setItem(row, 2, status);

        QList<QPushButton *> buttons;
        if (addon.status == AddonStatus::NotInstalled || addon.status == AddonStatus::Detected) {
            auto *install = new QPushButton(QStringLiteral("Install"), m_addonTable);
            if (addon.status == AddonStatus::Detected)
                install->setToolTip(QStringLiteral("Install the official copy and let this launcher manage it."));
            connect(install, &QPushButton::clicked, this,
                    [this, id = addon.id]() { m_addons->installAddons(QVector<int>{id}); });
            buttons.push_back(install);
        } else {
            auto *install = new QPushButton(
                addon.status == AddonStatus::UpdateAvailable
                    ? QStringLiteral("Update")
                    : QStringLiteral("Reinstall"),
                m_addonTable);
            connect(install, &QPushButton::clicked, this,
                    [this, id = addon.id]() { m_addons->installAddons(QVector<int>{id}); });
            buttons.push_back(install);

            auto *remove = new QPushButton(QStringLiteral("Remove"), m_addonTable);
            connect(remove, &QPushButton::clicked, this, [this, addon]() {
                const auto answer = QMessageBox::question(
                    this, QStringLiteral("Remove AddOn"),
                    QStringLiteral("Remove %1 and the folders tracked for this AddOn?")
                        .arg(addon.name),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (answer == QMessageBox::Yes)
                    m_addons->removeAddon(addon.id);
            });
            buttons.push_back(remove);
        }

        m_addonTable->setCellWidget(row, 3, buttonContainer(m_addonTable, buttons));
        m_addonTable->setRowHeight(row, 78);
    }
    updateControls();
}

void OptionalContentDialog::rebuildHdTable(const QVector<HdPatchInfo> &patches)
{
    m_currentPatches = patches;
    m_hdTable->setRowCount(0);

    for (const HdPatchInfo &patch : patches) {
        const int row = m_hdTable->rowCount();
        m_hdTable->insertRow(row);

        QString title = patch.title;
        if (!patch.description.isEmpty())
            title += QStringLiteral("\n%1").arg(patch.description);
        if (!patch.error.isEmpty())
            title += QStringLiteral("\n%1").arg(patch.error);

        m_hdTable->setItem(row, 0, new QTableWidgetItem(title));
        m_hdTable->setItem(row, 1, new QTableWidgetItem(patch.relativePath));
        m_hdTable->setItem(row, 2, new QTableWidgetItem(hdStatusText(patch.status)));

        QList<QPushButton *> buttons;
        if (patch.status != HdPatchStatus::Unavailable) {
            auto *install = new QPushButton(
                patch.status == HdPatchStatus::NotInstalled
                    ? QStringLiteral("Install")
                    : patch.status == HdPatchStatus::UpdateAvailable
                          ? QStringLiteral("Update")
                          : QStringLiteral("Reinstall"),
                m_hdTable);
            connect(install, &QPushButton::clicked, this,
                    [this, kind = patch.kind]() { m_hdPatches->install(kind); });
            buttons.push_back(install);

            if (patch.status == HdPatchStatus::Installed ||
                patch.status == HdPatchStatus::UpdateAvailable ||
                patch.status == HdPatchStatus::Error) {
                auto *remove = new QPushButton(QStringLiteral("Remove"), m_hdTable);
                connect(remove, &QPushButton::clicked, this, [this, patch]() {
                    const auto answer = QMessageBox::question(
                        this, QStringLiteral("Remove HD Patch"),
                        QStringLiteral("Remove %1?\n\n%2")
                            .arg(patch.title, patch.relativePath),
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                    if (answer == QMessageBox::Yes)
                        m_hdPatches->remove(patch.kind);
                });
                buttons.push_back(remove);
            }
        }

        m_hdTable->setCellWidget(row, 3, buttonContainer(m_hdTable, buttons));
        m_hdTable->setRowHeight(row, 78);
    }
    updateControls();
}

void OptionalContentDialog::refreshAll()
{
    m_operationText->setText(QStringLiteral("Refreshing optional content..."));
    m_progress->setRange(0, 0);
    m_addons->refreshCatalog();
    m_hdPatches->refresh();
    updateControls();
}

void OptionalContentDialog::updateControls()
{
    const bool busy = m_addons->isBusy() || m_hdPatches->isBusy();
    m_refreshButton->setEnabled(!busy);
    m_closeButton->setEnabled(!busy);
    m_openAddonFolderButton->setEnabled(!busy);

    bool updatesAvailable = false;
    for (const AddonInfo &addon : m_currentAddons) {
        if (addon.status == AddonStatus::UpdateAvailable) {
            updatesAvailable = true;
            break;
        }
    }
    m_updateAllButton->setEnabled(!busy && updatesAvailable);
    m_addonTable->setEnabled(!busy);
    m_hdTable->setEnabled(!busy);

    if (!busy && m_progress->maximum() == 0) {
        m_progress->setRange(0, 100);
        m_progress->setValue(100);
    }
}

void OptionalContentDialog::openAddonFolder()
{
    QString directory;
    QString pathError;
    if (!SafeFilesystem::ensureDirectory(m_gameDirectory,
                                         QStringLiteral("Interface/AddOns"),
                                         &directory,
                                         &pathError)) {
        QMessageBox::warning(this,
                             QStringLiteral("AddOn Folder"),
                             pathError.isEmpty()
                                 ? QStringLiteral("Could not safely create the AddOn folder.")
                                 : pathError);
        return;
    }

    QString program = QStandardPaths::findExecutable(QStringLiteral("xdg-open"));
    QStringList arguments;
    if (!program.isEmpty()) {
        arguments = {directory};
    } else {
        program = QStandardPaths::findExecutable(QStringLiteral("gio"));
        if (!program.isEmpty())
            arguments = {QStringLiteral("open"), directory};
    }

    if (program.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("AddOn Folder"),
                             QStringLiteral("No desktop folder opener (xdg-open or gio) was found."));
        return;
    }

    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setWorkingDirectory(m_gameDirectory);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    if (environment.contains(QStringLiteral("APPIMAGE")) ||
        environment.contains(QStringLiteral("APPDIR"))) {
        environment.remove(QStringLiteral("LD_LIBRARY_PATH"));
        environment.remove(QStringLiteral("LD_PRELOAD"));
        environment.remove(QStringLiteral("QT_PLUGIN_PATH"));
        environment.remove(QStringLiteral("QML2_IMPORT_PATH"));
        environment.remove(QStringLiteral("QML_IMPORT_PATH"));
    }
    process.setProcessEnvironment(environment);
    if (!process.startDetached()) {
        QMessageBox::warning(this, QStringLiteral("AddOn Folder"),
                             QStringLiteral("Could not open the AddOn folder."));
    }
}

void OptionalContentDialog::showOperationError(const QString &message)
{
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_operationText->setText(QStringLiteral("Operation failed"));
    updateControls();
    QMessageBox::critical(this, QStringLiteral("Optional Content"), message);
}
