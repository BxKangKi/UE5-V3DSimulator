# V3DSimulator

V3DSimulator is an Unreal Engine 5.8 simulator with runtime GLB-derived models, streamed worlds, character locomotion and ragdolls, vehicles, equipment, water, weather, and object-centred gravity fields.

This package contains C++ source, packaging configuration, and this manual. Use it with the project's existing `.uproject`, Content, and plugins.

## Install and build

1. Close Unreal Editor and replace the project's `Source` directory with this package's `Source`. Regenerate project files. Source replacement prevents removed classes from remaining in the build.
2. Merge the supplied `Config/DefaultGame.ini` and `Config/Windows/WindowsEngine.ini` sections into the corresponding project files. Keep the project's other Config files, input bindings, collision profiles, and asset references.
3. Use Unreal Engine 5.8, the Windows C++ toolchain/SDK, and the project's glTFRuntime plugin. The module also uses EnhancedInput, UMG/Slate, Niagara, PhysicsCore, MoviePlayer, JsonUtilities, and ProceduralMeshComponent.
4. Build `V3DSimulatorEditor` in Development. Use `/Game/Maps/MainWorld` as the startup map, the `BP_GameInstance` derived from `UV3DSimulatorGameInstance`, and `AMainGameMode`. The registry Blueprint supplies the menu, character, material, and actor classes. No StartActor is required.
5. Rebuild authored projects into `.v3d` files with this source before using them at runtime.

**File formats:** this implementation reads `.v3d` archive revision **4** and mutable `.dat` revision **3** (payload revision **2**). Older binary layouts are rejected; there is no migration reader. Preserve backups of existing worlds and saves. To start a world using the current format, rebuild its source project and move an incompatible `Worlds/Data/<WorldName>.dat` out of the active data directory so the simulator can create a fresh save. An old save cannot be resumed by simply renaming its extension or editing its version bytes.

The source does not rewrite Blueprint assets. Recompile project Blueprints after source replacement. Remove references to removed native properties if Unreal reports them, and assign the actual project browser class `/Game/Blueprints/UI/WBP_ProjectSelection.WBP_ProjectSelection_C` in the registry.

### Package for Windows Shipping

Run from the project root, with Unreal Editor closed:

```powershell
.\Source\Build\PackageShipping.ps1 -EngineRoot 'C:\Program Files\Epic Games\UE_5.8' -ArchiveRoot 'D:\V3DReleases'
```

If necessary, also supply `-Project 'D:\Project\V3DSimulator.uproject'`. The script builds the Editor module, cooks all Windows content and target shaders, checks cooked startup/UI assets and shader libraries, then builds and packages Shipping using that verified cook. Distribute the entire resulting Windows package directory.

Keep `bCookAll=True`, shared shader code, shader libraries, and the required UI/glTFRuntime cook directories. When intentionally changing UI asset paths, update the script's `RequiredCookedAssets` checks as well. Cooked shader bytecode is prepared during packaging. GPU/driver-specific PSO creation may still occur on the destination computer; menu initialization does not wait for the entire PSO queue to empty.

### Verification in Unreal

Build Development Editor and run the Automation tests under `V3DSimulator`. The `V3DSimulator.Gravity` tests cover radial acceleration, field input validation, all-axis character crouching, and source destruction. Archive tests cover persistent gravity settings and the source-chunk index. Also test a packaged executable with the project's actual assets.

For interactive acceptance, use a sphere with collision and a gravity radius larger than the sphere. Walk around its equator and poles, look up/down, crouch under an obstruction, jump, enable/recover a ragdoll, and drive/exit a vehicle. Test entering/leaving overlapping fields, moving/deleting a source, save/reload, and server/client play. This delivery was checked with a standalone C++ math/selection harness and address/undefined-behaviour sanitizers; the full Unreal Editor/Shipping build requires the project and engine installation.

## Files and authoring workflow

The user-data root is `FPlatformProcess::UserDir()/V3DSimulator`. The platform decides the exact user-directory location.

| Path relative to the user-data root | Purpose |
| --- | --- |
| `settings.json` | User rendering and quality settings |
| `Projects/<ProjectName>/config.json` | Authored project/world configuration |
| `Projects/<ProjectName>/resources/**/<AssetName>.glb` | Source model |
| `Projects/<ProjectName>/resources/**/<AssetName>.json` | Model definition next to its GLB |
| `Projects/<ProjectName>/resources/**/<SoundName>.wav` | Source sound |
| `Projects/<ProjectName>/resources/**/<SoundName>.json` | Sound definition next to its WAV |
| `Resources/<ProjectName>.v3d` | Built Prefab, Character, or Dynamic asset project |
| `Worlds/<ProjectName>.v3d` | Built World project |
| `Worlds/Data/<WorldName>.dat` | Mutable placements, gravity settings, player state, and world time |
| `Logs/` | Runtime diagnostics |

Create a project, put its GLB/WAV sources under `resources`, author their sibling definitions, and build through the project menu. The scan is recursive; directories do not determine model categories. A source file and its JSON must be in the same directory and have exactly the same basename. Use unique, nonzero UUIDs. A GLB and WAV with the same basename would share one JSON path and must be renamed to distinct names.

A missing sibling JSON is created with canonical identity fields and the project's default model type. Existing documents are validated without automatic metadata migration, UUID replacement, or rewriting. Add required character bone mappings before building a Character model. Runtime loading reads the built `.v3d` archive, not loose source GLB/JSON files. Rebuild after authoring changes.

## JSON conventions

Use UTF-8 JSON objects with quoted keys, finite numeric values, and no comments or trailing commas. Authored project/model/sound metadata uses `"Version": "1.0.0"`. Spell fields and enumerations exactly as shown. Optional settings use native class defaults unless the owning Blueprint supplies different defaults. Unknown application metadata is not automatically gameplay configuration.

Distances and positions use centimetres, speeds use cm/s, accelerations use cm/s², masses use kilograms, and rotations use degrees unless stated otherwise. UE force values are kg·cm/s²; torque values are kg·cm²/s². Gravity strength is acceleration, not source mass.

The following sections cover every project-owned JSON reader/writer: project/world configuration, model definitions and mesh settings, dynamic objects, gravity fields, vehicles, weapons, characters, sounds, user settings, interaction helper documents, and embedded player custom data. GLB's internal JSON follows the glTF 2.0 container/schema rather than a separate V3DSimulator JSON format.

## Project and world configuration: `config.json`

```json
{
  "UUID": "572928b5-24d6-4807-9c33-5ee1bf6e2951",
  "Name": "GravityDemo",
  "Version": "1.0.0",
  "ProjectType": "World",
  "WorldName": "Gravity Demo",
  "bAllowProjectAssets": false,
  "Latitude": 38.0,
  "Longitude": 127.0,
  "AxialTilt": 23.5,
  "OneYearDays": 365.0,
  "OneDayTime": 86400.0,
  "TimeSpeed": 60.0,
  "bOcean": false,
  "OceanHeightCm": 0.0,
  "Cloud": {
    "bEnabled": true,
    "Coverage": 0.55,
    "Density": 0.7,
    "Opacity": 1.0,
    "WindSpeed": 1.0,
    "Tint": { "R": 1.0, "G": 1.0, "B": 1.0, "A": 1.0 }
  },
  "Weather": {
    "bEnabled": false,
    "Preset": "Rain",
    "Intensity": 1.0,
    "TickIntervalSeconds": 1.0,
    "bAutoCycle": true,
    "MinDurationTicks": 300,
    "MaxDurationTicks": 1200,
    "ClearWeight": 0.55,
    "RainWeight": 0.35,
    "SnowWeight": 0.1
  },
  "Gameplay": {
    "WorldGameMode": "Default",
    "bCheatsEnabled": false,
    "PlayerMaxHealth": 100.0,
    "PlayerMassKg": 80.0,
    "PlayerPushTractionCoefficient": 0.3
  }
}
```

`UUID`, `Name`, `Version`, and `ProjectType` identify the project. `Name` is its display name; its folder must also be a valid single directory name. `WorldName` and `bAllowProjectAssets` apply to World projects. A project document is not an asset definition and must not use `AssetType` or `ModelType` to select its project type.

| `ProjectType` | Allowed model types | Default model type |
| --- | --- | --- |
| `World` | `Static`, `Dynamic`, `Character` | `Static` |
| `Prefab` | `Static` | `Static` |
| `Character` | `Character` | `Character` |
| `Dynamic` | `Dynamic` | `Dynamic` |

| World field | Default | Meaning |
| --- | --- | --- |
| `WorldName` | Project name | Display name for the world |
| `bAllowProjectAssets` | `false` | Permit built project asset resources |
| `Latitude`, `Longitude` | `38`, `127` | Geographic angles for environment calculations |
| `AxialTilt` | `23.5` | Planetary axial tilt in degrees |
| `OneYearDays` | `365` | Days per environment year; use a positive value |
| `OneDayTime` | `86400` | Simulated seconds per day; use a positive value |
| `TimeSpeed` | `60` | World-time multiplier |
| `bOcean` | `false` | Enable the global ocean |
| `OceanHeightCm` | `0` | Ocean surface world Z coordinate |

`Cloud` contains `bEnabled`, `Coverage`, `Density`, `Opacity`, `WindSpeed`, and RGBA `Tint`; the example shows their defaults. Coverage/density/opacity are normalized controls. `Weather.Preset` selects `Clear`, `Rain`, or `Snow`. Weather intensity is clamped to 0–10, tick interval to 0.05–3600 seconds, duration counts to 1–100000000 ticks, and weights to 0–1000000. Maximum duration is at least the minimum. Cycle weights determine relative selection probabilities.

`Gameplay.WorldGameMode` is a simulator rule preset (`Default`, `Creator`, or `RealLife`), not an Unreal GameMode class path. Player maximum health is at least 1, player mass is clamped to 1–10000 kg, and traction coefficient to 0–2. Push forces follow the character's current gravity magnitude.

Water surfaces remain horizontal world-Z surfaces. A radial field does not turn the ocean into a spherical ocean. Water surface constraints and buoyancy retain that geometry while character input and land support use the current gravity frame.

## Model definition: `<AssetName>.json`

Every model definition contains:

```json
{
  "AssetType": "Model",
  "UUID": "4d918c14-bb1a-4e23-b680-bcb6b441a1b9",
  "Name": "GravitySphere",
  "DisplayName": "Gravity Sphere",
  "Version": "1.0.0",
  "ModelType": "Static",
  "GravityField": {
    "Enabled": true,
    "RadiusCm": 20000.0,
    "StrengthCmPerSecondSquared": 980.0,
    "Priority": 10,
    "Falloff": "Constant",
    "LocalCenter": { "X": 0.0, "Y": 0.0, "Z": 0.0 }
  }
}
```

`Name` is a safe single identifier, `DisplayName` is nonempty display text, and `ModelType` is exactly `Static`, `Dynamic`, or `Character`. Dynamic models may specify **one** of `EntityType` (`Vehicle`, `Prop`, `Animal`) or `ItemType` (`Weapon`, `Tool`, `Misc`), or neither. Do not put these subtype fields on Static/Character models. A Character model requires the complete `Bones` mapping below.

### Mesh collision and lights: `MeshData`

`MeshData` is an optional object mapping base mesh names to settings. `Center` and overall `Size` are generated archive metadata; they are not author controls.

```json
{
  "MeshData": {
    "Platform": {
      "ComplexCollision": true,
      "SimpleCollision": false,
      "IsEntity": false,
      "Colliders": [
        { "Type": "Box", "X": 0, "Y": 0, "Z": 0, "DX": 100, "DY": 100, "DZ": 20 }
      ],
      "Lights": [
        {
          "X": 0, "Y": 0, "Z": 150,
          "Unit": "Unitless",
          "Intensity": 10,
          "SourceRadius": 10,
          "SoftSourceRadius": 10,
          "AttenuationRadius": 1000,
          "Length": 10
        }
      ]
    }
  }
}
```

Merge this fragment into a complete model definition. Mesh defaults are `ComplexCollision=true`, `SimpleCollision=false`, `IsEntity=false`, and empty collider/light arrays. Collider `Type` is `None`, `Sphere`, `Capsule`, or `Box`. `X/Y/Z` is its local centre. For Box, `DX/DY/DZ` are half extents; for Sphere, `DX` is radius; for Capsule, `DX` is radius and `DY` is half height (`DZ` is unused). Use nonnegative sizes and capsule half height at least its radius. Light units are Unreal `ELightUnits` names: `Unitless`, `Candelas`, `Lumens`, `EV`, or `Nits`. Light positions are local. The native defaults match the example; attenuation/source radii and length are nonnegative.

Mesh/node names may contain semicolon directives, for example `WheelFrontLeft;WHEEL` and `Terrain;LOD0`. `LOD0`–`LOD3` are the supported LOD slots. `WheelMeshNames` can explicitly identify vehicle wheels instead. Names alone do not supply a gravity radius; use `GravityField` on the owning model/actor.

## Object gravity fields

`GravityField` is optional on a Static or Dynamic model, including a Dynamic Vehicle or Weapon. Omitting it creates no field.

| Key | Type | Default | Accepted values |
| --- | --- | --- | --- |
| `Enabled` | boolean | `false` | Enable/disable this source |
| `RadiusCm` | number | `1000` | 1–100000000 cm |
| `StrengthCmPerSecondSquared` | number | `980` | 0–1000000 cm/s² |
| `Priority` | integer | `0` | Signed 32-bit integer |
| `Falloff` | string | `Constant` | `Constant` or `Linear` |
| `LocalCenter` | object | `{ "X": 0, "Y": 0, "Z": 0 }` | Exactly numeric `X`, `Y`, `Z`; absolute values at most 100000000 |

The centre is the owner's transform applied to `LocalCenter`. It moves and rotates with a dynamic source. Radius and strength are world-space quantities and do not scale with the actor; local-centre offsets do follow the actor's scale. Source mass does not determine field strength.

Inside the spherical radius, acceleration points toward the source centre. `Constant` uses the chosen strength; `Linear` fades to zero at the outer boundary. Within a small core of `min(10 cm, 1% of radius)`, acceleration tapers smoothly to zero to avoid a direction singularity. A strength of zero is an active zero-gravity field. A character at an exact zero-acceleration point retains its last valid up direction.

When fields overlap, the highest priority wins. Equal priorities use the largest effective acceleration, then the nearest centre, then centre XYZ order. Fields are not summed. The selected field replaces world gravity. Leaving all fields restores world gravity. A source does not attract its own owner's bodies.

Characters use Unreal's custom-gravity movement: gravity-relative floor tests, jumping, capsule up, camera yaw/pitch, movement input, animation speed, and land ragdoll recovery. CharacterMovement's `GravityScale` still multiplies field acceleration (native character default: `1.75`). Set it to `1` in the character Blueprint if the character should use exactly the same acceleration magnitude as an ordinary physics body.

Movable bodies with physics simulation, gravity enabled, and query-enabled collision are found through the physics broad phase and receive the field acceleration. This includes ragdoll bodies, vehicles, and unrelated editor-placed physics props. Bodies deliberately configured with gravity disabled remain excluded. Kinematic projectiles use their own bounded swept integration; outside fields they retain their straight-line projectile behaviour. Other custom kinematic movement implementations can call `UGravityFieldSubsystem::GetGravityAtLocation` and integrate the returned acceleration themselves.

### Set an individual actor's field

StaticActor, DynamicActor, VehiclePawn, and WeaponActor contain a `GravityField` component. In an actor instance/Blueprint, enable **Override Model Settings** to use that component's **Settings** instead of the JSON defaults. For another actor class, add **Gravity Field Component** in the editor.

At runtime, call the component's Blueprint `Set Settings` function on the server. A valid update returns true; invalid input/client-side mutation returns false. Use `Get Settings` to inspect it. Field settings replicate with replicated owners, and enabled sources are made always relevant so autonomous clients can evaluate the same fields. Moving sources also need actor movement replication enabled; the supplied moving actor classes already enable it.

The world-object save system stores effective field settings per placement in `.dat`. Restored placements retain their saved settings; changed JSON defaults apply to newly created placements. Actors placed directly in an Unreal map retain their editor properties through the map asset. Temporary actors outside the world-object save system require their own persistence policy.

Field-bearing static scenes and saved placement chunks remain resident independently of normal observer-distance culling. Dynamic gravity sources are exempt from distance-based physics suspension. Static miniature previews do not emit gameplay gravity. Very large radii, many overlapping fields, or many permanently resident sources increase physics query and streaming costs; choose ranges that match the scene.

## Dynamic props

Use `ModelType: "Dynamic"`. Optional `EntityType: "Prop"` identifies a prop. These root fields configure the standard DynamicActor physics proxy:

| Key | Default | Meaning |
| --- | --- | --- |
| `EnableCollision` | `true` | Enable the proxy's collision |
| `CollisionProfile` | `"BlockAll"` | An existing Unreal collision-profile name |
| `SimulatePhysics` | `false` | Enable body simulation |
| `MassKg` | `0` | 0 uses automatic mass; positive values override mass, capped at 1000000000 |
| `Transform` | Identity | Optional local model transform |
| `GravityField` | Disabled | Optional source field described above |

`Transform` is an object with optional `X`, `Y`, `Z`, `Pitch`, `Yaw`, `Roll`, `Scale`, `ScaleX`, `ScaleY`, `ScaleZ`. Translation/rotation default to zero and scale to one. `Scale` sets all three scale axes before explicitly supplied axis values override it.

```json
{
  "AssetType": "Model",
  "UUID": "1069d0d0-9dce-46e8-a350-3e080672e82e",
  "Name": "MovingSource",
  "DisplayName": "Moving Gravity Source",
  "Version": "1.0.0",
  "ModelType": "Dynamic",
  "EntityType": "Prop",
  "EnableCollision": true,
  "CollisionProfile": "BlockAll",
  "SimulatePhysics": true,
  "MassKg": 40,
  "Transform": { "X": 0, "Y": 0, "Z": 0, "Scale": 1 },
  "GravityField": { "Enabled": true, "RadiusCm": 1500, "StrengthCmPerSecondSquared": 300, "Priority": 5 }
}
```

## Vehicles

Set `ModelType: "Dynamic"`, `EntityType: "Vehicle"`. Put driving settings in the **`VehicleTuning`** object. The JSON keys below are supported; other editable vehicle C++/Blueprint properties are not automatically JSON keys. The vehicle requires body geometry and at least one wheel mesh identified by `;WHEEL` or `WheelMeshNames`.

```json
{
  "AssetType": "Model",
  "UUID": "a8af6d72-36b1-4aa0-8880-e79bfe8c6d60",
  "Name": "Rover",
  "DisplayName": "Gravity Rover",
  "Version": "1.0.0",
  "ModelType": "Dynamic",
  "EntityType": "Vehicle",
  "VehicleTuning": {
    "MaxSpeedForward": 4200,
    "EngineForce": 430000,
    "WheelHeightOffset": 0,
    "WheelSpinDirection": -1,
    "WheelMeshNames": ["FrontLeft", "FrontRight", "RearLeft", "RearRight"]
  }
}
```

The following numeric controls are clamped to the listed ranges. Defaults are native C++ defaults; a derived Blueprint may provide different defaults.

| `VehicleTuning` key | Default | Range |
| --- | --- | --- |
| `MaxSpeedForward` | 4200.0 | 0.0–12000 |
| `EngineForce` | 430000.0 | 0.0–5000000 |
| `ReverseForce` | 140000.0 | 0.0–5000000 |
| `BrakeForce` | 620000.0 | 0.0–5000000 |
| `EngineBrakingForce` | 72000.0 | 0.0–5000000 |
| `RollingResistance` | 0.012 | 0.0–0.20 |
| `MaxSteeringAngleDegrees` | 34.0 | 1.0–55.0 |
| `HighSpeedSteeringAngleDegrees` | 22.0 | 1.0–45.0 |
| `SteeringYawRateAssist` | 42000.0 | 0.0–2000000 |
| `HighSpeedYawAssistStrength` | 130000.0 | 0.0–2000000 |
| `HighSpeedYawAssistStartSpeed` | 1100.0 | 100.0–12000 |
| `SteeringYawDamping` | 118000.0 | 0.0–2000000 |
| `MaxSteeringAssistTorque` | 380000.0 | 0.0–2000000 |
| `LowSpeedSteeringYawAssistSpeed` | 145.0 | 0.0–12000 |
| `MinSteeringSpeedFactor` | 0.30 | 0.0–1.0 |
| `MaxSteeringSpeedFactor` | 1.0 | 0.0–1.0 |
| `SteeringSpeedForFullAssist` | 4300.0 | 1.0–12000 |
| `AckermannStrength` | 1.0 | 0.0–1.0 |
| `FrontSteeringGripMultiplier` | 1.34 | 0.1–10 |
| `RearSteeringGripMultiplier` | 1.24 | 0.1–10 |
| `HighSpeedFrontGripBoost` | 1.28 | 1.0–2.0 |
| `HighSpeedSteeringAuthorityScale` | 1.30 | 1.0–2.0 |
| `LateralGrip` | 1.08 | 0.1–10 |
| `TireLateralForceScale` | 0.96 | 0.0–1.0 |
| `MaxLateralGripForce` | 330000.0 | 0.0–5000000 |
| `TireLongitudinalFriction` | 1.48 | 0.1–10 |
| `TireLateralFriction` | 1.24 | 0.1–10 |
| `TireCorneringStiffness` | 7.2 | 0.1–100.0 |
| `TireSlipReferenceSpeed` | 120.0 | 1.0–12000 |
| `HighSpeedLateralGripScale` | 0.96 | 0.1–1.0 |
| `HighSpeedLateralGripSpeed` | 3400.0 | 100.0–12000 |
| `SteeringLateralGripReserve` | 0.55 | 0.0–0.90 |
| `DrivenFrontTorqueShare` | 0.32 | 0.0–1.0 |
| `AerodynamicDragCoefficient` | 0.010 | 0.0–1.0 |
| `MaxAerodynamicDrag` | 480000.0 | 0.0–5000000 |
| `GroundedDownforceCoefficient` | 0.00085 | 0.0–1.0 |
| `MaxGroundedDownforce` | 36000.0 | 0.0–5000000 |
| `MinimumDownforceSpeed` | 180.0 | 0.0–12000 |
| `FrontDownforceCoefficient` | 0.00050 | 0.0–1.0 |
| `MaxFrontDownforce` | 17000.0 | 0.0–5000000 |
| `ThrottleFrontDownforce` | 3200.0 | 0.0–5000000 |
| `ThrottleInputInterpSpeed` | 5.2 | 0.1–60.0 |
| `SteeringInputInterpSpeed` | 9.0 | 0.1–60.0 |
| `SteeringInputRiseRate` | 2.65 | 0.1–30.0 |
| `SteeringInputReturnRate` | 10.5 | 0.1–60.0 |
| `SteeringInputSpeedDamping` | 0.12 | 0.0–1.0 |
| `SteeringInputCurveExponent` | 1.35 | 1.0–3.0 |
| `RideHeightOffset` | 0.0 | -30.0–30.0 |
| `WheelHeightOffset` | 0.0 | -200–200 |
| `FrontWheelHeightOffset` | 0.0 | -200–200 |
| `RearWheelHeightOffset` | 0.0 | -200–200 |
| `WheelSpinDirection` | -1.0 | -1.0–1.0 |
| `WheelGroundContactBuffer` | 0.05 | 0.0–6.0 |
| `WheelVisualGroundContactBuffer` | 0.05 | 0.0–6.0 |

`WheelHeightOffsets` is an optional numeric array of per-wheel offsets (each clamped to ±200 cm). `WheelMeshNames` is an optional array of wheel mesh names. Global, front/rear, and per-wheel height offsets are applied to the authored wheel mounting positions. `WheelSpinDirection` selects the visual spin sign; values near zero resolve to the default reverse sign. Vehicle gravity and suspension support follow the field at the chassis. A vehicle's own source field does not pull its chassis toward itself.

## Weapons

Use `ModelType: "Dynamic"`, `ItemType: "Weapon"`. Weapon settings are root-level fields:

| Key | Native default | Meaning / limits |
| --- | --- | --- |
| `AttachSocketName` | `"rightHand"` | Character attachment socket/bone |
| `HoldTransform` | Location `(45,18,-18)`, zero rotation, unit scale | Socket-relative model transform |
| `RightHandIK` | Location `(20,8,-4)`, zero rotation, unit scale | Right-hand target transform |
| `LeftHandIK` | Location `(65,-9,-4)`, zero rotation, unit scale | Left-hand target transform |
| `MuzzleOffset` | `{ "X": 95, "Y": 0, "Z": 0 }` | Weapon-local muzzle position |
| `Range` | `20000` | 1–100000000 cm |
| `Damage` | `20` | 0–10000000 |
| `ImpactImpulse` | `24000` | 0–1000000000, kg·cm/s |
| `FireInterval` | `0.12` | 0.01–3600 seconds |
| `TraceRadius` | `0` | 0–100000 cm; zero uses a line trace |
| `bProjectile` | `false` | Projectile instead of instant trace |
| `ProjectileSpeed` | `6500` | 100–10000000 cm/s |
| `ProjectileLifeSeconds` | `5` | 0.1–3600 seconds |
| `GravityField` | Disabled | Field emitted by the weapon actor, not by each projectile |

Weapon transform objects use the same `X/Y/Z/Pitch/Yaw/Roll/Scale/ScaleX/ScaleY/ScaleZ` representation as a Dynamic `Transform`. `MuzzleOffset` uses an XYZ object. Projectile gravity is evaluated along its flight path, with collision sweeps on each integration segment. Damage is processed once even when a sweep and hit callback occur together.

## Characters and `Bones`

Use `ModelType: "Character"` with the normal model identity fields. `Bones` maps each canonical key to a distinct source skeleton bone name. Keys are case-sensitive. Every listed key is required, values must be nonempty, and extra keys or duplicate source names are rejected. The source bone names below are placeholders: replace the values with names from your GLB, keeping the keys unchanged.

```json
{
  "AssetType": "Model",
  "UUID": "50e11264-6f9a-4627-8731-2eb1ab8de170",
  "Name": "Explorer",
  "DisplayName": "Explorer",
  "Version": "1.0.0",
  "ModelType": "Character",
  "Bones": {
    "Root": "Root",
    "chest": "chest",
    "head": "head",
    "hips": "hips",
    "leftEye": "leftEye",
    "leftFoot": "leftFoot",
    "leftHand": "leftHand",
    "leftIndexDistal": "leftIndexDistal",
    "leftIndexIntermediate": "leftIndexIntermediate",
    "leftIndexProximal": "leftIndexProximal",
    "leftLittleDistal": "leftLittleDistal",
    "leftLittleIntermediate": "leftLittleIntermediate",
    "leftLittleProximal": "leftLittleProximal",
    "leftLowerArm": "leftLowerArm",
    "leftLowerLeg": "leftLowerLeg",
    "leftMiddleDistal": "leftMiddleDistal",
    "leftMiddleIntermediate": "leftMiddleIntermediate",
    "leftMiddleProximal": "leftMiddleProximal",
    "leftRingDistal": "leftRingDistal",
    "leftRingIntermediate": "leftRingIntermediate",
    "leftRingProximal": "leftRingProximal",
    "leftShoulder": "leftShoulder",
    "leftThumbDistal": "leftThumbDistal",
    "leftThumbIntermediate": "leftThumbIntermediate",
    "leftThumbProximal": "leftThumbProximal",
    "leftToes": "leftToes",
    "leftUpperArm": "leftUpperArm",
    "leftUpperLeg": "leftUpperLeg",
    "neck": "neck",
    "rightEye": "rightEye",
    "rightFoot": "rightFoot",
    "rightHand": "rightHand",
    "rightIndexDistal": "rightIndexDistal",
    "rightIndexIntermediate": "rightIndexIntermediate",
    "rightIndexProximal": "rightIndexProximal",
    "rightLittleDistal": "rightLittleDistal",
    "rightLittleIntermediate": "rightLittleIntermediate",
    "rightLittleProximal": "rightLittleProximal",
    "rightLowerArm": "rightLowerArm",
    "rightLowerLeg": "rightLowerLeg",
    "rightMiddleDistal": "rightMiddleDistal",
    "rightMiddleIntermediate": "rightMiddleIntermediate",
    "rightMiddleProximal": "rightMiddleProximal",
    "rightRingDistal": "rightRingDistal",
    "rightRingIntermediate": "rightRingIntermediate",
    "rightRingProximal": "rightRingProximal",
    "rightShoulder": "rightShoulder",
    "rightThumbDistal": "rightThumbDistal",
    "rightThumbIntermediate": "rightThumbIntermediate",
    "rightThumbProximal": "rightThumbProximal",
    "rightToes": "rightToes",
    "rightUpperArm": "rightUpperArm",
    "rightUpperLeg": "rightUpperLeg",
    "spine": "spine",
    "upperChest": "upperChest"
  }
}
```

Character animation, physics assets, default materials, and skeleton/AnimInstance classes are assigned through the central asset registry. Hair and secondary-body physical tuning are native runtime/physics-asset settings, not additional JSON properties. Source gravity is authored on Static/Dynamic definitions; attach a Gravity Field Component explicitly if a custom character Blueprint must emit one.

## Sound definitions

A Sound JSON pairs with a same-directory, same-basename WAV:

```json
{
  "AssetType": "Sound",
  "UUID": "648486aa-49d5-4ff6-9c7a-87a2a0c04102",
  "Name": "EngineLoop",
  "DisplayName": "Engine Loop",
  "Version": "1.0.0"
}
```

These five identity fields are the supported sound-definition fields. Do not add `ModelType`, `Bones`, or `ProjectType`. The build stores the definition and WAV bytes as archive members. Playback settings belong to runtime sound calls/components rather than extra fields in this definition. Sound JSON is bounded to 4 MiB, depth 16, 4096 values, and 1024 entries per container.

## User settings: `settings.json`

```json
{
  "Version": "1.0.0",
  "BloomIntensity": 0.675,
  "BloomThreshold": -1,
  "AmbientOcclusionIntensity": 0.5,
  "Exposure": -11,
  "ShadowQuality": 2,
  "TextureQuality": 2,
  "MaxTextureResolution": 768,
  "ViewDistanceQuality": 2,
  "AntiAliasingQuality": 2,
  "PostProcessingQuality": 2,
  "EffectsQuality": 2,
  "FoliageQuality": 2,
  "ShadingQuality": 2,
  "GlobalIlluminationQuality": 2,
  "ReflectionQuality": 2,
  "DynamicGlobalIlluminationMethod": 1,
  "ReflectionMethod": 1,
  "bRayTracing": true,
  "bHeightFog": true,
  "bCloud": true,
  "CelShadingMode": 1
}
```

All `*Quality` fields in this example are integers from 0 (Low) to 3 (Epic). `MaxTextureResolution` is clamped to 64–8192 pixels. Bloom intensity is clamped to 0–8, bloom threshold to -1–20, ambient occlusion intensity to 0–1. Exposure -11 selects histogram auto exposure; -10 through 20 selects manual EV100. `CelShadingMode` is written as 0 or 1. Ray tracing, fog, and clouds are booleans; enabling a setting still requires corresponding project/platform support.

`DynamicGlobalIlluminationMethod`: 0=None, 1=Lumen, 2=ScreenSpace. `ReflectionMethod`: 0=None, 1=Lumen, 2=ScreenSpace. Settings outside supported quality/method ranges are clamped.

Streaming distance and work budgets are derived from `ViewDistanceQuality`; there are no independent user JSON streaming multiplier/budget controls.

| View distance quality | Scene distance multiplier | Object radius (m) | Unload multiplier | Scene spawn budget | Node budget/frame |
| --- | --- | --- | --- | --- | --- |
| 0 | 40 | 1024 | 1.20 | 2 | 48 |
| 1 | 52 | 1536 | 1.17 | 4 | 72 |
| 2 | 64 | 2048 | 1.14 | 6 | 96 |
| 3 | 88 | 3072 | 1.12 | 8 | 144 |

Gravity source residency is independent of these distance limits.

## Interaction helper JSON

`USimulatorInteractionJsonLibrary` parses and emits two standalone struct documents. These are passed to the interaction APIs; they are not implicitly merged into a model definition. The converter uses lower-camel property names and `schemaVersion: 1`.

### Character interaction

```json
{
  "schemaVersion": 1,
  "dominantHand": "Right",
  "rightHandSocket": "hand_r_socket",
  "leftHandSocket": "hand_l_socket"
}
```

`dominantHand` is `Right` or `Left`. Socket strings name the attachment points on the character.

### Equipment interaction

```json
{
  "schemaVersion": 1,
  "bOverridePrimaryHand": false,
  "primaryHandOverride": "Right",
  "rightGrip": {
    "bEnabled": true,
    "hand": "Right",
    "role": "Primary",
    "characterSocket": "hand_r_socket"
  },
  "leftGrip": {
    "bEnabled": false,
    "hand": "Left",
    "role": "Support",
    "characterSocket": "hand_l_socket"
  },
  "heldPreviewLongestDimensionCm": 18
}
```

| Field | Default / meaning |
| --- | --- |
| `bOverridePrimaryHand` | `false`; otherwise use `primaryHandOverride` instead of the character's dominant hand |
| `primaryHandOverride` | `Right`; accepts `Right` or `Left` |
| `rightGrip`, `leftGrip` | Grip descriptions; disabled by default |
| Grip `bEnabled` | `false` |
| Grip `hand` | Sanitized to the containing Right/Left grip |
| Grip `role` | `Primary`; accepts `Disabled`, `Primary`, `Secondary`, `Support` |
| Grip `characterSocket` | No socket by default |
| Grip `attachmentOffset` | Identity `FTransform`; object offset relative to the character socket |
| `heldPreviewLongestDimensionCm` | `18`; clamped to 1–100 cm |

`attachmentOffset` is an Unreal reflected `FTransform` value, not the flat transform object used by model/weapon JSON. For a nonidentity offset, construct the struct in Blueprint/C++ and export with `EquipmentInteractionToJson`; retain the exported transform representation when editing the document. The example omits it to use identity. `CharacterInteractionToJson` provides the corresponding character serializer. Interaction input is bounded to 1 MiB, depth 16, 8192 values, and 1024 entries per container.

## Saved state and embedded custom JSON

`.v3d` and `.dat` are binary archives, not JSON documents to edit with a text editor. The `.v3d` contains the validated project config and model/sound definition snapshots together with decoded runtime resources. Its `gworld://<UUID>` references are runtime identities, not source file paths.

The `.dat` stores world time, selected player, player records, and spatial object records. Object records contain entity/model UUIDs, location, quaternion rotation, scale, linear/angular velocity, and the effective per-placement gravity settings. These settings use the same semantics and validation ranges as `GravityField`.

A player record has `PlayerId`, `DisplayName`, `Location`, `Rotation`, `Health`, `Level`, `PlayerGameMode`, `Items`, and `CustomJson`. These are native saved fields, not a standalone player JSON file. `CustomJson` is an arbitrary JSON object serialized as a bounded embedded string (4 MiB maximum), for example:

```json
{
  "questFlags": { "visitedGravityLab": true },
  "preferredTool": "Scanner"
}
```

The simulator preserves this application payload; it does not automatically interpret those example keys as gameplay rules. Runtime object and player writes are committed through the existing transactional save system. Source assets remain in `.v3d`; they are not duplicated into save records.

## Runtime behaviour and diagnostics

The main menu initializes when its player, viewport, widget class, and widget instance are ready. Failed readiness checks retry at a bounded cadence until success or map exit. Startup diagnostics distinguish class-loading, widget-creation, and viewport-attachment failures. A visible native status message is available while the authored widget is being prepared.

Gravity queries are world-local and game-thread-only. Source references are weak, invalid actors are skipped, and rigid body pointers are acquired for the current step instead of retained across mesh reconstruction or destruction. Physics bodies retain their normal world-gravity setting; field force compensation replaces acceleration without leaving gravity disabled on exit. Unchanged sleeping bodies are not repeatedly awakened. Removing or changing a source re-evaluates previous receivers so they can settle under the new acceleration.

Do not call gameplay UObject/actor APIs from background worker threads. Authoring/build workers operate on detached data, and runtime publication returns to the game thread. The startup, archive, character, and physics tests in `Source/V3DSimulator/Private/Tests` are intended to be run with the project's engine and assets before distribution.
