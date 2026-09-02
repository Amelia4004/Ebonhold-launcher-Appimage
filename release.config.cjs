module.exports = {
  branches: ["main"],
  tagFormat: "v${version}",
  plugins: [
    "@semantic-release/commit-analyzer",
    "@semantic-release/release-notes-generator",
    [
      "@semantic-release/exec",
      {
        prepareCmd: "./scripts/build-release.sh ${nextRelease.version}",
      },
    ],
    [
      "@semantic-release/github",
      {
        assets: [
          {
            path: "build/dist/EbonholdUpdater-*-x86_64.AppImage",
            label: "Ebonhold Updater <%= nextRelease.version %> AppImage (x86_64)",
          },
          {
            path: "build/dist/EbonholdUpdater-*-x86_64.AppImage.sha256",
            label: "SHA-256 checksum",
          },
          {
            path: "README-AppImage.md",
            label: "AppImage Installation & Usage Guide",
          },
        ],
        successComment: false,
        failComment: false,
      },
    ],
  ],
};
