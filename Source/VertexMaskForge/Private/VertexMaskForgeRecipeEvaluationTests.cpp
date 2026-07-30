// M16-H: automation tests for VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults
// -- ordered, whole-recipe orchestration on top of the M16-F/G single-Fill-Layer boundary
// (VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults). Every expected value below is
// derived directly from the real evaluator formulas already proven in
// VertexMaskForgeSequentialEvaluatorTests.cpp / VertexMaskForgeFillLayerResolutionTests.cpp, not assumed.
//
// Structural proofs (no dedicated test, verified instead by source inspection / diff / call graph, per
// this checkpoint's own contract): VertexMaskForgeRecipeEvaluation.cpp includes no Material Slot (or any
// other) generator header, calls no VertexMaskForgeInstanceResultStore mutator, never assigns to any
// field of FVertexMaskForgeRecipe/FVertexMaskForgeFillLayer/FVertexMaskForgeMaskInstance, and never calls
// VertexMaskForgeSequentialEvaluator directly (only through VertexMaskForgeFillLayerResolution's own
// boundary) -- confirming this module is orchestration only, with zero duplicated resolution/math.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VertexMaskForgeFillLayerResolution.h"
#include "VertexMaskForgeInstanceResultStore.h"
#include "VertexMaskForgeRecipeEvaluation.h"
#include "VertexMaskForgeRecipeTypes.h"

namespace
{
	constexpr float RCP_Tolerance = 1e-4f;

	FVertexMaskForgeInstanceMaskResult MakeRecipeResult(const TArray<float>& Values)
	{
		FVertexMaskForgeInstanceMaskResult Result;
		Result.Values = Values;
		Result.bHasValue.Init(true, Values.Num());
		return Result;
	}

	FVertexMaskForgeMaskInstance MakeRecipeInstance(const EVertexMaskForgeBlendMode Mode, const float Opacity, const bool bEnabled = true)
	{
		FVertexMaskForgeMaskInstance Instance = FVertexMaskForgeMaskInstance::Make(EVertexMaskForgeGeneratorType::MaterialSlot);
		Instance.BlendMode = Mode;
		Instance.Opacity = Opacity;
		Instance.bEnabled = bEnabled;
		return Instance;
	}

	/** White fill, Copy blend, full layer opacity -- Composite[i] = Lerp(Base[i], White, Coverage[i]). */
	FVertexMaskForgeFillLayer MakeRecipeWhiteCopyLayer()
	{
		FVertexMaskForgeFillLayer Layer = FVertexMaskForgeFillLayer::Make();
		Layer.FillValue = FVector3f(1.0f, 1.0f, 1.0f);
		Layer.BlendMode = EVertexMaskForgeBlendMode::Copy;
		Layer.Opacity = 1.0f;
		Layer.bEnabled = true;
		return Layer;
	}

	/** 0.5 fill, Multiply blend, full opacity, empty Mask Stack -- Coverage=1 always, so
	 *  Composite[i] = Base[i] * 0.5 regardless of what other layers do. */
	FVertexMaskForgeFillLayer MakeRecipeHalfMultiplyLayer()
	{
		FVertexMaskForgeFillLayer Layer = FVertexMaskForgeFillLayer::Make();
		Layer.FillValue = FVector3f(0.5f, 0.5f, 0.5f);
		Layer.BlendMode = EVertexMaskForgeBlendMode::Multiply;
		Layer.Opacity = 1.0f;
		Layer.bEnabled = true;
		return Layer;
	}

	/** 0.3 fill, Subtract blend, full opacity, empty Mask Stack -- Coverage=1 always, so
	 *  Composite[i] = Clamp01(Base[i] - 0.3). */
	FVertexMaskForgeFillLayer MakeRecipeSubtractLayer()
	{
		FVertexMaskForgeFillLayer Layer = FVertexMaskForgeFillLayer::Make();
		Layer.FillValue = FVector3f(0.3f, 0.3f, 0.3f);
		Layer.BlendMode = EVertexMaskForgeBlendMode::Subtract;
		Layer.Opacity = 1.0f;
		Layer.bEnabled = true;
		return Layer;
	}

	void ExpectRecipeVector(FAutomationTestBase& Test, const TCHAR* What, const FVector3f& Actual, const FVector3f& Expected)
	{
		Test.TestTrue(FString::Printf(TEXT("%s == (%f,%f,%f)"), What, Expected.X, Expected.Y, Expected.Z),
			Actual.Equals(Expected, RCP_Tolerance));
	}
}

// A. Empty recipe acts as identity over the initial base.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeEvaluationEmptyRecipeTest, "VertexMaskForge.RecipeEvaluation.EmptyRecipe", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeEvaluationEmptyRecipeTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store; // Empty -- must never be queried.
	FVertexMaskForgeRecipe Recipe; // FillLayers left empty.

	const TArray<FVector3f> Base = { FVector3f(0.2f, 0.4f, 0.6f), FVector3f(1.0f, 1.0f, 1.0f), FVector3f(0.0f, 0.0f, 0.0f) };

	FVertexMaskForgeRecipeEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(Recipe, Store, Base, Output);

	TestTrue(TEXT("Evaluation succeeds"), bSuccess);
	if (bSuccess)
	{
		TestEqual(TEXT("Composite.Num()"), Output.Composite.Num(), 3);
		for (int32 i = 0; i < 3; ++i)
		{
			ExpectRecipeVector(*this, *FString::Printf(TEXT("Composite[%d] == Base[%d]"), i, i), Output.Composite[i], Base[i]);
		}
	}
	TestEqual(TEXT("Store stayed empty (never queried)"), Store.Num(), 0);

	return true;
}

// B. Exactly one Fill Layer: recipe result must equal a direct M16-G call with the same inputs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeEvaluationOneLayerTest, "VertexMaskForge.RecipeEvaluation.OneLayerMatchesDirectCall", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeEvaluationOneLayerTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeRecipeInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	Store.StoreOrReplace(A.InstanceId, MakeRecipeResult({ 0.3f, 0.6f, 0.9f }));

	FVertexMaskForgeFillLayer Layer = MakeRecipeWhiteCopyLayer();
	Layer.MaskStack.Add(A);

	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	const TArray<FVector3f> Base = { FVector3f(0.2f, 0.4f, 0.6f), FVector3f(1.0f, 1.0f, 1.0f), FVector3f(0.0f, 0.0f, 0.0f) };

	FVertexMaskForgeRecipeEvaluationOutput RecipeOutput;
	const bool bRecipeSuccess = VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(Recipe, Store, Base, RecipeOutput);

	FVertexMaskForgeFillLayerEvaluationOutput DirectOutput;
	const bool bDirectSuccess = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer, Store, Base, DirectOutput);

	TestTrue(TEXT("Recipe evaluation succeeds"), bRecipeSuccess);
	TestTrue(TEXT("Direct M16-G call succeeds"), bDirectSuccess);
	if (bRecipeSuccess && bDirectSuccess)
	{
		TestEqual(TEXT("Same cardinality"), RecipeOutput.Composite.Num(), DirectOutput.Composite.Num());
		for (int32 i = 0; i < RecipeOutput.Composite.Num(); ++i)
		{
			ExpectRecipeVector(*this, *FString::Printf(TEXT("Recipe[%d] == Direct[%d]"), i, i), RecipeOutput.Composite[i], DirectOutput.Composite[i]);
		}
	}

	return true;
}

// C. Two Fill Layers: the first layer's output becomes the second layer's base.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeEvaluationTwoLayersTest, "VertexMaskForge.RecipeEvaluation.TwoLayers", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeEvaluationTwoLayersTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeRecipeInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	Store.StoreOrReplace(A.InstanceId, MakeRecipeResult({ 0.5f, 0.5f })); // Coverage=0.5 both samples.

	FVertexMaskForgeFillLayer Layer1 = MakeRecipeWhiteCopyLayer();
	Layer1.MaskStack.Add(A);
	FVertexMaskForgeFillLayer Layer2 = MakeRecipeHalfMultiplyLayer(); // Empty stack.

	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer1);
	Recipe.FillLayers.Add(Layer2);

	const TArray<FVector3f> Base = { FVector3f::ZeroVector, FVector3f(1.0f, 1.0f, 1.0f) };

	FVertexMaskForgeRecipeEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(Recipe, Store, Base, Output);

	TestTrue(TEXT("Evaluation succeeds"), bSuccess);
	if (bSuccess)
	{
		// Composite1[i] = Lerp(Base[i], White, 0.5); Composite2[i] = Composite1[i] * 0.5.
		ExpectRecipeVector(*this, TEXT("Composite[0]"), Output.Composite[0], FVector3f(0.25f, 0.25f, 0.25f));
		ExpectRecipeVector(*this, TEXT("Composite[1]"), Output.Composite[1], FVector3f(0.5f, 0.5f, 0.5f));
	}

	return true;
}

// D. Three Fill Layers, three samples: full fold correctness, no broadcasting.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeEvaluationThreeLayersTest, "VertexMaskForge.RecipeEvaluation.ThreeLayers", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeEvaluationThreeLayersTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeRecipeInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	const FVertexMaskForgeMaskInstance B = MakeRecipeInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	Store.StoreOrReplace(A.InstanceId, MakeRecipeResult({ 0.5f, 0.5f, 0.5f }));
	Store.StoreOrReplace(B.InstanceId, MakeRecipeResult({ 0.25f, 0.25f, 0.25f }));

	FVertexMaskForgeFillLayer Layer1 = MakeRecipeWhiteCopyLayer();
	Layer1.MaskStack.Add(A);
	FVertexMaskForgeFillLayer Layer2 = MakeRecipeHalfMultiplyLayer(); // Empty stack.
	FVertexMaskForgeFillLayer Layer3 = MakeRecipeWhiteCopyLayer();
	Layer3.MaskStack.Add(B);

	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer1);
	Recipe.FillLayers.Add(Layer2);
	Recipe.FillLayers.Add(Layer3);

	const TArray<FVector3f> Base = { FVector3f::ZeroVector, FVector3f(1.0f, 1.0f, 1.0f), FVector3f(0.5f, 0.5f, 0.5f) };

	FVertexMaskForgeRecipeEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(Recipe, Store, Base, Output);

	TestTrue(TEXT("Evaluation succeeds"), bSuccess);
	if (bSuccess)
	{
		// Composite1=(Base+1)/2; Composite2=Composite1*0.5; Composite3=Composite2*0.75+0.25.
		ExpectRecipeVector(*this, TEXT("Composite[0]"), Output.Composite[0], FVector3f(0.4375f, 0.4375f, 0.4375f));
		ExpectRecipeVector(*this, TEXT("Composite[1]"), Output.Composite[1], FVector3f(0.625f, 0.625f, 0.625f));
		ExpectRecipeVector(*this, TEXT("Composite[2]"), Output.Composite[2], FVector3f(0.53125f, 0.53125f, 0.53125f));
	}

	return true;
}

// E. Recipe (Fill Layer) order is preserved and observable: [Multiply,Subtract] != [Subtract,Multiply].
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeEvaluationOrderTest, "VertexMaskForge.RecipeEvaluation.LayerOrderPreserved", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeEvaluationOrderTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store; // Both layers use empty stacks -- store irrelevant here.
	FVertexMaskForgeFillLayer LayerMultiply = MakeRecipeHalfMultiplyLayer();
	FVertexMaskForgeFillLayer LayerSubtract = MakeRecipeSubtractLayer();

	FVertexMaskForgeRecipe RecipeXY;
	RecipeXY.FillLayers.Add(LayerMultiply);
	RecipeXY.FillLayers.Add(LayerSubtract);

	FVertexMaskForgeRecipe RecipeYX;
	RecipeYX.FillLayers.Add(LayerSubtract);
	RecipeYX.FillLayers.Add(LayerMultiply);

	const TArray<FVector3f> Base = { FVector3f(1.0f, 1.0f, 1.0f) };

	FVertexMaskForgeRecipeEvaluationOutput OutputXY, OutputYX;
	const bool bSuccessXY = VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(RecipeXY, Store, Base, OutputXY);
	const bool bSuccessYX = VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(RecipeYX, Store, Base, OutputYX);

	TestTrue(TEXT("XY succeeds"), bSuccessXY);
	TestTrue(TEXT("YX succeeds"), bSuccessYX);
	if (bSuccessXY && bSuccessYX)
	{
		// XY: 1*0.5=0.5 -> 0.5-0.3=0.2. YX: 1-0.3=0.7 -> 0.7*0.5=0.35.
		ExpectRecipeVector(*this, TEXT("[Multiply,Subtract]"), OutputXY.Composite[0], FVector3f(0.2f, 0.2f, 0.2f));
		ExpectRecipeVector(*this, TEXT("[Subtract,Multiply]"), OutputYX.Composite[0], FVector3f(0.35f, 0.35f, 0.35f));
	}

	return true;
}

// F. Spatial index correspondence survives a multi-layer fold -- no shift/remap/broadcast.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeEvaluationIndexCorrespondenceTest, "VertexMaskForge.RecipeEvaluation.IndexCorrespondencePreserved", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeEvaluationIndexCorrespondenceTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeRecipeInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	Store.StoreOrReplace(A.InstanceId, MakeRecipeResult({ 0.5f, 0.5f, 0.5f })); // Uniform Coverage isolates the base's own contribution.

	FVertexMaskForgeFillLayer Layer1 = MakeRecipeWhiteCopyLayer();
	Layer1.MaskStack.Add(A);
	FVertexMaskForgeFillLayer Layer2 = MakeRecipeHalfMultiplyLayer();

	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer1);
	Recipe.FillLayers.Add(Layer2);

	const TArray<FVector3f> Base = {
		FVector3f(1.0f, 0.0f, 0.0f), // red
		FVector3f(0.0f, 1.0f, 0.0f), // green
		FVector3f(0.0f, 0.0f, 1.0f), // blue
	};

	FVertexMaskForgeRecipeEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(Recipe, Store, Base, Output);

	TestTrue(TEXT("Evaluation succeeds"), bSuccess);
	if (bSuccess)
	{
		// Composite1[i] = 0.5*Base[i]+0.5; Composite2[i] = Composite1[i]*0.5.
		ExpectRecipeVector(*this, TEXT("Composite[0] (from red)"), Output.Composite[0], FVector3f(0.5f, 0.25f, 0.25f));
		ExpectRecipeVector(*this, TEXT("Composite[1] (from green)"), Output.Composite[1], FVector3f(0.25f, 0.5f, 0.25f));
		ExpectRecipeVector(*this, TEXT("Composite[2] (from blue)"), Output.Composite[2], FVector3f(0.25f, 0.25f, 0.5f));
	}

	return true;
}

// G. Empty Mask Stack within a single-layer recipe: EffectiveMask=1.0 uniformly, layer still applied
// per-sample correctly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeEvaluationEmptyStackLayerTest, "VertexMaskForge.RecipeEvaluation.EmptyMaskStackInLayer", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeEvaluationEmptyStackLayerTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store; // Empty -- must never be queried (empty Mask Stack).
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(MakeRecipeHalfMultiplyLayer()); // Empty Mask Stack.

	const TArray<FVector3f> Base = { FVector3f(0.2f, 0.4f, 0.6f), FVector3f(1.0f, 1.0f, 1.0f), FVector3f(0.0f, 0.0f, 0.0f) };

	FVertexMaskForgeRecipeEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(Recipe, Store, Base, Output);

	TestTrue(TEXT("Evaluation succeeds"), bSuccess);
	if (bSuccess)
	{
		ExpectRecipeVector(*this, TEXT("Composite[0]"), Output.Composite[0], FVector3f(0.1f, 0.2f, 0.3f));
		ExpectRecipeVector(*this, TEXT("Composite[1]"), Output.Composite[1], FVector3f(0.5f, 0.5f, 0.5f));
		ExpectRecipeVector(*this, TEXT("Composite[2]"), Output.Composite[2], FVector3f(0.0f, 0.0f, 0.0f));
	}
	TestEqual(TEXT("Store stayed empty"), Store.Num(), 0);

	return true;
}

// H. Empty-stack layer followed by a keyed layer: the second layer must receive the first layer's
// correct per-sample output, not the original InitialPerSampleBase.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeEvaluationEmptyThenKeyedTest, "VertexMaskForge.RecipeEvaluation.EmptyStackThenKeyed", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeEvaluationEmptyThenKeyedTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeRecipeInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	Store.StoreOrReplace(A.InstanceId, MakeRecipeResult({ 0.4f, 0.4f }));

	FVertexMaskForgeFillLayer Layer1 = MakeRecipeHalfMultiplyLayer(); // Empty stack.
	FVertexMaskForgeFillLayer Layer2 = MakeRecipeWhiteCopyLayer();
	Layer2.MaskStack.Add(A);

	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer1);
	Recipe.FillLayers.Add(Layer2);

	const TArray<FVector3f> Base = { FVector3f::ZeroVector, FVector3f(1.0f, 1.0f, 1.0f) };

	FVertexMaskForgeRecipeEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(Recipe, Store, Base, Output);

	TestTrue(TEXT("Evaluation succeeds"), bSuccess);
	if (bSuccess)
	{
		// Composite1 = Base*0.5 = [(0,0,0),(0.5,0.5,0.5)]; Composite2[i] = Composite1[i]*0.6+0.4.
		ExpectRecipeVector(*this, TEXT("Composite[0]"), Output.Composite[0], FVector3f(0.4f, 0.4f, 0.4f));
		ExpectRecipeVector(*this, TEXT("Composite[1]"), Output.Composite[1], FVector3f(0.7f, 0.7f, 0.7f));
	}

	return true;
}

// I. Failure on the FIRST layer fails the whole evaluation; sentinel/base/store untouched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeEvaluationFirstLayerFailsTest, "VertexMaskForge.RecipeEvaluation.FirstLayerFails", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeEvaluationFirstLayerFailsTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store; // Empty -- A is never stored.
	const FVertexMaskForgeMaskInstance A = MakeRecipeInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);

	FVertexMaskForgeFillLayer Layer = MakeRecipeWhiteCopyLayer();
	Layer.MaskStack.Add(A);

	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	const TArray<FVector3f> Base = { FVector3f::ZeroVector };

	FVertexMaskForgeRecipeEvaluationOutput Output;
	Output.Composite = { FVector3f(0.9f, 0.9f, 0.9f) }; // Sentinel.

	const bool bSuccess = VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(Recipe, Store, Base, Output);

	TestFalse(TEXT("Evaluation fails"), bSuccess);
	TestEqual(TEXT("Sentinel Num preserved"), Output.Composite.Num(), 1);
	if (Output.Composite.Num() == 1)
	{
		ExpectRecipeVector(*this, TEXT("Sentinel preserved"), Output.Composite[0], FVector3f(0.9f, 0.9f, 0.9f));
	}
	TestEqual(TEXT("Store stays empty"), Store.Num(), 0);

	return true;
}

// J. Failure on an INTERMEDIATE layer: earlier valid layers' composites are never published, later
// layers never run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeEvaluationMiddleLayerFailsTest, "VertexMaskForge.RecipeEvaluation.MiddleLayerFails", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeEvaluationMiddleLayerFailsTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance Missing = MakeRecipeInstance(EVertexMaskForgeBlendMode::Copy, 1.0f); // Never stored.
	const FVertexMaskForgeMaskInstance C = MakeRecipeInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	Store.StoreOrReplace(C.InstanceId, MakeRecipeResult({ 0.5f })); // Would succeed alone if reached.

	FVertexMaskForgeFillLayer Layer0 = MakeRecipeHalfMultiplyLayer(); // Valid, empty stack.
	FVertexMaskForgeFillLayer Layer1 = MakeRecipeWhiteCopyLayer();
	Layer1.MaskStack.Add(Missing); // Invalid -- forces failure here.
	FVertexMaskForgeFillLayer Layer2 = MakeRecipeWhiteCopyLayer();
	Layer2.MaskStack.Add(C); // Would succeed if the fold reached it.

	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer0);
	Recipe.FillLayers.Add(Layer1);
	Recipe.FillLayers.Add(Layer2);

	const TArray<FVector3f> Base = { FVector3f(1.0f, 1.0f, 1.0f) };

	FVertexMaskForgeRecipeEvaluationOutput Output;
	Output.Composite = { FVector3f(0.42f, 0.42f, 0.42f) }; // Sentinel.

	const bool bSuccess = VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(Recipe, Store, Base, Output);

	TestFalse(TEXT("Whole recipe evaluation fails"), bSuccess);
	TestEqual(TEXT("Sentinel Num preserved"), Output.Composite.Num(), 1);
	if (Output.Composite.Num() == 1)
	{
		ExpectRecipeVector(*this, TEXT("Sentinel preserved"), Output.Composite[0], FVector3f(0.42f, 0.42f, 0.42f));
	}
	TestEqual(TEXT("Store unaffected"), Store.Num(), 1);

	return true;
}

// K. Failure on the LAST layer: earlier successful layers still fail the whole evaluation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeEvaluationLastLayerFailsTest, "VertexMaskForge.RecipeEvaluation.LastLayerFails", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeEvaluationLastLayerFailsTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance Missing = MakeRecipeInstance(EVertexMaskForgeBlendMode::Copy, 1.0f); // Never stored.

	FVertexMaskForgeFillLayer Layer0 = MakeRecipeHalfMultiplyLayer();
	FVertexMaskForgeFillLayer Layer1 = MakeRecipeSubtractLayer();
	FVertexMaskForgeFillLayer Layer2 = MakeRecipeWhiteCopyLayer();
	Layer2.MaskStack.Add(Missing); // Invalid -- last layer fails.

	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer0);
	Recipe.FillLayers.Add(Layer1);
	Recipe.FillLayers.Add(Layer2);

	const TArray<FVector3f> Base = { FVector3f(1.0f, 1.0f, 1.0f) };

	FVertexMaskForgeRecipeEvaluationOutput Output;
	Output.Composite = { FVector3f(0.77f, 0.77f, 0.77f) }; // Sentinel.

	const bool bSuccess = VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(Recipe, Store, Base, Output);

	TestFalse(TEXT("Evaluation fails"), bSuccess);
	if (Output.Composite.Num() == 1)
	{
		ExpectRecipeVector(*this, TEXT("Sentinel preserved"), Output.Composite[0], FVector3f(0.77f, 0.77f, 0.77f));
	}

	return true;
}

// L. Cardinality incompatible between the initial base and a keyed result fails deterministically.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeEvaluationCardinalityMismatchTest, "VertexMaskForge.RecipeEvaluation.CardinalityMismatch", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeEvaluationCardinalityMismatchTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeRecipeInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	Store.StoreOrReplace(A.InstanceId, MakeRecipeResult({ 0.1f, 0.2f, 0.3f, 0.4f })); // M=4

	FVertexMaskForgeFillLayer Layer = MakeRecipeWhiteCopyLayer();
	Layer.MaskStack.Add(A);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	const TArray<FVector3f> Base = { FVector3f::ZeroVector, FVector3f::ZeroVector, FVector3f::ZeroVector }; // N=3

	FVertexMaskForgeRecipeEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(Recipe, Store, Base, Output);

	TestFalse(TEXT("Evaluation fails on cardinality mismatch"), bSuccess);
	TestEqual(TEXT("Output stays empty"), Output.Composite.Num(), 0);
	TestEqual(TEXT("Store unaffected"), Store.Num(), 1);

	return true;
}

// M. Empty initial base fails deterministically, for both an empty and a non-empty recipe.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeEvaluationEmptyBaseTest, "VertexMaskForge.RecipeEvaluation.EmptyBase", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeEvaluationEmptyBaseTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const TArray<FVector3f> EmptyBase;

	// Empty recipe + empty base.
	{
		FVertexMaskForgeRecipe EmptyRecipe;
		FVertexMaskForgeRecipeEvaluationOutput Output;
		const bool bSuccess = VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(EmptyRecipe, Store, EmptyBase, Output);
		TestFalse(TEXT("Empty recipe + empty base fails"), bSuccess);
		TestEqual(TEXT("Output empty"), Output.Composite.Num(), 0);
	}

	// Non-empty recipe + empty base.
	{
		FVertexMaskForgeRecipe Recipe;
		Recipe.FillLayers.Add(MakeRecipeHalfMultiplyLayer());
		FVertexMaskForgeRecipeEvaluationOutput Output;
		const bool bSuccess = VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(Recipe, Store, EmptyBase, Output);
		TestFalse(TEXT("Non-empty recipe + empty base fails"), bSuccess);
		TestEqual(TEXT("Output empty"), Output.Composite.Num(), 0);
	}

	TestEqual(TEXT("Store unaffected"), Store.Num(), 0);

	return true;
}

// N. The initial per-sample base is read-only.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeEvaluationBaseReadOnlyTest, "VertexMaskForge.RecipeEvaluation.BaseReadOnly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeEvaluationBaseReadOnlyTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeRecipeInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	Store.StoreOrReplace(A.InstanceId, MakeRecipeResult({ 0.5f, 0.5f }));

	FVertexMaskForgeFillLayer Layer = MakeRecipeWhiteCopyLayer();
	Layer.MaskStack.Add(A);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	TArray<FVector3f> Base = { FVector3f(0.1f, 0.2f, 0.3f), FVector3f(0.4f, 0.5f, 0.6f) };
	const TArray<FVector3f> BaseBefore = Base;

	FVertexMaskForgeRecipeEvaluationOutput Output;
	VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(Recipe, Store, Base, Output);

	TestEqual(TEXT("Base.Num() unchanged"), Base.Num(), BaseBefore.Num());
	for (int32 i = 0; i < BaseBefore.Num(); ++i)
	{
		ExpectRecipeVector(*this, *FString::Printf(TEXT("Base[%d] unchanged"), i), Base[i], BaseBefore[i]);
	}

	return true;
}

// O. The recipe (authoring state) is read-only.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeEvaluationRecipeReadOnlyTest, "VertexMaskForge.RecipeEvaluation.RecipeReadOnly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeEvaluationRecipeReadOnlyTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeRecipeInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	Store.StoreOrReplace(A.InstanceId, MakeRecipeResult({ 0.5f }));

	FVertexMaskForgeFillLayer Layer = MakeRecipeWhiteCopyLayer();
	Layer.MaskStack.Add(A);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	const FGuid LayerIdBefore = Recipe.FillLayers[0].LayerId;
	const int32 MaskStackNumBefore = Recipe.FillLayers[0].MaskStack.Num();
	const FGuid InstanceIdBefore = Recipe.FillLayers[0].MaskStack[0].InstanceId;
	const int32 NumLayersBefore = Recipe.FillLayers.Num();

	const TArray<FVector3f> Base = { FVector3f::ZeroVector };
	FVertexMaskForgeRecipeEvaluationOutput Output;
	VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(Recipe, Store, Base, Output);

	TestEqual(TEXT("FillLayers.Num() unchanged"), Recipe.FillLayers.Num(), NumLayersBefore);
	TestEqual(TEXT("LayerId unchanged"), Recipe.FillLayers[0].LayerId, LayerIdBefore);
	TestEqual(TEXT("MaskStack.Num() unchanged"), Recipe.FillLayers[0].MaskStack.Num(), MaskStackNumBefore);
	TestEqual(TEXT("InstanceId unchanged"), Recipe.FillLayers[0].MaskStack[0].InstanceId, InstanceIdBefore);

	return true;
}

// P. The result store is read-only.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeEvaluationStoreReadOnlyTest, "VertexMaskForge.RecipeEvaluation.StoreReadOnly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeEvaluationStoreReadOnlyTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeRecipeInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	Store.StoreOrReplace(A.InstanceId, MakeRecipeResult({ 0.42f, 0.84f }));

	const int32 NumBefore = Store.Num();
	const FVertexMaskForgeInstanceMaskResult* FoundBefore = Store.Find(A.InstanceId);
	const TArray<float> ValuesBefore = FoundBefore ? FoundBefore->Values : TArray<float>();

	FVertexMaskForgeFillLayer Layer = MakeRecipeWhiteCopyLayer();
	Layer.MaskStack.Add(A);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	const TArray<FVector3f> Base = { FVector3f::ZeroVector, FVector3f::ZeroVector };
	FVertexMaskForgeRecipeEvaluationOutput Output;
	VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(Recipe, Store, Base, Output);

	TestEqual(TEXT("Num() unchanged"), Store.Num(), NumBefore);
	const FVertexMaskForgeInstanceMaskResult* FoundAfter = Store.Find(A.InstanceId);
	TestNotNull(TEXT("Still found after evaluation"), FoundAfter);
	if (FoundAfter)
	{
		TestEqual(TEXT("Values.Num() unchanged"), FoundAfter->Values.Num(), ValuesBefore.Num());
		for (int32 i = 0; i < ValuesBefore.Num(); ++i)
		{
			TestNearlyEqual(FString::Printf(TEXT("Values[%d] unchanged"), i), FoundAfter->Values[i], ValuesBefore[i], RCP_Tolerance);
		}
	}

	return true;
}

// Q. Two independent result stores, same recipe/InstanceId, no cross-store leakage.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeEvaluationOwnerSeparationTest, "VertexMaskForge.RecipeEvaluation.OwnerSeparation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeEvaluationOwnerSeparationTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore StoreOne;
	FVertexMaskForgeInstanceResultStore StoreTwo;
	const FVertexMaskForgeMaskInstance Shared = MakeRecipeInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	StoreOne.StoreOrReplace(Shared.InstanceId, MakeRecipeResult({ 0.3f }));
	StoreTwo.StoreOrReplace(Shared.InstanceId, MakeRecipeResult({ 0.8f }));

	FVertexMaskForgeFillLayer Layer = MakeRecipeWhiteCopyLayer();
	Layer.MaskStack.Add(Shared);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	const TArray<FVector3f> Base = { FVector3f::ZeroVector };

	FVertexMaskForgeRecipeEvaluationOutput OutputOne, OutputTwo;
	const bool bSuccessOne = VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(Recipe, StoreOne, Base, OutputOne);
	const bool bSuccessTwo = VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(Recipe, StoreTwo, Base, OutputTwo);

	TestTrue(TEXT("StoreOne evaluation succeeds"), bSuccessOne);
	TestTrue(TEXT("StoreTwo evaluation succeeds"), bSuccessTwo);
	if (bSuccessOne && bSuccessTwo)
	{
		ExpectRecipeVector(*this, TEXT("StoreOne result"), OutputOne.Composite[0], FVector3f(0.3f, 0.3f, 0.3f));
		ExpectRecipeVector(*this, TEXT("StoreTwo result"), OutputTwo.Composite[0], FVector3f(0.8f, 0.8f, 0.8f));
	}

	return true;
}

// R. The same InstanceId, referenced by two different Fill Layers, resolves correctly in both --
// non-consuming: the store still holds it, unmodified, afterward.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeEvaluationSharedInstanceAcrossLayersTest, "VertexMaskForge.RecipeEvaluation.SharedInstanceAcrossLayers", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeEvaluationSharedInstanceAcrossLayersTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeRecipeInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	Store.StoreOrReplace(A.InstanceId, MakeRecipeResult({ 0.5f })); // Coverage=0.5 in both layers that use it.

	FVertexMaskForgeFillLayer Layer1 = MakeRecipeWhiteCopyLayer();
	Layer1.MaskStack.Add(A);
	FVertexMaskForgeFillLayer Layer2 = MakeRecipeHalfMultiplyLayer();
	Layer2.MaskStack.Add(A); // SAME InstanceId, second layer's own Blend/Fill/Opacity.

	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer1);
	Recipe.FillLayers.Add(Layer2);

	const TArray<FVector3f> Base = { FVector3f::ZeroVector };

	FVertexMaskForgeRecipeEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(Recipe, Store, Base, Output);

	TestTrue(TEXT("Evaluation succeeds"), bSuccess);
	if (bSuccess)
	{
		// Composite1 = Lerp(0, White, 0.5) = 0.5. Composite2 = Lerp(0.5, 0.5*0.5=0.25, 0.5) = 0.375.
		ExpectRecipeVector(*this, TEXT("Composite[0]"), Output.Composite[0], FVector3f(0.375f, 0.375f, 0.375f));
	}
	TestEqual(TEXT("Store still holds the single entry (not consumed)"), Store.Num(), 1);
	TestTrue(TEXT("A still resolvable"), Store.Contains(A.InstanceId));

	return true;
}

// S. Same generator type and identical parameters, different GUIDs -- resolution stays exclusively by
// InstanceId, no fallback by type/parameters (preserves the M16-F/G contract at the recipe level).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeEvaluationSameParamsDifferentGuidsTest, "VertexMaskForge.RecipeEvaluation.SameParamsDifferentGuids", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeEvaluationSameParamsDifferentGuidsTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeRecipeInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	const FVertexMaskForgeMaskInstance B = MakeRecipeInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	TestNotEqual(TEXT("A and B have different InstanceIds despite identical config"), A.InstanceId, B.InstanceId);
	Store.StoreOrReplace(A.InstanceId, MakeRecipeResult({ 0.2f }));
	Store.StoreOrReplace(B.InstanceId, MakeRecipeResult({ 0.9f }));

	FVertexMaskForgeFillLayer Layer = MakeRecipeWhiteCopyLayer();
	Layer.MaskStack.Add(A);
	Layer.MaskStack.Add(B); // Copy, Copy: fold overwrites -> final == B's value.
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	const TArray<FVector3f> Base = { FVector3f::ZeroVector };

	FVertexMaskForgeRecipeEvaluationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(Recipe, Store, Base, Output);

	TestTrue(TEXT("Evaluation succeeds"), bSuccess);
	if (bSuccess)
	{
		ExpectRecipeVector(*this, TEXT("Composite uses B's value (last Copy wins)"), Output.Composite[0], FVector3f(0.9f, 0.9f, 0.9f));
	}

	return true;
}

// T. A disabled Fill Layer between two enabled ones is skipped entirely -- no store lookup for its
// (deliberately absent) Mask Instance, composite carried forward unchanged.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeEvaluationDisabledLayerTest, "VertexMaskForge.RecipeEvaluation.DisabledLayerSkipped", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeEvaluationDisabledLayerTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance NeverStored = MakeRecipeInstance(EVertexMaskForgeBlendMode::Copy, 1.0f); // Deliberately absent from Store.

	FVertexMaskForgeFillLayer Layer0 = MakeRecipeHalfMultiplyLayer(); // Enabled.
	FVertexMaskForgeFillLayer Layer1 = MakeRecipeWhiteCopyLayer();
	Layer1.MaskStack.Add(NeverStored);
	Layer1.bEnabled = false; // Disabled -- must be skipped without a lookup for NeverStored.
	FVertexMaskForgeFillLayer Layer2 = MakeRecipeSubtractLayer(); // Enabled.

	FVertexMaskForgeRecipe RecipeWithDisabled;
	RecipeWithDisabled.FillLayers.Add(Layer0);
	RecipeWithDisabled.FillLayers.Add(Layer1);
	RecipeWithDisabled.FillLayers.Add(Layer2);

	FVertexMaskForgeRecipe RecipeWithoutDisabled; // Same as above, minus the disabled layer entirely.
	RecipeWithoutDisabled.FillLayers.Add(Layer0);
	RecipeWithoutDisabled.FillLayers.Add(Layer2);

	const TArray<FVector3f> Base = { FVector3f(1.0f, 1.0f, 1.0f) };

	FVertexMaskForgeRecipeEvaluationOutput OutputWithDisabled, OutputWithoutDisabled;
	const bool bSuccessWith = VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(RecipeWithDisabled, Store, Base, OutputWithDisabled);
	const bool bSuccessWithout = VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(RecipeWithoutDisabled, Store, Base, OutputWithoutDisabled);

	TestTrue(TEXT("Recipe with disabled layer succeeds (no lookup attempted for it)"), bSuccessWith);
	TestTrue(TEXT("Recipe without the disabled layer succeeds"), bSuccessWithout);
	if (bSuccessWith && bSuccessWithout)
	{
		ExpectRecipeVector(*this, TEXT("With-disabled == without-disabled"), OutputWithDisabled.Composite[0], OutputWithoutDisabled.Composite[0]);
	}
	TestEqual(TEXT("Store stayed empty (NeverStored was never looked up)"), Store.Num(), 0);

	return true;
}

// U. Manual equivalence: the recipe evaluator's result must equal explicit, sequential, manual calls to
// the M16-G boundary performed directly in this test -- proving M16-H is orchestration only.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeEvaluationManualEquivalenceTest, "VertexMaskForge.RecipeEvaluation.ManualEquivalence", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeEvaluationManualEquivalenceTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store;
	const FVertexMaskForgeMaskInstance A = MakeRecipeInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);
	Store.StoreOrReplace(A.InstanceId, MakeRecipeResult({ 0.5f, 0.5f }));

	FVertexMaskForgeFillLayer Layer1 = MakeRecipeWhiteCopyLayer();
	Layer1.MaskStack.Add(A);
	FVertexMaskForgeFillLayer Layer2 = MakeRecipeHalfMultiplyLayer();

	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer1);
	Recipe.FillLayers.Add(Layer2);

	const TArray<FVector3f> Base = { FVector3f::ZeroVector, FVector3f(1.0f, 1.0f, 1.0f) };

	FVertexMaskForgeRecipeEvaluationOutput RecipeOutput;
	const bool bRecipeSuccess = VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(Recipe, Store, Base, RecipeOutput);
	TestTrue(TEXT("Recipe evaluation succeeds"), bRecipeSuccess);

	// Manual, sequential calls to the M16-G boundary, performed explicitly here.
	FVertexMaskForgeFillLayerEvaluationOutput ManualOutput1;
	const bool bManual1 = VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer1, Store, Base, ManualOutput1);
	TestTrue(TEXT("Manual step 1 succeeds"), bManual1);

	FVertexMaskForgeFillLayerEvaluationOutput ManualOutput2;
	const bool bManual2 = bManual1 && VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults(Layer2, Store, ManualOutput1.Composite, ManualOutput2);
	TestTrue(TEXT("Manual step 2 succeeds"), bManual2);

	if (bRecipeSuccess && bManual2)
	{
		TestEqual(TEXT("Same cardinality"), RecipeOutput.Composite.Num(), ManualOutput2.Composite.Num());
		for (int32 i = 0; i < RecipeOutput.Composite.Num(); ++i)
		{
			ExpectRecipeVector(*this, *FString::Printf(TEXT("Recipe[%d] == Manual[%d]"), i, i), RecipeOutput.Composite[i], ManualOutput2.Composite[i]);
		}
	}

	return true;
}

// V. Failure preserves the caller's prior output exactly, even after at least one valid layer folded.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeEvaluationFailurePreservesOutputTest, "VertexMaskForge.RecipeEvaluation.FailurePreservesOutput", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeEvaluationFailurePreservesOutputTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeInstanceResultStore Store; // Missing is never stored.
	const FVertexMaskForgeMaskInstance Missing = MakeRecipeInstance(EVertexMaskForgeBlendMode::Copy, 1.0f);

	FVertexMaskForgeFillLayer Layer0 = MakeRecipeHalfMultiplyLayer(); // Valid.
	FVertexMaskForgeFillLayer Layer1 = MakeRecipeWhiteCopyLayer();
	Layer1.MaskStack.Add(Missing); // Invalid.

	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer0);
	Recipe.FillLayers.Add(Layer1);

	const TArray<FVector3f> Base = { FVector3f(1.0f, 1.0f, 1.0f), FVector3f(0.5f, 0.5f, 0.5f) };

	FVertexMaskForgeRecipeEvaluationOutput Output;
	Output.Composite = { FVector3f(0.11f, 0.22f, 0.33f), FVector3f(0.44f, 0.55f, 0.66f) }; // Sentinel.

	const bool bSuccess = VertexMaskForgeRecipeEvaluation::EvaluateRecipeFillLayersFromKeyedResults(Recipe, Store, Base, Output);

	TestFalse(TEXT("Evaluation fails"), bSuccess);
	TestEqual(TEXT("Sentinel Num preserved"), Output.Composite.Num(), 2);
	if (Output.Composite.Num() == 2)
	{
		ExpectRecipeVector(*this, TEXT("Sentinel[0] preserved"), Output.Composite[0], FVector3f(0.11f, 0.22f, 0.33f));
		ExpectRecipeVector(*this, TEXT("Sentinel[1] preserved"), Output.Composite[1], FVector3f(0.44f, 0.55f, 0.66f));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
