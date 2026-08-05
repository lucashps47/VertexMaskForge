// M16-K.6D-4: proves VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology
// -- the new, testable Dynamic Source-Topology composition orchestrator -- directly. See
// VertexMaskForgeDynamicSourceTopologyComposition.h for the full contract these tests exercise.
//
// No production caller exists yet (M16-K.6D-5, not this checkpoint, connects this to preview); these
// tests are this function's only caller anywhere in the codebase.

#if WITH_DEV_AUTOMATION_TESTS

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Misc/AutomationTest.h"
#include "VertexMaskForgeAmbientOcclusionGenerator.h"
#include "VertexMaskForgeBoundingBoxGenerator.h"
#include "VertexMaskForgeCurvatureGenerator.h"
#include "VertexMaskForgeDirectionalNormalGenerator.h"
#include "VertexMaskForgeDynamicLayerStack.h"
#include "VertexMaskForgeDynamicSourceTopologyComposition.h"
#include "VertexMaskForgeLayerTypes.h"
#include "VertexMaskForgeNoiseGenerator.h"
#include "VertexMaskForgeRecipeTypes.h"
#include "VertexMaskForgeThicknessGenerator.h"
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

	// M17-TH-DL-B: irregular (hand-placed, fully asymmetric) octahedron -- the same class of fixture already
	// proven, in the retained Thickness backend diagnostic itself, to give a clean, non-degenerate,
	// EVERY-corner-accepted (24/24) result via the isolated Legacy center ray ALONE, no fallback needed --
	// asymmetric vertex positions avoid the exact-axis-symmetry grazing failure a REGULAR octahedron would
	// hit. Independently reconstructed here (never included/copied from the diagnostic file) purely as
	// numeric vertex coordinates, for a dense (every-corner-valid) Thickness fixture this orchestrator's
	// strict Corner-domain contract requires (NumValidValues == ExpectedCornerCount or the WHOLE call
	// fails -- see ComputeComposedColorsRGBSourceTopology's own Thickness branch). 6 vertices, 8 triangles,
	// 24 corners, vertex-averaged (smooth) normals -- each corner's straight-in ray reaches a genuine,
	// non-grazing interior point of one of the 4 non-incident opposite-side triangles.
	FVertexMaskForgeWorkingMesh BuildIrregularOctahedronThicknessFixtureWorkingMesh()
	{
		FVertexMaskForgeWorkingMesh WorkingMesh;
		WorkingMesh.Mesh = MakeUnique<UE::Geometry::FDynamicMesh3>();
		UE::Geometry::FDynamicMesh3& Mesh = *WorkingMesh.Mesh;
		const int32 V0 = Mesh.AppendVertex(FVector3d(12.0, 0.3, 0.7));
		const int32 V1 = Mesh.AppendVertex(FVector3d(-9.0, 0.4, 0.2));
		const int32 V2 = Mesh.AppendVertex(FVector3d(0.6, 11.0, 0.5));
		const int32 V3 = Mesh.AppendVertex(FVector3d(0.4, -8.0, 0.9));
		const int32 V4 = Mesh.AppendVertex(FVector3d(0.8, 0.6, 13.0));
		const int32 V5 = Mesh.AppendVertex(FVector3d(0.2, 0.9, -10.0));
		Mesh.AppendTriangle(V0, V4, V2);
		Mesh.AppendTriangle(V2, V4, V1);
		Mesh.AppendTriangle(V1, V4, V3);
		Mesh.AppendTriangle(V3, V4, V0);
		Mesh.AppendTriangle(V0, V5, V3);
		Mesh.AppendTriangle(V3, V5, V1);
		Mesh.AppendTriangle(V1, V5, V2);
		Mesh.AppendTriangle(V2, V5, V0);

		Mesh.EnableAttributes();
		UE::Geometry::FDynamicMeshNormalOverlay* NormalOverlay = Mesh.Attributes()->PrimaryNormals();
		TArray<int32> VertexToElement;
		VertexToElement.Init(INDEX_NONE, Mesh.MaxVertexID());
		for (const int32 VertexID : Mesh.VertexIndicesItr())
		{
			FVector3d AveragedNormal = FVector3d::ZeroVector;
			int32 NumIncidentTriangles = 0;
			for (const int32 TriangleID : Mesh.VtxTrianglesItr(VertexID))
			{
				AveragedNormal += Mesh.GetTriNormal(TriangleID);
				++NumIncidentTriangles;
			}
			if (NumIncidentTriangles > 0)
			{
				AveragedNormal /= static_cast<double>(NumIncidentTriangles);
				AveragedNormal.Normalize();
			}
			VertexToElement[VertexID] = NormalOverlay->AppendElement(FVector3f(AveragedNormal));
		}
		for (const int32 TriangleID : Mesh.TriangleIndicesItr())
		{
			const UE::Geometry::FIndex3i Tri = Mesh.GetTriangle(TriangleID);
			NormalOverlay->SetTriangle(TriangleID,
				UE::Geometry::FIndex3i(VertexToElement[Tri.A], VertexToElement[Tri.B], VertexToElement[Tri.C]));
		}

		WorkingMesh.GeometryFingerprint = 200;
		return WorkingMesh;
	}

	// Adds a layer with a Thickness mask, configured via the stack's own controlled mutators -- mirrors
	// AddAmbientOcclusionLayer's own setup sequence exactly.
	FGuid AddThicknessLayer(
		FVertexMaskForgeDynamicLayerStack& Stack, const FString& Name,
		EVertexMaskForgeLayerFill Fill, EVertexMaskForgeBlendMode BlendMode, float Opacity,
		float MinThickness, float MaxThickness, float SearchDistance, bool bInvert)
	{
		const FGuid LayerId = Stack.AddLayer(Name);
		Stack.SetLayerFill(LayerId, Fill);
		Stack.SetLayerBlendMode(LayerId, BlendMode);
		Stack.SetLayerOpacity(LayerId, Opacity);
		Stack.SetLayerMaskGeneratorType(LayerId, EVertexMaskForgeGeneratorType::Thickness);

		const FVertexMaskForgeGeneratorMaskInstance* MaskInstance = Stack.GetLayerMask(LayerId);
		check(MaskInstance);
		FVertexMaskForgeGeneratorParams NewParams = MakeVertexMaskForgeGeneratorParams(EVertexMaskForgeGeneratorType::Thickness);
		FVertexMaskForgeThicknessParams& ThicknessParams = NewParams.Get<FVertexMaskForgeThicknessParams>();
		ThicknessParams.MinThickness = MinThickness;
		ThicknessParams.MaxThickness = MaxThickness;
		ThicknessParams.SearchDistance = SearchDistance;
		ThicknessParams.bInvert = bInvert;
		Stack.SetLayerMaskParams(LayerId, MaskInstance->MaskInstanceId, NewParams);

		return LayerId;
	}

	// Test-local only (never a production seam) -- copies LayerId's CURRENT Thickness params (byte/value-
	// exact), applies Mutator, writes back through the stack's own existing controlled API in ONE call,
	// mirroring MutateAmbientOcclusionParams' own established pattern exactly.
	void MutateThicknessParams(FVertexMaskForgeDynamicLayerStack& Stack, const FGuid& LayerId, TFunctionRef<void(FVertexMaskForgeThicknessParams&)> Mutator)
	{
		const FVertexMaskForgeGeneratorMaskInstance* MaskInstance = Stack.GetLayerMask(LayerId);
		check(MaskInstance && MaskInstance->Params.IsType<FVertexMaskForgeThicknessParams>());
		FVertexMaskForgeGeneratorParams NewParams = MaskInstance->Params;
		Mutator(NewParams.Get<FVertexMaskForgeThicknessParams>());
		Stack.SetLayerMaskParams(LayerId, MaskInstance->MaskInstanceId, NewParams);
	}

	// M17-TH-DL-B: a single, isolated, flat 2-triangle quad, WITH a valid Normal Overlay (so
	// GenerateThicknessMaskFromDynamicMesh reaches EVertexMaskForgeScalarMaskState::Ready, never the
	// earlier Unavailable early-out), but with NOTHING else in the mesh -- the center ray AND all 8
	// fallback cone rays alike find no opposing geometry at all, a genuine, total per-corner no-hit within
	// an otherwise-Ready mask (NumValidValues==0), exactly the sparse-mask failure case this orchestrator's
	// Corner-domain contract must preserve.
	FVertexMaskForgeWorkingMesh BuildIsolatedFlatQuadThicknessNoHitFixtureWorkingMesh()
	{
		FVertexMaskForgeWorkingMesh WorkingMesh;
		WorkingMesh.Mesh = MakeUnique<UE::Geometry::FDynamicMesh3>();
		UE::Geometry::FDynamicMesh3& Mesh = *WorkingMesh.Mesh;
		const int32 V0 = Mesh.AppendVertex(FVector3d(0.0, 0.0, 0.0));
		const int32 V1 = Mesh.AppendVertex(FVector3d(1.0, 0.0, 0.0));
		const int32 V2 = Mesh.AppendVertex(FVector3d(1.0, 1.0, 0.0));
		const int32 V3 = Mesh.AppendVertex(FVector3d(0.0, 1.0, 0.0));
		Mesh.AppendTriangle(V0, V1, V2);
		Mesh.AppendTriangle(V0, V2, V3);

		Mesh.EnableAttributes();
		UE::Geometry::FDynamicMeshNormalOverlay* NormalOverlay = Mesh.Attributes()->PrimaryNormals();
		for (const int32 TriangleID : Mesh.TriangleIndicesItr())
		{
			const FVector3f N(0.0f, 0.0f, 1.0f);
			UE::Geometry::FIndex3i ElementTri;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				ElementTri[Corner] = NormalOverlay->AppendElement(N);
			}
			NormalOverlay->SetTriangle(TriangleID, ElementTri);
		}

		WorkingMesh.GeometryFingerprint = 201;
		return WorkingMesh;
	}

	// M17-TH-DL-C: minimal deterministic fixture proving the M9 conservative fallback (VertexMaskForgeThicknessGenerator.cpp:
	// ThicknessFallbackConeAngleDegrees=30, ThicknessFallbackNumSecondaryRays=8, EvaluateThicknessFallbackConfidenceGate)
	// genuinely fires and its recovered value reaches a Dynamic Layer's composed output through the REAL production
	// entry point -- unlike BuildIrregularOctahedronThicknessFixtureWorkingMesh above, which is deliberately dense
	// via the center ray ALONE and never exercises the fallback at all. Two large flat parallel plates (PlateA at
	// Z=0, outward normal -Z, so its own ray direction -N is +Z; PlateB at Z=H=20, outward normal +Z, ray direction
	// -Z) span X,Y in [-60,60] and give every one of their own 12 corners a trivial, guaranteed direct center-ray
	// hit on each other. A third, separate, UNSHARED-vertex triangle ("Special") sits at PlateB's own height H,
	// outward normal +Z (ray direction -Z, same as PlateB), at X=61/62/63 -- just past PlateA's X=60 edge -- so
	// EACH of Special's 3 corners' straight-down center ray provably misses PlateA's finite extent entirely
	// (analytically: no lateral movement in a straight -Z ray, and 61/62/63 > 60).
	//
	// Hand-derived from the fallback's own deterministic ring construction (BuildThicknessFallbackRingDirections):
	// for CenterDir=(0,0,-1), the tangent basis it computes is Tangent=(0,1,0), Bitangent=(1,0,0), giving ring
	// direction i (phi = 45*i degrees) = (sin(theta)*sin(phi), sin(theta)*cos(phi), -cos(theta)) with theta=30deg.
	// The resulting X-displacement at the PlateA plane is H*tan(30deg)*sin(phi) = ~11.547*sin(phi), which is
	// negative (moving back toward PlateA, i.e. a real candidate hit) ONLY for ring indices 5/6/7 (phi=225/270/315,
	// magnitudes ~8.165/11.547/8.165) -- comfortably exceeding the 5/6/7-unit X offset chosen for S0/S1/S2 below,
	// so those three rings land back inside PlateA (X in roughly [49,57], Y within a few units of each Special
	// vertex's own Y, both deep inside PlateA's [-60,60] extent). The other five rings (i=0,1,2,3,4) have
	// zero or positive X-displacement and can never reach PlateA at all.
	//
	// Because PlateA is flat and parallel to the tangent frame, EVERY one of those three recovered rays yields the
	// IDENTICAL projected distance H+Bias (ComputeThicknessValueWithFallback's own formula, Hit.Distance *
	// dot(RingDir, CenterDir) + EffectiveBias, is a pure geometric identity that reproduces the true perpendicular
	// distance for any ring ray hitting a plane parallel to the origin's own tangent plane) -- giving
	// MedianSupportCount=3, RunnerUpSupportCount=0, Confidence=1.0, SupportLead=3, comfortably clearing every one
	// of the fixed gate's thresholds (ValidValueCount>=3, MedianSupportCount>=3, Confidence>2/3, SupportLead>=2).
	//
	// Because this orchestrator's own Corner-domain contract (see ComputeComposedColorsRGBSourceTopology's
	// Thickness branch) requires NumValidValues == ExpectedCornerCount (all 15 corners here) for the whole call to
	// succeed at all, and Special's 3 corners are analytically UNREACHABLE via the center ray alone, a passing
	// (bSucceeded==true) result on this fixture is possible ONLY if the fallback fired for all three and its
	// confidence-gated median reached the final mask -- otherwise NumValidValues would be 12/15 and the whole call
	// would fail, exactly like ThicknessNoHitFailsWholeCallTest above. Triangle append order (PlateA Tri0/Tri1,
	// PlateB Tri0/Tri1, Special) is fixed and relied on below to locate Special's corners at indices 12/13/14.
	FVertexMaskForgeWorkingMesh BuildThicknessFallbackRecoveryFixtureWorkingMesh()
	{
		FVertexMaskForgeWorkingMesh WorkingMesh;
		WorkingMesh.Mesh = MakeUnique<UE::Geometry::FDynamicMesh3>();
		UE::Geometry::FDynamicMesh3& Mesh = *WorkingMesh.Mesh;
		constexpr double H = 20.0;

		const int32 A0 = Mesh.AppendVertex(FVector3d(-60.0, -60.0, 0.0));
		const int32 A1 = Mesh.AppendVertex(FVector3d(60.0, -60.0, 0.0));
		const int32 A2 = Mesh.AppendVertex(FVector3d(60.0, 60.0, 0.0));
		const int32 A3 = Mesh.AppendVertex(FVector3d(-60.0, 60.0, 0.0));
		const int32 TriA0 = Mesh.AppendTriangle(A0, A1, A2);
		const int32 TriA1 = Mesh.AppendTriangle(A0, A2, A3);

		const int32 B0 = Mesh.AppendVertex(FVector3d(-60.0, -60.0, H));
		const int32 B1 = Mesh.AppendVertex(FVector3d(60.0, -60.0, H));
		const int32 B2 = Mesh.AppendVertex(FVector3d(60.0, 60.0, H));
		const int32 B3 = Mesh.AppendVertex(FVector3d(-60.0, 60.0, H));
		// Deliberately REVERSED winding relative to PlateA's own (B0,B2,B1)/(B0,B3,B2), not (B0,B1,B2)/(B0,B2,B3)
		// -- empirically required (M17-TH-DL-C debug run): PlateA and PlateB have identical X/Y layout, so
		// matching winding gives them the SAME FDynamicMesh3::GetTriNormal() geometric normal, meaning at most
		// one of the two ray directions between them can ever pass ComputeThicknessRawValue's front-facing
		// orientation check (dot(HitNormal, Direction) > epsilon) -- the other direction is unconditionally
		// orientation-rejected. Reversing PlateB's winding flips its geometric normal so BOTH directions hit.
		const int32 TriB0 = Mesh.AppendTriangle(B0, B2, B1);
		const int32 TriB1 = Mesh.AppendTriangle(B0, B3, B2);

		// Own, UNSHARED vertices (never reused by PlateB) so moving them cannot distort PlateB's own
		// direct-hit geometry. X=65/66/67, safely past PlateA's X=60 edge (delta 5/6/7, well below the
		// ~8.165-unit minimum recovering-ring displacement derived above).
		const int32 S0 = Mesh.AppendVertex(FVector3d(65.0, -3.0, H));
		const int32 S1 = Mesh.AppendVertex(FVector3d(66.0, 3.0, H));
		const int32 S2 = Mesh.AppendVertex(FVector3d(67.0, 0.0, H));
		const int32 TriSpecial = Mesh.AppendTriangle(S0, S1, S2);

		Mesh.EnableAttributes();
		UE::Geometry::FDynamicMeshNormalOverlay* NormalOverlay = Mesh.Attributes()->PrimaryNormals();
		auto AppendFlatTriangleNormals = [&](int32 TriangleID, const FVector3f& N)
		{
			UE::Geometry::FIndex3i ElementTri;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				ElementTri[Corner] = NormalOverlay->AppendElement(N);
			}
			NormalOverlay->SetTriangle(TriangleID, ElementTri);
		};
		AppendFlatTriangleNormals(TriA0, FVector3f(0.0f, 0.0f, -1.0f));
		AppendFlatTriangleNormals(TriA1, FVector3f(0.0f, 0.0f, -1.0f));
		AppendFlatTriangleNormals(TriB0, FVector3f(0.0f, 0.0f, 1.0f));
		AppendFlatTriangleNormals(TriB1, FVector3f(0.0f, 0.0f, 1.0f));
		AppendFlatTriangleNormals(TriSpecial, FVector3f(0.0f, 0.0f, 1.0f));

		WorkingMesh.GeometryFingerprint = 300;
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

	// M16-K.6D-8D-B: same two-triangle quad shape/vertex positions as BuildOrchestratorFixtureWorkingMesh
	// (so FixtureCornerToVertexID above still applies), but with a real Normal Overlay populated --
	// GenerateDirectionalNormalMaskFromDynamicMesh requires Mesh.HasAttributes() &&
	// Mesh.Attributes()->PrimaryNormals() != nullptr (Unavailable otherwise), which the other fixture in
	// this file deliberately never enables (Material Slot/Bounding Box need no normals at all). Every
	// corner gets its OWN, DELIBERATELY DISTINCT normal element -- including corners 0/3 (both at V0) and
	// corners 2/4 (both at V2), which share a vertex POSITION but never a normal here -- so a test built
	// on this fixture can distinguish genuine corner-domain treatment (each corner independent) from an
	// accidental vertex-domain one (which would incorrectly read the same value at those shared-position
	// corner pairs).
	FVertexMaskForgeWorkingMesh BuildDirectionalNormalFixtureWorkingMesh()
	{
		FVertexMaskForgeWorkingMesh WorkingMesh;
		WorkingMesh.Mesh = MakeUnique<UE::Geometry::FDynamicMesh3>();
		WorkingMesh.Mesh->AppendVertex(FVector3d(0.0, 0.0, 0.0)); // V0
		WorkingMesh.Mesh->AppendVertex(FVector3d(1.0, 0.0, 0.0)); // V1
		WorkingMesh.Mesh->AppendVertex(FVector3d(1.0, 1.0, 0.0)); // V2
		WorkingMesh.Mesh->AppendVertex(FVector3d(0.0, 1.0, 0.0)); // V3
		WorkingMesh.Mesh->AppendTriangle(0, 1, 2);
		WorkingMesh.Mesh->AppendTriangle(0, 2, 3);

		WorkingMesh.Mesh->EnableAttributes();
		UE::Geometry::FDynamicMeshNormalOverlay* NormalOverlay = WorkingMesh.Mesh->Attributes()->PrimaryNormals();

		// AUDITED: wide angular spread (0 deg / ~45 deg / 90 deg / ~72 deg / ~63 deg / 180 deg from +Z),
		// deliberately spanning well inside, at the transition edge of, and well outside a typical
		// Angle=90/Falloff=45 cone -- small tilts (a few degrees) were tried first and produced an
		// entirely constant (all-1.0) reference mask, since every corner landed deep inside the cone's
		// flat-top region; this spread guarantees genuine, non-constant raw values.
		const FVector3f CornerNormals[6] = {
			FVector3f(0.00f, 0.00f, 1.00f),                                  // corner 0 (Tri0, V0): 0 deg
			FVector3f(1.00f, 0.00f, 1.00f).GetSafeNormal(),                  // corner 1 (Tri0, V1): 45 deg
			FVector3f(1.00f, 0.00f, 0.00f),                                  // corner 2 (Tri0, V2): 90 deg
			FVector3f(-1.00f, 0.00f, 0.30f).GetSafeNormal(),                 // corner 3 (Tri1, V0): ~73 deg -- distinct from corner 0
			FVector3f(0.00f, 1.00f, 0.50f).GetSafeNormal(),                  // corner 4 (Tri1, V2): ~63 deg -- distinct from corner 2
			FVector3f(0.00f, 0.00f, -1.00f),                                 // corner 5 (Tri1, V3): 180 deg
		};

		int32 CornerIndex = 0;
		for (const int32 TriangleID : WorkingMesh.Mesh->TriangleIndicesItr())
		{
			UE::Geometry::FIndex3i ElementTri;
			for (int32 Corner = 0; Corner < 3; ++Corner, ++CornerIndex)
			{
				ElementTri[Corner] = NormalOverlay->AppendElement(CornerNormals[CornerIndex]);
			}
			NormalOverlay->SetTriangle(TriangleID, ElementTri);
		}

		return WorkingMesh;
	}

	// Adds a layer with a Directional Normal mask, configured via the stack's own controlled mutators --
	// mirrors AddMaterialSlotLayer's/AddBoundingBoxLayer's own setup sequence exactly.
	FGuid AddDirectionalNormalLayer(
		FVertexMaskForgeDynamicLayerStack& Stack, const FString& Name,
		EVertexMaskForgeLayerFill Fill, EVertexMaskForgeBlendMode BlendMode, float Opacity,
		EVertexMaskForgeNormalSpace Space, EVertexMaskForgeNormalDirection Direction,
		float Angle, float Falloff, float Blur, bool bInvert)
	{
		const FGuid LayerId = Stack.AddLayer(Name);
		Stack.SetLayerFill(LayerId, Fill);
		Stack.SetLayerBlendMode(LayerId, BlendMode);
		Stack.SetLayerOpacity(LayerId, Opacity);
		Stack.SetLayerMaskGeneratorType(LayerId, EVertexMaskForgeGeneratorType::DirectionalNormal);

		const FVertexMaskForgeGeneratorMaskInstance* MaskInstance = Stack.GetLayerMask(LayerId);
		check(MaskInstance);
		FVertexMaskForgeGeneratorParams NewParams = MakeVertexMaskForgeGeneratorParams(EVertexMaskForgeGeneratorType::DirectionalNormal);
		FVertexMaskForgeDirectionalNormalParams& NormalParams = NewParams.Get<FVertexMaskForgeDirectionalNormalParams>();
		NormalParams.Space = Space;
		NormalParams.Direction = Direction;
		NormalParams.Angle = Angle;
		NormalParams.Falloff = Falloff;
		NormalParams.Blur = Blur;
		NormalParams.bInvert = bInvert;
		Stack.SetLayerMaskParams(LayerId, MaskInstance->MaskInstanceId, NewParams);

		return LayerId;
	}

	// M16-K.6D-8E-B: same two-triangle quad topology (vertex/triangle append order) as
	// BuildOrchestratorFixtureWorkingMesh -- so FixtureCornerToVertexID ({0,1,2,0,2,3}) still applies
	// unchanged -- but deliberately NON-PLANAR (V2 raised in Z), unlike that fixture (flat, Z=0
	// everywhere -> zero curvature everywhere, useless for this generator) and unlike
	// BuildZVaryingFixtureWorkingMesh (its own two triangles are, by construction, coplanar despite
	// differing Z values -- verified by direct cross-product computation -- so it is ALSO unsuitable
	// here). The shared diagonal edge (V0-V2) has a genuine dihedral fold between Tri0 and Tri1, so raw
	// Curvature is nonzero at V0/V2 (both touched by that interior edge) and exactly zero at V1/V3 (only
	// touched by boundary edges, which ComputeRawCurvatureMagnitudes never accumulates into) -- a
	// genuinely non-constant, non-uniformly-zero result, never accidentally passing under a wrong domain.
	FVertexMaskForgeWorkingMesh BuildCurvatureFixtureWorkingMesh()
	{
		FVertexMaskForgeWorkingMesh WorkingMesh;
		WorkingMesh.Mesh = MakeUnique<UE::Geometry::FDynamicMesh3>();
		WorkingMesh.Mesh->AppendVertex(FVector3d(0.0, 0.0, 0.0)); // V0
		WorkingMesh.Mesh->AppendVertex(FVector3d(1.0, 0.0, 0.0)); // V1
		WorkingMesh.Mesh->AppendVertex(FVector3d(1.0, 1.0, 0.7)); // V2 -- raised in Z, creates the fold
		WorkingMesh.Mesh->AppendVertex(FVector3d(0.0, 1.0, 0.0)); // V3
		WorkingMesh.Mesh->AppendTriangle(0, 1, 2);
		WorkingMesh.Mesh->AppendTriangle(0, 2, 3);
		return WorkingMesh;
	}

	// Adds a layer with a Curvature mask, configured via the stack's own controlled mutators -- mirrors
	// AddDirectionalNormalLayer's own setup sequence exactly. Curvature has no Space field/concept at all
	// (confirmed by M16-K.6D-8E-A), so unlike AddDirectionalNormalLayer there is no Space parameter here.
	FGuid AddCurvatureLayer(
		FVertexMaskForgeDynamicLayerStack& Stack, const FString& Name,
		EVertexMaskForgeLayerFill Fill, EVertexMaskForgeBlendMode BlendMode, float Opacity,
		EVertexMaskForgeCurvatureType Type, float Multiplier, float Blur, float LevelsMin, float LevelsMax, bool bInvert)
	{
		const FGuid LayerId = Stack.AddLayer(Name);
		Stack.SetLayerFill(LayerId, Fill);
		Stack.SetLayerBlendMode(LayerId, BlendMode);
		Stack.SetLayerOpacity(LayerId, Opacity);
		Stack.SetLayerMaskGeneratorType(LayerId, EVertexMaskForgeGeneratorType::Curvature);

		const FVertexMaskForgeGeneratorMaskInstance* MaskInstance = Stack.GetLayerMask(LayerId);
		check(MaskInstance);
		FVertexMaskForgeGeneratorParams NewParams = MakeVertexMaskForgeGeneratorParams(EVertexMaskForgeGeneratorType::Curvature);
		FVertexMaskForgeCurvatureParams& CurvatureParams = NewParams.Get<FVertexMaskForgeCurvatureParams>();
		CurvatureParams.Type = Type;
		CurvatureParams.Multiplier = Multiplier;
		CurvatureParams.Blur = Blur;
		CurvatureParams.LevelsMin = LevelsMin;
		CurvatureParams.LevelsMax = LevelsMax;
		CurvatureParams.bInvert = bInvert;
		Stack.SetLayerMaskParams(LayerId, MaskInstance->MaskInstanceId, NewParams);

		return LayerId;
	}

	// M16-K.6D-8F-B: maps a layer-owned FVertexMaskForgeNoiseParams onto the generator's own
	// FVertexMaskForgeNoiseGenerativeParams (the "raw/generative" field subset only -- Multiplier/
	// LevelsMin/LevelsMax/bInvert are forwarded as separate positional arguments, exactly mirroring the
	// orchestrator's own Noise dispatch branch). This is a pure field-mapping helper, never a re-derivation
	// of any Noise formula -- reused identically by every test below to build its own independent reference
	// call to the real generator.
	FVertexMaskForgeNoiseGenerativeParams MakeNoiseGenerativeParams(const FVertexMaskForgeNoiseParams& Params)
	{
		FVertexMaskForgeNoiseGenerativeParams Generative;
		Generative.NoiseType = Params.Type;
		Generative.ScaleX = Params.ScaleX;
		Generative.ScaleY = Params.ScaleY;
		Generative.ScaleZ = Params.ScaleZ;
		Generative.OffsetX = Params.OffsetX;
		Generative.OffsetY = Params.OffsetY;
		Generative.OffsetZ = Params.OffsetZ;
		Generative.Seed = Params.Seed;
		Generative.Octaves = Params.Octaves;
		Generative.Roughness = Params.Roughness;
		Generative.Lacunarity = Params.Lacunarity;
		Generative.TurbulenceStrength = Params.TurbulenceStrength;
		Generative.Blur = Params.Blur;
		return Generative;
	}

	// A non-degenerate, fully-specified baseline -- ScaleX/Y/Z=2 (not the struct's own default of 1, so a
	// forwarding bug that silently falls back to the default would very likely diverge from a reference call
	// using this same struct), genuinely nonzero across all three axes given BuildZVaryingFixtureWorkingMesh's
	// own non-degenerate X/Y/Z spread.
	FVertexMaskForgeNoiseParams MakeNoiseBaselineParams()
	{
		FVertexMaskForgeNoiseParams P;
		P.Type = EVertexMaskForgeNoiseType::FractalPerlin;
		P.ScaleX = 2.0f;
		P.ScaleY = 2.0f;
		P.ScaleZ = 2.0f;
		P.OffsetX = 0.0f;
		P.OffsetY = 0.0f;
		P.OffsetZ = 0.0f;
		P.Seed = 0;
		P.Octaves = 4;
		P.Roughness = 0.5f;
		P.Lacunarity = 2.0f;
		P.TurbulenceStrength = 0.5f;
		P.Multiplier = 1.0f;
		P.Blur = 0.0f;
		P.LevelsMin = 0.0f;
		P.LevelsMax = 1.0f;
		P.bInvert = false;
		return P;
	}

	// Adds a layer with a Noise mask, configured via the stack's own controlled mutators -- mirrors
	// AddCurvatureLayer's own setup sequence, but takes the full FVertexMaskForgeNoiseParams by value/const-
	// ref (rather than one argument per field) since Noise's authoritative field count (16) is materially
	// larger than any other generator wired into Dynamic so far. Noise has no Space field/concept at all
	// (confirmed by M16-K.6D-8F-A), so unlike AddDirectionalNormalLayer there is no Space parameter here.
	FGuid AddNoiseLayer(
		FVertexMaskForgeDynamicLayerStack& Stack, const FString& Name,
		EVertexMaskForgeLayerFill Fill, EVertexMaskForgeBlendMode BlendMode, float Opacity,
		const FVertexMaskForgeNoiseParams& NoiseParams)
	{
		const FGuid LayerId = Stack.AddLayer(Name);
		Stack.SetLayerFill(LayerId, Fill);
		Stack.SetLayerBlendMode(LayerId, BlendMode);
		Stack.SetLayerOpacity(LayerId, Opacity);
		Stack.SetLayerMaskGeneratorType(LayerId, EVertexMaskForgeGeneratorType::Noise);

		const FVertexMaskForgeGeneratorMaskInstance* MaskInstance = Stack.GetLayerMask(LayerId);
		check(MaskInstance);
		FVertexMaskForgeGeneratorParams NewParams = MakeVertexMaskForgeGeneratorParams(EVertexMaskForgeGeneratorType::Noise);
		NewParams.Get<FVertexMaskForgeNoiseParams>() = NoiseParams;
		Stack.SetLayerMaskParams(LayerId, MaskInstance->MaskInstanceId, NewParams);

		return LayerId;
	}

	// M16-K.6D-8G-E: a narrow, deep two-panel "canyon" (NOT flat, unlike BuildOrchestratorFixtureWorkingMesh
	// -- a flat mesh gives Ambient Occlusion nothing to occlude against, since a hemisphere ray from a
	// planar surface with no other nearby geometry almost never hits anything) -- same two-triangle
	// topology/append order as every other fixture in this file (Tri0=(V0,V1,V2), Tri1=(V0,V2,V3)), so
	// FixtureCornerToVertexID still applies, but V0/V2 (the shared ridge, low) and V1/V3 (each panel's own
	// outer top corner, high and only slightly offset sideways) form two near-vertical walls ~0.8 units
	// apart at the top, hinged along the V0-V2 ridge -- genuine, non-degenerate self-occlusion between the
	// two panels, real enough for a raycaster to measure, never a hand-picked/faked scalar.
	//
	// A REAL Normal Overlay is populated with SIX INDEPENDENT elements (one per corner, never shared) --
	// V0 (corner 0 in Tri0, corner 3 in Tri1) and V2 (corner 2 in Tri0, corner 4 in Tri1) each get TWO
	// DELIBERATELY DISTINCT normals at the SAME geometric position: corner 0/2 point mostly toward +Z (up
	// and away from both walls -- open sky, LOWER expected occlusion), corner 3/4 point mostly toward the
	// OPPOSING wall (heavily -X/+X respectively -- INTO the canyon, HIGHER expected occlusion). This is
	// what lets a test built on this fixture distinguish genuine per-corner Normal-Overlay-Element-ID
	// resolution (each corner independently reads ITS OWN element's occlusion) from an incorrect
	// vertex-averaged or arbitrary-first-element substitute -- using the ACTUAL raycast result, never an
	// assumption about which value "should" be bigger asserted without also verifying real distinctness.
	FVertexMaskForgeWorkingMesh BuildAOFoldFixtureWorkingMesh()
	{
		FVertexMaskForgeWorkingMesh WorkingMesh;
		WorkingMesh.Mesh = MakeUnique<UE::Geometry::FDynamicMesh3>();
		WorkingMesh.Mesh->AppendVertex(FVector3d(0.0, 0.0, 0.0));  // V0 -- ridge (shared, low)
		WorkingMesh.Mesh->AppendVertex(FVector3d(0.4, 0.0, 1.2));  // V1 -- Tri0's own outer top corner (+X wall)
		WorkingMesh.Mesh->AppendVertex(FVector3d(0.0, 1.0, 0.0));  // V2 -- ridge (shared, low)
		WorkingMesh.Mesh->AppendVertex(FVector3d(-0.4, 1.0, 1.2)); // V3 -- Tri1's own outer top corner (-X wall)
		WorkingMesh.Mesh->AppendTriangle(0, 1, 2);
		WorkingMesh.Mesh->AppendTriangle(0, 2, 3);

		WorkingMesh.Mesh->EnableAttributes();
		UE::Geometry::FDynamicMeshNormalOverlay* NormalOverlay = WorkingMesh.Mesh->Attributes()->PrimaryNormals();

		const FVector3f CornerNormals[6] = {
			FVector3f(0.10f, 0.00f, 0.99f).GetSafeNormal(),   // corner 0 (Tri0, V0): mostly +Z, open sky
			FVector3f(0.70f, 0.00f, 0.70f).GetSafeNormal(),   // corner 1 (Tri0, V1): outward, +X wall's own face
			FVector3f(0.10f, 0.00f, 0.99f).GetSafeNormal(),   // corner 2 (Tri0, V2): mostly +Z, open sky
			FVector3f(-0.85f, 0.00f, 0.30f).GetSafeNormal(),  // corner 3 (Tri1, V0): heavily -X, INTO the canyon toward the -X wall -- deliberately distinct from corner 0
			FVector3f(0.85f, 0.00f, 0.30f).GetSafeNormal(),   // corner 4 (Tri1, V2): heavily +X, INTO the canyon toward the +X wall -- deliberately distinct from corner 2
			FVector3f(-0.70f, 0.00f, 0.70f).GetSafeNormal(),  // corner 5 (Tri1, V3): outward, -X wall's own face
		};

		int32 CornerIndex = 0;
		for (const int32 TriangleID : WorkingMesh.Mesh->TriangleIndicesItr())
		{
			UE::Geometry::FIndex3i ElementTri;
			for (int32 Corner = 0; Corner < 3; ++Corner, ++CornerIndex)
			{
				ElementTri[Corner] = NormalOverlay->AppendElement(CornerNormals[CornerIndex]);
			}
			NormalOverlay->SetTriangle(TriangleID, ElementTri);
		}

		WorkingMesh.GeometryFingerprint = 1;
		return WorkingMesh;
	}

	// Adds a layer with an Ambient Occlusion mask, configured via the stack's own controlled mutators --
	// mirrors AddCurvatureLayer's/AddNoiseLayer's own setup sequence exactly.
	FGuid AddAmbientOcclusionLayer(
		FVertexMaskForgeDynamicLayerStack& Stack, const FString& Name,
		EVertexMaskForgeLayerFill Fill, EVertexMaskForgeBlendMode BlendMode, float Opacity,
		int32 Samples, float MaxDistance, float Bias, float LevelsMin, float LevelsMax, bool bInvert)
	{
		const FGuid LayerId = Stack.AddLayer(Name);
		Stack.SetLayerFill(LayerId, Fill);
		Stack.SetLayerBlendMode(LayerId, BlendMode);
		Stack.SetLayerOpacity(LayerId, Opacity);
		Stack.SetLayerMaskGeneratorType(LayerId, EVertexMaskForgeGeneratorType::AmbientOcclusion);

		const FVertexMaskForgeGeneratorMaskInstance* MaskInstance = Stack.GetLayerMask(LayerId);
		check(MaskInstance);
		FVertexMaskForgeGeneratorParams NewParams = MakeVertexMaskForgeGeneratorParams(EVertexMaskForgeGeneratorType::AmbientOcclusion);
		FVertexMaskForgeAmbientOcclusionParams& AOParams = NewParams.Get<FVertexMaskForgeAmbientOcclusionParams>();
		AOParams.Samples = Samples;
		AOParams.MaxDistance = MaxDistance;
		AOParams.Bias = Bias;
		AOParams.LevelsMin = LevelsMin;
		AOParams.LevelsMax = LevelsMax;
		AOParams.bInvert = bInvert;
		Stack.SetLayerMaskParams(LayerId, MaskInstance->MaskInstanceId, NewParams);

		return LayerId;
	}

	// Builds the exact FVertexMaskForgeAOParams the orchestrator's own AO branch would translate this
	// layer's FVertexMaskForgeAmbientOcclusionParams into (see VertexMaskForgeDynamicSourceTopologyComposition
	// .cpp's own 1:1 field translation) -- used ONLY to drive the direct, independent oracle generator call
	// in a test, never to duplicate any AO math itself.
	FVertexMaskForgeAOParams MakeOracleAOParams(int32 Samples, float MaxDistance, float Bias, float LevelsMin, float LevelsMax, bool bInvert)
	{
		FVertexMaskForgeAOParams RawParams;
		RawParams.Samples = Samples;
		RawParams.MaxDistance = MaxDistance;
		RawParams.Bias = Bias;
		RawParams.LevelsMin = LevelsMin;
		RawParams.LevelsMax = LevelsMax;
		RawParams.bInvert = bInvert;
		return RawParams;
	}

	// Test-local only (never a production seam) -- copies LayerId's CURRENT Ambient Occlusion params
	// (byte/value-exact), applies Mutator, writes back through the stack's own existing controlled API in
	// ONE call, mirroring the exact pattern SVertexMaskForgePanel::MutateDynamicCurvatureParam/
	// MutateDynamicNoiseParam already establish in production for every other generator.
	void MutateAmbientOcclusionParams(FVertexMaskForgeDynamicLayerStack& Stack, const FGuid& LayerId, TFunctionRef<void(FVertexMaskForgeAmbientOcclusionParams&)> Mutator)
	{
		const FVertexMaskForgeGeneratorMaskInstance* MaskInstance = Stack.GetLayerMask(LayerId);
		check(MaskInstance && MaskInstance->Params.IsType<FVertexMaskForgeAmbientOcclusionParams>());
		FVertexMaskForgeGeneratorParams NewParams = MaskInstance->Params;
		Mutator(NewParams.Get<FVertexMaskForgeAmbientOcclusionParams>());
		Stack.SetLayerMaskParams(LayerId, MaskInstance->MaskInstanceId, NewParams);
	}

	// Byte-for-byte TArray<float> comparison -- used throughout this file's AO tests to prove RawValues
	// reuse/invalidation without relying on any production accessor beyond the cache struct's own already-
	// public fields.
	bool AreRawValueArraysIdentical(const TArray<float>& A, const TArray<float>& B)
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

// 1. An empty stack composes to a byte-exact passthrough of BaseColors -- matching
// VertexMaskForgeDynamicLayerEvaluator::EvaluateColor's own empty-stack contract exactly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompEmptyStackTest, "VertexMaskForge.DynamicSourceTopologyComposition.EmptyStackPreservesAllBaseColorsByteExact", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompEmptyStackTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1);
	const FVertexMaskForgeDynamicLayerStack Stack; // empty
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	TArray<FColor> Out;
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);

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
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);

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
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);

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

// 4. A disabled layer contributes nothing AND is never validated for its Mask's GeneratorType -- even a
// generator that WOULD fail the whole call if enabled must never be inspected while disabled. Uses
// Thickness: as of M17-TH-DL-B, Thickness is a fully supported, dispatched generator, but
// BuildOrchestratorFixtureWorkingMesh has no Normal Overlay at all (never EnableAttributes()'d) -- the
// Thickness backend requires one (GenerateThicknessMaskFromDynamicMesh returns Unavailable without it), so
// this layer would still fail the whole call if enabled; while disabled, it must never even be attempted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompDisabledLayerTest, "VertexMaskForge.DynamicSourceTopologyComposition.DisabledLayerContributesNothingAndSkipsValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompDisabledLayerTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1);
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = Stack.AddLayer(TEXT("Layer"));
	Stack.SetLayerFill(LayerId, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerBlendMode(LayerId, EVertexMaskForgeBlendMode::Copy);
	Stack.SetLayerOpacity(LayerId, 1.0f);
	// Would fail the whole call if enabled (no Normal Overlay on this fixture -- see comment above).
	Stack.SetLayerMaskGeneratorType(LayerId, EVertexMaskForgeGeneratorType::Thickness);
	Stack.SetLayerEnabled(LayerId, false);

	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();
	TArray<FColor> Out;
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);

	TestTrue(TEXT("Succeeds -- disabled layer's mask is never even inspected, let alone validated"), bSucceeded);
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

// 5. RECONCILED (M17-TH-DL-B): as of this checkpoint every EVertexMaskForgeGeneratorType enumerator is a
// real, dispatched generator (Thickness was the last remaining unsupported value) -- there is no longer a
// generator type this orchestrator structurally rejects, so this test's original "unsupported generator
// type" premise no longer has a real value to exercise. Repurposed (same fixture/mechanism, mirrors
// DisabledLayerContributesNothingAndSkipsValidation's own updated rationale immediately above) to prove an
// ENABLED masked layer whose generator legitimately CANNOT produce a value on this specific mesh (Thickness
// requires a Normal Overlay; BuildOrchestratorFixtureWorkingMesh has none) still fails the WHOLE call
// explicitly -- never silently skipped, never treated as Fill-only, never a fabricated value.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompUnsupportedGeneratorTest, "VertexMaskForge.DynamicSourceTopologyComposition.GeneratorUnavailableOnMeshFailsWholeCall", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompUnsupportedGeneratorTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1);
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = Stack.AddLayer(TEXT("Layer"));
	Stack.SetLayerFill(LayerId, EVertexMaskForgeLayerFill::White);
	Stack.SetLayerBlendMode(LayerId, EVertexMaskForgeBlendMode::Copy);
	Stack.SetLayerOpacity(LayerId, 1.0f);
	// M17-TH-DL-B: Thickness is now a real, dispatched generator, but requires a Normal Overlay
	// (GenerateThicknessMaskFromDynamicMesh's own Unavailable early-out) -- this fixture has none, so the
	// WHOLE call must still fail explicitly, exactly as it would for a structurally unsupported type.
	Stack.SetLayerMaskGeneratorType(LayerId, EVertexMaskForgeGeneratorType::Thickness);

	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();
	TArray<FColor> Out = { FColor(1, 2, 3, 4) }; // sentinel, wrong size on purpose
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);

	TestFalse(TEXT("Fails when the masked generator's own backend reports Unavailable (no Normal Overlay) on this mesh"), bSucceeded);
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

	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);

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
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);

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
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);

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
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);

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
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);

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
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> ForwardOutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ForwardOutThicknessCaches;
	const bool bForwardSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, ForwardStack, BaseColors, FTransform::Identity, ForwardOutAOCaches, ForwardOutThicknessCaches, ForwardOut);
	TArray<FColor> ReverseOut;
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> ReverseOutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ReverseOutThicknessCaches;
	const bool bReverseSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, ReverseStack, BaseColors, FTransform::Identity, ReverseOutAOCaches, ReverseOutThicknessCaches, ReverseOut);

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
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);

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
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);

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
		TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
		TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
		const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);
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
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);

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
		TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
		TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
		const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);
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
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);
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
		TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
		TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
		const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);
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
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);
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
		TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
		TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
		const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);
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
		TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
		TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
		const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);
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
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> ForwardOutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ForwardOutThicknessCaches;
	const bool bForwardSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, ForwardStack, BaseColors, FTransform::Identity, ForwardOutAOCaches, ForwardOutThicknessCaches, ForwardOut);
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> ReverseOutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ReverseOutThicknessCaches;
	const bool bReverseSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, ReverseStack, BaseColors, FTransform::Identity, ReverseOutAOCaches, ReverseOutThicknessCaches, ReverseOut);
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
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);

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
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);

	TestFalse(TEXT("Unified Bounds is rejected (fails the whole call)"), bSucceeded);
	TestEqual(TEXT("Out left completely untouched on failure"), Out.Num(), 1);
	if (Out.Num() == 1)
	{
		TestEqual(TEXT("Out[0] sentinel preserved"), Out[0], FColor(5, 6, 7, 8));
	}

	return true;
}

// --- M16-K.6D-8D-B: Local-space Directional Normal support ----------------------------------------

// A. A Local-space Directional Normal layer is accepted and produces a byte-exact result against the
// REAL, authoritative generator (called directly, never reimplemented). The fixture's deliberately
// distinct per-corner normals (including at corners sharing one vertex position) mean this test cannot
// pass via accidental constant-mask behavior, wrong corner indexing, or vertex-domain treatment.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompDirNormalLocalDispatchTest, "VertexMaskForge.DynamicSourceTopologyComposition.DirectionalNormalLocalSpaceByteExactAgainstGeneratorAndCornerDomain", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompDirNormalLocalDispatchTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildDirectionalNormalFixtureWorkingMesh();
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	const EVertexMaskForgeNormalSpace Space = EVertexMaskForgeNormalSpace::Local;
	const EVertexMaskForgeNormalDirection Direction = EVertexMaskForgeNormalDirection::PositiveZ;
	const float Angle = 90.0f;
	const float Falloff = 45.0f;
	const float Blur = 0.0f;
	const bool bInvert = false;

	// Authoritative, independent reference -- the REAL generator, called directly, never reimplemented.
	const FVertexMaskForgeScalarMask ReferenceMask = VertexMaskForgeDirectionalNormalGenerator::GenerateDirectionalNormalMaskFromDynamicMesh(
		WorkingMesh, Space, Direction, Angle, Falloff, Blur, bInvert, FTransform::Identity);
	TestTrue(TEXT("Reference generator State == Ready"), ReferenceMask.State == EVertexMaskForgeScalarMaskState::Ready);
	TestEqual(TEXT("Reference generator NumValidValues == 6 (fixture has no holes)"), ReferenceMask.NumValidValues, 6);

	FVertexMaskForgeDynamicLayerStack Stack;
	AddDirectionalNormalLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		Space, Direction, Angle, Falloff, Blur, bInvert);

	TArray<FColor> Out;
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);
	TestTrue(TEXT("Orchestrator accepts a Local-space Directional Normal layer"), bSucceeded);
	TestEqual(TEXT("Out.Num() == 6"), Out.Num(), 6);
	if (!bSucceeded || Out.Num() != 6)
	{
		return false;
	}

	// White Fill / Copy / Opacity 1.0 -> RGB == mask value broadcast, exactly the same established
	// contract SingleMaterialSlotLayerTest/BoundingBoxLocalXAxisTest already prove and reuse.
	for (int32 CornerIndex = 0; CornerIndex < 6; ++CornerIndex)
	{
		float ExpectedMaskValue = 0.0f;
		const bool bHasValue = ReferenceMask.TryGetValue(CornerIndex, ExpectedMaskValue);
		TestTrue(*FString::Printf(TEXT("Reference mask has a value at corner %d"), CornerIndex), bHasValue);
		if (!bHasValue)
		{
			continue;
		}
		const uint8 ExpectedByte = UnitFloatToByte(ExpectedMaskValue);
		TestEqual(*FString::Printf(TEXT("Out[%d].R byte-exact vs reference generator"), CornerIndex), Out[CornerIndex].R, ExpectedByte);
		TestEqual(*FString::Printf(TEXT("Out[%d].G byte-exact vs reference generator"), CornerIndex), Out[CornerIndex].G, ExpectedByte);
		TestEqual(*FString::Printf(TEXT("Out[%d].B byte-exact vs reference generator"), CornerIndex), Out[CornerIndex].B, ExpectedByte);
		TestEqual(*FString::Printf(TEXT("Out[%d].A == BaseColors[%d].A"), CornerIndex, CornerIndex), Out[CornerIndex].A, BaseColors[CornerIndex].A);
	}

	// Corner-domain proof: corners 0/3 share a vertex POSITION (both at V0) but were given DIFFERENT
	// normals in the fixture -- a genuinely corner-domain implementation must show different reference
	// values there; an accidental vertex-domain implementation could not. Same for corners 2/4 (V2).
	TestNotEqual(TEXT("Corner 0 and corner 3 (same vertex position, different normals) differ"), ReferenceMask.Values[0], ReferenceMask.Values[3]);
	TestNotEqual(TEXT("Corner 2 and corner 4 (same vertex position, different normals) differ"), ReferenceMask.Values[2], ReferenceMask.Values[4]);
	// Non-constant-mask proof.
	TestFalse(TEXT("Output is not a constant mask across all six corners"),
		Out[0].R == Out[1].R && Out[1].R == Out[2].R && Out[2].R == Out[3].R && Out[3].R == Out[4].R && Out[4].R == Out[5].R);

	return true;
}

// B. Parameter forwarding: Direction, Angle, Falloff, Blur (genuinely nonzero), and Invert are each
// forwarded unchanged to the real generator -- proven by byte-exact comparison against a direct call
// with the SAME parameters, never by re-deriving the trigonometric/blur formulas here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompDirNormalParamForwardingTest, "VertexMaskForge.DynamicSourceTopologyComposition.DirectionalNormalParameterForwardingByteExactAgainstGenerator", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompDirNormalParamForwardingTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildDirectionalNormalFixtureWorkingMesh();
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	struct FConfig
	{
		const TCHAR* Label;
		EVertexMaskForgeNormalDirection Direction;
		float Angle;
		float Falloff;
		float Blur;
		bool bInvert;
	};
	const FConfig Configs[] = {
		{ TEXT("DirectionPositiveX"), EVertexMaskForgeNormalDirection::PositiveX, 90.0f, 45.0f, 0.0f, false },
		{ TEXT("NarrowAngle"),        EVertexMaskForgeNormalDirection::PositiveZ, 30.0f, 10.0f, 0.0f, false },
		{ TEXT("WideAngleFalloff"),   EVertexMaskForgeNormalDirection::PositiveZ, 170.0f, 90.0f, 0.0f, false },
		{ TEXT("NonzeroBlur"),        EVertexMaskForgeNormalDirection::PositiveZ, 90.0f, 45.0f, 2.0f, false },
		{ TEXT("Inverted"),           EVertexMaskForgeNormalDirection::PositiveZ, 90.0f, 45.0f, 0.0f, true },
	};

	for (const FConfig& Config : Configs)
	{
		const FVertexMaskForgeScalarMask ReferenceMask = VertexMaskForgeDirectionalNormalGenerator::GenerateDirectionalNormalMaskFromDynamicMesh(
			WorkingMesh, EVertexMaskForgeNormalSpace::Local, Config.Direction, Config.Angle, Config.Falloff, Config.Blur, Config.bInvert, FTransform::Identity);
		TestTrue(*FString::Printf(TEXT("%s: reference generator State == Ready"), Config.Label), ReferenceMask.State == EVertexMaskForgeScalarMaskState::Ready);

		FVertexMaskForgeDynamicLayerStack Stack;
		AddDirectionalNormalLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
			EVertexMaskForgeNormalSpace::Local, Config.Direction, Config.Angle, Config.Falloff, Config.Blur, Config.bInvert);

		TArray<FColor> Out;
		TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
		TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
		const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);
		TestTrue(*FString::Printf(TEXT("%s: succeeds"), Config.Label), bSucceeded);
		if (!bSucceeded || Out.Num() != 6)
		{
			continue;
		}

		for (int32 CornerIndex = 0; CornerIndex < 6; ++CornerIndex)
		{
			float ExpectedMaskValue = 0.0f;
			if (!ReferenceMask.TryGetValue(CornerIndex, ExpectedMaskValue))
			{
				continue;
			}
			TestEqual(*FString::Printf(TEXT("%s: Out[%d].R byte-exact"), Config.Label, CornerIndex), Out[CornerIndex].R, UnitFloatToByte(ExpectedMaskValue));
		}
	}

	// Explicit proof Blur is genuinely nonzero and changes the result versus an unblurred reference --
	// not merely accepted as a parameter, but actually forwarded into the generator's own blur pass.
	const FVertexMaskForgeScalarMask UnblurredMask = VertexMaskForgeDirectionalNormalGenerator::GenerateDirectionalNormalMaskFromDynamicMesh(
		WorkingMesh, EVertexMaskForgeNormalSpace::Local, EVertexMaskForgeNormalDirection::PositiveZ, 90.0f, 45.0f, 0.0f, false, FTransform::Identity);
	const FVertexMaskForgeScalarMask BlurredMask = VertexMaskForgeDirectionalNormalGenerator::GenerateDirectionalNormalMaskFromDynamicMesh(
		WorkingMesh, EVertexMaskForgeNormalSpace::Local, EVertexMaskForgeNormalDirection::PositiveZ, 90.0f, 45.0f, 2.0f, false, FTransform::Identity);
	bool bAnyDifference = false;
	for (int32 CornerIndex = 0; CornerIndex < 6; ++CornerIndex)
	{
		float UnblurredValue = 0.0f;
		float BlurredValue = 0.0f;
		if (UnblurredMask.TryGetValue(CornerIndex, UnblurredValue) && BlurredMask.TryGetValue(CornerIndex, BlurredValue) && UnblurredValue != BlurredValue)
		{
			bAnyDifference = true;
		}
	}
	TestTrue(TEXT("Nonzero Blur genuinely changes the reference generator's own output (not a no-op)"), bAnyDifference);

	return true;
}

// C. World Space is REJECTED (whole-call failure), never silently reinterpreted as Local Space -- Out is
// left completely untouched on failure, and the layer's own stored params are never mutated by the
// rejected call. Mirrors BoundingBoxWorldSpaceRejectedInThisCheckpoint's own established pattern.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompDirNormalWorldSpaceRejectedTest, "VertexMaskForge.DynamicSourceTopologyComposition.DirectionalNormalWorldSpaceRejectedInThisCheckpoint", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompDirNormalWorldSpaceRejectedTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildDirectionalNormalFixtureWorkingMesh();
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = AddDirectionalNormalLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		EVertexMaskForgeNormalSpace::World, EVertexMaskForgeNormalDirection::PositiveZ, 90.0f, 45.0f, 0.0f, false);

	TArray<FColor> Out = { FColor(1, 2, 3, 4) }; // sentinel, wrong size on purpose
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);

	TestFalse(TEXT("World Space is rejected (fails the whole call)"), bSucceeded);
	TestEqual(TEXT("Out left completely untouched on failure"), Out.Num(), 1);
	if (Out.Num() == 1)
	{
		TestEqual(TEXT("Out[0] sentinel preserved"), Out[0], FColor(1, 2, 3, 4));
	}

	// Stored params must remain exactly as configured -- never rewritten/normalized to Local by the
	// rejected call.
	const FVertexMaskForgeGeneratorMaskInstance* Mask = Stack.GetLayerMask(LayerId);
	TestNotNull(TEXT("Layer mask still present after rejection"), Mask);
	if (Mask && Mask->Params.IsType<FVertexMaskForgeDirectionalNormalParams>())
	{
		TestTrue(TEXT("Stored Space is still World (never silently reinterpreted as Local)"),
			Mask->Params.Get<FVertexMaskForgeDirectionalNormalParams>().Space == EVertexMaskForgeNormalSpace::World);
	}

	return true;
}

// D. Two independently parameterized Directional Normal layers retain distinct masks and compose
// strictly in Stack order (Copy always wins with the LAST enabled layer's own value, the same order
// contract OrderMattersTest/TwoBoundingBoxLayersRetainDistinctMasksAndReorderChangesResult already
// prove) -- and reordering changes the final result, proving this generator type is not special-cased
// around the authoritative orchestrator.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompDirNormalTwoLayersOrderTest, "VertexMaskForge.DynamicSourceTopologyComposition.TwoDirectionalNormalLayersRetainDistinctMasksAndReorderChangesResult", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompDirNormalTwoLayersOrderTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildDirectionalNormalFixtureWorkingMesh();
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	const FVertexMaskForgeScalarMask XReference = VertexMaskForgeDirectionalNormalGenerator::GenerateDirectionalNormalMaskFromDynamicMesh(
		WorkingMesh, EVertexMaskForgeNormalSpace::Local, EVertexMaskForgeNormalDirection::PositiveX, 90.0f, 45.0f, 0.0f, false, FTransform::Identity);
	const FVertexMaskForgeScalarMask ZReference = VertexMaskForgeDirectionalNormalGenerator::GenerateDirectionalNormalMaskFromDynamicMesh(
		WorkingMesh, EVertexMaskForgeNormalSpace::Local, EVertexMaskForgeNormalDirection::PositiveZ, 90.0f, 45.0f, 0.0f, false, FTransform::Identity);
	TestTrue(TEXT("X reference State == Ready"), XReference.State == EVertexMaskForgeScalarMaskState::Ready);
	TestTrue(TEXT("Z reference State == Ready"), ZReference.State == EVertexMaskForgeScalarMaskState::Ready);

	// Forward: X-direction layer first, Z-direction layer last -- Z (folded last) determines the Copy result.
	FVertexMaskForgeDynamicLayerStack ForwardStack;
	AddDirectionalNormalLayer(ForwardStack, TEXT("X"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		EVertexMaskForgeNormalSpace::Local, EVertexMaskForgeNormalDirection::PositiveX, 90.0f, 45.0f, 0.0f, false);
	AddDirectionalNormalLayer(ForwardStack, TEXT("Z"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		EVertexMaskForgeNormalSpace::Local, EVertexMaskForgeNormalDirection::PositiveZ, 90.0f, 45.0f, 0.0f, false);

	// Reverse: same two layers, opposite order -- X (folded last) determines the Copy result instead.
	FVertexMaskForgeDynamicLayerStack ReverseStack;
	AddDirectionalNormalLayer(ReverseStack, TEXT("Z"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		EVertexMaskForgeNormalSpace::Local, EVertexMaskForgeNormalDirection::PositiveZ, 90.0f, 45.0f, 0.0f, false);
	AddDirectionalNormalLayer(ReverseStack, TEXT("X"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		EVertexMaskForgeNormalSpace::Local, EVertexMaskForgeNormalDirection::PositiveX, 90.0f, 45.0f, 0.0f, false);

	TArray<FColor> ForwardOut;
	TArray<FColor> ReverseOut;
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> ForwardOutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ForwardOutThicknessCaches;
	const bool bForwardSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, ForwardStack, BaseColors, FTransform::Identity, ForwardOutAOCaches, ForwardOutThicknessCaches, ForwardOut);
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> ReverseOutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ReverseOutThicknessCaches;
	const bool bReverseSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, ReverseStack, BaseColors, FTransform::Identity, ReverseOutAOCaches, ReverseOutThicknessCaches, ReverseOut);
	TestTrue(TEXT("Forward succeeds"), bForwardSucceeded);
	TestTrue(TEXT("Reverse succeeds"), bReverseSucceeded);
	if (!bForwardSucceeded || !bReverseSucceeded || ForwardOut.Num() != 6 || ReverseOut.Num() != 6)
	{
		return false;
	}

	for (int32 CornerIndex = 0; CornerIndex < 6; ++CornerIndex)
	{
		float ExpectedZValue = 0.0f;
		float ExpectedXValue = 0.0f;
		if (!ZReference.TryGetValue(CornerIndex, ExpectedZValue) || !XReference.TryGetValue(CornerIndex, ExpectedXValue))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("Forward[%d].R matches Z (folded last)"), CornerIndex), ForwardOut[CornerIndex].R, UnitFloatToByte(ExpectedZValue));
		TestEqual(*FString::Printf(TEXT("Reverse[%d].R matches X (folded last)"), CornerIndex), ReverseOut[CornerIndex].R, UnitFloatToByte(ExpectedXValue));
	}

	return true;
}

// E. Generic gating: a disabled Directional Normal layer contributes nothing (Baseline passthrough),
// exactly mirroring BoundingBoxOpacityZeroAndDisabledLayerPreserveBaseColors' own established pattern.
// Genuinely-unsupported-generator coverage (UnsupportedGeneratorTypeFailsWholeCall,
// DisabledLayerContributesNothingAndSkipsValidation) needed no change for Directional Normal's own
// dispatch to be added; it was later updated (M16-K.6D-8G-D) to use Thickness once Ambient Occlusion
// itself became a supported dispatch branch.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompDirNormalDisabledTest, "VertexMaskForge.DynamicSourceTopologyComposition.DirectionalNormalDisabledLayerPreservesBaseColors", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompDirNormalDisabledTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildDirectionalNormalFixtureWorkingMesh();
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = AddDirectionalNormalLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		EVertexMaskForgeNormalSpace::Local, EVertexMaskForgeNormalDirection::PositiveZ, 90.0f, 45.0f, 0.0f, false);
	Stack.SetLayerEnabled(LayerId, false);

	TArray<FColor> Out;
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);
	TestTrue(TEXT("Succeeds"), bSucceeded);
	if (bSucceeded && Out.Num() == 6)
	{
		for (int32 Index = 0; Index < 6; ++Index)
		{
			TestEqual(*FString::Printf(TEXT("Out[%d] byte-exact passthrough (disabled layer = no-op)"), Index), Out[Index], BaseColors[Index]);
		}
	}

	return true;
}

// M16-K.6D-8E-B: A. Dispatch and exact generator parity -- proves the orchestrator's Curvature branch is
// real dispatch (not merely non-empty output) by comparing byte-exact against a direct
// GenerateCurvatureMaskFromDynamicMesh call (its own fresh, independent FVertexMaskForgeGeneratorState),
// resolved through the SAME FixtureCornerToVertexID/TryGetValue correspondence the orchestrator's own
// DynamicMeshVertex-domain Pass 2 path uses -- never a re-derivation of the curvature math itself.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompCurvatureDispatchTest, "VertexMaskForge.DynamicSourceTopologyComposition.CurvatureDispatchByteExactAgainstGeneratorAndVertexDomain", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompCurvatureDispatchTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildCurvatureFixtureWorkingMesh();
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	const EVertexMaskForgeCurvatureType Type = EVertexMaskForgeCurvatureType::Both;
	const float Multiplier = 1.0f;
	const float Blur = 0.0f;
	const float LevelsMin = 0.0f;
	const float LevelsMax = 1.0f;
	const bool bInvert = false;

	// Authoritative, independent reference -- the REAL generator, called directly with its OWN fresh
	// FVertexMaskForgeGeneratorState, never reimplemented.
	FVertexMaskForgeGeneratorState ReferenceGeneratorState;
	const FVertexMaskForgeScalarMask ReferenceMask = VertexMaskForgeCurvatureGenerator::GenerateCurvatureMaskFromDynamicMesh(
		WorkingMesh, ReferenceGeneratorState, Type, Multiplier, Blur, LevelsMin, LevelsMax, bInvert);
	TestTrue(TEXT("Reference generator State == Ready"), ReferenceMask.State == EVertexMaskForgeScalarMaskState::Ready);

	// Non-uniformly-zero proof on the reference itself, before even reaching the orchestrator -- V0/V2
	// (touched by the folded interior edge) must differ from V1/V3 (boundary-only, never accumulated).
	float V0Value = 0.0f, V1Value = 0.0f;
	const bool bHasV0 = ReferenceMask.TryGetValue(0, V0Value);
	const bool bHasV1 = ReferenceMask.TryGetValue(1, V1Value);
	TestTrue(TEXT("Reference mask has a value at V0"), bHasV0);
	TestTrue(TEXT("Reference mask has a value at V1"), bHasV1);
	if (bHasV0 && bHasV1)
	{
		TestNotEqual(TEXT("V0 (folded edge) differs from V1 (boundary-only) -- fixture is non-degenerate"), V0Value, V1Value);
	}

	FVertexMaskForgeDynamicLayerStack Stack;
	AddCurvatureLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		Type, Multiplier, Blur, LevelsMin, LevelsMax, bInvert);

	TArray<FColor> Out;
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);
	TestTrue(TEXT("Orchestrator accepts a Curvature layer"), bSucceeded);
	TestEqual(TEXT("Out.Num() == 6"), Out.Num(), 6);
	if (!bSucceeded || Out.Num() != 6)
	{
		return false;
	}

	// White Fill / Copy / Opacity 1.0 -> RGB == mask value broadcast, the same established contract every
	// prior generator's own dispatch test already proves and reuses.
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
		TestEqual(*FString::Printf(TEXT("Out[%d].R byte-exact vs reference generator + vertex mapping"), CornerIndex), Out[CornerIndex].R, ExpectedByte);
		TestEqual(*FString::Printf(TEXT("Out[%d].G byte-exact vs reference generator + vertex mapping"), CornerIndex), Out[CornerIndex].G, ExpectedByte);
		TestEqual(*FString::Printf(TEXT("Out[%d].B byte-exact vs reference generator + vertex mapping"), CornerIndex), Out[CornerIndex].B, ExpectedByte);
		TestEqual(*FString::Printf(TEXT("Out[%d].A == BaseColors[%d].A"), CornerIndex, CornerIndex), Out[CornerIndex].A, BaseColors[CornerIndex].A);
	}

	return true;
}

// B. Full authoritative parameter forwarding: a small matrix exercising Convex/Concave/Both, a
// non-default Multiplier, a genuinely nonzero FRACTIONAL Blur, non-default Levels Min/Max, and Invert --
// each combination independently proven byte-exact against a direct generator call with the SAME
// parameters (its own fresh FVertexMaskForgeGeneratorState), never by re-deriving the Type-selection/
// Multiplier/blur/Levels/Invert formulas here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompCurvatureParamForwardingTest, "VertexMaskForge.DynamicSourceTopologyComposition.CurvatureParameterForwardingByteExactAgainstGenerator", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompCurvatureParamForwardingTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildCurvatureFixtureWorkingMesh();
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	struct FConfig
	{
		const TCHAR* Label;
		EVertexMaskForgeCurvatureType Type;
		float Multiplier;
		float Blur;
		float LevelsMin;
		float LevelsMax;
		bool bInvert;
	};
	const FConfig Configs[] = {
		{ TEXT("Convex/DefaultMultiplier/NoBlur/DefaultLevels/NoInvert"), EVertexMaskForgeCurvatureType::Convex, 1.0f, 0.0f, 0.0f, 1.0f, false },
		{ TEXT("Concave/DefaultMultiplier/NoBlur/DefaultLevels/NoInvert"), EVertexMaskForgeCurvatureType::Concave, 1.0f, 0.0f, 0.0f, 1.0f, false },
		{ TEXT("Both/NonDefaultMultiplier/FractionalBlur/NonDefaultLevels/Invert"), EVertexMaskForgeCurvatureType::Both, 2.5f, 1.5f, 0.1f, 0.9f, true },
	};

	for (const FConfig& Config : Configs)
	{
		FVertexMaskForgeGeneratorState ReferenceGeneratorState;
		const FVertexMaskForgeScalarMask ReferenceMask = VertexMaskForgeCurvatureGenerator::GenerateCurvatureMaskFromDynamicMesh(
			WorkingMesh, ReferenceGeneratorState, Config.Type, Config.Multiplier, Config.Blur, Config.LevelsMin, Config.LevelsMax, Config.bInvert);
		TestTrue(*FString::Printf(TEXT("%s: reference generator State == Ready"), Config.Label), ReferenceMask.State == EVertexMaskForgeScalarMaskState::Ready);

		FVertexMaskForgeDynamicLayerStack Stack;
		AddCurvatureLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
			Config.Type, Config.Multiplier, Config.Blur, Config.LevelsMin, Config.LevelsMax, Config.bInvert);

		TArray<FColor> Out;
		TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
		TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
		const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);
		TestTrue(*FString::Printf(TEXT("%s: orchestrator succeeds"), Config.Label), bSucceeded);
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
			TestEqual(*FString::Printf(TEXT("%s: Out[%d].R byte-exact"), Config.Label, CornerIndex), Out[CornerIndex].R, ExpectedByte);
		}
	}

	return true;
}

// C. DynamicMesh Vertex domain correctness: corners 0 and 3 share VertexID 0 (both map to V0 per
// FixtureCornerToVertexID); corners 2 and 4 share VertexID 2. A genuinely DynamicMeshVertex-domain
// implementation MUST show identical values at each pair (Curvature has no per-corner attribute input at
// all, unlike Directional Normal's normals -- its value is purely a function of VertexID). An accidental
// Corner-domain implementation would instead read Values[CornerIndex] against an array sized/indexed by
// VertexID (MaxVertexID == 4 here, far smaller than the 6-corner count), which would NOT reproduce this
// equality in general (Values[3] and Values[4] are actually V3's/out-of-range data, not V0's/V2's) --
// this fixture is specifically non-constant (see BuildCurvatureFixtureWorkingMesh's own doc comment) so
// this proof cannot pass by coincidence on a uniformly-zero mask.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompCurvatureVertexDomainTest, "VertexMaskForge.DynamicSourceTopologyComposition.CurvatureDynamicMeshVertexDomainCorrectness", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompCurvatureVertexDomainTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildCurvatureFixtureWorkingMesh();
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	FVertexMaskForgeDynamicLayerStack Stack;
	AddCurvatureLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		EVertexMaskForgeCurvatureType::Both, 0.2f, 0.0f, 0.0f, 1.0f, false);

	TArray<FColor> Out;
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);
	TestTrue(TEXT("Orchestrator succeeds"), bSucceeded);
	if (!bSucceeded || Out.Num() != 6)
	{
		return false;
	}

	TestEqual(TEXT("Corner 3 maps to the same VertexID as corner 0 (fixture precondition)"), FixtureCornerToVertexID[3], FixtureCornerToVertexID[0]);
	TestEqual(TEXT("Corner 4 maps to the same VertexID as corner 2 (fixture precondition)"), FixtureCornerToVertexID[4], FixtureCornerToVertexID[2]);

	TestEqual(TEXT("Corner 0 and corner 3 (same welded VertexID) resolve the identical Curvature value"), Out[0].R, Out[3].R);
	TestEqual(TEXT("Corner 2 and corner 4 (same welded VertexID) resolve the identical Curvature value"), Out[2].R, Out[4].R);

	// Non-constant-mask proof -- V0/V2 (folded) must differ from V1 (boundary-only), so this equality did
	// not pass merely because every value happens to be identical.
	TestNotEqual(TEXT("Output is not a constant mask (corner 0 differs from corner 1)"), Out[0].R, Out[1].R);

	return true;
}

// D. Independent Curvature layers and reorder: two layers with meaningfully different artistic params
// (differing Multiplier, both Type=Both so the shared fixture's fold contributes to both regardless of
// its dihedral sign), Copy/Opacity-1.0 so the LAST-folded layer's own mask determines the result -- the
// same "folded last wins" technique TwoBoundingBoxLayers/TwoDirectionalNormalLayers already establish.
// Each independently-computed reference uses its OWN fresh FVertexMaskForgeGeneratorState (never the
// orchestrator's shared one), proving the composed result matches independent evaluation regardless of
// whether the orchestrator shares raw Curvature internally.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompCurvatureTwoLayersOrderTest, "VertexMaskForge.DynamicSourceTopologyComposition.TwoCurvatureLayersRetainDistinctMasksAndReorderChangesResult", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompCurvatureTwoLayersOrderTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildCurvatureFixtureWorkingMesh();
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	FVertexMaskForgeGeneratorState LowGeneratorState;
	const FVertexMaskForgeScalarMask LowReference = VertexMaskForgeCurvatureGenerator::GenerateCurvatureMaskFromDynamicMesh(
		WorkingMesh, LowGeneratorState, EVertexMaskForgeCurvatureType::Both, 0.2f, 0.0f, 0.0f, 1.0f, false);
	FVertexMaskForgeGeneratorState HighGeneratorState;
	const FVertexMaskForgeScalarMask HighReference = VertexMaskForgeCurvatureGenerator::GenerateCurvatureMaskFromDynamicMesh(
		WorkingMesh, HighGeneratorState, EVertexMaskForgeCurvatureType::Both, 3.0f, 0.0f, 0.0f, 1.0f, false);
	TestTrue(TEXT("Low-Multiplier reference State == Ready"), LowReference.State == EVertexMaskForgeScalarMaskState::Ready);
	TestTrue(TEXT("High-Multiplier reference State == Ready"), HighReference.State == EVertexMaskForgeScalarMaskState::Ready);

	// Forward: Low-Multiplier layer first, High-Multiplier layer last -- High (folded last) determines
	// the Copy result.
	FVertexMaskForgeDynamicLayerStack ForwardStack;
	AddCurvatureLayer(ForwardStack, TEXT("Low"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		EVertexMaskForgeCurvatureType::Both, 0.2f, 0.0f, 0.0f, 1.0f, false);
	AddCurvatureLayer(ForwardStack, TEXT("High"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		EVertexMaskForgeCurvatureType::Both, 3.0f, 0.0f, 0.0f, 1.0f, false);

	// Reverse: same two layers, opposite order -- Low (folded last) determines the Copy result instead.
	FVertexMaskForgeDynamicLayerStack ReverseStack;
	AddCurvatureLayer(ReverseStack, TEXT("High"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		EVertexMaskForgeCurvatureType::Both, 3.0f, 0.0f, 0.0f, 1.0f, false);
	AddCurvatureLayer(ReverseStack, TEXT("Low"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		EVertexMaskForgeCurvatureType::Both, 0.2f, 0.0f, 0.0f, 1.0f, false);

	TArray<FColor> ForwardOut;
	TArray<FColor> ReverseOut;
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> ForwardOutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ForwardOutThicknessCaches;
	const bool bForwardSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, ForwardStack, BaseColors, FTransform::Identity, ForwardOutAOCaches, ForwardOutThicknessCaches, ForwardOut);
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> ReverseOutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ReverseOutThicknessCaches;
	const bool bReverseSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, ReverseStack, BaseColors, FTransform::Identity, ReverseOutAOCaches, ReverseOutThicknessCaches, ReverseOut);
	TestTrue(TEXT("Forward succeeds"), bForwardSucceeded);
	TestTrue(TEXT("Reverse succeeds"), bReverseSucceeded);
	if (!bForwardSucceeded || !bReverseSucceeded || ForwardOut.Num() != 6 || ReverseOut.Num() != 6)
	{
		return false;
	}

	for (int32 CornerIndex = 0; CornerIndex < 6; ++CornerIndex)
	{
		const int32 VertexID = FixtureCornerToVertexID[CornerIndex];
		float ExpectedHighValue = 0.0f;
		float ExpectedLowValue = 0.0f;
		if (!HighReference.TryGetValue(VertexID, ExpectedHighValue) || !LowReference.TryGetValue(VertexID, ExpectedLowValue))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("Forward[%d].R matches High-Multiplier (folded last)"), CornerIndex), ForwardOut[CornerIndex].R, UnitFloatToByte(ExpectedHighValue));
		TestEqual(*FString::Printf(TEXT("Reverse[%d].R matches Low-Multiplier (folded last)"), CornerIndex), ReverseOut[CornerIndex].R, UnitFloatToByte(ExpectedLowValue));
	}

	// Distinctness precondition -- if Low and High produced identical masks the test above would pass
	// vacuously; confirm the two Multipliers genuinely diverge at the folded vertex.
	float LowV0 = 0.0f, HighV0 = 0.0f;
	if (LowReference.TryGetValue(0, LowV0) && HighReference.TryGetValue(0, HighV0))
	{
		TestNotEqual(TEXT("Low-Multiplier and High-Multiplier references genuinely differ at V0"), LowV0, HighV0);
	}

	return true;
}

// E. Generic gating: a disabled Curvature layer contributes nothing (Baseline passthrough), exactly
// mirroring DirectionalNormalDisabledLayerPreservesBaseColors' own established pattern. Work avoidance
// (Curvature generation never invoked for a disabled layer) is proven structurally by the existing
// pre-dispatch `if (!Layer.bEnabled || !Layer.Mask.IsSet()) continue;` early-out in Pass 1 -- confirmed by
// direct source inspection, not by production instrumentation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompCurvatureDisabledTest, "VertexMaskForge.DynamicSourceTopologyComposition.CurvatureDisabledLayerPreservesBaseColors", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompCurvatureDisabledTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildCurvatureFixtureWorkingMesh();
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = AddCurvatureLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		EVertexMaskForgeCurvatureType::Both, 0.2f, 0.0f, 0.0f, 1.0f, false);
	Stack.SetLayerEnabled(LayerId, false);

	TArray<FColor> Out;
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);
	TestTrue(TEXT("Succeeds"), bSucceeded);
	if (bSucceeded && Out.Num() == 6)
	{
		for (int32 Index = 0; Index < 6; ++Index)
		{
			TestEqual(*FString::Printf(TEXT("Out[%d] byte-exact passthrough (disabled layer = no-op)"), Index), Out[Index], BaseColors[Index]);
		}
	}

	return true;
}

// F. Shared-state safety / no cross-layer contamination: evaluates three DISTINCT Curvature parameter
// sets against ONE shared FVertexMaskForgeGeneratorState, in the same sequential order the orchestrator's
// own Pass 1 loop would use, capturing each completed mask BY VALUE immediately after its own call --
// then independently recomputes the same three parameter sets, each against its OWN fresh
// FVertexMaskForgeGeneratorState. If shared-state reuse ever leaked one layer's artistic result into
// another's, or if a later call ever mutated an earlier layer's already-captured mask (both explicitly
// guarded against by GenerateCurvatureMaskFromDynamicMesh returning its FVertexMaskForgeScalarMask
// entirely by value -- see the Pass 1 dispatch's own doc comment), the shared-state and independent-state
// results would diverge. This does not by itself prove an internal cache HIT occurred (that is confirmed
// by source inspection of EnsureCurvatureRawCache's fingerprint gate and the orchestrator's own single
// CurvatureLocalGeneratorState declaration, not by this test) -- it proves output correctness/isolation
// regardless of whether sharing occurred.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompCurvatureSharedStateSafetyTest, "VertexMaskForge.DynamicSourceTopologyComposition.CurvatureSharedGeneratorStateProducesIndependentPerLayerResults", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompCurvatureSharedStateSafetyTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildCurvatureFixtureWorkingMesh();

	struct FConfig
	{
		const TCHAR* Label;
		EVertexMaskForgeCurvatureType Type;
		float Multiplier;
		float Blur;
		float LevelsMin;
		float LevelsMax;
		bool bInvert;
	};
	const FConfig Configs[] = {
		{ TEXT("Concave/Mult1/NoBlur"), EVertexMaskForgeCurvatureType::Concave, 1.0f, 0.0f, 0.0f, 1.0f, false },
		{ TEXT("Convex/Mult2/Blur1.5"), EVertexMaskForgeCurvatureType::Convex, 2.0f, 1.5f, 0.0f, 1.0f, false },
		{ TEXT("Both/Mult1/Levels0.2-0.8/Invert"), EVertexMaskForgeCurvatureType::Both, 1.0f, 0.0f, 0.2f, 0.8f, true },
	};

	// Sequential evaluation against ONE shared state, mirroring the orchestrator's own Pass 1 reuse --
	// each result captured BY VALUE (MoveTemp) immediately, so no later call can retroactively mutate an
	// earlier entry even if the underlying representation somehow aliased shared storage.
	FVertexMaskForgeGeneratorState SharedGeneratorState;
	TArray<FVertexMaskForgeScalarMask> SharedResults;
	for (const FConfig& Config : Configs)
	{
		FVertexMaskForgeScalarMask Result = VertexMaskForgeCurvatureGenerator::GenerateCurvatureMaskFromDynamicMesh(
			WorkingMesh, SharedGeneratorState, Config.Type, Config.Multiplier, Config.Blur, Config.LevelsMin, Config.LevelsMax, Config.bInvert);
		TestTrue(*FString::Printf(TEXT("%s: shared-state result State == Ready"), Config.Label), Result.State == EVertexMaskForgeScalarMaskState::Ready);
		SharedResults.Add(MoveTemp(Result));
	}

	// Independent evaluation: each config against its OWN fresh state, never reusing SharedGeneratorState.
	for (int32 ConfigIndex = 0; ConfigIndex < UE_ARRAY_COUNT(Configs); ++ConfigIndex)
	{
		const FConfig& Config = Configs[ConfigIndex];
		FVertexMaskForgeGeneratorState IndependentGeneratorState;
		const FVertexMaskForgeScalarMask IndependentResult = VertexMaskForgeCurvatureGenerator::GenerateCurvatureMaskFromDynamicMesh(
			WorkingMesh, IndependentGeneratorState, Config.Type, Config.Multiplier, Config.Blur, Config.LevelsMin, Config.LevelsMax, Config.bInvert);
		TestTrue(*FString::Printf(TEXT("%s: independent-state result State == Ready"), Config.Label), IndependentResult.State == EVertexMaskForgeScalarMaskState::Ready);

		const FVertexMaskForgeScalarMask& SharedResult = SharedResults[ConfigIndex];
		for (const int32 VertexID : WorkingMesh.Mesh->VertexIndicesItr())
		{
			float SharedValue = 0.0f;
			float IndependentValue = 0.0f;
			const bool bSharedHasValue = SharedResult.TryGetValue(VertexID, SharedValue);
			const bool bIndependentHasValue = IndependentResult.TryGetValue(VertexID, IndependentValue);
			TestEqual(*FString::Printf(TEXT("%s: VertexID %d has-value agrees between shared and independent state"), Config.Label, VertexID), bSharedHasValue, bIndependentHasValue);
			if (bSharedHasValue && bIndependentHasValue)
			{
				TestEqual(*FString::Printf(TEXT("%s: VertexID %d value byte-exact between shared and independent state"), Config.Label, VertexID), UnitFloatToByte(SharedValue), UnitFloatToByte(IndependentValue));
			}
		}
	}

	return true;
}

// M16-K.6D-8F-B: 1. Dispatch and exact generator parity -- proves the orchestrator's Noise branch is real
// dispatch (not merely non-empty output) by comparing byte-exact against a direct
// GenerateNoiseMaskFromDynamicMesh call (its own fresh, independent FVertexMaskForgeGeneratorState),
// resolved through the SAME FixtureCornerToVertexID/TryGetValue correspondence the orchestrator's own
// DynamicMeshVertex-domain Pass 2 path uses.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompNoiseDispatchTest, "VertexMaskForge.DynamicSourceTopologyComposition.NoiseDispatchMatchesDirectGenerator", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompNoiseDispatchTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildZVaryingFixtureWorkingMesh();
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	FVertexMaskForgeNoiseParams NoiseParams = MakeNoiseBaselineParams();
	NoiseParams.ScaleX = 3.0f;
	NoiseParams.ScaleY = 5.0f;
	NoiseParams.ScaleZ = 7.0f;
	NoiseParams.OffsetX = 11.0f;
	NoiseParams.OffsetY = 13.0f;
	NoiseParams.OffsetZ = 17.0f;
	NoiseParams.Seed = 42;
	NoiseParams.Octaves = 3;
	NoiseParams.Roughness = 0.6f;
	NoiseParams.Lacunarity = 2.5f;

	// Authoritative, independent reference -- the REAL generator, called directly with its OWN fresh
	// FVertexMaskForgeGeneratorState, never reimplemented.
	FVertexMaskForgeGeneratorState ReferenceState;
	const FVertexMaskForgeScalarMask ReferenceMask = VertexMaskForgeNoiseGenerator::GenerateNoiseMaskFromDynamicMesh(
		WorkingMesh, ReferenceState, MakeNoiseGenerativeParams(NoiseParams), NoiseParams.Multiplier, NoiseParams.LevelsMin, NoiseParams.LevelsMax, NoiseParams.bInvert);
	TestTrue(TEXT("Reference generator State == Ready"), ReferenceMask.State == EVertexMaskForgeScalarMaskState::Ready);

	FVertexMaskForgeDynamicLayerStack Stack;
	AddNoiseLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, NoiseParams);

	TArray<FColor> Out;
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);
	TestTrue(TEXT("Orchestrator accepts a Noise layer"), bSucceeded);
	TestEqual(TEXT("Out.Num() == 6"), Out.Num(), 6);
	if (!bSucceeded || Out.Num() != 6)
	{
		return false;
	}

	// White Fill / Copy / Opacity 1.0 -> RGB == mask value broadcast, the same established contract every
	// prior generator's own dispatch test already proves and reuses.
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
		TestEqual(*FString::Printf(TEXT("Out[%d].R byte-exact vs reference generator + vertex mapping"), CornerIndex), Out[CornerIndex].R, ExpectedByte);
		TestEqual(*FString::Printf(TEXT("Out[%d].G byte-exact vs reference generator + vertex mapping"), CornerIndex), Out[CornerIndex].G, ExpectedByte);
		TestEqual(*FString::Printf(TEXT("Out[%d].B byte-exact vs reference generator + vertex mapping"), CornerIndex), Out[CornerIndex].B, ExpectedByte);
		TestEqual(*FString::Printf(TEXT("Out[%d].A == BaseColors[%d].A"), CornerIndex, CornerIndex), Out[CornerIndex].A, BaseColors[CornerIndex].A);
	}

	return true;
}

// 2. Full authoritative parameter forwarding: a baseline plus 17 single-field-modified configs (all raw/
// generative fields: Type, ScaleX/Y/Z, OffsetX/Y/Z, Seed, Octaves, Roughness, Lacunarity,
// TurbulenceStrength, Blur; all artistic fields: Multiplier, LevelsMin, LevelsMax, Invert), each
// independently proven byte-exact against a direct generator call using the SAME NoiseParams -> if the
// orchestrator's own field-copy ever drops or mis-maps one field, that config's own reference (built from
// the actual modified value) would diverge from the orchestrator's actual internal (wrongly-defaulted)
// behavior, and the byte-exact comparison below would catch it. TurbulenceStrength's own config uses
// Type=Turbulence specifically, since that field is otherwise harmless/unused for every other Noise Type.
// Multiplier uses a sub-1.0 value to avoid the same clamp-saturation collision already discovered and
// fixed for Curvature's own equivalent test in M16-K.6D-8E-B.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompNoiseParamForwardingTest, "VertexMaskForge.DynamicSourceTopologyComposition.NoiseForwardsAllAuthoritativeParameters", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompNoiseParamForwardingTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildZVaryingFixtureWorkingMesh();
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();
	const FVertexMaskForgeNoiseParams Baseline = MakeNoiseBaselineParams();

	struct FConfig
	{
		const TCHAR* Label;
		FVertexMaskForgeNoiseParams Params;
	};
	TArray<FConfig> Configs;

	{ FConfig C{ TEXT("Type"), Baseline }; C.Params.Type = EVertexMaskForgeNoiseType::Perlin; Configs.Add(C); }
	{ FConfig C{ TEXT("ScaleX"), Baseline }; C.Params.ScaleX = 9.0f; Configs.Add(C); }
	{ FConfig C{ TEXT("ScaleY"), Baseline }; C.Params.ScaleY = 9.0f; Configs.Add(C); }
	{ FConfig C{ TEXT("ScaleZ"), Baseline }; C.Params.ScaleZ = 9.0f; Configs.Add(C); }
	{ FConfig C{ TEXT("OffsetX"), Baseline }; C.Params.OffsetX = 37.0f; Configs.Add(C); }
	{ FConfig C{ TEXT("OffsetY"), Baseline }; C.Params.OffsetY = 41.0f; Configs.Add(C); }
	{ FConfig C{ TEXT("OffsetZ"), Baseline }; C.Params.OffsetZ = 53.0f; Configs.Add(C); }
	{ FConfig C{ TEXT("Seed"), Baseline }; C.Params.Seed = 12345; Configs.Add(C); }
	{ FConfig C{ TEXT("Octaves"), Baseline }; C.Params.Octaves = 1; Configs.Add(C); }
	{ FConfig C{ TEXT("Roughness"), Baseline }; C.Params.Roughness = 0.1f; Configs.Add(C); }
	{ FConfig C{ TEXT("Lacunarity"), Baseline }; C.Params.Lacunarity = 4.0f; Configs.Add(C); }
	{ FConfig C{ TEXT("TurbulenceStrength"), Baseline }; C.Params.Type = EVertexMaskForgeNoiseType::Turbulence; C.Params.TurbulenceStrength = 3.0f; Configs.Add(C); }
	{ FConfig C{ TEXT("Blur"), Baseline }; C.Params.Blur = 0.6f; Configs.Add(C); }
	{ FConfig C{ TEXT("Multiplier"), Baseline }; C.Params.Multiplier = 0.37f; Configs.Add(C); }
	{ FConfig C{ TEXT("LevelsMin"), Baseline }; C.Params.LevelsMin = 0.3f; Configs.Add(C); }
	{ FConfig C{ TEXT("LevelsMax"), Baseline }; C.Params.LevelsMax = 0.7f; Configs.Add(C); }
	{ FConfig C{ TEXT("Invert"), Baseline }; C.Params.bInvert = true; Configs.Add(C); }

	for (const FConfig& Config : Configs)
	{
		FVertexMaskForgeGeneratorState ReferenceState;
		const FVertexMaskForgeScalarMask ReferenceMask = VertexMaskForgeNoiseGenerator::GenerateNoiseMaskFromDynamicMesh(
			WorkingMesh, ReferenceState, MakeNoiseGenerativeParams(Config.Params), Config.Params.Multiplier, Config.Params.LevelsMin, Config.Params.LevelsMax, Config.Params.bInvert);
		TestTrue(*FString::Printf(TEXT("%s: reference generator State == Ready"), Config.Label), ReferenceMask.State == EVertexMaskForgeScalarMaskState::Ready);

		FVertexMaskForgeDynamicLayerStack Stack;
		AddNoiseLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, Config.Params);

		TArray<FColor> Out;
		TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
		TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
		const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);
		TestTrue(*FString::Printf(TEXT("%s: orchestrator succeeds"), Config.Label), bSucceeded);
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
			TestEqual(*FString::Printf(TEXT("%s: Out[%d].R byte-exact"), Config.Label, CornerIndex), Out[CornerIndex].R, ExpectedByte);
		}
	}

	return true;
}

// 3. Mismatched tagged payload: AUDITED (M16-K.6D-8F-B; renamed M16-K.6D-8F-B.1 -- see that checkpoint's
// own report for the full rationale). This test does NOT deliver a mismatched payload to the orchestrator
// and does NOT prove orchestrator-level rejection -- a true GeneratorType==Noise/Params!=
// FVertexMaskForgeNoiseParams mismatch is structurally UNREACHABLE through
// FVertexMaskForgeDynamicLayerStack's own public API -- Layers is private, GetLayers() returns only a
// const reference, and SetLayerMaskParams itself refuses (returns false, stack left completely
// unmodified) any Params whose active TVariant alternative does not match the mask's stored GeneratorType
// (see that function's own doc contract, already generically proven by
// VertexMaskForge.GeneratorMaskInstance.SetParamsFailsOnParamsTypeMismatch). This is the SAME structural
// guarantee that already makes Material Slot's/Bounding Box's/Directional Normal's/Curvature's own
// identical defensive TryGet checks in the orchestrator provably unreachable in production (each is
// commented "should be unreachable... but never assumed") -- none of those four generators has a
// dedicated mismatch test in this file either, for the same reason. No test-only production hook was
// added to force a lower-level incoherent state. What this test actually proves, precisely matching its
// own registered name: the Dynamic LAYER STACK rejects an attempt to write a mismatched (Curvature)
// payload onto an already-Noise-tagged mask instance -- SetLayerMaskParams returns false, the layer's
// Noise payload remains completely unchanged -- and the orchestrator, run afterward against that still-
// coherent, never-actually-mismatched stack, continues to succeed normally (no crash, no fallback, no
// partial composition). It does not exercise the orchestrator's own Noise TryGet defensive branch, which
// remains untested for the same "unreachable in production" reason the other four generators' identical
// branches are untested.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompNoiseMismatchRejectedByStackTest, "VertexMaskForge.DynamicSourceTopologyComposition.NoiseMismatchedTaggedPayloadRejectedByLayerStackBeforeOrchestration", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompNoiseMismatchRejectedByStackTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildZVaryingFixtureWorkingMesh();
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = AddNoiseLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, MakeNoiseBaselineParams());

	const FVertexMaskForgeGeneratorMaskInstance* MaskInstance = Stack.GetLayerMask(LayerId);
	check(MaskInstance);
	const FGuid ExpectedMaskInstanceId = MaskInstance->MaskInstanceId;

	// Attempt to overwrite the Noise-tagged mask instance's Params with a Curvature payload -- must fail.
	const FVertexMaskForgeGeneratorParams MismatchedParams = MakeVertexMaskForgeGeneratorParams(EVertexMaskForgeGeneratorType::Curvature);
	const bool bMismatchedWriteSucceeded = Stack.SetLayerMaskParams(LayerId, ExpectedMaskInstanceId, MismatchedParams);
	TestFalse(TEXT("SetLayerMaskParams rejects a Curvature payload for a Noise-tagged mask instance"), bMismatchedWriteSucceeded);

	const FVertexMaskForgeGeneratorMaskInstance* MaskAfterRejectedWrite = Stack.GetLayerMask(LayerId);
	check(MaskAfterRejectedWrite);
	TestTrue(TEXT("Mask remains Noise-tagged after the rejected write"), MaskAfterRejectedWrite->GeneratorType == EVertexMaskForgeGeneratorType::Noise);
	TestTrue(TEXT("Params remain the Noise payload type after the rejected write"), MaskAfterRejectedWrite->Params.IsType<FVertexMaskForgeNoiseParams>());

	// The stack therefore remains fully coherent -- the orchestrator succeeds normally against it, never
	// crashing, never falling back, never partially composing.
	TArray<FColor> Out;
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);
	TestTrue(TEXT("Orchestrator succeeds against the still-coherent stack"), bSucceeded);
	TestEqual(TEXT("Out.Num() == 6"), Out.Num(), 6);

	return true;
}

// 4. DynamicMesh Vertex domain correctness: corners 0 and 3 share VertexID 0 (both map to V0 per
// FixtureCornerToVertexID); corners 2 and 4 share VertexID 2. A genuinely DynamicMeshVertex-domain
// implementation MUST show identical values at each pair (Noise has no per-corner attribute input at all
// -- its value is purely a function of VertexID position). An accidental Corner-domain implementation
// would instead read Values[CornerIndex] against an array sized/indexed by VertexID, which would NOT
// reproduce this equality in general.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompNoiseVertexDomainTest, "VertexMaskForge.DynamicSourceTopologyComposition.NoiseUsesDynamicMeshVertexDomainAcrossWeldedRenderCorners", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompNoiseVertexDomainTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildZVaryingFixtureWorkingMesh();
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	FVertexMaskForgeDynamicLayerStack Stack;
	AddNoiseLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, MakeNoiseBaselineParams());

	TArray<FColor> Out;
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);
	TestTrue(TEXT("Orchestrator succeeds"), bSucceeded);
	if (!bSucceeded || Out.Num() != 6)
	{
		return false;
	}

	TestEqual(TEXT("Corner 3 maps to the same VertexID as corner 0 (fixture precondition)"), FixtureCornerToVertexID[3], FixtureCornerToVertexID[0]);
	TestEqual(TEXT("Corner 4 maps to the same VertexID as corner 2 (fixture precondition)"), FixtureCornerToVertexID[4], FixtureCornerToVertexID[2]);

	TestEqual(TEXT("Corner 0 and corner 3 (same welded VertexID) resolve the identical Noise value"), Out[0].R, Out[3].R);
	TestEqual(TEXT("Corner 2 and corner 4 (same welded VertexID) resolve the identical Noise value"), Out[2].R, Out[4].R);

	// Non-constant-mask proof -- V0..V3 have genuinely distinct positions in this fixture (see
	// BuildZVaryingFixtureWorkingMesh's own doc comment), so Noise (a position-derived generator) must not
	// be a uniformly-constant mask across all six corners.
	TestFalse(TEXT("Output is not a constant mask across all six corners"),
		Out[0].R == Out[1].R && Out[1].R == Out[2].R && Out[2].R == Out[3].R && Out[3].R == Out[4].R && Out[4].R == Out[5].R);

	// Independently represented vertex retains its own value -- V1 (corner 1, touched by no other corner)
	// need not equal V0's value.
	TestNotEqual(TEXT("Corner 1 (independent VertexID 1) is not forced to equal corner 0's value"), Out[1].R, Out[0].R);

	return true;
}

// 5. Independent Noise layers and reorder: two layers with meaningfully different generative parameters
// (different Seed and Scale), Copy/Opacity-1.0 so the LAST-folded layer's own mask determines the
// result -- the same "folded last wins" technique every prior generator's own two-layer test already
// establishes. Each independently-computed reference uses its OWN fresh FVertexMaskForgeGeneratorState.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompNoiseTwoLayersOrderTest, "VertexMaskForge.DynamicSourceTopologyComposition.TwoNoiseLayersRetainDistinctMasksAndReorderChangesResult", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompNoiseTwoLayersOrderTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildZVaryingFixtureWorkingMesh();
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	FVertexMaskForgeNoiseParams ParamsA = MakeNoiseBaselineParams();
	ParamsA.Seed = 111;
	FVertexMaskForgeNoiseParams ParamsB = MakeNoiseBaselineParams();
	ParamsB.Seed = 222;
	ParamsB.ScaleX = 6.0f;

	FVertexMaskForgeGeneratorState RefStateA;
	const FVertexMaskForgeScalarMask ReferenceA = VertexMaskForgeNoiseGenerator::GenerateNoiseMaskFromDynamicMesh(
		WorkingMesh, RefStateA, MakeNoiseGenerativeParams(ParamsA), ParamsA.Multiplier, ParamsA.LevelsMin, ParamsA.LevelsMax, ParamsA.bInvert);
	FVertexMaskForgeGeneratorState RefStateB;
	const FVertexMaskForgeScalarMask ReferenceB = VertexMaskForgeNoiseGenerator::GenerateNoiseMaskFromDynamicMesh(
		WorkingMesh, RefStateB, MakeNoiseGenerativeParams(ParamsB), ParamsB.Multiplier, ParamsB.LevelsMin, ParamsB.LevelsMax, ParamsB.bInvert);
	TestTrue(TEXT("Reference A State == Ready"), ReferenceA.State == EVertexMaskForgeScalarMaskState::Ready);
	TestTrue(TEXT("Reference B State == Ready"), ReferenceB.State == EVertexMaskForgeScalarMaskState::Ready);

	// Forward: A first, B last -- B (folded last) determines the Copy result.
	FVertexMaskForgeDynamicLayerStack ForwardStack;
	AddNoiseLayer(ForwardStack, TEXT("A"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, ParamsA);
	AddNoiseLayer(ForwardStack, TEXT("B"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, ParamsB);

	// Reverse: same two layers, opposite order -- A (folded last) determines the Copy result instead.
	FVertexMaskForgeDynamicLayerStack ReverseStack;
	AddNoiseLayer(ReverseStack, TEXT("B"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, ParamsB);
	AddNoiseLayer(ReverseStack, TEXT("A"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, ParamsA);

	TArray<FColor> ForwardOut;
	TArray<FColor> ReverseOut;
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> ForwardOutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ForwardOutThicknessCaches;
	const bool bForwardSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, ForwardStack, BaseColors, FTransform::Identity, ForwardOutAOCaches, ForwardOutThicknessCaches, ForwardOut);
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> ReverseOutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ReverseOutThicknessCaches;
	const bool bReverseSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, ReverseStack, BaseColors, FTransform::Identity, ReverseOutAOCaches, ReverseOutThicknessCaches, ReverseOut);
	TestTrue(TEXT("Forward succeeds"), bForwardSucceeded);
	TestTrue(TEXT("Reverse succeeds"), bReverseSucceeded);
	if (!bForwardSucceeded || !bReverseSucceeded || ForwardOut.Num() != 6 || ReverseOut.Num() != 6)
	{
		return false;
	}

	for (int32 CornerIndex = 0; CornerIndex < 6; ++CornerIndex)
	{
		const int32 VertexID = FixtureCornerToVertexID[CornerIndex];
		float ExpectedBValue = 0.0f;
		float ExpectedAValue = 0.0f;
		if (!ReferenceB.TryGetValue(VertexID, ExpectedBValue) || !ReferenceA.TryGetValue(VertexID, ExpectedAValue))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("Forward[%d].R matches B (folded last)"), CornerIndex), ForwardOut[CornerIndex].R, UnitFloatToByte(ExpectedBValue));
		TestEqual(*FString::Printf(TEXT("Reverse[%d].R matches A (folded last)"), CornerIndex), ReverseOut[CornerIndex].R, UnitFloatToByte(ExpectedAValue));
	}

	// Distinctness precondition -- if A and B produced identical masks the test above would pass
	// vacuously; confirm the two configs genuinely diverge at V0.
	float AV0 = 0.0f, BV0 = 0.0f;
	if (ReferenceA.TryGetValue(0, AV0) && ReferenceB.TryGetValue(0, BV0))
	{
		TestNotEqual(TEXT("A and B references genuinely differ at V0"), AV0, BV0);
	}

	return true;
}

// 6. Generic gating: a disabled Noise layer contributes nothing (Baseline passthrough), exactly mirroring
// CurvatureDisabledLayerPreservesBaseColors' own established pattern. Work avoidance (Noise generation
// never invoked for a disabled layer) is proven structurally by the existing pre-dispatch
// `if (!Layer.bEnabled || !Layer.Mask.IsSet()) continue;` early-out in Pass 1 -- confirmed by direct source
// inspection, not by production instrumentation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompNoiseDisabledTest, "VertexMaskForge.DynamicSourceTopologyComposition.DisabledNoiseLayerIsNoOp", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompNoiseDisabledTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildZVaryingFixtureWorkingMesh();
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = AddNoiseLayer(Stack, TEXT("Layer"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, MakeNoiseBaselineParams());
	Stack.SetLayerEnabled(LayerId, false);

	TArray<FColor> Out;
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);
	TestTrue(TEXT("Succeeds"), bSucceeded);
	if (bSucceeded && Out.Num() == 6)
	{
		for (int32 Index = 0; Index < 6; ++Index)
		{
			TestEqual(*FString::Printf(TEXT("Out[%d] byte-exact passthrough (disabled layer = no-op)"), Index), Out[Index], BaseColors[Index]);
		}
	}

	return true;
}

// 7. Independent per-layer generator state / no cross-layer contamination: three enabled Noise layers,
// each with meaningfully DIFFERENT generative configurations (different Seed/Scale/Type), each isolated to
// its own RGB channel via SetLayerChannelFilter so the orchestrator's own multi-layer composed OUTPUT
// (not merely a standalone helper call) can be compared directly, per channel, against three fully
// independent direct-generator reference calls. UNLIKE Curvature's own shared-state-safety test (which
// proves a SHARED state produces correct results), this proves the opposite design: since Noise layers use
// INDEPENDENT per-layer transient state (never shared), evaluation order must never let a later layer's
// state affect an earlier layer's already-composed contribution, and a genuinely different generative
// configuration must never accidentally reuse another layer's raw pattern.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompNoiseIndependentStateTest, "VertexMaskForge.DynamicSourceTopologyComposition.MultipleNoiseLayersUseIndependentGeneratorState", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompNoiseIndependentStateTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildZVaryingFixtureWorkingMesh();
	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	FVertexMaskForgeNoiseParams ParamsA = MakeNoiseBaselineParams();
	ParamsA.Seed = 10;
	ParamsA.Type = EVertexMaskForgeNoiseType::Perlin;
	FVertexMaskForgeNoiseParams ParamsB = MakeNoiseBaselineParams();
	ParamsB.Seed = 20;
	ParamsB.ScaleX = 6.0f;
	FVertexMaskForgeNoiseParams ParamsC = MakeNoiseBaselineParams();
	ParamsC.Seed = 30;
	ParamsC.Type = EVertexMaskForgeNoiseType::Ridged;

	FVertexMaskForgeGeneratorState RefStateA;
	const FVertexMaskForgeScalarMask ReferenceA = VertexMaskForgeNoiseGenerator::GenerateNoiseMaskFromDynamicMesh(
		WorkingMesh, RefStateA, MakeNoiseGenerativeParams(ParamsA), ParamsA.Multiplier, ParamsA.LevelsMin, ParamsA.LevelsMax, ParamsA.bInvert);
	FVertexMaskForgeGeneratorState RefStateB;
	const FVertexMaskForgeScalarMask ReferenceB = VertexMaskForgeNoiseGenerator::GenerateNoiseMaskFromDynamicMesh(
		WorkingMesh, RefStateB, MakeNoiseGenerativeParams(ParamsB), ParamsB.Multiplier, ParamsB.LevelsMin, ParamsB.LevelsMax, ParamsB.bInvert);
	FVertexMaskForgeGeneratorState RefStateC;
	const FVertexMaskForgeScalarMask ReferenceC = VertexMaskForgeNoiseGenerator::GenerateNoiseMaskFromDynamicMesh(
		WorkingMesh, RefStateC, MakeNoiseGenerativeParams(ParamsC), ParamsC.Multiplier, ParamsC.LevelsMin, ParamsC.LevelsMax, ParamsC.bInvert);
	TestTrue(TEXT("Reference A/B/C all Ready"),
		ReferenceA.State == EVertexMaskForgeScalarMaskState::Ready
		&& ReferenceB.State == EVertexMaskForgeScalarMaskState::Ready
		&& ReferenceC.State == EVertexMaskForgeScalarMaskState::Ready);

	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerA = AddNoiseLayer(Stack, TEXT("A"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, ParamsA);
	const FGuid LayerB = AddNoiseLayer(Stack, TEXT("B"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, ParamsB);
	const FGuid LayerC = AddNoiseLayer(Stack, TEXT("C"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, ParamsC);
	Stack.SetLayerChannelFilter(LayerA, /*bAffectRed=*/true, /*bAffectGreen=*/false, /*bAffectBlue=*/false);
	Stack.SetLayerChannelFilter(LayerB, /*bAffectRed=*/false, /*bAffectGreen=*/true, /*bAffectBlue=*/false);
	Stack.SetLayerChannelFilter(LayerC, /*bAffectRed=*/false, /*bAffectGreen=*/false, /*bAffectBlue=*/true);

	TArray<FColor> Out;
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);
	TestTrue(TEXT("Orchestrator succeeds with three independent Noise layers in one invocation"), bSucceeded);
	if (!bSucceeded || Out.Num() != 6)
	{
		return false;
	}

	for (int32 CornerIndex = 0; CornerIndex < 6; ++CornerIndex)
	{
		const int32 VertexID = FixtureCornerToVertexID[CornerIndex];
		float ExpectedA = 0.0f, ExpectedB = 0.0f, ExpectedC = 0.0f;
		if (!ReferenceA.TryGetValue(VertexID, ExpectedA) || !ReferenceB.TryGetValue(VertexID, ExpectedB) || !ReferenceC.TryGetValue(VertexID, ExpectedC))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("Out[%d].R matches independent Layer A (not contaminated by B/C)"), CornerIndex), Out[CornerIndex].R, UnitFloatToByte(ExpectedA));
		TestEqual(*FString::Printf(TEXT("Out[%d].G matches independent Layer B (not contaminated by A/C)"), CornerIndex), Out[CornerIndex].G, UnitFloatToByte(ExpectedB));
		TestEqual(*FString::Printf(TEXT("Out[%d].B matches independent Layer C (not contaminated by A/B)"), CornerIndex), Out[CornerIndex].B, UnitFloatToByte(ExpectedC));
	}

	return true;
}

// M16-K.6D-8G-E, AO-1/AO-6: a real, enabled Ambient Occlusion layer dispatched through the Dynamic
// orchestrator, byte-exact against the direct, independent oracle generator call (own cold cache, same
// per-corner Normal Overlay Element ID resolved via NormalOverlay->GetTriangle(TriangleID)[Corner] --
// never Mesh->GetTriangle, never a positional VertexID/ElementID assumption). Also proves hard-edge
// domain resolution (5.6): V0's two elements (corner 0 vs corner 3) and V2's two elements (corner 2 vs
// corner 4) are asserted to produce GENUINELY DIFFERENT raw occlusion via the oracle itself (not assumed)
// before checking that the orchestrator's own per-corner output correctly reflects each corner's OWN
// element, never a shared/vertex-averaged value.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompAODispatchTest, "VertexMaskForge.DynamicSourceTopologyComposition.AmbientOcclusionDispatchByteExactAgainstGeneratorAndElementDomain", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompAODispatchTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildAOFoldFixtureWorkingMesh();
	const FVertexMaskForgeAOParams OracleParams = MakeOracleAOParams(/*Samples=*/32, /*MaxDistance=*/100.0f, /*Bias=*/0.05f, 0.0f, 1.0f, false);

	TUniquePtr<FVertexMaskForgeSourceTopologyAOCache> OracleCachePtr;
	const FVertexMaskForgeScalarMask ReferenceMask = VertexMaskForgeAmbientOcclusionGenerator::GenerateAmbientOcclusionMaskFromDynamicMesh(
		OracleCachePtr, *WorkingMesh.Mesh, WorkingMesh.GeometryFingerprint, FTransform::Identity, OracleParams);
	TestTrue(TEXT("Oracle generator State == Ready"), ReferenceMask.State == EVertexMaskForgeScalarMaskState::Ready);
	if (ReferenceMask.State != EVertexMaskForgeScalarMaskState::Ready)
	{
		return false;
	}

	// Hard-edge distinctness (5.6): resolve the ACTUAL per-corner Element IDs the same way Legacy's own
	// UpdateWorkingColorsSourceTopology does (NormalOverlay->GetTriangle(TriangleID)[Corner]), then prove
	// V0's two elements (corners 0/3) and V2's two elements (corners 2/4) genuinely differ in the oracle's
	// own raw output -- a real geometric/raycast fact, never assumed.
	const UE::Geometry::FDynamicMeshNormalOverlay* NormalOverlay = WorkingMesh.Mesh->Attributes()->PrimaryNormals();
	int32 ElementIDByCorner[6];
	{
		int32 RunningCorner = 0;
		for (const int32 TriangleID : WorkingMesh.Mesh->TriangleIndicesItr())
		{
			const UE::Geometry::FIndex3i TriElements = NormalOverlay->GetTriangle(TriangleID);
			ElementIDByCorner[RunningCorner++] = TriElements.A;
			ElementIDByCorner[RunningCorner++] = TriElements.B;
			ElementIDByCorner[RunningCorner++] = TriElements.C;
		}
	}
	TestFalse(TEXT("Corner 0 and corner 3 (both at V0) resolve to DIFFERENT Normal Overlay Element IDs"), ElementIDByCorner[0] == ElementIDByCorner[3]);
	TestFalse(TEXT("Corner 2 and corner 4 (both at V2) resolve to DIFFERENT Normal Overlay Element IDs"), ElementIDByCorner[2] == ElementIDByCorner[4]);

	float Value0 = 0.0f, Value3 = 0.0f, Value2 = 0.0f, Value4 = 0.0f;
	const bool bHas0 = ReferenceMask.TryGetValue(ElementIDByCorner[0], Value0);
	const bool bHas3 = ReferenceMask.TryGetValue(ElementIDByCorner[3], Value3);
	const bool bHas2 = ReferenceMask.TryGetValue(ElementIDByCorner[2], Value2);
	const bool bHas4 = ReferenceMask.TryGetValue(ElementIDByCorner[4], Value4);
	TestTrue(TEXT("Oracle has a value for corner 0's element"), bHas0);
	TestTrue(TEXT("Oracle has a value for corner 3's element"), bHas3);
	TestTrue(TEXT("Oracle has a value for corner 2's element"), bHas2);
	TestTrue(TEXT("Oracle has a value for corner 4's element"), bHas4);
	if (bHas0 && bHas3)
	{
		TestFalse(TEXT("V0's two elements (corners 0 vs 3) produce GENUINELY DIFFERENT raw AO -- proves the fixture's fold geometry is non-degenerate, never a vacuous flat-mesh result"), FMath::IsNearlyEqual(Value0, Value3, 1e-4f));
	}
	if (bHas2 && bHas4)
	{
		TestFalse(TEXT("V2's two elements (corners 2 vs 4) produce GENUINELY DIFFERENT raw AO"), FMath::IsNearlyEqual(Value2, Value4, 1e-4f));
	}

	FVertexMaskForgeDynamicLayerStack Stack;
	AddAmbientOcclusionLayer(Stack, TEXT("AO"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		/*Samples=*/32, /*MaxDistance=*/100.0f, /*Bias=*/0.05f, 0.0f, 1.0f, false);

	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();
	TArray<FColor> Out;
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> OutAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> OutThicknessCaches;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, OutAOCaches, OutThicknessCaches, Out);

	TestTrue(TEXT("Orchestrator accepts a real enabled Ambient Occlusion layer"), bSucceeded);
	TestEqual(TEXT("Out.Num() == 6"), Out.Num(), 6);
	TestEqual(TEXT("Exactly one AO cache entry was created"), OutAOCaches.Num(), 1);
	TestTrue(TEXT("The AO cache entry is keyed by the AO layer's own stable LayerId"), OutAOCaches.Contains(Stack.GetLayers()[0].LayerId));
	if (!bSucceeded || Out.Num() != 6)
	{
		return false;
	}

	for (int32 CornerIndex = 0; CornerIndex < 6; ++CornerIndex)
	{
		float ExpectedMaskValue = 0.0f;
		const bool bHasValue = ReferenceMask.TryGetValue(ElementIDByCorner[CornerIndex], ExpectedMaskValue);
		TestTrue(*FString::Printf(TEXT("Oracle has a value at ElementID %d (corner %d)"), ElementIDByCorner[CornerIndex], CornerIndex), bHasValue);
		if (!bHasValue)
		{
			continue;
		}

		const uint8 ExpectedByte = UnitFloatToByte(ExpectedMaskValue);
		TestEqual(*FString::Printf(TEXT("Out[%d].R byte-exact vs oracle generator + per-corner Element ID"), CornerIndex), Out[CornerIndex].R, ExpectedByte);
		TestEqual(*FString::Printf(TEXT("Out[%d].G byte-exact vs oracle generator + per-corner Element ID"), CornerIndex), Out[CornerIndex].G, ExpectedByte);
		TestEqual(*FString::Printf(TEXT("Out[%d].B byte-exact vs oracle generator + per-corner Element ID"), CornerIndex), Out[CornerIndex].B, ExpectedByte);
		TestEqual(*FString::Printf(TEXT("Out[%d].A == BaseColors[%d].A"), CornerIndex, CornerIndex), Out[CornerIndex].A, BaseColors[CornerIndex].A);
	}

	// Corner 0 and corner 3 map to the SAME Dynamic Mesh VertexID (V0) but DIFFERENT Element IDs -- if the
	// evaluator incorrectly used VertexID (or averaged/picked one element arbitrarily), Out[0] and Out[3]
	// would incorrectly match despite their genuinely different oracle values.
	if (!FMath::IsNearlyEqual(Value0, Value3, 1e-4f))
	{
		TestNotEqual(TEXT("Out[0] (corner 0, V0) != Out[3] (corner 3, also V0) -- proves per-ELEMENT, not per-VERTEX, resolution"), Out[0].R, Out[3].R);
	}

	return true;
}

// M16-K.6D-8G-E, AO-2: cold vs. warm cache -- proves the persistent Model D cache entry is genuinely
// reused across two separate orchestrator calls with identical inputs, and implicitly proves the M16-K.6D-
// 8G-D adapter (by-value map entry -> call-scoped TUniquePtr -> back into the same map entry) restores
// ownership correctly on the success path: if it did not, the SECOND call would see FindOrAdd return a
// fresh, cold (bTreeValid=false) entry instead of the warm one. No elapsed-time evidence is used.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompAOWarmCacheTest, "VertexMaskForge.DynamicSourceTopologyComposition.AmbientOcclusionColdAndWarmCacheProduceIdenticalResultAndReuseEntry", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompAOWarmCacheTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildAOFoldFixtureWorkingMesh();
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = AddAmbientOcclusionLayer(Stack, TEXT("AO"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		/*Samples=*/32, /*MaxDistance=*/100.0f, /*Bias=*/0.05f, 0.0f, 1.0f, false);

	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> AOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ThicknessCaches;

	TArray<FColor> ColdOut;
	const bool bColdSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, ColdOut);
	TestTrue(TEXT("Cold evaluation succeeds"), bColdSucceeded);
	TestEqual(TEXT("Exactly one cache entry after cold evaluation"), AOCaches.Num(), 1);

	const FVertexMaskForgeSourceTopologyAOCache* ColdEntry = AOCaches.Find(LayerId);
	TestTrue(TEXT("Cold entry exists under the AO layer's own LayerId"), ColdEntry != nullptr);
	if (!ColdEntry || !bColdSucceeded)
	{
		return false;
	}
	TestTrue(TEXT("Cold entry's tree is valid -- proves the adapter restored ownership on the success path"), ColdEntry->bTreeValid);
	TestTrue(TEXT("Cold entry's raw values are valid"), ColdEntry->bValuesValid);
	TestTrue(TEXT("Cold entry retained a real, non-null acceleration tree"), ColdEntry->Tree.IsValid());
	TestTrue(TEXT("Cold entry retained a real, non-null WorldMesh"), ColdEntry->WorldMesh.IsValid());
	const TArray<float> ColdRawValues = ColdEntry->RawValues;

	TArray<FColor> WarmOut;
	const bool bWarmSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, WarmOut);
	TestTrue(TEXT("Warm evaluation succeeds"), bWarmSucceeded);
	TestEqual(TEXT("Still exactly one cache entry after warm evaluation -- no duplicate entry created"), AOCaches.Num(), 1);

	TestEqual(TEXT("Cold and warm output counts match"), ColdOut.Num(), WarmOut.Num());
	for (int32 Index = 0; Index < FMath::Min(ColdOut.Num(), WarmOut.Num()); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("ColdOut[%d] == WarmOut[%d] (byte-identical)"), Index, Index), ColdOut[Index], WarmOut[Index]);
	}

	const FVertexMaskForgeSourceTopologyAOCache* WarmEntry = AOCaches.Find(LayerId);
	TestTrue(TEXT("Warm entry still exists under the SAME LayerId"), WarmEntry != nullptr);
	if (WarmEntry)
	{
		TestTrue(TEXT("Warm entry's tree remains valid"), WarmEntry->bTreeValid);
		TestTrue(TEXT("Warm entry's raw values remain valid"), WarmEntry->bValuesValid);
		// Strongest safely observable proof of reuse available without a production pointer-identity
		// accessor (none exists, and none is added here) -- RawValues content identity across two calls.
		TestTrue(TEXT("RawValues are byte-for-byte identical across cold/warm calls"), AreRawValueArraysIdentical(WarmEntry->RawValues, ColdRawValues));
	}

	return true;
}

// M16-K.6D-8G-E, AO-3: Levels and Invert are purely compositional -- both must reuse RawValues without
// ever rebuilding the tree or rerunning raycasts, and must never be applied twice. Uses the fold fixture
// (genuinely non-constant raw AO, never 0/1-everywhere) so a Levels/Invert change produces a REAL,
// meaningfully different composed output, never a vacuous no-op.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompAOLevelsInvertReuseTest, "VertexMaskForge.DynamicSourceTopologyComposition.AmbientOcclusionLevelsAndInvertReuseRawValuesWithoutDoubleProcessing", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompAOLevelsInvertReuseTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildAOFoldFixtureWorkingMesh();
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = AddAmbientOcclusionLayer(Stack, TEXT("AO"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		/*Samples=*/32, /*MaxDistance=*/100.0f, /*Bias=*/0.05f, 0.0f, 1.0f, false);

	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> AOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ThicknessCaches;

	TArray<FColor> BaselineOut;
	TestTrue(TEXT("Baseline (Levels 0/1, Invert false) evaluation succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, BaselineOut));

	const FVertexMaskForgeSourceTopologyAOCache* Entry = AOCaches.Find(LayerId);
	TestTrue(TEXT("Cache entry exists"), Entry != nullptr);
	if (!Entry)
	{
		return false;
	}
	const TArray<float> RawAfterCold = Entry->RawValues;
	const int32 CachedSamplesAfterCold = Entry->CachedSamples;

	MutateAmbientOcclusionParams(Stack, LayerId, [](FVertexMaskForgeAmbientOcclusionParams& P) { P.LevelsMin = 0.4f; });

	TArray<FColor> LevelsOut;
	TestTrue(TEXT("Levels-only-changed evaluation succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, LevelsOut));

	const FVertexMaskForgeSourceTopologyAOCache* EntryAfterLevels = AOCaches.Find(LayerId);
	TestTrue(TEXT("Same cache entry still exists after a Levels-only change"), EntryAfterLevels != nullptr);
	TestEqual(TEXT("Still exactly one cache entry (no duplicate created by the param change)"), AOCaches.Num(), 1);
	if (EntryAfterLevels)
	{
		TestEqual(TEXT("CachedSamples unchanged -- proves raw values were NOT recomputed for a Levels-only change"), EntryAfterLevels->CachedSamples, CachedSamplesAfterCold);
		TestTrue(TEXT("RawValues are byte-for-byte unchanged after a Levels-only edit"), AreRawValueArraysIdentical(EntryAfterLevels->RawValues, RawAfterCold));
	}
	TestNotEqual(TEXT("Composed output DOES change when Levels changes (proves Levels is actually applied, not silently ignored)"), LevelsOut[0].R, BaselineOut[0].R);

	MutateAmbientOcclusionParams(Stack, LayerId, [](FVertexMaskForgeAmbientOcclusionParams& P) { P.bInvert = true; });

	TArray<FColor> InvertOut;
	TestTrue(TEXT("Invert-added evaluation succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, InvertOut));

	const FVertexMaskForgeSourceTopologyAOCache* EntryAfterInvert = AOCaches.Find(LayerId);
	TestTrue(TEXT("Same cache entry still exists after adding Invert"), EntryAfterInvert != nullptr);
	if (EntryAfterInvert)
	{
		TestTrue(TEXT("RawValues remain byte-for-byte unchanged after also toggling Invert"), AreRawValueArraysIdentical(EntryAfterInvert->RawValues, RawAfterCold));
	}
	TestNotEqual(TEXT("Composed output changes again when Invert is added (proves Invert applied once, not skipped or double-applied)"), InvertOut[0].R, LevelsOut[0].R);

	return true;
}

// M16-K.6D-8G-E, AO-4: ordinary Dynamic composition properties (Fill/Blend/Opacity/Channel Filter) never
// touch AO's own raw generation state -- distinguishing AO generation state from ordinary layer-
// composition state, exactly as the checkpoint requires.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompAOCompositionEditsPreserveRawTest, "VertexMaskForge.DynamicSourceTopologyComposition.AmbientOcclusionCompositionOnlyEditsPreserveRawValues", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompAOCompositionEditsPreserveRawTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildAOFoldFixtureWorkingMesh();
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = AddAmbientOcclusionLayer(Stack, TEXT("AO"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		/*Samples=*/32, /*MaxDistance=*/100.0f, /*Bias=*/0.05f, 0.0f, 1.0f, false);

	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> AOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ThicknessCaches;

	TArray<FColor> BaselineOut;
	TestTrue(TEXT("Baseline evaluation succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, BaselineOut));
	const TArray<float> RawAfterCold = AOCaches.FindChecked(LayerId).RawValues;

	// Fill: White -> Black.
	Stack.SetLayerFill(LayerId, EVertexMaskForgeLayerFill::Black);
	TArray<FColor> FillChangedOut;
	TestTrue(TEXT("Fill-changed evaluation succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, FillChangedOut));
	TestTrue(TEXT("RawValues unchanged after a Fill-only edit"), AreRawValueArraysIdentical(AOCaches.FindChecked(LayerId).RawValues, RawAfterCold));
	TestNotEqual(TEXT("Composed output changes when Fill changes"), FillChangedOut[0].R, BaselineOut[0].R);
	Stack.SetLayerFill(LayerId, EVertexMaskForgeLayerFill::White);

	// Blend Mode: Copy -> Multiply.
	Stack.SetLayerBlendMode(LayerId, EVertexMaskForgeBlendMode::Multiply);
	TArray<FColor> BlendChangedOut;
	TestTrue(TEXT("Blend-changed evaluation succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, BlendChangedOut));
	TestTrue(TEXT("RawValues unchanged after a Blend Mode-only edit"), AreRawValueArraysIdentical(AOCaches.FindChecked(LayerId).RawValues, RawAfterCold));
	Stack.SetLayerBlendMode(LayerId, EVertexMaskForgeBlendMode::Copy);

	// Opacity: 1.0 -> 0.5.
	Stack.SetLayerOpacity(LayerId, 0.5f);
	TArray<FColor> OpacityChangedOut;
	TestTrue(TEXT("Opacity-changed evaluation succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, OpacityChangedOut));
	TestTrue(TEXT("RawValues unchanged after an Opacity-only edit"), AreRawValueArraysIdentical(AOCaches.FindChecked(LayerId).RawValues, RawAfterCold));
	TestNotEqual(TEXT("Composed output changes when Opacity changes"), OpacityChangedOut[0].R, BaselineOut[0].R);
	Stack.SetLayerOpacity(LayerId, 1.0f);

	// Channel Filter: all -> Red only.
	Stack.SetLayerChannelFilter(LayerId, true, false, false);
	TArray<FColor> ChannelChangedOut;
	TestTrue(TEXT("Channel-Filter-changed evaluation succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, ChannelChangedOut));
	TestTrue(TEXT("RawValues unchanged after a Channel-Filter-only edit"), AreRawValueArraysIdentical(AOCaches.FindChecked(LayerId).RawValues, RawAfterCold));
	TestEqual(TEXT("Green channel untouched (Channel Filter excludes it) -- matches BaseColors' own Green"), ChannelChangedOut[0].G, BaseColors[0].G);
	TestEqual(TEXT("Alpha preserved from BaseColors regardless of Channel Filter"), ChannelChangedOut[0].A, BaseColors[0].A);

	TestEqual(TEXT("No additional AO cache entry was created by any composition-only edit"), AOCaches.Num(), 1);

	return true;
}

// M16-K.6D-8G-E, AO-5: Samples/Max Distance/Bias are the ONLY inputs that invalidate RawValues -- each
// must trigger a real recompute (CachedSamples/MaxDistance/Bias advancing to match), while the
// acceleration tree (keyed only by mesh/fingerprint/transform) is retained, never rebuilt, across a
// raw-parameter-only change.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompAORawInvalidationTest, "VertexMaskForge.DynamicSourceTopologyComposition.AmbientOcclusionRawParameterChangesInvalidateOnlyRawValues", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompAORawInvalidationTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildAOFoldFixtureWorkingMesh();
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = AddAmbientOcclusionLayer(Stack, TEXT("AO"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		/*Samples=*/16, /*MaxDistance=*/100.0f, /*Bias=*/0.1f, 0.0f, 1.0f, false);

	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> AOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ThicknessCaches;

	TArray<FColor> Out;
	TestTrue(TEXT("Baseline evaluation succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, Out));
	const UE::Geometry::FDynamicMesh3* TreeMeshAfterCold = AOCaches.FindChecked(LayerId).WorldMesh.Get();
	TestTrue(TEXT("Tree valid after cold evaluation"), AOCaches.FindChecked(LayerId).bTreeValid);

	// Samples: 16 -> 64.
	MutateAmbientOcclusionParams(Stack, LayerId, [](FVertexMaskForgeAmbientOcclusionParams& P) { P.Samples = 64; });
	TestTrue(TEXT("Samples-changed evaluation succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, Out));
	{
		const FVertexMaskForgeSourceTopologyAOCache& E = AOCaches.FindChecked(LayerId);
		TestEqual(TEXT("CachedSamples advanced to 64 -- proves a real raw recompute occurred"), E.CachedSamples, 64);
		TestTrue(TEXT("Tree remains valid (never rebuilt for a Samples-only change)"), E.bTreeValid);
		TestTrue(TEXT("WorldMesh pointer identity unchanged -- the tree was retained, not rebuilt"), E.WorldMesh.Get() == TreeMeshAfterCold);
	}

	// Max Distance: 100 -> 5 (a genuinely different search radius on this small fixture).
	MutateAmbientOcclusionParams(Stack, LayerId, [](FVertexMaskForgeAmbientOcclusionParams& P) { P.MaxDistance = 5.0f; });
	TestTrue(TEXT("MaxDistance-changed evaluation succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, Out));
	{
		const FVertexMaskForgeSourceTopologyAOCache& E = AOCaches.FindChecked(LayerId);
		TestEqual(TEXT("CachedMaxDistance advanced to 5.0"), E.CachedMaxDistance, 5.0f);
		TestTrue(TEXT("Tree remains valid (never rebuilt for a MaxDistance-only change)"), E.bTreeValid);
		TestTrue(TEXT("WorldMesh pointer identity still unchanged"), E.WorldMesh.Get() == TreeMeshAfterCold);
	}

	// Bias: 0.1 -> 0.5.
	MutateAmbientOcclusionParams(Stack, LayerId, [](FVertexMaskForgeAmbientOcclusionParams& P) { P.Bias = 0.5f; });
	TestTrue(TEXT("Bias-changed evaluation succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, Out));
	{
		const FVertexMaskForgeSourceTopologyAOCache& E = AOCaches.FindChecked(LayerId);
		TestEqual(TEXT("CachedBias advanced to 0.5"), E.CachedBias, 0.5f);
		TestTrue(TEXT("Tree remains valid (never rebuilt for a Bias-only change)"), E.bTreeValid);
		TestTrue(TEXT("WorldMesh pointer identity still unchanged"), E.WorldMesh.Get() == TreeMeshAfterCold);
	}

	TestEqual(TEXT("Still exactly one cache entry under the same LayerId across all three raw-parameter changes"), AOCaches.Num(), 1);

	return true;
}

// M16-K.6D-8G-E, AO-6: a deliberately non-identity component transform (rotation + translation + non-
// uniform scale) is genuinely propagated to the AO generator -- parity against the direct oracle using
// the SAME real transform, and the result must differ from an identity-transform evaluation (proving no
// silent identity-transform fallback).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompAOTransformTest, "VertexMaskForge.DynamicSourceTopologyComposition.AmbientOcclusionNonIdentityTransformMatchesDirectGenerator", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompAOTransformTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildAOFoldFixtureWorkingMesh();
	const FTransform RealTransform(FRotator(20.0f, 35.0f, 10.0f), FVector(5.0f, -3.0f, 2.0f), FVector(1.3f, 0.7f, 1.1f));
	const FVertexMaskForgeAOParams OracleParams = MakeOracleAOParams(/*Samples=*/32, /*MaxDistance=*/100.0f, /*Bias=*/0.05f, 0.0f, 1.0f, false);

	TUniquePtr<FVertexMaskForgeSourceTopologyAOCache> OracleCachePtr;
	const FVertexMaskForgeScalarMask ReferenceMask = VertexMaskForgeAmbientOcclusionGenerator::GenerateAmbientOcclusionMaskFromDynamicMesh(
		OracleCachePtr, *WorkingMesh.Mesh, WorkingMesh.GeometryFingerprint, RealTransform, OracleParams);
	TestTrue(TEXT("Oracle (real transform) State == Ready"), ReferenceMask.State == EVertexMaskForgeScalarMaskState::Ready);

	FVertexMaskForgeDynamicLayerStack Stack;
	AddAmbientOcclusionLayer(Stack, TEXT("AO"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		/*Samples=*/32, /*MaxDistance=*/100.0f, /*Bias=*/0.05f, 0.0f, 1.0f, false);
	const FGuid LayerId = Stack.GetLayers()[0].LayerId;

	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> AOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ThicknessCaches;
	TArray<FColor> RealTransformOut;
	TestTrue(TEXT("Orchestrator evaluation with the real transform succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, RealTransform, AOCaches, ThicknessCaches, RealTransformOut));

	const UE::Geometry::FDynamicMeshNormalOverlay* NormalOverlay = WorkingMesh.Mesh->Attributes()->PrimaryNormals();
	int32 ElementIDByCorner[6];
	{
		int32 RunningCorner = 0;
		for (const int32 TriangleID : WorkingMesh.Mesh->TriangleIndicesItr())
		{
			const UE::Geometry::FIndex3i TriElements = NormalOverlay->GetTriangle(TriangleID);
			ElementIDByCorner[RunningCorner++] = TriElements.A;
			ElementIDByCorner[RunningCorner++] = TriElements.B;
			ElementIDByCorner[RunningCorner++] = TriElements.C;
		}
	}
	for (int32 CornerIndex = 0; CornerIndex < 6; ++CornerIndex)
	{
		float ExpectedMaskValue = 0.0f;
		if (ReferenceMask.TryGetValue(ElementIDByCorner[CornerIndex], ExpectedMaskValue))
		{
			const uint8 ExpectedByte = UnitFloatToByte(ExpectedMaskValue);
			TestEqual(*FString::Printf(TEXT("Out[%d].R byte-exact vs oracle using the SAME real transform"), CornerIndex), RealTransformOut[CornerIndex].R, ExpectedByte);
		}
	}

	// The transform's own cache key (CachedTransform) must reflect the REAL transform, never Identity.
	const FVertexMaskForgeSourceTopologyAOCache& Entry = AOCaches.FindChecked(LayerId);
	TestTrue(TEXT("Cache entry's CachedTransform matches the real transform, not Identity"), Entry.CachedTransform.Equals(RealTransform, 1e-4f));
	TestFalse(TEXT("Cache entry's CachedTransform is NOT Identity (proves no silent identity fallback)"), Entry.CachedTransform.Equals(FTransform::Identity, 1e-4f));

	// A separate, independent Identity-transform evaluation (fresh cache map) must produce a DIFFERENT
	// result, proving the transform genuinely affects the outcome rather than being ignored.
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> IdentityAOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> IdentityThicknessCaches;
	TArray<FColor> IdentityOut;
	TestTrue(TEXT("Identity-transform evaluation succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, IdentityAOCaches, IdentityThicknessCaches, IdentityOut));
	bool bAnyCornerDiffers = false;
	for (int32 CornerIndex = 0; CornerIndex < 6; ++CornerIndex)
	{
		if (RealTransformOut[CornerIndex].R != IdentityOut[CornerIndex].R)
		{
			bAnyCornerDiffers = true;
			break;
		}
	}
	TestTrue(TEXT("Real-transform result differs from Identity-transform result on at least one corner"), bAnyCornerDiffers);

	return true;
}

// M16-K.6D-8G-E, AO-7 (5.8): two enabled AO layers with different stable LayerId values, in the SAME
// component-owned map, own two fully independent cache entries -- Model D never shares a tree between
// layers. Changing one layer's raw parameters must never affect the other's retained state.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompAOMultiLayerIsolationTest, "VertexMaskForge.DynamicSourceTopologyComposition.AmbientOcclusionTwoLayersOwnIndependentCacheEntries", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompAOMultiLayerIsolationTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildAOFoldFixtureWorkingMesh();
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerIdA = AddAmbientOcclusionLayer(Stack, TEXT("AO-A"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		/*Samples=*/16, /*MaxDistance=*/100.0f, /*Bias=*/0.05f, 0.0f, 1.0f, false);
	const FGuid LayerIdB = AddAmbientOcclusionLayer(Stack, TEXT("AO-B"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Multiply, 1.0f,
		/*Samples=*/48, /*MaxDistance=*/100.0f, /*Bias=*/0.05f, 0.0f, 1.0f, false);
	TestNotEqual(TEXT("The two AO layers have distinct LayerIds"), LayerIdA, LayerIdB);

	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> AOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ThicknessCaches;
	TArray<FColor> Out;
	TestTrue(TEXT("Two-AO-layer evaluation succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, Out));

	TestEqual(TEXT("Exactly two cache entries -- one per LayerId"), AOCaches.Num(), 2);
	const FVertexMaskForgeSourceTopologyAOCache* EntryA = AOCaches.Find(LayerIdA);
	const FVertexMaskForgeSourceTopologyAOCache* EntryB = AOCaches.Find(LayerIdB);
	TestTrue(TEXT("Entry A exists under LayerIdA"), EntryA != nullptr);
	TestTrue(TEXT("Entry B exists under LayerIdB"), EntryB != nullptr);
	if (!EntryA || !EntryB)
	{
		return false;
	}
	TestEqual(TEXT("Entry A's CachedSamples matches ITS OWN layer's Samples (16)"), EntryA->CachedSamples, 16);
	TestEqual(TEXT("Entry B's CachedSamples matches ITS OWN layer's Samples (48) -- no aliasing/overwrite"), EntryB->CachedSamples, 48);
	TestTrue(TEXT("Entry A and Entry B own DIFFERENT (non-aliased) acceleration trees"), EntryA->Tree.Get() != EntryB->Tree.Get());
	TestTrue(TEXT("Entry A and Entry B own DIFFERENT (non-aliased) WorldMesh objects"), EntryA->WorldMesh.Get() != EntryB->WorldMesh.Get());

	const TArray<float> RawBBeforeChange = EntryB->RawValues;

	// Change ONLY Layer A's Bias -- Layer B's own entry must remain completely untouched.
	MutateAmbientOcclusionParams(Stack, LayerIdA, [](FVertexMaskForgeAmbientOcclusionParams& P) { P.Bias = 0.9f; });
	TestTrue(TEXT("Re-evaluation after changing only Layer A's Bias succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, Out));

	const FVertexMaskForgeSourceTopologyAOCache& EntryAAfter = AOCaches.FindChecked(LayerIdA);
	const FVertexMaskForgeSourceTopologyAOCache& EntryBAfter = AOCaches.FindChecked(LayerIdB);
	TestEqual(TEXT("Entry A's CachedBias advanced to 0.9"), EntryAAfter.CachedBias, 0.9f);
	TestEqual(TEXT("Entry B's CachedSamples STILL 48 -- unaffected by Layer A's own change"), EntryBAfter.CachedSamples, 48);
	TestTrue(TEXT("Entry B's RawValues are UNCHANGED -- proves no cross-layer contamination"), AreRawValueArraysIdentical(EntryBAfter.RawValues, RawBBeforeChange));
	TestEqual(TEXT("Still exactly two cache entries"), AOCaches.Num(), 2);

	return true;
}

// M16-K.6D-8G-E, AO-8 (5.9): two SEPARATE component-owned maps (never a single shared/global map),
// evaluated with the SAME stable LayerId but different raw parameters, own fully independent entries --
// component identity comes from map ownership, never from the key.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompAOCrossComponentIsolationTest, "VertexMaskForge.DynamicSourceTopologyComposition.AmbientOcclusionTwoComponentOwnedMapsCannotShareEntries", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompAOCrossComponentIsolationTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildAOFoldFixtureWorkingMesh();
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = AddAmbientOcclusionLayer(Stack, TEXT("AO"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		/*Samples=*/16, /*MaxDistance=*/100.0f, /*Bias=*/0.05f, 0.0f, 1.0f, false);

	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();

	// "Component A" -- its own independent map.
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> MapA;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ThicknessMapA;
	TArray<FColor> OutA;
	TestTrue(TEXT("Component A evaluation succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, MapA, ThicknessMapA, OutA));

	// "Component B" -- a SEPARATE map, same LayerId, DIFFERENT raw params.
	MutateAmbientOcclusionParams(Stack, LayerId, [](FVertexMaskForgeAmbientOcclusionParams& P) { P.Samples = 64; P.Bias = 0.5f; });
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> MapB;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ThicknessMapB;
	TArray<FColor> OutB;
	TestTrue(TEXT("Component B evaluation succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, MapB, ThicknessMapB, OutB));

	TestEqual(TEXT("Map A has exactly one entry"), MapA.Num(), 1);
	TestEqual(TEXT("Map B has exactly one entry"), MapB.Num(), 1);
	const FVertexMaskForgeSourceTopologyAOCache& EntryA = MapA.FindChecked(LayerId);
	const FVertexMaskForgeSourceTopologyAOCache& EntryB = MapB.FindChecked(LayerId);
	TestEqual(TEXT("Map A's entry retained its OWN original Samples (16)"), EntryA.CachedSamples, 16);
	TestEqual(TEXT("Map B's entry reflects the NEW Samples (64) -- proves the two maps never shared storage"), EntryB.CachedSamples, 64);
	TestTrue(TEXT("Map A and Map B own DIFFERENT (non-aliased) acceleration trees"), EntryA.Tree.Get() != EntryB.Tree.Get());

	// Re-evaluate Component A again (same params as its own first call) -- Map A must be completely
	// unaffected by whatever happened to Map B in between.
	MutateAmbientOcclusionParams(Stack, LayerId, [](FVertexMaskForgeAmbientOcclusionParams& P) { P.Samples = 16; P.Bias = 0.05f; });
	TArray<FColor> OutAAgain;
	TestTrue(TEXT("Component A re-evaluation succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, MapA, ThicknessMapA, OutAAgain));
	TestEqual(TEXT("Map A still has exactly one entry"), MapA.Num(), 1);
	for (int32 Index = 0; Index < OutA.Num(); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("OutA[%d] unaffected by Component B's own evaluations"), Index), OutA[Index], OutAAgain[Index]);
	}

	return true;
}

// M16-K.6D-8G-E, AO-9 (5.10): reorder preserves the AO cache under its original stable LayerId -- never
// erased, never replaced, raw state remains reusable -- and the final composed output follows the NEW
// sequential order for a Blend/Fill combination where order is observably noncommutative.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompAOReorderTest, "VertexMaskForge.DynamicSourceTopologyComposition.AmbientOcclusionReorderPreservesLayerIdCacheOwnership", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompAOReorderTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildAOFoldFixtureWorkingMesh();
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid AOLayerId = AddAmbientOcclusionLayer(Stack, TEXT("AO"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		/*Samples=*/16, /*MaxDistance=*/100.0f, /*Bias=*/0.05f, 0.0f, 1.0f, false);
	// A second, fully deterministic Fill-only layer that OVERWRITES (Copy @ Opacity 1.0) whatever precedes
	// it -- makes final-order sensitivity trivial and unambiguous to assert: whichever layer is LAST wins.
	const FGuid FillLayerId = AddFillOnlyLayer(Stack, TEXT("Black Overwrite"), EVertexMaskForgeLayerFill::Black, EVertexMaskForgeBlendMode::Copy, 1.0f);

	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> AOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ThicknessCaches;

	TArray<FColor> OriginalOrderOut;
	TestTrue(TEXT("Original order (AO, then Black overwrite) evaluation succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, OriginalOrderOut));
	TestEqual(TEXT("Black (the LAST layer) wins under the original order"), OriginalOrderOut[0].R, (uint8)0);
	const TArray<float> RawBeforeReorder = AOCaches.FindChecked(AOLayerId).RawValues;
	const int32 CachedSamplesBeforeReorder = AOCaches.FindChecked(AOLayerId).CachedSamples;

	// Reorder: move the AO layer to the END (index 1), so it now overwrites the Black layer instead.
	const bool bMoved = Stack.MoveLayer(AOLayerId, 1);
	TestTrue(TEXT("MoveLayer succeeds"), bMoved);

	TArray<FColor> ReorderedOut;
	TestTrue(TEXT("Reordered evaluation succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, ReorderedOut));

	TestEqual(TEXT("Still exactly one AO cache entry after reorder -- no replacement entry created"), AOCaches.Num(), 1);
	const FVertexMaskForgeSourceTopologyAOCache* EntryAfterReorder = AOCaches.Find(AOLayerId);
	TestTrue(TEXT("AO cache entry remains under the ORIGINAL stable LayerId after reorder"), EntryAfterReorder != nullptr);
	if (EntryAfterReorder)
	{
		TestEqual(TEXT("CachedSamples unchanged by reorder -- raw state remains reusable, no recompute forced"), EntryAfterReorder->CachedSamples, CachedSamplesBeforeReorder);
		TestTrue(TEXT("RawValues byte-for-byte unchanged by reorder"), AreRawValueArraysIdentical(EntryAfterReorder->RawValues, RawBeforeReorder));
	}

	// AO is now LAST -- its own (non-Black, genuinely varying) value should win instead of Black.
	TestNotEqual(TEXT("Composed output changes after reorder (AO, not Black, now wins -- proves the new order actually took effect)"), ReorderedOut[0].R, OriginalOrderOut[0].R);

	return true;
}

// M16-K.6D-8G-E, AO-10 (5.11): a disabled AO layer contributes nothing and creates NO cache entry at
// all -- no generation, no overlay-domain resolution required -- while another enabled layer still
// produces its own correct, unaffected result. Preserves the existing Thickness-as-unsupported-generator
// negative fixture unchanged elsewhere in this file.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompAODisabledNoCacheTest, "VertexMaskForge.DynamicSourceTopologyComposition.AmbientOcclusionDisabledLayerCreatesNoCacheEntry", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompAODisabledNoCacheTest::RunTest(const FString& Parameters)
{
	// The plain Material-Slot-ready fixture (never the AO fold fixture) -- AO is disabled here and needs
	// no Normal Overlay at all, while the OTHER enabled layer (Material Slot) needs valid Material Slot
	// resolution data, which only BuildOrchestratorFixtureWorkingMesh provides.
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1);
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid AOLayerId = AddAmbientOcclusionLayer(Stack, TEXT("AO"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		/*Samples=*/16, /*MaxDistance=*/100.0f, /*Bias=*/0.05f, 0.0f, 1.0f, false);
	Stack.SetLayerEnabled(AOLayerId, false);
	AddMaterialSlotLayer(Stack, TEXT("Slot"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f, /*SelectedSlotIndex=*/0);

	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> AOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ThicknessCaches;
	TArray<FColor> Out;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, Out);

	TestTrue(TEXT("Evaluation succeeds (disabled AO layer never fails the whole call)"), bSucceeded);
	TestEqual(TEXT("NO AO cache entry was created for the disabled layer"), AOCaches.Num(), 0);
	TestFalse(TEXT("The disabled layer's own LayerId has no entry"), AOCaches.Contains(AOLayerId));

	if (bSucceeded && Out.Num() == 6)
	{
		// Material Slot 0 covers Tri0 (corners 0/1/2); the enabled layer's own result must still be correct
		// and unaffected by the disabled AO layer sitting above it.
		TestEqual(TEXT("Out[0] reflects the enabled Material Slot layer's own White result"), Out[0].R, (uint8)255);
	}

	return true;
}

// M16-K.6D-8G-E, AO-11 (5.12 + 5.13 failure path): a legitimate, already-existing AO failure --
// GenerateAmbientOcclusionMaskFromDynamicMesh reports Unavailable when the mesh has no Normal Overlay at
// all (BuildOrchestratorFixtureWorkingMesh never enables mesh attributes) -- proves the whole-call failure
// contract (OutComposedColors left completely untouched) AND that the M16-K.6D-8G-D cache adapter still
// restores the map entry after this internal generator failure (the FindOrAdd-created entry survives the
// TUniquePtr round trip in a sane, cold, non-corrupted state), never leaking or double-owning the
// acceleration tree. No production failure hook was added to reach this path -- it is already reachable
// through a legitimate, pre-existing input (a mesh with no normals).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompAOTransactionalFailureTest, "VertexMaskForge.DynamicSourceTopologyComposition.AmbientOcclusionMissingNormalOverlayFailsTransactionallyAndRestoresCacheOwnership", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompAOTransactionalFailureTest::RunTest(const FString& Parameters)
{
	// Deliberately the ORIGINAL fixture (no EnableAttributes(), no Normal Overlay) -- the AO generator's
	// own committed contract reports Unavailable for exactly this input, never a crash/UB.
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOrchestratorFixtureWorkingMesh(0, 1);
	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid AOLayerId = AddAmbientOcclusionLayer(Stack, TEXT("AO"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		/*Samples=*/16, /*MaxDistance=*/100.0f, /*Bias=*/0.05f, 0.0f, 1.0f, false);

	const TArray<FColor> BaseColors = MakeSixCornerBaseColors();
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> AOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ThicknessCaches;

	// Recognizable sentinel, deliberately the wrong cardinality (proves OutComposedColors is untouched,
	// never resized/padded/truncated on failure) -- same established idiom as
	// UnsupportedGeneratorTypeFailsWholeCall's own sentinel elsewhere in this file.
	TArray<FColor> Out = { FColor(9, 9, 9, 9) };
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, Out);

	TestFalse(TEXT("Whole call fails when the mesh has no Normal Overlay"), bSucceeded);
	TestEqual(TEXT("OutComposedColors left completely untouched (sentinel survives, no partial/fallback output)"), Out.Num(), 1);
	if (Out.Num() == 1)
	{
		TestEqual(TEXT("Sentinel value itself is untouched"), Out[0], FColor(9, 9, 9, 9));
	}

	// The adapter must still have restored the map entry it took ownership of before returning false --
	// proves no leak/double-ownership/corruption on this internal-failure path.
	TestEqual(TEXT("A cache entry was created for the AO layer even though generation failed"), AOCaches.Num(), 1);
	const FVertexMaskForgeSourceTopologyAOCache* Entry = AOCaches.Find(AOLayerId);
	TestTrue(TEXT("The entry exists, safely destructible and reusable (not null/corrupted)"), Entry != nullptr);
	if (Entry)
	{
		TestFalse(TEXT("Entry's tree was never built (no normal overlay to raycast against) -- sane cold state, not a half-built/corrupted one"), Entry->bTreeValid);
		TestFalse(TEXT("Entry's raw values were never populated"), Entry->bValuesValid);
	}

	return true;
}

// M17-TH-DL-B, 1: a Thickness Dynamic Layer is DISPATCHED (reaches the real production backend,
// VertexMaskForgeThicknessGenerator::GenerateThicknessMaskFromDynamicMesh) rather than rejected as
// unsupported -- proven on a dense (every-corner-valid) fixture by comparing the composed output WITH a
// Thickness layer against the SAME stack's own Fill-only passthrough (no Thickness layer at all): if the
// backend were never really invoked, both would be byte-identical (White Fill, Copy blend, Opacity 1.0 ==
// pure White passthrough either way); a real difference proves real per-corner computed values reached
// composition. Retires the M16-K.6D-4-era premise that Thickness is unsupported (see
// DisabledLayerContributesNothingAndSkipsValidation's own updated comment below) -- as of M17-TH-DL-B this
// orchestrator dispatches all seven EVertexMaskForgeGeneratorType enumerators; there is no longer a
// structurally-unsupported value to use as a negative-case sentinel.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompThicknessDispatchedTest, "VertexMaskForge.DynamicSourceTopologyComposition.ThicknessDispatchedToRealBackend", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompThicknessDispatchedTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildIrregularOctahedronThicknessFixtureWorkingMesh();
	const int32 NumCorners = WorkingMesh.Mesh->TriangleCount() * 3;
	TArray<FColor> BaseColors;
	BaseColors.Init(FColor::Black, NumCorners);

	// Baseline: Fill-only layer, no Thickness mask at all -- pure White passthrough.
	{
		FVertexMaskForgeDynamicLayerStack FillOnlyStack;
		AddFillOnlyLayer(FillOnlyStack, TEXT("Fill"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f);
		TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> AOCaches;
		TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ThicknessCaches;
		TArray<FColor> FillOnlyOut;
		TestTrue(TEXT("Fill-only baseline composes"),
			VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(
				WorkingMesh, FillOnlyStack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, FillOnlyOut));

		// Real Thickness layer, same Fill/BlendMode/Opacity.
		FVertexMaskForgeDynamicLayerStack ThicknessStack;
		AddThicknessLayer(ThicknessStack, TEXT("Thickness"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
			/*MinThickness=*/0.0f, /*MaxThickness=*/1000.0f, /*SearchDistance=*/1000.0f, /*bInvert=*/false);
		TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> AOCaches2;
		TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ThicknessCaches2;
		TArray<FColor> ThicknessOut;
		const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(
			WorkingMesh, ThicknessStack, BaseColors, FTransform::Identity, AOCaches2, ThicknessCaches2, ThicknessOut);

		TestTrue(TEXT("Dense Thickness fixture composes successfully (NumValidValues == ExpectedCornerCount)"), bSucceeded);
		TestEqual(TEXT("ThicknessOut has one FColor per corner"), ThicknessOut.Num(), NumCorners);
		TestEqual(TEXT("Exactly one Thickness cache entry was created"), ThicknessCaches2.Num(), 1);

		bool bAnyDifferentFromFillOnlyPassthrough = false;
		if (bSucceeded && FillOnlyOut.Num() == NumCorners && ThicknessOut.Num() == NumCorners)
		{
			for (int32 i = 0; i < NumCorners; ++i)
			{
				if (ThicknessOut[i] != FillOnlyOut[i]) { bAnyDifferentFromFillOnlyPassthrough = true; }
			}
		}
		TestTrue(TEXT("Thickness output differs from pure Fill-only passthrough -- proves the real backend actually computed per-corner values, not a no-op"), bAnyDifferentFromFillOnlyPassthrough);
	}

	return true;
}

// M17-TH-DL-B, 2: a genuine, total per-corner no-hit (center ray AND every fallback cone ray miss, on an
// otherwise-Ready mask) preserves the EXISTING sparse-mask failure contract -- the WHOLE call fails, exactly
// like Directional Normal's own established precedent for Corner-domain masks (see this orchestrator's own
// Thickness branch doc comment) -- never a substituted zero/one/Fill Value, never a new validity policy.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompThicknessNoHitFailsWholeCallTest, "VertexMaskForge.DynamicSourceTopologyComposition.ThicknessNoHitPreservesSparseMaskContract", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompThicknessNoHitFailsWholeCallTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildIsolatedFlatQuadThicknessNoHitFixtureWorkingMesh();
	FVertexMaskForgeDynamicLayerStack Stack;
	AddThicknessLayer(Stack, TEXT("Thickness"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		0.0f, 50.0f, 100.0f, false);

	const TArray<FColor> BaseColors = { FColor(1,1,1,1), FColor(2,2,2,2), FColor(3,3,3,3), FColor(4,4,4,4), FColor(5,5,5,5), FColor(6,6,6,6) };
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> AOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ThicknessCaches;
	TArray<FColor> Out = { FColor(9, 9, 9, 9) }; // sentinel, wrong size on purpose
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(
		WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, Out);

	TestFalse(TEXT("Whole call fails when every corner's Thickness value is unresolved (center miss + fallback gate/no-hit)"), bSucceeded);
	TestEqual(TEXT("OutComposedColors left completely untouched on failure"), Out.Num(), 1);
	if (Out.Num() == 1)
	{
		TestEqual(TEXT("Sentinel value itself is untouched"), Out[0], FColor(9, 9, 9, 9));
	}
	// The adapter must still have restored the map entry it took ownership of before returning false.
	TestEqual(TEXT("A cache entry was still created for the Thickness layer even though generation found no valid values"), ThicknessCaches.Num(), 1);

	return true;
}

// M17-TH-DL-B, 3: two Thickness Dynamic Layers on the SAME mesh, with DIFFERENT SearchDistance, receive
// independent cache entries (keyed by their own stable LayerId) that never cross-contaminate -- the exact
// Model D guarantee AO's own AmbientOcclusionTwoLayersIndependentCacheEntries test already establishes for
// its own generator, reused here for Thickness's own persistent cache.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompThicknessTwoLayersIndependentCacheTest, "VertexMaskForge.DynamicSourceTopologyComposition.ThicknessTwoLayersIndependentCacheIdentity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompThicknessTwoLayersIndependentCacheTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildIrregularOctahedronThicknessFixtureWorkingMesh();
	const int32 NumCorners = WorkingMesh.Mesh->TriangleCount() * 3;
	FVertexMaskForgeDynamicLayerStack Stack;
	// NOTE: MaxThickness must never exceed SearchDistance for either layer -- SanitizeThicknessParams
	// (VertexMaskForgeThicknessGenerator.cpp) clamps Search = Max3(Search, Max, ...), so a SearchDistance
	// smaller than MaxThickness would be silently raised back up, defeating this test's own purpose.
	const FGuid LayerIdA = AddThicknessLayer(Stack, TEXT("Thickness-A"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		0.0f, 1000.0f, /*SearchDistance=*/1000.0f, false);
	const FGuid LayerIdB = AddThicknessLayer(Stack, TEXT("Thickness-B"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Multiply, 1.0f,
		0.0f, 200.0f, /*SearchDistance=*/500.0f, false);
	TestNotEqual(TEXT("The two Thickness layers have distinct LayerIds"), LayerIdA, LayerIdB);

	TArray<FColor> BaseColors;
	BaseColors.Init(FColor::Black, NumCorners);
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> AOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ThicknessCaches;
	TArray<FColor> Out;
	TestTrue(TEXT("Two-Thickness-layer evaluation succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, Out));

	TestEqual(TEXT("Exactly two cache entries -- one per LayerId"), ThicknessCaches.Num(), 2);
	const FVertexMaskForgeSourceTopologyThicknessCache* EntryA = ThicknessCaches.Find(LayerIdA);
	const FVertexMaskForgeSourceTopologyThicknessCache* EntryB = ThicknessCaches.Find(LayerIdB);
	TestTrue(TEXT("Entry A exists under LayerIdA"), EntryA != nullptr);
	TestTrue(TEXT("Entry B exists under LayerIdB"), EntryB != nullptr);
	if (!EntryA || !EntryB)
	{
		return false;
	}
	TestEqual(TEXT("Entry A's CachedSearchDistance matches ITS OWN layer's SearchDistance (1000)"), EntryA->CachedSearchDistance, 1000.0f);
	TestEqual(TEXT("Entry B's CachedSearchDistance matches ITS OWN layer's SearchDistance (500) -- no aliasing/overwrite"), EntryB->CachedSearchDistance, 500.0f);
	TestTrue(TEXT("Entry A and Entry B own DIFFERENT (non-aliased) acceleration trees"), EntryA->Tree.Get() != EntryB->Tree.Get());
	TestTrue(TEXT("Entry A and Entry B own DIFFERENT (non-aliased) LocalMesh objects"), EntryA->LocalMesh.Get() != EntryB->LocalMesh.Get());

	return true;
}

// M17-TH-DL-B, 4/5: classifies exactly which Thickness parameter changes force a real raycast recompute
// versus which reuse the existing raw distances untouched. SearchDistance is expected to invalidate
// (CachedSearchDistance re-stamped to the new value via a real recompute pass); Min Thickness/Max
// Thickness/Invert are expected to leave BOTH the acceleration tree AND the raw distance array completely
// byte-identical (remap-only), while still changing the FINAL composed output (proving the remap itself
// really ran).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompThicknessInvalidationClassificationTest, "VertexMaskForge.DynamicSourceTopologyComposition.ThicknessParamInvalidationClassification", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompThicknessInvalidationClassificationTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildIrregularOctahedronThicknessFixtureWorkingMesh();
	const int32 NumCorners = WorkingMesh.Mesh->TriangleCount() * 3;
	TArray<FColor> BaseColors;
	BaseColors.Init(FColor::Black, NumCorners);

	// --- Min/Max/Invert-only edits: remap only, never a new raycast. ---
	{
		FVertexMaskForgeDynamicLayerStack Stack;
		const FGuid LayerId = AddThicknessLayer(Stack, TEXT("Thickness"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
			0.0f, 1000.0f, 1000.0f, false);
		TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> AOCaches;
		TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ThicknessCaches;
		TArray<FColor> Out1;
		TestTrue(TEXT("First evaluation succeeds"),
			VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, Out1));

		const FVertexMaskForgeSourceTopologyThicknessCache* EntryBefore = ThicknessCaches.Find(LayerId);
		if (!TestTrue(TEXT("Cache entry exists"), EntryBefore != nullptr)) { return false; }
		const UE::Geometry::FDynamicMeshAABBTree3* TreeBefore = EntryBefore->Tree.Get();
		const TArray<float> RawDistancesBefore = EntryBefore->RawDistances;
		const float CachedSearchDistanceBefore = EntryBefore->CachedSearchDistance;
		const float CachedBiasBefore = EntryBefore->CachedBias;

		MutateThicknessParams(Stack, LayerId, [](FVertexMaskForgeThicknessParams& P) { P.MinThickness = 1.0f; P.MaxThickness = 20.0f; P.bInvert = true; });
		TArray<FColor> Out2;
		TestTrue(TEXT("Second evaluation (Min/Max/Invert changed) succeeds"),
			VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, Out2));

		const FVertexMaskForgeSourceTopologyThicknessCache* EntryAfter = ThicknessCaches.Find(LayerId);
		if (!TestTrue(TEXT("Cache entry still exists"), EntryAfter != nullptr)) { return false; }
		TestTrue(TEXT("Tree pointer UNCHANGED -- no rebuild"), EntryAfter->Tree.Get() == TreeBefore);
		TestEqual(TEXT("CachedSearchDistance UNCHANGED -- no new raycast"), EntryAfter->CachedSearchDistance, CachedSearchDistanceBefore);
		TestEqual(TEXT("CachedBias UNCHANGED -- no new raycast"), EntryAfter->CachedBias, CachedBiasBefore);
		TestEqual(TEXT("RawDistances array size unchanged"), EntryAfter->RawDistances.Num(), RawDistancesBefore.Num());
		bool bRawDistancesIdentical = EntryAfter->RawDistances.Num() == RawDistancesBefore.Num();
		for (int32 i = 0; bRawDistancesIdentical && i < RawDistancesBefore.Num(); ++i)
		{
			if (EntryAfter->RawDistances[i] != RawDistancesBefore[i]) { bRawDistancesIdentical = false; }
		}
		TestTrue(TEXT("RawDistances byte-identical -- Min/Max/Invert never touch the raw raycast cache"), bRawDistancesIdentical);

		bool bOutputChanged = Out1.Num() == Out2.Num();
		if (bOutputChanged)
		{
			bOutputChanged = false;
			for (int32 i = 0; i < Out1.Num(); ++i) { if (Out1[i] != Out2[i]) { bOutputChanged = true; } }
		}
		TestTrue(TEXT("Composed output DID change -- the remap itself really ran, even though the cache didn't"), bOutputChanged);
	}

	// --- SearchDistance edit: forces a real recompute (CachedSearchDistance re-stamped). ---
	{
		FVertexMaskForgeDynamicLayerStack Stack;
		const FGuid LayerId = AddThicknessLayer(Stack, TEXT("Thickness"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
			0.0f, 1000.0f, 1000.0f, false);
		TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> AOCaches;
		TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ThicknessCaches;
		TArray<FColor> Out1;
		TestTrue(TEXT("First evaluation succeeds"),
			VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, Out1));

		MutateThicknessParams(Stack, LayerId, [](FVertexMaskForgeThicknessParams& P) { P.SearchDistance = 42.0f; P.MaxThickness = FMath::Min(P.MaxThickness, 42.0f); });
		TArray<FColor> Out2;
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, Out2);

		const FVertexMaskForgeSourceTopologyThicknessCache* EntryAfter = ThicknessCaches.Find(LayerId);
		if (!TestTrue(TEXT("Cache entry still exists"), EntryAfter != nullptr)) { return false; }
		TestEqual(TEXT("CachedSearchDistance re-stamped to the NEW value -- a real recompute ran"), EntryAfter->CachedSearchDistance, 42.0f);
	}

	return true;
}

// M17-TH-DL-B, 6: Invert is applied exactly ONCE, entirely inside the backend (never duplicated in
// composition's generic Pass 2) -- proven by the exact White-Fill/Copy-blend/Opacity-1.0 algebraic identity
// this composition already relies on (PaintValue = FillValue*EffectiveMask; Composite = PaintValue on a
// Copy blend): if Invert flips EffectiveMask -> 1-EffectiveMask, then Composed(Invert=true) + Composed
// (Invert=false) must equal White (255) per channel EXACTLY, for every corner -- any double-application
// (or a second inversion point in composition) would break this exact identity.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompThicknessInvertAppliedOnceTest, "VertexMaskForge.DynamicSourceTopologyComposition.ThicknessInvertAppliedExactlyOnce", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompThicknessInvertAppliedOnceTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildIrregularOctahedronThicknessFixtureWorkingMesh();
	const int32 NumCorners = WorkingMesh.Mesh->TriangleCount() * 3;
	TArray<FColor> BaseColors;
	BaseColors.Init(FColor::Black, NumCorners);

	TArray<FColor> OutNotInverted, OutInverted;
	for (const bool bInvert : { false, true })
	{
		FVertexMaskForgeDynamicLayerStack Stack;
		AddThicknessLayer(Stack, TEXT("Thickness"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
			0.0f, 1000.0f, 1000.0f, bInvert);
		TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> AOCaches;
		TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ThicknessCaches;
		TArray<FColor>& Out = bInvert ? OutInverted : OutNotInverted;
		TestTrue(*FString::Printf(TEXT("Evaluation succeeds (bInvert=%s)"), bInvert ? TEXT("true") : TEXT("false")),
			VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, Out));
	}

	bool bExactComplementEverywhere = OutNotInverted.Num() == NumCorners && OutInverted.Num() == NumCorners;
	for (int32 i = 0; bExactComplementEverywhere && i < NumCorners; ++i)
	{
		const FColor& A = OutNotInverted[i];
		const FColor& B = OutInverted[i];
		if ((A.R + B.R) != 255 || (A.G + B.G) != 255 || (A.B + B.B) != 255) { bExactComplementEverywhere = false; }
	}
	TestTrue(TEXT("Invert(true) and Invert(false) outputs are EXACT per-channel complements (R+R'==255 etc.) at every corner -- proves a single inversion, never duplicated in composition"), bExactComplementEverywhere);

	return true;
}

// M17-TH-DL-B, 7: Blend Mode, Opacity, and layer reorder are purely Pass-2/recomposition concerns for
// Thickness too -- none of them touch the persistent raycast cache (Tree/RawDistances/CachedSearchDistance/
// CachedBias all remain byte-identical), even though the composed OUTPUT correctly changes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompThicknessBlendOpacityReorderRecomposeOnlyTest, "VertexMaskForge.DynamicSourceTopologyComposition.ThicknessBlendOpacityReorderRecomposeOnly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompThicknessBlendOpacityReorderRecomposeOnlyTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildIrregularOctahedronThicknessFixtureWorkingMesh();
	const int32 NumCorners = WorkingMesh.Mesh->TriangleCount() * 3;
	TArray<FColor> BaseColors;
	BaseColors.Init(FColor(40, 60, 80, 255), NumCorners);

	FVertexMaskForgeDynamicLayerStack Stack;
	const FGuid LayerId = AddThicknessLayer(Stack, TEXT("Thickness"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		0.0f, 1000.0f, 1000.0f, false);
	const FGuid FillLayerId = AddFillOnlyLayer(Stack, TEXT("Fill"), EVertexMaskForgeLayerFill::Black, EVertexMaskForgeBlendMode::Add, 0.5f);

	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> AOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ThicknessCaches;
	TArray<FColor> Out1;
	TestTrue(TEXT("First evaluation succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, Out1));

	const FVertexMaskForgeSourceTopologyThicknessCache* EntryBefore = ThicknessCaches.Find(LayerId);
	if (!TestTrue(TEXT("Cache entry exists"), EntryBefore != nullptr)) { return false; }
	const UE::Geometry::FDynamicMeshAABBTree3* TreeBefore = EntryBefore->Tree.Get();
	const TArray<float> RawDistancesBefore = EntryBefore->RawDistances;
	const float CachedSearchDistanceBefore = EntryBefore->CachedSearchDistance;

	// Change Blend Mode, Opacity, and reorder (move the Thickness layer down one slot) -- none of these are
	// Thickness-specific parameters at all.
	Stack.SetLayerBlendMode(LayerId, EVertexMaskForgeBlendMode::Multiply);
	Stack.SetLayerOpacity(LayerId, 0.5f);
	Stack.MoveLayerDown(LayerId);
	(void)FillLayerId;

	TArray<FColor> Out2;
	TestTrue(TEXT("Second evaluation (Blend/Opacity/reorder changed) succeeds"),
		VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, Out2));

	const FVertexMaskForgeSourceTopologyThicknessCache* EntryAfter = ThicknessCaches.Find(LayerId);
	if (!TestTrue(TEXT("Cache entry still exists"), EntryAfter != nullptr)) { return false; }
	TestTrue(TEXT("Tree pointer UNCHANGED by Blend/Opacity/reorder"), EntryAfter->Tree.Get() == TreeBefore);
	TestEqual(TEXT("CachedSearchDistance UNCHANGED by Blend/Opacity/reorder"), EntryAfter->CachedSearchDistance, CachedSearchDistanceBefore);
	bool bRawDistancesIdentical = EntryAfter->RawDistances.Num() == RawDistancesBefore.Num();
	for (int32 i = 0; bRawDistancesIdentical && i < RawDistancesBefore.Num(); ++i)
	{
		if (EntryAfter->RawDistances[i] != RawDistancesBefore[i]) { bRawDistancesIdentical = false; }
	}
	TestTrue(TEXT("RawDistances byte-identical -- Blend/Opacity/reorder never touch the raw raycast cache"), bRawDistancesIdentical);

	bool bOutputChanged = Out1.Num() == Out2.Num();
	if (bOutputChanged)
	{
		bOutputChanged = false;
		for (int32 i = 0; i < Out1.Num(); ++i) { if (Out1[i] != Out2[i]) { bOutputChanged = true; } }
	}
	TestTrue(TEXT("Composed output DID change -- Blend/Opacity/reorder really are applied, just never invalidate the cache"), bOutputChanged);

	return true;
}

// M17-TH-DL-C: production-path proof that the M9 conservative fallback (automatic, internal, never exposed to
// Dynamic Layers -- see BuildThicknessFallbackRecoveryFixtureWorkingMesh's own doc comment for the full
// geometric derivation) actually fires through this orchestrator's Thickness branch and its recovered value
// reaches the composed Dynamic Layer output, closing the coverage gap left by ThicknessDispatchedTest's own
// fixture (dense via the center ray alone, "no fallback needed").
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeDynSrcTopoCompThicknessFallbackRecoveryReachesComposedOutputTest, "VertexMaskForge.DynamicSourceTopologyComposition.ThicknessFallbackRecoveryReachesComposedOutput", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeDynSrcTopoCompThicknessFallbackRecoveryReachesComposedOutputTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildThicknessFallbackRecoveryFixtureWorkingMesh();
	const int32 NumCorners = WorkingMesh.Mesh->TriangleCount() * 3;
	TestEqual(TEXT("Fixture has 5 triangles / 15 corners (2 PlateA + 2 PlateB + 1 Special)"), NumCorners, 15);

	TArray<FColor> BaseColors;
	BaseColors.Init(FColor::Black, NumCorners);

	FVertexMaskForgeDynamicLayerStack Stack;
	AddThicknessLayer(Stack, TEXT("Thickness"), EVertexMaskForgeLayerFill::White, EVertexMaskForgeBlendMode::Copy, 1.0f,
		/*MinThickness=*/0.0f, /*MaxThickness=*/30.0f, /*SearchDistance=*/50.0f, /*bInvert=*/false);

	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache> AOCaches;
	TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache> ThicknessCaches;
	TArray<FColor> Out;
	const bool bSucceeded = VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(
		WorkingMesh, Stack, BaseColors, FTransform::Identity, AOCaches, ThicknessCaches, Out);

	// The Special triangle's 3 corners (indices 12/13/14, by the fixture's own fixed append order) are
	// analytically unreachable via the center ray alone. Per this orchestrator's strict Corner-domain
	// contract (NumValidValues == ExpectedCornerCount or the whole call fails), a TRUE result here is only
	// possible if the fallback recovered all three -- otherwise NumValidValues would be 12/15 and this would
	// fail exactly like ThicknessNoHitFailsWholeCallTest above.
	if (!TestTrue(TEXT("Fallback recovery for Special's 3 corners lets the whole 15/15 dense call succeed"), bSucceeded))
	{
		return false;
	}
	TestEqual(TEXT("Composed output has one FColor per corner"), Out.Num(), NumCorners);
	if (Out.Num() != NumCorners) { return false; }

	// The recovered value is a pure geometric identity (H+Bias, see the fixture's own derivation) -- all
	// three Special corners must therefore land on the SAME normalized thickness, and it must be a real,
	// non-trivial mid-range value (never 0 -- the no-hit/failure value -- and never saturated at 255 the way
	// an unrelated clamp artifact could be).
	const FColor SpecialA = Out[12];
	const FColor SpecialB = Out[13];
	const FColor SpecialC = Out[14];
	AddInfo(FString::Printf(TEXT("Special corner recovered bytes: [12]=%d [13]=%d [14]=%d"), SpecialA.R, SpecialB.R, SpecialC.R));

	TestTrue(TEXT("Corner 12 recovered a real, non-zero, non-saturated value"), SpecialA.R > 32 && SpecialA.R < 224);
	TestTrue(TEXT("Corner 13 recovered a real, non-zero, non-saturated value"), SpecialB.R > 32 && SpecialB.R < 224);
	TestTrue(TEXT("Corner 14 recovered a real, non-zero, non-saturated value"), SpecialC.R > 32 && SpecialC.R < 224);
	TestTrue(TEXT("All 3 recovered corners agree (same underlying H+Bias projected distance)"),
		FMath::Abs(SpecialA.R - SpecialB.R) <= 1 && FMath::Abs(SpecialA.R - SpecialC.R) <= 1);

	// PlateA/PlateB's own 12 corners hit directly (center ray only, H+Bias as well, by construction) -- the
	// Special corners' recovered value must match them too, since it is analytically the SAME distance.
	const FColor DirectHitSample = Out[0];
	TestTrue(TEXT("Special's fallback-recovered value matches PlateA/PlateB's own direct-hit value (both measure the same H+Bias separation)"),
		FMath::Abs(SpecialA.R - DirectHitSample.R) <= 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
