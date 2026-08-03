// M16-K.6D-4: proves VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology
// -- the new, testable Dynamic Source-Topology composition orchestrator -- directly. See
// VertexMaskForgeDynamicSourceTopologyComposition.h for the full contract these tests exercise.
//
// No production caller exists yet (M16-K.6D-5, not this checkpoint, connects this to preview); these
// tests are this function's only caller anywhere in the codebase.

#if WITH_DEV_AUTOMATION_TESTS

#include "DynamicMesh/DynamicMesh3.h"
#include "Misc/AutomationTest.h"
#include "VertexMaskForgeBoundingBoxGenerator.h"
#include "VertexMaskForgeDynamicLayerStack.h"
#include "VertexMaskForgeDynamicSourceTopologyComposition.h"
#include "VertexMaskForgeLayerTypes.h"
#include "VertexMaskForgeRecipeTypes.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

namespace
{
	// Same two-triangle (quad) fixture shape as VertexMaskForgeMaterialSlotCallerOwnedTests.cpp's own
	// BuildCallerOwnedFixtureWorkingMesh -- deliberately re-derived here (not shared) so this file
	// exercises the new orchestrator with zero dependency on that other file's own helper.
	FVertexMaskForgeWorkingMesh BuildOrchestratorFixtureWorkingMesh(const int32 SlotForTri0, const int32 SlotForTri1, const int32 NumSlotOptions = 2)
	{
		FVertexMaskForgeWorkingMesh WorkingMesh;

		WorkingMesh.Mesh = MakeUnique<UE::Geometry::FDynamicMesh3>();
		const int32 V0 = WorkingMesh.Mesh->AppendVertex(FVector3d(0.0, 0.0, 0.0));
		const int32 V1 = WorkingMesh.Mesh->AppendVertex(FVector3d(1.0, 0.0, 0.0));
		const int32 V2 = WorkingMesh.Mesh->AppendVertex(FVector3d(1.0, 1.0, 0.0));
		const int32 V3 = WorkingMesh.Mesh->AppendVertex(FVector3d(0.0, 1.0, 0.0));
		const int32 Tri0 = WorkingMesh.Mesh->AppendTriangle(V0, V1, V2);
		const int32 Tri1 = WorkingMesh.Mesh->AppendTriangle(V0, V2, V3);

		WorkingMesh.bMaterialSlotResolutionValid = true;
		WorkingMesh.bRenderVertexMaterialSlotAmbiguous = false;
		WorkingMesh.MaterialSlotOptions.SetNum(NumSlotOptions);

		WorkingMesh.DynamicTriangleToMaterialSlot.Init(INDEX_NONE, WorkingMesh.Mesh->MaxTriangleID());
		WorkingMesh.DynamicTriangleToMaterialSlot[Tri0] = SlotForTri0;
		WorkingMesh.DynamicTriangleToMaterialSlot[Tri1] = SlotForTri1;

		return WorkingMesh;
	}

	TArray<FColor> MakeSixCornerBaseColors()
	{
		return {
			FColor(10, 20, 30, 100),
			FColor(11, 21, 31, 101),
			FColor(12, 22, 32, 102),
			FColor(13, 23, 33, 103),
			FColor(14, 24, 34, 104),
			FColor(15, 25, 35, 105),
		};
	}

	// Adds a layer with a Material Slot mask, configured via the stack's own controlled mutators --
	// mirrors VertexMaskForgeDynamicCompositionSourceTopologyIntegrationTests.cpp's own setup sequence.
	FGuid AddMaterialSlotLayer(FVertexMaskForgeDynamicLayerStack& Stack, const FString& Name, EVertexMaskForgeLayerFill Fill, EVertexMaskForgeBlendMode BlendMode, float Opacity, int32 SelectedSlotIndex, bool bInvert = false)
	{
		const FGuid LayerId = Stack.AddLayer(Name);
		Stack.SetLayerFill(LayerId, Fill);
		Stack.SetLayerBlendMode(LayerId, BlendMode);
		Stack.SetLayerOpacity(LayerId, Opacity);
		Stack.SetLayerMaskGeneratorType(LayerId, EVertexMaskForgeGeneratorType::MaterialSlot);

		const FVertexMaskForgeGeneratorMaskInstance* MaskInstance = Stack.GetLayerMask(LayerId);
		check(MaskInstance);
		FVertexMaskForgeGeneratorParams NewParams = MakeVertexMaskForgeGeneratorParams(EVertexMaskForgeGeneratorType::MaterialSlot);
		NewParams.Get<FVertexMaskForgeMaterialSlotParams>().SelectedSlotIndex = SelectedSlotIndex;
		NewParams.Get<FVertexMaskForgeMaterialSlotParams>().bInvert = bInvert;
		Stack.SetLayerMaskParams(LayerId, MaskInstance->MaskInstanceId, NewParams);

		return LayerId;
	}

	// Adds a Fill-only layer (no mask) -- EffectiveMask implicitly 1.0.
	FGuid AddFillOnlyLayer(FVertexMaskForgeDynamicLayerStack& Stack, const FString& Name, EVertexMaskForgeLayerFill Fill, EVertexMaskForgeBlendMode BlendMode, float Opacity)
	{
		const FGuid LayerId = Stack.AddLayer(Name);
		Stack.SetLayerFill(LayerId, Fill);
		Stack.SetLayerBlendMode(LayerId, BlendMode);
		Stack.SetLayerOpacity(LayerId, Opacity);
		return LayerId;
	}

	// M16-K.6D-8B: adds a layer with a Bounding Box mask, configured via the stack's own controlled
	// mutators -- mirrors AddMaterialSlotLayer's own setup sequence exactly.
	FGuid AddBoundingBoxLayer(
		FVertexMaskForgeDynamicLayerStack& Stack, const FString& Name,
		EVertexMaskForgeLayerFill Fill, EVertexMaskForgeBlendMode BlendMode, float Opacity,
		const TStaticArray<FVertexMaskForgeAxisMaskParams, 3>& Axes, bool bUseUnifiedBounds = false)
	{
		const FGuid LayerId = Stack.AddLayer(Name);
		Stack.SetLayerFill(LayerId, Fill);
		Stack.SetLayerBlendMode(LayerId, BlendMode);
		Stack.SetLayerOpacity(LayerId, Opacity);
		Stack.SetLayerMaskGeneratorType(LayerId, EVertexMaskForgeGeneratorType::BoundingBox);

		const FVertexMaskForgeGeneratorMaskInstance* MaskInstance = Stack.GetLayerMask(LayerId);
		check(MaskInstance);
		FVertexMaskForgeGeneratorParams NewParams = MakeVertexMaskForgeGeneratorParams(EVertexMaskForgeGeneratorType::BoundingBox);
		FVertexMaskForgeBoundingBoxParams& BBoxParams = NewParams.Get<FVertexMaskForgeBoundingBoxParams>();
		BBoxParams.Axes = Axes;
		BBoxParams.bUseUnifiedBounds = bUseUnifiedBounds;
		Stack.SetLayerMaskParams(LayerId, MaskInstance->MaskInstanceId, NewParams);

		return LayerId;
	}

	// Same two-triangle quad shape as BuildOrchestratorFixtureWorkingMesh, but with distinct Z values
	// (Z correlated with Y: V0/V1 at Z=0, V2/V3 at Z=1) so a Z-axis Bounding Box test has a genuine,
	// non-degenerate Z extent -- BuildOrchestratorFixtureWorkingMesh's own flat (Z=0 everywhere) geometry
	// is deliberately left untouched for every other test in this file.
	FVertexMaskForgeWorkingMesh BuildZVaryingFixtureWorkingMesh()
	{
		FVertexMaskForgeWorkingMesh WorkingMesh;
		WorkingMesh.Mesh = MakeUnique<UE::Geometry::FDynamicMesh3>();
		WorkingMesh.Mesh->AppendVertex(FVector3d(0.0, 0.0, 0.0)); // V0
		WorkingMesh.Mesh->AppendVertex(FVector3d(1.0, 0.0, 0.0)); // V1
		WorkingMesh.Mesh->AppendVertex(FVector3d(1.0, 1.0, 1.0)); // V2
		WorkingMesh.Mesh->AppendVertex(FVector3d(0.0, 1.0, 1.0)); // V3
		WorkingMesh.Mesh->AppendTriangle(0, 1, 2);
		WorkingMesh.Mesh->AppendTriangle(0, 2, 3);
		return WorkingMesh;
	}

	// The fixed corner->VertexID mapping BOTH fixture meshes above share, by construction: Tri0=(V0,V1,V2),
	// Tri1=(V0,V2,V3), appended in that order, so TriangleIndicesItr() yields Tri0 (ID 0) then Tri1 (ID 1)
	// for this freshly-built, never-edited mesh. Corner 0/1/2 -> V0/V1/V2 (identity, by coincidence);
	// corner 3/4/5 -> V0/V2/V3 (NOT identity -- proves a test cannot pass via accidental
	// CornerIndex==VertexID equivalence, and V0/V2 being revisited proves the mapping is exercised on
	// vertices SHARED by multiple triangles). Kept as an explicit, independent, test-side constant --
	// never derived from or shared with the orchestrator's own internal corner->VertexID construction.
	constexpr int32 FixtureCornerToVertexID[6] = { 0, 1, 2, 0, 2, 3 };

	// Matches VertexMaskForgeColorConversion::ToDisplayFColor's own documented rounding contract
	// (RoundToInt(Value*255), clamped to [0,255]) -- already independently proven exact for integer
	// FColor round-trips elsewhere in this codebase (see the ColorConversion test suite); reused here as
	// already-established knowledge, never re-derived as a new formula.
	uint8 UnitFloatToByte(const float Value)
	{
		return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Value * 255.0f), 0, 255));
	}
}

// 1. An empty stack composes to a byte-exact passthrough of BaseColors -- matching
// VertexMaskForgeDynamicLayerEvaluator::EvaluateColor's own empty-stack contract exactly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompEmptyStackTest, "VertexMaskForge.DynamicSourceTopologyComposition.EmptyStackPreservesAllBaseColorsByteExact", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompEmptyStackTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1);
	const FVertexMaskForgeDynamicLayerStack Stack; // empty
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	TArray<FColor> Out;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, Out);

	TestTrue(TEXT("Succeeds for an empty stack"), bSucceeded);
	TestEqual(TEXT("Out.Num() == BaseColors.Num()"), Out.Num(), BaseColors.Num());
	if (Out.Num() == BaseColors.Num())
	{
		for (int32 Index = 0; Index < BaseColors.Num(); ++Index)
		{
			TestEqual(*FString::Printf(TEXT("Out[%d] byte-exact to BaseColors[%d]"), Index, Index), Out[Index], BaseColors[Index]);
		}
	}

	return true;
}

// 2. A single Material-Slot-masked layer (White Fill, Copy, Opacity 1.0, Slot 0 selected) produces the
// same expected coverage as VertexMaskForgeDynamicCompositionSourceTopologyIntegrationTests.cpp's own
// proven real-store-based result, independently computed here without any store.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompSingleMaterialSlotLayerTest, "VertexMaskForge.DynamicSourceTopologyComposition.SingleMaterialSlotLayerWhiteFillCopyProducesExpectedCoverage", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompSingleMaterialSlotLayerTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(/*SlotForTri0=*/0, /*SlotForTri1=*/1);
	FVertexMaskForgeDynamicLayerStack Stack;
	AddMaterialSlotLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, /*SelectedSlotIndex=*/0);

	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();
	TArray<FColor> Out;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, Out);

	TestTrue(TEXT("Succeeds"), bSucceeded);
	TestEqual(TEXT("Out.Num() == 6"), Out.Num(), 6);
	if (Out.Num() != 6)
	{
		return false;
	}

	const FColor Expected[6] = {
		FColor(255, 255, 255, 100),
		FColor(255, 255, 255, 101),
		FColor(255, 255, 255, 102),
		FColor(0, 0, 0, 103),
		FColor(0, 0, 0, 104),
		FColor(0, 0, 0, 105),
	};
	for (int32 Index = 0; Index < 6; ++Index)
	{
		TestEqual(*FString::Printf(TEXT("Out[%d] matches expected"), Index), Out[Index], Expected[Index]);
	}

	return true;
}

// 3. A Fill-only layer (no Mask) never consults Material Slot generation -- EffectiveMask is implicitly
// 1.0 everywhere, so a Black Fill/Copy/Opacity-1 layer forces every corner fully black regardless of the
// mesh's own material-slot mapping.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompFillOnlyLayerTest, "VertexMaskForge.DynamicSourceTopologyComposition.FillOnlyLayerAppliesUniformlyWithoutMask", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompFillOnlyLayerTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1);
	FVertexMaskForgeDynamicLayerStack Stack;
	AddFillOnlyLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::Black, EVertexMaskForgeBlendMode::Copy, 1.0f);

	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();
	TArray<FColor> Out;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, Out);

	TestTrue(TEXT("Succeeds"), bSucceeded);
	TestEqual(TEXT("Out.Num() == 6"), Out.Num(), 6);
	if (Out.Num() != 6)
	{
		return false;
	}

	for (int32 Index = 0; Index < 6; ++Index)
	{
		TestEqual(*FString::Printf(TEXT("Out[%d].R == 0"), Index), Out[Index].R, uint8(0));
		TestEqual(*FString::Printf(TEXT("Out[%d].G == 0"), Index), Out[Index].G, uint8(0));
		TestEqual(*FString::Printf(TEXT("Out[%d].B == 0"), Index), Out[Index].B, uint8(0));
		TestEqual(*FString::Printf(TEXT("Out[%d].A preserved from BaseColors"), Index), Out[Index].A, BaseColors[Index].A);
	}

	return true;
}

// 4. A disabled layer contributes nothing AND is never validated for its Mask's GeneratorType -- an
// unsupported generator type on a disabled layer must not fail the whole call. Uses AmbientOcclusion
// (genuinely unsupported, unlike BoundingBox as of M16-K.6D-8B) so this test's own meaning survives
// Bounding Box becoming a supported generator.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompDisabledLayerTest, "VertexMaskForge.DynamicSourceTopologyComposition.DisabledLayerContributesNothingAndSkipsValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompDisabledLayerTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1);
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = Stack.AddLayer(TEXT("Layer"));
	Stack.SetLayerFill(LayerId, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerBlendMode(LayerId, EVertexMaskForgeBlendMode::Copy);
	Stack.SetLayerOpacity(LayerId, 1.0f);
	// AmbientOcclusion is genuinely unsupported -- would fail the whole call if this layer were enabled.
	Stack.SetLayerMaskGeneratorType(LayerId, EVertexMaskForgeGeneratorType::AmbientOcclusion);
	Stack.SetLayerEnabled(LayerId, false);

	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();
	TArray<FColor> Out;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, Out);

	TestTrue(TEXT("Succeeds -- disabled layer's unsupported generator type is never validated"), bSucceeded);
	TestEqual(TEXT("Out.Num() == 6"), Out.Num(), 6);
	if (Out.Num() == 6)
	{
		for (int32 Index = 0; Index < 6; ++Index)
		{
			TestEqual(*FString::Printf(TEXT("Out[%d] byte-exact passthrough (disabled layer = no-op)"), Index), Out[Index], BaseColors[Index]);
		}
	}

	return true;
}

// 5. An ENABLED masked layer whose GeneratorType is not MaterialSlot fails the WHOLE call explicitly --
// never silently skipped, never treated as Fill-only.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompUnsupportedGeneratorTest, "VertexMaskForge.DynamicSourceTopologyComposition.UnsupportedGeneratorTypeFailsWholeCall", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompUnsupportedGeneratorTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1);
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = Stack.AddLayer(TEXT("Layer"));
	Stack.SetLayerFill(LayerId, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerBlendMode(LayerId, EVertexMaskForgeBlendMode::Copy);
	Stack.SetLayerOpacity(LayerId, 1.0f);
	Stack.SetLayerMaskGeneratorType(LayerId, EVertexMaskForgeGeneratorType::AmbientOcclusion);

	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();
	TArray<FColor> Out = { FColor(1, 2, 3, 4) }; // sentinel, wrong size on purpose
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, Out);

	TestFalse(TEXT("Fails for an unsupported (non-MaterialSlot) masked generator type"), bSucceeded);
	TestEqual(TEXT("Out left completely untouched on failure"), Out.Num(), 1);
	if (Out.Num() == 1)
	{
		TestEqual(TEXT("Out[0] sentinel preserved"), Out[0], FColor(1, 2, 3, 4));
	}

	return true;
}

// 6. Cardinality mismatch (BaseColors.Num() != Mesh.TriangleCount() * 3) fails explicitly with no resize
// and Out left completely untouched -- proving the preserve-on-failure policy this checkpoint chose.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompCardinalityMismatchTest, "VertexMaskForge.DynamicSourceTopologyComposition.CardinalityMismatchFailsAndPreservesPriorOutput", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompCardinalityMismatchTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1);
	const FVertexMaskForgeDynamicLayerStack Stack; // empty -- irrelevant, cardinality fails first

	// Only 4 colors supplied; the fixture expects exactly 6 (two triangles * 3 corners).
	const TArray<FColor> BaseColors = { FColor(1, 1, 1, 255), FColor(2, 2, 2, 255), FColor(3, 3, 3, 255), FColor(4, 4, 4, 255) };
	TArray<FColor> Out = { FColor(9, 9, 9, 9), FColor(8, 8, 8, 8) }; // prior content, must survive failure

	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, Out);

	TestFalse(TEXT("Fails for cardinality mismatch"), bSucceeded);
	TestEqual(TEXT("Out.Num() unchanged (still 2, the prior content)"), Out.Num(), 2);
	if (Out.Num() == 2)
	{
		TestEqual(TEXT("Out[0] prior content preserved"), Out[0], FColor(9, 9, 9, 9));
		TestEqual(TEXT("Out[1] prior content preserved"), Out[1], FColor(8, 8, 8, 8));
	}

	return true;
}

// 7. An invalid WorkingMesh (null Mesh) fails explicitly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompInvalidMeshTest, "VertexMaskForge.DynamicSourceTopologyComposition.InvalidMeshFailsExplicitly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompInvalidMeshTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh; // Mesh is never assigned -- null.
	const FVertexMaskForgeDynamicLayerStack Stack;
	const TArray<FColor> BaseColors;

	TArray<FColor> Out;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, Out);

	TestFalse(TEXT("Fails for a null Mesh"), bSucceeded);

	return true;
}

// 8. A structural Material Slot generation failure (out-of-range SelectedSlotIndex -> Unavailable) fails
// the WHOLE call, even though the mesh and cardinality are otherwise valid.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompGeneratorFailureTest, "VertexMaskForge.DynamicSourceTopologyComposition.MaterialSlotGeneratorFailureFailsWholeCall", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompGeneratorFailureTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1, /*NumSlotOptions=*/2);
	FVertexMaskForgeDynamicLayerStack Stack;
	// SelectedSlotIndex 5 is out of range for MaterialSlotOptions.Num() == 2 -> Unavailable.
	AddMaterialSlotLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, /*SelectedSlotIndex=*/5);

	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();
	TArray<FColor> Out;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, Out);

	TestFalse(TEXT("Fails when the underlying Material Slot generator is Unavailable"), bSucceeded);

	return true;
}

// 9. Alpha is always carried from BaseColors, completely unconditionally -- proven with a Fill/Copy layer
// that forces R/G/B, while Alpha still matches BaseColors exactly at every corner.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompAlphaPassthroughTest, "VertexMaskForge.DynamicSourceTopologyComposition.AlphaPreservedFromBaseColorsUnconditionally", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompAlphaPassthroughTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1);
	FVertexMaskForgeDynamicLayerStack Stack;
	AddFillOnlyLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f);

	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();
	TArray<FColor> Out;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, Out);

	TestTrue(TEXT("Succeeds"), bSucceeded);
	if (Out.Num() == 6)
	{
		for (int32 Index = 0; Index < 6; ++Index)
		{
			TestEqual(*FString::Printf(TEXT("Out[%d].A == BaseColors[%d].A"), Index, Index), Out[Index].A, BaseColors[Index].A);
		}
	}

	return true;
}

// 10. Per-layer Channel Filter (bAffectRed/Green/Blue) isolates a layer's contribution to exactly the
// channels it affects -- a Red-only White/Copy/Opacity-1 layer forces Red to 255 while leaving Green/Blue
// exactly at their original BaseColors value (round-tripped through the same /255,*255 conversion the
// K.6D-2 seam tests already prove is exact for integer FColor inputs).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompChannelFilterTest, "VertexMaskForge.DynamicSourceTopologyComposition.PerLayerChannelFilterIsolatesAffectedChannelOnly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompChannelFilterTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1);
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = AddFillOnlyLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f);
	Stack.SetLayerChannelFilter(LayerId, /*bAffectRed=*/true, /*bAffectGreen=*/false, /*bAffectBlue=*/false);

	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();
	TArray<FColor> Out;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, Out);

	TestTrue(TEXT("Succeeds"), bSucceeded);
	if (Out.Num() == 6)
	{
		for (int32 Index = 0; Index < 6; ++Index)
		{
			TestEqual(*FString::Printf(TEXT("Out[%d].R forced to 255 (affected)"), Index), Out[Index].R, uint8(255));
			TestEqual(*FString::Printf(TEXT("Out[%d].G preserved (not affected)"), Index), Out[Index].G, BaseColors[Index].G);
			TestEqual(*FString::Printf(TEXT("Out[%d].B preserved (not affected)"), Index), Out[Index].B, BaseColors[Index].B);
		}
	}

	return true;
}

// 11. Order-of-composition proof: two Fill-only, Red-only-affecting, Copy/Opacity-1 layers with opposite
// Fill values compose differently depending on array order (Copy always wins with the LAST enabled
// layer's own value for the channel it affects) -- an independently reasoned expected value for each of
// the two orderings, using FVertexMaskForgeDynamicLayerStack's own real, already-ordered layer list (this
// is NOT invented multi-layer support -- the Stack type already supports an arbitrary ordered layer list;
// this test only proves this orchestrator's own fold respects that existing order contract).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompOrderMattersTest, "VertexMaskForge.DynamicSourceTopologyComposition.LayerOrderDeterminesFinalResultWithIndependentlyComputedExpectations", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompOrderMattersTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1);
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	// Forward: White-Red-Copy added first, then Black-Red-Copy -- Black is folded LAST, so Red ends at 0.
	FVertexMaskForgeDynamicLayerStack ForwardStack;
	{
		const FGuid WhiteLayerId = AddFillOnlyLayer(ForwardStack, TEXT("White"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f);
		ForwardStack.SetLayerChannelFilter(WhiteLayerId, true, false, false);
		const FGuid BlackLayerId = AddFillOnlyLayer(ForwardStack, TEXT("Black"), EVertexMaskForgeLayerFill::Black, EVertexMaskForgeBlendMode::Copy, 1.0f);
		ForwardStack.SetLayerChannelFilter(BlackLayerId, true, false, false);
	}

	// Reverse: the same two layers, added in the opposite order -- White is folded LAST, so Red ends at 255.
	FVertexMaskForgeDynamicLayerStack ReverseStack;
	{
		const FGuid BlackLayerId = AddFillOnlyLayer(ReverseStack, TEXT("Black"), EVertexMaskForgeLayerFill::Black, EVertexMaskForgeBlendMode::Copy, 1.0f);
		ReverseStack.SetLayerChannelFilter(BlackLayerId, true, false, false);
		const FGuid WhiteLayerId = AddFillOnlyLayer(ReverseStack, TEXT("White"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f);
		ReverseStack.SetLayerChannelFilter(WhiteLayerId, true, false, false);
	}

	TArray<FColor> ForwardOut;
	const bool bForwardSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, ForwardStack, BaseColors, ForwardOut);
	TArray<FColor> ReverseOut;
	const bool bReverseSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, ReverseStack, BaseColors, ReverseOut);

	TestTrue(TEXT("Forward succeeds"), bForwardSucceeded);
	TestTrue(TEXT("Reverse succeeds"), bReverseSucceeded);
	if (ForwardOut.Num() == 6 && ReverseOut.Num() == 6)
	{
		for (int32 Index = 0; Index < 6; ++Index)
		{
			TestEqual(*FString::Printf(TEXT("Forward[%d].R == 0 (Black folded last)"), Index), ForwardOut[Index].R, uint8(0));
			TestEqual(*FString::Printf(TEXT("Reverse[%d].R == 255 (White folded last)"), Index), ReverseOut[Index].R, uint8(255));
			TestEqual(*FString::Printf(TEXT("Forward[%d].G untouched"), Index), ForwardOut[Index].G, BaseColors[Index].G);
			TestEqual(*FString::Printf(TEXT("Reverse[%d].G untouched"), Index), ReverseOut[Index].G, BaseColors[Index].G);
		}
	}

	return true;
}

// 12. Structural isolation proof: the orchestrator never touches WorkingMesh.InstanceResults (the only
// FVertexMaskForgeInstanceResultStore reachable from its own parameters) -- it starts and ends empty
// across a successful, real Material-Slot-masked call.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompNoStoreTouchedTest, "VertexMaskForge.DynamicSourceTopologyComposition.NeverTouchesWorkingMeshInstanceResultStore", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompNoStoreTouchedTest::RunTest(const FString& Parameters)
{
	FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1);
	TestEqual(TEXT("InstanceResults starts empty"), WorkingMesh.InstanceResults.Num(), 0);

	FVertexMaskForgeDynamicLayerStack Stack;
	AddMaterialSlotLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, 0);

	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();
	TArray<FColor> Out;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, Out);

	TestTrue(TEXT("Succeeds"), bSucceeded);
	TestEqual(TEXT("InstanceResults still empty -- never written by this orchestrator"), WorkingMesh.InstanceResults.Num(), 0);

	return true;
}

// 13. Inputs are never mutated -- WorkingMesh, Stack, and BaseColors are all taken by const&/const view;
// re-check their observable state is unchanged after a real, successful call.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompInputsUnchangedTest, "VertexMaskForge.DynamicSourceTopologyComposition.InputsUnchangedAfterCall", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompInputsUnchangedTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1);
	FVertexMaskForgeDynamicLayerStack Stack;
	AddMaterialSlotLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, 0);
	const int32 LayerCountBefore = Stack.Num();

	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();
	const TArray<FColor> BaseColorsCopy = BaseColors;

	TArray<FColor> Out;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, Out);

	TestTrue(TEXT("Succeeds"), bSucceeded);
	TestEqual(TEXT("Stack.Num() unchanged"), Stack.Num(), LayerCountBefore);
	TestEqual(TEXT("BaseColors.Num() unchanged"), BaseColors.Num(), BaseColorsCopy.Num());
	for (int32 Index = 0; Index < BaseColors.Num(); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("BaseColors[%d] unchanged"), Index), BaseColors[Index], BaseColorsCopy[Index]);
	}

	return true;
}

// 14. M16-K.6D-6 defect investigation (B4): a single Fill-only (Mask=None), White, Copy layer over a
// known, uniform, non-white baseline (FColor(64,64,64,255) at every corner) must produce a value that
// is CONTINUOUS and MONOTONIC across the full [0,1] Opacity range -- Copy's own formula
// (BlendMaskValueUnclamped: Lerp(Base, White, Opacity), see VertexMaskForgeMaskStackComposer.cpp) has
// no discontinuity or plateau anywhere in [0,1] by construction. Expected R values below are computed
// independently from that same documented formula (never copied from the orchestrator's own output):
// R(t) = Base/255 + (1 - Base/255) * t, then ToDisplayFColor's own documented rounding
// (RoundToInt(R*255), clamped) -- Base=64: R(1.0)=255, R(0.75)=207, R(0.5)=160, R(0.25)=112, R(0.0)=64.
// This test exists to determine whether the "no visible change between Opacity 1.0/0.75/0.5" defect
// reported during M16-K.6D-6 manual validation is a composition-math defect (this orchestrator) or lies
// downstream of it (the K.6D-2 seam / panel wiring / preview material) -- it asserts the orchestrator's
// OWN numeric output only, never anything about the viewport.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompOpacityContinuityTest, "VertexMaskForge.DynamicSourceTopologyComposition.CopyBlendOpacityIsContinuousAndMonotonicAcrossFullRange", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompOpacityContinuityTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1);

	const TArray<FColor> BaseColors = {
		FColor(64, 64, 64, 255), FColor(64, 64, 64, 255), FColor(64, 64, 64, 255),
		FColor(64, 64, 64, 255), FColor(64, 64, 64, 255), FColor(64, 64, 64, 255),
	};

	const float OpacitySamples[5] = { 1.0f, 0.75f, 0.5f, 0.25f, 0.0f };
	const uint8 ExpectedR[5] = { 255, 207, 160, 112, 64 };

	uint8 PreviousR = 255; // Sentinel higher than any possible first sample's predecessor comparison need.
	bool bFirstSample = true;
	for (int32 SampleIndex = 0; SampleIndex < 5; ++SampleIndex)
	{
		FVertexMaskForgeDynamicLayerStack Stack;
		AddFillOnlyLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, OpacitySamples[SampleIndex]);

		TArray<FColor> Out;
		const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, Out);
		TestTrue(*FString::Printf(TEXT("Opacity %f succeeds"), OpacitySamples[SampleIndex]), bSucceeded);
		if (!bSucceeded || Out.Num() != 6)
		{
			continue;
		}

		TestEqual(*FString::Printf(TEXT("Opacity %f: R matches independently-computed expected value"), OpacitySamples[SampleIndex]), Out[0].R, ExpectedR[SampleIndex]);

		if (!bFirstSample)
		{
			TestTrue(*FString::Printf(TEXT("Opacity %f: R is strictly less than the previous (higher-opacity) sample's R -- no plateau"), OpacitySamples[SampleIndex]), Out[0].R < PreviousR);
		}
		PreviousR = Out[0].R;
		bFirstSample = false;
	}

	return true;
}

// --- M16-K.6D-8B: Local-space Bounding Box support ------------------------------------------------

// 15. A Local-space X-axis Bounding Box layer is accepted and produces a byte-exact result against the
// REAL, authoritative generator (called directly, never reimplemented) combined with an explicit,
// independent test-side corner->VertexID mapping. Reuses the already-established "White Fill / Copy /
// Opacity 1.0 -> RGB == mask value" and "Alpha == BaseColors.Alpha" contracts (proven above by
// SingleMaterialSlotLayerTest/AlphaPassthroughTest) rather than re-deriving the blend formula. The fixed
// FixtureCornerToVertexID mapping ([0,1,2,0,2,3]) means corners 3-5 do NOT equal their own VertexID and
// revisit vertices shared by both triangles -- a naive Values[CornerIndex] bug cannot pass this test.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompBBoxLocalXAxisTest, "VertexMaskForge.DynamicSourceTopologyComposition.BoundingBoxLocalSpaceXAxisByteExactAgainstGeneratorAndCornerMapping", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompBBoxLocalXAxisTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1);

	TStaticArray<FVertexMaskForgeAxisMaskParams, 3> Axes;
	Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].bEnabled = true;
	Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].Position = 0.5f;
	Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].TransitionWidth = 1.0f;

	const FVertexMaskForgeScalarMask ReferenceMask = VertexMaskForgeBoundingBoxGenerator::GenerateBoundingBoxMaskFromDynamicMesh(
		*WorkingMesh.Mesh, Axes, FTransform::Identity);
	TestTrue(TEXT("Reference generator State == Ready"), ReferenceMask.State == EVertexMaskForgeScalarMaskState::Ready);

	FVertexMaskForgeDynamicLayerStack Stack;
	AddBoundingBoxLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, Axes);

	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();
	TArray<FColor> Out;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, Out);

	TestTrue(TEXT("Orchestrator accepts a Local-space Bounding Box layer"), bSucceeded);
	TestEqual(TEXT("Out.Num() == 6"), Out.Num(), 6);
	if (!bSucceeded || Out.Num() != 6)
	{
		return false;
	}

	for (int32 CornerIndex = 0; CornerIndex < 6; ++CornerIndex)
	{
		const int32 VertexID = FixtureCornerToVertexID[CornerIndex];
		float ExpectedMaskValue = 0.0f;
		const bool bHasValue = ReferenceMask.TryGetValue(VertexID, ExpectedMaskValue);
		TestTrue(*FString::Printf(TEXT("Reference mask has a value at VertexID %d (corner %d)"), VertexID, CornerIndex), bHasValue);
		if (!bHasValue)
		{
			continue;
		}

		const uint8 ExpectedByte = UnitFloatToByte(ExpectedMaskValue);
		TestEqual(*FString::Printf(TEXT("Out[%d].R byte-exact vs reference generator + corner mapping"), CornerIndex), Out[CornerIndex].R, ExpectedByte);
		TestEqual(*FString::Printf(TEXT("Out[%d].G byte-exact vs reference generator + corner mapping"), CornerIndex), Out[CornerIndex].G, ExpectedByte);
		TestEqual(*FString::Printf(TEXT("Out[%d].B byte-exact vs reference generator + corner mapping"), CornerIndex), Out[CornerIndex].B, ExpectedByte);
		TestEqual(*FString::Printf(TEXT("Out[%d].A == BaseColors[%d].A"), CornerIndex, CornerIndex), Out[CornerIndex].A, BaseColors[CornerIndex].A);
	}

	TestFalse(TEXT("Corner 3 does not map to VertexID 3 (proves non-identity mapping is exercised)"), FixtureCornerToVertexID[3] == 3);
	TestFalse(TEXT("Corner 4 does not map to VertexID 4"), FixtureCornerToVertexID[4] == 4);
	TestFalse(TEXT("Corner 5 does not map to VertexID 5"), FixtureCornerToVertexID[5] == 5);

	return true;
}

// 16. Y-axis and Z-axis Local-space evaluation, on a mesh with genuine Z variation (the primary fixture
// is flat in Z, so a real Z-axis test needs BuildZVaryingFixtureWorkingMesh) -- each byte-exact against
// the real generator, same technique as the X-axis test above.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompBBoxLocalYZAxisTest, "VertexMaskForge.DynamicSourceTopologyComposition.BoundingBoxLocalSpaceYAndZAxisByteExactAgainstGenerator", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompBBoxLocalYZAxisTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildZVaryingFixtureWorkingMesh();
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	for (const EVertexMaskForgeBoundsAxis Axis : { EVertexMaskForgeBoundsAxis::Y, EVertexMaskForgeBoundsAxis::Z })
	{
		TStaticArray<FVertexMaskForgeAxisMaskParams, 3> Axes;
		Axes[static_cast<int32>(Axis)].bEnabled = true;
		Axes[static_cast<int32>(Axis)].Position = 0.5f;
		Axes[static_cast<int32>(Axis)].TransitionWidth = 1.0f;

		const FVertexMaskForgeScalarMask ReferenceMask = VertexMaskForgeBoundingBoxGenerator::GenerateBoundingBoxMaskFromDynamicMesh(
			*WorkingMesh.Mesh, Axes, FTransform::Identity);
		TestTrue(*FString::Printf(TEXT("Axis %d reference generator State == Ready"), static_cast<int32>(Axis)), ReferenceMask.State == EVertexMaskForgeScalarMaskState::Ready);

		FVertexMaskForgeDynamicLayerStack Stack;
		AddBoundingBoxLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, Axes);

		TArray<FColor> Out;
		const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, Out);
		TestTrue(*FString::Printf(TEXT("Axis %d succeeds"), static_cast<int32>(Axis)), bSucceeded);
		if (!bSucceeded || Out.Num() != 6)
		{
			continue;
		}

		for (int32 CornerIndex = 0; CornerIndex < 6; ++CornerIndex)
		{
			const int32 VertexID = FixtureCornerToVertexID[CornerIndex];
			float ExpectedMaskValue = 0.0f;
			if (!ReferenceMask.TryGetValue(VertexID, ExpectedMaskValue))
			{
				continue;
			}
			const uint8 ExpectedByte = UnitFloatToByte(ExpectedMaskValue);
			TestEqual(*FString::Printf(TEXT("Axis %d: Out[%d].R byte-exact"), static_cast<int32>(Axis), CornerIndex), Out[CornerIndex].R, ExpectedByte);
		}
	}

	return true;
}

// 17. Multiple enabled axes (X and Y together) preserve the generator's own combination semantics --
// proven by byte-exact comparison against a direct generator call with the SAME two-axis params, never
// by re-deriving the max() combination rule in this test.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompBBoxMultipleAxesTest, "VertexMaskForge.DynamicSourceTopologyComposition.BoundingBoxMultipleEnabledAxesByteExactAgainstGenerator", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompBBoxMultipleAxesTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildZVaryingFixtureWorkingMesh();
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	TStaticArray<FVertexMaskForgeAxisMaskParams, 3> Axes;
	Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].bEnabled = true;
	Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].Position = 0.5f;
	Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].TransitionWidth = 1.0f;
	Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::Y)].bEnabled = true;
	Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::Y)].Position = 0.25f;
	Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::Y)].TransitionWidth = 0.5f;

	const FVertexMaskForgeScalarMask ReferenceMask = VertexMaskForgeBoundingBoxGenerator::GenerateBoundingBoxMaskFromDynamicMesh(
		*WorkingMesh.Mesh, Axes, FTransform::Identity);
	TestTrue(TEXT("Reference generator State == Ready"), ReferenceMask.State == EVertexMaskForgeScalarMaskState::Ready);

	FVertexMaskForgeDynamicLayerStack Stack;
	AddBoundingBoxLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, Axes);

	TArray<FColor> Out;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, Out);
	TestTrue(TEXT("Succeeds"), bSucceeded);
	if (!bSucceeded || Out.Num() != 6)
	{
		return false;
	}

	for (int32 CornerIndex = 0; CornerIndex < 6; ++CornerIndex)
	{
		const int32 VertexID = FixtureCornerToVertexID[CornerIndex];
		float ExpectedMaskValue = 0.0f;
		if (!ReferenceMask.TryGetValue(VertexID, ExpectedMaskValue))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("Out[%d].R byte-exact for combined X+Y axes"), CornerIndex), Out[CornerIndex].R, UnitFloatToByte(ExpectedMaskValue));
	}

	return true;
}

// 18. Per-axis Invert and Mirror are honored -- each byte-exact against a direct generator call with the
// matching flag set, never re-derived here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompBBoxInvertMirrorTest, "VertexMaskForge.DynamicSourceTopologyComposition.BoundingBoxInvertAndMirrorByteExactAgainstGenerator", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompBBoxInvertMirrorTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1);
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	auto RunOneConfig = [&](const bool bInvert, const bool bMirror, const TCHAR* Label)
	{
		TStaticArray<FVertexMaskForgeAxisMaskParams, 3> Axes;
		Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].bEnabled = true;
		Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].Position = 0.5f;
		Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].TransitionWidth = 1.0f;
		Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].bInvert = bInvert;
		Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].bMirror = bMirror;

		const FVertexMaskForgeScalarMask ReferenceMask = VertexMaskForgeBoundingBoxGenerator::GenerateBoundingBoxMaskFromDynamicMesh(
			*WorkingMesh.Mesh, Axes, FTransform::Identity);
		TestTrue(*FString::Printf(TEXT("%s: reference generator State == Ready"), Label), ReferenceMask.State == EVertexMaskForgeScalarMaskState::Ready);

		FVertexMaskForgeDynamicLayerStack Stack;
		AddBoundingBoxLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, Axes);

		TArray<FColor> Out;
		const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, Out);
		TestTrue(*FString::Printf(TEXT("%s: succeeds"), Label), bSucceeded);
		if (!bSucceeded || Out.Num() != 6)
		{
			return;
		}
		for (int32 CornerIndex = 0; CornerIndex < 6; ++CornerIndex)
		{
			const int32 VertexID = FixtureCornerToVertexID[CornerIndex];
			float ExpectedMaskValue = 0.0f;
			if (!ReferenceMask.TryGetValue(VertexID, ExpectedMaskValue))
			{
				continue;
			}
			TestEqual(*FString::Printf(TEXT("%s: Out[%d].R byte-exact"), Label, CornerIndex), Out[CornerIndex].R, UnitFloatToByte(ExpectedMaskValue));
		}
	};

	RunOneConfig(/*bInvert=*/true, /*bMirror=*/false, TEXT("Invert"));
	RunOneConfig(/*bInvert=*/false, /*bMirror=*/true, TEXT("Mirror"));

	return true;
}

// 19. Falloff (TransitionWidth) and Position produce representative non-binary intermediate values --
// byte-exact against a direct generator call with the same wide-falloff, off-center params, never
// re-derived here. Proves the orchestrator does not silently clamp/binarize the generator's own gradient.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompBBoxFalloffTest, "VertexMaskForge.DynamicSourceTopologyComposition.BoundingBoxFalloffAndPositionByteExactAgainstGenerator", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompBBoxFalloffTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1);
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	TStaticArray<FVertexMaskForgeAxisMaskParams, 3> Axes;
	Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].bEnabled = true;
	Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].Position = 0.25f;
	Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].TransitionWidth = 2.0f; // Wide falloff -- avoids a hard 0/1 binary result.

	const FVertexMaskForgeScalarMask ReferenceMask = VertexMaskForgeBoundingBoxGenerator::GenerateBoundingBoxMaskFromDynamicMesh(
		*WorkingMesh.Mesh, Axes, FTransform::Identity);
	TestTrue(TEXT("Reference generator State == Ready"), ReferenceMask.State == EVertexMaskForgeScalarMaskState::Ready);

	FVertexMaskForgeDynamicLayerStack Stack;
	AddBoundingBoxLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, Axes);

	TArray<FColor> Out;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, Out);
	TestTrue(TEXT("Succeeds"), bSucceeded);
	if (!bSucceeded || Out.Num() != 6)
	{
		return false;
	}

	bool bAnyNonBinary = false;
	for (int32 CornerIndex = 0; CornerIndex < 6; ++CornerIndex)
	{
		const int32 VertexID = FixtureCornerToVertexID[CornerIndex];
		float ExpectedMaskValue = 0.0f;
		if (!ReferenceMask.TryGetValue(VertexID, ExpectedMaskValue))
		{
			continue;
		}
		if (ExpectedMaskValue > KINDA_SMALL_NUMBER && ExpectedMaskValue < 1.0f - KINDA_SMALL_NUMBER)
		{
			bAnyNonBinary = true;
		}
		TestEqual(*FString::Printf(TEXT("Out[%d].R byte-exact vs reference generator"), CornerIndex), Out[CornerIndex].R, UnitFloatToByte(ExpectedMaskValue));
	}
	TestTrue(TEXT("At least one corner produced a genuine non-binary (0<v<1) mask value"), bAnyNonBinary);

	return true;
}

// 20. Blend/Opacity remain downstream composition concerns for a Bounding-Box-sourced mask, exactly as
// for Material Slot: Opacity 0.0 leaves the composite at BaseColors regardless of the (real, Ready)
// Bounding Box mask's own values, since PaintValue's contribution vanishes at the Copy formula's own
// Opacity==0 case (already established, never re-derived here). A disabled-but-otherwise-valid Bounding
// Box layer likewise contributes nothing -- distinct from the existing DisabledLayerTest, which uses a
// genuinely UNSUPPORTED disabled generator to prove validation-skipping; this proves a SUPPORTED,
// disabled generator still contributes nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompBBoxOpacityAndDisabledTest, "VertexMaskForge.DynamicSourceTopologyComposition.BoundingBoxOpacityZeroAndDisabledLayerPreserveBaseColors", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompBBoxOpacityAndDisabledTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1);
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	TStaticArray<FVertexMaskForgeAxisMaskParams, 3> Axes;
	Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].bEnabled = true;
	Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].Position = 0.5f;
	Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].TransitionWidth = 1.0f;

	// --- Opacity 0.0, enabled, otherwise-valid layer -- must be a full passthrough. ---
	{
		FVertexMaskForgeDynamicLayerStack Stack;
		AddBoundingBoxLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, /*Opacity=*/0.0f, Axes);

		TArray<FColor> Out;
		const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, Out);
		TestTrue(TEXT("Opacity 0.0: succeeds"), bSucceeded);
		if (bSucceeded && Out.Num() == 6)
		{
			for (int32 Index = 0; Index < 6; ++Index)
			{
				TestEqual(*FString::Printf(TEXT("Opacity 0.0: Out[%d] byte-exact passthrough"), Index), Out[Index], BaseColors[Index]);
			}
		}
	}

	// --- Disabled, otherwise-valid layer -- must also be a full passthrough. ---
	{
		FVertexMaskForgeDynamicLayerStack Stack;
		const FGuid LayerId = AddBoundingBoxLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, Axes);
		Stack.SetLayerEnabled(LayerId, false);

		TArray<FColor> Out;
		const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, Out);
		TestTrue(TEXT("Disabled: succeeds"), bSucceeded);
		if (bSucceeded && Out.Num() == 6)
		{
			for (int32 Index = 0; Index < 6; ++Index)
			{
				TestEqual(*FString::Printf(TEXT("Disabled: Out[%d] byte-exact passthrough"), Index), Out[Index], BaseColors[Index]);
			}
		}
	}

	return true;
}

// 21. Two independently parameterized Bounding Box layers retain distinct masks and compose strictly in
// Stack order (Copy always wins with the LAST enabled layer's own value, the same order contract
// OrderMattersTest above already proves for Fill-only layers) -- and reordering them changes the final
// result, proving this specific generator type is not special-cased around the authoritative orchestrator.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompBBoxTwoLayersOrderTest, "VertexMaskForge.DynamicSourceTopologyComposition.TwoBoundingBoxLayersRetainDistinctMasksAndReorderChangesResult", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompBBoxTwoLayersOrderTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1);
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	TStaticArray<FVertexMaskForgeAxisMaskParams, 3> XAxes;
	XAxes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].bEnabled = true;
	XAxes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].Position = 0.5f;
	XAxes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].TransitionWidth = 1.0f;

	TStaticArray<FVertexMaskForgeAxisMaskParams, 3> YAxes;
	YAxes[static_cast<int32>(EVertexMaskForgeBoundsAxis::Y)].bEnabled = true;
	YAxes[static_cast<int32>(EVertexMaskForgeBoundsAxis::Y)].Position = 0.5f;
	YAxes[static_cast<int32>(EVertexMaskForgeBoundsAxis::Y)].TransitionWidth = 1.0f;

	const FVertexMaskForgeScalarMask XReference = VertexMaskForgeBoundingBoxGenerator::GenerateBoundingBoxMaskFromDynamicMesh(*WorkingMesh.Mesh, XAxes, FTransform::Identity);
	const FVertexMaskForgeScalarMask YReference = VertexMaskForgeBoundingBoxGenerator::GenerateBoundingBoxMaskFromDynamicMesh(*WorkingMesh.Mesh, YAxes, FTransform::Identity);
	TestTrue(TEXT("X reference State == Ready"), XReference.State == EVertexMaskForgeScalarMaskState::Ready);
	TestTrue(TEXT("Y reference State == Ready"), YReference.State == EVertexMaskForgeScalarMaskState::Ready);

	// Forward: X-masked layer first, Y-masked layer last -- Y (folded last) determines the Copy result.
	FVertexMaskForgeDynamicLayerStack ForwardStack;
	AddBoundingBoxLayer(ForwardStack, TEXT("X"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, XAxes);
	AddBoundingBoxLayer(ForwardStack, TEXT("Y"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, YAxes);

	// Reverse: same two layers, opposite order -- X (folded last) determines the Copy result instead.
	FVertexMaskForgeDynamicLayerStack ReverseStack;
	AddBoundingBoxLayer(ReverseStack, TEXT("Y"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, YAxes);
	AddBoundingBoxLayer(ReverseStack, TEXT("X"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, XAxes);

	TArray<FColor> ForwardOut;
	TArray<FColor> ReverseOut;
	const bool bForwardSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, ForwardStack, BaseColors, ForwardOut);
	const bool bReverseSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, ReverseStack, BaseColors, ReverseOut);
	TestTrue(TEXT("Forward succeeds"), bForwardSucceeded);
	TestTrue(TEXT("Reverse succeeds"), bReverseSucceeded);
	if (!bForwardSucceeded || !bReverseSucceeded || ForwardOut.Num() != 6 || ReverseOut.Num() != 6)
	{
		return false;
	}

	for (int32 CornerIndex = 0; CornerIndex < 6; ++CornerIndex)
	{
		const int32 VertexID = FixtureCornerToVertexID[CornerIndex];
		float ExpectedYValue = 0.0f;
		float ExpectedXValue = 0.0f;
		if (!YReference.TryGetValue(VertexID, ExpectedYValue) || !XReference.TryGetValue(VertexID, ExpectedXValue))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("Forward[%d].R matches Y (folded last)"), CornerIndex), ForwardOut[CornerIndex].R, UnitFloatToByte(ExpectedYValue));
		TestEqual(*FString::Printf(TEXT("Reverse[%d].R matches X (folded last)"), CornerIndex), ReverseOut[CornerIndex].R, UnitFloatToByte(ExpectedXValue));
	}

	return true;
}

// 22. World Space is REJECTED (whole-call failure), never silently reinterpreted as Local Space -- Out is
// left completely untouched on failure, matching this orchestrator's existing preserve-on-failure policy.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompBBoxWorldSpaceRejectedTest, "VertexMaskForge.DynamicSourceTopologyComposition.BoundingBoxWorldSpaceRejectedInThisCheckpoint", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompBBoxWorldSpaceRejectedTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1);
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	TStaticArray<FVertexMaskForgeAxisMaskParams, 3> Axes;
	Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].bEnabled = true;
	Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].Position = 0.5f;
	Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].TransitionWidth = 1.0f;
	Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].bWorldSpace = true;

	FVertexMaskForgeDynamicLayerStack Stack;
	AddBoundingBoxLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, Axes);

	TArray<FColor> Out = { FColor(1, 2, 3, 4) }; // sentinel, wrong size on purpose
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, Out);

	TestFalse(TEXT("World Space is rejected (fails the whole call)"), bSucceeded);
	TestEqual(TEXT("Out left completely untouched on failure"), Out.Num(), 1);
	if (Out.Num() == 1)
	{
		TestEqual(TEXT("Out[0] sentinel preserved"), Out[0], FColor(1, 2, 3, 4));
	}

	return true;
}

// 23. Unified Bounds is REJECTED (whole-call failure), never silently reinterpreted as per-mesh bounds --
// Out is left completely untouched on failure, checked unconditionally regardless of axis enable state.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompBBoxUnifiedBoundsRejectedTest, "VertexMaskForge.DynamicSourceTopologyComposition.BoundingBoxUnifiedBoundsRejectedInThisCheckpoint", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompBBoxUnifiedBoundsRejectedTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1);
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	TStaticArray<FVertexMaskForgeAxisMaskParams, 3> Axes;
	Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].bEnabled = true;
	Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].Position = 0.5f;
	Axes[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].TransitionWidth = 1.0f;

	FVertexMaskForgeDynamicLayerStack Stack;
	AddBoundingBoxLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, Axes, /*bUseUnifiedBounds=*/true);

	TArray<FColor> Out = { FColor(5, 6, 7, 8) }; // sentinel, wrong size on purpose
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, Out);

	TestFalse(TEXT("Unified Bounds is rejected (fails the whole call)"), bSucceeded);
	TestEqual(TEXT("Out left completely untouched on failure"), Out.Num(), 1);
	if (Out.Num() == 1)
	{
		TestEqual(TEXT("Out[0] sentinel preserved"), Out[0], FColor(5, 6, 7, 8));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
