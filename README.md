# V3DSimulator

A 3D simulator project built with Unreal Engine and glTFRuntime. It builds source models on a per-project basis and loads worlds and resources from the resulting archives.

## Development Environment

- Unreal Engine 5.8 and the corresponding C++ build tools, as specified by the current Target settings
- glTFRuntime plugin: must be installed separately
- Engine plugins enabled by the project: Enhanced Input, Procedural Mesh Component, Niagara
- Python 3: required only for the optional user data migration tool

V3DSimulator.uproject is configured based on the dependencies in the supplied source. If your existing development environment uses additional plugins or platform settings, incorporate those as well. The names glTFRuntime and glTF/GLB remain unchanged because they refer to an external plugin and standard file formats.

## Project Structure

| Path | Purpose |
| --- | --- |
| V3DSimulator.uproject | Project entry point |
| Source/V3DSimulator | Runtime module, V3DSIMULATOR_API |
| Source/V3DSimulatorEditor | Editor services and automation tests, V3DSIMULATOREDITOR_API |
| Config | Project and platform settings |
| Content | Maps, Blueprints, materials, animations, and other assets |
| Tools/migrate_user_data.py | Tool for copying existing user data |
| Tools/PostBlueprintResave/DefaultEngine.ini | Configuration to apply after resaving assets |
| CHANGELOG_KO.md | Changes and validation scope |

## Building and Running

1. Back up your existing project and user data, then extract the archive into a new folder. Do not retain the old Source folder alongside the new one.
2. Install the glTFRuntime plugin and check the engine association for V3DSimulator.uproject.
3. Generate the project files and build the V3DSimulatorEditor target in the Development configuration.
4. Open the project and compile the Blueprints, including BP_GameInstance and BP_AssetRegistry. The default map is /Game/Maps/MainWorld.
5. Verify project selection and building, world loading, re-entry after saving, character switching, and exiting the world.

## Storage Paths and File Formats

The user data root is FPlatformProcess::UserDir()/V3DSimulator. Its actual location on each operating system follows the value returned by Unreal Engine's UserDir. This is separate from the Saved folder inside the project.

| Path Under the User Data Root | Contents |
| --- | --- |
| Projects/<project_name>/config.json | Project settings |
| Projects/<project_name>/resources | Source resources used for building |
| Worlds/<project_name>.v3d | Built world |
| Resources/<project_name>.v3d | Built resources |
| Logs | Logs |
| settings.json | User settings |

The supplied version used .gworld and .gasset rather than .glz. New builds and file discovery paths now consistently use .v3d. The binary revision 3 structure, checksums, and internal gworld:// identifiers remain unchanged. Renaming the extension alone does not make an arbitrary legacy .glz file compatible.

To migrate existing user data, close the application and run the following command. The destination path must not already exist.

```bash
python Tools/migrate_user_data.py "old path/glTFSimulator" "new path/V3DSimulator"
```

The tool copies the data without deleting the originals. Only .glz/.gworld/.gasset files whose headers and revisions match the current format are renamed to .v3d; other formats retain their original extensions. Rebuild older formats in the original project. The tool stops on file conflicts or symbolic links, and a failed migration may leave partially copied data at the destination.

## Blueprint Redirects and Config Cleanup

The default Config/DefaultEngine.ini contains CoreRedirects that map the old module and type names to their new names. These are required for the first launch because 18 included assets still reference the old modules. Simply opening the project in the editor does not permanently update those references.

A configuration for running without redirects is provided at Tools/PostBlueprintResave/DefaultEngine.ini. This file is not applied automatically.

1. Build and open the project with the redirects in the default Config still in place.
2. Compile all affected Blueprints, then resave the maps and related assets. Check parent classes, struct variables, and GameInstance and AssetRegistry references. Do not assume that running Fix Up Redirectors in the Content Browser alone completes the C++ type migration handled by CoreRedirects.
3. Close the editor and back up Config/DefaultEngine.ini.
4. Copy Tools/PostBlueprintResave/DefaultEngine.ini to Config/DefaultEngine.ini. If you have made other configuration changes since then, remove only the CoreRedirects block for the project rename from the existing file instead of replacing the entire file.
5. Restart the editor and verify Blueprint compilation, map loading, saving and reloading, and packaging. If reference errors occur, restore the backed-up Config and resave any missed assets.

Redirects may be needed again if you later import external assets or assets from another branch that use the old names. Import-history strings remaining in binary files must be distinguished from actual class references.

This cleanup removed the unnecessary DefaultEngine.txt, the file storing personal Content Browser selection paths, a nonexistent iOS plist path, staging for a nonexistent Splash directory, inactive DLSS settings, and the empty LicensingTerms entry. The editor's default map for newly created maps was set to MainWorld. Collision profiles, rendering, input, GC, Niagara, and Mac device settings remain unchanged. INI -/+ array entries can be used to replace inherited lists, so they should not be removed indiscriminately even if they appear redundant.

## Performance and Lifetime Management

- Keep update callbacks alive throughout execution to reduce the risk of map reallocation during registration and reference invalidation during shutdown.
- Deduplicate mesh indices using sorting and linear compaction.
- Release compressed data buffers before checksum verification.
- Use transferable buffer ownership for asynchronous binary saves.
- Preserve existing game-thread completion handling, weak UObject references, task draining during shutdown, and bounds and checksum validation.

Actual performance gains have not been measured. See CHANGELOG_KO.md for the detailed scope of changes.

## Validation Status

Source names, include paths, redirect targets, preservation of the original Content files, the user data migration tool, and archive integrity were checked in this working environment. UHT, Windows/macOS builds, Blueprint resaving, automation test execution, and packaging could not be performed because Unreal Engine and the glTFRuntime plugin itself were unavailable. Editor automation tests can be found using the V3DSimulator prefix.

Core Redirects reference: https://dev.epicgames.com/documentation/unreal-engine/core-redirects-in-unreal-engine
