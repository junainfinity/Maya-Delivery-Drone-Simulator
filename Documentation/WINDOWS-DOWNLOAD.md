# Download and play Maya on Windows

[Open the v0.1 release](https://github.com/junainfinity/Maya-Delivery-Drone-Simulator/releases/tag/v0.1).

## Playable game

1. Download all seven `Maya-Delivery-Drone-Simulator-v0.1-Windows.zip.001` through `.007` files into one folder. They are ordered pieces of one ZIP; each part cannot be extracted separately.
2. Download `Join-Windows-Download.ps1` and `Maya-Windows-Download.manifest.json` into that same folder. Read the helper before running it. Keep at least 45 GB available while retaining the parts, assembled ZIP and extracted game together.
3. Open PowerShell in that folder and run the command below. It verifies every part, joins them and checks the complete original ZIP SHA-256. It refuses to overwrite existing output.
4. Extract the assembled `Maya-Delivery-Drone-Simulator-v0.1-Windows.zip` completely to a writable folder. Run `Play-MayaRobotics.cmd`; follow the included prerequisite guide if prompted. Choose **Start Shift** and read `How-to-Play.md`.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Join-Windows-Download.ps1
```

The execution-policy option applies only to that PowerShell process; it does not change your persistent policy. Download the helper from this release and inspect it before running it. It performs local checksum verification and file joining, without installing software or downloading anything.

The complete ZIP is **13,862,928,084 bytes**. Original SHA-256:

```text
7d11f5cb0309a389e1349db0790c1f0fb9bd79b509ad56f6396e6f53cc53636d
```

After checking successful extraction, you may remove your downloaded part files and assembled ZIP to recover disk space. The helper does not remove the parts automatically.

The parts preserve the exact existing Windows release11 package. Its internal window, menu and executable names retain Maya Robotics / DDSCity. The new promotional icon is supplied separately. This publication does not rebuild the game or establish a clean-PC/Steam installation test.

## Source and Steam publishing packages

- `Maya-Delivery-Drone-Simulator-v0.1-Source-and-Publishing.zip`: custom source, artwork, screenshots and documentation from the tagged public repository. Independently licensed engine/sample/robot dependencies are required to reconstruct an editor project.
- `Maya-Delivery-Drone-Simulator-v0.1-Steam-Publishing-Kit.zip`: artwork, screenshots, provenance and publishing documentation. Trailer production, hardware requirements, Steam account configuration and review remain outstanding.
- `SHA256SUMS.txt`: SHA-256 values for the release's downloadable files. The game manifest also contains every part's size and hash.

GitHub automatically provides source ZIP/TAR downloads from the tag. Choose the seven Windows parts for the playable game. Private Unreal source assets, publisher source packages, credentials and private project reports are excluded from these downloads. The packaged game retains its required cooked runtime content.

GitHub release assets must each be below 2 GiB; no total release-size or bandwidth limit is listed. [GitHub release limits](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases#storage-and-bandwidth-quotas).
