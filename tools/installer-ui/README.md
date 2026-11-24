# Windows installer source and release checks

This directory is the version-controlled source for FormaView's Inno Setup installer and its build/verification helpers. The former working copy under `asset-preview-planning/development/tools/installer-ui` is a development snapshot, not the source of truth. The generated installer, runtime payload, screenshots, and verification output remain outside this repository under the release staging directory.

## Inputs and build

The tracked sources include the 0.1.0 installer templates, the policy and wizard code, the Inno Setup compiler lock, runtime packaging, and verification scripts. Supply the delivery manifest for the current CMake project version and the pinned compiler in an external staging root. The installer reads its version from the three delivered executables. Set or pass that root explicitly; do not use a destination that already contains files.

```powershell
$stage = 'E:/CodexProject/asset-preview-planning/development'
python tools/installer-ui/build.py `
  --staging-root $stage `
  --runtime-delivery "$stage/verification/<runtime-candidate>/delivery.json" `
  --destination "$stage/deploy/<new-candidate>" `
  --evidence "$stage/verification/<new-candidate>"
```

For a tester build, add `--development` to the build command. The setup filename, wizard title, installed guide and Windows Apps entry identify the development build and its source revision. The numeric product version and installer identity continue to support in-place updates of earlier development builds.

`build.py` refuses non-empty output/evidence directories. The compiler binary, runtime delivery, candidate installer, and evidence are not checked into Git. Do not overwrite frozen candidate files. Review their recorded source identity and hashes before treating any generated package as a release candidate.

The setup and upgrade simulation helpers require an isolated Windows environment and the release staging root:

```powershell
$env:FORMAVIEW_RELEASE_STAGING_ROOT = 'E:/CodexProject/asset-preview-planning/development'
python tools/installer-ui/verify.py
python tools/installer-ui/verify_versions.py
```

These helpers exercise disposable installer payloads; simulated version strings are not evidence that real binaries can launch or read each other's data. Follow [`../../docs/release-validation.md`](../../docs/release-validation.md) for the mandatory actual-binary, bidirectional version and user-data checks. Finalize only the exact reviewed candidate, passing its staging root explicitly:

Image, video and model associations use distinct ICO files named by category and SHA-256. Per-user registration advertises the FormaView ProgID in Capabilities. If Windows already resolves the extension to a different handler, the installer avoids adding a competing OpenWithProgids value. When repairing an ambiguous legacy association, it removes only a matching FormaView-owned candidate. Changes to icons and cleanup follow the ownership ledger and notify the Shell. To regress this Windows behavior, run verify_shell_upgrade.ps1 with the v0.1.2 setup, the candidate setup and an evidence directory inside a fresh ordinary-user Windows Sandbox.

```powershell
pwsh -File tools/installer-ui/finalize.ps1 -StagingRoot 'E:/CodexProject/asset-preview-planning/development'
```

Never sign, publish, or distribute a candidate unless separately authorized and all release gates pass.
