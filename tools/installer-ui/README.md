# Windows installer source and release checks

This directory is the version-controlled source for FormaView's Inno Setup installer and its build/verification helpers. The former working copy under `asset-preview-planning/development/tools/installer-ui` is a development snapshot, not the source of truth. The generated installer, runtime payload, screenshots, and verification output remain outside this repository under the release staging directory.

## Inputs and build

The tracked sources include the 0.1.0 installer templates, the policy and wizard code, the Inno Setup compiler lock, runtime packaging, and verification scripts. A build still requires a prepared runtime delivery and the pinned compiler in an external staging root. Set or pass that root explicitly; do not use a destination that already contains files.

```powershell
$stage = 'E:/CodexProject/asset-preview-planning/development'
python tools/installer-ui/build.py `
  --staging-root $stage `
  --runtime-delivery "$stage/verification/M13-task-history-20261110/delivery.json" `
  --destination "$stage/deploy/<new-candidate>" `
  --evidence "$stage/verification/<new-candidate>"
```

`build.py` refuses non-empty output/evidence directories. The compiler binary, runtime delivery, candidate installer, and evidence are not checked into Git. Do not overwrite frozen candidate files. Review their recorded source identity and hashes before treating any generated package as a release candidate.

The setup and upgrade simulation helpers require an isolated Windows environment and the release staging root:

```powershell
$env:FORMAVIEW_RELEASE_STAGING_ROOT = 'E:/CodexProject/asset-preview-planning/development'
python tools/installer-ui/verify.py
python tools/installer-ui/verify_versions.py
```

These helpers exercise disposable installer payloads; simulated version strings are not evidence that real binaries can launch or read each other's data. Follow [`../../docs/release-validation.md`](../../docs/release-validation.md) for the mandatory actual-binary, bidirectional version and user-data checks. Finalize only the exact reviewed candidate, passing its staging root explicitly:

```powershell
pwsh -File tools/installer-ui/finalize.ps1 -StagingRoot 'E:/CodexProject/asset-preview-planning/development'
```

Never sign, publish, or distribute a candidate unless separately authorized and all release gates pass.
