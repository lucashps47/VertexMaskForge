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
// AUDITED (M16-K.2): both production functions now take an explicit LayerOrder parameter (the panel's
// own persistent generator layer order -- see SVertexMaskForgePanel::GeneratorLayerOrder's own doc
// comment) instead of sorting Layers by Mask->Source internally. Every test below that is NOT itself
// testing reorder passes VertexMaskForgeLayerOrder::MakeDefault() -- proven (tests A/B) to reproduce
// exactly the same numeric results as before this checkpoint. Tests C onward prove the new LayerOrder
// parameter is genuinely load-bearing (not a decorative parameter shadowed by a leftover sort).
//
// Structural corroboration (not the primary evidence, per this checkpoint's own instructions -- see this
// file's own report companion): `grep -rn "ComposeMaskStack\|VertexMaskForgeMaskStackComposer::ComposeStack"
// SVertexMaskForgePanel.cpp` finds zero production call sites after this checkpoint (only comments and the
// now-removed wrapper's own doc-comment history); `grep -rn "\.Sort(" SVertexMaskForgePanel.cpp` finds no
// occurrence inside ComputeComposedColorsRGB/ComputeComposedColorsRGBSourceTopology (M16-K.2 removed both).

#if WITH_DEV_AUTOMATION_TESTS

#include "DynamicMesh/DynamicMesh3.h"
#include "Misc/AutomationTest.h"
#include "VertexMaskForgeLayerOrder.h"
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

	bool ByteNear(const uint8 Actual, const float Expected)
	{
		return FMath::Abs(static_cast<float>(Actual) - Expected) <= PCI_ByteTolerance;
	}
}

// A. Render-vertex path, DEFAULT order: VertexMaskForgePanel::ComputeComposedColorsRGB, called directly
// (the real production function, not a reimplementation), with a Multiply(0.5) layer (Curvature) and an
// Add(0.3) layer (Noise). MakeDefault() places Curvature before Noise (same relative order the OLD
// Sort()-by-Mask->Source already produced, since Curvature's enum value is lower) -- proving the default
// order reproduces byte-identical results to before this checkpoint. Base=0.2 -> Multiply(0.5) ->
// Lerp(0.2,0.1,1)=0.1 -> Add(0.3) -> Lerp(0.1,0.4,1)=0.4 -> ~102.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgePanelCompositionUsesSequentialEvaluatorTest, "VertexMaskForge.PanelCompositionIntegration.RenderVertexUsesSequentialEvaluator", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgePanelCompositionUsesSequentialEvaluatorTest::RunTest(const FString& Parameters)
{
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
		Baseline, Committed, Layers, VertexMaskForgeLayerOrder::MakeDefault(),
		/*bFilterR=*/true, /*bFilterG=*/true, /*bFilterB=*/true,
		OutFinalColors, OutNumComposed);

	TestEqual(TEXT("One vertex composed"), OutNumComposed, 1);
	if (TestEqual(TEXT("OutFinalColors sized"), OutFinalColors.Num(), 1))
	{
		TestTrue(TEXT("Default order matches the pre-M16-K.2 result exactly (~102)"), ByteNear(OutFinalColors[0].R, 102.0f));
	}

	return true;
}

// B. Source-Topology path, DEFAULT order: same divergent fixture, proving the corner-domain call site
// reaches the identical result under the default order too.
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
		Baseline, Committed, Layers, VertexMaskForgeLayerOrder::MakeDefault(), Mesh,
		/*bFilterR=*/true, /*bFilterG=*/true, /*bFilterB=*/true,
		OutFinalColors, OutNumComposed);

	TestEqual(TEXT("Three corners composed"), OutNumComposed, 3);
	if (TestEqual(TEXT("OutFinalColors sized"), OutFinalColors.Num(), 3))
	{
		for (int32 i = 0; i < 3; ++i)
		{
			TestTrue(FString::Printf(TEXT("Corner %d matches the default-order result (~102)"), i), ByteNear(OutFinalColors[i].R, 102.0f));
		}
	}

	return true;
}

// C. Render-vertex path, NON-DEFAULT cross-mode order: same Curvature(Multiply,0.5)/Noise(Add,0.3)
// fixture as test A, but LayerOrder places Noise BEFORE Curvature -- the OPPOSITE of both the default
// order and of EVertexMaskForgeScalarMaskSource's own enum-declaration order (Curvature=4 < Noise=5).
// Base=0.2 -> Add(0.3) -> Lerp(0.2,0.5,1)=0.5 -> Multiply(0.5) -> Lerp(0.5,0.25,1)=0.25 -> ~64. This is
// the value the OLD stage-grouped ComposeStack would always have produced for THIS pair regardless of
// array order (Add always resolved before Multiply, a fixed stage) -- reached here instead purely
// because LayerOrder was told Noise comes first; if the production path silently fell back to any
// Source-numeric sort, this test would see ~102 (Curvature first) instead and fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgePanelCompositionNonDefaultOrderRenderVertexTest, "VertexMaskForge.PanelCompositionIntegration.NonDefaultCrossModeOrderChangesRenderVertexResult", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgePanelCompositionNonDefaultOrderRenderVertexTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeScalarMask MultiplyMask = MakeIntegrationMask(0.5f, EVertexMaskForgeScalarMaskSource::Curvature);
	const FVertexMaskForgeScalarMask AddMask = MakeIntegrationMask(0.3f, EVertexMaskForgeScalarMaskSource::Noise);

	TArray<VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams> Layers;
	Layers.Add({ &MultiplyMask, EVertexMaskForgeBlendMode::Multiply, 1.0f, -1 });
	Layers.Add({ &AddMask, EVertexMaskForgeBlendMode::Add, 1.0f, -1 });

	// A full, valid 7-element order with Noise BEFORE Curvature (reversed relative to default/enum order).
	const TArray<EVertexMaskForgeScalarMaskSource> ReorderedLayerOrder = {
		EVertexMaskForgeScalarMaskSource::BoundingBox, EVertexMaskForgeScalarMaskSource::AmbientOcclusion,
		EVertexMaskForgeScalarMaskSource::Noise, EVertexMaskForgeScalarMaskSource::Curvature,
		EVertexMaskForgeScalarMaskSource::MaterialSlot, EVertexMaskForgeScalarMaskSource::DirectionalNormal,
		EVertexMaskForgeScalarMaskSource::Thickness
	};
	TestTrue(TEXT("Sanity: ReorderedLayerOrder is a valid permutation"), VertexMaskForgeLayerOrder::IsValid(ReorderedLayerOrder));

	const TArray<FColor> Baseline = { FColor(51, 51, 51, 255) };
	const TArray<FColor> Committed = Baseline;
	TArray<FColor> OutFinalColors;
	int32 OutNumComposed = 0;

	VertexMaskForgePanel::ComputeComposedColorsRGB(
		Baseline, Committed, Layers, ReorderedLayerOrder,
		/*bFilterR=*/true, /*bFilterG=*/true, /*bFilterB=*/true,
		OutFinalColors, OutNumComposed);

	TestEqual(TEXT("One vertex composed"), OutNumComposed, 1);
	if (TestEqual(TEXT("OutFinalColors sized"), OutFinalColors.Num(), 1))
	{
		TestTrue(TEXT("Non-default order (Noise before Curvature) produces ~64, not the default's ~102"), ByteNear(OutFinalColors[0].R, 64.0f));
	}

	return true;
}

// D. Source-Topology sibling of test C -- same reordered LayerOrder, same divergent proof, corner domain.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgePanelCompositionNonDefaultOrderSourceTopologyTest, "VertexMaskForge.PanelCompositionIntegration.NonDefaultCrossModeOrderChangesSourceTopologyResult", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgePanelCompositionNonDefaultOrderSourceTopologyTest::RunTest(const FString& Parameters)
{
	using namespace UE::Geometry;

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

	FVertexMaskForgeScalarMask MultiplyMask = MakeIntegrationMask(0.5f, EVertexMaskForgeScalarMaskSource::Curvature);
	MultiplyMask.Values = { 0.5f, 0.5f, 0.5f };
	MultiplyMask.bHasValue.Init(true, 3);
	FVertexMaskForgeScalarMask AddMask = MakeIntegrationMask(0.3f, EVertexMaskForgeScalarMaskSource::Noise);
	AddMask.Values = { 0.3f, 0.3f, 0.3f };
	AddMask.bHasValue.Init(true, 3);

	TArray<VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams> Layers;
	Layers.Add({ &MultiplyMask, EVertexMaskForgeBlendMode::Multiply, 1.0f, -1 });
	Layers.Add({ &AddMask, EVertexMaskForgeBlendMode::Add, 1.0f, -1 });

	const TArray<EVertexMaskForgeScalarMaskSource> ReorderedLayerOrder = {
		EVertexMaskForgeScalarMaskSource::BoundingBox, EVertexMaskForgeScalarMaskSource::AmbientOcclusion,
		EVertexMaskForgeScalarMaskSource::Noise, EVertexMaskForgeScalarMaskSource::Curvature,
		EVertexMaskForgeScalarMaskSource::MaterialSlot, EVertexMaskForgeScalarMaskSource::DirectionalNormal,
		EVertexMaskForgeScalarMaskSource::Thickness
	};

	const TArray<FColor> Baseline = { FColor(51, 51, 51, 255), FColor(51, 51, 51, 255), FColor(51, 51, 51, 255) };
	const TArray<FColor> Committed = Baseline;
	TArray<FColor> OutFinalColors;
	int32 OutNumComposed = 0;

	VertexMaskForgePanel::ComputeComposedColorsRGBSourceTopology(
		Baseline, Committed, Layers, ReorderedLayerOrder, Mesh,
		/*bFilterR=*/true, /*bFilterG=*/true, /*bFilterB=*/true,
		OutFinalColors, OutNumComposed);

	TestEqual(TEXT("Three corners composed"), OutNumComposed, 3);
	if (TestEqual(TEXT("OutFinalColors sized"), OutFinalColors.Num(), 3))
	{
		for (int32 i = 0; i < 3; ++i)
		{
			TestTrue(FString::Printf(TEXT("Corner %d: non-default order produces ~64, not the default's ~102"), i), ByteNear(OutFinalColors[i].R, 64.0f));
		}
	}

	return true;
}

// E. Both paths receive the SAME LayerOrder and both respect it identically -- render-vertex (1 sample)
// and Source-Topology (3 corners, all fed the same per-corner values) fed the exact same reordered
// LayerOrder from tests C/D produce the SAME semantic result (~64), proving neither path has its own,
// independently-derived notion of order.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgePanelCompositionBothPathsSameOrderTest, "VertexMaskForge.PanelCompositionIntegration.BothPathsReceiveSameOrder", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgePanelCompositionBothPathsSameOrderTest::RunTest(const FString& Parameters)
{
	using namespace UE::Geometry;

	const TArray<EVertexMaskForgeScalarMaskSource> SharedOrder = {
		EVertexMaskForgeScalarMaskSource::BoundingBox, EVertexMaskForgeScalarMaskSource::AmbientOcclusion,
		EVertexMaskForgeScalarMaskSource::Noise, EVertexMaskForgeScalarMaskSource::Curvature,
		EVertexMaskForgeScalarMaskSource::MaterialSlot, EVertexMaskForgeScalarMaskSource::DirectionalNormal,
		EVertexMaskForgeScalarMaskSource::Thickness
	};

	float RenderVertexResult = 0.0f;
	{
		const FVertexMaskForgeScalarMask MultiplyMask = MakeIntegrationMask(0.5f, EVertexMaskForgeScalarMaskSource::Curvature);
		const FVertexMaskForgeScalarMask AddMask = MakeIntegrationMask(0.3f, EVertexMaskForgeScalarMaskSource::Noise);
		TArray<VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams> Layers;
		Layers.Add({ &MultiplyMask, EVertexMaskForgeBlendMode::Multiply, 1.0f, -1 });
		Layers.Add({ &AddMask, EVertexMaskForgeBlendMode::Add, 1.0f, -1 });

		const TArray<FColor> Baseline = { FColor(51, 51, 51, 255) };
		TArray<FColor> OutFinalColors;
		int32 OutNumComposed = 0;
		VertexMaskForgePanel::ComputeComposedColorsRGB(Baseline, Baseline, Layers, SharedOrder, true, true, true, OutFinalColors, OutNumComposed);
		RenderVertexResult = OutFinalColors.IsValidIndex(0) ? OutFinalColors[0].R : -1.0f;
	}

	float SourceTopologyResult = 0.0f;
	{
		FDynamicMesh3 Mesh;
		const int32 V0 = Mesh.AppendVertex(FVector3d(0.0, 0.0, 0.0));
		const int32 V1 = Mesh.AppendVertex(FVector3d(1.0, 0.0, 0.0));
		const int32 V2 = Mesh.AppendVertex(FVector3d(0.0, 1.0, 0.0));
		Mesh.AppendTriangle(V0, V1, V2);
		Mesh.EnableAttributes();
		FDynamicMeshNormalOverlay* NormalOverlay = Mesh.Attributes()->PrimaryNormals();
		FIndex3i ElementTri;
		for (int32 Corner = 0; Corner < 3; ++Corner) { ElementTri[Corner] = NormalOverlay->AppendElement(FVector3f(0.0f, 0.0f, 1.0f)); }
		NormalOverlay->SetTriangle(0, ElementTri);

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
		TArray<FColor> OutFinalColors;
		int32 OutNumComposed = 0;
		VertexMaskForgePanel::ComputeComposedColorsRGBSourceTopology(Baseline, Baseline, Layers, SharedOrder, Mesh, true, true, true, OutFinalColors, OutNumComposed);
		SourceTopologyResult = OutFinalColors.IsValidIndex(0) ? OutFinalColors[0].R : -1.0f;
	}

	TestTrue(TEXT("Render-vertex path respects SharedOrder (~64)"), ByteNear(static_cast<uint8>(RenderVertexResult), 64.0f));
	TestTrue(TEXT("Source-Topology path respects the SAME SharedOrder (~64)"), ByteNear(static_cast<uint8>(SourceTopologyResult), 64.0f));
	TestTrue(TEXT("Both paths agree, within tolerance, on one shared order"), FMath::Abs(RenderVertexResult - SourceTopologyResult) <= PCI_ByteTolerance);

	return true;
}

// F. Identity association preserved after reorder: 3 generators (BoundingBox=Copy 0.9, Curvature=Multiply
// 0.5, Thickness=Add 0.2), each with a DIFFERENT Blend Mode/value pair so any cross-wiring of identity
// during resolution would produce a detectably wrong number. Order 1 (default-relative:
// BoundingBox, Curvature, Thickness): Base=0.2 -> Copy(0.9)=0.9 -> Multiply(0.5)=0.45 ->
// Add(0.2)=Lerp(0.45,0.65,1)=0.65 -> ~166. Order 2 (reversed: Thickness, Curvature, BoundingBox):
// Base=0.2 -> Add(0.2)=0.4 -> Multiply(0.5)=Lerp(0.4,0.2,1)=0.2 -> Copy(0.9)=0.9 -> ~230. If resolution
// ever mismatched a Source to the WRONG BlendMode/Opacity/MaskValue, at least one of these two would not
// match its independently-derived expectation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgePanelCompositionIdentityPreservedTest, "VertexMaskForge.PanelCompositionIntegration.IdentityAssociationPreservedAfterReorder", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgePanelCompositionIdentityPreservedTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeScalarMask BBoxMask = MakeIntegrationMask(0.9f, EVertexMaskForgeScalarMaskSource::BoundingBox);
	const FVertexMaskForgeScalarMask CurvatureMask = MakeIntegrationMask(0.5f, EVertexMaskForgeScalarMaskSource::Curvature);
	const FVertexMaskForgeScalarMask ThicknessMask = MakeIntegrationMask(0.2f, EVertexMaskForgeScalarMaskSource::Thickness);

	TArray<VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams> Layers;
	Layers.Add({ &BBoxMask, EVertexMaskForgeBlendMode::Copy, 1.0f, -1 });
	Layers.Add({ &CurvatureMask, EVertexMaskForgeBlendMode::Multiply, 1.0f, -1 });
	Layers.Add({ &ThicknessMask, EVertexMaskForgeBlendMode::Add, 1.0f, -1 });

	const TArray<FColor> Baseline = { FColor(51, 51, 51, 255) };

	auto RunWithOrder = [&](const TArray<EVertexMaskForgeScalarMaskSource>& Order) -> uint8
	{
		TArray<FColor> OutFinalColors;
		int32 OutNumComposed = 0;
		VertexMaskForgePanel::ComputeComposedColorsRGB(Baseline, Baseline, Layers, Order, true, true, true, OutFinalColors, OutNumComposed);
		return OutFinalColors.IsValidIndex(0) ? OutFinalColors[0].R : 0;
	};

	const uint8 DefaultOrderResult = RunWithOrder(VertexMaskForgeLayerOrder::MakeDefault());
	TestTrue(TEXT("Default-relative order (BoundingBox, Curvature, Thickness) -> ~166"), ByteNear(DefaultOrderResult, 166.0f));

	const TArray<EVertexMaskForgeScalarMaskSource> ReversedOrder = {
		EVertexMaskForgeScalarMaskSource::Thickness, EVertexMaskForgeScalarMaskSource::DirectionalNormal,
		EVertexMaskForgeScalarMaskSource::MaterialSlot, EVertexMaskForgeScalarMaskSource::Curvature,
		EVertexMaskForgeScalarMaskSource::Noise, EVertexMaskForgeScalarMaskSource::AmbientOcclusion,
		EVertexMaskForgeScalarMaskSource::BoundingBox
	};
	const uint8 ReversedOrderResult = RunWithOrder(ReversedOrder);
	TestTrue(TEXT("Reversed order (Thickness, Curvature, BoundingBox) -> ~230, each identity kept its own Mask/BlendMode"), ByteNear(ReversedOrderResult, 230.0f));

	return true;
}

// G. A generator absent from Layers (the ApplyPreviewToEntry contract for "disabled or not Ready" --
// see ResolveLayersInPersistentOrder's own doc comment) never contributes, even though its own identity
// remains present in the full, valid LayerOrder passed alongside it. The remaining layers keep their own
// relative order. Curvature is absent from Layers here (simulating disabled) while still present in
// LayerOrder. Base=0.2 -> BoundingBox Copy(0.7)=0.7 -> Thickness Add(0.2)=Lerp(0.7,0.9,1)=0.9 -> ~230.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgePanelCompositionDisabledLayerTest, "VertexMaskForge.PanelCompositionIntegration.DisabledLayerRemainsOrderedButDoesNotContribute", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgePanelCompositionDisabledLayerTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeScalarMask BBoxMask = MakeIntegrationMask(0.7f, EVertexMaskForgeScalarMaskSource::BoundingBox);
	const FVertexMaskForgeScalarMask ThicknessMask = MakeIntegrationMask(0.2f, EVertexMaskForgeScalarMaskSource::Thickness);

	// Curvature is deliberately NOT added to Layers -- exactly what ApplyPreviewToEntry does for a
	// disabled or not-Ready generator. LayerOrder is still the full, valid default order, WITH Curvature
	// present in it.
	TArray<VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams> Layers;
	Layers.Add({ &BBoxMask, EVertexMaskForgeBlendMode::Copy, 1.0f, -1 });
	Layers.Add({ &ThicknessMask, EVertexMaskForgeBlendMode::Add, 1.0f, -1 });

	const TArray<EVertexMaskForgeScalarMaskSource> FullOrder = VertexMaskForgeLayerOrder::MakeDefault();
	TestTrue(TEXT("Curvature is present in LayerOrder"), FullOrder.Contains(EVertexMaskForgeScalarMaskSource::Curvature));

	const TArray<FColor> Baseline = { FColor(51, 51, 51, 255) };
	TArray<FColor> OutFinalColors;
	int32 OutNumComposed = 0;
	VertexMaskForgePanel::ComputeComposedColorsRGB(Baseline, Baseline, Layers, FullOrder, true, true, true, OutFinalColors, OutNumComposed);

	TestEqual(TEXT("One vertex composed"), OutNumComposed, 1);
	if (TestEqual(TEXT("OutFinalColors sized"), OutFinalColors.Num(), 1))
	{
		TestTrue(TEXT("Curvature's absence from Layers means no contribution -- result reflects only BoundingBox+Thickness (~230)"),
			ByteNear(OutFinalColors[0].R, 230.0f));
	}

	return true;
}

// H. An invalid LayerOrder (wrong count, or a full-count array with a duplicate) is rejected explicitly
// and safely -- no generator layer is composed at all for that call (no partial/best-effort reorder, no
// crash); a strongly-contributing BoundingBox layer that WOULD dominate the result under a valid order
// is confirmed absent from the output, which instead falls back to Committed verbatim.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgePanelCompositionInvalidOrderTest, "VertexMaskForge.PanelCompositionIntegration.InvalidOrderRejected", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgePanelCompositionInvalidOrderTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeScalarMask BBoxMask = MakeIntegrationMask(0.9f, EVertexMaskForgeScalarMaskSource::BoundingBox);
	TArray<VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams> Layers;
	Layers.Add({ &BBoxMask, EVertexMaskForgeBlendMode::Copy, 1.0f, -1 });

	const TArray<FColor> Baseline = { FColor(51, 51, 51, 255) };
	const TArray<FColor> Committed = { FColor(80, 80, 80, 255) }; // Distinguishable from Baseline.

	// Wrong count (6 elements).
	{
		TArray<EVertexMaskForgeScalarMaskSource> ShortOrder = VertexMaskForgeLayerOrder::MakeDefault();
		ShortOrder.RemoveAt(0);
		TestFalse(TEXT("Sanity: 6-element order is invalid"), VertexMaskForgeLayerOrder::IsValid(ShortOrder));

		TArray<FColor> OutFinalColors;
		int32 OutNumComposed = 0;
		VertexMaskForgePanel::ComputeComposedColorsRGB(Baseline, Committed, Layers, ShortOrder, true, true, true, OutFinalColors, OutNumComposed);

		TestEqual(TEXT("Wrong-count order: zero vertices composed"), OutNumComposed, 0);
		if (TestEqual(TEXT("OutFinalColors sized"), OutFinalColors.Num(), 1))
		{
			TestEqual(TEXT("Wrong-count order: falls back to Committed verbatim, BoundingBox never contributed"), OutFinalColors[0].R, Committed[0].R);
		}
	}

	// Duplicate (7 elements, BoundingBox twice, Thickness missing).
	{
		const TArray<EVertexMaskForgeScalarMaskSource> DuplicateOrder = {
			EVertexMaskForgeScalarMaskSource::BoundingBox, EVertexMaskForgeScalarMaskSource::AmbientOcclusion,
			EVertexMaskForgeScalarMaskSource::Curvature, EVertexMaskForgeScalarMaskSource::Noise,
			EVertexMaskForgeScalarMaskSource::MaterialSlot, EVertexMaskForgeScalarMaskSource::DirectionalNormal,
			EVertexMaskForgeScalarMaskSource::BoundingBox
		};
		TestFalse(TEXT("Sanity: duplicate order is invalid"), VertexMaskForgeLayerOrder::IsValid(DuplicateOrder));

		TArray<FColor> OutFinalColors;
		int32 OutNumComposed = 0;
		VertexMaskForgePanel::ComputeComposedColorsRGB(Baseline, Committed, Layers, DuplicateOrder, true, true, true, OutFinalColors, OutNumComposed);

		TestEqual(TEXT("Duplicate order: zero vertices composed"), OutNumComposed, 0);
		if (TestEqual(TEXT("OutFinalColors sized"), OutFinalColors.Num(), 1))
		{
			TestEqual(TEXT("Duplicate order: falls back to Committed verbatim"), OutFinalColors[0].R, Committed[0].R);
		}
	}

	return true;
}

// I. Default is not a numeric-sort dependency: AmbientOcclusion (enum value 3) and MaterialSlot (enum
// value 6) are given a LayerOrder placing MaterialSlot BEFORE AmbientOcclusion -- the OPPOSITE of what
// any Mask->Source-numeric sort (the removed Sort() lambda's own comparator) would ever produce,
// regardless of array order, since 6 > 3 always. Base=0.2 -> MaterialSlot Add(0.3)=Lerp(0.2,0.5,1)=0.5
// -> AmbientOcclusion Multiply(0.5)=Lerp(0.5,0.25,1)=0.25 -> ~64. A residual numeric-sort comparator
// anywhere in the resolution path would force AmbientOcclusion first regardless (giving ~102 instead --
// see the enum-ascending-order companion value in test A/C's own fixture), so ~64 here is possible only
// if LayerOrder is genuinely authoritative.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgePanelCompositionNotNumericSortTest, "VertexMaskForge.PanelCompositionIntegration.DefaultIsNotNumericSortDependency", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgePanelCompositionNotNumericSortTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeScalarMask AOMask = MakeIntegrationMask(0.5f, EVertexMaskForgeScalarMaskSource::AmbientOcclusion);
	const FVertexMaskForgeScalarMask MaterialSlotMask = MakeIntegrationMask(0.3f, EVertexMaskForgeScalarMaskSource::MaterialSlot);

	TArray<VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams> Layers;
	Layers.Add({ &AOMask, EVertexMaskForgeBlendMode::Multiply, 1.0f, -1 });
	Layers.Add({ &MaterialSlotMask, EVertexMaskForgeBlendMode::Add, 1.0f, -1 });

	const TArray<EVertexMaskForgeScalarMaskSource> MaterialSlotFirstOrder = {
		EVertexMaskForgeScalarMaskSource::BoundingBox, EVertexMaskForgeScalarMaskSource::MaterialSlot,
		EVertexMaskForgeScalarMaskSource::Curvature, EVertexMaskForgeScalarMaskSource::Noise,
		EVertexMaskForgeScalarMaskSource::AmbientOcclusion, EVertexMaskForgeScalarMaskSource::DirectionalNormal,
		EVertexMaskForgeScalarMaskSource::Thickness
	};
	TestTrue(TEXT("Sanity: MaterialSlotFirstOrder is a valid permutation"), VertexMaskForgeLayerOrder::IsValid(MaterialSlotFirstOrder));

	const TArray<FColor> Baseline = { FColor(51, 51, 51, 255) };
	TArray<FColor> OutFinalColors;
	int32 OutNumComposed = 0;
	VertexMaskForgePanel::ComputeComposedColorsRGB(Baseline, Baseline, Layers, MaterialSlotFirstOrder, true, true, true, OutFinalColors, OutNumComposed);

	TestEqual(TEXT("One vertex composed"), OutNumComposed, 1);
	if (TestEqual(TEXT("OutFinalColors sized"), OutFinalColors.Num(), 1))
	{
		TestTrue(TEXT("MaterialSlot-before-AmbientOcclusion (higher enum value first) -> ~64, proving no numeric-sort remnant"),
			ByteNear(OutFinalColors[0].R, 64.0f));
	}

	return true;
}

// J. Homogeneous-mode reorder is NOT required to change the result (not primary evidence of the
// connection -- tests C/D/I already prove that; this documents the companion, non-obligatory case per
// this checkpoint's own instructions). Two Add-mode layers: Base=0.2 -> Add(0.1) -> Add(0.2), in either
// order, both resolve to 0.5 (Add's own Lerp form is associative for two full-opacity steps on the same
// starting Base). This test would NOT be valid evidence that reorder is connected to production (an
// unconnected/no-op path would also report equal results) -- it exists only to document that a
// same-result reorder is a legitimate, expected outcome for this specific homogeneous case, not a defect.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgePanelCompositionHomogeneousModesTest, "VertexMaskForge.PanelCompositionIntegration.HomogeneousModesMayRemainEqual", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgePanelCompositionHomogeneousModesTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeScalarMask BBoxMask = MakeIntegrationMask(0.1f, EVertexMaskForgeScalarMaskSource::BoundingBox);
	const FVertexMaskForgeScalarMask CurvatureMask = MakeIntegrationMask(0.2f, EVertexMaskForgeScalarMaskSource::Curvature);

	TArray<VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams> Layers;
	Layers.Add({ &BBoxMask, EVertexMaskForgeBlendMode::Add, 1.0f, -1 });
	Layers.Add({ &CurvatureMask, EVertexMaskForgeBlendMode::Add, 1.0f, -1 });

	const TArray<FColor> Baseline = { FColor(51, 51, 51, 255) };

	auto RunWithOrder = [&](const TArray<EVertexMaskForgeScalarMaskSource>& Order) -> uint8
	{
		TArray<FColor> OutFinalColors;
		int32 OutNumComposed = 0;
		VertexMaskForgePanel::ComputeComposedColorsRGB(Baseline, Baseline, Layers, Order, true, true, true, OutFinalColors, OutNumComposed);
		return OutFinalColors.IsValidIndex(0) ? OutFinalColors[0].R : 0;
	};

	const uint8 DefaultResult = RunWithOrder(VertexMaskForgeLayerOrder::MakeDefault()); // BoundingBox, then Curvature.
	const TArray<EVertexMaskForgeScalarMaskSource> ReversedOrder = {
		EVertexMaskForgeScalarMaskSource::Curvature, EVertexMaskForgeScalarMaskSource::AmbientOcclusion,
		EVertexMaskForgeScalarMaskSource::BoundingBox, EVertexMaskForgeScalarMaskSource::Noise,
		EVertexMaskForgeScalarMaskSource::MaterialSlot, EVertexMaskForgeScalarMaskSource::DirectionalNormal,
		EVertexMaskForgeScalarMaskSource::Thickness
	};
	const uint8 ReversedResult = RunWithOrder(ReversedOrder); // Curvature, then BoundingBox.

	TestTrue(TEXT("Both orders reach 0.5 (~127-128)"), ByteNear(DefaultResult, 127.5f) && ByteNear(ReversedResult, 127.5f));
	TestTrue(TEXT("Homogeneous Add-mode reorder is not obligated to differ (documented, not primary evidence)"), DefaultResult == ReversedResult);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
