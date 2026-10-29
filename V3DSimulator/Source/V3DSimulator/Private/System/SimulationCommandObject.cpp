// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#include "System/SimulationCommandObject.h"
#include "System/SafeFileIO.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

bool USimulationCommandObject::InitializeFromJson(const FString& Json, FString& Error)
{
    Error.Reset();
    if (bInstalled) { Error = TEXT("Installed commands are immutable; construct a new command object."); return false; }
    FSafeJsonLimits Limits; Limits.MaxFileBytes = 128 * 1024; Limits.MaxDepth = 8; Limits.MaxValues = 10000;
    const auto Parsed = FSafeFileIO::ParseJsonText(Json, TEXT("command object"), Limits);
    if (!Parsed.IsSuccess()) { Error = Parsed.Error; return false; }
    FV3DCommandDefinition NewDefinition;
    if (!FV3DSimulationCommandConfig::ParseCommand(Parsed.JsonObject, NewDefinition, Error)) return false;
    Definition = MoveTemp(NewDefinition); return true;
}

FString USimulationCommandObject::ToJson() const
{
    if (!IsInitialized()) return FString();
    auto Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("Name"), Definition.Name.ToString());
    Json->SetBoolField(TEXT("AllowChat"), Definition.bAllowChat);
    Json->SetNumberField(TEXT("Cooldown"), Definition.Cooldown);
    TArray<TSharedPtr<FJsonValue>> Actions;
    auto Vector = [](const TSharedRef<FJsonObject>& Object, const TCHAR* Key, const FVector& Value)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Add(MakeShared<FJsonValueNumber>(Value.X));
        Values.Add(MakeShared<FJsonValueNumber>(Value.Y));
        Values.Add(MakeShared<FJsonValueNumber>(Value.Z));
        Object->SetArrayField(Key, Values);
    };
    for (const auto& Action : Definition.Actions)
    {
        auto Object = MakeShared<FJsonObject>();
        if (Action.Type != EV3DCommandAction::TimeScale) Object->SetStringField(TEXT("Target"), Action.Target);
        switch (Action.Type)
        {
        case EV3DCommandAction::Transform:
            Object->SetStringField(TEXT("Type"), TEXT("transform"));
            Object->SetBoolField(TEXT("Relative"), Action.bRelative);
            if (Action.bLocation) Vector(Object, TEXT("Location"), Action.Location);
            if (Action.bRotation) Vector(Object, TEXT("Rotation"), Action.Rotation);
            if (Action.bScale) Vector(Object, TEXT("Scale"), Action.Scale);
            break;
        case EV3DCommandAction::Velocity:
            Object->SetStringField(TEXT("Type"), TEXT("velocity"));
            Vector(Object, TEXT("Linear"), Action.Linear);
            if (Action.bAngular) Vector(Object, TEXT("Angular"), Action.Angular);
            break;
        case EV3DCommandAction::Speed:
            Object->SetStringField(TEXT("Type"), TEXT("speed")); Object->SetNumberField(TEXT("Value"), Action.Value); break;
        case EV3DCommandAction::TimeScale:
            Object->SetStringField(TEXT("Type"), TEXT("time_scale")); Object->SetNumberField(TEXT("Value"), Action.Value); break;
        }
        Actions.Add(MakeShared<FJsonValueObject>(Object));
    }
    Json->SetArrayField(TEXT("Actions"), Actions);
    FString Text; const auto Writer = TJsonWriterFactory<>::Create(&Text);
    return FJsonSerializer::Serialize(Json, Writer) ? Text : FString();
}
