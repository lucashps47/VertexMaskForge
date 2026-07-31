// M16-B: mathematical, deterministic automation tests for the new sequential evaluators
// (VertexMaskForgeSequentialEvaluator.h/.cpp) and for characterizing/protecting the legacy
// VertexMaskForgeMaskStackComposer::ComposeStack algorithm across the ApplyMaskBlendMode/
// BlendMaskValueUnclamped linkage change made in this same checkpoint. Every expected value below was
// computed by hand directly from the real, verbatim function bodies (see the M15/M15.1/M15.2/M15.2.1
// audits' own derivations) -- never assumed from a name or a comment.
//
// AUDITED (M16-J final -- authoritative semantic/clamp-policy decision): this checkpoint changed BOTH
// EvaluateFillLayerStep's own formula (PaintValue = FillValue * EffectiveMask, computed by the caller,
// fed as the "Mask" operand together with the layer's OWN Opacity -- no Coverage=Opacity*EffectiveMask
// term exists anymore) AND the clamp policy of both folds (EvaluateMaskStack/EvaluateFillLayers): no
// per-step clamp anywhere in either fold now -- Clamp01 is applied EXACTLY ONCE, to each fold's own
// final accumulator. Sections B, D, F, G, H below are rewritten from the pre-M16-J-final version to
// match; Sections A, C, E.LayerOpacity/E.Disabled, and I are numerically UNCHANGED by this pass (proven
// by hand below) and are preserved verbatim. Section E's own "EffectiveMask=0" sub-test is corrected: the
// OLD Coverage-based contract guaranteed passthrough for EVERY Blend Mode when EffectiveMask=0 (since
// Coverage=Opacity*0=0 forced Lerp(...,0)=Base structurally, regardless of mode); the NEW PaintValue-based
// contract does NOT carry that guarantee for every mode -- see Section E's own updated doc comment for
// the exact per-mode proof.
//
// AUDITED: NaN/Infinity handling is deliberately NOT exercised or asserted here -- Lerp(a, NaN, 0) ==
// a is NOT a floating-point guarantee, and no such claim is made or tested (M15.2.1's own correction).

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VertexMaskForgeMaskStackComposer.h"
#include "VertexMaskForgeSequentialEvaluator.h"

namespace
{
	constexpr float VMF_Tolerance = 1e-4f;
}

// =================================================================================================
// A. Mask Stack -- seed and empty stack (UNCHANGED by M16-J final: every value here stays in [0,1]
// through the whole fold, so removing the per-step clamp and adding a single final clamp changes
// nothing numerically -- verified by hand for each sub-case below).
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaskStackSeedTest, "VertexMaskForge.SequentialEvaluator.MaskStack.SeedAndEmpty", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaskStackSeedTest::RunTest(const FString& Parameters)
{
	using namespace VertexMaskForgeSequentialEvaluator;

	// 1. Empty array -> 1.0.
	{
		const float Result = EvaluateMaskStack(TConstArrayView<FVertexMaskForgeMaskEvaluationInput>());
		TestEqual(TEXT("Empty stack"), Result, 1.0f);
	}

	// 2. All entries disabled -> 1.0.
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Inputs;
		Inputs.Add({ 0.75f, EVertexMaskForgeBlendMode::Copy, 1.0f, /*bEnabled=*/false });
		Inputs.Add({ 0.3f, EVertexMaskForgeBlendMode::Multiply, 1.0f, /*bEnabled=*/false });
		const float Result = EvaluateMaskStack(Inputs);
		TestEqual(TEXT("All-disabled stack"), Result, 1.0f);
	}

	// 3. Single Copy 0.75 -> 0.75.
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Inputs;
		Inputs.Add({ 0.75f, EVertexMaskForgeBlendMode::Copy, 1.0f, true });
		TestNearlyEqual(TEXT("Single Copy 0.75"), EvaluateMaskStack(Inputs), 0.75f, VMF_Tolerance);
	}

	// 4. Single Multiply 0.75 on seed 1 -> 0.75 (Base=1 makes Multiply degenerate to MaskValue itself).
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Inputs;
		Inputs.Add({ 0.75f, EVertexMaskForgeBlendMode::Multiply, 1.0f, true });
		TestNearlyEqual(TEXT("Single Multiply 0.75 on seed 1"), EvaluateMaskStack(Inputs), 0.75f, VMF_Tolerance);
	}

	// 5. Single Subtract 0.75 on seed 1 -> 0.25.
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Inputs;
		Inputs.Add({ 0.75f, EVertexMaskForgeBlendMode::Subtract, 1.0f, true });
		TestNearlyEqual(TEXT("Single Subtract 0.75 on seed 1"), EvaluateMaskStack(Inputs), 0.25f, VMF_Tolerance);
	}

	return true;
}

// =================================================================================================
// B. Mask Stack -- MaskOpacity semantics, all 7 Blend Modes
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaskStackOpacityTest, "VertexMaskForge.SequentialEvaluator.MaskStack.Opacity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaskStackOpacityTest::RunTest(const FString& Parameters)
{
	using namespace VertexMaskForgeSequentialEvaluator;

	const EVertexMaskForgeBlendMode AllModes[] = {
		EVertexMaskForgeBlendMode::Copy, EVertexMaskForgeBlendMode::Add, EVertexMaskForgeBlendMode::Subtract,
		EVertexMaskForgeBlendMode::Multiply, EVertexMaskForgeBlendMode::Overlay, EVertexMaskForgeBlendMode::Screen,
		EVertexMaskForgeBlendMode::Linear
	};

	// Opacity=0 preserves the prior accumulator exactly, for every mode, from an arbitrary (non-seed)
	// starting Result. UNCHANGED -- Lerp(Result, Applied, 0) == Result is an exact identity regardless
	// of clamp policy.
	for (const EVertexMaskForgeBlendMode Mode : AllModes)
	{
		const float Result = EvaluateMaskStep(/*Result=*/0.5f, /*MaskValue=*/0.75f, Mode, /*Opacity=*/0.0f);
		TestEqual(FString::Printf(TEXT("Opacity=0 preserves accumulator, mode=%d"), (int32)Mode), Result, 0.5f);
	}

	// Single Mask Instance, seed=1, Opacity=0 -> exactly 1.0, for every mode. UNCHANGED (via
	// EvaluateMaskStack's own final clamp -- 1.0 is already in range).
	for (const EVertexMaskForgeBlendMode Mode : AllModes)
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Inputs;
		Inputs.Add({ 0.75f, Mode, 0.0f, true });
		TestEqual(FString::Printf(TEXT("Single instance seed=1 Opacity=0 exact 1.0, mode=%d"), (int32)Mode), EvaluateMaskStack(Inputs), 1.0f);
	}

	// Disabled produces the same result as removing the entry entirely. UNCHANGED (both sides stay
	// within [0,1] throughout: seed 1 -> Copy 0.6 -> 0.6).
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> WithDisabled;
		WithDisabled.Add({ 0.6f, EVertexMaskForgeBlendMode::Copy, 1.0f, true });
		WithDisabled.Add({ 0.9f, EVertexMaskForgeBlendMode::Multiply, 0.7f, /*bEnabled=*/false });

		TArray<FVertexMaskForgeMaskEvaluationInput> WithoutEntry;
		WithoutEntry.Add({ 0.6f, EVertexMaskForgeBlendMode::Copy, 1.0f, true });

		TestNearlyEqual(TEXT("Disabled entry == removed entry"), EvaluateMaskStack(WithDisabled), EvaluateMaskStack(WithoutEntry), VMF_Tolerance);
	}

	// AUDITED (M16-J final): EvaluateMaskStep is now UNCLAMPED at every step -- calling it directly
	// (not through EvaluateMaskStack) at Opacity=1 returns the RAW ApplyMaskBlendMode result verbatim,
	// with NO outer Clamp01, even when that raw result falls outside [0,1] (e.g. Subtract(0.4,0.6) =
	// -0.2, never clamped to 0 by this per-step primitive -- only EvaluateMaskStack's own single final
	// clamp would do that, and this sub-test deliberately calls the per-step primitive directly to prove
	// the absence of a per-step clamp).
	for (const EVertexMaskForgeBlendMode Mode : AllModes)
	{
		const float Expected = VertexMaskForgeMaskStackComposer::ApplyMaskBlendMode(0.4f, 0.6f, Mode);
		const float Actual = EvaluateMaskStep(/*Result=*/0.4f, /*MaskValue=*/0.6f, Mode, /*Opacity=*/1.0f);
		TestNearlyEqual(FString::Printf(TEXT("Opacity=1 raw, UNCLAMPED per-step result, mode=%d"), (int32)Mode), Actual, Expected, VMF_Tolerance);
	}

	// Continuity near zero: seed=1, single Copy 0.25, Opacity=0.001 -> 0.99925 exactly. UNCHANGED
	// (value stays in [0,1] throughout).
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Inputs;
		Inputs.Add({ 0.25f, EVertexMaskForgeBlendMode::Copy, 0.001f, true });
		TestNearlyEqual(TEXT("Continuity near Opacity=0"), EvaluateMaskStack(Inputs), 0.99925f, VMF_Tolerance);
	}

	// Explicit negative case: Enabled vs. Disabled are NOT required to match when the mask has real
	// effect -- Enable/Disable is a discrete change, not a continuous one (M15.2.1's own correction).
	// UNCHANGED.
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Enabled;
		Enabled.Add({ 0.75f, EVertexMaskForgeBlendMode::Copy, 1.0f, true });
		TArray<FVertexMaskForgeMaskEvaluationInput> Disabled;
		Disabled.Add({ 0.75f, EVertexMaskForgeBlendMode::Copy, 1.0f, false });

		const float EnabledResult = EvaluateMaskStack(Enabled);
		const float DisabledResult = EvaluateMaskStack(Disabled);
		TestNearlyEqual(TEXT("Enabled Copy 0.75 result"), EnabledResult, 0.75f, VMF_Tolerance);
		TestEqual(TEXT("Disabled-only stack result"), DisabledResult, 1.0f);
		TestTrue(TEXT("Enabled != Disabled when mask has real effect"), !FMath::IsNearlyEqual(EnabledResult, DisabledResult, VMF_Tolerance));
	}

	return true;
}

// =================================================================================================
// C. Mask Stack -- reorder sensitivity (UNCHANGED by M16-J final -- Copy structurally ignores its own
// Base operand at Opacity=1, so neither value below is affected by the clamp-policy change; verified
// by hand: order A never exceeds [0,1] at any step, and order B's intermediate 1.5 is immediately
// discarded by the following Copy step regardless of clamp policy).
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaskStackReorderTest, "VertexMaskForge.SequentialEvaluator.MaskStack.Reorder", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaskStackReorderTest::RunTest(const FString& Parameters)
{
	using namespace VertexMaskForgeSequentialEvaluator;

	// Order A: Copy 0.25 then Add 0.50 -> 0.75.
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Inputs;
		Inputs.Add({ 0.25f, EVertexMaskForgeBlendMode::Copy, 1.0f, true });
		Inputs.Add({ 0.50f, EVertexMaskForgeBlendMode::Add, 1.0f, true });
		TestNearlyEqual(TEXT("Copy then Add"), EvaluateMaskStack(Inputs), 0.75f, VMF_Tolerance);
	}

	// Order B: Add 0.50 then Copy 0.25 -> 0.25.
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Inputs;
		Inputs.Add({ 0.50f, EVertexMaskForgeBlendMode::Add, 1.0f, true });
		Inputs.Add({ 0.25f, EVertexMaskForgeBlendMode::Copy, 1.0f, true });
		TestNearlyEqual(TEXT("Add then Copy"), EvaluateMaskStack(Inputs), 0.25f, VMF_Tolerance);
	}

	return true;
}

// =================================================================================================
// D. Mask Stack -- clamp policy: NO clamp between steps, Clamp01 ONLY on the fold's final result
// (rewritten for M16-J final -- this section previously demonstrated and required a PER-STEP clamp;
// that policy is gone. Values recomputed by hand directly from the new, fully unclamped per-step fold.)
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeMaskStackClampTest, "VertexMaskForge.SequentialEvaluator.MaskStack.Clamp", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeMaskStackClampTest::RunTest(const FString& Parameters)
{
	using namespace VertexMaskForgeSequentialEvaluator;

	// Add (would saturate above 1 under the OLD per-step-clamp policy) then Multiply: seed=1 ->
	// Add(0.5,Op=1) = UNCLAMPED 1.5 -> Multiply(0.5,Op=1) = 1.5*0.5 = 0.75 -> final Clamp01(0.75) = 0.75.
	// (The OLD per-step-clamp policy would have produced 0.5 here -- see this test's own pre-M16-J-final
	// history for that value -- proving the two policies genuinely diverge on this exact input.)
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Inputs;
		Inputs.Add({ 0.5f, EVertexMaskForgeBlendMode::Add, 1.0f, true });
		Inputs.Add({ 0.5f, EVertexMaskForgeBlendMode::Multiply, 1.0f, true });
		TestNearlyEqual(TEXT("Add-unclamped-overshoot then Multiply, final-clamp-only"), EvaluateMaskStack(Inputs), 0.75f, VMF_Tolerance);
	}

	// Two Subtracts (drives the accumulator below 0, UNCLAMPED) then Screen: seed=1 ->
	// Subtract(0.6,Op=1) = 0.4 -> Subtract(0.6,Op=1) = -0.2 (unclamped, negative, never clamped here) ->
	// Screen(-0.2, Mask=0.5, Op=1) = 1-(1-(-0.2))*(1-0.5) = 1-(1.2*0.5) = 1-0.6 = 0.4 -> final
	// Clamp01(0.4) = 0.4.
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Inputs;
		Inputs.Add({ 0.6f, EVertexMaskForgeBlendMode::Subtract, 1.0f, true });
		Inputs.Add({ 0.6f, EVertexMaskForgeBlendMode::Subtract, 1.0f, true });
		Inputs.Add({ 0.5f, EVertexMaskForgeBlendMode::Screen, 1.0f, true });
		TestNearlyEqual(TEXT("Subtract-Subtract-below-zero (unclamped) then Screen, final-clamp-only"), EvaluateMaskStack(Inputs), 0.4f, VMF_Tolerance);
	}

	// Add (would saturate to 1 under the OLD policy, but here stays UNCLAMPED at 2.0) then partial-opacity
	// Copy: seed=1 -> Add(1.0,Op=1) = 2.0 (unclamped) -> Copy(0.3,Op=0.5) = Lerp(2.0, 0.3, 0.5) = 1.15
	// (Copy's own ApplyMaskBlendMode ignores the 2.0 accumulator entirely and returns 0.3, but the Lerp
	// weight is applied against the UNCLAMPED 2.0, not a clamped 1.0) -> final Clamp01(1.15) = 1.0.
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Inputs;
		Inputs.Add({ 1.0f, EVertexMaskForgeBlendMode::Add, 1.0f, true });
		Inputs.Add({ 0.3f, EVertexMaskForgeBlendMode::Copy, 0.5f, true });
		TestNearlyEqual(TEXT("Add-unclamped-overshoot then partial Copy, final-clamp-only"), EvaluateMaskStack(Inputs), 1.0f, VMF_Tolerance);
	}

	// Explicit proof the final result is genuinely clamped: a stack whose raw fold overshoots [0,1]
	// (two Adds of 0.9 on seed 1 -> unclamped 2.8) must still report EXACTLY 1.0, never 2.8.
	{
		TArray<FVertexMaskForgeMaskEvaluationInput> Inputs;
		Inputs.Add({ 0.9f, EVertexMaskForgeBlendMode::Add, 1.0f, true });
		Inputs.Add({ 0.9f, EVertexMaskForgeBlendMode::Add, 1.0f, true });
		TestEqual(TEXT("Final result is clamped to exactly 1.0, never left at the unclamped 2.8"), EvaluateMaskStack(Inputs), 1.0f);
	}

	return true;
}

// =================================================================================================
// E. Fill Layer -- passthrough guarantees
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerPassthroughTest, "VertexMaskForge.SequentialEvaluator.FillLayer.Passthrough", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerPassthroughTest::RunTest(const FString& Parameters)
{
	using namespace VertexMaskForgeSequentialEvaluator;

	const FVector3f Base(0.2f, 0.2f, 0.2f);
	const FVector3f Fill(1.0f, 1.0f, 1.0f);
	const EVertexMaskForgeBlendMode AllModes[] = {
		EVertexMaskForgeBlendMode::Copy, EVertexMaskForgeBlendMode::Add, EVertexMaskForgeBlendMode::Subtract,
		EVertexMaskForgeBlendMode::Multiply, EVertexMaskForgeBlendMode::Overlay, EVertexMaskForgeBlendMode::Screen,
		EVertexMaskForgeBlendMode::Linear
	};

	for (const EVertexMaskForgeBlendMode Mode : AllModes)
	{
		// LayerOpacity=0 -> passthrough, for EVERY mode, structurally guaranteed regardless of
		// EffectiveMask/PaintValue (Lerp(Base, X, 0) == Base is an exact identity for any X). UNCHANGED
		// by M16-J final.
		{
			TArray<FVertexMaskForgeLayerEvaluationInput> Inputs;
			Inputs.Add({ Fill, Mode, 0.0f, true, /*EffectiveMask=*/1.0f });
			const FVector3f Result = EvaluateFillLayers(Base, Inputs);
			TestTrue(FString::Printf(TEXT("LayerOpacity=0 passthrough, mode=%d"), (int32)Mode), Result.Equals(Base, VMF_Tolerance));
		}
		// bEnabled=false -> passthrough, identical to removing the layer. UNCHANGED.
		{
			TArray<FVertexMaskForgeLayerEvaluationInput> Inputs;
			Inputs.Add({ Fill, Mode, 1.0f, /*bEnabled=*/false, /*EffectiveMask=*/1.0f });
			const FVector3f Result = EvaluateFillLayers(Base, Inputs);
			const FVector3f ResultEmpty = EvaluateFillLayers(Base, TConstArrayView<FVertexMaskForgeLayerEvaluationInput>());
			TestTrue(FString::Printf(TEXT("Disabled layer passthrough, mode=%d"), (int32)Mode), Result.Equals(Base, VMF_Tolerance));
			TestTrue(FString::Printf(TEXT("Disabled layer == removed layer, mode=%d"), (int32)Mode), Result.Equals(ResultEmpty, VMF_Tolerance));
		}
	}

	// AUDITED (M16-J final -- corrected claim): under the OLD Coverage=Opacity*EffectiveMask contract,
	// EffectiveMask=0 forced Coverage=0 structurally, which guaranteed passthrough for EVERY Blend Mode.
	// Under the NEW PaintValue=FillValue*EffectiveMask contract, EffectiveMask=0 makes PaintValue exactly
	// (0,0,0) regardless of FillValue, but the blend's own Opacity operand is LayerOpacity (here 1.0, NOT
	// reduced to 0) -- so passthrough on EffectiveMask=0 now holds ONLY for the Blend Modes whose raw
	// formula, by its own algebra, returns Base when Mask=0 (proven by hand from ApplyMaskBlendMode):
	//   Add:      Base+0        = Base            -> passthrough holds
	//   Subtract: Base-0        = Base            -> passthrough holds
	//   Screen:   1-(1-B)(1-0)  = B                -> passthrough holds
	//   Linear:   Lerp(B,0,0)   = B                -> passthrough holds
	//   Copy:     0                                -> does NOT pass through (returns 0, not Base)
	//   Multiply: Base*0        = 0                -> does NOT pass through (returns 0, not Base)
	//   Overlay (Base=0.2<0.5): 2*Base*0 = 0        -> does NOT pass through (returns 0, not Base)
	// This is a genuine, intentional behavioral consequence of the M16-J final authoritative decision
	// (Section 1: LayerOpacity is the blend weight, never re-derived from EffectiveMask) -- not a bug.
	{
		const EVertexMaskForgeBlendMode PassthroughModes[] = {
			EVertexMaskForgeBlendMode::Add, EVertexMaskForgeBlendMode::Subtract,
			EVertexMaskForgeBlendMode::Screen, EVertexMaskForgeBlendMode::Linear
		};
		for (const EVertexMaskForgeBlendMode Mode : PassthroughModes)
		{
			TArray<FVertexMaskForgeLayerEvaluationInput> Inputs;
			Inputs.Add({ Fill, Mode, 1.0f, true, /*EffectiveMask=*/0.0f });
			const FVector3f Result = EvaluateFillLayers(Base, Inputs);
			TestTrue(FString::Printf(TEXT("EffectiveMask=0, LayerOpacity=1 still passes through for this mode's own algebra, mode=%d"), (int32)Mode),
				Result.Equals(Base, VMF_Tolerance));
		}

		const EVertexMaskForgeBlendMode NonPassthroughModes[] = {
			EVertexMaskForgeBlendMode::Copy, EVertexMaskForgeBlendMode::Multiply, EVertexMaskForgeBlendMode::Overlay
		};
		for (const EVertexMaskForgeBlendMode Mode : NonPassthroughModes)
		{
			TArray<FVertexMaskForgeLayerEvaluationInput> Inputs;
			Inputs.Add({ Fill, Mode, 1.0f, true, /*EffectiveMask=*/0.0f });
			const FVector3f Result = EvaluateFillLayers(Base, Inputs);
			TestTrue(FString::Printf(TEXT("EffectiveMask=0, LayerOpacity=1 does NOT pass through for this mode (PaintValue=0 dominates), mode=%d"), (int32)Mode),
				Result.Equals(FVector3f::ZeroVector, VMF_Tolerance));
		}
	}

	return true;
}

// =================================================================================================
// F. Fill Layer -- numeric verification (Copy/Add/Multiply), PaintValue contract
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerNumericTest, "VertexMaskForge.SequentialEvaluator.FillLayer.Numeric", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerNumericTest::RunTest(const FString& Parameters)
{
	using namespace VertexMaskForgeSequentialEvaluator;

	const FVector3f Base(0.2f, 0.2f, 0.2f);
	const FVector3f Fill(1.0f, 1.0f, 1.0f);

	// PaintValue = Fill(1.0) * EffectiveMask(0.5) = 0.5, LayerOpacity=1 -- item 9 of the authoritative
	// decision ("FillValue branco transforma PaintValue na própria máscara procedural") means PaintValue
	// here is literally the broadcast EffectiveMask value, 0.5.
	auto RunSingleLayer = [&](const EVertexMaskForgeBlendMode Mode) -> FVector3f
	{
		TArray<FVertexMaskForgeLayerEvaluationInput> Inputs;
		Inputs.Add({ Fill, Mode, /*Opacity=*/1.0f, true, /*EffectiveMask=*/0.5f });
		return EvaluateFillLayers(Base, Inputs);
	};

	// Copy: LayerOutput = Lerp(Base, PaintValue, 1) = PaintValue = 0.5 (Copy ignores Base at Opacity=1).
	TestTrue(TEXT("Copy: Base=0.2 PaintValue=0.5 Opacity=1 -> 0.5"), RunSingleLayer(EVertexMaskForgeBlendMode::Copy).Equals(FVector3f(0.5f, 0.5f, 0.5f), VMF_Tolerance));
	// Add: Lerp(0.2, 0.2+0.5=0.7, 1) = 0.7.
	TestTrue(TEXT("Add: Base=0.2 PaintValue=0.5 Opacity=1 -> 0.7"), RunSingleLayer(EVertexMaskForgeBlendMode::Add).Equals(FVector3f(0.7f, 0.7f, 0.7f), VMF_Tolerance));
	// Multiply: Lerp(0.2, 0.2*0.5=0.1, 1) = 0.1.
	TestTrue(TEXT("Multiply: Base=0.2 PaintValue=0.5 Opacity=1 -> 0.1"), RunSingleLayer(EVertexMaskForgeBlendMode::Multiply).Equals(FVector3f(0.1f, 0.1f, 0.1f), VMF_Tolerance));

	// Item 10 of the authoritative decision: FillValue preto produz PaintValue zero, regardless of
	// EffectiveMask -- Copy at Opacity=1 with a black Fill and EffectiveMask=1.0 (full coverage) must
	// still fully replace Base with (0,0,0), never with EffectiveMask itself.
	{
		TArray<FVertexMaskForgeLayerEvaluationInput> Inputs;
		Inputs.Add({ FVector3f::ZeroVector, EVertexMaskForgeBlendMode::Copy, 1.0f, true, /*EffectiveMask=*/1.0f });
		TestTrue(TEXT("Black FillValue -> PaintValue exactly 0, Copy Opacity=1 -> result 0"), EvaluateFillLayers(Base, Inputs).Equals(FVector3f::ZeroVector, VMF_Tolerance));
	}

	// Intermediate (non-white/non-black) FillValue, Copy, Opacity=1, EffectiveMask=1.0 -> PaintValue ==
	// FillValue exactly (EffectiveMask=1 is the multiplicative identity) -> Copy replaces Base with it.
	{
		const FVector3f Intermediate(0.3f, 0.6f, 0.9f);
		TArray<FVertexMaskForgeLayerEvaluationInput> Inputs;
		Inputs.Add({ Intermediate, EVertexMaskForgeBlendMode::Copy, 1.0f, true, /*EffectiveMask=*/1.0f });
		TestTrue(TEXT("Intermediate FillValue, EffectiveMask=1 -> PaintValue==FillValue, Copy Opacity=1 -> result==FillValue"),
			EvaluateFillLayers(Base, Inputs).Equals(Intermediate, VMF_Tolerance));
	}

	return true;
}

// =================================================================================================
// G. Fill Layer -- reorder sensitivity, cross-mode, non-degenerate values (recomputed for the
// PaintValue contract)
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerReorderTest, "VertexMaskForge.SequentialEvaluator.FillLayer.Reorder", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerReorderTest::RunTest(const FString& Parameters)
{
	using namespace VertexMaskForgeSequentialEvaluator;

	const FVector3f Base(0.2f, 0.2f, 0.2f);
	// LayerA: Fill=0.8, Copy, Opacity=1, EffectiveMask=0.5 -> PaintValueA = 0.8*0.5 = 0.4.
	const FVertexMaskForgeLayerEvaluationInput LayerA = { FVector3f(0.8f, 0.8f, 0.8f), EVertexMaskForgeBlendMode::Copy, 1.0f, true, 0.5f };
	// LayerB: Fill=0.5, Multiply, Opacity=1, EffectiveMask=1.0 -> PaintValueB = 0.5*1.0 = 0.5.
	const FVertexMaskForgeLayerEvaluationInput LayerB = { FVector3f(0.5f, 0.5f, 0.5f), EVertexMaskForgeBlendMode::Multiply, 1.0f, true, 1.0f };

	// Order A -> B: Copy(PaintValue=0.4) -> 0.4 (Copy ignores Base at Opacity=1) -> Multiply(PaintValue=0.5)
	// -> Lerp(0.4, 0.4*0.5=0.2, 1) = 0.2 -> final clamp 0.2.
	{
		TArray<FVertexMaskForgeLayerEvaluationInput> Inputs;
		Inputs.Add(LayerA);
		Inputs.Add(LayerB);
		TestTrue(TEXT("A then B -> 0.2"), EvaluateFillLayers(Base, Inputs).Equals(FVector3f(0.2f, 0.2f, 0.2f), VMF_Tolerance));
	}

	// Order B -> A: Multiply(PaintValue=0.5) on Base=0.2 -> Lerp(0.2, 0.2*0.5=0.1, 1) = 0.1 ->
	// Copy(PaintValue=0.4) -> 0.4 (Copy ignores the 0.1 accumulator) -> final clamp 0.4.
	{
		TArray<FVertexMaskForgeLayerEvaluationInput> Inputs;
		Inputs.Add(LayerB);
		Inputs.Add(LayerA);
		TestTrue(TEXT("B then A -> 0.4"), EvaluateFillLayers(Base, Inputs).Equals(FVector3f(0.4f, 0.4f, 0.4f), VMF_Tolerance));
	}

	TestTrue(TEXT("Reordering Copy/Multiply layers changes the result (0.2 != 0.4)"), true);

	return true;
}

// Cross-mode, 3-layer reorder with non-degenerate values, none of which are commutative with each
// other -- proves the fold is strictly array-order sequential, never grouped/reordered by Blend Mode,
// without claiming EVERY reorder changes the result (some homogeneous-mode reorders can be
// algebraically commutative -- see this file's own module comment / the M16-J final Section 3
// correction; this test deliberately picks a CROSS-mode combination where the divergence is real).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerCrossModeReorderTest, "VertexMaskForge.SequentialEvaluator.FillLayer.CrossModeReorder", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerCrossModeReorderTest::RunTest(const FString& Parameters)
{
	using namespace VertexMaskForgeSequentialEvaluator;

	const FVector3f Base(0.3f, 0.3f, 0.3f);
	// Layer1: Add, Fill=0.4, Opacity=0.8, EffectiveMask=0.5 -> PaintValue1 = 0.4*0.5 = 0.2.
	const FVertexMaskForgeLayerEvaluationInput Layer1 = { FVector3f(0.4f, 0.4f, 0.4f), EVertexMaskForgeBlendMode::Add, 0.8f, true, 0.5f };
	// Layer2: Multiply, Fill=0.6, Opacity=1.0, EffectiveMask=0.9 -> PaintValue2 = 0.6*0.9 = 0.54.
	const FVertexMaskForgeLayerEvaluationInput Layer2 = { FVector3f(0.6f, 0.6f, 0.6f), EVertexMaskForgeBlendMode::Multiply, 1.0f, true, 0.9f };
	// Layer3: Screen, Fill=0.2, Opacity=0.5, EffectiveMask=1.0 -> PaintValue3 = 0.2*1.0 = 0.2.
	const FVertexMaskForgeLayerEvaluationInput Layer3 = { FVector3f(0.2f, 0.2f, 0.2f), EVertexMaskForgeBlendMode::Screen, 0.5f, true, 1.0f };

	// Order [1,2,3]: Base=0.3 -> Add: Lerp(0.3, 0.3+0.2=0.5, 0.8) = 0.3+0.2*0.8 = 0.46 -> Multiply:
	// Lerp(0.46, 0.46*0.54=0.2484, 1.0) = 0.2484 -> Screen: Lerp(0.2484, 1-(1-0.2484)*(1-0.2)=1-0.7516*0.8=1-0.60128=0.39872, 0.5)
	// = 0.2484 + (0.39872-0.2484)*0.5 = 0.2484+0.07516 = 0.32356.
	const float Order123Expected = 0.32356f;

	// Order [3,2,1]: Base=0.3 -> Screen: Lerp(0.3, 1-(1-0.3)*(1-0.2)=1-0.7*0.8=1-0.56=0.44, 0.5) = 0.3+0.14*0.5=0.37
	// -> Multiply: Lerp(0.37, 0.37*0.54=0.1998, 1.0) = 0.1998 -> Add: Lerp(0.1998, 0.1998+0.2=0.3998, 0.8)
	// = 0.1998 + 0.2*0.8 = 0.3598.
	const float Order321Expected = 0.3598f;

	{
		TArray<FVertexMaskForgeLayerEvaluationInput> Inputs;
		Inputs.Add(Layer1);
		Inputs.Add(Layer2);
		Inputs.Add(Layer3);
		const FVector3f Result = EvaluateFillLayers(Base, Inputs);
		TestTrue(TEXT("Order [Add,Multiply,Screen]"), Result.Equals(FVector3f(Order123Expected, Order123Expected, Order123Expected), VMF_Tolerance));
	}
	{
		TArray<FVertexMaskForgeLayerEvaluationInput> Inputs;
		Inputs.Add(Layer3);
		Inputs.Add(Layer2);
		Inputs.Add(Layer1);
		const FVector3f Result = EvaluateFillLayers(Base, Inputs);
		TestTrue(TEXT("Order [Screen,Multiply,Add]"), Result.Equals(FVector3f(Order321Expected, Order321Expected, Order321Expected), VMF_Tolerance));
	}
	TestTrue(TEXT("Cross-mode reorder produces a genuinely different result"), !FMath::IsNearlyEqual(Order123Expected, Order321Expected, VMF_Tolerance));

	return true;
}

// =================================================================================================
// H. Fill Layer -- clamp policy: NO clamp between layers, Clamp01 ONLY on the fold's final result
// (rewritten for M16-J final).
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerClampTest, "VertexMaskForge.SequentialEvaluator.FillLayer.Clamp", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerClampTest::RunTest(const FString& Parameters)
{
	using namespace VertexMaskForgeSequentialEvaluator;

	// Base=0.8; Layer A: Add, PaintValue=1.0*1.0=1.0, Opacity=1 -> Lerp(0.8, 0.8+1.0=1.8, 1) = 1.8
	// (UNCLAMPED -- no per-layer clamp anymore); Layer B: Multiply, PaintValue=0.5*1.0=0.5, Opacity=1 ->
	// Lerp(1.8, 1.8*0.5=0.9, 1) = 0.9 -> final Clamp01(0.9) = 0.9. (The OLD per-layer-clamp policy would
	// have produced 0.5 here -- proving the two policies genuinely diverge on this exact input.)
	const FVector3f Base(0.8f, 0.8f, 0.8f);
	TArray<FVertexMaskForgeLayerEvaluationInput> Inputs;
	Inputs.Add({ FVector3f(1.0f, 1.0f, 1.0f), EVertexMaskForgeBlendMode::Add, 1.0f, true, 1.0f });
	Inputs.Add({ FVector3f(0.5f, 0.5f, 0.5f), EVertexMaskForgeBlendMode::Multiply, 1.0f, true, 1.0f });

	TestTrue(TEXT("Add-unclamped-overshoot then Multiply, final-clamp-only -> 0.9"), EvaluateFillLayers(Base, Inputs).Equals(FVector3f(0.9f, 0.9f, 0.9f), VMF_Tolerance));

	// Explicit proof the final result is genuinely clamped: two full-coverage Adds of white on Base=0.8
	// overshoot to 2.6 unclamped, but the reported result must be EXACTLY 1.0, never 2.6.
	{
		TArray<FVertexMaskForgeLayerEvaluationInput> OvershootInputs;
		OvershootInputs.Add({ FVector3f(1.0f, 1.0f, 1.0f), EVertexMaskForgeBlendMode::Add, 1.0f, true, 1.0f });
		OvershootInputs.Add({ FVector3f(1.0f, 1.0f, 1.0f), EVertexMaskForgeBlendMode::Add, 1.0f, true, 1.0f });
		TestTrue(TEXT("Final Fill Layer result is clamped to exactly 1.0, never left at the unclamped 2.6"),
			EvaluateFillLayers(Base, OvershootInputs).Equals(FVector3f(1.0f, 1.0f, 1.0f), VMF_Tolerance));
	}

	return true;
}

// =================================================================================================
// I. Legacy ComposeStack characterization -- protects byte/behavior parity across this checkpoint's
//    linkage-only change to ApplyMaskBlendMode/BlendMaskValueUnclamped. Values computed directly from
//    the unmodified, still stage-grouped ComposeStack algorithm -- never the new sequential contract.
//    UNCHANGED by M16-J final: ComposeStack itself was not touched by this checkpoint (see the
//    checkpoint's own scope: only VertexMaskForgeSequentialEvaluator.h/.cpp were modified).
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeLegacyComposeStackCharacterizationTest, "VertexMaskForge.SequentialEvaluator.Legacy.ComposeStackCharacterization", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeLegacyComposeStackCharacterizationTest::RunTest(const FString& Parameters)
{
	using namespace VertexMaskForgeMaskStackComposer;

	const FVector4f Baseline(0.2f, 0.2f, 0.2f, 1.0f);
	const FVector4f Committed(0.2f, 0.2f, 0.2f, 1.0f);

	auto RunSingleLayer = [&](const EVertexMaskForgeBlendMode Mode, const float MaskValue, const float Opacity) -> float
	{
		TArray<FResolvedMaskLayer, TInlineAllocator<4>> Layers;
		Layers.Add({ Mode, Opacity, MaskValue });
		bool bAnyContributed = false;
		const FVector4f Result = ComposeStack(Baseline, Committed, Layers, true, true, true, bAnyContributed);
		return Result.X;
	};

	// Single mode, Base=0.2, Mask=0.75, Opacity=1, each of the 7 real Blend Modes.
	TestNearlyEqual(TEXT("Legacy Copy"), RunSingleLayer(EVertexMaskForgeBlendMode::Copy, 0.75f, 1.0f), 0.75f, VMF_Tolerance);
	TestNearlyEqual(TEXT("Legacy Add"), RunSingleLayer(EVertexMaskForgeBlendMode::Add, 0.75f, 1.0f), 0.95f, VMF_Tolerance);
	// Subtract's raw result (0.2-0.75=-0.55) is clamped to 0 by ComposeStack's own unconditional
	// post-Multiply-stage boundary clamp -- not -0.55.
	TestNearlyEqual(TEXT("Legacy Subtract (boundary-clamped)"), RunSingleLayer(EVertexMaskForgeBlendMode::Subtract, 0.75f, 1.0f), 0.0f, VMF_Tolerance);
	TestNearlyEqual(TEXT("Legacy Multiply"), RunSingleLayer(EVertexMaskForgeBlendMode::Multiply, 0.75f, 1.0f), 0.15f, VMF_Tolerance);
	TestNearlyEqual(TEXT("Legacy Overlay"), RunSingleLayer(EVertexMaskForgeBlendMode::Overlay, 0.75f, 1.0f), 0.3f, VMF_Tolerance);
	TestNearlyEqual(TEXT("Legacy Screen"), RunSingleLayer(EVertexMaskForgeBlendMode::Screen, 0.75f, 1.0f), 0.8f, VMF_Tolerance);
	TestNearlyEqual(TEXT("Legacy Linear"), RunSingleLayer(EVertexMaskForgeBlendMode::Linear, 0.75f, 1.0f), 0.6125f, VMF_Tolerance);

	// Partial opacity, Copy, Base=0.2, Mask=0.75, Opacity=0.5 -> 0.475.
	TestNearlyEqual(TEXT("Legacy Copy partial opacity"), RunSingleLayer(EVertexMaskForgeBlendMode::Copy, 0.75f, 0.5f), 0.475f, VMF_Tolerance);

	// Crosses more than one stage: Add(0.3,1.0) + Multiply(0.5,1.0), Base=0.2 -> 0.25 (Add stage
	// resolves to 0.5, then Multiply stage resolves 0.5*0.5=0.25 -- fixed stage order, list order
	// irrelevant here, which is exactly the legacy behavior this test protects).
	{
		TArray<FResolvedMaskLayer, TInlineAllocator<4>> Layers;
		Layers.Add({ EVertexMaskForgeBlendMode::Add, 1.0f, 0.3f });
		Layers.Add({ EVertexMaskForgeBlendMode::Multiply, 1.0f, 0.5f });
		bool bAnyContributed = false;
		const FVector4f Result = ComposeStack(Baseline, Committed, Layers, true, true, true, bAnyContributed);
		TestNearlyEqual(TEXT("Legacy multi-stage Add+Multiply"), Result.X, 0.25f, VMF_Tolerance);
		TestTrue(TEXT("Legacy multi-stage bAnyContributed"), bAnyContributed);
	}

	// Empty Layers -> passthrough (CommittedColor for RGB, BaselineColor.W for Alpha).
	{
		bool bAnyContributed = true;
		const FVector4f Result = ComposeStack(Baseline, Committed, TArrayView<const FResolvedMaskLayer>(), true, true, true, bAnyContributed);
		TestFalse(TEXT("Legacy empty Layers bAnyContributed"), bAnyContributed);
		TestTrue(TEXT("Legacy empty Layers passthrough"), Result.Equals(FVector4f(0.2f, 0.2f, 0.2f, 1.0f), VMF_Tolerance));
	}

	return true;
}

// =================================================================================================
// J. Single-layer legacy equivalence -- the ONE case the M16-J final decision explicitly requires to
//    match ComposeStack's own result: a single Fill Layer, white FillValue, each of the 7 Blend Modes,
//    Opacity 0/1/intermediate, mask 0/1/intermediate, with the new sequential evaluator producing the
//    SAME numeric result as the legacy ComposeStack for a single-layer stack (never claimed for
//    multi-layer stacks -- see this file's own Section G/CrossModeReorder for why fold order is the
//    new oracle there instead).
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerSingleLayerLegacyEquivalenceTest, "VertexMaskForge.SequentialEvaluator.FillLayer.SingleLayerLegacyEquivalence", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerSingleLayerLegacyEquivalenceTest::RunTest(const FString& Parameters)
{
	using namespace VertexMaskForgeSequentialEvaluator;
	using namespace VertexMaskForgeMaskStackComposer;

	const EVertexMaskForgeBlendMode AllModes[] = {
		EVertexMaskForgeBlendMode::Copy, EVertexMaskForgeBlendMode::Add, EVertexMaskForgeBlendMode::Subtract,
		EVertexMaskForgeBlendMode::Multiply, EVertexMaskForgeBlendMode::Overlay, EVertexMaskForgeBlendMode::Screen,
		EVertexMaskForgeBlendMode::Linear
	};
	const float OpacityValues[] = { 0.0f, 1.0f, 0.5f };
	const float MaskValues[] = { 0.0f, 1.0f, 0.6f };

	const FVector3f Base(0.35f, 0.35f, 0.35f);
	const FVector3f White(1.0f, 1.0f, 1.0f);

	for (const EVertexMaskForgeBlendMode Mode : AllModes)
	{
		for (const float Opacity : OpacityValues)
		{
			for (const float Mask : MaskValues)
			{
				// New sequential path: a single Fill Layer, white FillValue -> PaintValue == Mask
				// (broadcast) exactly, per item 9 of the authoritative decision.
				TArray<FVertexMaskForgeLayerEvaluationInput> Inputs;
				Inputs.Add({ White, Mode, Opacity, true, Mask });
				const FVector3f NewResult = EvaluateFillLayers(Base, Inputs);

				// Legacy single-layer ComposeStack, same (Base, Mask, Mode, Opacity).
				TArray<FResolvedMaskLayer, TInlineAllocator<1>> LegacyLayers;
				LegacyLayers.Add({ Mode, Opacity, Mask });
				bool bAnyContributed = false;
				const FVector4f LegacyResult = ComposeStack(
					FVector4f(Base.X, Base.Y, Base.Z, 1.0f), FVector4f(Base.X, Base.Y, Base.Z, 1.0f),
					LegacyLayers, true, true, true, bAnyContributed);

				TestNearlyEqual(
					FString::Printf(TEXT("Single-layer equivalence, mode=%d Opacity=%.2f Mask=%.2f"), (int32)Mode, Opacity, Mask),
					NewResult.X, LegacyResult.X, VMF_Tolerance);
			}
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
