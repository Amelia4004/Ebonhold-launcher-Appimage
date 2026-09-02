#include "MainWindow.h"

#include "AuthManager.h"
#include "UpdateManager.h"
#include "OptionalContentDialog.h"
#include "SafeFilesystem.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      m_auth(new AuthManager(this)),
      m_updater(new UpdateManager(this))
{
    buildUi();
    loadSettings();

    connect(m_browseButton, &QPushButton::clicked, this, [this]() {
        const QString directory = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Select Ebonhold / WoW directory"), m_gamePath->text());
        if (!directory.isEmpty()) {
            m_gamePath->setText(directory);
            saveSettings();
            resetUpdateState();
        }
    });

    connect(m_gamePath, &QLineEdit::editingFinished, this, [this]() {
        saveSettings();
        const QString current = QDir::cleanPath(m_gamePath->text().trimmed());
        if (!m_scannedGameDirectory.isEmpty() && current != m_scannedGameDirectory)
            resetUpdateState();
    });

    connect(m_checkButton, &QPushButton::clicked, this, [this]() {
        if (!m_pendingUpdates.isEmpty())
            startUpdate();
        else
            startCheck();
    });

    connect(m_fullRepairButton, &QPushButton::clicked, this, [this]() {
        startCheck(true);
    });

    connect(m_launcherScriptsButton, &QPushButton::clicked,
            this, &MainWindow::showLauncherScriptsDialog);

    connect(m_optionalContentButton, &QPushButton::clicked,
            this, &MainWindow::requestOptionalContent);

    connect(m_addonFolderButton, &QPushButton::clicked, this, [this]() {
        const QString gameDirectory = QDir::cleanPath(m_gamePath->text().trimmed());
        if (gameDirectory.isEmpty() || !QDir(gameDirectory).exists()) {
            QMessageBox::warning(this, QStringLiteral("AddOn Folder"),
                                 QStringLiteral("Select a valid WoW directory first."));
            return;
        }

        const QStringList candidates = {
            QStringLiteral("Interface/AddOns"),
            QStringLiteral("interface/AddOns"),
            QStringLiteral("Interface/addons"),
            QStringLiteral("interface/addons")
        };

        QString addonDirectory;
        for (const QString &relative : candidates) {
            const QString candidate = QDir(gameDirectory).filePath(relative);
            if (QFileInfo(candidate).isDir()) {
                addonDirectory = candidate;
                break;
            }
        }

        if (addonDirectory.isEmpty()) {
            QMessageBox::warning(
                this, QStringLiteral("AddOn Folder"),
                QStringLiteral("The AddOn folder was not found.\n\nExpected location:\n%1")
                    .arg(QDir(gameDirectory).filePath(QStringLiteral("Interface/AddOns"))));
            return;
        }

        QString opener = QStandardPaths::findExecutable(QStringLiteral("xdg-open"));
        QStringList arguments;
        if (!opener.isEmpty()) {
            arguments = {addonDirectory};
        } else {
            opener = QStandardPaths::findExecutable(QStringLiteral("gio"));
            if (!opener.isEmpty())
                arguments = {QStringLiteral("open"), addonDirectory};
        }

        if (opener.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("AddOn Folder"),
                                 QStringLiteral("No desktop folder opener (xdg-open or gio) was found."));
            return;
        }

        QProcess process;
        process.setProgram(opener);
        process.setArguments(arguments);
        process.setWorkingDirectory(gameDirectory);

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
            return;
        }

        appendLog(QStringLiteral("Opened AddOn folder: %1").arg(addonDirectory));
    });

    connect(m_playButton, &QPushButton::clicked, this, [this]() {
        const QString gameDirectory = QDir::cleanPath(m_gamePath->text().trimmed());
        const QString wowPath = QDir(gameDirectory).filePath(QStringLiteral("Wow.exe"));
        if (!QFileInfo::exists(wowPath)) {
            QMessageBox::warning(this, QStringLiteral("Ebonhold Updater"),
                                 QStringLiteral("Wow.exe was not found in the selected directory."));
            return;
        }

        auto startExternal = [&gameDirectory](const QString &program, const QStringList &arguments) {
            QProcess process;
            process.setProgram(program);
            process.setArguments(arguments);
            process.setWorkingDirectory(gameDirectory);

            // AppImage sets its own library and plugin paths.
            // Remove them here so Protontricks/Wine uses the host libraries.
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
            return process.startDetached();
        };

        // KDE/KIO blocks executable files through QDesktopServices.
        // Use the Protontricks launcher so the prefix selector still opens.
        const QString protontricks = QStandardPaths::findExecutable(QStringLiteral("protontricks-launch"));
        if (!protontricks.isEmpty()) {
            if (startExternal(protontricks, {QStringLiteral("--no-term"), wowPath})) {
                appendLog(QStringLiteral("Launching Wow.exe with Protontricks prefix selector."));
                return;
            }
        }

        // If Protontricks is missing, use Wine directly.
        // QDesktopServices would run into the same KIO restriction.
        const QString wine = QStandardPaths::findExecutable(QStringLiteral("wine"));
        if (!wine.isEmpty()) {
            if (startExternal(wine, {wowPath})) {
                appendLog(QStringLiteral("Launching Wow.exe with Wine."));
                return;
            }
        }

        QMessageBox::warning(
            this, QStringLiteral("Ebonhold Updater"),
            QStringLiteral("Could not start Wow.exe. Neither protontricks-launch nor wine could be started.\n\n"
                           "Install/configure Protontricks or Wine, or start Wow.exe using your preferred runner."));
    });

    connect(m_auth, &AuthManager::loginRequired, this, &MainWindow::showLoginDialog);
    connect(m_auth, &AuthManager::loginSucceeded, this, [this]() {
        appendLog(QStringLiteral("API login successful."));
        m_updater->setAuthToken(m_auth->token());
        m_auth->fetchGamesManifest();
    });
    connect(m_auth, &AuthManager::loginFailed, this, [this](const QString &message) {
        m_optionalContentRequested = false;
        setBusy(false);
        m_status->setText(QStringLiteral("Login failed"));
        QMessageBox::critical(this, QStringLiteral("Ebonhold Login"), message);
    });
    connect(m_auth, &AuthManager::requestFailed, this, [this](const QString &message) {
        m_optionalContentRequested = false;
        setBusy(false);
        m_status->setText(QStringLiteral("API error"));
        appendLog(message);
        QMessageBox::critical(this, QStringLiteral("Ebonhold API"), message);
    });
    connect(m_auth, &AuthManager::manifestReady, this, [this](const QJsonObject &manifest) {
        m_manifest = manifest;
        m_updater->setAuthToken(m_auth->token());

        if (m_optionalContentRequested) {
            m_optionalContentRequested = false;
            setBusy(false);
            m_status->setText(QStringLiteral("Optional content ready"));
            showOptionalContentDialog();
            return;
        }

        appendLog(QStringLiteral("Manifest received. Checking local files..."));
        m_updater->scan(m_gamePath->text(), m_manifest);
    });

    connect(m_updater, &UpdateManager::scanStarted, this, [this](int fileCount) {
        m_progress->setRange(0, 0);
        m_status->setText(QStringLiteral("Checking %1 files...").arg(fileCount));
    });
    connect(m_updater, &UpdateManager::scanFailed, this, [this](const QString &message) {
        resetUpdateState();
        setBusy(false);
        m_status->setText(QStringLiteral("Check failed"));
        appendLog(message);
        QMessageBox::critical(this, QStringLiteral("Update check"), message);
    });
    connect(m_updater, &UpdateManager::scanFinished, this,
            [this](const QVector<PatchScanResult> &results, const QString &realmlist) {
                int current = 0;
                int missing = 0;
                int mismatched = 0;
                int errors = 0;
                QVector<PatchFile> allRequiredFiles;
                allRequiredFiles.reserve(results.size());

                m_pendingUpdates.clear();
                m_realmlist = realmlist;

                for (const PatchScanResult &result : results) {
                    // These are already validated required files from the manifest.
                    // Full Repair downloads all of them again regardless of the local state.
                    allRequiredFiles.push_back(result.file);

                    if (!result.error.isEmpty()) {
                        ++errors;
                        appendLog(QStringLiteral("ERROR: %1 (%2)")
                                      .arg(result.file.relativePath, result.error));
                    } else if (!result.exists) {
                        ++missing;
                        m_pendingUpdates.push_back(result.file);
                        appendLog(QStringLiteral("MISSING: %1").arg(result.file.relativePath));
                    } else if (!result.matches) {
                        ++mismatched;
                        m_pendingUpdates.push_back(result.file);
                        appendLog(QStringLiteral("OUTDATED: %1").arg(result.file.relativePath));
                    } else {
                        ++current;
                    }
                }

                m_progress->setRange(0, 100);
                m_progress->setValue(100);
                setBusy(false);

                appendLog(QStringLiteral("Summary: %1 current, %2 missing, %3 outdated, %4 errors.")
                              .arg(current).arg(missing).arg(mismatched).arg(errors));
                if (!realmlist.isEmpty())
                    appendLog(QStringLiteral("Realm: %1").arg(realmlist));

                // Full Repair downloads every required file again.
                // Optional files are left alone.
                if (m_fullRepairRequested) {
                    m_fullRepairRequested = false;

                    if (allRequiredFiles.isEmpty()) {
                        m_status->setText(QStringLiteral("Full repair failed"));
                        QMessageBox::critical(this, QStringLiteral("Full Repair"),
                                              QStringLiteral("The manifest contains no required files to repair."));
                        return;
                    }

                    m_pendingUpdates = allRequiredFiles;
                    m_checkButton->setText(QStringLiteral("Check for updates"));
                    m_status->setText(QStringLiteral("Full repair: %1 file(s) queued")
                                          .arg(m_pendingUpdates.size()));
                    appendLog(QStringLiteral("FULL REPAIR: Redownloading all %1 required file(s).")
                                  .arg(m_pendingUpdates.size()));
                    startUpdate();
                    return;
                }

                const int updates = missing + mismatched;
                if (errors > 0) {
                    // Do not overwrite a file during a normal update if its scan failed.
                    // Full Repair can still force a clean download.
                    m_pendingUpdates.clear();
                    m_checkButton->setText(QStringLiteral("Check for updates"));
                    m_status->setText(QStringLiteral("Check completed with %1 error(s)").arg(errors));
                } else if (updates == 0) {
                    m_checkButton->setText(QStringLiteral("Check for updates"));
                    m_status->setText(QStringLiteral("Client is up to date"));
                } else {
                    m_checkButton->setText(QStringLiteral("Update (%1)").arg(updates));
                    m_status->setText(QStringLiteral("%1 update(s) required").arg(updates));
                }
            });

    connect(m_updater, &UpdateManager::updateStarted, this, [this](int fileCount) {
        setBusy(true);
        m_progress->setRange(0, 100);
        m_progress->setValue(0);
        m_status->setText(QStringLiteral("Updating %1 file(s)...").arg(fileCount));
        appendLog(QStringLiteral("Starting update of %1 file(s)...").arg(fileCount));
    });

    connect(m_updater, &UpdateManager::updateFileStarted, this,
            [this](int index, int total, const QString &relativePath) {
                m_status->setText(QStringLiteral("Downloading %1 (%2/%3)")
                                      .arg(relativePath)
                                      .arg(index)
                                      .arg(total));
                appendLog(QStringLiteral("DOWNLOADING [%1/%2]: %3")
                              .arg(index)
                              .arg(total)
                              .arg(relativePath));
            });

    connect(m_updater, &UpdateManager::updateProgress, this,
            [this](int percent, const QString &) {
                m_progress->setRange(0, 100);
                m_progress->setValue(percent);
            });

    connect(m_updater, &UpdateManager::updateFileFinished, this,
            [this](int index, int total, const QString &relativePath) {
                appendLog(QStringLiteral("UPDATED [%1/%2]: %3")
                              .arg(index)
                              .arg(total)
                              .arg(relativePath));
            });

    connect(m_updater, &UpdateManager::updateFinished, this, [this]() {
        m_pendingUpdates.clear();
        m_checkButton->setText(QStringLiteral("Check for updates"));
        m_progress->setValue(100);
        m_status->setText(QStringLiteral("Update complete. Verifying files..."));
        appendLog(QStringLiteral("Update complete. Verifying local files..."));

        // Keep the controls disabled until the verification is done.
        m_updater->scan(m_gamePath->text(), m_manifest);
    });

    connect(m_updater, &UpdateManager::updateFailed, this, [this](const QString &message) {
        setBusy(false);
        m_checkButton->setText(m_pendingUpdates.isEmpty()
                                   ? QStringLiteral("Check for updates")
                                   : QStringLiteral("Retry update (%1)").arg(m_pendingUpdates.size()));
        m_status->setText(QStringLiteral("Update failed"));
        appendLog(QStringLiteral("UPDATE FAILED: %1").arg(message));
        QMessageBox::critical(this, QStringLiteral("Ebonhold Update"), message);
    });

    connect(m_updater, &UpdateManager::authenticationExpired, this, [this]() {
        m_optionalContentRequested = false;
        m_auth->clearToken();
        m_updater->setAuthToken(QString());
        setBusy(false);
        m_status->setText(QStringLiteral("Session expired"));
        appendLog(QStringLiteral("API session expired. Please log in again."));

        // Get a fresh login and manifest before trying the update again.
        m_auth->fetchGamesManifest();
    });
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("Ebonhold Updater"));
    resize(900, 500);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);

    auto *title = new QLabel(QStringLiteral("Ebonhold Updater"), central);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 6);
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    auto *pathLayout = new QHBoxLayout;
    m_gamePath = new QLineEdit(central);
    m_gamePath->setPlaceholderText(QStringLiteral("/home/user/Games/Ebonhold"));
    m_browseButton = new QPushButton(QStringLiteral("Browse..."), central);
    pathLayout->addWidget(m_gamePath, 1);
    pathLayout->addWidget(m_browseButton);
    layout->addLayout(pathLayout);

    m_status = new QLabel(QStringLiteral("Ready"), central);
    layout->addWidget(m_status);

    m_progress = new QProgressBar(central);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    layout->addWidget(m_progress);

    m_log = new QPlainTextEdit(central);
    m_log->setReadOnly(true);
    m_log->setPlaceholderText(QStringLiteral("Update status will appear here."));
    layout->addWidget(m_log, 1);

    auto *buttonLayout = new QHBoxLayout;
    m_checkButton = new QPushButton(QStringLiteral("Check for updates"), central);
    m_fullRepairButton = new QPushButton(QStringLiteral("Full repair"), central);
    m_fullRepairButton->setToolTip(
        QStringLiteral("Redownload every required file from the current manifest."));
    m_launcherScriptsButton = new QPushButton(QStringLiteral("Launcher scripts..."), central);
    m_launcherScriptsButton->setToolTip(
        QStringLiteral("Create optional .sh launchers for Protontricks, Wine or Lutris."));
    m_optionalContentButton = new QPushButton(QStringLiteral("Optional Content..."), central);
    m_optionalContentButton->setToolTip(
        QStringLiteral("Manage official AddOns and optional HD patches."));
    m_addonFolderButton = new QPushButton(QStringLiteral("AddOn Folder"), central);
    m_addonFolderButton->setToolTip(
        QStringLiteral("Open the WoW Interface/AddOns folder in your file manager."));
    m_playButton = new QPushButton(QStringLiteral("Play"), central);
    buttonLayout->addWidget(m_checkButton);
    buttonLayout->addWidget(m_fullRepairButton);
    buttonLayout->addWidget(m_launcherScriptsButton);
    buttonLayout->addWidget(m_optionalContentButton);
    buttonLayout->addWidget(m_addonFolderButton);
    buttonLayout->addStretch();
    buttonLayout->addWidget(m_playButton);
    layout->addLayout(buttonLayout);

    setCentralWidget(central);
}

void MainWindow::loadSettings()
{
    QSettings settings;
    m_gamePath->setText(settings.value(QStringLiteral("gamePath")).toString());
}

void MainWindow::saveSettings()
{
    QSettings settings;
    settings.setValue(QStringLiteral("gamePath"), m_gamePath->text());
}

void MainWindow::resetUpdateState()
{
    if (m_busy)
        return;

    m_pendingUpdates.clear();
    m_fullRepairRequested = false;
    m_optionalContentRequested = false;
    m_realmlist.clear();
    m_scannedGameDirectory.clear();
    m_manifest = QJsonObject();
    m_checkButton->setText(QStringLiteral("Check for updates"));
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_status->setText(QStringLiteral("Ready"));
}

void MainWindow::setBusy(bool busy)
{
    m_busy = busy;
    m_gamePath->setEnabled(!busy);
    m_browseButton->setEnabled(!busy);
    m_checkButton->setEnabled(!busy);
    m_fullRepairButton->setEnabled(!busy);
    m_launcherScriptsButton->setEnabled(!busy);
    m_optionalContentButton->setEnabled(!busy);
    m_addonFolderButton->setEnabled(!busy);
    m_playButton->setEnabled(!busy);
}

void MainWindow::appendLog(const QString &text)
{
    m_log->appendPlainText(text);
}

void MainWindow::startCheck(bool fullRepair)
{
    const QString gameDirectory = QDir::cleanPath(m_gamePath->text().trimmed());
    if (gameDirectory.isEmpty() || !QDir(gameDirectory).exists()) {
        QMessageBox::warning(this, QStringLiteral("Ebonhold Updater"),
                             QStringLiteral("Select a valid WoW directory first."));
        return;
    }

    const QString wowPath = QDir(gameDirectory).filePath(QStringLiteral("Wow.exe"));
    if (!QFileInfo::exists(wowPath)) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Wow.exe not found"),
            QStringLiteral("Wow.exe was not found in this directory. Check it anyway?"));
        if (answer != QMessageBox::Yes)
            return;
    }

    if (fullRepair) {
        const auto answer = QMessageBox::warning(
            this, QStringLiteral("Full Repair"),
            QStringLiteral(
                "Full Repair will redownload every required game file from the current manifest, "
                "even files that are already correct. This can download a large amount of data.\n\n"
                "Each download is checksum-verified before replacing the existing file. "
                "Optional manifest files are not forced.\n\nContinue?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }

    m_gamePath->setText(gameDirectory);
    saveSettings();
    m_scannedGameDirectory = gameDirectory;
    m_pendingUpdates.clear();
    m_fullRepairRequested = fullRepair;
    m_optionalContentRequested = false;
    m_realmlist.clear();
    m_checkButton->setText(QStringLiteral("Check for updates"));
    m_log->clear();
    m_progress->setRange(0, 0);
    m_status->setText(fullRepair
                          ? QStringLiteral("Preparing full repair...")
                          : QStringLiteral("Connecting to Ebonhold API..."));
    setBusy(true);

    m_auth->fetchGamesManifest();
}

void MainWindow::startUpdate()
{
    if (m_pendingUpdates.isEmpty()) {
        startCheck();
        return;
    }

    const QString gameDirectory = QDir::cleanPath(m_gamePath->text().trimmed());
    if (gameDirectory.isEmpty() || !QDir(gameDirectory).exists()) {
        QMessageBox::warning(this, QStringLiteral("Ebonhold Updater"),
                             QStringLiteral("The selected WoW directory no longer exists."));
        resetUpdateState();
        return;
    }

    if (gameDirectory != m_scannedGameDirectory) {
        resetUpdateState();
        startCheck();
        return;
    }

    if (m_auth->token().isEmpty()) {
        m_status->setText(QStringLiteral("Login required"));
        m_auth->fetchGamesManifest();
        return;
    }

    m_updater->setAuthToken(m_auth->token());
    m_updater->installUpdates(gameDirectory, m_pendingUpdates, m_realmlist);
}

void MainWindow::requestOptionalContent()
{
    const QString gameDirectory = QDir::cleanPath(m_gamePath->text().trimmed());
    if (gameDirectory.isEmpty() || !QDir(gameDirectory).exists()) {
        QMessageBox::warning(this, QStringLiteral("Optional Content"),
                             QStringLiteral("Select a valid WoW directory first."));
        return;
    }

    m_gamePath->setText(gameDirectory);
    saveSettings();
    m_optionalContentRequested = true;
    m_status->setText(QStringLiteral("Loading optional content..."));
    setBusy(true);
    m_auth->fetchGamesManifest();
}

void MainWindow::showOptionalContentDialog()
{
    const QString gameDirectory = QDir::cleanPath(m_gamePath->text().trimmed());
    if (gameDirectory.isEmpty() || !QDir(gameDirectory).exists() || m_manifest.isEmpty())
        return;

    OptionalContentDialog dialog(gameDirectory, m_auth->token(), m_manifest, this);
    connect(&dialog, &OptionalContentDialog::authenticationExpired, this, [this]() {
        m_auth->clearToken();
        m_updater->setAuthToken(QString());
        m_optionalContentRequested = true;
        m_status->setText(QStringLiteral("Session expired"));
    });

    dialog.exec();

    if (m_optionalContentRequested) {
        setBusy(true);
        m_auth->fetchGamesManifest();
    } else if (!m_busy) {
        m_status->setText(QStringLiteral("Ready"));
    }
}

void MainWindow::showLauncherScriptsDialog()
{
    const QString gameDirectory = QDir::cleanPath(m_gamePath->text().trimmed());
    if (gameDirectory.isEmpty() || !QDir(gameDirectory).exists()) {
        QMessageBox::warning(this, QStringLiteral("Launcher scripts"),
                             QStringLiteral("Select a valid WoW directory first."));
        return;
    }

    const QString wowPath = QDir(gameDirectory).filePath(QStringLiteral("Wow.exe"));
    if (!QFileInfo::exists(wowPath)) {
        QMessageBox::warning(this, QStringLiteral("Launcher scripts"),
                             QStringLiteral("Wow.exe was not found in the selected directory."));
        return;
    }

    QSettings settings;

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Create launcher scripts"));
    auto *layout = new QVBoxLayout(&dialog);

    auto *info = new QLabel(
        QStringLiteral("Creates optional executable .sh files in <game>/launcher. "
                       "They are independent of the updater and can be used as desktop/shortcut targets."),
        &dialog);
    info->setWordWrap(true);
    layout->addWidget(info);

    auto *protontricks = new QCheckBox(QStringLiteral("Protontricks / Steam prefix selector"), &dialog);
    auto *wine = new QCheckBox(QStringLiteral("Wine (default Wine prefix)"), &dialog);
    auto *winePrefix = new QCheckBox(QStringLiteral("Wine + custom prefix"), &dialog);
    auto *lutris = new QCheckBox(QStringLiteral("Lutris configured game"), &dialog);

    protontricks->setChecked(true);
    layout->addWidget(protontricks);
    layout->addWidget(wine);
    layout->addWidget(winePrefix);

    auto *prefixForm = new QFormLayout;
    auto *prefixPath = new QLineEdit(&dialog);
    prefixPath->setPlaceholderText(QStringLiteral("/home/user/Games/prefix"));
    prefixPath->setText(settings.value(QStringLiteral("launcher/winePrefix")).toString());
    prefixForm->addRow(QStringLiteral("Wine prefix:"), prefixPath);
    layout->addLayout(prefixForm);

    layout->addWidget(lutris);
    auto *lutrisForm = new QFormLayout;
    auto *lutrisSlug = new QLineEdit(&dialog);
    lutrisSlug->setPlaceholderText(QStringLiteral("ebonhold"));
    lutrisSlug->setText(settings.value(QStringLiteral("launcher/lutrisSlug")).toString());
    lutrisForm->addRow(QStringLiteral("Lutris game slug:"), lutrisSlug);
    layout->addLayout(lutrisForm);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Create"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted)
        return;

    if (!protontricks->isChecked() && !wine->isChecked() &&
        !winePrefix->isChecked() && !lutris->isChecked()) {
        QMessageBox::information(this, QStringLiteral("Launcher scripts"),
                                 QStringLiteral("No launcher type was selected."));
        return;
    }

    const QString customPrefixInput = prefixPath->text().trimmed();
    if (winePrefix->isChecked() && customPrefixInput.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Launcher scripts"),
                             QStringLiteral("Enter a Wine prefix path for the custom-prefix launcher."));
        return;
    }
    const QString customPrefix = customPrefixInput.isEmpty()
                                     ? QString()
                                     : QDir::cleanPath(customPrefixInput);

    const QString slug = lutrisSlug->text().trimmed();
    if (lutris->isChecked() && slug.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Launcher scripts"),
                             QStringLiteral("Enter the Lutris game slug for the Lutris launcher."));
        return;
    }

    settings.setValue(QStringLiteral("launcher/winePrefix"), customPrefix);
    settings.setValue(QStringLiteral("launcher/lutrisSlug"), slug);

    QString launcherDirectory;
    QString launcherPathError;
    if (!SafeFilesystem::ensureDirectory(gameDirectory,
                                         QStringLiteral("launcher"),
                                         &launcherDirectory,
                                         &launcherPathError)) {
        QMessageBox::critical(
            this,
            QStringLiteral("Launcher scripts"),
            launcherPathError.isEmpty()
                ? QStringLiteral("Could not safely create the launcher directory.")
                : launcherPathError);
        return;
    }

    const QString header = QStringLiteral(
        "#!/usr/bin/env bash\n"
        "set -e\n"
        "SCRIPT_DIR=\"$(cd -- \"$(dirname -- \"${BASH_SOURCE[0]}\")\" && pwd)\"\n"
        "GAME_DIR=\"$(cd -- \"$SCRIPT_DIR/..\" && pwd)\"\n"
        "WOW_EXE=\"$GAME_DIR/Wow.exe\"\n"
        "if [[ ! -f \"$WOW_EXE\" ]]; then\n"
        "  echo \"Wow.exe not found: $WOW_EXE\" >&2\n"
        "  exit 1\n"
        "fi\n\n");

    auto shellQuote = [](QString value) {
        value.replace(QLatin1Char('\''), QStringLiteral("'\"'\"'"));
        return QStringLiteral("'") + value + QStringLiteral("'");
    };

    auto writeScript = [&](const QString &name, const QString &body) -> bool {
        QString path;
        QString pathError;
        if (!SafeFilesystem::resolveDestination(gameDirectory,
                                                QStringLiteral("launcher/") + name,
                                                true,
                                                &path,
                                                &pathError)) {
            return false;
        }

        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
            return false;

        const QFileDevice::Permissions permissions =
            QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
            QFileDevice::ReadGroup | QFileDevice::ExeGroup |
            QFileDevice::ReadOther | QFileDevice::ExeOther;

        if (!file.setPermissions(permissions)) {
            file.cancelWriting();
            return false;
        }

        const QByteArray data = (header + body).toUtf8();
        if (file.write(data) != data.size()) {
            file.cancelWriting();
            return false;
        }

        QString verifiedPath;
        if (!SafeFilesystem::resolveDestination(gameDirectory,
                                                QStringLiteral("launcher/") + name,
                                                false,
                                                &verifiedPath,
                                                &pathError) ||
            QDir::cleanPath(verifiedPath) != QDir::cleanPath(path)) {
            file.cancelWriting();
            return false;
        }

        if (!file.commit())
            return false;

        return QFile::setPermissions(path, permissions);
    };
    QStringList created;
    QStringList failed;

    if (protontricks->isChecked()) {
        const QString name = QStringLiteral("launch-protontricks.sh");
        const QString body = QStringLiteral(
            "if ! command -v protontricks-launch >/dev/null 2>&1; then\n"
            "  echo \"protontricks-launch was not found.\" >&2\n"
            "  exit 1\n"
            "fi\n"
            "exec protontricks-launch --no-term \"$WOW_EXE\"\n");
        (writeScript(name, body) ? created : failed).append(name);
    }

    if (wine->isChecked()) {
        const QString name = QStringLiteral("launch-wine.sh");
        const QString body = QStringLiteral(
            "if ! command -v wine >/dev/null 2>&1; then\n"
            "  echo \"wine was not found.\" >&2\n"
            "  exit 1\n"
            "fi\n"
            "exec wine \"$WOW_EXE\"\n");
        (writeScript(name, body) ? created : failed).append(name);
    }

    if (winePrefix->isChecked()) {
        const QString name = QStringLiteral("launch-wine-prefix.sh");
        const QString body = QStringLiteral(
            "if ! command -v wine >/dev/null 2>&1; then\n"
            "  echo \"wine was not found.\" >&2\n"
            "  exit 1\n"
            "fi\n"
            "export WINEPREFIX=%1\n"
            "exec wine \"$WOW_EXE\"\n").arg(shellQuote(customPrefix));
        (writeScript(name, body) ? created : failed).append(name);
    }

    if (lutris->isChecked()) {
        const QString name = QStringLiteral("launch-lutris.sh");
        const QString body = QStringLiteral(
            "if ! command -v lutris >/dev/null 2>&1; then\n"
            "  echo \"lutris was not found.\" >&2\n"
            "  exit 1\n"
            "fi\n"
            "LUTRIS_SLUG=%1\n"
            "exec lutris \"lutris:rungame/$LUTRIS_SLUG\"\n").arg(shellQuote(slug));
        (writeScript(name, body) ? created : failed).append(name);
    }

    if (!created.isEmpty())
        appendLog(QStringLiteral("Created launcher scripts in %1: %2")
                      .arg(launcherDirectory, created.join(QStringLiteral(", "))));

    if (!failed.isEmpty()) {
        QMessageBox::critical(this, QStringLiteral("Launcher scripts"),
                              QStringLiteral("Could not create or mark executable: %1")
                                  .arg(failed.join(QStringLiteral(", "))));
        return;
    }

    QMessageBox::information(
        this, QStringLiteral("Launcher scripts"),
        QStringLiteral("Created %1 launcher script(s) in:\n%2\n\n"
                       "The scripts are already executable; no chmod command is required.")
            .arg(created.size())
            .arg(launcherDirectory));
}

void MainWindow::showLoginDialog()
{
    if (m_loginDialogOpen)
        return;

    m_loginDialogOpen = true;
    setBusy(false);

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Ebonhold API Login"));

    auto *layout = new QVBoxLayout(&dialog);
    auto *info = new QLabel(
        QStringLiteral("The updater needs an Ebonhold API session to read the patch manifest.\n"
                       "Your password is sent to Ebonhold and is not stored locally."), &dialog);
    info->setWordWrap(true);
    layout->addWidget(info);

    auto *form = new QFormLayout;
    auto *username = new QLineEdit(&dialog);
    auto *password = new QLineEdit(&dialog);
    password->setEchoMode(QLineEdit::Password);
    form->addRow(QStringLiteral("Username:"), username);
    form->addRow(QStringLiteral("Password:"), password);
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    username->setFocus();
    const int result = dialog.exec();
    m_loginDialogOpen = false;

    if (result != QDialog::Accepted) {
        m_status->setText(QStringLiteral("Login required"));
        return;
    }

    setBusy(true);
    m_status->setText(QStringLiteral("Logging in..."));
    m_auth->login(username->text(), password->text());
}
