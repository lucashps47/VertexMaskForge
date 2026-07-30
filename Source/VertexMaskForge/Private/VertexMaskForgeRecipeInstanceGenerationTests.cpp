// M16-I: automation tests for
// VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe -- coordinates
// generating every keyed instance result an ENABLED recipe requires, delegating all actual generation to
// the M16-E boundary (VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult).
//
// Structural proofs (no dedicated test, verified instead by source inspection / diff / call graph, per
// this checkpoint's own contract): VertexMaskForgeRecipeInstanceGeneration.cpp includes no generator
// header other than VertexMaskForgeMaterialSlotGenerator.h, calls no VertexMaskForgeFillLayerResolution
// or VertexMaskForgeRecipeEvaluation function, never references "Composite" or "EffectiveMask", never
// calls VertexMaskForgeSequentialEvaluator, and never touches VertexMaskForgeMaskStackComposer.
//
// M "Resultado inválido/stale/failed existente" from the checkpoint's own test list is deliberately NOT
// implemented: FVertexMaskForgeInstanceMaskResult (M16-C's real type) has no State/valid/stale/failed
// field at all -- only Values/bHasValue -- so that state is not representable by the real type, exactly
// the "if the model structurally prevents this case, document and do not force the test" escape clause
// the checkpoint's own instructions provide.
//
// AC "Ordem de descoberta" is not covered by a dedicated runtime-observable test either: the boundary's
// output is aggregate counts only (NumRequired/NumReused/NumGenerated), and adding permanent
// instrumentation just to observe discovery order was explicitly disallowed. First-occurrence-order is
// instead proven structurally: RequiredOrder.Add(InstanceId) executes exactly once per GUID, at the
// point TMap::Find first fails for it, and TArray preserves insertion order by construction.

#if WITH_DEV_AUTOMATION_TESTS

#include "DynamicMesh/DynamicMesh3.h"
#include "Misc/AutomationTest.h"
#include "VertexMaskForgeMaterialSlotGenerator.h"
#include "VertexMaskForgeRecipeInstanceGeneration.h"
#include "VertexMaskForgeRecipeTypes.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

namespace
{
	constexpr float RIG_Tolerance = 1e-4f;

	/** Minimal two-triangle Source-Topology fixture -- Triangle 0 = corners {0,1,2}, Triangle 1 = corners
	 *  {0,2,3}. Independently duplicated test-only construction (not shared production logic), same
	 *  shape as VertexMaskForgeMaterialSlotGeneratorTests.cpp's own BuildFixtureWorkingMesh. */
	FVertexMaskForgeWorkingMesh BuildRIGFixtureWorkingMesh(const int32 SlotForTri0, const int32 SlotForTri1)
	{
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
		WorkingMesh.DynamicTriangleToMaterialSlot[0] = SlotForTri0;
		WorkingMesh.DynamicTriangleToMaterialSlot[1] = SlotForTri1;

		return WorkingMesh;
	}

	FVertexMaskForgeMaskInstance MakeRIGMaterialSlotInstance(const int32 SelectedSlotIndex, const bool bInvert = false)
	{
		FVertexMaskForgeMaskInstance Instance = FVertexMaskForgeMaskInstance::Make(EVertexMaskForgeGeneratorType::MaterialSlot);
		FVertexMaskForgeMaterialSlotParams& Params = Instance.Params.Get<FVertexMaskForgeMaterialSlotParams>();
		Params.SelectedSlotIndex = SelectedSlotIndex;
		Params.bInvert = bInvert;
		return Instance;
	}

	FVertexMaskForgeMaskInstance MakeUnsupportedInstance()
	{
		return FVertexMaskForgeMaskInstance::Make(EVertexMaskForgeGeneratorType::BoundingBox);
	}

	FVertexMaskForgeInstanceMaskResult MakeRIGResult(const TArray<float>& Values)
	{
		FVertexMaskForgeInstanceMaskResult Result;
		Result.Values = Values;
		Result.bHasValue.Init(true, Values.Num());
		return Result;
	}

	FVertexMaskForgeFillLayer MakeRIGLayer(const bool bEnabled = true)
	{
		FVertexMaskForgeFillLayer Layer = FVertexMaskForgeFillLayer::Make();
		Layer.bEnabled = bEnabled;
		return Layer;
	}
}

// A. Empty recipe: success, zero generation, store untouched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationEmptyRecipeTest, "VertexMaskForge.RecipeInstanceGeneration.EmptyRecipe", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationEmptyRecipeTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	FVertexMaskForgeRecipe Recipe; // FillLayers left empty.

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, /*bUseSourceTopology=*/true, /*LOD0=*/nullptr, Output);

	TestTrue(TEXT("Succeeds"), bSuccess);
	TestEqual(TEXT("NumRequired"), Output.NumRequired, 0);
	TestEqual(TEXT("NumReused"), Output.NumReused, 0);
	TestEqual(TEXT("NumGenerated"), Output.NumGenerated, 0);
	TestEqual(TEXT("Store untouched"), WorkingMesh.InstanceResults.Num(), 0);

	return true;
}

// B. Only disabled layers, with deliberately invalid/unsupported content inside them: success, zero lookup.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationOnlyDisabledTest, "VertexMaskForge.RecipeInstanceGeneration.OnlyDisabledLayers", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationOnlyDisabledTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);

	FVertexMaskForgeFillLayer Layer0 = MakeRIGLayer(/*bEnabled=*/false);
	FVertexMaskForgeMaskInstance InvalidGuidInstance = MakeRIGMaterialSlotInstance(0);
	InvalidGuidInstance.InstanceId = FGuid();
	Layer0.MaskStack.Add(InvalidGuidInstance);

	FVertexMaskForgeFillLayer Layer1 = MakeRIGLayer(/*bEnabled=*/false);
	Layer1.MaskStack.Add(MakeUnsupportedInstance());

	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer0);
	Recipe.FillLayers.Add(Layer1);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestTrue(TEXT("Succeeds despite invalid content, since both layers are disabled"), bSuccess);
	TestEqual(TEXT("NumRequired"), Output.NumRequired, 0);
	TestEqual(TEXT("Store untouched"), WorkingMesh.InstanceResults.Num(), 0);

	return true;
}

// C. Enabled layer with an empty Mask Stack: success, zero generation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationEmptyStackTest, "VertexMaskForge.RecipeInstanceGeneration.EnabledLayerEmptyStack", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationEmptyStackTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(MakeRIGLayer(true)); // Empty MaskStack.

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestTrue(TEXT("Succeeds"), bSuccess);
	TestEqual(TEXT("NumRequired"), Output.NumRequired, 0);
	TestEqual(TEXT("Store untouched"), WorkingMesh.InstanceResults.Num(), 0);

	return true;
}

// D. One Material Slot Instance: generated, correct payload.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationOneInstanceTest, "VertexMaskForge.RecipeInstanceGeneration.OneMaterialSlotInstance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationOneInstanceTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(/*SlotForTri0=*/0, /*SlotForTri1=*/1);
	const FVertexMaskForgeMaskInstance A = MakeRIGMaterialSlotInstance(/*SelectedSlotIndex=*/0);

	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(A);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestTrue(TEXT("Succeeds"), bSuccess);
	TestEqual(TEXT("NumRequired"), Output.NumRequired, 1);
	TestEqual(TEXT("NumReused"), Output.NumReused, 0);
	TestEqual(TEXT("NumGenerated"), Output.NumGenerated, 1);

	const FVertexMaskForgeInstanceMaskResult* Found = WorkingMesh.InstanceResults.Find(A.InstanceId);
	TestNotNull(TEXT("Found"), Found);
	if (Found)
	{
		// Triangle 0 (slot 0, selected) -> corners 0-2 = 1.0; Triangle 1 (slot 1) -> corners 3-5 = 0.0.
		const TArray<float> Expected = { 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f };
		TestEqual(TEXT("Values.Num()"), Found->Values.Num(), Expected.Num());
		for (int32 i = 0; i < Expected.Num(); ++i)
		{
			TestNearlyEqual(FString::Printf(TEXT("Values[%d]"), i), Found->Values[i], Expected[i], RIG_Tolerance);
		}
	}

	return true;
}

// E. Two Material Slot Instances, different GUIDs and slot selections: both generated independently.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationTwoInstancesTest, "VertexMaskForge.RecipeInstanceGeneration.TwoMaterialSlotInstances", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationTwoInstancesTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	const FVertexMaskForgeMaskInstance A = MakeRIGMaterialSlotInstance(0);
	const FVertexMaskForgeMaskInstance B = MakeRIGMaterialSlotInstance(1);

	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(A);
	Layer.MaskStack.Add(B);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestTrue(TEXT("Succeeds"), bSuccess);
	TestEqual(TEXT("NumRequired"), Output.NumRequired, 2);
	TestEqual(TEXT("NumGenerated"), Output.NumGenerated, 2);
	TestEqual(TEXT("Store has both entries"), WorkingMesh.InstanceResults.Num(), 2);
	TestTrue(TEXT("A present"), WorkingMesh.InstanceResults.Contains(A.InstanceId));
	TestTrue(TEXT("B present"), WorkingMesh.InstanceResults.Contains(B.InstanceId));

	return true;
}

// F. Same parameters, different GUIDs: no deduplication by content.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationSameParamsTest, "VertexMaskForge.RecipeInstanceGeneration.SameParamsDifferentGuids", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationSameParamsTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	const FVertexMaskForgeMaskInstance A = MakeRIGMaterialSlotInstance(0);
	const FVertexMaskForgeMaskInstance B = MakeRIGMaterialSlotInstance(0); // Same slot, different GUID.
	TestNotEqual(TEXT("Different InstanceIds"), A.InstanceId, B.InstanceId);

	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(A);
	Layer.MaskStack.Add(B);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestTrue(TEXT("Succeeds"), bSuccess);
	TestEqual(TEXT("NumRequired (no content-based dedup)"), Output.NumRequired, 2);
	TestEqual(TEXT("NumGenerated"), Output.NumGenerated, 2);
	TestEqual(TEXT("Two distinct entries"), WorkingMesh.InstanceResults.Num(), 2);

	return true;
}

// G. Same GUID referenced twice within the same layer: generated at most once, one entry.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationRepeatSameLayerTest, "VertexMaskForge.RecipeInstanceGeneration.RepeatGuidSameLayer", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationRepeatSameLayerTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	const FVertexMaskForgeMaskInstance A = MakeRIGMaterialSlotInstance(0);

	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(A);
	Layer.MaskStack.Add(A); // Same InstanceId, referenced twice.
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestTrue(TEXT("Succeeds"), bSuccess);
	TestEqual(TEXT("NumRequired (deduplicated)"), Output.NumRequired, 1);
	TestEqual(TEXT("NumGenerated (generated once)"), Output.NumGenerated, 1);
	TestEqual(TEXT("One entry"), WorkingMesh.InstanceResults.Num(), 1);

	return true;
}

// H. Same GUID referenced across two enabled layers: generated at most once, one entry.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationRepeatAcrossLayersTest, "VertexMaskForge.RecipeInstanceGeneration.RepeatGuidAcrossLayers", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationRepeatAcrossLayersTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	const FVertexMaskForgeMaskInstance A = MakeRIGMaterialSlotInstance(0);

	FVertexMaskForgeFillLayer Layer0 = MakeRIGLayer(true);
	Layer0.MaskStack.Add(A);
	FVertexMaskForgeFillLayer Layer1 = MakeRIGLayer(true);
	Layer1.MaskStack.Add(A);

	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer0);
	Recipe.FillLayers.Add(Layer1);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestTrue(TEXT("Succeeds"), bSuccess);
	TestEqual(TEXT("NumRequired (deduplicated across layers)"), Output.NumRequired, 1);
	TestEqual(TEXT("NumGenerated"), Output.NumGenerated, 1);
	TestEqual(TEXT("One entry"), WorkingMesh.InstanceResults.Num(), 1);

	return true;
}

// I. Same GUID with divergent authoring (different SelectedSlotIndex): deterministic failure, zero publication.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationDivergentCollisionTest, "VertexMaskForge.RecipeInstanceGeneration.DivergentGuidCollision", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationDivergentCollisionTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	FVertexMaskForgeMaskInstance A = MakeRIGMaterialSlotInstance(0);
	FVertexMaskForgeMaskInstance ADivergent = A; // Same InstanceId (copy), different slot selection.
	ADivergent.Params.Get<FVertexMaskForgeMaterialSlotParams>().SelectedSlotIndex = 1;

	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(A);
	Layer.MaskStack.Add(ADivergent);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestFalse(TEXT("Fails on divergent collision"), bSuccess);
	TestEqual(TEXT("Store untouched"), WorkingMesh.InstanceResults.Num(), 0);

	return true;
}

// J. Already-valid existing result: reused, not regenerated (sentinel payload proves this).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationReuseExistingTest, "VertexMaskForge.RecipeInstanceGeneration.ReuseExistingResult", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationReuseExistingTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	const FVertexMaskForgeMaskInstance A = MakeRIGMaterialSlotInstance(0);

	// Sentinel payload, deliberately NOT what real generation for slot 0 would produce.
	WorkingMesh.InstanceResults.StoreOrReplace(A.InstanceId, MakeRIGResult({ 0.42f, 0.42f, 0.42f, 0.42f, 0.42f, 0.42f }));

	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(A);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestTrue(TEXT("Succeeds"), bSuccess);
	TestEqual(TEXT("NumRequired"), Output.NumRequired, 1);
	TestEqual(TEXT("NumReused"), Output.NumReused, 1);
	TestEqual(TEXT("NumGenerated (not regenerated)"), Output.NumGenerated, 0);

	const FVertexMaskForgeInstanceMaskResult* Found = WorkingMesh.InstanceResults.Find(A.InstanceId);
	TestNotNull(TEXT("Found"), Found);
	if (Found && Found->Values.Num() == 6)
	{
		TestNearlyEqual(TEXT("Sentinel payload preserved (not overwritten)"), Found->Values[0], 0.42f, RIG_Tolerance);
	}

	return true;
}

// K. Unrelated existing entry preserved exactly while a new one is generated.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationUnrelatedPreservedTest, "VertexMaskForge.RecipeInstanceGeneration.UnrelatedResultPreserved", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationUnrelatedPreservedTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	const FGuid UnrelatedId = FGuid::NewGuid();
	WorkingMesh.InstanceResults.StoreOrReplace(UnrelatedId, MakeRIGResult({ 0.99f }));

	const FVertexMaskForgeMaskInstance A = MakeRIGMaterialSlotInstance(0);
	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(A);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestTrue(TEXT("Succeeds"), bSuccess);
	TestEqual(TEXT("Store has unrelated + new entry"), WorkingMesh.InstanceResults.Num(), 2);
	const FVertexMaskForgeInstanceMaskResult* FoundUnrelated = WorkingMesh.InstanceResults.Find(UnrelatedId);
	TestNotNull(TEXT("Unrelated entry still present"), FoundUnrelated);
	if (FoundUnrelated && FoundUnrelated->Values.Num() == 1)
	{
		TestNearlyEqual(TEXT("Unrelated payload unchanged"), FoundUnrelated->Values[0], 0.99f, RIG_Tolerance);
	}
	TestTrue(TEXT("New entry present"), WorkingMesh.InstanceResults.Contains(A.InstanceId));

	return true;
}

// L. Multiple pre-existing unrelated entries: none touched by publication.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationMultipleUnrelatedTest, "VertexMaskForge.RecipeInstanceGeneration.MultipleUnrelatedResultsPreserved", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationMultipleUnrelatedTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	TArray<FGuid> UnrelatedIds;
	for (int32 i = 0; i < 3; ++i)
	{
		const FGuid Id = FGuid::NewGuid();
		UnrelatedIds.Add(Id);
		WorkingMesh.InstanceResults.StoreOrReplace(Id, MakeRIGResult({ static_cast<float>(i) * 0.1f }));
	}

	const FVertexMaskForgeMaskInstance A = MakeRIGMaterialSlotInstance(0);
	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(A);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestTrue(TEXT("Succeeds"), bSuccess);
	TestEqual(TEXT("3 unrelated + 1 new"), WorkingMesh.InstanceResults.Num(), 4);
	for (int32 i = 0; i < 3; ++i)
	{
		const FVertexMaskForgeInstanceMaskResult* Found = WorkingMesh.InstanceResults.Find(UnrelatedIds[i]);
		TestNotNull(*FString::Printf(TEXT("Unrelated[%d] present"), i), Found);
		if (Found && Found->Values.Num() == 1)
		{
			TestNearlyEqual(*FString::Printf(TEXT("Unrelated[%d] unchanged"), i), Found->Values[0], static_cast<float>(i) * 0.1f, RIG_Tolerance);
		}
	}

	return true;
}

// N. Unsupported generator, no existing result: deterministic failure, no fallback.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationUnsupportedTest, "VertexMaskForge.RecipeInstanceGeneration.UnsupportedGenerator", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationUnsupportedTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(MakeUnsupportedInstance());
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestFalse(TEXT("Fails for unsupported generator"), bSuccess);
	TestEqual(TEXT("Store untouched"), WorkingMesh.InstanceResults.Num(), 0);

	return true;
}

// O. Unsupported generator appearing AFTER a valid Material Slot requirement: classification happens
// fully before generation, so zero Material Slot generation occurs either way.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationUnsupportedAfterValidTest, "VertexMaskForge.RecipeInstanceGeneration.UnsupportedAfterValidRequirement", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationUnsupportedAfterValidTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	const FVertexMaskForgeMaskInstance A = MakeRIGMaterialSlotInstance(0); // Would succeed alone.

	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(A);
	Layer.MaskStack.Add(MakeUnsupportedInstance()); // Appears after A.
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestFalse(TEXT("Fails"), bSuccess);
	TestEqual(TEXT("Store untouched -- A was never generated"), WorkingMesh.InstanceResults.Num(), 0);
	TestFalse(TEXT("A specifically absent"), WorkingMesh.InstanceResults.Contains(A.InstanceId));

	return true;
}

// P. First (only) generation fails: store rolled back to its prior (empty) state.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationFirstGenerationFailsTest, "VertexMaskForge.RecipeInstanceGeneration.FirstGenerationFails", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationFirstGenerationFailsTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	WorkingMesh.bMaterialSlotResolutionValid = false; // Forces GenerateMaterialSlotMaskInstanceResult to fail.

	const FVertexMaskForgeMaskInstance A = MakeRIGMaterialSlotInstance(0);
	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(A);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestFalse(TEXT("Fails"), bSuccess);
	TestEqual(TEXT("Store rolled back to empty"), WorkingMesh.InstanceResults.Num(), 0);

	return true;
}

// Q. Intermediate generation fails: nothing from before or after is published; store rolled back.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationMiddleGenerationFailsTest, "VertexMaskForge.RecipeInstanceGeneration.MiddleGenerationFails", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationMiddleGenerationFailsTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1); // MaterialSlotOptions.Num()==2 -> valid indices {0,1}.
	const FVertexMaskForgeMaskInstance A = MakeRIGMaterialSlotInstance(0); // Valid.
	const FVertexMaskForgeMaskInstance B = MakeRIGMaterialSlotInstance(5); // Out-of-range slot -> fails.
	const FVertexMaskForgeMaskInstance C = MakeRIGMaterialSlotInstance(1); // Would be valid if reached.

	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(A);
	Layer.MaskStack.Add(B);
	Layer.MaskStack.Add(C);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestFalse(TEXT("Fails"), bSuccess);
	TestEqual(TEXT("Store rolled back to empty (A's staged success discarded)"), WorkingMesh.InstanceResults.Num(), 0);

	return true;
}

// R. Failure on the LAST generation: earlier successful (staged) generations are still rolled back.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationLastGenerationFailsTest, "VertexMaskForge.RecipeInstanceGeneration.LastGenerationFails", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationLastGenerationFailsTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	const FVertexMaskForgeMaskInstance A = MakeRIGMaterialSlotInstance(0);
	const FVertexMaskForgeMaskInstance B = MakeRIGMaterialSlotInstance(1);
	const FVertexMaskForgeMaskInstance C = MakeRIGMaterialSlotInstance(5); // Out-of-range -> fails last.

	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(A);
	Layer.MaskStack.Add(B);
	Layer.MaskStack.Add(C);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestFalse(TEXT("Fails"), bSuccess);
	TestEqual(TEXT("Store rolled back to empty"), WorkingMesh.InstanceResults.Num(), 0);

	return true;
}

// S. Invalid GUID in an ENABLED layer: deterministic failure.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationInvalidGuidEnabledTest, "VertexMaskForge.RecipeInstanceGeneration.InvalidGuidInEnabledLayer", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationInvalidGuidEnabledTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	FVertexMaskForgeMaskInstance Invalid = MakeRIGMaterialSlotInstance(0);
	Invalid.InstanceId = FGuid();

	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(Invalid);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestFalse(TEXT("Fails"), bSuccess);
	TestEqual(TEXT("Store untouched"), WorkingMesh.InstanceResults.Num(), 0);

	return true;
}

// T. Invalid GUID in a DISABLED layer: ignored entirely, success.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationInvalidGuidDisabledTest, "VertexMaskForge.RecipeInstanceGeneration.InvalidGuidInDisabledLayer", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationInvalidGuidDisabledTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	FVertexMaskForgeMaskInstance Invalid = MakeRIGMaterialSlotInstance(0);
	Invalid.InstanceId = FGuid();

	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(/*bEnabled=*/false);
	Layer.MaskStack.Add(Invalid);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestTrue(TEXT("Succeeds -- disabled layer's invalid GUID is never inspected"), bSuccess);
	TestEqual(TEXT("NumRequired"), Output.NumRequired, 0);
	TestEqual(TEXT("Store untouched"), WorkingMesh.InstanceResults.Num(), 0);

	return true;
}

// U. Recipe (authoring state) is read-only, in both success and failure cases.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationRecipeReadOnlyTest, "VertexMaskForge.RecipeInstanceGeneration.RecipeReadOnly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationRecipeReadOnlyTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	const FVertexMaskForgeMaskInstance A = MakeRIGMaterialSlotInstance(0);
	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(A);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	const FGuid LayerIdBefore = Recipe.FillLayers[0].LayerId;
	const bool bEnabledBefore = Recipe.FillLayers[0].bEnabled;
	const int32 MaskStackNumBefore = Recipe.FillLayers[0].MaskStack.Num();
	const FGuid InstanceIdBefore = Recipe.FillLayers[0].MaskStack[0].InstanceId;
	const EVertexMaskForgeGeneratorType TypeBefore = Recipe.FillLayers[0].MaskStack[0].GeneratorType;

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(Recipe, WorkingMesh, true, nullptr, Output);

	TestEqual(TEXT("LayerId unchanged"), Recipe.FillLayers[0].LayerId, LayerIdBefore);
	TestEqual(TEXT("bEnabled unchanged"), Recipe.FillLayers[0].bEnabled, bEnabledBefore);
	TestEqual(TEXT("MaskStack.Num() unchanged"), Recipe.FillLayers[0].MaskStack.Num(), MaskStackNumBefore);
	TestEqual(TEXT("InstanceId unchanged"), Recipe.FillLayers[0].MaskStack[0].InstanceId, InstanceIdBefore);
	TestTrue(TEXT("GeneratorType unchanged"), Recipe.FillLayers[0].MaskStack[0].GeneratorType == TypeBefore);

	return true;
}

// V. Domain (geometry/lookup fields on WorkingMesh) is read-only -- only InstanceResults may change.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationDomainReadOnlyTest, "VertexMaskForge.RecipeInstanceGeneration.DomainReadOnly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationDomainReadOnlyTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	const TArray<int32> LookupBefore = WorkingMesh.DynamicTriangleToMaterialSlot;
	const int32 OptionsNumBefore = WorkingMesh.MaterialSlotOptions.Num();
	const bool bValidBefore = WorkingMesh.bMaterialSlotResolutionValid;

	const FVertexMaskForgeMaskInstance A = MakeRIGMaterialSlotInstance(0);
	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(A);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(Recipe, WorkingMesh, true, nullptr, Output);

	TestEqual(TEXT("DynamicTriangleToMaterialSlot.Num() unchanged"), WorkingMesh.DynamicTriangleToMaterialSlot.Num(), LookupBefore.Num());
	for (int32 i = 0; i < LookupBefore.Num(); ++i)
	{
		TestEqual(*FString::Printf(TEXT("DynamicTriangleToMaterialSlot[%d] unchanged"), i), WorkingMesh.DynamicTriangleToMaterialSlot[i], LookupBefore[i]);
	}
	TestEqual(TEXT("MaterialSlotOptions.Num() unchanged"), WorkingMesh.MaterialSlotOptions.Num(), OptionsNumBefore);
	TestEqual(TEXT("bMaterialSlotResolutionValid unchanged"), WorkingMesh.bMaterialSlotResolutionValid, bValidBefore);

	return true;
}

// W. Store is atomic on success: prior unrelated entries preserved, exactly the expected new entries added.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationAtomicSuccessTest, "VertexMaskForge.RecipeInstanceGeneration.StoreAtomicOnSuccess", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationAtomicSuccessTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	const FGuid PriorId = FGuid::NewGuid();
	WorkingMesh.InstanceResults.StoreOrReplace(PriorId, MakeRIGResult({ 0.5f }));

	const FVertexMaskForgeMaskInstance A = MakeRIGMaterialSlotInstance(0);
	const FVertexMaskForgeMaskInstance B = MakeRIGMaterialSlotInstance(1);
	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(A);
	Layer.MaskStack.Add(B);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestTrue(TEXT("Succeeds"), bSuccess);
	TestEqual(TEXT("Exactly prior + A + B"), WorkingMesh.InstanceResults.Num(), 3);
	TestTrue(TEXT("Prior still present"), WorkingMesh.InstanceResults.Contains(PriorId));
	TestTrue(TEXT("A present"), WorkingMesh.InstanceResults.Contains(A.InstanceId));
	TestTrue(TEXT("B present"), WorkingMesh.InstanceResults.Contains(B.InstanceId));

	return true;
}

// X. Store is atomic on failure: byte-for-byte equivalent to the initial sentinel state.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationAtomicFailureTest, "VertexMaskForge.RecipeInstanceGeneration.StoreAtomicOnFailure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationAtomicFailureTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	const FGuid SentinelId = FGuid::NewGuid();
	WorkingMesh.InstanceResults.StoreOrReplace(SentinelId, MakeRIGResult({ 0.5f, 0.6f }));

	const FVertexMaskForgeMaskInstance A = MakeRIGMaterialSlotInstance(0); // Would succeed alone (staged first).
	const FVertexMaskForgeMaskInstance Bad = MakeRIGMaterialSlotInstance(5); // Out-of-range -> fails.
	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(A);
	Layer.MaskStack.Add(Bad);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestFalse(TEXT("Fails"), bSuccess);
	TestEqual(TEXT("Num() back to exactly the sentinel state"), WorkingMesh.InstanceResults.Num(), 1);
	const FVertexMaskForgeInstanceMaskResult* Found = WorkingMesh.InstanceResults.Find(SentinelId);
	TestNotNull(TEXT("Sentinel present"), Found);
	if (Found && Found->Values.Num() == 2)
	{
		TestNearlyEqual(TEXT("Sentinel[0]"), Found->Values[0], 0.5f, RIG_Tolerance);
		TestNearlyEqual(TEXT("Sentinel[1]"), Found->Values[1], 0.6f, RIG_Tolerance);
	}
	TestFalse(TEXT("A was rolled back"), WorkingMesh.InstanceResults.Contains(A.InstanceId));

	return true;
}

// Y. Independent owners: two separate WorkingMesh fixtures, same recipe -- no cross leakage.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationOwnerSeparationTest, "VertexMaskForge.RecipeInstanceGeneration.OwnerSeparation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationOwnerSeparationTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMeshOne = BuildRIGFixtureWorkingMesh(0, 1);
	FVertexMaskForgeWorkingMesh WorkingMeshTwo = BuildRIGFixtureWorkingMesh(0, 1);

	const FVertexMaskForgeMaskInstance A = MakeRIGMaterialSlotInstance(0);
	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(A);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput OutputOne, OutputTwo;
	const bool bSuccessOne = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(Recipe, WorkingMeshOne, true, nullptr, OutputOne);
	const bool bSuccessTwo = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(Recipe, WorkingMeshTwo, true, nullptr, OutputTwo);

	TestTrue(TEXT("One succeeds"), bSuccessOne);
	TestTrue(TEXT("Two succeeds"), bSuccessTwo);
	TestEqual(TEXT("One has exactly its own entry"), WorkingMeshOne.InstanceResults.Num(), 1);
	TestEqual(TEXT("Two has exactly its own entry"), WorkingMeshTwo.InstanceResults.Num(), 1);

	return true;
}

// Z. Equivalence with a direct M16-E call: same payload for the same InstanceId/params/domain.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationEquivalenceWithM16ETest, "VertexMaskForge.RecipeInstanceGeneration.EquivalenceWithDirectM16ECall", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationEquivalenceWithM16ETest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMeshViaRecipe = BuildRIGFixtureWorkingMesh(0, 1);
	FVertexMaskForgeWorkingMesh WorkingMeshDirect = BuildRIGFixtureWorkingMesh(0, 1);
	const FVertexMaskForgeMaskInstance A = MakeRIGMaterialSlotInstance(0);

	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(A);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bRecipeSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMeshViaRecipe, true, nullptr, Output);
	const bool bDirectSuccess = VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult(
		A, WorkingMeshDirect, true, nullptr);

	TestTrue(TEXT("Via recipe succeeds"), bRecipeSuccess);
	TestTrue(TEXT("Direct M16-E call succeeds"), bDirectSuccess);

	const FVertexMaskForgeInstanceMaskResult* ViaRecipe = WorkingMeshViaRecipe.InstanceResults.Find(A.InstanceId);
	const FVertexMaskForgeInstanceMaskResult* Direct = WorkingMeshDirect.InstanceResults.Find(A.InstanceId);
	TestNotNull(TEXT("Via-recipe result found"), ViaRecipe);
	TestNotNull(TEXT("Direct result found"), Direct);
	if (ViaRecipe && Direct)
	{
		TestEqual(TEXT("Same cardinality"), ViaRecipe->Values.Num(), Direct->Values.Num());
		for (int32 i = 0; i < ViaRecipe->Values.Num(); ++i)
		{
			TestNearlyEqual(*FString::Printf(TEXT("Values[%d] equivalent"), i), ViaRecipe->Values[i], Direct->Values[i], RIG_Tolerance);
		}
	}

	return true;
}

// --- M16-I.1: disabled Mask Instance semantics -----------------------------------------------------
// A disabled Mask Instance is semantically ABSENT: skipped before InstanceId validation, before
// GeneratorType/Params are read, before store lookup, before deduplication/collision detection, and
// before classification. It never establishes or participates in a requirement.

// A. Disabled instance with an invalid InstanceId is skipped -- no failure, zero generation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationDisabledInvalidGuidTest, "VertexMaskForge.RecipeInstanceGeneration.DisabledInvalidGuidSkipped", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationDisabledInvalidGuidTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	FVertexMaskForgeMaskInstance Disabled = MakeRIGMaterialSlotInstance(0);
	Disabled.InstanceId = FGuid();
	Disabled.bEnabled = false;

	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(Disabled);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestTrue(TEXT("Succeeds -- disabled instance's invalid GUID never inspected"), bSuccess);
	TestEqual(TEXT("NumRequired"), Output.NumRequired, 0);
	TestEqual(TEXT("NumGenerated"), Output.NumGenerated, 0);
	TestEqual(TEXT("Store untouched"), WorkingMesh.InstanceResults.Num(), 0);

	return true;
}

// B. Disabled instance of an unsupported generator is skipped -- no failure, no fallback.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationDisabledUnsupportedTest, "VertexMaskForge.RecipeInstanceGeneration.DisabledUnsupportedGeneratorSkipped", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationDisabledUnsupportedTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	FVertexMaskForgeMaskInstance Disabled = MakeUnsupportedInstance();
	Disabled.bEnabled = false;

	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(Disabled);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestTrue(TEXT("Succeeds -- disabled unsupported-generator instance never inspected"), bSuccess);
	TestEqual(TEXT("NumRequired"), Output.NumRequired, 0);
	TestEqual(TEXT("Store untouched"), WorkingMesh.InstanceResults.Num(), 0);

	return true;
}

// C. Disabled Material Slot instance is skipped even though the domain would allow generation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationDisabledMaterialSlotTest, "VertexMaskForge.RecipeInstanceGeneration.DisabledMaterialSlotSkipped", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationDisabledMaterialSlotTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1); // Would generate successfully if enabled.
	FVertexMaskForgeMaskInstance Disabled = MakeRIGMaterialSlotInstance(0);
	Disabled.bEnabled = false;

	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(Disabled);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestTrue(TEXT("Succeeds"), bSuccess);
	TestEqual(TEXT("NumRequired == 0 -- no generation was required"), Output.NumRequired, 0);
	TestEqual(TEXT("NumGenerated == 0"), Output.NumGenerated, 0);
	TestFalse(TEXT("No entry created for the disabled instance's GUID"), WorkingMesh.InstanceResults.Contains(Disabled.InstanceId));
	TestEqual(TEXT("Store untouched"), WorkingMesh.InstanceResults.Num(), 0);

	return true;
}

// D. Mixed enabled/disabled stack: only the enabled Material Slot instance is generated.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationMixedEnabledDisabledTest, "VertexMaskForge.RecipeInstanceGeneration.MixedEnabledDisabledStack", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationMixedEnabledDisabledTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	const FVertexMaskForgeMaskInstance Enabled = MakeRIGMaterialSlotInstance(0);
	FVertexMaskForgeMaskInstance DisabledUnsupported = MakeUnsupportedInstance();
	DisabledUnsupported.bEnabled = false;

	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(DisabledUnsupported); // Would fail the whole call if it were enabled.
	Layer.MaskStack.Add(Enabled);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestTrue(TEXT("Succeeds"), bSuccess);
	TestEqual(TEXT("NumRequired == 1 (only the enabled instance)"), Output.NumRequired, 1);
	TestEqual(TEXT("NumGenerated == 1"), Output.NumGenerated, 1);
	TestEqual(TEXT("Store has exactly the enabled instance's entry"), WorkingMesh.InstanceResults.Num(), 1);
	TestTrue(TEXT("Enabled entry present"), WorkingMesh.InstanceResults.Contains(Enabled.InstanceId));

	return true;
}

// E1. Same GUID: disabled occurrence BEFORE the enabled one, with divergent (irrelevant) params.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationSameGuidDisabledThenEnabledTest, "VertexMaskForge.RecipeInstanceGeneration.SameGuidDisabledThenEnabled", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationSameGuidDisabledThenEnabledTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	const FVertexMaskForgeMaskInstance Enabled = MakeRIGMaterialSlotInstance(0);
	FVertexMaskForgeMaskInstance Disabled = Enabled; // Same InstanceId.
	Disabled.Params.Get<FVertexMaskForgeMaterialSlotParams>().SelectedSlotIndex = 1; // Divergent -- must not matter.
	Disabled.bEnabled = false;

	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(Disabled); // Before.
	Layer.MaskStack.Add(Enabled);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestTrue(TEXT("Succeeds -- no collision, disabled occurrence never inspected"), bSuccess);
	TestEqual(TEXT("NumRequired == 1"), Output.NumRequired, 1);
	TestEqual(TEXT("NumGenerated == 1"), Output.NumGenerated, 1);
	TestEqual(TEXT("One entry"), WorkingMesh.InstanceResults.Num(), 1);

	const FVertexMaskForgeInstanceMaskResult* Found = WorkingMesh.InstanceResults.Find(Enabled.InstanceId);
	TestNotNull(TEXT("Found"), Found);
	if (Found && Found->Values.Num() == 6)
	{
		// Slot 0 (the ENABLED occurrence's own param) selected -> corners 0-2 = 1.0, 3-5 = 0.0.
		TestNearlyEqual(TEXT("Values[0] reflects the enabled occurrence's own slot 0"), Found->Values[0], 1.0f, RIG_Tolerance);
	}

	return true;
}

// E2. Same GUID: enabled occurrence BEFORE the disabled one, with divergent (irrelevant) params.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationSameGuidEnabledThenDisabledTest, "VertexMaskForge.RecipeInstanceGeneration.SameGuidEnabledThenDisabled", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationSameGuidEnabledThenDisabledTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	const FVertexMaskForgeMaskInstance Enabled = MakeRIGMaterialSlotInstance(0);
	FVertexMaskForgeMaskInstance Disabled = Enabled; // Same InstanceId.
	Disabled.Params.Get<FVertexMaskForgeMaterialSlotParams>().SelectedSlotIndex = 1; // Divergent -- must not matter.
	Disabled.bEnabled = false;

	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(Enabled);
	Layer.MaskStack.Add(Disabled); // After.
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestTrue(TEXT("Succeeds -- no collision"), bSuccess);
	TestEqual(TEXT("NumRequired == 1"), Output.NumRequired, 1);
	TestEqual(TEXT("One entry"), WorkingMesh.InstanceResults.Num(), 1);

	return true;
}

// E3. Same GUID across two different enabled layers: one reference disabled, the other enabled.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationSameGuidAcrossLayersMixedTest, "VertexMaskForge.RecipeInstanceGeneration.SameGuidAcrossLayersMixedEnabled", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationSameGuidAcrossLayersMixedTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	const FVertexMaskForgeMaskInstance Enabled = MakeRIGMaterialSlotInstance(0);
	FVertexMaskForgeMaskInstance Disabled = Enabled;
	Disabled.Params.Get<FVertexMaskForgeMaterialSlotParams>().SelectedSlotIndex = 1;
	Disabled.bEnabled = false;

	FVertexMaskForgeFillLayer Layer0 = MakeRIGLayer(true);
	Layer0.MaskStack.Add(Disabled);
	FVertexMaskForgeFillLayer Layer1 = MakeRIGLayer(true);
	Layer1.MaskStack.Add(Enabled);

	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer0);
	Recipe.FillLayers.Add(Layer1);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestTrue(TEXT("Succeeds"), bSuccess);
	TestEqual(TEXT("NumRequired == 1"), Output.NumRequired, 1);
	TestEqual(TEXT("One entry"), WorkingMesh.InstanceResults.Num(), 1);

	return true;
}

// F. Same GUID referenced only by disabled occurrences: zero requirement, zero entry.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationSameGuidOnlyDisabledTest, "VertexMaskForge.RecipeInstanceGeneration.SameGuidOnlyDisabled", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationSameGuidOnlyDisabledTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	FVertexMaskForgeMaskInstance DisabledA = MakeRIGMaterialSlotInstance(0);
	DisabledA.bEnabled = false;
	FVertexMaskForgeMaskInstance DisabledB = DisabledA; // Same InstanceId, divergent slot, both disabled.
	DisabledB.Params.Get<FVertexMaskForgeMaterialSlotParams>().SelectedSlotIndex = 1;

	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(DisabledA);
	Layer.MaskStack.Add(DisabledB);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestTrue(TEXT("Succeeds"), bSuccess);
	TestEqual(TEXT("NumRequired == 0"), Output.NumRequired, 0);
	TestEqual(TEXT("Store untouched"), WorkingMesh.InstanceResults.Num(), 0);

	return true;
}

// G. A GUID referenced only by a disabled instance leaves an unrelated existing entry for that same
// GUID completely untouched (no regeneration, since there is no requirement for it at all).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationDisabledExistingResultTest, "VertexMaskForge.RecipeInstanceGeneration.DisabledExistingResultUntouched", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationDisabledExistingResultTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	FVertexMaskForgeMaskInstance Disabled = MakeRIGMaterialSlotInstance(0);
	Disabled.bEnabled = false;
	WorkingMesh.InstanceResults.StoreOrReplace(Disabled.InstanceId, MakeRIGResult({ 0.77f, 0.77f, 0.77f, 0.77f, 0.77f, 0.77f }));

	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(Disabled);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestTrue(TEXT("Succeeds"), bSuccess);
	TestEqual(TEXT("NumRequired == 0 (disabled instance created no requirement)"), Output.NumRequired, 0);
	const FVertexMaskForgeInstanceMaskResult* Found = WorkingMesh.InstanceResults.Find(Disabled.InstanceId);
	TestNotNull(TEXT("Existing entry still present"), Found);
	if (Found && Found->Values.Num() == 6)
	{
		TestNearlyEqual(TEXT("Existing payload unchanged"), Found->Values[0], 0.77f, RIG_Tolerance);
	}
	TestEqual(TEXT("Store still has exactly one entry"), WorkingMesh.InstanceResults.Num(), 1);

	return true;
}

// H. Control: enabled-only behavior is unchanged by the M16-I.1 fix.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeRecipeInstanceGenerationEnabledControlTest, "VertexMaskForge.RecipeInstanceGeneration.EnabledBehaviorUnchanged", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeRecipeInstanceGenerationEnabledControlTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildRIGFixtureWorkingMesh(0, 1);
	const FVertexMaskForgeMaskInstance A = MakeRIGMaterialSlotInstance(0); // bEnabled defaults to true via Make().

	FVertexMaskForgeFillLayer Layer = MakeRIGLayer(true);
	Layer.MaskStack.Add(A);
	FVertexMaskForgeRecipe Recipe;
	Recipe.FillLayers.Add(Layer);

	FVertexMaskForgeRecipeInstanceGenerationOutput Output;
	const bool bSuccess = VertexMaskForgeRecipeInstanceGeneration::GenerateRequiredInstanceResultsForRecipe(
		Recipe, WorkingMesh, true, nullptr, Output);

	TestTrue(TEXT("Succeeds"), bSuccess);
	TestEqual(TEXT("NumRequired"), Output.NumRequired, 1);
	TestEqual(TEXT("NumReused"), Output.NumReused, 0);
	TestEqual(TEXT("NumGenerated"), Output.NumGenerated, 1);
	const FVertexMaskForgeInstanceMaskResult* Found = WorkingMesh.InstanceResults.Find(A.InstanceId);
	TestNotNull(TEXT("Found"), Found);
	if (Found && Found->Values.Num() == 6)
	{
		TestNearlyEqual(TEXT("Values[0]"), Found->Values[0], 1.0f, RIG_Tolerance);
		TestNearlyEqual(TEXT("Values[3]"), Found->Values[3], 0.0f, RIG_Tolerance);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
