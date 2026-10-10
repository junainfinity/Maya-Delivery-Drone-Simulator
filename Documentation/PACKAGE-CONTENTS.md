# Prepared assets and distribution

The v0.1 publishing kit contains four store capsules, four library assets, an optional page background, a Windows multi-resolution ICO, PNG icon sizes and the Steam app JPG. Dimensions and alpha are recorded in `Provenance/artwork-validation.json`.

Artwork follows the existing Atlas Noir charcoal/ivory/amber palette and depicts the six-wheel ground robot. Cover images are AI-generated promotional illustrations, with clean natively authored vector logo assets. Source artwork is preserved; exports are size/format conversions. The library hero is scaled to the official upload resolution from its generated master, not claimed as native 4K. Check final crop/logo placement in Steamworks before uploading.

Two transparent logo generations were rejected because their alpha edges contained noise. The final transparent PNG is exported from the clean editable SVG wordmark. This records an unsuccessful attempt without distributing the rejected graphics as final assets.

Five authentic 2560x1440 historical v0.1 screenshots are included as candidates, alongside three release11 images at 1280x720 for reference. Version and resolution labels prevent the lower-resolution or older captures from being mistaken for newly captured final Steam images. A trailer shot plan is provided; no new trailer was recorded.

The local Windows ZIP is the unchanged accepted release11 binary archive with the new distribution filename. Its source checksum is `7d11f5cb0309a389e1349db0790c1f0fb9bd79b509ad56f6396e6f53cc53636d`. Runtime branding and embedded icon retain historical names. No rebuilding, Steam upload, Valve approval or new clean-PC playtest occurred in this packaging task.

The public source snapshot includes 211 custom code/config/descriptor files, including the DDSCity module, custom editor bridge files and DDSN05RobotProbe source. Hashes match the original workspace files. No Epic/sample source, publisher Content assets, engine binaries, private reports, credentials or build caches are included. Three legacy diagnostic/config files retain fixed development paths that authorized developers must configure for their workspace. A pattern-based credential scan returned zero findings; it does not prove every possible secret is absent.

The source-and-publishing ZIP contains artwork, screenshots, custom source and documentation. The Steam publishing ZIP contains artwork, screenshots, provenance and documentation. The 13.9 GB playable game is distributed as seven GitHub Release assets that reconstruct the original ZIP, with a checksum manifest and joining helper. [Windows download guide](WINDOWS-DOWNLOAD.md). Large game binaries remain outside Git history. Steam distribution uses extracted depot contents; publishing kit files belong outside the game depot.
