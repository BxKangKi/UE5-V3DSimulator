// Copyright © 2026 BxKangKi. Licensed under the MIT License.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Vehicle/V3DVehicleSuspensionPolicy.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FV3DVehicleSuspensionDepthTest,
    "V3DSimulator.Vehicle.SuspensionDepth",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FV3DVehicleSuspensionDepthTest::RunTest(const FString&)
{
    using namespace V3DVehicleSuspensionPolicy;
    TestEqual(TEXT("No hard-stop force at ride height"), BottomOutDepth(20.0f, 40.0f), 0.0f);
    TestEqual(TEXT("Bottom-out uses actual penetration"), BottomOutDepth(20.0f, 12.0f), 8.0f);
    TestEqual(TEXT("Negative trace lengths retain penetration"), BottomOutDepth(20.0f, -3.0f), 23.0f);
    TestEqual(TEXT("No constant step assist at rest"), StepCompressionDepth(40.0f, 40.0f), 0.0f);
    TestEqual(TEXT("Trace noise does not create step assist"), StepCompressionDepth(40.0f, 39.5f), 0.0f);
    TestTrue(TEXT("Deep compression enables step assist"), StepCompressionDepth(40.0f, 25.0f) > 0.0f);
    TestEqual(TEXT("Configured authored rest geometry"), AuthoredRestLength(30.0f, 80.0f, 20.0f, 30.0f, 0.75f), 60.0f);
    TestEqual(TEXT("Too-short suspension reserves compression and droop"), AuthoredRestLength(30.0f, 1.0f, 20.0f, 30.0f, 0.75f), 44.25f);
    return true;
}
#endif
