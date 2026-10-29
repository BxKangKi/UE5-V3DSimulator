// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Simulator/ModelDefinitionJson.h"
#include "System/GlbValidation.h"
#include "System/SourceModelImport.h"
#include "System/WorldBakedData.h"
#include "SourceModelImportTestLibrary.h"
#include "UObject/StrongObjectPtr.h"
#include "glTFRuntimeAsset.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace SourceModelImportTestsPrivate
{
    struct FFiles
    {
        FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"),
            TEXT("SourceImport-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
        FFiles() { IFileManager::Get().MakeDirectory(*Root, true); }
        ~FFiles() { IFileManager::Get().DeleteDirectory(*Root, false, true); }
        FString Write(const TCHAR* Name, const FString& Text) const
        {
            const FString Path = FPaths::Combine(Root, Name);
            FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
            return Path;
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DSourceFormatDispatchTest,
    "V3DSimulator.Project.SourceImport.FormatDispatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DSourceFormatDispatchTest::RunTest(const FString&)
{
    using namespace V3DSourceModelImport;
    TestTrue(TEXT("GLB is native glTF"), GetFormat(TEXT("/models/Room.GLB")) == EFormat::Glb);
    TestTrue(TEXT("glTF JSON is discovered"), GetFormat(TEXT("/models/Room.gLtF")) == EFormat::Gltf);
    TestTrue(TEXT("FBX case folding"), GetFormat(TEXT("Room.FbX")) == EFormat::Fbx);
    TestTrue(TEXT("OBJ case folding"), GetFormat(TEXT("Room.OBJ")) == EFormat::Obj);
    TestTrue(TEXT("STL case folding"), GetFormat(TEXT("Room.STL")) == EFormat::Stl);
    TestFalse(TEXT("Sibling metadata is not a model"), IsModelSource(TEXT("Room.json")));
    TestFalse(TEXT("Arbitrary renamed formats are not enabled"), IsModelSource(TEXT("Room.blend")));
    TestFalse(TEXT("GLB must use the glTF parser"), UsesBlob(EFormat::Glb));
    TestTrue(TEXT("FBX must use Blob mode"), UsesBlob(EFormat::Fbx));
    TestEqual(TEXT("OBJ names its optional module"), FString(RequiredPlugin(EFormat::Obj)), FString(TEXT("glTFRuntimeOBJ")));

    SourceModelImportTestsPrivate::FFiles Files;
    const FString Obj = Files.Write(TEXT("Triangle.OBJ"), TEXT("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"));
    FString Error;
    TestTrue(TEXT("OBJ preflight never expects a GLB magic header"), ValidateSourceFile(Obj, Error));
    TestFalse(TEXT("Renaming OBJ bytes to GLB fails GLB validation"),
        ValidateSourceFile(Files.Write(TEXT("Wrong.glb"), TEXT("v 0 0 0\nv 1 0 0\nf 1 2 3\n")), Error));
    if (!IPluginManager::Get().FindPlugin(TEXT("glTFRuntimeOBJ")).IsValid())
    {
        TestFalse(TEXT("Missing extension is reported before loading a parser"), CheckAvailability(Obj, Error));
        TestTrue(TEXT("Missing extension error identifies the required plugin"), Error.Contains(TEXT("glTFRuntimeOBJ")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DGltfExternalResourceTest,
    "V3DSimulator.Project.SourceImport.GltfResources",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DGltfExternalResourceTest::RunTest(const FString&)
{
    SourceModelImportTestsPrivate::FFiles Files;
    Files.Write(TEXT("mesh.bin"), TEXT("1234"));
    Files.Write(TEXT("image.png"), TEXT("bounded image bytes"));
    Files.Write(TEXT("image space.png"), TEXT("bounded image bytes"));
    const FString Valid = TEXT("{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"mesh.bin\",\"byteLength\":4}],\"images\":[{\"uri\":\"image.png\"}]}");
    FString Error;
    const FString Source = Files.Write(TEXT("Room.GLTF"), Valid);
    TestTrue(TEXT("External glTF buffers/images are supported"), V3DSourceModelImport::ValidateSourceFile(Source, Error));
    TestTrue(TEXT("Legacy build preflight dispatches .gltf without a GLB header"), GlbValidation::ValidateBuildSourceFile(Source, Error));
    TestFalse(TEXT("Missing external buffer fails before native decoding"),
        V3DSourceModelImport::ValidateSourceFile(Files.Write(TEXT("Missing.gltf"), Valid.Replace(TEXT("mesh.bin"), TEXT("missing.bin"))), Error));
    TestFalse(TEXT("Undersized buffer is rejected"),
        V3DSourceModelImport::ValidateSourceFile(Files.Write(TEXT("Short.gltf"), Valid.Replace(TEXT("byteLength\":4"), TEXT("byteLength\":40"))), Error));
    TestFalse(TEXT("Parent path escape stays rejected"),
        V3DSourceModelImport::ValidateSourceFile(Files.Write(TEXT("Escape.gltf"), Valid.Replace(TEXT("mesh.bin"), TEXT("../mesh.bin"))), Error));
    TestFalse(TEXT("Remote resources stay rejected"),
        V3DSourceModelImport::ValidateSourceFile(Files.Write(TEXT("Remote.gltf"), Valid.Replace(TEXT("image.png"), TEXT("https://example.invalid/image.png"))), Error));
    TestTrue(TEXT("Image URL decoding matches glTFRuntime for names containing spaces"),
        V3DSourceModelImport::ValidateSourceFile(Files.Write(TEXT("Encoded.gltf"), Valid.Replace(TEXT("image.png"), TEXT("image%20space.png"))), Error));
    TestFalse(TEXT("Encoded image parent escapes stay rejected"),
        V3DSourceModelImport::ValidateSourceFile(Files.Write(TEXT("EncodedEscape.gltf"), Valid.Replace(TEXT("image.png"), TEXT("%2e%2e/image.png"))), Error));
    IFileManager::Get().Delete(*FPaths::Combine(Files.Root, TEXT("mesh.bin")));
    TestFalse(TEXT("External changes cannot reuse a main-source-only validation cache"), V3DSourceModelImport::ValidateSourceFile(Source, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DMultiFormatDiscoveryTest,
    "V3DSimulator.Project.SourceImport.RecursiveDiscovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DMultiFormatDiscoveryTest::RunTest(const FString&)
{
    SourceModelImportTestsPrivate::FFiles Files;
    for (const TCHAR* Name : { TEXT("Scene.GLB"), TEXT("Room.gLtF"), TEXT("Chair.OBJ"), TEXT("Rig.FBX"), TEXT("Print.stl") })
        Files.Write(Name, TEXT("source preflight is a separate stage"));
    Files.Write(TEXT("Notes.txt"), TEXT("not a model"));
    TArray<FString> Sources;
    TestTrue(TEXT("All known formats generate sibling metadata even before optional plugins are installed"),
        ModelDefinitionJson::EnsureDefinitions(Files.Root, EV3DSimulatorProjectType::World, nullptr, &Sources));
    TestEqual(TEXT("Five model sources are discovered once"), Sources.Num(), 5);
    for (const FString& Path : Sources)
        TestTrue(TEXT("Every source receives its own sidecar"), IFileManager::Get().FileExists(*FPaths::ChangeExtension(Path, TEXT("json"))));
    // Two formats with the same basename would otherwise share one UUID and overwrite each
    // other's authoring definition. Detect it before mutating either sidecar.
    Files.Write(TEXT("Chair.fbx"), TEXT("second source"));
    AddExpectedError(TEXT("Model sidecar collision"), EAutomationExpectedErrorFlags::Contains, 1);
    TestFalse(TEXT("Cross-format sidecar collisions are rejected"),
        ModelDefinitionJson::EnsureDefinitions(Files.Root, EV3DSimulatorProjectType::World, nullptr, &Sources));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DReflectedSourceDecoderTest,
    "V3DSimulator.Project.SourceImport.ReflectedDecoderABI",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DReflectedSourceDecoderTest::RunTest(const FString&)
{
    TStrongObjectPtr<UglTFRuntimeAsset> Asset(NewObject<UglTFRuntimeAsset>());
    FglTFRuntimeMaterialsConfig Materials;
    Materials.SpecularFactor = 0.25f;
    FglTFRuntimeMeshLOD LOD;
    FString Error;
    TestTrue(TEXT("Unknown library function name is resolved by compatible input/output types"),
        V3DSourceModelImport::DecodeWithLibraryForTesting(UV3DSourceImportCompatibleTestLibrary::StaticClass(),
            Asset.Get(), Materials, LOD, Error));
    TestTrue(TEXT("Successful API fallback clears the failed preferred-name error"), Error.IsEmpty());
    TestEqual(TEXT("Native LOD arrays survive reflected parameter ownership transfer"), LOD.Primitives.Num(), 1);
    if (!LOD.Primitives.IsEmpty())
        TestTrue(TEXT("Both static/skeletal material configs and the asset argument reach the native decoder"),
            LOD.Primitives[0].Positions.Num() == 1 && LOD.Primitives[0].Positions[0] == FVector(123.0, 456.0, 789.0));
    TestFalse(TEXT("Ambiguous decoder candidates are never invoked"),
        V3DSourceModelImport::DecodeWithLibraryForTesting(UV3DSourceImportAmbiguousTestLibrary::StaticClass(),
            Asset.Get(), Materials, LOD, Error));
    TestTrue(TEXT("Ambiguity is diagnosed"), Error.Contains(TEXT("multiple compatible")));
    TestFalse(TEXT("A required unsupported scalar is never guessed"),
        V3DSourceModelImport::DecodeWithLibraryForTesting(UV3DSourceImportRequiredArgTestLibrary::StaticClass(),
            Asset.Get(), Materials, LOD, Error));
    TestTrue(TEXT("Incompatible argument is named"), Error.Contains(TEXT("RequiredMeshIndex")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DImportedHierarchyTest,
    "V3DSimulator.Project.SourceImport.HierarchySafety",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DImportedHierarchyTest::RunTest(const FString&)
{
    TArray<FglTFRuntimeNode> Nodes;
    Nodes.SetNum(20000);
    for (int32 Index = 0; Index < Nodes.Num(); ++Index)
    {
        Nodes[Index].Index = Index;
        Nodes[Index].ParentIndex = Index + 1 < Nodes.Num() ? Index + 1 : INDEX_NONE;
    }
    FString Error;
    TestTrue(TEXT("Deep parent chains validate iteratively without recursion"), V3DSourceModelImport::ValidateHierarchy(Nodes, Error));
    Nodes.Last().ParentIndex = 0;
    TestFalse(TEXT("A multi-node cycle fails before character traversal"), V3DSourceModelImport::ValidateHierarchy(Nodes, Error));
    TestTrue(TEXT("Cycle error identifies the hierarchy"), Error.Contains(TEXT("cycle")));
    Nodes.Last().ParentIndex = INDEX_NONE;
    Nodes[0].ParentIndex = 999999;
    TestFalse(TEXT("Missing parent fails before decoding"), V3DSourceModelImport::ValidateHierarchy(Nodes, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DExtensionSkinCaptureTest,
    "V3DSimulator.Project.SourceImport.ExtensionSkinCapture",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DExtensionSkinCaptureTest::RunTest(const FString&)
{
    V3DSourceModelImport::FSession Session(V3DSourceModelImport::EFormat::Fbx);
    FglTFRuntimeMeshLOD LOD;
    FglTFRuntimeBone& Root = LOD.Skeleton.AddDefaulted_GetRef();
    Root.BoneName = TEXT("Root"); Root.Transform = FTransform::Identity;
    FglTFRuntimeBone& Child = LOD.Skeleton.AddDefaulted_GetRef();
    Child.BoneName = TEXT("Child"); Child.ParentIndex = 0;
    Child.Transform = FTransform(FVector(0.0, 0.0, 10.0));
    FglTFRuntimePrimitive& Primitive = LOD.Primitives.AddDefaulted_GetRef();
    Primitive.Joints.SetNum(1);
    Primitive.Joints[0].AddDefaulted();
    Primitive.Joints[0][0].X = 1;
    Primitive.Weights.SetNum(1);
    Primitive.Weights[0].Add(FVector4(1.0, 0.0, 0.0, 0.0));
    FGWorldBakedSkin Skin;
    FString Error;
    TestTrue(TEXT("Extension LOD skeleton is captured without asking a blob parser for glTF skin JSON"),
        Session.CaptureSkin(LOD, 7, TMap<FString, FString>(), nullptr, Skin, Error));
    TestEqual(TEXT("Build-local skin id is preserved"), Skin.SkinIndex, 7);
    TestEqual(TEXT("Both reference bones are preserved"), Skin.Bones.Num(), 2);
    const FName* Bone = Primitive.OverrideBoneMap.Find(1);
    TestTrue(TEXT("Empty per-primitive maps use glTFRuntime's LOD-skeleton index namespace"), Bone && *Bone == FName(TEXT("Child")));
    LOD.Skeleton[1].ParentIndex = INDEX_NONE;
    TestFalse(TEXT("Multiple roots cannot produce a corrupt baked skeleton"),
        Session.CaptureSkin(LOD, 7, TMap<FString, FString>(), nullptr, Skin, Error));
    return true;
}
#endif
