# Ebonhold Updater 0.9

Native Qt 6 updater/launcher for Project Ebonhold on Linux.

## Build requirements on CachyOS / Arch

```bash
sudo pacman -S --needed base-devel cmake ninja qt6-base libarchive
```

## Build the normal executable

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Executable:

```text
build/EbonholdUpdater
```

Or simply run:

```bash
./build-local.sh
```

## Build the AppImage

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target appimage
```

Result:

```text
build/dist/EbonholdUpdater-0.9.0-x86_64.AppImage
```

Or simply run:

```bash
./build-appimage.sh
```

The AppImage target stages the executable and desktop assets, downloads linuxdeploy/linuxdeploy-plugin-qt when needed, bundles the linked Qt/libarchive runtime dependencies, and creates an executable AppImage.

## Main functions

- Ebonhold API login/token handling
- Manifest retrieval
- MD5 verification
- Missing/outdated required-file download and replacement
- Full Repair of all required files
- `realmlist.wtf` update
- Play via Protontricks prefix selector, with Wine fallback
- Optional launcher-script generation for Protontricks, Wine, custom Wine prefixes and Lutris
- Open `Interface/AddOns` directly

## Optional Content

The **Optional Content...** dialog is intentionally separate from normal updates and Full Repair.

### Official AddOns

The launcher reads the catalog from:

```text
https://api.project-ebonhold.com/api/launcher/addons
```

It can install/update the currently offered AddOns (for example Bagnon, Details and ElvUI) through the Ebonhold launcher download API. Installed folders and the remote `updated_at` value are tracked in:

```text
Interface/AddOns/.ebonhold-launcher-addons.json
```

This format is compatible with the state format used by Synthtrash' `ebonhold-linux-patcher`. ZIP archives are inspected before installation, extracted to a staging directory, and then moved into `Interface/AddOns`. Managed AddOns can be updated, reinstalled and removed without deleting unrelated user AddOns.

### HD Patches

The launcher discovers these optional files from the current game manifest instead of hardcoding their API file IDs:

```text
Data/patch-H.MPQ  - Character & World HD Patch
Data/patch-G.MPQ  - Creatures HD Patch
```

Their installed copies are MD5-verified against the manifest. Installation/reinstallation uses the same authenticated file-download API and checksum verification as normal game patches.

**Normal updates and Full Repair only process required manifest files (`option_slug == null`). They do not automatically install or remove HD patches or AddOns.**

## Login data

The password is used only for the Ebonhold API login request and is not stored. After a successful login, only the API bearer token is stored in the current Linux user's application config directory with owner-only read/write permissions (`0600`). The token is not embedded in the executable or AppImage.

## Credits / third-party code

This launcher builds on work from:

- Synthtrash / Trash — `ebonhold-linux-patcher`
- Sigurd Bøe (`sigboe`) — `ebonhold-updater`

Both upstream projects are MIT licensed. Their copyright notices and license terms are preserved in `THIRD_PARTY_LICENSES.md`, which is also installed into the AppImage's documentation directory.
