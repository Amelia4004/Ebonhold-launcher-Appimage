# Ebonhold Updater AppImage

The Ebonhold Updater is distributed as an AppImage for Linux.

An AppImage does not need to be installed. Download the file, make it executable once, and start it.

## Download

Download the latest AppImage from the **Releases** section of this repository.

The file will look similar to:

```text
EbonholdUpdater-0.9.0-x86_64.AppImage
```

A `.sha256` checksum file is also provided with every release.

---

## Make the AppImage executable

Linux may remove the executable permission when downloading files from the internet.

Open a terminal in the directory containing the AppImage and run:

```bash
chmod +x EbonholdUpdater-*.AppImage
```

You only need to do this once for each downloaded AppImage.

### KDE / Dolphin

You can also enable the executable permission without using a terminal:

1. Right-click the AppImage.
2. Select **Properties**.
3. Open the **Permissions** tab.
4. Enable **Is executable** or **Allow executing file as program**.
5. Close the window.

---

## Start the launcher

You can now start the AppImage by double-clicking it.

Alternatively, start it from a terminal:

```bash
./EbonholdUpdater-*.AppImage
```

No installation is required.

---

## First start

When starting the launcher for the first time:

1. Log in with your Project Ebonhold account.
2. Select your World of Warcraft / Ebonhold installation directory.
3. Let the launcher check your game files.
4. Download missing or outdated files if required.
5. Use **Play** to launch the game.

The launcher remembers its local configuration for future starts.

---

## Game updates and repair

The launcher can automatically check the required Ebonhold game files.

### Check for Updates

Checks for missing or outdated required game files and downloads replacements when necessary.

### Full Repair

Downloads all required game files again.

Optional content is not automatically installed or removed by Full Repair.

---

## Optional Content

The launcher includes an **Optional Content** section.

### Official AddOns

The currently supported AddOns are:

* Bagnon
* Details
* ElvUI

They can be installed, updated, reinstalled or removed directly through the launcher.

AddOns are installed into:

```text
Interface/AddOns/
```

The launcher only manages AddOns installed through its AddOn manager and does not remove unrelated user-installed AddOns.

### HD Patches

The following optional HD patches are supported:

#### Character & World HD Patch

```text
Data/patch-H.MPQ
```

#### Creatures HD Patch

```text
Data/patch-G.MPQ
```

HD patches are optional and are only downloaded when explicitly selected.

Normal game updates and Full Repair do not automatically install optional HD patches.

---

## Wine / Lutris / Proton

The launcher supports starting the game through compatible Wine-based environments.

Depending on your configuration, the launcher can be used with:

* Wine
* Lutris
* Proton

Configure the desired launch method in the launcher settings before pressing **Play**.

---

## Open the AddOn folder

The **AddOn Folder** button opens:

```text
Interface/AddOns/
```

inside your configured Ebonhold installation.

This can be used to manually manage additional AddOns that are not provided through the launcher.

---

## Verify the download

Every release includes a SHA-256 checksum file.

Example:

```text
EbonholdUpdater-0.9.0-x86_64.AppImage.sha256
```

Place the AppImage and its `.sha256` file in the same directory and run:

```bash
sha256sum -c EbonholdUpdater-0.9.0-x86_64.AppImage.sha256
```

A successful verification looks like:

```text
EbonholdUpdater-0.9.0-x86_64.AppImage: OK
```

If the check reports `FAILED`, download the AppImage again.

---

## Troubleshooting

### AppImage does not start

Make sure it is executable:

```bash
chmod +x EbonholdUpdater-*.AppImage
```

Then start it from a terminal to see possible error messages:

```bash
./EbonholdUpdater-*.AppImage
```

### Game directory is incorrect

Open the launcher settings and select the directory containing your Ebonhold / World of Warcraft installation.

### AddOns are not visible in-game

Check that the AddOns were installed into:

```text
Interface/AddOns/
```

You can open this directory directly using the **AddOn Folder** button.

Also make sure **Load out of date AddOns** is enabled in the World of Warcraft AddOn menu if required.

### AppImage was updated

Each new AppImage is a separate executable file.

You can safely remove an older AppImage after downloading and testing the new release.

---

## Updates

New launcher versions are published through GitHub Releases.

Download the newest AppImage when a new launcher version becomes available.

The AppImage itself does not need to be installed system-wide.

---

## Credits

This project builds upon open-source work from:

* Synthtrash / Trash — `ebonhold-linux-patcher`
* Sigurd Bøe / sigboe — `ebonhold-updater`

See `THIRD_PARTY_LICENSES.md` for the applicable license notices.

---

## Disclaimer

This is a community launcher for Project Ebonhold.

World of Warcraft and related names, trademarks and assets belong to their respective owners.
