// Copyright © 2026 BxKangKi. Licensed under the MIT License.

/**
 * @file PlayerData.cpp
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * UObject and Actor access stays on the game thread; worker tasks receive detached native data only.
 */

#include "World/PlayerData.h"
#include "System/MacroLibrary.h"
#include "System/JsonMetadata.h"

UPlayerData::UPlayerData()
{
    Version = V3DSimulatorJsonMetadata::SchemaVersion;
}

FWorldPlayerRecord* UPlayerData::FindPlayer(const FString& PlayerId)
{
    return Players.FindByPredicate([&PlayerId](const FWorldPlayerRecord& Record)
    {
        return Record.PlayerId.Equals(PlayerId, ESearchCase::IgnoreCase);
    });
}

const FWorldPlayerRecord* UPlayerData::FindPlayer(const FString& PlayerId) const
{
    return Players.FindByPredicate([&PlayerId](const FWorldPlayerRecord& Record)
    {
        return Record.PlayerId.Equals(PlayerId, ESearchCase::IgnoreCase);
    });
}

FWorldPlayerRecord& UPlayerData::FindOrAddPlayer(const FString& PlayerId)
{
    const FString SafeId = PlayerId.IsEmpty() ? FString(TEXT("Player")) : PlayerId;
    if (FWorldPlayerRecord* Existing = FindPlayer(SafeId))
    {
        return *Existing;
    }

    FWorldPlayerRecord& NewRecord = Players.AddDefaulted_GetRef();
    NewRecord.PlayerId = SafeId;
    NewRecord.DisplayName = SafeId;
    NewRecord.CustomJson = MakeShared<FJsonObject>();
    return NewRecord;
}
