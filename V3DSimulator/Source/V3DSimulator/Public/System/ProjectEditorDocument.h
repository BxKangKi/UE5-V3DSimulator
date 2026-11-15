// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** Authoring-only document. No transforms are applied to runtime actors. */
struct V3DSIMULATOR_API FV3DEditorDocument
{
    FString Path, SourcePath, Label, DiskText;
    TSharedPtr<FJsonObject> Json;
    bool bDirty = false;
    TArray<FString> Undo;
};
namespace V3DProjectEditor
{
    V3DSIMULATOR_API bool Discover(const FString& Project, TArray<TSharedPtr<FV3DEditorDocument>>& Documents, FString& Error);
    V3DSIMULATOR_API bool Validate(const FV3DEditorDocument& Document, const FString& Project, FString& Error, const TArray<FV3DEditorDocument>* Batch = nullptr);
    V3DSIMULATOR_API TSharedPtr<FJsonObject> Suggestions(const FV3DEditorDocument& Document);
    V3DSIMULATOR_API TSharedPtr<FJsonObject> EntryTemplate(const FString& Container);
    V3DSIMULATOR_API FString Serialize(const TSharedPtr<FJsonObject>& Json);
    V3DSIMULATOR_API bool Save(FV3DEditorDocument& Document, const FString& Project, FString& Error, const TArray<FV3DEditorDocument>* Batch = nullptr);
}
