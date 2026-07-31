// M16-J final: integration proof that the REAL panel production path (VertexMaskForgePanel::
// ComputeComposedColorsRGB / ComputeComposedColorsRGBSourceTopology, both defined in
// SVertexMaskForgePanel.cpp, exposed non-static via VertexMaskForgeMaskTypes.h for exactly this purpose
// -- see that header's own doc comment) reaches VertexMaskForgeGeneratorLayerBridge::
// ComposeGeneratorLayersSequential (and therefore VertexMaskForgeSequentialEvaluator), and NOT
// VertexMaskForgeMaskStackComposer::ComposeStack (the legacy, fixed-stage, Blend-Mode-grouped compositor).
//
// This is deliberately NOT a textual/structural assertion -- it calls the real, unmodified production
// function with a fixture chosen so the legacy and new algorithms provably DIVERGE, and asserts the
// production function's own return value matches the NEW algorithm, never the OLD one. See the numeric
// derivation in each test's own comment.
//
// Structural corroboration (not the primary evidence, per this checkpoint's own instructions -- see this
// file's own report companion): `grep -rn "ComposeMaskStack\|VertexMaskForgeMaskStackComposer::ComposeStack"
// SVertexMaskForgePanel.cpp` finds zero production call sites after this checkpoint (only comments and the
// now-removed wrapper's own doc-comment history); `grep -rn "ComposeGeneratorLayersSequential"
// SVertexMaskForgePanel.cpp` finds exactly the two real call sites inside ComputeComposedColorsRGB /
// ComputeComposedColorsRGBSourceTopology.

#if WITH_DEV_AUTOMATION_TESTS

#include "DynamicMesh/DynamicMesh3.h"
#include "Misc/AutomationTest.h"
#include "VertexMaskForgeMaskTypes.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

namespace
{
	constexpr float PCI_ByteTolerance = 2.0f; // FColor byte rounding -- generous, exact algorithm is what matters.

	FVertexMaskForgeScalarMask MakeIntegrationMask(const float Value, const EVertexMaskForgeScalarMaskSource Source)
	{
		FVertexMaskForgeScalarMask Mask;
		Mask.State = EVertexMaskForgeScalarMaskState::Ready;
		Mask.Values = { Value };
		Mask.bHasValue.Init(true, 1);
		Mask.NumValidValues = 1;
		Mask.Source = Source;
		return Mask;
	}
}

// A. Render-vertex path: VertexMaskForgePanel::ComputeComposedColorsRGB, called directly (the real
// production function, not a reimplementation), with a Multiply(0.5) layer sorted (by Mask->Source, the
// SAME sort ComputeComposedColorsRGB itself performs internally -- see its own SortedLayers.Sort call)
// BEFORE an Add(0.3) layer. The legacy VertexMaskForgeMaskStackComposer::ComposeStack groups Add and
// Multiply into FIXED stages (Add always resolves before Multiply, REGARDLESS of array/sort order) and
// would produce Base=0.2 -> Add(0.3)=0.5 -> Multiply(0.5)=0.25 no matter what order the layers arrive in.
// The new sequential fold respects strict array order instead: Base=0.2 -> Multiply(0.5) ->
// Lerp(0.2,0.1,1)=0.1 -> Add(0.3) -> Lerp(0.1,0.4,1)=0.4. These two results (0.25 vs 0.4) are provably
// different, so whichever one the REAL production function returns proves which algorithm it actually
// reached.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgePanelCompositionUsesSequentialEvaluatorTest, "VertexMaskForge.PanelCompositionIntegration.RenderVertexUsesSequentialEvaluator", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgePanelCompositionUsesSequentialEvaluatorTest::RunTest(const FString& Parameters)
{
	// Curvature sorts before Noise in EVertexMaskForgeScalarMaskSource's own declaration order -- forces
	// ComputeComposedColorsRGB's own internal Source-sort to place the Multiply layer BEFORE the Add
	// layer, exactly like a real panel entry with both Curvature and Noise enabled would.
	const FVertexMaskForgeScalarMask MultiplyMask = MakeIntegrationMask(0.5f, EVertexMaskForgeScalarMaskSource::Curvature);
	const FVertexMaskForgeScalarMask AddMask = MakeIntegrationMask(0.3f, EVertexMaskForgeScalarMaskSource::Noise);

	TArray<VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams> Layers;
	Layers.Add({ &MultiplyMask, EVertexMaskForgeBlendMode::Multiply, 1.0f, -1 });
	Layers.Add({ &AddMask, EVertexMaskForgeBlendMode::Add, 1.0f, -1 });

	const TArray<FColor> Baseline = { FColor(51, 51, 51, 255) }; // ~0.2 in [0,1].
	const TArray<FColor> Committed = Baseline;
	TArray<FColor> OutFinalColors;
	int32 OutNumComposed = 0;

	// The REAL production function -- not VertexMaskForgeGeneratorLayerBridge called directly, not a
	// hand-rolled fold. This is exactly what ApplyPreviewToEntry itself calls.
	VertexMaskForgePanel::ComputeComposedColorsRGB(
		Baseline, Committed, Layers, /*bFilterR=*/true, /*bFilterG=*/true, /*bFilterB=*/true,
		OutFinalColors, OutNumComposed);

	TestEqual(TEXT("One vertex composed"), OutNumComposed, 1);
	if (TestEqual(TEXT("OutFinalColors sized"), OutFinalColors.Num(), 1))
	{
		// Expected (NEW, sequential, array-order fold): 0.4 -> ~102. Legacy stage-grouped ComposeStack
		// would have produced 0.25 -> ~64 instead -- these are far enough apart (>2 byte units) that the
		// tolerance below cannot accidentally accept the legacy value.
		TestTrue(TEXT("Result matches the NEW sequential fold (~102), not the legacy stage-grouped ComposeStack (~64)"),
			FMath::Abs(static_cast<float>(OutFinalColors[0].R) - 102.0f) <= PCI_ByteTolerance);
	}

	return true;
}

// B. Source-Topology path: VertexMaskForgePanel::ComputeComposedColorsRGBSourceTopology, same divergent
// fixture, proving the corner-domain call site reaches the identical new algorithm too (see item 14 of
// this checkpoint's own required test matrix: "render-vertex e Source-Topology usando a mesma semântica").
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgePanelCompositionSourceTopologyUsesSequentialEvaluatorTest, "VertexMaskForge.PanelCompositionIntegration.SourceTopologyUsesSequentialEvaluator", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgePanelCompositionSourceTopologyUsesSequentialEvaluatorTest::RunTest(const FString& Parameters)
{
	using namespace UE::Geometry;

	// Minimal one-triangle mesh -- ComputeComposedColorsRGBSourceTopology iterates
	// Mesh.TriangleIndicesItr() to drive the corner loop; a single triangle gives exactly 3 corners.
	FDynamicMesh3 Mesh;
	const int32 V0 = Mesh.AppendVertex(FVector3d(0.0, 0.0, 0.0));
	const int32 V1 = Mesh.AppendVertex(FVector3d(1.0, 0.0, 0.0));
	const int32 V2 = Mesh.AppendVertex(FVector3d(0.0, 1.0, 0.0));
	Mesh.AppendTriangle(V0, V1, V2);
	Mesh.EnableAttributes();
	FDynamicMeshNormalOverlay* NormalOverlay = Mesh.Attributes()->PrimaryNormals();
	FIndex3i ElementTri;
	for (int32 Corner = 0; Corner < 3; ++Corner)
	{
		ElementTri[Corner] = NormalOverlay->AppendElement(FVector3f(0.0f, 0.0f, 1.0f));
	}
	NormalOverlay->SetTriangle(0, ElementTri);

	// Same divergent fixture as the render-vertex test above, but with 3 identical corner values (one per
	// corner of the single triangle) since this domain is corner-indexed.
	FVertexMaskForgeScalarMask MultiplyMask = MakeIntegrationMask(0.5f, EVertexMaskForgeScalarMaskSource::Curvature);
	MultiplyMask.Values = { 0.5f, 0.5f, 0.5f };
	MultiplyMask.bHasValue.Init(true, 3);
	FVertexMaskForgeScalarMask AddMask = MakeIntegrationMask(0.3f, EVertexMaskForgeScalarMaskSource::Noise);
	AddMask.Values = { 0.3f, 0.3f, 0.3f };
	AddMask.bHasValue.Init(true, 3);

	TArray<VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams> Layers;
	Layers.Add({ &MultiplyMask, EVertexMaskForgeBlendMode::Multiply, 1.0f, -1 });
	Layers.Add({ &AddMask, EVertexMaskForgeBlendMode::Add, 1.0f, -1 });

	const TArray<FColor> Baseline = { FColor(51, 51, 51, 255), FColor(51, 51, 51, 255), FColor(51, 51, 51, 255) };
	const TArray<FColor> Committed = Baseline;
	TArray<FColor> OutFinalColors;
	int32 OutNumComposed = 0;

	VertexMaskForgePanel::ComputeComposedColorsRGBSourceTopology(
		Baseline, Committed, Layers, Mesh, /*bFilterR=*/true, /*bFilterG=*/true, /*bFilterB=*/true,
		OutFinalColors, OutNumComposed);

	TestEqual(TEXT("Three corners composed"), OutNumComposed, 3);
	if (TestEqual(TEXT("OutFinalColors sized"), OutFinalColors.Num(), 3))
	{
		for (int32 i = 0; i < 3; ++i)
		{
			TestTrue(FString::Printf(TEXT("Corner %d matches the NEW sequential fold (~102), not the legacy ComposeStack (~64)"), i),
				FMath::Abs(static_cast<float>(OutFinalColors[i].R) - 102.0f) <= PCI_ByteTolerance);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
