// M16-F/M16-G: automation tests for VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults
// -- the first real integration of authoring state (FVertexMaskForgeFillLayer/FVertexMaskForgeMaskInstance,
// M16-A), keyed result resolution (FVertexMaskForgeInstanceResultStore, M16-C), and pure sequential math
// (VertexMaskForgeSequentialEvaluator, M16-B), generalized in M16-G from a single uniform FVector3f
// BaseValue to a per-sample TConstArrayView<FVector3f> PerSampleBase. Every expected value below is
// derived directly from the real evaluator formulas already proven in
// VertexMaskForgeSequentialEvaluatorTests.cpp, not assumed.
//
// Structural proofs (no dedicated test, verified instead by source inspection / diff / call graph, per
// this checkpoint's own contract): VertexMaskForgeFillLayerResolution.cpp includes no Material Slot (or
// any other) generator header, calls no VertexMaskForgeInstanceResultStore mutator (StoreOrReplace/
// Remove/PruneToInstanceIds/Reset -- only Find), never assigns to any field of FVertexMaskForgeRecipe/
// FVertexMaskForgeFillLayer/FVertexMaskForgeMaskInstance passed to it, contains no loop over Fill Layers,
// and accepts no recipe/array-of-Fill-Layers parameter -- exactly one FVertexMaskForgeFillLayer per call.
//
// M16-G also audited and preserved the M16-F/M16-B decision to NOT validate non-finite (NaN/Inf) values
// in any input -- no such check existed in M16-F, and this checkpoint does not add one (no dedicated
// "non-finite base" test exists here for that reason; adding rejection now would be inventing a new
// contract, not preserving the existing one).

#if WITH_DEV_AUTOMATION_TESTS

#include "DynamicMesh/DynamicMesh3.h"
#include "Misc/AutomationTest.h"
#include "VertexMaskForgeFillLayerResolution.h"
#include "VertexMaskForgeInstanceResultStore.h"
#include "VertexMaskForgeMaterialSlotGenerator.h"
#include "VertexMaskForgeRecipeTypes.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

namespace
{
	constexpr float FLR_Tolerance = 1e-4f;

	FVertexMaskForgeInstanceMaskResult MakeFillLayerTestResult(const TArray<float>& Values)
	{
		FVertexMaskForgeInstanceMaskResult Result;
		Result.Values = Values;
		Result.bHasValue.Init(true, Values.Num());
		return Result;
	}

	FVertexMaskForgeMaskInstance MakeFillLayerTestInstance(const EVertexMaskForgeBlendMode Mode, const float Opacity, const bool bEnabled = true)
	{
		FVertexMaskForgeMaskInstance Instance = FVertexMaskForgeMaskInstance::Make(EVertexMaskForgeGeneratorType::MaterialSlot);
		Instance.BlendMode = Mode;
		Instance.Opacity = Opacity;
		Instance.bEnabled = bEnabled;
		return Instance;
	}

	/** White fill, Copy blend, full layer opacity -- Composite[i] = Lerp(Base[i], White, Coverage[i]). */
	FVertexMaskForgeFillLayer MakeWhiteFillCopyLayer()
	{
		FVertexMaskForgeFillLayer Layer = FVertexMaskForgeFillLayer::Make();
		Layer.FillValue = FVector3f(1.0f, 1.0f, 1.0f);
		Layer.BlendMode = EVertexMaskForgeBlendMode::Copy;
		Layer.Opacity = 1.0f;
		Layer.bEnabled = true;
		return Layer;
	}

	/** 0.5 fill, Multiply blend, full layer opacity -- with an empty Mask Stack (Coverage=1 always),
	 *  Composite[i] = Base[i] * 0.5 exactly, making the result a direct, unambiguous function of
	 *  Base[i] alone (no Coverage/mask term to obscure whether Base was actually read per-sample). */
	FVertexMaskForgeFillLayer MakeHalfMultiplyLayer()
	{
		FVertexMaskForgeFillLayer Layer = FVertexMaskForgeFillLayer::Make();
		Layer.FillValue = FVector3f(0.5f, 0.5f, 0.5f);
		Layer.BlendMode = EVertexMaskForgeBlendMode::Multiply;
		Layer.Opacity = 1.0f;
		Layer.bEnabled = true;
		return Layer;
	}

	void ExpectVector(FAutomationTestBase& Test, const TCHAR* What, const FVector3f& Actual, const FVector3f& Expected)
	{
		Test.TestTrue(FString::Printf(TEXT("%s == (%f,%f,%f)"), What, Expected.X, Expected.Y, Expected.Z),
			Actual.Equals(Expected, FLR_Tolerance));
	}
}

// Regression (updated from M16-F) + N. Uniform-base equivalence: a PerSampleBase with the same value
// repeated at every index must reproduce M16-F's own BasicResolution numbers exactly, proving no
// mathematical regression from replacing the uniform BaseValue parameter.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionBasicTest, "VertexMaskForge.FillLayerResolution.BasicResolution", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionBasicTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	Store.StoreOrReplace(A.InstanceId, MakeFillLayerTestResult({ 0.2f, 0.6f, 1.0f }));

	FVertexMaskForgeFillLayer Layer = MakeWhiteFillCopyLayer();
	Layer.MaskStack.Add(A);

	const TArray<FVector3f> Base = { FVector3f::ZeroVector, FVector3f::ZeroVector, FVector3f::ZeroVector };

	FVertexMaskForgeFillLayerEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer, Store, Base, Output);

	TestTrue(TEXT("Evaluation succeeds"), bSuccess);
	if (bSuccess)
	{
		TestEqual(TEXT("EffectiveMask.Num()"), Output.EffectiveMask.Num(), 3);
		TestNearlyEqual(TEXT("EffectiveMask[0]"), Output.EffectiveMask[0], 0.2f, FLR_Tolerance);
		TestNearlyEqual(TEXT("EffectiveMask[1]"), Output.EffectiveMask[1], 0.6f, FLR_Tolerance);
		TestNearlyEqual(TEXT("EffectiveMask[2]"), Output.EffectiveMask[2], 1.0f, FLR_Tolerance);
		ExpectVector(*this, TEXT("Composite[0]"), Output.Composite[0], FVector3f(0.2f, 0.2f, 0.2f));
		ExpectVector(*this, TEXT("Composite[2]"), Output.Composite[2], FVector3f(1.0f, 1.0f, 1.0f));
	}
	TestEqual(TEXT("Store unchanged"), Store.Num(), 1);

	return true;
}

// A. Base variable per sample: three distinct base colors, one keyed mask, Composite computed
// individually from each Base[i] -- proves the base was never reduced to a uniform value.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionVaryingBaseTest, "VertexMaskForge.FillLayerResolution.VaryingBasePerSample", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionVaryingBaseTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	Store.StoreOrReplace(A.InstanceId, MakeFillLayerTestResult({ 0.3f, 0.6f, 0.9f })); // Coverage == these values (Copy, Opacity=1).

	FVertexMaskForgeFillLayer Layer = MakeWhiteFillCopyLayer();
	Layer.MaskStack.Add(A);

	const TArray<FVector3f> Base = {
		FVector3f(0.2f, 0.4f, 0.6f),
		FVector3f(1.0f, 1.0f, 1.0f),
		FVector3f(0.0f, 0.0f, 0.0f),
	};

	FVertexMaskForgeFillLayerEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer, Store, Base, Output);

	TestTrue(TEXT("Evaluation succeeds"), bSuccess);
	if (bSuccess)
	{
		// Composite[i] = Lerp(Base[i], White, Coverage[i]) = Base[i] + Coverage[i]*(1-Base[i]).
		ExpectVector(*this, TEXT("Composite[0]"), Output.Composite[0], FVector3f(0.44f, 0.58f, 0.72f));
		ExpectVector(*this, TEXT("Composite[1]"), Output.Composite[1], FVector3f(1.0f, 1.0f, 1.0f));
		ExpectVector(*this, TEXT("Composite[2]"), Output.Composite[2], FVector3f(0.9f, 0.9f, 0.9f));
	}

	return true;
}

// B. Index correspondence: distinguishable per-index colors prove Base[i] is paired with the evaluator
// output at that SAME index i -- no spatial reorder, shift, or broadcast from index 0.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionIndexCorrespondenceTest, "VertexMaskForge.FillLayerResolution.IndexCorrespondence", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionIndexCorrespondenceTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	Store.StoreOrReplace(A.InstanceId, MakeFillLayerTestResult({ 0.5f, 0.5f, 0.5f })); // Uniform Coverage=0.5 isolates the base's own contribution.

	FVertexMaskForgeFillLayer Layer = MakeWhiteFillCopyLayer();
	Layer.MaskStack.Add(A);

	const TArray<FVector3f> Base = {
		FVector3f(1.0f, 0.0f, 0.0f), // red
		FVector3f(0.0f, 1.0f, 0.0f), // green
		FVector3f(0.0f, 0.0f, 1.0f), // blue
	};

	FVertexMaskForgeFillLayerEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer, Store, Base, Output);

	TestTrue(TEXT("Evaluation succeeds"), bSuccess);
	if (bSuccess)
	{
		// Composite[i] = 0.5*Base[i] + 0.5.
		ExpectVector(*this, TEXT("Composite[0] (from red base)"), Output.Composite[0], FVector3f(1.0f, 0.5f, 0.5f));
		ExpectVector(*this, TEXT("Composite[1] (from green base)"), Output.Composite[1], FVector3f(0.5f, 1.0f, 0.5f));
		ExpectVector(*this, TEXT("Composite[2] (from blue base)"), Output.Composite[2], FVector3f(0.5f, 0.5f, 1.0f));
	}

	return true;
}

// C. Empty Mask Stack with a varying base: EffectiveMask is uniformly 1.0 (M16-B's own empty-stack
// contract), but Composite still differs per sample because Composite is always computed from
// PerSampleBase[i] individually -- proves the base is never collapsed even when no masks are resolved.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionEmptyStackVaryingBaseTest, "VertexMaskForge.FillLayerResolution.EmptyStackVaryingBase", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionEmptyStackVaryingBaseTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store; // Empty -- must never be queried (no Mask Instances).
	FVertexMaskForgeFillLayer Layer = MakeHalfMultiplyLayer(); // MaskStack left empty.

	const TArray<FVector3f> Base = {
		FVector3f(0.2f, 0.4f, 0.6f),
		FVector3f(1.0f, 1.0f, 1.0f),
		FVector3f(0.0f, 0.0f, 0.0f),
	};

	FVertexMaskForgeFillLayerEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer, Store, Base, Output);

	TestTrue(TEXT("Evaluation succeeds with empty stack"), bSuccess);
	if (bSuccess)
	{
		for (int32 i = 0; i < 3; ++i)
		{
			TestNearlyEqual(FString::Printf(TEXT("EffectiveMask[%d] == 1.0"), i), Output.EffectiveMask[i], 1.0f, FLR_Tolerance);
		}
		// Multiply, Coverage=1 always -> Composite[i] = Base[i] * 0.5, individually.
		ExpectVector(*this, TEXT("Composite[0]"), Output.Composite[0], FVector3f(0.1f, 0.2f, 0.3f));
		ExpectVector(*this, TEXT("Composite[1]"), Output.Composite[1], FVector3f(0.5f, 0.5f, 0.5f));
		ExpectVector(*this, TEXT("Composite[2]"), Output.Composite[2], FVector3f(0.0f, 0.0f, 0.0f));
	}
	TestEqual(TEXT("Store stayed empty (never queried)"), Store.Num(), 0);

	return true;
}

// Regression (updated): two Mask Instances resolved by their own GUIDs, sequential fold applied.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionTwoInstancesTest, "VertexMaskForge.FillLayerResolution.TwoInstancesByGuid", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionTwoInstancesTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	const FVertexMaskForgeMaskInstance B = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	Store.StoreOrReplace(A.InstanceId, MakeFillLayerTestResult({ 0.25f }));
	Store.StoreOrReplace(B.InstanceId, MakeFillLayerTestResult({ 0.75f }));

	FVertexMaskForgeFillLayer Layer = MakeWhiteFillCopyLayer();
	Layer.MaskStack.Add(A);
	Layer.MaskStack.Add(B);

	const TArray<FVector3f> Base = { FVector3f::ZeroVector };

	FVertexMaskForgeFillLayerEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer, Store, Base, Output);

	TestTrue(TEXT("Evaluation succeeds"), bSuccess);
	if (bSuccess)
	{
		// Both Copy: the fold overwrites with each step's own value in order -- final == B's value.
		TestNearlyEqual(TEXT("Final EffectiveMask == B (last Copy wins)"), Output.EffectiveMask[0], 0.75f, FLR_Tolerance);
	}

	return true;
}

// Regression (updated): same generator type and identical parameters, different GUIDs -- still
// independently resolved.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionSameParamsTest, "VertexMaskForge.FillLayerResolution.SameParamsDifferentGuids", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionSameParamsTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Add, 1.0f);
	const FVertexMaskForgeMaskInstance B = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Add, 1.0f);
	TestNotEqual(TEXT("A and B have different InstanceIds despite identical config"), A.InstanceId, B.InstanceId);

	Store.StoreOrReplace(A.InstanceId, MakeFillLayerTestResult({ 0.1f }));
	Store.StoreOrReplace(B.InstanceId, MakeFillLayerTestResult({ 0.4f }));

	const FVertexMaskForgeInstanceMaskResult* FoundA = Store.Find(A.InstanceId);
	const FVertexMaskForgeInstanceMaskResult* FoundB = Store.Find(B.InstanceId);
	TestNotNull(TEXT("Find A"), FoundA);
	TestNotNull(TEXT("Find B"), FoundB);
	if (FoundA && FoundB)
	{
		TestNearlyEqual(TEXT("A's own value"), FoundA->Values[0], 0.1f, FLR_Tolerance);
		TestNearlyEqual(TEXT("B's own value"), FoundB->Values[0], 0.4f, FLR_Tolerance);
	}

	return true;
}

// E. / Regression (updated): authoring order is preserved and observable -- Multiply-then-Subtract !=
// Subtract-then-Multiply, proven with a two-sample varying base to also confirm index correspondence
// survives the reorder.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionOrderTest, "VertexMaskForge.FillLayerResolution.OrderPreserved", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionOrderTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Multiply, 1.0f);
	const FVertexMaskForgeMaskInstance B = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Subtract, 1.0f);
	Store.StoreOrReplace(A.InstanceId, MakeFillLayerTestResult({ 0.5f, 0.5f }));
	Store.StoreOrReplace(B.InstanceId, MakeFillLayerTestResult({ 0.3f, 0.3f }));

	// Order [A, B]: seed 1.0 -> Multiply(0.5): 0.5 -> Subtract(0.3): 0.2.
	FVertexMaskForgeFillLayer LayerAB = MakeWhiteFillCopyLayer();
	LayerAB.MaskStack.Add(A);
	LayerAB.MaskStack.Add(B);

	// Order [B, A]: seed 1.0 -> Subtract(0.3): 0.7 -> Multiply(0.5): 0.35.
	FVertexMaskForgeFillLayer LayerBA = MakeWhiteFillCopyLayer();
	LayerBA.MaskStack.Add(B);
	LayerBA.MaskStack.Add(A);

	const TArray<FVector3f> Base = { FVector3f::ZeroVector, FVector3f(1.0f, 1.0f, 1.0f) };

	FVertexMaskForgeFillLayerEvaluationOutput OutputAB, OutputBA;
	const bool bSuccessAB = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(LayerAB, Store, Base, OutputAB);
	const bool bSuccessBA = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(LayerBA, Store, Base, OutputBA);

	TestTrue(TEXT("AB succeeds"), bSuccessAB);
	TestTrue(TEXT("BA succeeds"), bSuccessBA);
	if (bSuccessAB && bSuccessBA)
	{
		TestNearlyEqual(TEXT("Order [A,B] index 0 == 0.2"), OutputAB.EffectiveMask[0], 0.2f, FLR_Tolerance);
		TestNearlyEqual(TEXT("Order [B,A] index 0 == 0.35"), OutputBA.EffectiveMask[0], 0.35f, FLR_Tolerance);
		TestTrue(TEXT("Orders produce different results at index 0"), !FMath::IsNearlyEqual(OutputAB.EffectiveMask[0], OutputBA.EffectiveMask[0], FLR_Tolerance));
		// Base[0]=(0,0,0), Coverage=EffectiveMask[0] -> Composite[0] = White*Coverage.
		ExpectVector(*this, TEXT("Composite[0] order AB"), OutputAB.Composite[0], FVector3f(0.2f, 0.2f, 0.2f));
		ExpectVector(*this, TEXT("Composite[0] order BA"), OutputBA.Composite[0], FVector3f(0.35f, 0.35f, 0.35f));
	}

	return true;
}

// Regression (updated): missing keyed result fails deterministically -- no fallback, no implicit generation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionMissingResultTest, "VertexMaskForge.FillLayerResolution.MissingResult", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionMissingResultTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store; // Deliberately empty -- A is never stored.
	const FVertexMaskForgeMaskInstance A = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);

	FVertexMaskForgeFillLayer Layer = MakeWhiteFillCopyLayer();
	Layer.MaskStack.Add(A);

	const TArray<FVector3f> Base = { FVector3f::ZeroVector };

	FVertexMaskForgeFillLayerEvaluationOutput Output;
	Output.EffectiveMask = { -1.0f }; // Sentinel.
	const bool bSuccess = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer, Store, Base, Output);

	TestFalse(TEXT("Evaluation fails for missing result"), bSuccess);
	TestEqual(TEXT("Store stays empty"), Store.Num(), 0);
	TestEqual(TEXT("Output sentinel untouched"), Output.EffectiveMask.Num(), 1);
	if (Output.EffectiveMask.Num() == 1)
	{
		TestNearlyEqual(TEXT("Sentinel value untouched"), Output.EffectiveMask[0], -1.0f, FLR_Tolerance);
	}

	return true;
}

// Regression (updated): invalid GUID fails deterministically -- no crash, no ensure.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionInvalidGuidTest, "VertexMaskForge.FillLayerResolution.InvalidGuid", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionInvalidGuidTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	FVertexMaskForgeMaskInstance Invalid = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	Invalid.InstanceId = FGuid(); // Deliberately invalid.
	TestFalse(TEXT("Default FGuid is invalid"), Invalid.InstanceId.IsValid());

	FVertexMaskForgeFillLayer Layer = MakeWhiteFillCopyLayer();
	Layer.MaskStack.Add(Invalid);

	const TArray<FVector3f> Base = { FVector3f::ZeroVector };

	FVertexMaskForgeFillLayerEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer, Store, Base, Output);

	TestFalse(TEXT("Evaluation fails for invalid GUID"), bSuccess);
	TestEqual(TEXT("Store unaffected"), Store.Num(), 0);

	return true;
}

// F./G. Regression (updated): a keyed result's cardinality diverging from the base's cardinality fails
// deterministically -- the base array IS the domain authority now (no separate ExpectedCardinality).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionCardinalityMismatchTest, "VertexMaskForge.FillLayerResolution.CardinalityMismatch", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionCardinalityMismatchTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	Store.StoreOrReplace(A.InstanceId, MakeFillLayerTestResult({ 0.4f, 0.5f, 0.6f, 0.7f })); // M=4

	FVertexMaskForgeFillLayer Layer = MakeWhiteFillCopyLayer();
	Layer.MaskStack.Add(A);

	const TArray<FVector3f> Base = { FVector3f::ZeroVector, FVector3f::ZeroVector, FVector3f::ZeroVector }; // N=3

	FVertexMaskForgeFillLayerEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer, Store, Base, Output);

	TestFalse(TEXT("Evaluation fails on cardinality mismatch (base N=3 vs result M=4)"), bSuccess);
	TestEqual(TEXT("Output stays empty"), Output.EffectiveMask.Num(), 0);
	TestEqual(TEXT("Store unaffected"), Store.Num(), 1);

	return true;
}

// Regression (updated): a sparse (partially-unwritten) keyed result fails deterministically.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionSparseResultTest, "VertexMaskForge.FillLayerResolution.SparseResultRejected", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionSparseResultTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);

	FVertexMaskForgeInstanceMaskResult Sparse;
	Sparse.Values = { 0.1f, 0.2f, 0.3f };
	Sparse.bHasValue.Init(true, 3);
	Sparse.bHasValue[1] = false; // Index 1 deliberately never written.
	Store.StoreOrReplace(A.InstanceId, MoveTemp(Sparse));

	FVertexMaskForgeFillLayer Layer = MakeWhiteFillCopyLayer();
	Layer.MaskStack.Add(A);

	const TArray<FVector3f> Base = { FVector3f::ZeroVector, FVector3f::ZeroVector, FVector3f::ZeroVector };

	FVertexMaskForgeFillLayerEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer, Store, Base, Output);

	TestFalse(TEXT("Evaluation fails on sparse result"), bSuccess);
	TestEqual(TEXT("Output stays empty"), Output.EffectiveMask.Num(), 0);

	return true;
}

// H. Empty base: PerSampleBase.Num()==0 fails deterministically -- same "domain size must be known and
// positive" contract M16-F's ExpectedCardinality already enforced (see this test file's own header
// comment on why this is a preserved, not invented, policy).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionEmptyBaseTest, "VertexMaskForge.FillLayerResolution.EmptyBase", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionEmptyBaseTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	FVertexMaskForgeFillLayer Layer = MakeWhiteFillCopyLayer(); // Empty MaskStack too -- irrelevant, base is checked first.

	const TArray<FVector3f> EmptyBase;

	FVertexMaskForgeFillLayerEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer, Store, EmptyBase, Output);

	TestFalse(TEXT("Evaluation fails for an empty PerSampleBase"), bSuccess);
	TestEqual(TEXT("Output stays empty"), Output.EffectiveMask.Num(), 0);
	TestEqual(TEXT("Store unaffected"), Store.Num(), 0);

	return true;
}

// Regression (updated): output is temporary -- recipe/Fill Layer/Mask Instance/store all untouched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionTemporaryOutputTest, "VertexMaskForge.FillLayerResolution.TemporaryOutput", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionTemporaryOutputTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	Store.StoreOrReplace(A.InstanceId, MakeFillLayerTestResult({ 0.5f }));

	FVertexMaskForgeFillLayer Layer = MakeWhiteFillCopyLayer();
	Layer.MaskStack.Add(A);
	const FGuid OriginalLayerId = Layer.LayerId;
	const int32 OriginalMaskStackNum = Layer.MaskStack.Num();

	const TArray<FVector3f> Base = { FVector3f::ZeroVector };

	FVertexMaskForgeFillLayerEvaluationOutput Output;
	VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer, Store, Base, Output);

	TestEqual(TEXT("LayerId unchanged"), Layer.LayerId, OriginalLayerId);
	TestEqual(TEXT("MaskStack.Num() unchanged"), Layer.MaskStack.Num(), OriginalMaskStackNum);
	TestEqual(TEXT("Store still holds only the original entry"), Store.Num(), 1);

	return true;
}

// Regression (updated): a failed evaluation preserves the caller's prior output exactly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionFailurePreservesOutputTest, "VertexMaskForge.FillLayerResolution.FailurePreservesOutput", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionFailurePreservesOutputTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store; // Empty -- guarantees failure below.
	const FVertexMaskForgeMaskInstance A = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);

	FVertexMaskForgeFillLayer Layer = MakeWhiteFillCopyLayer();
	Layer.MaskStack.Add(A);

	const TArray<FVector3f> Base = { FVector3f::ZeroVector, FVector3f::ZeroVector };

	FVertexMaskForgeFillLayerEvaluationOutput Output;
	Output.EffectiveMask = { 0.11f, 0.22f };
	Output.Composite = { FVector3f(0.1f, 0.2f, 0.3f), FVector3f(0.4f, 0.5f, 0.6f) };

	const bool bSuccess = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer, Store, Base, Output);

	TestFalse(TEXT("Evaluation fails"), bSuccess);
	TestEqual(TEXT("EffectiveMask sentinel preserved (Num)"), Output.EffectiveMask.Num(), 2);
	TestEqual(TEXT("Composite sentinel preserved (Num)"), Output.Composite.Num(), 2);
	if (Output.EffectiveMask.Num() == 2 && Output.Composite.Num() == 2)
	{
		TestNearlyEqual(TEXT("EffectiveMask[0] sentinel"), Output.EffectiveMask[0], 0.11f, FLR_Tolerance);
		TestNearlyEqual(TEXT("EffectiveMask[1] sentinel"), Output.EffectiveMask[1], 0.22f, FLR_Tolerance);
		ExpectVector(*this, TEXT("Composite[0] sentinel"), Output.Composite[0], FVector3f(0.1f, 0.2f, 0.3f));
		ExpectVector(*this, TEXT("Composite[1] sentinel"), Output.Composite[1], FVector3f(0.4f, 0.5f, 0.6f));
	}

	return true;
}

// J. The per-sample base is read-only: the caller's own array is unchanged by evaluation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionBaseReadOnlyTest, "VertexMaskForge.FillLayerResolution.BaseReadOnly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionBaseReadOnlyTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	Store.StoreOrReplace(A.InstanceId, MakeFillLayerTestResult({ 0.5f, 0.5f }));

	FVertexMaskForgeFillLayer Layer = MakeWhiteFillCopyLayer();
	Layer.MaskStack.Add(A);

	TArray<FVector3f> Base = { FVector3f(0.1f, 0.2f, 0.3f), FVector3f(0.4f, 0.5f, 0.6f) };
	const TArray<FVector3f> BaseBefore = Base;

	FVertexMaskForgeFillLayerEvaluationOutput Output;
	VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer, Store, Base, Output);

	TestEqual(TEXT("Base.Num() unchanged"), Base.Num(), BaseBefore.Num());
	for (int32 i = 0; i < BaseBefore.Num(); ++i)
	{
		ExpectVector(*this, *FString::Printf(TEXT("Base[%d] unchanged"), i), Base[i], BaseBefore[i]);
	}

	return true;
}

// Regression (updated): the result store is read-only.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionStoreReadOnlyTest, "VertexMaskForge.FillLayerResolution.StoreReadOnly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionStoreReadOnlyTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	Store.StoreOrReplace(A.InstanceId, MakeFillLayerTestResult({ 0.42f, 0.84f }));

	const int32 NumBefore = Store.Num();
	const FVertexMaskForgeInstanceMaskResult* FoundBefore = Store.Find(A.InstanceId);
	const TArray<float> ValuesBefore = FoundBefore ? FoundBefore->Values : TArray<float>();

	FVertexMaskForgeFillLayer Layer = MakeWhiteFillCopyLayer();
	Layer.MaskStack.Add(A);

	const TArray<FVector3f> Base = { FVector3f::ZeroVector, FVector3f::ZeroVector };

	FVertexMaskForgeFillLayerEvaluationOutput Output;
	VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer, Store, Base, Output);

	TestEqual(TEXT("Num() unchanged"), Store.Num(), NumBefore);
	const FVertexMaskForgeInstanceMaskResult* FoundAfter = Store.Find(A.InstanceId);
	TestNotNull(TEXT("Still found after evaluation"), FoundAfter);
	if (FoundAfter)
	{
		TestEqual(TEXT("Values.Num() unchanged"), FoundAfter->Values.Num(), ValuesBefore.Num());
		for (int32 i = 0; i < ValuesBefore.Num(); ++i)
		{
			TestNearlyEqual(FString::Printf(TEXT("Values[%d] unchanged"), i), FoundAfter->Values[i], ValuesBefore[i], FLR_Tolerance);
		}
	}

	return true;
}

// Regression (updated): two independent result stores, same InstanceId, same per-sample base, no
// cross-store leakage.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionOwnerSeparationTest, "VertexMaskForge.FillLayerResolution.OwnerSeparation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionOwnerSeparationTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore StoreOne;
	FVertexMaskForgeInstanceResultStore StoreTwo;
	const FVertexMaskForgeMaskInstance Shared = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	StoreOne.StoreOrReplace(Shared.InstanceId, MakeFillLayerTestResult({ 0.3f, 0.3f }));
	StoreTwo.StoreOrReplace(Shared.InstanceId, MakeFillLayerTestResult({ 0.8f, 0.8f }));

	FVertexMaskForgeFillLayer Layer = MakeWhiteFillCopyLayer();
	Layer.MaskStack.Add(Shared);

	const TArray<FVector3f> Base = { FVector3f::ZeroVector, FVector3f(1.0f, 1.0f, 1.0f) };

	FVertexMaskForgeFillLayerEvaluationOutput OutputOne, OutputTwo;
	const bool bSuccessOne = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer, StoreOne, Base, OutputOne);
	const bool bSuccessTwo = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer, StoreTwo, Base, OutputTwo);

	TestTrue(TEXT("StoreOne evaluation succeeds"), bSuccessOne);
	TestTrue(TEXT("StoreTwo evaluation succeeds"), bSuccessTwo);
	if (bSuccessOne && bSuccessTwo)
	{
		TestNearlyEqual(TEXT("StoreOne EffectiveMask"), OutputOne.EffectiveMask[0], 0.3f, FLR_Tolerance);
		TestNearlyEqual(TEXT("StoreTwo EffectiveMask"), OutputTwo.EffectiveMask[0], 0.8f, FLR_Tolerance);
	}

	return true;
}

// M. Composability: the Composite output of one evaluation is used, unmodified, as the PerSampleBase of
// a second, independent evaluation call -- no orchestration exists to do this automatically; the test
// performs it explicitly across two separate calls, proving the API supports it without a recipe runner.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionOutputAsNextBaseTest, "VertexMaskForge.FillLayerResolution.OutputAsNextBase", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionOutputAsNextBaseTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	Store.StoreOrReplace(A.InstanceId, MakeFillLayerTestResult({ 0.5f, 0.5f }));

	FVertexMaskForgeFillLayer Layer1 = MakeWhiteFillCopyLayer();
	Layer1.MaskStack.Add(A);
	const TArray<FVector3f> Base1 = { FVector3f::ZeroVector, FVector3f(1.0f, 1.0f, 1.0f) };

	FVertexMaskForgeFillLayerEvaluationOutput Output1;
	const bool bSuccess1 = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer1, Store, Base1, Output1);
	TestTrue(TEXT("First evaluation succeeds"), bSuccess1);
	if (!bSuccess1) { return true; }

	// Coverage=0.5 both indices -> Composite1 = Lerp(Base1, White, 0.5).
	ExpectVector(*this, TEXT("Composite1[0]"), Output1.Composite[0], FVector3f(0.5f, 0.5f, 0.5f));
	ExpectVector(*this, TEXT("Composite1[1]"), Output1.Composite[1], FVector3f(1.0f, 1.0f, 1.0f));

	// Second, independent call: Output1.Composite becomes PerSampleBase for a different Fill Layer,
	// empty Mask Stack, Multiply-half -- Composite2[i] = Composite1[i] * 0.5.
	FVertexMaskForgeFillLayer Layer2 = MakeHalfMultiplyLayer();

	FVertexMaskForgeFillLayerEvaluationOutput Output2;
	const bool bSuccess2 = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer2, Store, Output1.Composite, Output2);

	TestTrue(TEXT("Second evaluation succeeds"), bSuccess2);
	if (bSuccess2)
	{
		ExpectVector(*this, TEXT("Composite2[0]"), Output2.Composite[0], FVector3f(0.25f, 0.25f, 0.25f));
		ExpectVector(*this, TEXT("Composite2[1]"), Output2.Composite[1], FVector3f(0.5f, 0.5f, 0.5f));
	}

	return true;
}

// Regression (updated): compatibility with a real M16-E-generated Material Slot keyed result
// (Source-Topology domain), now evaluated against a varying per-sample base.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionMaterialSlotCompatibilityTest, "VertexMaskForge.FillLayerResolution.MaterialSlotCompatibility", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionMaterialSlotCompatibilityTest::RunTest(const FString& Parameters)
{
	// Minimal two-triangle Source-Topology fixture -- see VertexMaskForgeMaterialSlotGeneratorTests.cpp's
	// own BuildFixtureWorkingMesh for the identical, independently-duplicated construction (test-only
	// code, not shared production logic).
	FVertexMaskForgeWorkingMesh WorkingMesh;
	WorkingMesh.Mesh = MakeUnique<UE::Geometry::FDynamicMesh3>();
	const int32 V0 = WorkingMesh.Mesh->AppendVertex(FVector3d(0.0, 0.0, 0.0));
	const int32 V1 = WorkingMesh.Mesh->AppendVertex(FVector3d(1.0, 0.0, 0.0));
	const int32 V2 = WorkingMesh.Mesh->AppendVertex(FVector3d(1.0, 1.0, 0.0));
	const int32 V3 = WorkingMesh.Mesh->AppendVertex(FVector3d(0.0, 1.0, 0.0));
	WorkingMesh.Mesh->AppendTriangle(V0, V1, V2);
	WorkingMesh.Mesh->AppendTriangle(V0, V2, V3);
	WorkingMesh.bMaterialSlotResolutionValid = true;
	WorkingMesh.bRenderVertexMaterialSlotAmbiguous = false;
	WorkingMesh.MaterialSlotOptions.SetNum(2);
	WorkingMesh.DynamicTriangleToMaterialSlot.Init(INDEX_NONE, WorkingMesh.Mesh->MaxTriangleID());
	WorkingMesh.DynamicTriangleToMaterialSlot[0] = 0;
	WorkingMesh.DynamicTriangleToMaterialSlot[1] = 1;

	FVertexMaskForgeMaskInstance A = FVertexMaskForgeMaskInstance::Make(EVertexMaskForgeGeneratorType::MaterialSlot);
	A.Params.Get<FVertexMaskForgeMaterialSlotParams>().SelectedSlotIndex = 0;
	A.BlendMode = EVertexMaskForgeBlendMode::Copy;
	A.Opacity = 1.0f;

	const bool bGenerated = VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult(
		A, WorkingMesh, /*bUseSourceTopology=*/true, /*LOD0=*/nullptr);
	TestTrue(TEXT("M16-E generation succeeds"), bGenerated);

	FVertexMaskForgeFillLayer Layer = MakeWhiteFillCopyLayer();
	Layer.MaskStack.Add(A);

	TArray<FVector3f> Base;
	Base.Init(FVector3f::ZeroVector, 6); // 6 corners (2 triangles * 3), matching M16-E's own domain.

	FVertexMaskForgeFillLayerEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(
		Layer, WorkingMesh.InstanceResults, Base, Output);

	TestTrue(TEXT("Evaluation against M16-E result succeeds"), bSuccess);
	if (bSuccess)
	{
		// Triangle 0 (slot 0, selected) -> corners 0-2 = 1.0; Triangle 1 (slot 1) -> corners 3-5 = 0.0.
		for (int32 i = 0; i < 3; ++i)
		{
			TestNearlyEqual(FString::Printf(TEXT("EffectiveMask[%d] (tri 0)"), i), Output.EffectiveMask[i], 1.0f, FLR_Tolerance);
		}
		for (int32 i = 3; i < 6; ++i)
		{
			TestNearlyEqual(FString::Printf(TEXT("EffectiveMask[%d] (tri 1)"), i), Output.EffectiveMask[i], 0.0f, FLR_Tolerance);
		}
	}

	return true;
}

// --- M16-I.1: disabled Mask Instance semantics -----------------------------------------------------
// A disabled Mask Instance (bEnabled == false) is semantically ABSENT: skipped before InstanceId
// validation and before any store lookup, never folded, and a Mask Stack containing only disabled
// entries behaves exactly like an empty one.

// A. Disabled instance with an invalid InstanceId never causes a failure.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionDisabledInvalidGuidTest, "VertexMaskForge.FillLayerResolution.DisabledInstanceInvalidGuid", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionDisabledInvalidGuidTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store; // Empty -- must never be queried.
	FVertexMaskForgeMaskInstance Disabled = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Copy, 1.0f, /*bEnabled=*/false);
	Disabled.InstanceId = FGuid(); // Deliberately invalid.

	FVertexMaskForgeFillLayer Layer = MakeWhiteFillCopyLayer();
	Layer.MaskStack.Add(Disabled);

	const TArray<FVector3f> Base = { FVector3f(0.3f, 0.3f, 0.3f) };
	FVertexMaskForgeFillLayerEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer, Store, Base, Output);

	TestTrue(TEXT("Succeeds despite invalid GUID -- disabled instance never inspected"), bSuccess);
	if (bSuccess)
	{
		TestNearlyEqual(TEXT("EffectiveMask == 1.0 (empty-stack semantics)"), Output.EffectiveMask[0], 1.0f, FLR_Tolerance);
		ExpectVector(*this, TEXT("Composite == white"), Output.Composite[0], FVector3f(1.0f, 1.0f, 1.0f));
	}
	TestEqual(TEXT("Store stayed empty"), Store.Num(), 0);

	return true;
}

// B. Disabled instance with a valid GUID but no store entry never causes a failure.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionDisabledMissingResultTest, "VertexMaskForge.FillLayerResolution.DisabledInstanceMissingResult", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionDisabledMissingResultTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store; // Empty -- the disabled instance's (valid) GUID is never stored.
	const FVertexMaskForgeMaskInstance Disabled = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Copy, 1.0f, /*bEnabled=*/false);

	FVertexMaskForgeFillLayer Layer = MakeWhiteFillCopyLayer();
	Layer.MaskStack.Add(Disabled);

	const TArray<FVector3f> Base = { FVector3f(0.3f, 0.3f, 0.3f) };
	FVertexMaskForgeFillLayerEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer, Store, Base, Output);

	TestTrue(TEXT("Succeeds -- no result is ever required for a disabled instance"), bSuccess);
	if (bSuccess)
	{
		TestNearlyEqual(TEXT("EffectiveMask == 1.0"), Output.EffectiveMask[0], 1.0f, FLR_Tolerance);
	}
	TestEqual(TEXT("Store stayed empty"), Store.Num(), 0);

	return true;
}

// C. Disabled instance whose GUID has a sparse/incompatible existing entry: still never inspected.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionDisabledInvalidExistingResultTest, "VertexMaskForge.FillLayerResolution.DisabledInstanceInvalidExistingResult", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionDisabledInvalidExistingResultTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance Disabled = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Copy, 1.0f, /*bEnabled=*/false);

	// A store entry that would fail validation if this instance were enabled (cardinality mismatch: 1
	// sample expected, 3 stored) -- representable via the real type, unlike a "failed/stale" State enum
	// which FVertexMaskForgeInstanceMaskResult does not have.
	Store.StoreOrReplace(Disabled.InstanceId, MakeFillLayerTestResult({ 0.1f, 0.2f, 0.3f }));

	FVertexMaskForgeFillLayer Layer = MakeWhiteFillCopyLayer();
	Layer.MaskStack.Add(Disabled);

	const TArray<FVector3f> Base = { FVector3f(0.3f, 0.3f, 0.3f) }; // Cardinality 1 -- would mismatch the stored 3.
	FVertexMaskForgeFillLayerEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer, Store, Base, Output);

	TestTrue(TEXT("Succeeds -- the incompatible existing entry is never read for a disabled instance"), bSuccess);
	if (bSuccess)
	{
		TestNearlyEqual(TEXT("EffectiveMask == 1.0"), Output.EffectiveMask[0], 1.0f, FLR_Tolerance);
	}
	TestEqual(TEXT("Store unaffected"), Store.Num(), 1);

	return true;
}

// D. Mixed enabled/disabled stack: result equals folding only the enabled entries.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionMixedStackTest, "VertexMaskForge.FillLayerResolution.MixedEnabledDisabledStack", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionMixedStackTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	FVertexMaskForgeMaskInstance TrulyDisabled = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Copy, 1.0f, /*bEnabled=*/false);
	TrulyDisabled.InstanceId = FGuid(); // Invalid -- must not matter.

	const FVertexMaskForgeMaskInstance Enabled = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Copy, 1.0f, /*bEnabled=*/true);
	Store.StoreOrReplace(Enabled.InstanceId, MakeFillLayerTestResult({ 0.4f }));

	FVertexMaskForgeFillLayer Layer = MakeWhiteFillCopyLayer();
	Layer.MaskStack.Add(TrulyDisabled); // Before the enabled entry -- must not affect the fold.
	Layer.MaskStack.Add(Enabled);

	const TArray<FVector3f> Base = { FVector3f::ZeroVector };
	FVertexMaskForgeFillLayerEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer, Store, Base, Output);

	TestTrue(TEXT("Succeeds"), bSuccess);
	if (bSuccess)
	{
		// Only the enabled Copy(0.4) entry folds: seed 1.0 -> Copy(0.4) -> 0.4.
		TestNearlyEqual(TEXT("EffectiveMask == 0.4 (only the enabled entry)"), Output.EffectiveMask[0], 0.4f, FLR_Tolerance);
		ExpectVector(*this, TEXT("Composite"), Output.Composite[0], FVector3f(0.4f, 0.4f, 0.4f));
	}

	return true;
}

// E. A disabled instance's own resolvable value, even if present, is never folded into the result.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionDisabledValueNeverFoldedTest, "VertexMaskForge.FillLayerResolution.DisabledValueNeverFolded", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionDisabledValueNeverFoldedTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance Disabled = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Copy, 1.0f, /*bEnabled=*/false);
	// A value that, if folded (Copy), would drive EffectiveMask to 0.9 -- clearly distinct from the
	// empty-stack value of 1.0 this test expects instead.
	Store.StoreOrReplace(Disabled.InstanceId, MakeFillLayerTestResult({ 0.9f }));

	FVertexMaskForgeFillLayer Layer = MakeWhiteFillCopyLayer();
	Layer.MaskStack.Add(Disabled);

	const TArray<FVector3f> Base = { FVector3f::ZeroVector };
	FVertexMaskForgeFillLayerEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer, Store, Base, Output);

	TestTrue(TEXT("Succeeds"), bSuccess);
	if (bSuccess)
	{
		TestNearlyEqual(TEXT("EffectiveMask == 1.0, NOT 0.9 -- disabled value never folded"), Output.EffectiveMask[0], 1.0f, FLR_Tolerance);
	}

	return true;
}

// F. A Mask Stack containing only disabled instances (with deliberately invalid content) behaves
// exactly like an empty Mask Stack.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeFillLayerResolutionOnlyDisabledStackTest, "VertexMaskForge.FillLayerResolution.OnlyDisabledStack", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeFillLayerResolutionOnlyDisabledStackTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store; // Empty -- must never be queried.
	FVertexMaskForgeMaskInstance DisabledInvalidGuid = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Multiply, 0.5f, /*bEnabled=*/false);
	DisabledInvalidGuid.InstanceId = FGuid();
	const FVertexMaskForgeMaskInstance DisabledUnstored = MakeFillLayerTestInstance(EVertexMaskForgeBlendMode::Subtract, 1.0f, /*bEnabled=*/false);

	FVertexMaskForgeFillLayer Layer = MakeWhiteFillCopyLayer();
	Layer.MaskStack.Add(DisabledInvalidGuid);
	Layer.MaskStack.Add(DisabledUnstored);

	const TArray<FVector3f> Base = { FVector3f(0.2f, 0.2f, 0.2f) };
	FVertexMaskForgeFillLayerEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer, Store, Base, Output);

	TestTrue(TEXT("Succeeds -- equivalent to an empty Mask Stack"), bSuccess);
	if (bSuccess)
	{
		TestNearlyEqual(TEXT("EffectiveMask == 1.0"), Output.EffectiveMask[0], 1.0f, FLR_Tolerance);
		ExpectVector(*this, TEXT("Composite == white"), Output.Composite[0], FVector3f(1.0f, 1.0f, 1.0f));
	}
	TestEqual(TEXT("Store stayed empty"), Store.Num(), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
