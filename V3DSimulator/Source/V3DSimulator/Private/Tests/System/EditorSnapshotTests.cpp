// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "System/ProjectEditorDocument.h"
#include "System/SafeFileIO.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DEditorSnapshotIsolationTest,
    "V3DSimulator.Editor.SnapshotsAndCachedDefaultsAreIndependent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DEditorSnapshotIsolationTest::RunTest(const FString&)
{
    const auto Parsed = FSafeFileIO::ParseJsonText(TEXT(R"JSON({
        "Nested":{"Name":"original","Flag":true},
        "Values":[{"Value":2},"text",null,3.25,false]
    })JSON"), TEXT("snapshot-test"));
    if (!TestTrue(TEXT("Fixture parses"), Parsed.IsSuccess())) return false;
    const auto Copy = V3DProjectEditor::CloneObject(Parsed.JsonObject);
    TestEqual(TEXT("Clone retains all value types"), V3DProjectEditor::Serialize(Copy), V3DProjectEditor::Serialize(Parsed.JsonObject));
    Copy->GetObjectField(TEXT("Nested"))->SetStringField(TEXT("Name"), TEXT("copy"));
    Copy->GetArrayField(TEXT("Values"))[0]->AsObject()->SetNumberField(TEXT("Value"), 99);
    TestEqual(TEXT("Nested object is detached"), Parsed.JsonObject->GetObjectField(TEXT("Nested"))->GetStringField(TEXT("Name")), FString(TEXT("original")));
    TestEqual(TEXT("Nested array object is detached"), Parsed.JsonObject->GetArrayField(TEXT("Values"))[0]->AsObject()->GetNumberField(TEXT("Value")), 2.0);
    Parsed.JsonObject->GetObjectField(TEXT("Nested"))->SetBoolField(TEXT("Flag"), false);
    TestTrue(TEXT("Worker snapshot cannot see later UI changes"), Copy->GetObjectField(TEXT("Nested"))->GetBoolField(TEXT("Flag")));
    TestFalse(TEXT("Invalid document stays invalid"), V3DProjectEditor::CloneObject(nullptr).IsValid());

    auto First = V3DProjectEditor::EntryTemplate(TEXT("FootstepMappings"));
    const FString Default = V3DProjectEditor::Serialize(First);
    First->SetNumberField(TEXT("Volume"), 99);
    First->SetArrayField(TEXT("Sounds"), {MakeShared<FJsonValueString>(TEXT("modified"))});
    auto Second = V3DProjectEditor::EntryTemplate(TEXT("FootstepMappings"));
    TestEqual(TEXT("Editing defaults never mutates schema cache"), V3DProjectEditor::Serialize(Second), Default);

    FV3DEditorDocument Doc;
    Doc.Path = TEXT("config.json"); Doc.Json = MakeShared<FJsonObject>();
    Doc.Json->SetStringField(TEXT("ProjectType"), TEXT("World"));
    auto Suggestions = V3DProjectEditor::Suggestions(Doc);
    const FString OriginalAudio = V3DProjectEditor::Serialize(Suggestions->GetObjectField(TEXT("Audio")));
    Suggestions->GetObjectField(TEXT("Audio"))->SetStringField(TEXT("DefaultFootstep"), TEXT("none"));
    auto Fresh = V3DProjectEditor::Suggestions(Doc);
    TestEqual(TEXT("Suggested nested fields stay independent across documents"),
        V3DProjectEditor::Serialize(Fresh->GetObjectField(TEXT("Audio"))), OriginalAudio);
    return true;
}
#endif
