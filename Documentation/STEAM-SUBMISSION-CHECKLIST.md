# Steam submission checklist

**Game:** Maya - Delivery Drone Simulator  
**Preparation date:** 10 October 2026 (Asia/Kolkata)  
**State:** publishing kit prepared; Steam onboarding, upload, approval and release are not established.

## 1. Partner account and release timing

- [ ] Complete Steamworks legal identity, bank/tax verification and distribution agreements in the private partner dashboard.
- [ ] Pay the product's Steam Direct fee and record the assigned App ID. Current onboarding lists US$100 or its local equivalent per product.
- [ ] For the first few titles, satisfy the current **21-day** fee-to-release wait and at least **two weeks** of a public Coming Soon page.
- [ ] Allow review time. Steam's specific review guide says typically 3–5 business days and recommends submitting at least seven business days ahead; corrections may extend this.
- [ ] Assign build permissions to a dedicated uploader account. Keep login information outside the source repository and publishing kit.

Sources: [Onboarding](https://partner.steamgames.com/doc/gettingstarted/onboarding), [Review Process](https://partner.steamgames.com/doc/store/review_process), [Uploading to Steam](https://partner.steamgames.com/doc/sdk/uploading).

## 2. Store metadata to complete

| Field | Prepared value or outstanding decision |
|---|---|
| Product title | Maya - Delivery Drone Simulator |
| Developer / publisher | Unconfirmed; enter the authorized publishing names |
| App ID / depot ID / package ID | Unassigned or unverified; never substitute a sample App ID |
| Release date / price / regions | Publisher decision; no date or price promised |
| Release type | Decide full release versus Early Access; do not label this automatically |
| OS | Current local package is Windows x64; exact supported Windows versions require clean-machine tests |
| Languages | Current visible UI is English; verify interface, subtitles and audio columns separately |
| Genres / tags | Candidate: Simulation, Driving, Robotics, Singleplayer; select only accurate supported tags |
| Features | Single-player keyboard/mouse play is the current scope; controller, achievements, Cloud, multiplayer, VR and Deck compatibility are unverified |
| Support / contact / website | Publisher-owned public contact details required; no private addresses |
| Minimum / recommended hardware | **Not measured.** CPU, GPU, RAM, DirectX, OS and performance targets remain to be tested |
| Storage | Measure the finalized installed depot plus update headroom; ZIP size is not an installed-space requirement |
| Ratings / content survey | Complete from the entire uploaded content, using the [draft](CONTENT-SURVEY-AI-DRAFT.md) as an inventory aid |
| Store descriptions | [Markdown draft](STORE-COPY.md), [About BBCode](STORE-ABOUT.bbcode.txt) |
| Screenshots / trailer | Five or more authentic gameplay images and a gameplay trailer; see capture documents |

## 3. Build and installation configuration

The existing v0.1 release11 package contains both files below. Retain their relative locations and the complete `DDSCity/` and `Engine/` directories.

```text
DDSCity.exe                         # package bootstrap executable
DDSCity/Binaries/Win64/DDSCity.exe   # actual game executable
```

**Candidate Steam launch option:** executable `DDSCity.exe`, launch arguments:

```text
/Game/Map/Small_City_LVL?game=/Script/DDSCity.MAYADeliveryGameMode -DPCVars=CitySample.PreloadCrowdCharacterData=0,CitySample.PreloadAnimToTextureData=0
```

These arguments reproduce the existing local player launcher; this document verified their source and file existence, not a launch through Steam. The `Play-MayaRobotics.cmd` portable launcher can remain for direct local distribution. Steam should launch the configured executable without asking players to run a developer batch file.

1. Prepare a Windows depot from the extracted game folder, excluding logs, saved user state, caches, source, artwork and private evidence. Upload executable/content files through SteamPipe rather than uploading the portable ZIP as one opaque game file.
2. Configure the depot, Windows launch option and Developer Comp package ownership in Steamworks; publish those configuration changes.
3. Configure required redistributables or a reviewed install script. Test first-run prerequisites on a clean Windows account or machine that lacks Unreal development tools.
4. Validate the local file mapping, upload to a private test branch and install it using the Steam client.
5. Test the actual installed build before selecting the customer branch. No SteamPipe upload has been performed by creating these documents.

Source: [Uploading to Steam](https://partner.steamgames.com/doc/sdk/uploading). App/depot build templates require real IDs and local content paths before use.

## 4. Acceptance before submitting

- [ ] Audit licenses for the robot publisher assets, Epic content, audio/fonts and all redistributed binaries; provide required notices. [Rights inventory](RIGHTS-AND-DEPENDENCIES.md).
- [ ] Check title/logo branding in the window, menus, shortcuts and executable metadata. The existing runtime uses historical Maya Robotics / DDSCity identifiers; repository naming alone does not change the build.
- [ ] Verify Steam install, launch, quit, uninstall/reinstall, offline behavior, settings, prerequisites and ordinary user permissions.
- [ ] Run representative complete shifts, pickup/unload, battery/charging, traffic failure/restart, map/cameras and every advertised control. Measure performance on the stated minimum and recommended machines.
- [ ] Review authentic screenshots, trailer, AI inventory, language declarations and descriptions against the exact uploaded build; confirm no planned feature appears as finished.

## 5. Steam review and release

Submit store presence for review, then the near-final build once the Steamworks checklist permits it. Both need approval. Correct review feedback and observe the waiting periods before using the dashboard's release controls. Local acceptance and a prepared ZIP do not replace Valve review.

Source: [Review Process](https://partner.steamgames.com/doc/store/review_process).
