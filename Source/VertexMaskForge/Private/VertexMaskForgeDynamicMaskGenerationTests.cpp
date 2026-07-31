// M16-K.5E: cross-module integration proof -- a real FVertexMaskForgeGeneratorMaskInstance (Dynamic
// Layer model identity, never a Recipe FVertexMaskForgeMaskInstance) generates a REAL Material Slot
// result via VertexMaskForgeDynamicMaskGeneration::GenerateStoredResultForMaterialSlotInstance, the
// result is stored in a real FVertexMaskForgeInstanceResultStore under its own native MaskInstanceId
// (never a copied/correlated GUID from any other identity model), and that stored result is then
// consumed by VertexMaskForgeDynamicLayerEvaluator::EvaluateColor's ResultStore overload to produce a
// real composed color. No FVertexMaskForgeMaskInstance is ever constructed. No StoreOrReplace call is
// made directly by this test -- storage is proven exclusively via WorkingMesh.InstanceResults.Find.
//
// Domain scope (deliberately limited): this test exercises ONLY the Source-Topology (triangle-corner)
// generation path (bUseSourceTopology=true, LOD0=nullptr) -- corner indices 0 and 3 are TRIANGLE-CORNER
// indices (Mesh.TriangleCount()*3 domain), never render-vertex indices, never Dynamic Mesh Vertex IDs.
// This test does NOT prove compatibility with the render-vertex domain, and does NOT touch
// BaselineColors/CommittedColors/WorkingColors, the panel, preview, or any multi-component flow -- see
// this checkpoint's own report for the explicit statement of these limitations.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VertexMaskForgeDynamicLayerEvaluator.h"
#include "VertexMaskForgeDynamicMaskGeneration.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

namespace
{
	// Named distinctly from VertexMaskForgeDynamicLayerEvaluatorTests.cpp's own identical-shaped helper
	// -- unity builds concatenate translation units, so two same-named anonymous-namespace functions
	// across sibling test files collide as a real redefinition, even though each file is independently
	// well-formed.
	bool DynamicMaskGenerationColorsNearlyEqual(const FVector4f& A, const FVector4f& B, const float Tolerance = UE_KINDA_SMALL_NUMBER)
	{
		return FMath::IsNearlyEqual(A.X, B.X, Tolerance)
			&& FMath::IsNearlyEqual(A.Y, B.Y, Tolerance)
			&& FMath::IsNearlyEqual(A.Z, B.Z, Tolerance)
			&& FMath::IsNearlyEqual(A.W, B.W, Tolerance);
	}
}

// 1. GeneratedMaterialSlotResultDrivesEvaluatedColorAtBothCorners.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynamicMaskGenerationCrossModuleTest, "VertexMaskForge.DynamicMaskGeneration.GeneratedMaterialSlotResultDrivesEvaluatedColorAtBothCorners", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynamicMaskGenerationCrossModuleTest::RunTest(const FString& Parameters)
{
	// 1-4. Real Dynamic Layer stack: one layer, White Copy@1 -- Copy@Opacity=1 makes the composed
	// result equal PaintValue exactly (verified against VertexMaskForgeMaskStackComposer::
	// ApplyMaskBlendMode's own Copy case, which returns the Mask/PaintValue operand and ignores Base
	// entirely), which is what lets this test isolate EffectiveMask's real value cleanly.
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = Stack.AddLayer(TEXT("Layer"));
	TestTrue(TEXT("SetLayerFill(White) succeeds"), Stack.SetLayerFill(LayerId, EVertexMaskForgeLayerFill::White));
	TestTrue(TEXT("SetLayerBlendMode(Copy) succeeds"), Stack.SetLayerBlendMode(LayerId, EVertexMaskForgeBlendMode::Copy));
	TestTrue(TEXT("SetLayerOpacity(1.0) succeeds"), Stack.SetLayerOpacity(LayerId, 1.0f));

	// 5. Assign Material Slot via the stack's own controlled API -- mints a real MaskInstanceId.
	TestTrue(TEXT("SetLayerMaskGeneratorType(MaterialSlot) succeeds"), Stack.SetLayerMaskGeneratorType(LayerId, EVertexMaskForgeGeneratorType::MaterialSlot));

	// 6-8. Explicitly construct FVertexMaskForgeMaterialSlotParams and apply it via SetLayerMaskParams
	// (not left at its default) -- SelectedSlotIndex=0 chosen deliberately so triangle 0 (slot 0) is
	// selected and triangle 1 (slot 1) is not, giving two genuinely different, non-coincidental
	// coverage outcomes at the two corners checked below.
	const FGuid MaskInstanceId = Stack.GetLayerMask(LayerId)->MaskInstanceId;
	FVertexMaskForgeGeneratorParams NewParams = MakeVertexMaskForgeGeneratorParams(EVertexMaskForgeGeneratorType::MaterialSlot);
	FVertexMaskForgeMaterialSlotParams& SlotParams = NewParams.Get<FVertexMaskForgeMaterialSlotParams>();
	SlotParams.SelectedSlotIndex = 0;
	SlotParams.bInvert = false;
	TestTrue(TEXT("SetLayerMaskParams succeeds"), Stack.SetLayerMaskParams(LayerId, MaskInstanceId, NewParams));

	// 9-10. Obtain the GeneratorMaskInstance via the stack's own public API -- MaskInstanceId is the
	// one the stack itself minted, never fabricated or copied from elsewhere.
	const FVertexMaskForgeGeneratorMaskInstance* GeneratorMaskInstance = Stack.GetLayerMask(LayerId);
	TestNotNull(TEXT("GeneratorMaskInstance present"), GeneratorMaskInstance);
	if (!GeneratorMaskInstance)
	{
		return false;
	}
	TestEqual(TEXT("MaskInstanceId unchanged by SetLayerMaskParams"), GeneratorMaskInstance->MaskInstanceId, MaskInstanceId);

	// 11-14. Real, synthetic WorkingMesh: two triangles, first mapped to Material Slot 0, second to
	// Material Slot 1 -- DynamicTriangleToMaterialSlot indexed by the real TriangleIDs Mesh.AppendTriangle
	// returns (never assumed to equal a running counter).
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
	WorkingMesh.MaterialSlotOptions.SetNum(2);
	WorkingMesh.DynamicTriangleToMaterialSlot.Init(INDEX_NONE, WorkingMesh.Mesh->MaxTriangleID());
	WorkingMesh.DynamicTriangleToMaterialSlot[Tri0] = 0;
	WorkingMesh.DynamicTriangleToMaterialSlot[Tri1] = 1;

	// 15-17. Real generation, Source-Topology domain (triangle-corner), via the new production API.
	const bool bGenerated = VertexMaskForgeDynamicMaskGeneration::GenerateStoredResultForMaterialSlotInstance(
		*GeneratorMaskInstance, WorkingMesh, /*bUseSourceTopology=*/true, /*LOD0=*/nullptr);
	TestTrue(TEXT("GenerateStoredResultForMaterialSlotInstance returns true"), bGenerated);

	// 18-20. Prove storage directly via Find -- never StoreOrReplace, never a fabricated
	// FVertexMaskForgeInstanceMaskResult, and under exactly the Dynamic Layer's own native MaskInstanceId.
	const FVertexMaskForgeInstanceMaskResult* StoredResult = WorkingMesh.InstanceResults.Find(MaskInstanceId);
	TestNotNull(TEXT("A result exists under the GeneratorMaskInstance's own MaskInstanceId"), StoredResult);
	if (!StoredResult)
	{
		return false;
	}

	// Corner 0 belongs to Tri0 (Slot 0, selected) -- expect coverage 1.0. Corner 3 belongs to Tri1
	// (Slot 1, not selected) -- expect coverage 0.0. Both read directly from the real stored result,
	// confirming the two corners genuinely differ (not a coincidental uniform value).
	float StoredAtCorner0 = -1.0f;
	float StoredAtCorner3 = -1.0f;
	TestTrue(TEXT("Stored result has a value at corner 0"), StoredResult->TryGetValue(0, StoredAtCorner0));
	TestTrue(TEXT("Stored result has a value at corner 3"), StoredResult->TryGetValue(3, StoredAtCorner3));
	TestEqual(TEXT("Corner 0 (Tri0, selected slot) coverage is 1.0"), StoredAtCorner0, 1.0f);
	TestEqual(TEXT("Corner 3 (Tri1, unselected slot) coverage is 0.0"), StoredAtCorner3, 0.0f);

	// 23. Evaluate via the real overload, consuming the real store -- never re-deriving the expected
	// value from StoredAtCorner0/3 above; EffectiveMask multiplies FillValue (PaintValue = FillValue *
	// EffectiveMask, per VertexMaskForgeDynamicLayerEvaluator.cpp's own EvaluateColorInternal), and
	// Copy@Opacity=1 makes the final composed value equal PaintValue exactly, independent of BaseColor.
	const FVector4f Base(0.2f, 0.2f, 0.2f, 1.0f);
	const FVector4f ResultAtCorner0 = VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack, WorkingMesh.InstanceResults, 0);
	const FVector4f ResultAtCorner3 = VertexMaskForgeDynamicLayerEvaluator::EvaluateColor(Base, Stack, WorkingMesh.InstanceResults, 3);

	// Corner 0: PaintValue = White(1) * 1.0 = 1.0 -> Copy@1 -> (1,1,1), Alpha carried verbatim from Base.W.
	TestTrue(TEXT("Corner 0: real generated coverage 1.0 composes to White, Alpha preserved"), DynamicMaskGenerationColorsNearlyEqual(ResultAtCorner0, FVector4f(1.0f, 1.0f, 1.0f, 1.0f)));
	// Corner 3: PaintValue = White(1) * 0.0 = 0.0 -> Copy@1 -> (0,0,0), NOT BaseColor (Copy ignores the
	// composite below it entirely -- see ApplyMaskBlendMode's own Copy case), Alpha carried verbatim.
	TestTrue(TEXT("Corner 3: real generated coverage 0.0 composes to Black, not BaseColor, Alpha preserved"), DynamicMaskGenerationColorsNearlyEqual(ResultAtCorner3, FVector4f(0.0f, 0.0f, 0.0f, 1.0f)));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
