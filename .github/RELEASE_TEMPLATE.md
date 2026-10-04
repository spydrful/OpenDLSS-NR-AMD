# OpenNR AMD Alpha <number>

<!-- Authoring template: copy into docs/releases/<tag>.md and the GitHub release body.
Replace every placeholder, remove these comments, and keep the two bodies aligned.
Use exact uploaded asset names and URLs; do not replace historical assets or tags.
This file does not automatically configure GitHub release notes. -->

An **experimental Windows build for <tested GPU> and <tested game>**. Explain in one sentence what someone can try with this release. State the release status and whether **NR starts disabled**.

## What changed

- Explain the user-visible change in plain language and say whether it is optional.
- Explain any fix and the condition that previously caused the problem.
- State whether installing or upgrading changes defaults.

## Download

| File | What it is for |
| --- | --- |
| [<exact package ZIP name>](<uploaded ZIP URL>) | Prebuilt binaries, installer, importer, documentation and corresponding source. |
| [<exact package ZIP name>.sha256](<uploaded checksum URL>) | Compare the ZIP's SHA-256 before extracting. |

GitHub's automatic **Source code** archives do not contain built DLLs. State which local model input is required and which files are excluded from the download.

## Get started

State the tested OS, GPU, driver, game, prerequisites and accepted local model versions.

1. Download the ZIP and checksum, verify the hash, and extract the package.
2. Close the game and follow the [installation guide](<tag-pinned guide URL>). Preview the install with `-WhatIf` first.
3. Import a supported model locally, or reuse a verified model already present.
4. Use the tested game settings, launch, then press **Insert → Neural → Enable NR** when ready to test.

State how to disable inference. Link to the optional-selection guide if relevant and explain restart requirements. If a simplified online guide was added after publication, distinguish it from the frozen guide included in the package.

## Upgrade or go back

Close the game and uninstall the current version with its retained package before installing another. Keep the package, `.open-nr-install.json` and `.open-nr-backup-*` folder. Preserve edited managed files and follow the guide if hash checks block removal. Imported models remain. To return to an earlier version, uninstall this one and use that release's own package and guide.

## Known limits

State unmet performance and image-quality targets visibly. Distinguish network-only GPU timing from NR-plus-bridge timing and measured game FPS. Label historical measurements with their original version; do not imply a new game benchmark. List untested hardware/platforms/features and remaining visual-review scope.

For issues, request release version, GPU/driver, settings, selected options and error text. Exclude proprietary DLLs, model files and game captures from attachments.

<details>
<summary>Technical notes, measurements and validation</summary>

## Measurement scope

Record the exact baseline and candidate builds/policies, valid and padded geometry, sample protocol, medians/tails, and what timing excludes. Link to tag-pinned delivery and scalar evidence. Keep profiling, replay and offline compiler results separate from ordinary game measurements.

## Changes and validation

Record actual selectors, guards, defaults, compatibility/ABI changes, model/operator checks, image comparison scope, lifecycle tests and source/package checks. Preserve caveats, failed experiments and their identities. State pending work precisely.

## Package identities

Record source commit, release tag, asset names/hashes and the verified publication receipt when available. Do not claim binary reproduction or performance beyond completed evidence. When improving an existing release description, retain its complete original technical record here.

</details>
