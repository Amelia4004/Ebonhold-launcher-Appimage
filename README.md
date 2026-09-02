# Ebonhold-launcher-Appimage
## Credits and Third-Party Code

This project builds upon and incorporates work from the following open-source projects:

* **Synthtrash / Trash** — `ebonhold-linux-patcher`

  * Linux patcher/launcher functionality used as a basis for parts of this project.
  * Licensed under the MIT License.
  * Copyright (c) 2026 Trash

* **Sigurd Bøe (sigboe)** — `ebonhold-updater`

  * Original Ebonhold Linux updater on which Synthtrash's project is based.
  * Licensed under the MIT License.
  * Copyright (c) 2026 Sigurd Bøe

Many thanks to both developers for making their work available as open source.

This project extends and modifies the original work with its own graphical launcher, packaging and additional functionality.

The original copyright notices and MIT License terms are preserved in `THIRD_PARTY_LICENSES.md`.


Ebonhold Updater – How to Start the AppImage

If the AppImage does not start when you double-click it, Linux may not have marked it as executable yet.

1. Open a terminal in the folder containing the AppImage

For example, if the file is in your Downloads folder:

cd ~/Downloads

2. Make the AppImage executable

chmod +x EbonholdUpdater-*.AppImage

You only need to do this once.

3. Start it

You can now double-click the AppImage normally.

Or start it from the terminal:

./EbonholdUpdater-*.AppImage

KDE / Dolphin alternative

If you do not want to use the terminal:

Right-click the AppImage.

Open Properties.

Go to Permissions.

Enable Is executable / Allow executing file as program.

Close the window and double-click the AppImage.

Why is this needed?

Linux does not always preserve the executable permission when a file is downloaded from the internet. This is normal and only needs to be fixed once per downloaded AppImage.

