// M16-K.5F: automation tests for the shared VertexMaskForgeColorConversion module -- proves the exact
// numeric contract (plain division/multiplication by 255.0f, FMath::RoundToInt, Clamp to [0,255], no
// sRGB/linear transform) that both real callers (SVertexMaskForgePanel.cpp,
// VertexMaskForgeDisplayColorDerivation.cpp) now rely on after their own local duplicates were removed.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VertexMaskForgeColorConversion.h"

// 1. FColorToLinearColorFMatchesExplicitExpectedValues.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeColorConversionToLinearColorFTest, "VertexMaskForge.ColorConversion.FColorToLinearColorFMatchesExplicitExpectedValues", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeColorConversionToLinearColorFTest::RunTest(const FString& Parameters)
{
	// FColor(0, 64, 128, 255) -> explicit expected values, not re-derived from the implementation itself.
	const FVector4f Result = VertexMaskForgeColorConversion::ToLinearColorF(FColor(0, 64, 128, 255));

	TestEqual(TEXT("R == 0.0f"), Result.X, 0.0f);
	TestEqual(TEXT("G == 64.0f/255.0f"), Result.Y, 64.0f / 255.0f);
	TestEqual(TEXT("B == 128.0f/255.0f"), Result.Z, 128.0f / 255.0f);
	TestEqual(TEXT("A == 1.0f"), Result.W, 1.0f);

	return true;
}

// 2. RoundTripThroughAllByteValuesIsExact: for every uint8 V (0..255), ToDisplayFColor(ToLinearColorF(
// FColor(V,V,V,V))) == FColor(V,V,V,V) -- proves byte-exact preservation across the entire domain for
// RGB and Alpha alike, without duplicating the implementation to compute the expected value.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeColorConversionRoundTripTest, "VertexMaskForge.ColorConversion.RoundTripThroughAllByteValuesIsExact", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeColorConversionRoundTripTest::RunTest(const FString& Parameters)
{
	for (int32 V = 0; V <= 255; ++V)
	{
		const uint8 Byte = static_cast<uint8>(V);
		const FColor Original(Byte, Byte, Byte, Byte);
		const FVector4f Linear = VertexMaskForgeColorConversion::ToLinearColorF(Original);
		const FColor RoundTripped = VertexMaskForgeColorConversion::ToDisplayFColor(Linear);

		if (RoundTripped != Original)
		{
			AddError(FString::Printf(TEXT("Round-trip mismatch at V=%d: got (%d,%d,%d,%d), expected (%d,%d,%d,%d)"),
				V, RoundTripped.R, RoundTripped.G, RoundTripped.B, RoundTripped.A, Byte, Byte, Byte, Byte));
			return false;
		}
	}

	return true;
}

// 3. OutOfRangeChannelsClampToByteBounds: values below 0.0 and above 1.0 clamp to the 0/255 byte bounds.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeColorConversionClampTest, "VertexMaskForge.ColorConversion.OutOfRangeChannelsClampToByteBounds", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeColorConversionClampTest::RunTest(const FString& Parameters)
{
	const FColor Low = VertexMaskForgeColorConversion::ToDisplayFColor(FVector4f(-0.5f, -100.0f, -1.0f, -0.001f));
	TestEqual(TEXT("Below-zero R clamps to 0"), Low.R, static_cast<uint8>(0));
	TestEqual(TEXT("Below-zero G clamps to 0"), Low.G, static_cast<uint8>(0));
	TestEqual(TEXT("Below-zero B clamps to 0"), Low.B, static_cast<uint8>(0));
	TestEqual(TEXT("Below-zero A clamps to 0"), Low.A, static_cast<uint8>(0));

	const FColor High = VertexMaskForgeColorConversion::ToDisplayFColor(FVector4f(1.5f, 2.0f, 100.0f, 1.001f));
	TestEqual(TEXT("Above-one R clamps to 255"), High.R, static_cast<uint8>(255));
	TestEqual(TEXT("Above-one G clamps to 255"), High.G, static_cast<uint8>(255));
	TestEqual(TEXT("Above-one B clamps to 255"), High.B, static_cast<uint8>(255));
	TestEqual(TEXT("Above-one A clamps to 255"), High.A, static_cast<uint8>(255));

	return true;
}

// 4. RoundingFollowsRoundToIntExactly: unambiguous values around known byte boundaries follow
// FMath::RoundToInt(Value * 255.0f) precisely (explicit expected bytes, not re-derived).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeColorConversionRoundingTest, "VertexMaskForge.ColorConversion.RoundingFollowsRoundToIntExactly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeColorConversionRoundingTest::RunTest(const FString& Parameters)
{
	// 0.5f * 255.0f = 127.5 -> RoundToInt rounds half-to-even-away-from-zero per FMath::RoundToInt's own
	// contract (127.5 -> 128).
	const FColor Half = VertexMaskForgeColorConversion::ToDisplayFColor(FVector4f(0.5f, 0.5f, 0.5f, 0.5f));
	TestEqual(TEXT("0.5f rounds to byte 128"), Half.R, static_cast<uint8>(128));

	// 100.0f/255.0f * 255.0f == 100.0f exactly (no rounding ambiguity) -> byte 100.
	const FColor Exact = VertexMaskForgeColorConversion::ToDisplayFColor(FVector4f(100.0f / 255.0f, 100.0f / 255.0f, 100.0f / 255.0f, 100.0f / 255.0f));
	TestEqual(TEXT("100/255 round-trips to byte 100"), Exact.R, static_cast<uint8>(100));

	// Just below the 1/255 threshold for byte 1 (0.5/255 -> 0.5 -> rounds to 0 via round-half-away-from-zero...
	// use an unambiguous just-below-half value instead to avoid relying on rounding-mode edge behavior.
	const FColor JustBelowOne = VertexMaskForgeColorConversion::ToDisplayFColor(FVector4f(0.4f / 255.0f, 0.4f / 255.0f, 0.4f / 255.0f, 0.4f / 255.0f));
	TestEqual(TEXT("0.4/255 rounds down to byte 0"), JustBelowOne.R, static_cast<uint8>(0));

	const FColor JustAboveHalf = VertexMaskForgeColorConversion::ToDisplayFColor(FVector4f(1.6f / 255.0f, 1.6f / 255.0f, 1.6f / 255.0f, 1.6f / 255.0f));
	TestEqual(TEXT("1.6/255 rounds up to byte 2"), JustAboveHalf.R, static_cast<uint8>(2));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
