// M16-K.3B: automation tests for VertexMaskForgeDynamicLayerEvaluator -- the pure composition core for
// FVertexMaskForgeDynamicLayerStack. Nothing here constructs SVertexMaskForgePanel, touches the active
// production composition path (ComposeGeneratorLayersSequential/ComputeComposedColorsRGB*), or reads
// GeneratorLayerOrder -- GeneratorLayerOrder remains the sole production order owner; this evaluator is
// not wired into any production call site yet.
//
// EvaluateColor's own header doc comment is the authoritative contract; each test below maps to one
// numbered requirement from the M16-K.3B checkpoint prompt (noted in each test's own comment).

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VertexMaskForgeDynamicLayerEvaluator.h"

namespace
{
	bool ColorsNearlyEqual(const FVector4f& A, const FVector4f& B, const float Tolerance = UE_KINDA_SMALL_NUMBER)
	{
		return FMath::IsNearlyEqual(A.X, B.X, Tolerance)
			&& FMath::IsNearlyEqual(A.Y, B.Y, Tolerance)
			&& FMath::IsNearlyEqual(A.Z, B.Z, Tolerance)
			&& FMath::IsNearlyEqual(A.W, B.W, Tolerance);
	}
}

// 1. EmptyStackPreservesBase: an empty stack returns BaseColor unmodified, RGB and Alpha alike.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorEmptyStackTest, "VertexMaskForge.DynamicLayerEvaluator.EmptyStackPreservesBase", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorEmptyStackTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeDynamicLayerStack Stack;
	const FVector4f Base(0.3f, 0.5f, 0.7f, 0.9f);

	const FVector4f Result = VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack);
	TestTrue(TEXT("Empty stack returns BaseColor unmodified"), ColorsNearlyEqual(Result, Base));

	return true;
}

// 2. InitialStackPreservesBase: MakeInitialStack's single Fill=None Base Layer contributes nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorInitialStackTest, "VertexMaskForge.DynamicLayerEvaluator.InitialStackPreservesBase", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorInitialStackTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeDynamicLayerStack Stack = FVertexMaskForgeDynamicLayerStack::MakeInitialStack();
	const FVector4f Base(0.1f, 0.2f, 0.3f, 0.4f);

	const FVector4f Result = VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack);
	TestTrue(TEXT("MakeInitialStack's Base Layer (Fill=None) preserves BaseColor"), ColorsNearlyEqual(Result, Base));

	return true;
}

// 3. NoneFillIsNoOp: an enabled layer with Fill=None (default) is a no-op regardless of BlendMode/Opacity.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorNoneFillNoOpTest, "VertexMaskForge.DynamicLayerEvaluator.NoneFillIsNoOp", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorNoneFillNoOpTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	TestTrue(TEXT("SetLayerBlendMode(Multiply)"), Stack.SetLayerBlendMode(Id, EVertexMaskForgeBlendMode::Multiply));
	TestTrue(TEXT("SetLayerOpacity(1.0)"), Stack.SetLayerOpacity(Id, 1.0f));
	// Fill is left at its default -- None.

	const FVector4f Base(0.5f, 0.5f, 0.5f, 1.0f);
	const FVector4f Result = VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack);
	TestTrue(TEXT("Fill=None is a no-op even with an active BlendMode/Opacity"), ColorsNearlyEqual(Result, Base));

	return true;
}

// 4. BlackFillResolvesToZero: proves Black is real content (0.0), distinct from None.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorBlackFillTest, "VertexMaskForge.DynamicLayerEvaluator.BlackFillResolvesToZero", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorBlackFillTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::Black);
	Stack.SetLayerBlendMode(Id, EVertexMaskForgeBlendMode::Copy);
	Stack.SetLayerOpacity(Id, 1.0f);

	const FVector4f Base(0.5f, 0.5f, 0.5f, 1.0f);
	const FVector4f Result = VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack);
	TestTrue(TEXT("Black Copy@1 writes 0.0, not a no-op"), ColorsNearlyEqual(Result, FVector4f(0.0f, 0.0f, 0.0f, 1.0f)));

	return true;
}

// 5. WhiteFillResolvesToOne: proves White resolves to content 1.0.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorWhiteFillTest, "VertexMaskForge.DynamicLayerEvaluator.WhiteFillResolvesToOne", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorWhiteFillTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerBlendMode(Id, EVertexMaskForgeBlendMode::Copy);
	Stack.SetLayerOpacity(Id, 1.0f);

	const FVector4f Base(0.2f, 0.2f, 0.2f, 1.0f);
	const FVector4f Result = VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack);
	TestTrue(TEXT("White Copy@1 writes 1.0"), ColorsNearlyEqual(Result, FVector4f(1.0f, 1.0f, 1.0f, 1.0f)));

	return true;
}

// 6. DisabledLayerIsNoOp: a disabled layer with a valid Fill still contributes nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorDisabledLayerTest, "VertexMaskForge.DynamicLayerEvaluator.DisabledLayerIsNoOp", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorDisabledLayerTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerBlendMode(Id, EVertexMaskForgeBlendMode::Copy);
	Stack.SetLayerOpacity(Id, 1.0f);
	Stack.SetLayerEnabled(Id, false);

	const FVector4f Base(0.4f, 0.4f, 0.4f, 1.0f);
	const FVector4f Result = VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack);
	TestTrue(TEXT("Disabled layer is a no-op"), ColorsNearlyEqual(Result, Base));

	return true;
}

// 7. ZeroOpacityIsNoOp: Opacity=0 is a no-op regardless of BlendMode (Copy and Multiply both proven).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorZeroOpacityTest, "VertexMaskForge.DynamicLayerEvaluator.ZeroOpacityIsNoOp", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorZeroOpacityTest::RunTest(const FString& Parameters)
{
	const FVector4f Base(0.4f, 0.4f, 0.4f, 1.0f);
	const EVertexMaskForgeBlendMode ModesToCheck[] = { EVertexMaskForgeBlendMode::Copy, EVertexMaskForgeBlendMode::Multiply };

	for (const EVertexMaskForgeBlendMode Mode : ModesToCheck)
	{
		FVertexMaskForgeDynamicLayerStack Stack;
		const FGuid Id = Stack.AddLayer(TEXT("Layer"));
		Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::White);
		Stack.SetLayerBlendMode(Id, Mode);
		Stack.SetLayerOpacity(Id, 0.0f);

		const FVector4f Result = VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack);
		TestTrue(FString::Printf(TEXT("Opacity=0 is a no-op for mode=%d"), (int32)Mode), ColorsNearlyEqual(Result, Base));
	}

	return true;
}

// 8. FullOpacityCopy: Black writes 0, White writes 1, at full Opacity, all channels enabled.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorFullOpacityCopyTest, "VertexMaskForge.DynamicLayerEvaluator.FullOpacityCopy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorFullOpacityCopyTest::RunTest(const FString& Parameters)
{
	const FVector4f Base(0.5f, 0.5f, 0.5f, 1.0f);
	{
		FVertexMaskForgeDynamicLayerStack Stack;
		const FGuid Id = Stack.AddLayer(TEXT("Black"));
		Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::Black);
		Stack.SetLayerBlendMode(Id, EVertexMaskForgeBlendMode::Copy);
		Stack.SetLayerOpacity(Id, 1.0f);
		TestTrue(TEXT("Black Copy@1 -> 0"), ColorsNearlyEqual(VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack), FVector4f(0.0f, 0.0f, 0.0f, 1.0f)));
	}
	{
		FVertexMaskForgeDynamicLayerStack Stack;
		const FGuid Id = Stack.AddLayer(TEXT("White"));
		Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::White);
		Stack.SetLayerBlendMode(Id, EVertexMaskForgeBlendMode::Copy);
		Stack.SetLayerOpacity(Id, 1.0f);
		TestTrue(TEXT("White Copy@1 -> 1"), ColorsNearlyEqual(VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack), FVector4f(1.0f, 1.0f, 1.0f, 1.0f)));
	}

	return true;
}

// 9. PartialOpacityCopy: BlendResult=Copy(White)=1.0, Result=Lerp(Base,1.0,0.5).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorPartialOpacityCopyTest, "VertexMaskForge.DynamicLayerEvaluator.PartialOpacityCopy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorPartialOpacityCopyTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerBlendMode(Id, EVertexMaskForgeBlendMode::Copy);
	Stack.SetLayerOpacity(Id, 0.5f);

	const FVector4f Base(0.2f, 0.2f, 0.2f, 1.0f);
	const FVector4f Result = VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack);
	// Lerp(0.2, 1.0, 0.5) = 0.6.
	TestTrue(TEXT("Partial Opacity Copy interpolates correctly"), ColorsNearlyEqual(Result, FVector4f(0.6f, 0.6f, 0.6f, 1.0f)));

	return true;
}

// 10. Add: BlendResult=Base+Mask; verified with Black (identity, Base+0) and White at partial Opacity
// (a value that stays in [0,1] without needing the final clamp to prove the formula, not just the clamp).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorAddTest, "VertexMaskForge.DynamicLayerEvaluator.Add", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorAddTest::RunTest(const FString& Parameters)
{
	{
		FVertexMaskForgeDynamicLayerStack Stack;
		const FGuid Id = Stack.AddLayer(TEXT("Black"));
		Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::Black);
		Stack.SetLayerBlendMode(Id, EVertexMaskForgeBlendMode::Add);
		Stack.SetLayerOpacity(Id, 1.0f);
		const FVector4f Base(0.4f, 0.4f, 0.4f, 1.0f);
		// Base + 0 = Base -- Add with Black is real content (0), producing an identity result for a
		// different reason than a no-op (Black is composed, it just happens to add zero).
		TestTrue(TEXT("Add Black@1 == Base (Base+0)"), ColorsNearlyEqual(VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack), Base));
	}
	{
		FVertexMaskForgeDynamicLayerStack Stack;
		const FGuid Id = Stack.AddLayer(TEXT("White"));
		Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::White);
		Stack.SetLayerBlendMode(Id, EVertexMaskForgeBlendMode::Add);
		Stack.SetLayerOpacity(Id, 0.5f);
		const FVector4f Base(0.2f, 0.2f, 0.2f, 1.0f);
		// BlendResult = 0.2 + 1.0 = 1.2; Lerp(0.2, 1.2, 0.5) = 0.7 -- stays in [0,1], so this value proves
		// the formula itself, not just the final clamp.
		TestTrue(TEXT("Add White@0.5 == 0.7"), ColorsNearlyEqual(VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack), FVector4f(0.7f, 0.7f, 0.7f, 1.0f)));
	}

	return true;
}

// 11. Subtract: BlendResult=Base-Mask; White at partial Opacity produces an in-range value.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorSubtractTest, "VertexMaskForge.DynamicLayerEvaluator.Subtract", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorSubtractTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerBlendMode(Id, EVertexMaskForgeBlendMode::Subtract);
	Stack.SetLayerOpacity(Id, 0.5f);

	const FVector4f Base(0.7f, 0.7f, 0.7f, 1.0f);
	// BlendResult = 0.7 - 1.0 = -0.3; Lerp(0.7, -0.3, 0.5) = 0.2.
	const FVector4f Result = VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack);
	TestTrue(TEXT("Subtract White@0.5 == 0.2"), ColorsNearlyEqual(Result, FVector4f(0.2f, 0.2f, 0.2f, 1.0f)));

	return true;
}

// 12. Multiply: Black zeroes (real content), White is the identity (Base*1 == Base) at ANY Opacity --
// both are meaningful, distinct proofs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorMultiplyTest, "VertexMaskForge.DynamicLayerEvaluator.Multiply", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorMultiplyTest::RunTest(const FString& Parameters)
{
	{
		FVertexMaskForgeDynamicLayerStack Stack;
		const FGuid Id = Stack.AddLayer(TEXT("Black"));
		Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::Black);
		Stack.SetLayerBlendMode(Id, EVertexMaskForgeBlendMode::Multiply);
		Stack.SetLayerOpacity(Id, 0.5f);
		const FVector4f Base(0.6f, 0.6f, 0.6f, 1.0f);
		// BlendResult = 0.6 * 0 = 0; Lerp(0.6, 0, 0.5) = 0.3.
		TestTrue(TEXT("Multiply Black@0.5 == 0.3"), ColorsNearlyEqual(VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack), FVector4f(0.3f, 0.3f, 0.3f, 1.0f)));
	}
	{
		FVertexMaskForgeDynamicLayerStack Stack;
		const FGuid Id = Stack.AddLayer(TEXT("White"));
		Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::White);
		Stack.SetLayerBlendMode(Id, EVertexMaskForgeBlendMode::Multiply);
		Stack.SetLayerOpacity(Id, 0.7f); // Opacity is irrelevant -- BlendResult already equals Base.
		const FVector4f Base(0.42f, 0.42f, 0.42f, 1.0f);
		// BlendResult = Base * 1 = Base; Lerp(Base, Base, AnyOpacity) = Base -- White/Multiply is always
		// the identity, regardless of Opacity.
		TestTrue(TEXT("Multiply White is the identity at any Opacity"), ColorsNearlyEqual(VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack), Base));
	}

	return true;
}

// 13. Overlay: both sides of the Base<0.5 branch, at full Opacity.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorOverlayTest, "VertexMaskForge.DynamicLayerEvaluator.Overlay", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorOverlayTest::RunTest(const FString& Parameters)
{
	{
		// Base < 0.5 branch: R = 2*B*M. Base=0.3, White(M=1) -> 2*0.3*1 = 0.6.
		FVertexMaskForgeDynamicLayerStack Stack;
		const FGuid Id = Stack.AddLayer(TEXT("Layer"));
		Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::White);
		Stack.SetLayerBlendMode(Id, EVertexMaskForgeBlendMode::Overlay);
		Stack.SetLayerOpacity(Id, 1.0f);
		const FVector4f Base(0.3f, 0.3f, 0.3f, 1.0f);
		TestTrue(TEXT("Overlay, Base<0.5 branch"), ColorsNearlyEqual(VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack), FVector4f(0.6f, 0.6f, 0.6f, 1.0f)));
	}
	{
		// Base >= 0.5 branch: R = 1 - 2*(1-B)*(1-M). Base=0.7, Black(M=0) -> 1 - 2*0.3*1 = 0.4.
		FVertexMaskForgeDynamicLayerStack Stack;
		const FGuid Id = Stack.AddLayer(TEXT("Layer"));
		Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::Black);
		Stack.SetLayerBlendMode(Id, EVertexMaskForgeBlendMode::Overlay);
		Stack.SetLayerOpacity(Id, 1.0f);
		const FVector4f Base(0.7f, 0.7f, 0.7f, 1.0f);
		TestTrue(TEXT("Overlay, Base>=0.5 branch"), ColorsNearlyEqual(VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack), FVector4f(0.4f, 0.4f, 0.4f, 1.0f)));
	}

	return true;
}

// 14. Screen: R = 1-(1-B)(1-M). Base=0.4, White, Opacity=0.5.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorScreenTest, "VertexMaskForge.DynamicLayerEvaluator.Screen", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorScreenTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerBlendMode(Id, EVertexMaskForgeBlendMode::Screen);
	Stack.SetLayerOpacity(Id, 0.5f);

	const FVector4f Base(0.4f, 0.4f, 0.4f, 1.0f);
	// BlendResult = 1-(1-0.4)(1-1) = 1; Lerp(0.4, 1, 0.5) = 0.7.
	const FVector4f Result = VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack);
	TestTrue(TEXT("Screen White@0.5 == 0.7"), ColorsNearlyEqual(Result, FVector4f(0.7f, 0.7f, 0.7f, 1.0f)));

	return true;
}

// 15. Linear: R = lerp(B,M,M). Black (M=0) PRESERVES Base -- the exact proof that Linear is NOT an alias
// of Copy (Copy+Black would write 0.0; Linear+Black leaves Base untouched).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorLinearTest, "VertexMaskForge.DynamicLayerEvaluator.Linear", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorLinearTest::RunTest(const FString& Parameters)
{
	{
		FVertexMaskForgeDynamicLayerStack Stack;
		const FGuid Id = Stack.AddLayer(TEXT("Black"));
		Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::Black);
		Stack.SetLayerBlendMode(Id, EVertexMaskForgeBlendMode::Linear);
		Stack.SetLayerOpacity(Id, 1.0f);
		const FVector4f Base(0.6f, 0.6f, 0.6f, 1.0f);
		// lerp(0.6, 0, 0) = 0.6 -- unlike Copy, Linear+Black does NOT write zero.
		TestTrue(TEXT("Linear Black@1 preserves Base -- NOT an alias of Copy"), ColorsNearlyEqual(VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack), Base));
	}
	{
		FVertexMaskForgeDynamicLayerStack Stack;
		const FGuid Id = Stack.AddLayer(TEXT("White"));
		Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::White);
		Stack.SetLayerBlendMode(Id, EVertexMaskForgeBlendMode::Linear);
		Stack.SetLayerOpacity(Id, 0.5f);
		const FVector4f Base(0.2f, 0.2f, 0.2f, 1.0f);
		// lerp(0.2, 1, 1) = 1 (BlendResult); Lerp(0.2, 1, 0.5) = 0.6.
		TestTrue(TEXT("Linear White@0.5 == 0.6"), ColorsNearlyEqual(VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack), FVector4f(0.6f, 0.6f, 0.6f, 1.0f)));
	}

	return true;
}

// 16/17/18. ChannelFilter{Red,Green,Blue}Only: only the enabled channel changes, the other two and
// Alpha are preserved exactly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorChannelFilterSingleTest, "VertexMaskForge.DynamicLayerEvaluator.ChannelFilterSingleChannel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorChannelFilterSingleTest::RunTest(const FString& Parameters)
{
	const FVector4f Base(0.2f, 0.3f, 0.4f, 0.9f);

	{
		FVertexMaskForgeDynamicLayerStack Stack;
		const FGuid Id = Stack.AddLayer(TEXT("RedOnly"));
		Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::White);
		Stack.SetLayerBlendMode(Id, EVertexMaskForgeBlendMode::Copy);
		Stack.SetLayerOpacity(Id, 1.0f);
		Stack.SetLayerChannelFilter(Id, true, false, false);
		TestTrue(TEXT("Red only changes; Green/Blue/Alpha preserved"), ColorsNearlyEqual(VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack), FVector4f(1.0f, 0.3f, 0.4f, 0.9f)));
	}
	{
		FVertexMaskForgeDynamicLayerStack Stack;
		const FGuid Id = Stack.AddLayer(TEXT("GreenOnly"));
		Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::White);
		Stack.SetLayerBlendMode(Id, EVertexMaskForgeBlendMode::Copy);
		Stack.SetLayerOpacity(Id, 1.0f);
		Stack.SetLayerChannelFilter(Id, false, true, false);
		TestTrue(TEXT("Green only changes; Red/Blue/Alpha preserved"), ColorsNearlyEqual(VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack), FVector4f(0.2f, 1.0f, 0.4f, 0.9f)));
	}
	{
		FVertexMaskForgeDynamicLayerStack Stack;
		const FGuid Id = Stack.AddLayer(TEXT("BlueOnly"));
		Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::White);
		Stack.SetLayerBlendMode(Id, EVertexMaskForgeBlendMode::Copy);
		Stack.SetLayerOpacity(Id, 1.0f);
		Stack.SetLayerChannelFilter(Id, false, false, true);
		TestTrue(TEXT("Blue only changes; Red/Green/Alpha preserved"), ColorsNearlyEqual(VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack), FVector4f(0.2f, 0.3f, 1.0f, 0.9f)));
	}

	return true;
}

// 19. ChannelFilterAllDisabled: a fully-configured, otherwise-active layer with all 3 channels disabled
// is a complete no-op.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorChannelFilterAllDisabledTest, "VertexMaskForge.DynamicLayerEvaluator.ChannelFilterAllDisabled", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorChannelFilterAllDisabledTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Id = Stack.AddLayer(TEXT("Layer"));
	Stack.SetLayerFill(Id, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerBlendMode(Id, EVertexMaskForgeBlendMode::Copy);
	Stack.SetLayerOpacity(Id, 1.0f);
	Stack.SetLayerChannelFilter(Id, false, false, false);

	const FVector4f Base(0.2f, 0.3f, 0.4f, 0.9f);
	const FVector4f Result = VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack);
	TestTrue(TEXT("All channels disabled is a complete no-op"), ColorsNearlyEqual(Result, Base));

	return true;
}

// 20. AlphaAlwaysPreserved: several layers, several BlendModes -- Alpha exactly equals BaseColor.W.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorAlphaPreservedTest, "VertexMaskForge.DynamicLayerEvaluator.AlphaAlwaysPreserved", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorAlphaPreservedTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid A = Stack.AddLayer(TEXT("A"));
	Stack.SetLayerFill(A, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerBlendMode(A, EVertexMaskForgeBlendMode::Add);
	Stack.SetLayerOpacity(A, 0.6f);

	const FGuid B = Stack.AddLayer(TEXT("B"));
	Stack.SetLayerFill(B, EVertexMaskForgeLayerFill::Black);
	Stack.SetLayerBlendMode(B, EVertexMaskForgeBlendMode::Overlay);
	Stack.SetLayerOpacity(B, 1.0f);

	const FGuid C = Stack.AddLayer(TEXT("C"));
	Stack.SetLayerFill(C, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerBlendMode(C, EVertexMaskForgeBlendMode::Screen);
	Stack.SetLayerOpacity(C, 0.3f);

	const float ExpectedAlpha = 0.4321f;
	const FVector4f Base(0.5f, 0.5f, 0.5f, ExpectedAlpha);
	const FVector4f Result = VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack);
	TestEqual(TEXT("Alpha exactly equals BaseColor.W"), Result.W, ExpectedAlpha);

	return true;
}

// 21. SequentialOrderAffectsResult: White/Copy then Black/Multiply vs. the reverse order, via an actual
// GUID-based reorder (MoveLayerUp) -- proves the TArray is the ordering authority.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorOrderAffectsResultTest, "VertexMaskForge.DynamicLayerEvaluator.SequentialOrderAffectsResult", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorOrderAffectsResultTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid A = Stack.AddLayer(TEXT("A"));
	Stack.SetLayerFill(A, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerBlendMode(A, EVertexMaskForgeBlendMode::Copy);
	Stack.SetLayerOpacity(A, 1.0f);

	const FGuid B = Stack.AddLayer(TEXT("B"));
	Stack.SetLayerFill(B, EVertexMaskForgeLayerFill::Black);
	Stack.SetLayerBlendMode(B, EVertexMaskForgeBlendMode::Multiply);
	Stack.SetLayerOpacity(B, 1.0f);

	const FVector4f Base(0.5f, 0.5f, 0.5f, 1.0f);

	// Order [A, B]: Copy White -> 1.0, then Multiply Black -> 1.0*0 = 0.0.
	const FVector4f ResultAB = VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack);
	TestTrue(TEXT("[A,B] order == 0.0"), ColorsNearlyEqual(ResultAB, FVector4f(0.0f, 0.0f, 0.0f, 1.0f)));

	// Reorder by GUID: move B before A.
	TestTrue(TEXT("MoveLayerUp(B) succeeds"), Stack.MoveLayerUp(B));

	// Order [B, A]: Multiply Black -> 0.5*0 = 0.0, then Copy White -> 1.0.
	const FVector4f ResultBA = VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack);
	TestTrue(TEXT("[B,A] order == 1.0"), ColorsNearlyEqual(ResultBA, FVector4f(1.0f, 1.0f, 1.0f, 1.0f)));

	TestFalse(TEXT("The two orders produce different results"), ColorsNearlyEqual(ResultAB, ResultBA));

	return true;
}

// 22. ReorderPreservesLayerIdentityAndData: after MoveLayerUp, both layers' LayerId/Name/Fill/BlendMode/
// Opacity are exactly what they were -- only the order changed -- and evaluation reflects the new order.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorReorderPreservesDataTest, "VertexMaskForge.DynamicLayerEvaluator.ReorderPreservesLayerIdentityAndData", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorReorderPreservesDataTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid A = Stack.AddLayer(TEXT("A"));
	Stack.SetLayerFill(A, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerBlendMode(A, EVertexMaskForgeBlendMode::Copy);
	Stack.SetLayerOpacity(A, 1.0f);

	const FGuid B = Stack.AddLayer(TEXT("B"));
	Stack.SetLayerFill(B, EVertexMaskForgeLayerFill::Black);
	Stack.SetLayerBlendMode(B, EVertexMaskForgeBlendMode::Multiply);
	Stack.SetLayerOpacity(B, 1.0f);

	TestTrue(TEXT("MoveLayerUp(B) succeeds"), Stack.MoveLayerUp(B));

	const FVertexMaskForgeLayer* FoundA = Stack.FindLayerById(A);
	const FVertexMaskForgeLayer* FoundB = Stack.FindLayerById(B);
	TestNotNull(TEXT("A still found"), FoundA);
	TestNotNull(TEXT("B still found"), FoundB);
	if (FoundA && FoundB)
	{
		TestEqual(TEXT("A name preserved"), FoundA->Name, FString(TEXT("A")));
		TestTrue(TEXT("A Fill preserved"), FoundA->Fill == EVertexMaskForgeLayerFill::White);
		TestTrue(TEXT("A BlendMode preserved"), FoundA->BlendMode == EVertexMaskForgeBlendMode::Copy);
		TestEqual(TEXT("A Opacity preserved"), FoundA->Opacity, 1.0f);

		TestEqual(TEXT("B name preserved"), FoundB->Name, FString(TEXT("B")));
		TestTrue(TEXT("B Fill preserved"), FoundB->Fill == EVertexMaskForgeLayerFill::Black);
		TestTrue(TEXT("B BlendMode preserved"), FoundB->BlendMode == EVertexMaskForgeBlendMode::Multiply);
		TestEqual(TEXT("B Opacity preserved"), FoundB->Opacity, 1.0f);
	}

	TestEqual(TEXT("B now first"), Stack.GetLayers()[0].LayerId, B);
	TestEqual(TEXT("A now second"), Stack.GetLayers()[1].LayerId, A);

	// Evaluation follows the new [B, A] order: Multiply Black -> 0, then Copy White -> 1.0.
	const FVector4f Result = VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(FVector4f(0.5f, 0.5f, 0.5f, 1.0f), Stack);
	TestTrue(TEXT("Evaluation follows the new order"), ColorsNearlyEqual(Result, FVector4f(1.0f, 1.0f, 1.0f, 1.0f)));

	return true;
}

// 23. FinalClampOnly: Base=0.8, Layer1=White/Add -> unclamped intermediate 1.8, Layer2=White/Subtract ->
// unclamped final 0.8, clamp applied once at the end -> 0.8. Incorrect intermediate clamping would
// instead produce 0.0 (1.8 clamped to 1.0, then 1.0-1.0=0.0) -- this test proves the correct policy.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorFinalClampOnlyTest, "VertexMaskForge.DynamicLayerEvaluator.FinalClampOnly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorFinalClampOnlyTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid Layer1 = Stack.AddLayer(TEXT("Add"));
	Stack.SetLayerFill(Layer1, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerBlendMode(Layer1, EVertexMaskForgeBlendMode::Add);
	Stack.SetLayerOpacity(Layer1, 1.0f);

	const FGuid Layer2 = Stack.AddLayer(TEXT("Subtract"));
	Stack.SetLayerFill(Layer2, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerBlendMode(Layer2, EVertexMaskForgeBlendMode::Subtract);
	Stack.SetLayerOpacity(Layer2, 1.0f);

	const FVector4f Base(0.8f, 0.8f, 0.8f, 1.0f);
	const FVector4f Result = VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack);

	// Correct (unclamped-intermediate, final-clamp-only): 0.8 + 1.0 = 1.8 (intermediate, unclamped) ->
	// 1.8 - 1.0 = 0.8 (unclamped) -> clamp(0.8) = 0.8.
	// Incorrect (intermediate clamp) would instead produce: clamp(1.8)=1.0 -> 1.0-1.0=0.0.
	TestTrue(TEXT("Final-clamp-only policy produces 0.8, not the incorrect intermediate-clamp 0.0"), ColorsNearlyEqual(Result, Base));

	return true;
}

// 24. MultipleChannelsEvolveIndependently: Layer1 affects only Red (White/Copy), Layer2 affects only
// Green (Black/Copy) -- each channel's own sequential composite evolves independently.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorChannelsIndependentTest, "VertexMaskForge.DynamicLayerEvaluator.MultipleChannelsEvolveIndependently", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorChannelsIndependentTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid RedLayer = Stack.AddLayer(TEXT("RedLayer"));
	Stack.SetLayerFill(RedLayer, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerBlendMode(RedLayer, EVertexMaskForgeBlendMode::Copy);
	Stack.SetLayerOpacity(RedLayer, 1.0f);
	Stack.SetLayerChannelFilter(RedLayer, true, false, false);

	const FGuid GreenLayer = Stack.AddLayer(TEXT("GreenLayer"));
	Stack.SetLayerFill(GreenLayer, EVertexMaskForgeLayerFill::Black);
	Stack.SetLayerBlendMode(GreenLayer, EVertexMaskForgeBlendMode::Copy);
	Stack.SetLayerOpacity(GreenLayer, 1.0f);
	Stack.SetLayerChannelFilter(GreenLayer, false, true, false);

	const FVector4f Base(0.5f, 0.5f, 0.5f, 1.0f);
	const FVector4f Result = VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack);

	// R: only RedLayer touches it -> 1.0. G: only GreenLayer touches it -> 0.0. B: neither touches it -> 0.5.
	TestTrue(TEXT("Each channel evolves via its own layer's filter"), ColorsNearlyEqual(Result, FVector4f(1.0f, 0.0f, 0.5f, 1.0f)));

	return true;
}

// 25. DuplicateNamesDoNotAffectComposition: identity stays by GUID; identical layer names do not change
// lookup, order, or the composed result.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorDuplicateNamesTest, "VertexMaskForge.DynamicLayerEvaluator.DuplicateNamesDoNotAffectComposition", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorDuplicateNamesTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid A = Stack.AddLayer(TEXT("Layer"));
	Stack.SetLayerFill(A, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerBlendMode(A, EVertexMaskForgeBlendMode::Copy);
	Stack.SetLayerOpacity(A, 1.0f);

	const FGuid B = Stack.AddLayer(TEXT("Layer")); // Same name as A -- identity must still separate them.
	Stack.SetLayerFill(B, EVertexMaskForgeLayerFill::Black);
	Stack.SetLayerBlendMode(B, EVertexMaskForgeBlendMode::Multiply);
	Stack.SetLayerOpacity(B, 1.0f);

	TestNotEqual(TEXT("A and B have distinct identity despite identical names"), A, B);

	const FVector4f Base(0.5f, 0.5f, 0.5f, 1.0f);
	const FVector4f Result = VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack);
	// [A(White Copy), B(Black Multiply)] order -> Copy White (1.0), then Multiply Black (0.0).
	TestTrue(TEXT("Composition follows GUID-based order, unaffected by duplicate names"), ColorsNearlyEqual(Result, FVector4f(0.0f, 0.0f, 0.0f, 1.0f)));

	return true;
}

// 30. EvaluationDoesNotMutateStack: identity, order, and every field are exactly the same before and
// after a call to EvaluateColor.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicLayerEvaluatorDoesNotMutateStackTest, "VertexMaskForge.DynamicLayerEvaluator.EvaluationDoesNotMutateStack", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicLayerEvaluatorDoesNotMutateStackTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid A = Stack.AddLayer(TEXT("A"));
	Stack.SetLayerFill(A, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerBlendMode(A, EVertexMaskForgeBlendMode::Overlay);
	Stack.SetLayerOpacity(A, 0.4f);
	Stack.SetLayerChannelFilter(A, true, false, true);

	const FGuid B = Stack.AddLayer(TEXT("B"));
	Stack.SetLayerFill(B, EVertexMaskForgeLayerFill::Black);
	Stack.SetLayerBlendMode(B, EVertexMaskForgeBlendMode::Screen);
	Stack.SetLayerOpacity(B, 0.9f);

	const TArray<FVertexMaskForgeLayer> Before = Stack.GetLayers();

	const FVector4f Base(0.5f, 0.5f, 0.5f, 1.0f);
	VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack);

	const TArray<FVertexMaskForgeLayer>& After = Stack.GetLayers();
	TestEqual(TEXT("Layer count unchanged"), After.Num(), Before.Num());
	for (int32 i = 0; i < Before.Num(); ++i)
	{
		TestEqual(FString::Printf(TEXT("LayerId[%d] unchanged"), i), After[i].LayerId, Before[i].LayerId);
		TestEqual(FString::Printf(TEXT("Name[%d] unchanged"), i), After[i].Name, Before[i].Name);
		TestTrue(FString::Printf(TEXT("Fill[%d] unchanged"), i), After[i].Fill == Before[i].Fill);
		TestTrue(FString::Printf(TEXT("BlendMode[%d] unchanged"), i), After[i].BlendMode == Before[i].BlendMode);
		TestEqual(FString::Printf(TEXT("Opacity[%d] unchanged"), i), After[i].Opacity, Before[i].Opacity);
		TestTrue(FString::Printf(TEXT("bEnabled[%d] unchanged"), i), After[i].bEnabled == Before[i].bEnabled);
		TestTrue(FString::Printf(TEXT("Channels[%d] unchanged"), i),
			After[i].bAffectRed == Before[i].bAffectRed && After[i].bAffectGreen == Before[i].bAffectGreen && After[i].bAffectBlue == Before[i].bAffectBlue);
	}
	TestTrue(TEXT("Stack still valid"), Stack.IsValid());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
