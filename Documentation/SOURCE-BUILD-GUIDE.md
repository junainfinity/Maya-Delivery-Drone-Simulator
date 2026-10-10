# Public source build guide

The public repository is a source/documentation/marketing kit. **A checkout alone is not a complete runnable Unreal project.** Epic City Sample, licensed publisher content and required project-owned Unreal assets must be supplied separately. Do not treat missing assets as evidence of a broken C++ implementation or replace them with arbitrary downloaded packages.

## Dependencies

| Dependency | Version or location | Obtain |
|---|---|---|
| Unreal Engine | 5.3.2; project association `5.3` | Epic's authorized engine installation |
| City Sample | Unreal 5.3-compatible sample, Small City map | Acquire from Epic under the applicable license |
| City Sample modules | `CitySample`, `CitySampleEditor`, `CitySampleAnimGraphRuntime` | Restore from the independently obtained matching sample |
| City Sample plugins | Project descriptor includes Traffic, CitySampleMassCrowd, HoverDrone, CitySampleSensorGrid, CustomizableSequencerTracks and others | Restore the matching sample's required plugins, not generic substitutes |
| Maya editor helpers | Four custom `Source/CitySampleEditor/MAYA*` bridge/observer files and `Plugins/DDSN05RobotProbe/` | Supplied in the public source snapshot; overlay into the independently acquired sample |
| Robot publisher package | `/Game/BendylabStudios/BLS_DeliveryRobot_v1/` | Acquire separately under its publisher terms; it is not granted by this repository |
| Project-owned assets | `/Game/MAYA/`, `/Game/DDSCity/` and applicable map modifications/dependencies | Obtain an authorized asset package or recreate with the maintainer's dependency manifest |
| Native Windows toolchain | Visual Studio 2022 C++ toolchain and Windows SDK compatible with UE 5.3 | Follow Epic's UE 5.3 setup guidance |

The source contains references to parcel meshes/materials, objective rings, loading presentation and robot collision/render assets under `/Game/MAYA/`. The exact approved project assets and world modifications matter; acquiring the base City Sample does not supply all Maya-specific content. Engine headers and C++ sample/plugin dependencies are also required at compile time.

## Reconstruct an authorized workspace

1. Install UE 5.3.2 and the matching C++ toolchain. Acquire the compatible City Sample and robot dependencies separately.
2. Make a working copy of that sample. Restore its required `Content/`, sample `Source/` modules and `Plugins/` without publicly redistributing them.
3. Overlay the repository's project-owned `Source/DDSCity/`, target files, `Config/`, `DDSCity.uproject`, four custom `Source/CitySampleEditor/MAYA*` files and the supplied `Plugins/DDSN05RobotProbe/`. Keep the sample's original editor module and build rules; these four files do not replace that module.
4. Supply the authorized Maya asset package and any required map changes at their original Unreal package paths. Check the editor for missing references.
5. Check the editor dependency configuration below, configure legacy development paths for your workspace, generate Visual Studio project files and build `DDSCityEditor` for Win64 Development Editor. This reconstruction has not been tested from a clean public checkout.

### Editor helper dependencies

The supplied editor files are `MAYANativeTestBridge.cpp/.h` and `MAYAStartupWindowObserver.cpp/.h`. They are compiled as part of the separately acquired `CitySampleEditor` module. The inspected local `CitySampleEditor.Build.cs` includes `EnhancedInput`, `ApplicationCore` and `WorldPartitionHLODUtilities` in `PrivateDependencyModuleNames`, as well as the stock public editor/UI dependencies. Preserve that dependency configuration in your authorized local sample. In particular, the custom bridge includes Enhanced Input headers, so add `EnhancedInput` to the private dependency list if the independently acquired sample lacks it. The bridge finds Maya classes through Unreal reflection; this inspection found no direct DDSCity header dependency to add to that editor module.

`DDSN05RobotProbe` is supplied with its descriptor, build rules and source. It is restricted to Win64 Editor, uses the supplied DDSCity module and resolves its header directory relative to the project. Keep the plugin at `Plugins/DDSN05RobotProbe/`; it is an editor diagnostic helper, not a player runtime requirement.

### Legacy development paths

Three files preserve their exact historical workspace paths: `Config/DefaultEngine.ini` (derived-data cache roots), `Source/DDSCity/DDSOriginalAssemblyStageSubsystem.cpp` and `Source/DDSCity/DDSOriginalCityCalibrationDiagnosticSubsystem.cpp` (diagnostic output roots). Configure those paths in your own working copy before using the affected cache or diagnostics. Do not run diagnostic helpers blindly against their original paths. The public snapshot keeps original source bytes; this guide does not claim a portable configuration or modify accepted runtime artifacts.

## Example commands

Run from a PowerShell session after setting the two paths to your own authorized local installations. These are placeholders, not private project paths.

```powershell
$mayaEngineRoot = 'D:\Unreal\UE_5.3'
$mayaWorkspaceRoot = 'D:\Games\Maya-Delivery-Drone-Simulator'
$mayaProjectFile = Join-Path $mayaWorkspaceRoot 'DDSCity.uproject'

& "$mayaEngineRoot\Engine\Build\BatchFiles\Build.bat" `
  DDSCityEditor Win64 Development "-Project=$mayaProjectFile" -WaitMutex
```

Launch the editor, inspect `/Game/Map/Small_City_LVL`, and use the Maya delivery game mode. For a standalone editor game launch:

```powershell
& "$mayaEngineRoot\Engine\Binaries\Win64\UnrealEditor.exe" `
  $mayaProjectFile '/Game/Map/Small_City_LVL?game=/Script/DDSCity.MAYADeliveryGameMode' `
  -game '-DPCVars=CitySample.PreloadCrowdCharacterData=0,CitySample.PreloadAnimToTextureData=0'
```

The final packaged game's verified relative layout is `DDSCity.exe`, `DDSCity/Binaries/Win64/DDSCity.exe`, `DDSCity/Content/Paks/` and `Engine/`. Keep that layout intact. Rebuilding a Shipping package, clean-machine testing and Steam installation review are separate tasks; a successful source compilation does not establish release acceptance.

## Dependency and rights sources

- [City Sample overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/city-sample-project-unreal-engine-demonstration) — official sample description and setup.
- [Visual Studio setup for Unreal Engine](https://dev.epicgames.com/documentation/en-us/unreal-engine/setting-up-visual-studio-development-environment-for-cplusplus-projects-in-unreal-engine) — select the UE 5.3 documentation version when checking toolchain requirements.
- [Rights and dependencies](RIGHTS-AND-DEPENDENCIES.md) — public source scope and excluded packages.
