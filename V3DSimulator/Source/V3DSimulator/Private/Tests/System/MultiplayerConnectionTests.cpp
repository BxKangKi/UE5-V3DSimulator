// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Engine/GameInstance.h"
#include "System/MultiplayerWorldSubSystem.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DRejectedConnectionStateTest,
    "V3DSimulator.Multiplayer.RejectedConnectionPreservesState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FV3DRejectedConnectionStateTest::RunTest(const FString&)
{
    TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>());
    TStrongObjectPtr<UMultiplayerWorldSubSystem> Multiplayer(NewObject<UMultiplayerWorldSubSystem>(Instance.Get()));
    Multiplayer->SetSelectedWorldFolderName(TEXT("previous"));
    // A stale Blueprint callback after teardown must neither select a new world nor enter a
    // permanent Client mode when it has no player/world from which to issue travel.
    TestFalse(TEXT("Missing context is rejected"), Multiplayer->JoinMultiplayerWorld(nullptr, TEXT("192.0.2.1:7777"), TEXT("newworld")));
    TestFalse(TEXT("A game instance without an active player is rejected"), Multiplayer->JoinMultiplayerWorld(Instance.Get(), TEXT("192.0.2.1:7777"), TEXT("newworld")));
    TestTrue(TEXT("Rejected request preserves local mode"), Multiplayer->GetWorldMode() == EMultiplayerWorldMode::SinglePlayer);
    TestEqual(TEXT("Rejected request preserves world selection"), Multiplayer->GetSelectedWorldFolderName(), FString(TEXT("previous")));
    TestFalse(TEXT("No attempt is left pending"), Multiplayer->CanCancelConnection());
    TestFalse(TEXT("Back without an attempt is harmless"), Multiplayer->CancelClientConnection(Instance.Get()));
    return true;
}
#endif
