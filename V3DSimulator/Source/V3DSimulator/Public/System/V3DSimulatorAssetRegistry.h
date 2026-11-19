// Copyright © 2026 BxKangKi. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Model/MaterialDefaultAsset.h"
#include "V3DSimulatorAssetRegistry.generated.h"

class AActor;
class AStaticActor;
class ADynamicActor;
class AVehiclePawn;
class AWeaponActor;
class AWeaponProjectileActor;
class AWorldEnvManager;
class AWaterActor;
class UCreatorHUDWidget;
class UPauseMenuWidget;
class USettingsMenuWidget;
class UBuildStatusWidget;
class UMainWorldWidget;
class UWorldSelectionWidget;
class UProjectSelectionWidget;
class UMaterialInterface;
class UMaterialParameterCollection;
class UStaticMesh;
class USkeletalMesh;
class UPhysicsAsset;
class UPhysicalMaterial;
class USkeleton;
class USoundBase;
class UInputMappingContext;
class UInputAction;
class UAnimInstance;
class UNiagaraSystem;
class UUserWidget;
class UWorld;


USTRUCT(BlueprintType)
struct V3DSIMULATOR_API FV3DSimulatorFootstepBinding
{
    GENERATED_BODY()

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Audio")
    TSoftObjectPtr<UPhysicalMaterial> PhysicalMaterial;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Audio")
    TArray<TSoftObjectPtr<USoundBase>> Sounds;


    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Audio") bool bLoop = false;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Audio")
    TSoftObjectPtr<UNiagaraSystem> Effect;
};

/** One random candidate per playback. Loop never changes the shared source asset. */
USTRUCT(BlueprintType)
struct V3DSIMULATOR_API FV3DDefaultSoundSettings
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Audio") TArray<TSoftObjectPtr<USoundBase>> Sounds;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Audio") bool bLoop = false;
};

USTRUCT(BlueprintType)
struct V3DSIMULATOR_API FV3DSimulatorAudioDefaults
{
    GENERATED_BODY()
    FV3DSimulatorAudioDefaults() { Water.bLoop = Underwater.bLoop = EnvironmentMusic.bLoop = true; }
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Audio") FV3DDefaultSoundSettings Footstep;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Audio") FV3DDefaultSoundSettings Water;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Audio") FV3DDefaultSoundSettings Underwater;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Audio") FV3DDefaultSoundSettings EnvironmentMusic;
    /** Fallback for cooked physical-material assets. Project mappings live in scripts/sounds.json. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Audio") TArray<FV3DSimulatorFootstepBinding> Footsteps;
};

USTRUCT(BlueprintType)
struct V3DSIMULATOR_API FV3DWorldLaunchProfile
{
    GENERATED_BODY()

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="World Launch")
    FString WorldFolderName;

    /** Destination map for local play. Its World Settings owns the local GameMode class. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="World Launch")
    TSoftObjectPtr<UWorld> SinglePlayerWorld;

    /** Destination map for hosting. Assign explicitly; it is not inferred from SinglePlayerWorld. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="World Launch")
    TSoftObjectPtr<UWorld> HostWorld;

};

USTRUCT(BlueprintType)
struct V3DSIMULATOR_API FV3DSimulatorInputMappingContextConfig
{
    GENERATED_BODY()

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputMappingContext> MappingContext;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    int32 Priority = 50;
};

/**
 * Project-wide editor asset registry.
 *
 * The custom GameInstance creates one private transient instance from AssetRegistryClass. Every
 * heavyweight asset/class/world inside the registry is a soft reference, so creating the registry does not make those
 * packages resident. Runtime systems resolve only what they need and keep a reflected strong
 * reference for exactly as long as the resolved asset is in use.
 */
UCLASS(BlueprintType, Blueprintable)
class V3DSIMULATOR_API UV3DSimulatorAssetRegistry : public UDataAsset
{
    GENERATED_BODY()

public:
    UV3DSimulatorAssetRegistry(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

    /** Supplies current built-in menu classes when a class reference is empty. */
    void EnsureMenuDefaults();

    /** Synchronously resolves TerrainMaterial on the game thread. */
    UMaterialInterface* ResolveTerrainMaterial() const;

    /** Loaded once by the world surface subsystem; the registry retains only the soft path. */
    UPhysicalMaterial* ResolveDefaultPhysicalMaterial() const;

    /** Fallback for runtime meshes without a project mapping or authored physical material. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Physics")
    TSoftObjectPtr<UPhysicalMaterial> DefaultPhysicalMaterial;

    // Native actor classes can be replaced by Blueprint subclasses without forcing them to load at startup.
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Actors")
    TSoftClassPtr<AStaticActor> StaticActorClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Actors")
    TSoftClassPtr<ADynamicActor> DynamicActorClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Actors")
    TSoftClassPtr<AVehiclePawn> VehiclePawnClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Actors")
    TSoftClassPtr<AWeaponActor> WeaponActorClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Actors")
    TSoftClassPtr<AWeaponProjectileActor> WeaponProjectileActorClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Actors")
    TSoftClassPtr<AWorldEnvManager> WorldEnvManagerClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Actors")
    TSoftClassPtr<AWaterActor> WaterActorClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Actors")
    TSoftClassPtr<AActor> RainWeatherActorClass;

    // Only screen classes are configurable assets. Reusable buttons/settings rows are native
    // components, so opening a menu never synchronously loads separate component Blueprints.
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="UI")
    TSoftClassPtr<UMainWorldWidget> StartMenuWidgetClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="UI")
    TSoftClassPtr<UWorldSelectionWidget> WorldSelectionWidgetClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="UI")
    TSoftClassPtr<UMainWorldWidget> MultiplayerMenuWidgetClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="UI")
    TSoftClassPtr<UProjectSelectionWidget> ProjectSelectionWidgetClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="UI")
    TSoftClassPtr<UPauseMenuWidget> PauseMenuWidgetClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="UI")
    TSoftClassPtr<USettingsMenuWidget> SettingsMenuWidgetClass;

    /** WBP subclass of UBuildStatusWidget shown while a Projects entry is being built. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="UI")
    TSoftClassPtr<UBuildStatusWidget> BuildStatusWidgetClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="UI")
    TSoftClassPtr<UCreatorHUDWidget> CreatorHUDWidgetClass;

    /** Use a Blueprint child of UDebugHUDWidget; assign the current native HUD parent. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="UI")
    TSoftClassPtr<UUserWidget> DebugWidgetClass;

    /** Use a Blueprint child of ULoadingHUDWidget; assign the current native HUD parent. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="UI")
    TSoftClassPtr<UUserWidget> LoadingWidgetClass;

    // Menu/navigation worlds. Soft references prevent menu maps from being resident in gameplay.
    // GameMode classes are deliberately not registered here; each destination map owns that choice.
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Worlds")
    TSoftObjectPtr<UWorld> GameplayWorld;

    /** Host map. Assign the same map explicitly only if that map intentionally uses a unified GameMode. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Worlds")
    TSoftObjectPtr<UWorld> HostWorld;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Worlds")
    TSoftObjectPtr<UWorld> ClientWorld;

    /** Single MainWorld. Set its menu GameMode explicitly in the map's World Settings. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Worlds")
    TSoftObjectPtr<UWorld> MainWorld;

    /** Optional per-world map overrides. Each referenced map owns its GameMode through World Settings. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Worlds", meta=(TitleProperty="WorldFolderName"))
    TArray<FV3DWorldLaunchProfile> WorldLaunchProfiles;

    // Rendering/default assets.
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Rendering")
    TSoftObjectPtr<UMaterialInterface> PlacementGridMaterial;

    /** MI_Opaque / MI_Masked / MI_Translucent / MI_Toon. Dynamic models share these parents and set MID FadeDistance=50. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Rendering")
    FV3DMaterialSoftAssetReferences V3DMaterials;

    /** Base material used by baked V3D terrain palettes (Texture2DArray + vertex-color layer weights). */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Rendering")
    TSoftObjectPtr<UMaterialInterface> TerrainMaterial;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Rendering")
    TSoftObjectPtr<UMaterialInterface> StaticDecalLightMaterial;

    /** Underwater-only post-process material for the global ocean. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Rendering")
    TSoftObjectPtr<UMaterialInterface> OceanPostProcessMaterial;

    /** Shared shader parameter collection used by global post-process/environment shaders. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Rendering")
    TSoftObjectPtr<UMaterialParameterCollection> ShaderLibraryMPC;


    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Environment")
    TSoftObjectPtr<UStaticMesh> SkyboxMesh;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Environment")
    TSoftObjectPtr<UMaterialInterface> CloudMaterial;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Water")
    TSoftObjectPtr<UStaticMesh> WaterMesh;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Water")
    TSoftObjectPtr<UMaterialInterface> WaterDecalMaterial;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Water")
    TSoftObjectPtr<UMaterialInterface> UnderWaterMaterial;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Weapon")
    TSoftObjectPtr<UStaticMesh> DefaultWeaponMesh;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Character")
    TSoftObjectPtr<USkeletalMesh> DefaultCharacterSkeletalMesh;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Character")
    TSoftObjectPtr<UPhysicsAsset> DefaultCharacterPhysicsAsset;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Character")
    TSoftObjectPtr<USkeleton> DefaultCharacterSkeleton;


    /** Animation Blueprint class applied to the character mesh. The class is loaded only when a character begins play. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Character")
    TSoftClassPtr<UAnimInstance> DefaultCharacterAnimInstanceClass;

    /** All cooked sound defaults. Project-specific surfaces/events are configured in sounds.json. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Audio", meta=(DisplayName="Sound Defaults"))
    FV3DSimulatorAudioDefaults AudioDefaults;

    // Enhanced Input is centralized for the same reason: controller Blueprints no longer hard-reference assets.
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputMappingContext> PrimaryInputMappingContext;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    int32 PrimaryInputMappingPriority = 50;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TArray<FV3DSimulatorInputMappingContextConfig> AdditionalInputMappingContexts;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> MoveAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> LookAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> JumpAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> SprintAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> CrouchAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> FlyAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> RagdollAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> InteractAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> ToggleFirstPersonAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> ChangeCharacterAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> ToolbarScrollAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> ToggleItemListAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> SnapAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> VehicleMoveAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> VehicleThrottleAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> VehicleSteeringAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> VehicleStopAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> PauseAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Input")
    TSoftObjectPtr<UInputAction> DebugAction;
};
