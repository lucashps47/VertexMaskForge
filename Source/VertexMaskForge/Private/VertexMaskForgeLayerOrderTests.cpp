// M16-K.1: automation tests for VertexMaskForgeLayerOrder -- the pure, Slate-free value-domain for the
// persistent generator layer order. Nothing here constructs SVertexMaskForgePanel, touches the
// composition path (ComposeGeneratorLayersSequential/EvaluateFillLayers), or depends on the numeric/
// declaration order of EVertexMaskForgeScalarMaskSource -- every expected value is the literal sequence
// this checkpoint's own contract specifies, never derived from the enum's own layout.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VertexMaskForgeLayerOrder.h"

namespace
{
	using EMaskSource = EVertexMaskForgeScalarMaskSource;

	bool ArraysEqual(const TArray<EMaskSource>& A, const TArray<EMaskSource>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (int32 i = 0; i < A.Num(); ++i)
		{
			if (A[i] != B[i])
			{
				return false;
			}
		}
		return true;
	}
}

// A. Default order: exactly 7 elements, the explicit approved sequence, valid, no Fill sources.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeLayerOrderDefaultTest, "VertexMaskForge.LayerOrder.DefaultOrder", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeLayerOrderDefaultTest::RunTest(const FString& Parameters)
{
	const TArray<EMaskSource> Default = VertexMaskForgeLayerOrder::MakeDefault();

	TestEqual(TEXT("Exactly seven elements"), Default.Num(), 7);

	const TArray<EMaskSource> Expected = {
		EMaskSource::BoundingBox, EMaskSource::AmbientOcclusion, EMaskSource::Curvature,
		EMaskSource::Noise, EMaskSource::MaterialSlot, EMaskSource::DirectionalNormal, EMaskSource::Thickness
	};
	TestTrue(TEXT("Explicit approved sequence, element by element"), ArraysEqual(Default, Expected));

	TestTrue(TEXT("IsValid(default) == true"), VertexMaskForgeLayerOrder::IsValid(Default));
	TestFalse(TEXT("Default does not contain ConstantWhite"), Default.Contains(EMaskSource::ConstantWhite));
	TestFalse(TEXT("Default does not contain ConstantBlack"), Default.Contains(EMaskSource::ConstantBlack));

	return true;
}

// B. Generator classification: the 7 real generators true; ConstantWhite/ConstantBlack/an invalid cast false.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeLayerOrderClassificationTest, "VertexMaskForge.LayerOrder.GeneratorClassification", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeLayerOrderClassificationTest::RunTest(const FString& Parameters)
{
	const EMaskSource Generators[] = {
		EMaskSource::BoundingBox, EMaskSource::AmbientOcclusion, EMaskSource::Curvature,
		EMaskSource::Noise, EMaskSource::MaterialSlot, EMaskSource::DirectionalNormal, EMaskSource::Thickness
	};
	for (const EMaskSource Source : Generators)
	{
		TestTrue(FString::Printf(TEXT("IsGeneratorLayer true for source=%d"), (int32)Source), VertexMaskForgeLayerOrder::IsGeneratorLayer(Source));
	}

	TestFalse(TEXT("ConstantWhite is not a generator layer"), VertexMaskForgeLayerOrder::IsGeneratorLayer(EMaskSource::ConstantWhite));
	TestFalse(TEXT("ConstantBlack is not a generator layer"), VertexMaskForgeLayerOrder::IsGeneratorLayer(EMaskSource::ConstantBlack));
	TestFalse(TEXT("An out-of-range cast is not a generator layer"), VertexMaskForgeLayerOrder::IsGeneratorLayer(static_cast<EMaskSource>(255)));

	return true;
}

// C. Validation accepts a non-default, valid permutation -- proves "valid" does not mean "equal to default".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeLayerOrderAcceptsPermutationTest, "VertexMaskForge.LayerOrder.ValidationAcceptsEveryPermutationShape", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeLayerOrderAcceptsPermutationTest::RunTest(const FString& Parameters)
{
	const TArray<EMaskSource> Reversed = {
		EMaskSource::Thickness, EMaskSource::DirectionalNormal, EMaskSource::MaterialSlot,
		EMaskSource::Noise, EMaskSource::Curvature, EMaskSource::AmbientOcclusion, EMaskSource::BoundingBox
	};
	TestFalse(TEXT("Sanity: this permutation is NOT the default order"), ArraysEqual(Reversed, VertexMaskForgeLayerOrder::MakeDefault()));
	TestTrue(TEXT("A non-default, complete permutation is still valid"), VertexMaskForgeLayerOrder::IsValid(Reversed));

	return true;
}

// D. Validation rejects wrong element counts: empty, six, eight.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeLayerOrderRejectsWrongCountTest, "VertexMaskForge.LayerOrder.ValidationRejectsWrongCount", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeLayerOrderRejectsWrongCountTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("Empty order is invalid"), VertexMaskForgeLayerOrder::IsValid(TArray<EMaskSource>()));

	const TArray<EMaskSource> SixElements = {
		EMaskSource::BoundingBox, EMaskSource::AmbientOcclusion, EMaskSource::Curvature,
		EMaskSource::Noise, EMaskSource::MaterialSlot, EMaskSource::DirectionalNormal
	};
	TestFalse(TEXT("Six elements is invalid"), VertexMaskForgeLayerOrder::IsValid(SixElements));

	TArray<EMaskSource> EightElements = VertexMaskForgeLayerOrder::MakeDefault();
	EightElements.Add(EMaskSource::BoundingBox); // Deliberately duplicated to reach 8 -- count alone must already reject this.
	TestFalse(TEXT("Eight elements is invalid"), VertexMaskForgeLayerOrder::IsValid(EightElements));

	return true;
}

// E. Validation rejects a duplicated generator (with another necessarily absent), input untouched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeLayerOrderRejectsDuplicateTest, "VertexMaskForge.LayerOrder.ValidationRejectsDuplicate", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeLayerOrderRejectsDuplicateTest::RunTest(const FString& Parameters)
{
	// BoundingBox duplicated, Thickness missing -- still exactly 7 elements.
	const TArray<EMaskSource> Duplicated = {
		EMaskSource::BoundingBox, EMaskSource::AmbientOcclusion, EMaskSource::Curvature,
		EMaskSource::Noise, EMaskSource::MaterialSlot, EMaskSource::DirectionalNormal, EMaskSource::BoundingBox
	};
	const TArray<EMaskSource> Before = Duplicated;

	TestFalse(TEXT("Duplicated generator is invalid"), VertexMaskForgeLayerOrder::IsValid(Duplicated));
	TestTrue(TEXT("Input array untouched by IsValid"), ArraysEqual(Duplicated, Before));

	return true;
}

// F. Validation rejects a Fill source (ConstantWhite/ConstantBlack) standing in for a missing generator.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeLayerOrderRejectsMissingGeneratorTest, "VertexMaskForge.LayerOrder.ValidationRejectsMissingGenerator", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeLayerOrderRejectsMissingGeneratorTest::RunTest(const FString& Parameters)
{
	// Thickness replaced by ConstantWhite.
	const TArray<EMaskSource> WithConstantWhite = {
		EMaskSource::BoundingBox, EMaskSource::AmbientOcclusion, EMaskSource::Curvature,
		EMaskSource::Noise, EMaskSource::MaterialSlot, EMaskSource::DirectionalNormal, EMaskSource::ConstantWhite
	};
	TestFalse(TEXT("ConstantWhite standing in for a missing generator is invalid"), VertexMaskForgeLayerOrder::IsValid(WithConstantWhite));

	// Thickness replaced by ConstantBlack.
	const TArray<EMaskSource> WithConstantBlack = {
		EMaskSource::BoundingBox, EMaskSource::AmbientOcclusion, EMaskSource::Curvature,
		EMaskSource::Noise, EMaskSource::MaterialSlot, EMaskSource::DirectionalNormal, EMaskSource::ConstantBlack
	};
	TestFalse(TEXT("ConstantBlack standing in for a missing generator is invalid"), VertexMaskForgeLayerOrder::IsValid(WithConstantBlack));

	return true;
}

// G. Validation rejects an unknown/cast-invalid value.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeLayerOrderRejectsUnknownValueTest, "VertexMaskForge.LayerOrder.ValidationRejectsUnknownValue", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeLayerOrderRejectsUnknownValueTest::RunTest(const FString& Parameters)
{
	const TArray<EMaskSource> WithUnknown = {
		EMaskSource::BoundingBox, EMaskSource::AmbientOcclusion, EMaskSource::Curvature,
		EMaskSource::Noise, EMaskSource::MaterialSlot, EMaskSource::DirectionalNormal, static_cast<EMaskSource>(255)
	};
	TestFalse(TEXT("An out-of-range cast value makes the order invalid"), VertexMaskForgeLayerOrder::IsValid(WithUnknown));

	return true;
}

// H. MoveUp on a middle element: exactly it and its predecessor swap; result stays valid.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeLayerOrderMoveUpMiddleTest, "VertexMaskForge.LayerOrder.MoveUpMiddle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeLayerOrderMoveUpMiddleTest::RunTest(const FString& Parameters)
{
	TArray<EMaskSource> Order = VertexMaskForgeLayerOrder::MakeDefault();
	// Default: [BoundingBox, AmbientOcclusion, Curvature, Noise, MaterialSlot, DirectionalNormal, Thickness].
	// Move Curvature (index 2) up -- must swap with AmbientOcclusion (index 1).
	const bool bResult = VertexMaskForgeLayerOrder::MoveUp(Order, EMaskSource::Curvature);

	TestTrue(TEXT("MoveUp returns true"), bResult);
	const TArray<EMaskSource> Expected = {
		EMaskSource::BoundingBox, EMaskSource::Curvature, EMaskSource::AmbientOcclusion,
		EMaskSource::Noise, EMaskSource::MaterialSlot, EMaskSource::DirectionalNormal, EMaskSource::Thickness
	};
	TestTrue(TEXT("Exactly Curvature and AmbientOcclusion swapped, nothing else moved"), ArraysEqual(Order, Expected));
	TestTrue(TEXT("Result stays valid"), VertexMaskForgeLayerOrder::IsValid(Order));

	return true;
}

// I. MoveDown on a middle element: exactly it and its successor swap; result stays valid.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeLayerOrderMoveDownMiddleTest, "VertexMaskForge.LayerOrder.MoveDownMiddle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeLayerOrderMoveDownMiddleTest::RunTest(const FString& Parameters)
{
	TArray<EMaskSource> Order = VertexMaskForgeLayerOrder::MakeDefault();
	// Move Curvature (index 2) down -- must swap with Noise (index 3).
	const bool bResult = VertexMaskForgeLayerOrder::MoveDown(Order, EMaskSource::Curvature);

	TestTrue(TEXT("MoveDown returns true"), bResult);
	const TArray<EMaskSource> Expected = {
		EMaskSource::BoundingBox, EMaskSource::AmbientOcclusion, EMaskSource::Noise,
		EMaskSource::Curvature, EMaskSource::MaterialSlot, EMaskSource::DirectionalNormal, EMaskSource::Thickness
	};
	TestTrue(TEXT("Exactly Curvature and Noise swapped, nothing else moved"), ArraysEqual(Order, Expected));
	TestTrue(TEXT("Result stays valid"), VertexMaskForgeLayerOrder::IsValid(Order));

	return true;
}

// J. MoveUp on the first element is rejected, array left exactly as it was.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeLayerOrderMoveUpFirstRejectedTest, "VertexMaskForge.LayerOrder.MoveUpFirstRejected", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeLayerOrderMoveUpFirstRejectedTest::RunTest(const FString& Parameters)
{
	TArray<EMaskSource> Order = VertexMaskForgeLayerOrder::MakeDefault();
	const TArray<EMaskSource> Before = Order;

	const bool bResult = VertexMaskForgeLayerOrder::MoveUp(Order, EMaskSource::BoundingBox); // Already first.

	TestFalse(TEXT("MoveUp on the first element returns false"), bResult);
	TestTrue(TEXT("Array unchanged"), ArraysEqual(Order, Before));

	return true;
}

// K. MoveDown on the last element is rejected, array left exactly as it was.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeLayerOrderMoveDownLastRejectedTest, "VertexMaskForge.LayerOrder.MoveDownLastRejected", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeLayerOrderMoveDownLastRejectedTest::RunTest(const FString& Parameters)
{
	TArray<EMaskSource> Order = VertexMaskForgeLayerOrder::MakeDefault();
	const TArray<EMaskSource> Before = Order;

	const bool bResult = VertexMaskForgeLayerOrder::MoveDown(Order, EMaskSource::Thickness); // Already last.

	TestFalse(TEXT("MoveDown on the last element returns false"), bResult);
	TestTrue(TEXT("Array unchanged"), ArraysEqual(Order, Before));

	return true;
}

// L. MoveUp/MoveDown reject a non-generator Source (ConstantWhite, ConstantBlack, an invalid cast),
// array left exactly as it was, on an otherwise-valid order.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeLayerOrderInvalidSourceRejectedTest, "VertexMaskForge.LayerOrder.InvalidSourceRejected", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeLayerOrderInvalidSourceRejectedTest::RunTest(const FString& Parameters)
{
	const EMaskSource InvalidSources[] = { EMaskSource::ConstantWhite, EMaskSource::ConstantBlack, static_cast<EMaskSource>(255) };

	for (const EMaskSource InvalidSource : InvalidSources)
	{
		{
			TArray<EMaskSource> Order = VertexMaskForgeLayerOrder::MakeDefault();
			const TArray<EMaskSource> Before = Order;
			const bool bResult = VertexMaskForgeLayerOrder::MoveUp(Order, InvalidSource);
			TestFalse(FString::Printf(TEXT("MoveUp rejects non-generator source=%d"), (int32)InvalidSource), bResult);
			TestTrue(FString::Printf(TEXT("MoveUp leaves array unchanged for source=%d"), (int32)InvalidSource), ArraysEqual(Order, Before));
		}
		{
			TArray<EMaskSource> Order = VertexMaskForgeLayerOrder::MakeDefault();
			const TArray<EMaskSource> Before = Order;
			const bool bResult = VertexMaskForgeLayerOrder::MoveDown(Order, InvalidSource);
			TestFalse(FString::Printf(TEXT("MoveDown rejects non-generator source=%d"), (int32)InvalidSource), bResult);
			TestTrue(FString::Printf(TEXT("MoveDown leaves array unchanged for source=%d"), (int32)InvalidSource), ArraysEqual(Order, Before));
		}
	}

	return true;
}

// M. MoveUp/MoveDown reject an invalid input order (duplicate, incomplete, containing a Fill source),
// array left exactly as it was.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeLayerOrderInvalidOrderRejectedTest, "VertexMaskForge.LayerOrder.InvalidOrderRejectedWithoutMutation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeLayerOrderInvalidOrderRejectedTest::RunTest(const FString& Parameters)
{
	const TArray<EMaskSource> WithDuplicate = {
		EMaskSource::BoundingBox, EMaskSource::AmbientOcclusion, EMaskSource::Curvature,
		EMaskSource::Noise, EMaskSource::MaterialSlot, EMaskSource::DirectionalNormal, EMaskSource::BoundingBox
	};
	const TArray<EMaskSource> Incomplete = {
		EMaskSource::BoundingBox, EMaskSource::AmbientOcclusion, EMaskSource::Curvature
	};
	const TArray<EMaskSource> WithFillSource = {
		EMaskSource::BoundingBox, EMaskSource::AmbientOcclusion, EMaskSource::Curvature,
		EMaskSource::Noise, EMaskSource::MaterialSlot, EMaskSource::DirectionalNormal, EMaskSource::ConstantWhite
	};

	const TArray<EMaskSource>* InvalidOrders[] = { &WithDuplicate, &Incomplete, &WithFillSource };
	const TCHAR* Labels[] = { TEXT("WithDuplicate"), TEXT("Incomplete"), TEXT("WithFillSource") };

	for (int32 i = 0; i < 3; ++i)
	{
		{
			TArray<EMaskSource> Order = *InvalidOrders[i];
			const TArray<EMaskSource> Before = Order;
			const bool bResult = VertexMaskForgeLayerOrder::MoveUp(Order, EMaskSource::BoundingBox);
			TestFalse(FString::Printf(TEXT("MoveUp rejects invalid order (%s)"), Labels[i]), bResult);
			TestTrue(FString::Printf(TEXT("MoveUp leaves order unchanged (%s)"), Labels[i]), ArraysEqual(Order, Before));
		}
		{
			TArray<EMaskSource> Order = *InvalidOrders[i];
			const TArray<EMaskSource> Before = Order;
			const bool bResult = VertexMaskForgeLayerOrder::MoveDown(Order, EMaskSource::BoundingBox);
			TestFalse(FString::Printf(TEXT("MoveDown rejects invalid order (%s)"), Labels[i]), bResult);
			TestTrue(FString::Printf(TEXT("MoveDown leaves order unchanged (%s)"), Labels[i]), ArraysEqual(Order, Before));
		}
	}

	return true;
}

// N. Round trip: MoveDown then MoveUp of the same element returns the order to its exact starting shape.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeLayerOrderRoundTripTest, "VertexMaskForge.LayerOrder.RoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeLayerOrderRoundTripTest::RunTest(const FString& Parameters)
{
	TArray<EMaskSource> Order = VertexMaskForgeLayerOrder::MakeDefault();
	const TArray<EMaskSource> Original = Order;

	const bool bDownResult = VertexMaskForgeLayerOrder::MoveDown(Order, EMaskSource::Curvature);
	TestTrue(TEXT("MoveDown returns true"), bDownResult);
	TestFalse(TEXT("Order differs from original after MoveDown"), ArraysEqual(Order, Original));

	const bool bUpResult = VertexMaskForgeLayerOrder::MoveUp(Order, EMaskSource::Curvature);
	TestTrue(TEXT("MoveUp returns true"), bUpResult);
	TestTrue(TEXT("Order matches the original exactly after the round trip"), ArraysEqual(Order, Original));

	return true;
}

// O. Identity preserved across a sequence of valid moves: order stays valid, same 7 IDs present, none
// created/lost/duplicated.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeLayerOrderIdentityPreservedTest, "VertexMaskForge.LayerOrder.IdentityPreserved", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeLayerOrderIdentityPreservedTest::RunTest(const FString& Parameters)
{
	TArray<EMaskSource> Order = VertexMaskForgeLayerOrder::MakeDefault();

	TestTrue(TEXT("Move 1"), VertexMaskForgeLayerOrder::MoveDown(Order, EMaskSource::BoundingBox));
	TestTrue(TEXT("Move 2"), VertexMaskForgeLayerOrder::MoveUp(Order, EMaskSource::Thickness));
	TestTrue(TEXT("Move 3"), VertexMaskForgeLayerOrder::MoveDown(Order, EMaskSource::AmbientOcclusion));
	TestTrue(TEXT("Move 4"), VertexMaskForgeLayerOrder::MoveUp(Order, EMaskSource::MaterialSlot));

	TestTrue(TEXT("Order is still valid after the sequence"), VertexMaskForgeLayerOrder::IsValid(Order));

	const EMaskSource AllSeven[] = {
		EMaskSource::BoundingBox, EMaskSource::AmbientOcclusion, EMaskSource::Curvature,
		EMaskSource::Noise, EMaskSource::MaterialSlot, EMaskSource::DirectionalNormal, EMaskSource::Thickness
	};
	for (const EMaskSource Source : AllSeven)
	{
		TestTrue(FString::Printf(TEXT("Order still contains source=%d exactly once"), (int32)Source),
			Order.FilterByPredicate([Source](const EMaskSource Element) { return Element == Source; }).Num() == 1);
	}
	TestEqual(TEXT("Order still has exactly seven elements"), Order.Num(), 7);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
