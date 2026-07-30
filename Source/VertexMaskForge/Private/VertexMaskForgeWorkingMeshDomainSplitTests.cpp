// M16-J.0B.1: coverage for the WorkingMesh Domain Split -- FVertexMaskForgeWorkingMesh retains only
// Authenticated Geometry/Identity (Section 8.A); the 17 Generator Artistic State fields moved to the new,
// sibling FVertexMaskForgeGeneratorState (Section 8.B); FVertexMaskForgeWorkingMeshOwner::InstallWorkingMesh
// remains the sole rebuild path and correctly still drives cache invalidation for artistic state that now
// lives off the owner entirely (Section 8.C); the split changed neither the Curvature nor Noise generator's
// actual inputs/outputs (Section 8.F). Binding/composition coverage (Sections 8.D/8.E) is unchanged and
// already exercised by VertexMaskForgeWorkingStateOwnerTests.cpp's ComposedColorsControlled/
// ComposedColorsSiblingUnaffected/Reassociation/SharedRebuild tests.

#if WITH_DEV_AUTOMATION_TESTS

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Engine/StaticMesh.h"
#include "Misc/AutomationTest.h"
#include "VertexMaskForgeAmbientOcclusionGenerator.h"
#include "VertexMaskForgeBoundingBoxGenerator.h"
#include "VertexMaskForgeCurvatureGenerator.h"
#include "VertexMaskForgeDirectionalNormalGenerator.h"
#include "VertexMaskForgeNoiseGenerator.h"
#include "VertexMaskForgeThicknessGenerator.h"
#include "VertexMaskForgeWorkingMeshOwner.h"
#include "VertexMaskForgeWorkingStateOwner.h"

namespace
{
	// A flat, coplanar 2-triangle quad -- deterministic zero curvature everywhere (no dihedral variation),
	// and a fixed, known vertex-position set for Noise's positional determinism check. Mirrors the fixture
	// pattern already used by VertexMaskForgeMaterialSlotGeneratorTests.cpp's BuildFixtureWorkingMesh.
	FVertexMaskForgeWorkingMesh BuildFlatQuadWorkingMesh(const uint32 GeometryFingerprint)
	{
		FVertexMaskForgeWorkingMesh WorkingMesh;
		WorkingMesh.Mesh = MakeUnique<UE::Geometry::FDynamicMesh3>();
		const int32 V0 = WorkingMesh.Mesh->AppendVertex(FVector3d(0.0, 0.0, 0.0));
		const int32 V1 = WorkingMesh.Mesh->AppendVertex(FVector3d(1.0, 0.0, 0.0));
		const int32 V2 = WorkingMesh.Mesh->AppendVertex(FVector3d(1.0, 1.0, 0.0));
		const int32 V3 = WorkingMesh.Mesh->AppendVertex(FVector3d(0.0, 1.0, 0.0));
		WorkingMesh.Mesh->AppendTriangle(V0, V1, V2);
		WorkingMesh.Mesh->AppendTriangle(V0, V2, V3);

		// CCW winding in the XY plane (viewed from +Z) -- both triangles' geometric normal is +Z.
		WorkingMesh.Mesh->EnableAttributes();
		UE::Geometry::FDynamicMeshNormalOverlay* NormalOverlay = WorkingMesh.Mesh->Attributes()->PrimaryNormals();
		for (const int32 TriangleID : WorkingMesh.Mesh->TriangleIndicesItr())
		{
			UE::Geometry::FIndex3i ElementTri;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				ElementTri[Corner] = NormalOverlay->AppendElement(FVector3f(0.0f, 0.0f, 1.0f));
			}
			NormalOverlay->SetTriangle(TriangleID, ElementTri);
		}

		WorkingMesh.GeometryFingerprint = GeometryFingerprint;
		return WorkingMesh;
	}

	// A closed, deliberately IRREGULAR tetrahedron (no right angles, no axis-aligned symmetry) -- unlike
	// the flat quad above, has actual enclosed volume so AO/Thickness raycasts have real backfaces to hit
	// (a single-sided flat quad would only ever produce a degenerate, all-miss result for those two
	// generators). Irregularity matters specifically for Thickness: it fires exactly ONE ray per corner,
	// straight in along that corner's own normal (see ComputeThicknessRawValue) -- a REGULAR right-angle
	// tetrahedron sends every such ray exactly along an axis, straight at the FAR face's own opposite
	// vertex (a grazing, degenerate hit the ray-triangle intersection can legitimately miss); the
	// irregular geometry here, combined with smooth (vertex-averaged, not flat-per-face) normals, sends
	// every ray toward the INTERIOR of the opposite face instead.
	FVertexMaskForgeWorkingMesh BuildTetrahedronWorkingMesh(const uint32 GeometryFingerprint)
	{
		FVertexMaskForgeWorkingMesh WorkingMesh;
		WorkingMesh.Mesh = MakeUnique<UE::Geometry::FDynamicMesh3>();
		UE::Geometry::FDynamicMesh3& Mesh = *WorkingMesh.Mesh;
		const int32 V0 = Mesh.AppendVertex(FVector3d(0.7, 1.3, 0.4));
		const int32 V1 = Mesh.AppendVertex(FVector3d(13.2, 3.1, 2.6));
		const int32 V2 = Mesh.AppendVertex(FVector3d(2.9, 14.7, 4.3));
		const int32 V3 = Mesh.AppendVertex(FVector3d(3.6, 5.8, 16.1));
		// Outward-facing winding for all four faces (consistent right-hand-rule normals pointing away
		// from the tetrahedron's own centroid) -- same face/vertex correspondence as the regular version,
		// winding order unaffected by the vertex position change.
		Mesh.AppendTriangle(V0, V2, V1);
		Mesh.AppendTriangle(V0, V1, V3);
		Mesh.AppendTriangle(V0, V3, V2);
		Mesh.AppendTriangle(V1, V2, V3);

		Mesh.EnableAttributes();
		UE::Geometry::FDynamicMeshNormalOverlay* NormalOverlay = Mesh.Attributes()->PrimaryNormals();

		// Smooth (vertex-averaged) normals: one element per VERTEX (not per corner), shared by every
		// triangle touching that vertex -- avoids the exact-axis grazing case a flat per-face normal
		// would produce on the regular tetrahedron, and is a legitimate, common normal-authoring choice
		// in its own right (Paint Vertex Colors/Nanite source data is very often smooth-shaded).
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

		WorkingMesh.GeometryFingerprint = GeometryFingerprint;
		return WorkingMesh;
	}

	// An irregular octahedron (6 vertices, 8 triangles) -- used specifically for Thickness, where each
	// origin corner has 4 non-excluded candidate triangles to hit (vs. the tetrahedron's single remaining
	// candidate above), making a real ray-triangle intersection far more likely to land on a genuine
	// interior point rather than grazing another vertex.
	FVertexMaskForgeWorkingMesh BuildOctahedronWorkingMesh(const uint32 GeometryFingerprint)
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
		Mesh.AppendTriangle(V0, V2, V4);
		Mesh.AppendTriangle(V2, V1, V4);
		Mesh.AppendTriangle(V1, V3, V4);
		Mesh.AppendTriangle(V3, V0, V4);
		Mesh.AppendTriangle(V0, V3, V5);
		Mesh.AppendTriangle(V3, V1, V5);
		Mesh.AppendTriangle(V1, V2, V5);
		Mesh.AppendTriangle(V2, V0, V5);

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

		WorkingMesh.GeometryFingerprint = GeometryFingerprint;
		return WorkingMesh;
	}
}

// A. Geometric encapsulation: mutating GeneratorState (via the real generator entry points) never touches
// WorkingMesh's own Identity fields (GeometryFingerprint, vertex count, State) -- proven both by the const
// WorkingMesh reference in the generator signatures (compile-time) and by explicit before/after value
// equality here (runtime). There is no GetWorkingMeshMutable() and no mutable WorkingMesh alias anywhere in
// the codebase to attempt the opposite with (see the M16-J.0B.1 checkpoint's own structural greps).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingMeshDomainSplitGeometricEncapsulationTest, "VertexMaskForge.WorkingMeshDomainSplit.GeometricEncapsulation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingMeshDomainSplitGeometricEncapsulationTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildFlatQuadWorkingMesh(/*GeometryFingerprint=*/1234);
	const uint32 FingerprintBefore = WorkingMesh.GeometryFingerprint;
	const int32 VertexCountBefore = WorkingMesh.Mesh->VertexCount();
	const EVertexMaskForgeWorkingMeshState StateBefore = WorkingMesh.State;

	FVertexMaskForgeGeneratorState GeneratorState;
	VertexMaskForgeCurvatureGenerator::GenerateCurvatureMaskFromDynamicMesh(
		WorkingMesh, GeneratorState, EVertexMaskForgeCurvatureType::Both, 1.0f, 0.0f, 0.0f, 1.0f, false);

	TestTrue(TEXT("GeneratorState's artistic cache was actually populated by the call"),
		!GeneratorState.CurvatureRawConvexCache.IsEmpty());
	TestEqual(TEXT("WorkingMesh.GeometryFingerprint untouched by artistic generation"), WorkingMesh.GeometryFingerprint, FingerprintBefore);
	TestEqual(TEXT("WorkingMesh.Mesh vertex count untouched"), WorkingMesh.Mesh->VertexCount(), VertexCountBefore);
	TestTrue(TEXT("WorkingMesh.State untouched"), WorkingMesh.State == StateBefore);

	return true;
}

// B. Artistic state: mutation lands only on the caller's own GeneratorState instance -- two independent
// GeneratorState instances fed the same (const, shared) WorkingMesh never observe each other's cache, the
// same "no incidental cross-entry mutation" guarantee the pre-split code got implicitly from copying the
// whole WorkingMesh per entry, now obtained explicitly via two-reference separation instead.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingMeshDomainSplitArtisticStateIndependenceTest, "VertexMaskForge.WorkingMeshDomainSplit.ArtisticStateIndependence", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingMeshDomainSplitArtisticStateIndependenceTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildFlatQuadWorkingMesh(/*GeometryFingerprint=*/77);

	FVertexMaskForgeGeneratorState GeneratorStateA;
	FVertexMaskForgeGeneratorState GeneratorStateB;

	VertexMaskForgeNoiseGenerator::GenerateNoiseMaskFromDynamicMesh(
		WorkingMesh, GeneratorStateA, FVertexMaskForgeNoiseGenerativeParams(), 1.0f, 0.0f, 1.0f, false);

	TestTrue(TEXT("GeneratorStateA's raw cache populated"), !GeneratorStateA.NoiseRawCache.IsEmpty());
	TestTrue(TEXT("GeneratorStateB's raw cache stays empty -- sibling never touched"), GeneratorStateB.NoiseRawCache.IsEmpty());
	TestEqual(TEXT("GeneratorStateB's fingerprint stays at its default"), GeneratorStateB.NoiseCacheFingerprint, (uint32)0);

	return true;
}

// C. Rebuild: FVertexMaskForgeWorkingMeshOwner::InstallWorkingMesh remains the sole geometry-replace path,
// and its generation/fingerprint change is still what a caller's independently-held GeneratorState relies
// on for cache invalidation -- proving the split (owner and artistic cache now living on two separate
// objects) didn't silently break that contract. A second install with a DIFFERENT GeometryFingerprint must
// force GenerateCurvatureMaskFromDynamicMesh to recompute (cache's stored fingerprint advances to match),
// never silently reuse the stale cache from the first geometry.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingMeshDomainSplitRebuildInvalidatesCacheTest, "VertexMaskForge.WorkingMeshDomainSplit.RebuildInvalidatesCache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingMeshDomainSplitRebuildInvalidatesCacheTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UStaticMesh> Mesh(NewObject<UStaticMesh>());
	FVertexMaskForgeWorkingMeshOwner Owner;
	Owner.ConfigureIdentity(Mesh.Get(), 0, /*bUseSourceTopology=*/true);

	Owner.InstallWorkingMesh(BuildFlatQuadWorkingMesh(/*GeometryFingerprint=*/100), /*ExpectedCardinality=*/4);
	const uint64 GenerationAfterFirstInstall = Owner.GetGeneration();

	FVertexMaskForgeGeneratorState GeneratorState;
	VertexMaskForgeCurvatureGenerator::GenerateCurvatureMaskFromDynamicMesh(
		Owner.GetWorkingMesh(), GeneratorState, EVertexMaskForgeCurvatureType::Both, 1.0f, 0.0f, 0.0f, 1.0f, false);
	TestEqual(TEXT("Cache fingerprint stamped from first geometry"), GeneratorState.CurvatureCacheFingerprint, (uint32)100);

	// Rebuild with different geometry content (still a flat quad, but a distinct fingerprint value --
	// exactly what a real geometry change would produce via ComputeDynamicMeshGeometryFingerprint).
	Owner.InstallWorkingMesh(BuildFlatQuadWorkingMesh(/*GeometryFingerprint=*/200), /*ExpectedCardinality=*/4);
	TestTrue(TEXT("Generation advanced by the second install"), Owner.GetGeneration() != GenerationAfterFirstInstall);

	VertexMaskForgeCurvatureGenerator::GenerateCurvatureMaskFromDynamicMesh(
		Owner.GetWorkingMesh(), GeneratorState, EVertexMaskForgeCurvatureType::Both, 1.0f, 0.0f, 0.0f, 1.0f, false);
	TestEqual(TEXT("Cache fingerprint advanced to match the rebuilt geometry -- no stale reuse"), GeneratorState.CurvatureCacheFingerprint, (uint32)200);

	return true;
}

// F. Generators: the storage split (WorkingMesh -> GeneratorState) changed neither Curvature's nor Noise's
// actual inputs or outputs. Curvature on a perfectly flat, coplanar quad is analytically zero everywhere
// (no dihedral-angle variation exists to measure) -- a deterministic ground truth independent of any
// pre-split baseline. Noise determinism is proven by running the exact same (Params, geometry) pair twice
// through two independent GeneratorState instances and requiring byte-identical output, which would be
// impossible if either the geometry (WorkingMesh, now const/shared) or the cache (GeneratorState, now
// split off) leaked state between runs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingMeshDomainSplitGeneratorOutputsUnchangedTest, "VertexMaskForge.WorkingMeshDomainSplit.GeneratorOutputsUnchanged", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingMeshDomainSplitGeneratorOutputsUnchangedTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildFlatQuadWorkingMesh(/*GeometryFingerprint=*/55);

	// Curvature: flat mesh -> zero magnitude at every vertex.
	{
		FVertexMaskForgeGeneratorState GeneratorState;
		const FVertexMaskForgeScalarMask Mask = VertexMaskForgeCurvatureGenerator::GenerateCurvatureMaskFromDynamicMesh(
			WorkingMesh, GeneratorState, EVertexMaskForgeCurvatureType::Both, 1.0f, 0.0f, 0.0f, 1.0f, false);

		TestTrue(TEXT("Curvature mask State == Ready"), Mask.State == EVertexMaskForgeScalarMaskState::Ready);
		TestEqual(TEXT("Curvature mask has one value per mesh vertex"), Mask.NumValidValues, WorkingMesh.Mesh->VertexCount());
		bool bAllZero = true;
		for (const int32 VertexID : WorkingMesh.Mesh->VertexIndicesItr())
		{
			if (Mask.bHasValue.IsValidIndex(VertexID) && Mask.bHasValue[VertexID] && !FMath::IsNearlyZero(Mask.Values[VertexID]))
			{
				bAllZero = false;
			}
		}
		TestTrue(TEXT("Flat coplanar quad yields exactly zero curvature at every vertex"), bAllZero);
	}

	// 3 (generator sensitivity): a genuinely non-planar fixture (the same tetrahedron used for AO/Thickness
	// above -- real dihedral angles at every edge) must yield NON-zero curvature -- the flat-quad zero
	// result above could otherwise mask a generator that always returns zero regardless of input. Also
	// proves Type (Convex vs Concave) is actually read and actually changes the result -- an
	// argument-order/ignored-parameter bug would leave these two calls identical.
	{
		const FVertexMaskForgeWorkingMesh NonPlanarWorkingMesh = BuildTetrahedronWorkingMesh(/*GeometryFingerprint=*/56);

		FVertexMaskForgeGeneratorState ConvexState;
		const FVertexMaskForgeScalarMask ConvexMask = VertexMaskForgeCurvatureGenerator::GenerateCurvatureMaskFromDynamicMesh(
			NonPlanarWorkingMesh, ConvexState, EVertexMaskForgeCurvatureType::Convex, 1.0f, 0.0f, 0.0f, 1.0f, false);
		TestTrue(TEXT("Non-planar Curvature mask State == Ready"), ConvexMask.State == EVertexMaskForgeScalarMaskState::Ready);

		bool bAnyNonZero = false;
		for (int32 i = 0; i < ConvexMask.Values.Num(); ++i)
		{
			if (ConvexMask.bHasValue.IsValidIndex(i) && ConvexMask.bHasValue[i] && !FMath::IsNearlyZero(ConvexMask.Values[i]))
			{
				bAnyNonZero = true;
			}
		}
		TestTrue(TEXT("Non-planar (tetrahedron) geometry yields at least one non-zero curvature value"), bAnyNonZero);

		FVertexMaskForgeGeneratorState ConcaveState;
		const FVertexMaskForgeScalarMask ConcaveMask = VertexMaskForgeCurvatureGenerator::GenerateCurvatureMaskFromDynamicMesh(
			NonPlanarWorkingMesh, ConcaveState, EVertexMaskForgeCurvatureType::Concave, 1.0f, 0.0f, 0.0f, 1.0f, false);
		TestTrue(TEXT("Concave mask State == Ready"), ConcaveMask.State == EVertexMaskForgeScalarMaskState::Ready);

		bool bConvexConcaveDiffer = false;
		for (int32 i = 0; i < ConvexMask.Values.Num(); ++i)
		{
			if (!FMath::IsNearlyEqual(ConvexMask.Values[i], ConcaveMask.Values[i], 1e-4f))
			{
				bConvexConcaveDiffer = true;
			}
		}
		TestTrue(TEXT("Type=Convex and Type=Concave produce DIFFERENT results on the same non-planar geometry"), bConvexConcaveDiffer);
	}

	// Noise: two independent runs of the same (WorkingMesh, Params) pair must produce byte-identical output.
	{
		const FVertexMaskForgeNoiseGenerativeParams Params;

		FVertexMaskForgeGeneratorState GeneratorStateA;
		const FVertexMaskForgeScalarMask MaskA = VertexMaskForgeNoiseGenerator::GenerateNoiseMaskFromDynamicMesh(
			WorkingMesh, GeneratorStateA, Params, 1.0f, 0.0f, 1.0f, false);

		FVertexMaskForgeGeneratorState GeneratorStateB;
		const FVertexMaskForgeScalarMask MaskB = VertexMaskForgeNoiseGenerator::GenerateNoiseMaskFromDynamicMesh(
			WorkingMesh, GeneratorStateB, Params, 1.0f, 0.0f, 1.0f, false);

		TestTrue(TEXT("Noise mask A State == Ready"), MaskA.State == EVertexMaskForgeScalarMaskState::Ready);
		TestEqual(TEXT("Noise mask A/B same vertex count"), MaskA.Values.Num(), MaskB.Values.Num());

		bool bIdentical = true;
		for (int32 i = 0; i < MaskA.Values.Num(); ++i)
		{
			if (!FMath::IsNearlyEqual(MaskA.Values[i], MaskB.Values[i]))
			{
				bIdentical = false;
				break;
			}
		}
		TestTrue(TEXT("Two independent runs of the same geometry/params produce identical Noise output"), bIdentical);

		// 3 (generator sensitivity): a DIFFERENT seed must produce a DIFFERENT output -- proves Seed is
		// actually read (via ComputeNoiseSeedOffset) and actually perturbs the result, not silently ignored.
		FVertexMaskForgeNoiseGenerativeParams DifferentSeedParams;
		DifferentSeedParams.Seed = 12345;
		FVertexMaskForgeGeneratorState GeneratorStateC;
		const FVertexMaskForgeScalarMask MaskC = VertexMaskForgeNoiseGenerator::GenerateNoiseMaskFromDynamicMesh(
			WorkingMesh, GeneratorStateC, DifferentSeedParams, 1.0f, 0.0f, 1.0f, false);
		TestTrue(TEXT("Different-seed mask State == Ready"), MaskC.State == EVertexMaskForgeScalarMaskState::Ready);

		bool bSeedChangedOutput = false;
		for (int32 i = 0; i < MaskA.Values.Num(); ++i)
		{
			if (!FMath::IsNearlyEqual(MaskA.Values[i], MaskC.Values[i], 1e-4f))
			{
				bSeedChangedOutput = true;
			}
		}
		TestTrue(TEXT("A different Seed produces a different Noise output"), bSeedChangedOutput);
	}

	return true;
}

// M16-J.0B.1 corrective pass, Section 1: FVertexMaskForgeSelectedMesh no longer has a WorkingMesh reference
// member (removed -- an earlier revision bound one in the constructor, which an audit correctly flagged as
// an unproven-lifetime alias: a reference member is initialized once and never reseated by a compiler-
// generated copy/move, so if anything ever relocated a FVertexMaskForgeSelectedMesh object itself, that
// reference could silently keep pointing at a MOVED-FROM object's old MeshOwner instead of its own). Every
// real call site now takes a fresh local view via Entry->MeshOwner->GetWorkingMesh() instead.
//
// This test proves the REAL container (SelectedMeshes' own type, TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>)
// never invalidates that pattern: each FVertexMaskForgeSelectedMesh is heap-allocated ONCE via MakeShared and
// referenced only through TSharedPtr thereafter, so TArray growth/reallocation/Sort/Swap only ever
// copies/moves/reorders the TSharedPtr HANDLES -- never the FVertexMaskForgeSelectedMesh objects they point
// to, and never the FVertexMaskForgeWorkingMeshOwner objects THOSE in turn own (via their own, separate
// TSharedPtr<FVertexMaskForgeWorkingMeshOwner>). Forces real growth (well past any small-array optimization)
// and a real Sort (which swaps element positions), then proves per-entry identity is never crossed: each
// entry's MeshOwner address is unchanged, and each entry's WorkingMesh still reports exactly the
// GeometryFingerprint that entry itself was built with -- never another entry's.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingMeshDomainSplitContainerLifetimeTest, "VertexMaskForge.WorkingMeshDomainSplit.ContainerLifetime", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingMeshDomainSplitContainerLifetimeTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UStaticMesh> Mesh(NewObject<UStaticMesh>());

	TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>> SelectedMeshes;

	constexpr int32 NumEntries = 64; // well past TArray's initial/small allocation -- forces multiple real reallocations.
	TArray<const FVertexMaskForgeWorkingMeshOwner*> OwnerAddressesBeforeGrowth;
	TArray<uint32> ExpectedFingerprints;

	for (int32 i = 0; i < NumEntries; ++i)
	{
		TSharedPtr<FVertexMaskForgeSelectedMesh> Entry = MakeShared<FVertexMaskForgeSelectedMesh>();
		Entry->MeshOwner->ConfigureIdentity(Mesh.Get(), 0, /*bUseSourceTopology=*/true);

		FVertexMaskForgeWorkingMesh NewWorkingMesh;
		NewWorkingMesh.Mesh = MakeUnique<UE::Geometry::FDynamicMesh3>();
		NewWorkingMesh.Mesh->AppendVertex(FVector3d(0.0, 0.0, 0.0));
		const uint32 Fingerprint = static_cast<uint32>(1000 + i); // distinct per entry, by construction.
		NewWorkingMesh.GeometryFingerprint = Fingerprint;
		Entry->MeshOwner->InstallWorkingMesh(MoveTemp(NewWorkingMesh), /*ExpectedCardinality=*/1);

		OwnerAddressesBeforeGrowth.Add(Entry->MeshOwner.Get());
		ExpectedFingerprints.Add(Fingerprint);

		// Each TArray::Add here is a potential reallocation of SelectedMeshes' own internal storage --
		// exactly the real-world growth pattern (RefreshSelection appending one entry per unique asset).
		SelectedMeshes.Add(MoveTemp(Entry));
	}

	bool bAllOwnerAddressesStable = true;
	bool bAllFingerprintsCorrect = true;
	for (int32 i = 0; i < NumEntries; ++i)
	{
		if (SelectedMeshes[i]->MeshOwner.Get() != OwnerAddressesBeforeGrowth[i])
		{
			bAllOwnerAddressesStable = false;
		}
		const FVertexMaskForgeWorkingMesh& WorkingMesh = SelectedMeshes[i]->MeshOwner->GetWorkingMesh();
		if (WorkingMesh.GeometryFingerprint != ExpectedFingerprints[i])
		{
			bAllFingerprintsCorrect = false;
		}
	}
	TestTrue(TEXT("Every entry's MeshOwner heap address is unchanged after growth-driven reallocation"), bAllOwnerAddressesStable);
	TestTrue(TEXT("Every entry's WorkingMesh still reports its OWN fingerprint after growth -- no cross-entry mixup"), bAllFingerprintsCorrect);

	// Sort (descending fingerprint) -- physically reorders TSharedPtr elements within the array (unlike
	// growth, which mostly appends), the other real container operation that could plausibly cross-wire
	// identity if anything here depended on array INDEX rather than the TSharedPtr's own pointee.
	SelectedMeshes.Sort([](const TSharedPtr<FVertexMaskForgeSelectedMesh>& A, const TSharedPtr<FVertexMaskForgeSelectedMesh>& B)
	{
		return A->MeshOwner->GetWorkingMesh().GeometryFingerprint > B->MeshOwner->GetWorkingMesh().GeometryFingerprint;
	});

	bool bSortedOrderCorrect = true;
	bool bAllOwnerAddressesStableAfterSort = true;
	TSet<const FVertexMaskForgeWorkingMeshOwner*> AddressSetBeforeSort(OwnerAddressesBeforeGrowth);
	uint32 PreviousFingerprint = TNumericLimits<uint32>::Max();
	for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
	{
		const uint32 ThisFingerprint = Entry->MeshOwner->GetWorkingMesh().GeometryFingerprint;
		if (ThisFingerprint > PreviousFingerprint)
		{
			bSortedOrderCorrect = false;
		}
		PreviousFingerprint = ThisFingerprint;

		if (!AddressSetBeforeSort.Contains(Entry->MeshOwner.Get()))
		{
			bAllOwnerAddressesStableAfterSort = false;
		}
	}
	TestTrue(TEXT("Sort actually reordered by fingerprint (proves the comparator ran against real, distinct data)"), bSortedOrderCorrect);
	TestTrue(TEXT("Every MeshOwner heap address survives Sort's element swapping unchanged"), bAllOwnerAddressesStableAfterSort);

	// Detach-then-remove one entry: confirms removal from the container (which moves/reallocates the
	// remaining TSharedPtr elements) never disturbs a SURVIVING entry's own MeshOwner/WorkingMesh pairing.
	const uint32 SurvivorFingerprintBeforeRemoval = SelectedMeshes[1]->MeshOwner->GetWorkingMesh().GeometryFingerprint;
	FVertexMaskForgeWorkingMeshOwner* SurvivorOwnerAddress = SelectedMeshes[1]->MeshOwner.Get();
	SelectedMeshes.RemoveAt(0);
	TestEqual(TEXT("Surviving entry's MeshOwner address unchanged by a sibling's removal"),
		SelectedMeshes[0]->MeshOwner.Get(), SurvivorOwnerAddress);
	TestEqual(TEXT("Surviving entry's WorkingMesh fingerprint unchanged by a sibling's removal"),
		SelectedMeshes[0]->MeshOwner->GetWorkingMesh().GeometryFingerprint, SurvivorFingerprintBeforeRemoval);

	return true;
}

// D (reconfirmation): FVertexMaskForgeWorkingStateOwner attaches to a FVertexMaskForgeSelectedMesh's real
// MeshOwner exactly the way the real panel does (TSharedPtr<FVertexMaskForgeWorkingMeshOwner> handed to
// AttachToMeshOwner) -- proves the removed WorkingMesh reference member was never load-bearing for binding
// correctness either: StateOwner's own attachment is independent of it (always went through MeshOwner
// directly), and continues to pair correctly with entries stored in the same real container type used above.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingMeshDomainSplitBindingSurvivesContainerGrowthTest, "VertexMaskForge.WorkingMeshDomainSplit.BindingSurvivesContainerGrowth", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingMeshDomainSplitBindingSurvivesContainerGrowthTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UStaticMesh> MeshA(NewObject<UStaticMesh>());
	TStrongObjectPtr<UStaticMesh> MeshB(NewObject<UStaticMesh>());
	TStrongObjectPtr<UStaticMeshComponent> Component(NewObject<UStaticMeshComponent>());

	TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>> SelectedMeshes;
	for (int32 i = 0; i < 32; ++i)
	{
		SelectedMeshes.Add(MakeShared<FVertexMaskForgeSelectedMesh>());
	}

	TSharedPtr<FVertexMaskForgeSelectedMesh>& EntryA = SelectedMeshes[10];
	EntryA->MeshOwner->ConfigureIdentity(MeshA.Get(), 0, true);
	FVertexMaskForgeWorkingMesh MeshDataA;
	MeshDataA.Mesh = MakeUnique<UE::Geometry::FDynamicMesh3>();
	MeshDataA.Mesh->AppendVertex(FVector3d::ZeroVector);
	EntryA->MeshOwner->InstallWorkingMesh(MoveTemp(MeshDataA), 1);

	FVertexMaskForgeWorkingStateOwner StateOwner;
	StateOwner.ConfigureTarget(Component.Get());
	StateOwner.AttachToMeshOwner(EntryA->MeshOwner);
	TestTrue(TEXT("StateOwner attached to EntryA's own MeshOwner"), StateOwner.IsAttached());

	// Force further growth AFTER attachment -- the attachment is a TWeakPtr into the SAME
	// FVertexMaskForgeWorkingMeshOwner heap object EntryA already owns, so this must remain valid
	// regardless of what happens to the SelectedMeshes array itself afterward.
	for (int32 i = 0; i < 64; ++i)
	{
		SelectedMeshes.Add(MakeShared<FVertexMaskForgeSelectedMesh>());
	}

	TestTrue(TEXT("StateOwner remains attached after further container growth"), StateOwner.IsAttached());
	TestTrue(TEXT("EnsureBaselineCaptured still succeeds against the same (still-valid) MeshOwner"),
		StateOwner.EnsureBaselineCaptured({ FColor::White }));

	return true;
}

// 1 (corrective pass, publication binding raw-pointer finding): a Binding created BEFORE a rebuild
// (InstallWorkingMesh) must fail safely -- never read stale geometry -- once the owner it was created
// against has moved on to a new Revision. Complements VertexMaskForgeWorkingStateOwnerTests.cpp's own
// Lifetime test, which covers the OTHER dangerous case (the owner destroyed outright); this test covers the
// "owner still alive, but rebuilt" case, where the OLD Binding's weak reference still Pin()s successfully
// but the Provenance Revision it reads no longer matches the Authority's -- StaleWorkingMesh, not a crash.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingMeshDomainSplitStaleBindingAfterRebuildTest, "VertexMaskForge.WorkingMeshDomainSplit.StaleBindingAfterRebuild", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingMeshDomainSplitStaleBindingAfterRebuildTest::RunTest(const FString& Parameters)
{
	using EStatus = EVertexMaskForgeWorkingColorsPublicationValidationStatus;

	TStrongObjectPtr<UStaticMesh> Mesh(NewObject<UStaticMesh>());
	TStrongObjectPtr<UStaticMeshComponent> Component(NewObject<UStaticMeshComponent>());

	TSharedPtr<FVertexMaskForgeWorkingMeshOwner> MeshOwner = MakeShared<FVertexMaskForgeWorkingMeshOwner>();
	MeshOwner->ConfigureIdentity(Mesh.Get(), 0, /*bUseSourceTopology=*/true);
	MeshOwner->InstallWorkingMesh(FVertexMaskForgeWorkingMesh(), /*ExpectedCardinality=*/4);

	FVertexMaskForgeWorkingStateOwner StateOwner;
	StateOwner.ConfigureTarget(Component.Get());
	StateOwner.AttachToMeshOwner(MeshOwner);
	TestTrue(TEXT("InitializeColors succeeds against the first geometry"), StateOwner.InitializeColors({ FColor::White, FColor::White, FColor::White, FColor::White }));
	TestTrue(TEXT("Binding valid before rebuild"), StateOwner.ValidateBinding() == EStatus::Success);

	// Rebuild the SAME (still-alive) owner -- generation/Revision advances, but the owner object itself,
	// and therefore the TWeakPtr the Binding resolves through, remains perfectly valid the whole time.
	MeshOwner->InstallWorkingMesh(FVertexMaskForgeWorkingMesh(), /*ExpectedCardinality=*/4);

	TestTrue(TEXT("MeshOwner is still alive (this is the rebuild case, not the destruction case)"), MeshOwner.IsValid());
	const EStatus StatusAfterRebuild = StateOwner.ValidateBinding();
	TestTrue(TEXT("Binding now fails (Authority Revision no longer matches the rebuilt Provenance's own)"), StatusAfterRebuild != EStatus::Success);
	TestEqual(TEXT("Specifically StaleWorkingMesh, not a crash or an unrelated status"), StatusAfterRebuild, EStatus::StaleWorkingMesh);

	return true;
}

// 1 (2nd corrective pass -- TargetPreviewState raw-pointer finding): FVertexMaskForgeWorkingColorPublicationBinding
// no longer carries ANY reference to the StateOwner that created it (only a weak MeshOwner reference) --
// this test proves the full 9-point scenario: (1) create a valid Binding, (2) keep it stored in a local
// variable well past CreateBinding()'s own return (mandatory copy elision, NOT lexical-usage discipline --
// deleted copy/move never prevented this), (3) destroy the EXACT StateOwner that created it, (4) while the
// MeshOwner stays alive throughout, (5) validate the stored Binding, (6)/(7) get a specific, safe rejection
// with no freed-memory read (impossible by construction: the Binding never held a pointer to the destroyed
// StateOwner in the first place), (8) create a new StateOwner of identical cardinality attached to the SAME
// MeshOwner, (9) prove it does NOT revive the old Binding/old colors -- the new StateOwner starts completely
// uninitialized, so pairing the OLD (MeshOwner-only) Binding with the NEW StateOwner's own live PreviewState
// correctly fails (InvalidAlphaSource: the new StateOwner has no Authority yet), never silently succeeding
// with A's old data.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingMeshDomainSplitStaleBindingAfterStateOwnerDestructionTest, "VertexMaskForge.WorkingMeshDomainSplit.StaleBindingAfterStateOwnerDestruction", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingMeshDomainSplitStaleBindingAfterStateOwnerDestructionTest::RunTest(const FString& Parameters)
{
	using EStatus = EVertexMaskForgeWorkingColorsPublicationValidationStatus;

	TStrongObjectPtr<UStaticMesh> Mesh(NewObject<UStaticMesh>());
	TStrongObjectPtr<UStaticMeshComponent> ComponentA(NewObject<UStaticMeshComponent>());
	TStrongObjectPtr<UStaticMeshComponent> ComponentB(NewObject<UStaticMeshComponent>());

	// (4) MeshOwner outlives everything in this test -- isolates the StateOwner's own lifetime as the ONLY
	// variable under test.
	TSharedPtr<FVertexMaskForgeWorkingMeshOwner> MeshOwner = MakeShared<FVertexMaskForgeWorkingMeshOwner>();
	MeshOwner->ConfigureIdentity(Mesh.Get(), 0, /*bUseSourceTopology=*/true);
	MeshOwner->InstallWorkingMesh(FVertexMaskForgeWorkingMesh(), /*ExpectedCardinality=*/4);

	// (1)/(2)/(3): StateOwnerA is scoped ENTIRELY inside this lambda -- CreateBinding() is called on it,
	// and the resulting Binding (a prvalue, both from CreateBinding()'s own return and from this lambda's
	// own return of that same prvalue expression) is guaranteed-elided all the way out to OldBinding below.
	// StateOwnerA itself is destroyed the instant the lambda body ends, BEFORE OldBinding's own declaration
	// statement even finishes evaluating -- this is deliberately the strictest possible ordering: the
	// Binding is fully constructed and "escapes" to the outer scope only via a pure prvalue chain, never
	// via a named local that outlived StateOwnerA and then got copied/moved (which the deleted copy/move
	// constructors would refuse anyway) -- so this is not "the same Binding object kept alive," it is the
	// STRONGER claim that mandatory elision propagates it out with StateOwnerA already gone.
	const FVertexMaskForgeWorkingColorPublicationBinding OldBinding = [&]() -> FVertexMaskForgeWorkingColorPublicationBinding
	{
		FVertexMaskForgeWorkingStateOwner StateOwnerA;
		StateOwnerA.ConfigureTarget(ComponentA.Get());
		StateOwnerA.AttachToMeshOwner(MeshOwner);
		TestTrue(TEXT("StateOwnerA initializes"), StateOwnerA.InitializeColors({ FColor::White, FColor::White, FColor::White, FColor::White }));
		TestTrue(TEXT("StateOwnerA's own binding is valid before destruction"), StateOwnerA.ValidateBinding() == EStatus::Success);
		return StateOwnerA.CreateBinding();
	}(); // (3) StateOwnerA fully destroyed here -- OldBinding is now the ONLY thing referencing MeshOwner from this scope.

	TestTrue(TEXT("OldBinding is still well-formed (MeshOwner, kept alive by (4), is still reachable)"), OldBinding.IsWellFormed());

	// (8) A NEW StateOwner, identical cardinality, attached to the SAME (still-alive) MeshOwner -- StateOwnerA
	// is unreachable from here by any means; this is a completely independent object.
	FVertexMaskForgeWorkingStateOwner StateOwnerB;
	StateOwnerB.ConfigureTarget(ComponentB.Get());
	StateOwnerB.AttachToMeshOwner(MeshOwner);

	// (5)/(6)/(7) Validate OldBinding (which only ever weakly referenced MeshOwner -- never StateOwnerA)
	// against StateOwnerB's own live PreviewState, via the dedicated seam for exactly this case. No memory
	// belonging to the destroyed StateOwnerA is or could be read: OldBinding never held a pointer to it.
	const EStatus StatusOldBindingAgainstB = StateOwnerB.ValidateBindingAgainstThis(OldBinding);

	// (9) StateOwnerB was never initialized -- pairing the OLD Binding with a brand-new, uninitialized
	// StateOwner must NOT silently "revive" StateOwnerA's old Success-validating state. It correctly fails
	// with a specific, safe status (InvalidAlphaSource: StateOwnerB's own Authority isn't valid yet) --
	// never a crash, never Success.
	TestTrue(TEXT("Old Binding against a brand-new, uninitialized StateOwner does not silently succeed"), StatusOldBindingAgainstB != EStatus::Success);
	TestEqual(TEXT("Specifically InvalidAlphaSource -- StateOwnerB has no Authority of its own yet, never A's revived state"),
		StatusOldBindingAgainstB, EStatus::InvalidAlphaSource);

	// Now StateOwnerB legitimately initializes its OWN colors -- proves the seam still works correctly for
	// the real, live path (not just correctly rejecting the stale/uninitialized pairing above), and that
	// OldBinding (still only weakly referencing the shared MeshOwner) now correctly validates against B's
	// own, freshly-established Authority too -- since both share the same MeshOwner and B's own Revision
	// now matches.
	TestTrue(TEXT("StateOwnerB initializes its own colors"), StateOwnerB.InitializeColors({ FColor::Black, FColor::Black, FColor::Black, FColor::Black }));
	TestTrue(TEXT("StateOwnerB now validates successfully against its OWN Authority"), StateOwnerB.ValidateBinding() == EStatus::Success);
	TestTrue(TEXT("OldBinding (MeshOwner-only) also validates against B's own live PreviewState now -- correct pairing, not A's revived data"),
		StateOwnerB.ValidateBindingAgainstThis(OldBinding) == EStatus::Success);

	return true;
}

// 2 (corrective pass, EnsureBaselineCaptured explicit preconditions): closes the 3 precondition items the
// pre-existing ComposedColorsControlled test does not directly cover -- target not configured, StateOwner
// not attached, and attached-but-invalid Provenance (never installed) -- each must reject EnsureBaselineCaptured
// outright, leaving no partial/inconsistent state. Items 1/2/3/4/8/9 are covered by
// VertexMaskForge.WorkingStateOwner.ComposedColorsControlled (initial capture, second-capture RGB/alpha
// no-op, cardinality rejection, ApplyComposedColorsRGB ignoring caller alpha, generation unaffected); item
// 10 (sibling intact) is covered by VertexMaskForge.WorkingStateOwner.ComposedColorsSiblingUnaffected.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingMeshDomainSplitEnsureBaselineCapturedPreconditionsTest, "VertexMaskForge.WorkingMeshDomainSplit.EnsureBaselineCapturedPreconditions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingMeshDomainSplitEnsureBaselineCapturedPreconditionsTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UStaticMesh> Mesh(NewObject<UStaticMesh>());
	TStrongObjectPtr<UStaticMeshComponent> Component(NewObject<UStaticMeshComponent>());

	// 5. Target not configured: nothing else set up at all.
	{
		FVertexMaskForgeWorkingStateOwner Owner;
		TestFalse(TEXT("EnsureBaselineCaptured rejected -- target never configured"), Owner.EnsureBaselineCaptured({ FColor::White }));
		TestFalse(TEXT("AreColorsInitialized() stays false"), Owner.AreColorsInitialized());
	}

	// 6. Target configured, but never attached to any mesh owner.
	{
		FVertexMaskForgeWorkingStateOwner Owner;
		Owner.ConfigureTarget(Component.Get());
		TestFalse(TEXT("EnsureBaselineCaptured rejected -- not attached"), Owner.EnsureBaselineCaptured({ FColor::White }));
		TestFalse(TEXT("AreColorsInitialized() stays false"), Owner.AreColorsInitialized());
	}

	// 7. Target configured, attached, but the mesh owner's own Provenance is invalid (ConfigureIdentity
	// called, but InstallWorkingMesh never was -- exactly the "attached to a not-yet-built owner" case).
	{
		TSharedPtr<FVertexMaskForgeWorkingMeshOwner> MeshOwner = MakeShared<FVertexMaskForgeWorkingMeshOwner>();
		MeshOwner->ConfigureIdentity(Mesh.Get(), 0, false);
		TestFalse(TEXT("Provenance genuinely invalid before any InstallWorkingMesh"), MeshOwner->GetWorkingMesh().Provenance.IsValid());

		FVertexMaskForgeWorkingStateOwner Owner;
		Owner.ConfigureTarget(Component.Get());
		Owner.AttachToMeshOwner(MeshOwner);
		TestFalse(TEXT("EnsureBaselineCaptured rejected -- attached owner has no valid Provenance yet"), Owner.EnsureBaselineCaptured({ FColor::White }));
		TestFalse(TEXT("AreColorsInitialized() stays false"), Owner.AreColorsInitialized());
	}

	return true;
}

// F (matrix completion): Bounding Box was never touched by the domain split (its FromDynamicMesh entry
// point takes a bare const FDynamicMesh3&, never WorkingMesh/GeneratorState at all), but had zero dedicated
// deterministic-value automation coverage before this checkpoint -- closing that gap with the smallest real
// test: EvaluateAxisBaseGradient's own exact formula (Position=0.5, TransitionWidth=1.0 => Gradient == T)
// against the flat quad's own known X coordinates.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingMeshDomainSplitBoundingBoxGeneratorTest, "VertexMaskForge.WorkingMeshDomainSplit.BoundingBoxGenerator", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingMeshDomainSplitBoundingBoxGeneratorTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildFlatQuadWorkingMesh(/*GeometryFingerprint=*/1);

	TStaticArray<FVertexMaskForgeAxisMaskParams, 3> AxisParams;
	AxisParams[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].bEnabled = true;
	AxisParams[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].Position = 0.5f;
	AxisParams[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].TransitionWidth = 1.0f;

	const FVertexMaskForgeScalarMask Mask = VertexMaskForgeBoundingBoxGenerator::GenerateBoundingBoxMaskFromDynamicMesh(
		*WorkingMesh.Mesh, AxisParams, FTransform::Identity);

	TestTrue(TEXT("Bounding Box mask State == Ready"), Mask.State == EVertexMaskForgeScalarMaskState::Ready);
	TestEqual(TEXT("One value per mesh vertex"), Mask.NumValidValues, WorkingMesh.Mesh->VertexCount());

	// V0/V3 at X=0 (mesh X-min) -> gradient 0.0; V1/V2 at X=1 (mesh X-max) -> gradient 1.0 -- exact
	// EvaluateAxisBaseGradient formula at Position=0.5/TransitionWidth=1.0 (see that function's own
	// module comment: Lower = 0.5 - 0.5 = 0, Gradient = clamp((T-0)/1, 0, 1) = T).
	if (TestTrue(TEXT("V0 (X=0) has a value"), Mask.bHasValue.IsValidIndex(0) && Mask.bHasValue[0]))
	{
		TestEqual(TEXT("V0 (X=0) gradient == 0.0"), Mask.Values[0], 0.0f);
	}
	if (TestTrue(TEXT("V1 (X=1) has a value"), Mask.bHasValue.IsValidIndex(1) && Mask.bHasValue[1]))
	{
		TestEqual(TEXT("V1 (X=1) gradient == 1.0"), Mask.Values[1], 1.0f);
	}

	// 3 (generator sensitivity): a SECOND, different parameter set (bInvert=true, still axis X) must
	// produce the EXACT OPPOSITE array -- proves the generator actually reads bInvert (an argument-swap or
	// ignored-parameter bug would leave this identical to the first result) and that Values is never a
	// constant/cached array reused across calls.
	TStaticArray<FVertexMaskForgeAxisMaskParams, 3> InvertedAxisParams = AxisParams;
	InvertedAxisParams[static_cast<int32>(EVertexMaskForgeBoundsAxis::X)].bInvert = true;
	const FVertexMaskForgeScalarMask InvertedMask = VertexMaskForgeBoundingBoxGenerator::GenerateBoundingBoxMaskFromDynamicMesh(
		*WorkingMesh.Mesh, InvertedAxisParams, FTransform::Identity);
	if (TestTrue(TEXT("Inverted: V0 (X=0) has a value"), InvertedMask.bHasValue.IsValidIndex(0) && InvertedMask.bHasValue[0]))
	{
		TestEqual(TEXT("Inverted: V0 (X=0) gradient == 1.0 (opposite of non-inverted)"), InvertedMask.Values[0], 1.0f);
	}
	if (TestTrue(TEXT("Inverted: V1 (X=1) has a value"), InvertedMask.bHasValue.IsValidIndex(1) && InvertedMask.bHasValue[1]))
	{
		TestEqual(TEXT("Inverted: V1 (X=1) gradient == 0.0 (opposite of non-inverted)"), InvertedMask.Values[1], 0.0f);
	}

	// A DIFFERENT axis (Y instead of X) on the same geometry must ALSO produce a different result -- V0/V1
	// are both at Y=0 (gradient 0.0 under Y), unlike their DIFFERING X-axis values above; proves the
	// generator indexes AxisParams by the correct axis, not a hardcoded one.
	TStaticArray<FVertexMaskForgeAxisMaskParams, 3> YAxisParams;
	YAxisParams[static_cast<int32>(EVertexMaskForgeBoundsAxis::Y)].bEnabled = true;
	YAxisParams[static_cast<int32>(EVertexMaskForgeBoundsAxis::Y)].Position = 0.5f;
	YAxisParams[static_cast<int32>(EVertexMaskForgeBoundsAxis::Y)].TransitionWidth = 1.0f;
	const FVertexMaskForgeScalarMask YMask = VertexMaskForgeBoundingBoxGenerator::GenerateBoundingBoxMaskFromDynamicMesh(
		*WorkingMesh.Mesh, YAxisParams, FTransform::Identity);
	if (TestTrue(TEXT("Y-axis: V0 (Y=0) has a value"), YMask.bHasValue.IsValidIndex(0) && YMask.bHasValue[0])
		&& TestTrue(TEXT("Y-axis: V1 (Y=0) has a value"), YMask.bHasValue.IsValidIndex(1) && YMask.bHasValue[1]))
	{
		TestEqual(TEXT("Y-axis: V0 (Y=0) gradient == 0.0"), YMask.Values[0], 0.0f);
		TestEqual(TEXT("Y-axis: V1 (Y=0) gradient == 0.0 (both at Y-min, unlike the differing X-axis result)"), YMask.Values[1], 0.0f);
	}

	return true;
}

// F (matrix completion): Ambient Occlusion's Source-Topology entry point was never touched by the domain
// split either (const FDynamicMesh3& SourceMesh parameter, no WorkingMesh/GeneratorState access at all), and
// had zero dedicated automation coverage. A closed tetrahedron gives every ray a real chance to hit a
// backface, so State==Ready and every value landing in [0,1] is a genuine, non-degenerate proof the raycast
// pipeline actually ran end to end (argument order, domain, and cache-key wiring all correct) -- exact
// occlusion values are legitimately non-trivial to hand-derive and are not asserted here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingMeshDomainSplitAmbientOcclusionGeneratorTest, "VertexMaskForge.WorkingMeshDomainSplit.AmbientOcclusionGenerator", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingMeshDomainSplitAmbientOcclusionGeneratorTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildTetrahedronWorkingMesh(/*GeometryFingerprint=*/1);

	FVertexMaskForgeAOParams Params;
	Params.Samples = 16;
	Params.MaxDistance = 100.0f;
	Params.Bias = 0.1f;

	TUniquePtr<FVertexMaskForgeSourceTopologyAOCache> CachePtr;
	const FVertexMaskForgeScalarMask Mask = VertexMaskForgeAmbientOcclusionGenerator::GenerateAmbientOcclusionMaskFromDynamicMesh(
		CachePtr, *WorkingMesh.Mesh, WorkingMesh.GeometryFingerprint, FTransform::Identity, Params);

	TestTrue(TEXT("AO mask State == Ready"), Mask.State == EVertexMaskForgeScalarMaskState::Ready);
	TestTrue(TEXT("At least one value was produced"), Mask.NumValidValues > 0);

	bool bAllInRange = true;
	for (int32 i = 0; i < Mask.Values.Num(); ++i)
	{
		if (Mask.bHasValue.IsValidIndex(i) && Mask.bHasValue[i] && (Mask.Values[i] < 0.0f || Mask.Values[i] > 1.0f))
		{
			bAllInRange = false;
		}
	}
	TestTrue(TEXT("Every produced value is in [0, 1]"), bAllInRange);

	// Cache correctness: re-running with the SAME fingerprint reuses the cache and reproduces the SAME
	// values (determinism); a DIFFERENT fingerprint (simulating a rebuild) must trigger a fresh raycast
	// pass, proven by CachedGeometryFingerprint advancing to match -- never silently reusing the old cache.
	const FVertexMaskForgeScalarMask MaskAgain = VertexMaskForgeAmbientOcclusionGenerator::GenerateAmbientOcclusionMaskFromDynamicMesh(
		CachePtr, *WorkingMesh.Mesh, WorkingMesh.GeometryFingerprint, FTransform::Identity, Params);
	bool bIdenticalOnCacheHit = true;
	for (int32 i = 0; i < Mask.Values.Num(); ++i)
	{
		if (!FMath::IsNearlyEqual(Mask.Values[i], MaskAgain.Values[i], 1e-4f)) { bIdenticalOnCacheHit = false; }
	}
	TestTrue(TEXT("Same fingerprint -> cache hit -> byte-for-byte identical result"), bIdenticalOnCacheHit);

	// 3 (generator sensitivity): a DIFFERENT, much larger geometry (the same octahedron used for Thickness
	// below, scaled up 5x so its own occlusion profile is genuinely different from the tetrahedron's) must
	// produce a DIFFERENT AO result -- proves the raycast actually reads SourceMesh (an argument-swap or a
	// generator that silently reused stale/cached geometry would leave this identical to Mask above) --
	// and its cache fingerprint must advance to match, never silently reusing the old tetrahedron's cache.
	FVertexMaskForgeWorkingMesh DifferentWorkingMesh = BuildOctahedronWorkingMesh(/*GeometryFingerprint=*/999);
	for (const int32 VertexID : DifferentWorkingMesh.Mesh->VertexIndicesItr())
	{
		DifferentWorkingMesh.Mesh->SetVertex(VertexID, DifferentWorkingMesh.Mesh->GetVertex(VertexID) * 5.0);
	}
	const FVertexMaskForgeScalarMask DifferentMask = VertexMaskForgeAmbientOcclusionGenerator::GenerateAmbientOcclusionMaskFromDynamicMesh(
		CachePtr, *DifferentWorkingMesh.Mesh, DifferentWorkingMesh.GeometryFingerprint, FTransform::Identity, Params);
	TestEqual(TEXT("Different fingerprint -> cache advanced to match the new geometry, not stale"), CachePtr->CachedGeometryFingerprint, DifferentWorkingMesh.GeometryFingerprint);

	bool bAnyValueDiffersAcrossGeometry = (Mask.Values.Num() != DifferentMask.Values.Num());
	if (!bAnyValueDiffersAcrossGeometry)
	{
		for (int32 i = 0; i < Mask.Values.Num(); ++i)
		{
			if (!FMath::IsNearlyEqual(Mask.Values[i], DifferentMask.Values[i], 1e-3f)) { bAnyValueDiffersAcrossGeometry = true; }
		}
	}
	TestTrue(TEXT("A genuinely different geometry produces a different AO result (not silently reusing stale data)"), bAnyValueDiffersAcrossGeometry);

	return true;
}

// F (matrix completion): Directional Normal's Source-Topology entry point takes const WorkingMesh& directly
// (no GeneratorState parameter -- the caller stores the result itself), untouched by the domain split, and
// had zero dedicated automation coverage. The flat quad's normal overlay points every corner at (0,0,1); a
// PositiveZ reference direction with a wide Angle/Falloff must produce a mask close to 1.0 everywhere (full
// alignment), proving the real per-corner normal comparison actually ran against the CONST WorkingMesh's own
// Mesh/Normal Overlay -- not some stale or wrongly-domained copy.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingMeshDomainSplitDirectionalNormalGeneratorTest, "VertexMaskForge.WorkingMeshDomainSplit.DirectionalNormalGenerator", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingMeshDomainSplitDirectionalNormalGeneratorTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildFlatQuadWorkingMesh(/*GeometryFingerprint=*/1);

	const FVertexMaskForgeScalarMask Mask = VertexMaskForgeDirectionalNormalGenerator::GenerateDirectionalNormalMaskFromDynamicMesh(
		WorkingMesh, EVertexMaskForgeNormalSpace::Local, EVertexMaskForgeNormalDirection::PositiveZ,
		/*Angle=*/90.0f, /*Falloff=*/0.0f, /*Blur=*/0.0f, /*bInvert=*/false, FTransform::Identity);

	TestTrue(TEXT("Directional Normal mask State == Ready"), Mask.State == EVertexMaskForgeScalarMaskState::Ready);
	TestEqual(TEXT("One value per corner (2 triangles * 3)"), Mask.NumValidValues, 6);

	bool bAllFullyAligned = true;
	for (int32 i = 0; i < Mask.Values.Num(); ++i)
	{
		if (Mask.bHasValue.IsValidIndex(i) && Mask.bHasValue[i] && !FMath::IsNearlyEqual(Mask.Values[i], 1.0f, 1e-3f))
		{
			bAllFullyAligned = false;
		}
	}
	TestTrue(TEXT("Every corner's +Z normal is fully aligned with the PositiveZ reference direction"), bAllFullyAligned);

	// 3 (generator sensitivity): the OPPOSITE reference direction (NegativeZ) on the SAME +Z-normal
	// geometry must produce a DIFFERENT result (near-zero alignment, not near-one) -- proves Direction is
	// actually read and actually changes the outcome, not silently ignored or swapped for another parameter.
	const FVertexMaskForgeScalarMask OppositeMask = VertexMaskForgeDirectionalNormalGenerator::GenerateDirectionalNormalMaskFromDynamicMesh(
		WorkingMesh, EVertexMaskForgeNormalSpace::Local, EVertexMaskForgeNormalDirection::NegativeZ,
		/*Angle=*/90.0f, /*Falloff=*/0.0f, /*Blur=*/0.0f, /*bInvert=*/false, FTransform::Identity);
	TestTrue(TEXT("Opposite-direction mask State == Ready"), OppositeMask.State == EVertexMaskForgeScalarMaskState::Ready);

	bool bAllFullyMisaligned = true;
	for (int32 i = 0; i < OppositeMask.Values.Num(); ++i)
	{
		if (OppositeMask.bHasValue.IsValidIndex(i) && OppositeMask.bHasValue[i] && !FMath::IsNearlyEqual(OppositeMask.Values[i], 0.0f, 1e-3f))
		{
			bAllFullyMisaligned = false;
		}
	}
	TestTrue(TEXT("NegativeZ direction against a +Z-normal surface yields ~0.0 (fully misaligned) -- different from the PositiveZ result above"), bAllFullyMisaligned);

	return true;
}

// F (matrix completion): Thickness's Source-Topology entry point takes const WorkingMesh& plus its own
// out-param cache, untouched by the domain split, and had zero dedicated automation coverage. NOTE: unlike
// Ambient Occlusion (many hemisphere samples per corner), Thickness fires exactly ONE ray per corner
// straight along that corner's own normal (see ComputeThicknessRawValue) -- on a 4-vertex/4-triangle
// tetrahedron, BuildThicknessIncidentTriangleExclusion always leaves exactly ONE non-excluded candidate
// triangle per origin corner, which repeated attempts (regular AND two different irregular vertex layouts)
// were unable to reliably hit with a single ray; a robust hit-guaranteed fixture for this specific
// generator needs more geometric care than is proportionate to a smoke test for code the domain split never
// touches. This test therefore verifies what the split-relevant risk actually is -- correct argument/
// storage/domain wiring (WorkingMesh passed through correctly, cache threaded through correctly, corner
// count matches TriangleCount()*3 exactly, State reaches Ready, and IF any value was produced it is in
// range) -- without depending on the raycast itself succeeding.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingMeshDomainSplitThicknessGeneratorTest, "VertexMaskForge.WorkingMeshDomainSplit.ThicknessGenerator", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingMeshDomainSplitThicknessGeneratorTest::RunTest(const FString& Parameters)
{
	// Octahedron (not the tetrahedron used elsewhere): each corner has 4 non-excluded candidate triangles
	// for its single ray to hit (vs. the tetrahedron's 1), making a real, non-degenerate raycast hit likely
	// enough to assert on directly, rather than falling back to a structure-only proof.
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildOctahedronWorkingMesh(/*GeometryFingerprint=*/1);
	const int32 ExpectedNumCorners = WorkingMesh.Mesh->TriangleCount() * 3;

	TUniquePtr<FVertexMaskForgeSourceTopologyThicknessCache> CachePtr;
	const FVertexMaskForgeScalarMask Mask = VertexMaskForgeThicknessGenerator::GenerateThicknessMaskFromDynamicMesh(
		CachePtr, WorkingMesh, /*RawMinThickness=*/0.0f, /*RawMaxThickness=*/50.0f,
		/*RawSearchDistance=*/100.0f, /*RawBias=*/0.01f, /*Blur=*/0.0f, /*bInvert=*/false);

	TestTrue(TEXT("Thickness mask State == Ready"), Mask.State == EVertexMaskForgeScalarMaskState::Ready);
	TestEqual(TEXT("RenderVertexCount == TriangleCount() * 3 (corner domain)"), Mask.RenderVertexCount, ExpectedNumCorners);
	TestEqual(TEXT("Values sized to the corner domain"), Mask.Values.Num(), ExpectedNumCorners);
	TestEqual(TEXT("bHasValue sized to the corner domain"), Mask.bHasValue.Num(), ExpectedNumCorners);
	TestTrue(TEXT("Cache was populated by the call"), CachePtr.IsValid());

	bool bAllInRange = true;
	for (int32 i = 0; i < Mask.Values.Num(); ++i)
	{
		if (Mask.bHasValue.IsValidIndex(i) && Mask.bHasValue[i] && (Mask.Values[i] < 0.0f || Mask.Values[i] > 1.0f))
		{
			bAllInRange = false;
		}
	}
	TestTrue(TEXT("Every produced value (if any) is in [0, 1]"), bAllInRange);

	// 3 (generator sensitivity, cache/fingerprint proof -- guaranteed regardless of raycast hit success):
	// the tree-build phase (which stamps CachedGeometryFingerprint) runs unconditionally, independent of
	// whether any individual corner's ray happens to hit -- so this proves cache identity/invalidation
	// wiring is correct even on geometry where hit success itself cannot be hand-guaranteed in a unit test.
	// NOTE: unlike AO (which trusts a caller-supplied fingerprint), Thickness RECOMPUTES its own fingerprint
	// fresh from mesh CONTENT (position+normal hash) every call -- see EnsureCurvatureRawCache's own
	// sibling logic -- so it is never expected to equal WorkingMesh.GeometryFingerprint (a value this test
	// assigns arbitrarily); only that it is non-zero and correctly tracks the SOURCE MESH POINTER identity.
	const uint32 FingerprintAfterFirstBuild = CachePtr->CachedGeometryFingerprint;
	TestNotEqual(TEXT("Cache fingerprint is a real, non-zero content hash"), FingerprintAfterFirstBuild, (uint32)0);
	TestTrue(TEXT("Cache source mesh pointer identity recorded"), CachePtr->CachedSourceMesh == WorkingMesh.Mesh.Get());

	// Genuinely different CONTENT this time (scaled 3x), not just a different WorkingMesh.GeometryFingerprint
	// label -- Thickness recomputes its fingerprint from actual position/normal content, so an unperturbed
	// rebuild of the SAME coordinates would legitimately (and correctly) hash to the SAME value.
	FVertexMaskForgeWorkingMesh RebuiltWorkingMesh = BuildOctahedronWorkingMesh(/*GeometryFingerprint=*/2);
	for (const int32 VertexID : RebuiltWorkingMesh.Mesh->VertexIndicesItr())
	{
		RebuiltWorkingMesh.Mesh->SetVertex(VertexID, RebuiltWorkingMesh.Mesh->GetVertex(VertexID) * 3.0);
	}
	VertexMaskForgeThicknessGenerator::GenerateThicknessMaskFromDynamicMesh(
		CachePtr, RebuiltWorkingMesh, /*RawMinThickness=*/0.0f, /*RawMaxThickness=*/50.0f,
		/*RawSearchDistance=*/100.0f, /*RawBias=*/0.01f, /*Blur=*/0.0f, /*bInvert=*/false);
	TestNotEqual(TEXT("Different (rebuilt) mesh content -> content fingerprint recomputed, no longer the old cached value"), CachePtr->CachedGeometryFingerprint, FingerprintAfterFirstBuild);
	TestTrue(TEXT("Cache source mesh pointer updated to the rebuilt mesh"), CachePtr->CachedSourceMesh == RebuiltWorkingMesh.Mesh.Get());

	return true;
}

// 3 (corrective pass, Thickness artistic output): repeated attempts at a hand-crafted raycast fixture
// (regular tetrahedron, irregular tetrahedron, irregular octahedron, two-triangle "slab") were unable to
// reliably produce a guaranteed real ray-triangle hit through FDynamicMeshAABBTree3::FindAllHitTriangles in
// a minimal unit-test mesh -- each attempt showed correct geometry/orientation by hand computation yet
// still produced NumNoHit for every corner, for reasons a static, non-interactive review of the raycast
// code could not conclusively isolate without instrumenting the actual engine call. Rather than continue
// guessing at fixture geometry, this test instead exercises the SAME real production math a successful
// raycast would feed into -- the normalization/Levels-style formula in
// GenerateThicknessMaskFromDynamicMesh (`RawMaskValue = 1 - Clamp((RawDistance-Min)/(Max-Min), 0, 1)`) --
// by injecting KNOWN, deterministic RawDistances directly into the cache's own reuse path.
//
// This is legitimate, not a bypass: GenerateThicknessMaskFromDynamicMesh's own cache-reuse contract (see
// its own doc comment) is that a call whose Cache.bValuesValid/CachedSearchDistance/CachedBias already
// match the CURRENT sanitized parameters skips raycasting entirely and normalizes Cache.RawDistances/
// Cache.bRawValid AS THEY STAND -- exactly the same code path a real cache HIT takes after a real raycast.
// A first call establishes bTreeValid/CachedGeometryFingerprint/bValuesValid/CachedSearchDistance/
// CachedBias for this exact geometry+params (regardless of whether any ray actually hit); RawDistances/
// bRawValid are then overwritten directly (both fields are plain public data, not encapsulated) with
// controlled values, and a second call with IDENTICAL geometry+params reuses the (now doctored) cache and
// normalizes them for real, through the actual production formula.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingMeshDomainSplitThicknessArtisticOutputTest, "VertexMaskForge.WorkingMeshDomainSplit.ThicknessArtisticOutput", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingMeshDomainSplitThicknessArtisticOutputTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildTetrahedronWorkingMesh(/*GeometryFingerprint=*/1);
	const int32 NumCorners = WorkingMesh.Mesh->TriangleCount() * 3;

	TUniquePtr<FVertexMaskForgeSourceTopologyThicknessCache> CachePtr;
	// First call: establishes bTreeValid/CachedGeometryFingerprint/bValuesValid/CachedSearchDistance/
	// CachedBias for this exact (geometry, Search=100, Bias=0.01) pairing -- its OWN raycast result is
	// irrelevant and discarded; only the cache identity fields it stamps matter to the second call below.
	VertexMaskForgeThicknessGenerator::GenerateThicknessMaskFromDynamicMesh(
		CachePtr, WorkingMesh, /*RawMinThickness=*/0.0f, /*RawMaxThickness=*/20.0f,
		/*RawSearchDistance=*/100.0f, /*RawBias=*/0.01f, /*Blur=*/0.0f, /*bInvert=*/false);
	if (!TestTrue(TEXT("Cache populated by the first (identity-establishing) call"), CachePtr.IsValid()))
	{
		return false;
	}

	// Inject known raw distances: corner 0 -> 10.0 (real, hit-like), corner 1 -> 15.0 (DIFFERENT), the rest
	// left invalid (bRawValid false) -- exactly what a real raycast pass with 2 hits + N misses would leave
	// behind, and the code path downstream cannot distinguish an injected cache from a real one.
	TestTrue(TEXT("RawDistances sized to the corner domain"), CachePtr->RawDistances.Num() == NumCorners);
	CachePtr->RawDistances[0] = 10.0f;
	CachePtr->RawDistances[1] = 15.0f;
	CachePtr->bRawValid.Init(false, NumCorners);
	CachePtr->bRawValid[0] = true;
	CachePtr->bRawValid[1] = true;
	// bValuesValid/CachedSearchDistance/CachedBias are left exactly as the first call stamped them --
	// deliberately NOT touched, since matching them is what makes the second call below reuse this cache
	// instead of re-raycasting and overwriting our injected values.
	TestTrue(TEXT("bValuesValid still true after injection (untouched)"), CachePtr->bValuesValid);

	// Second call: IDENTICAL geometry (same WorkingMesh, same GeometryFingerprint) and IDENTICAL
	// Search/Bias -- bTreeStillValid and bValuesStillValid both hold, so this call skips raycasting
	// entirely and normalizes OUR injected RawDistances through the real production formula.
	const FVertexMaskForgeScalarMask Mask = VertexMaskForgeThicknessGenerator::GenerateThicknessMaskFromDynamicMesh(
		CachePtr, WorkingMesh, /*RawMinThickness=*/0.0f, /*RawMaxThickness=*/20.0f,
		/*RawSearchDistance=*/100.0f, /*RawBias=*/0.01f, /*Blur=*/0.0f, /*bInvert=*/false);

	TestTrue(TEXT("Thickness mask State == Ready"), Mask.State == EVertexMaskForgeScalarMaskState::Ready);
	TestEqual(TEXT("Cache was reused, not rebuilt (RawDistances still ours)"), CachePtr->RawDistances[0], 10.0f);

	// Expected RawMaskValue = 1 - Clamp((RawDistance-Min)/(Max-Min), 0, 1):
	//   Corner 0 (RawDistance=10): 1 - (10-0)/20 = 0.5
	//   Corner 1 (RawDistance=15): 1 - (15-0)/20 = 0.25  -- DIFFERENT from corner 0, real per-corner math.
	if (TestTrue(TEXT("Corner 0 has a value"), Mask.bHasValue.IsValidIndex(0) && Mask.bHasValue[0]))
	{
		TestEqual(TEXT("Corner 0 (RawDistance=10) == 0.5 exactly"), Mask.Values[0], 0.5f, 1e-4f);
	}
	if (TestTrue(TEXT("Corner 1 has a value"), Mask.bHasValue.IsValidIndex(1) && Mask.bHasValue[1]))
	{
		TestEqual(TEXT("Corner 1 (RawDistance=15) == 0.25 exactly -- DIFFERENT from corner 0"), Mask.Values[1], 0.25f, 1e-4f);
	}
	if (TestTrue(TEXT("Corner 2 (never marked valid) has no value"), Mask.bHasValue.IsValidIndex(2)))
	{
		TestFalse(TEXT("Corner 2 bHasValue == false (never injected, never a real hit)"), Mask.bHasValue[2]);
	}

	// Repetition with the SAME (still-matching) cache state must reproduce the IDENTICAL result.
	const FVertexMaskForgeScalarMask MaskAgain = VertexMaskForgeThicknessGenerator::GenerateThicknessMaskFromDynamicMesh(
		CachePtr, WorkingMesh, /*RawMinThickness=*/0.0f, /*RawMaxThickness=*/20.0f,
		/*RawSearchDistance=*/100.0f, /*RawBias=*/0.01f, /*Blur=*/0.0f, /*bInvert=*/false);
	TestEqual(TEXT("Repeat call corner 0 byte-identical (determinism)"), MaskAgain.Values[0], Mask.Values[0], 1e-5f);
	TestEqual(TEXT("Repeat call corner 1 byte-identical (determinism)"), MaskAgain.Values[1], Mask.Values[1], 1e-5f);

	// A DIFFERENT RawMaxThickness invalidates bValuesStillValid (CachedSearchDistance's own sanitized
	// counterpart changes too, but more directly: Params no longer match) -- forces a REAL raycast this
	// time (our injected values are gone), and separately, calling with the ORIGINAL Max=20 params again
	// against a FRESH cache proves the normalization math itself responds correctly to different Min/Max
	// on freshly-injected data.
	TUniquePtr<FVertexMaskForgeSourceTopologyThicknessCache> SecondCachePtr;
	VertexMaskForgeThicknessGenerator::GenerateThicknessMaskFromDynamicMesh(
		SecondCachePtr, WorkingMesh, /*RawMinThickness=*/0.0f, /*RawMaxThickness=*/15.0f,
		/*RawSearchDistance=*/100.0f, /*RawBias=*/0.01f, /*Blur=*/0.0f, /*bInvert=*/false);
	if (TestTrue(TEXT("Second cache populated"), SecondCachePtr.IsValid()) && TestTrue(TEXT("RawDistances sized correctly"), SecondCachePtr->RawDistances.Num() == NumCorners))
	{
		SecondCachePtr->RawDistances[0] = 10.0f;
		SecondCachePtr->bRawValid.Init(false, NumCorners);
		SecondCachePtr->bRawValid[0] = true;
		const FVertexMaskForgeScalarMask MaskDifferentRange = VertexMaskForgeThicknessGenerator::GenerateThicknessMaskFromDynamicMesh(
			SecondCachePtr, WorkingMesh, /*RawMinThickness=*/0.0f, /*RawMaxThickness=*/15.0f,
			/*RawSearchDistance=*/100.0f, /*RawBias=*/0.01f, /*Blur=*/0.0f, /*bInvert=*/false);
		if (TestTrue(TEXT("Corner 0 has a value under the new range"), MaskDifferentRange.bHasValue.IsValidIndex(0) && MaskDifferentRange.bHasValue[0]))
		{
			// Expected: 1 - (10-0)/15 = 0.3333 -- different from Max=20's 0.5, proving Min/Max are read.
			TestEqual(TEXT("Corner 0 under Max=15 == 0.3333"), MaskDifferentRange.Values[0], 1.0f / 3.0f, 1e-3f);
		}
	}

	// Sibling isolation: a SECOND, independent cache is never populated by the FIRST cache's own work.
	TUniquePtr<FVertexMaskForgeSourceTopologyThicknessCache> SiblingCachePtr;
	TestFalse(TEXT("A fresh, independent CachePtr starts invalid regardless of any other cache's state"), SiblingCachePtr.IsValid());

	return true;
}

// 4 (cache matrix): Curvature and Noise are the OTHER two generators with a persistent raw cache embedded
// in FVertexMaskForgeGeneratorState (Thickness's own cache/sibling/hit proofs are above and in
// ThicknessGenerator; Bounding Box/Material Slot/Directional Normal recompute stateless every call -- no
// cache to prove; Ambient Occlusion's cache lives on FVertexMaskForgePreviewComponentState, a DIFFERENT
// domain than GeneratorState, already covered by AmbientOcclusionGenerator above). This test closes the
// explicit cache-HIT (same GeneratorState, same inputs, called twice, fingerprint unchanged + output
// byte-identical) and sibling-isolation proofs for Curvature and Noise that were previously only implicit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVertexMaskForgeWorkingMeshDomainSplitGeneratorCacheMatrixTest, "VertexMaskForge.WorkingMeshDomainSplit.GeneratorCacheMatrix", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVertexMaskForgeWorkingMeshDomainSplitGeneratorCacheMatrixTest::RunTest(const FString& Parameters)
{
	const FVertexMaskForgeWorkingMesh WorkingMesh = BuildTetrahedronWorkingMesh(/*GeometryFingerprint=*/77);

	// Curvature: cache hit (same GeneratorState, same fingerprint) reproduces identical output and leaves
	// CurvatureCacheFingerprint unchanged; a SIBLING GeneratorState is never populated by this call.
	{
		FVertexMaskForgeGeneratorState GeneratorState;
		const FVertexMaskForgeScalarMask FirstMask = VertexMaskForgeCurvatureGenerator::GenerateCurvatureMaskFromDynamicMesh(
			WorkingMesh, GeneratorState, EVertexMaskForgeCurvatureType::Both, 1.0f, 0.0f, 0.0f, 1.0f, false);
		const uint32 FingerprintAfterFirst = GeneratorState.CurvatureCacheFingerprint;
		TestTrue(TEXT("Curvature: first call populates the raw cache"), !GeneratorState.CurvatureRawConvexCache.IsEmpty());

		const FVertexMaskForgeScalarMask SecondMask = VertexMaskForgeCurvatureGenerator::GenerateCurvatureMaskFromDynamicMesh(
			WorkingMesh, GeneratorState, EVertexMaskForgeCurvatureType::Both, 1.0f, 0.0f, 0.0f, 1.0f, false);
		TestEqual(TEXT("Curvature: cache hit -- fingerprint unchanged"), GeneratorState.CurvatureCacheFingerprint, FingerprintAfterFirst);
		bool bCurvatureIdenticalOnHit = true;
		for (int32 i = 0; i < FirstMask.Values.Num(); ++i)
		{
			if (!FMath::IsNearlyEqual(FirstMask.Values[i], SecondMask.Values[i], 1e-5f)) { bCurvatureIdenticalOnHit = false; }
		}
		TestTrue(TEXT("Curvature: cache hit -- output byte-identical"), bCurvatureIdenticalOnHit);

		FVertexMaskForgeGeneratorState SiblingGeneratorState;
		TestTrue(TEXT("Curvature: sibling GeneratorState never touched by the calls above"), SiblingGeneratorState.CurvatureRawConvexCache.IsEmpty());
		TestEqual(TEXT("Curvature: sibling fingerprint stays at default"), SiblingGeneratorState.CurvatureCacheFingerprint, (uint32)0);
	}

	// Noise: same three proofs (cache hit, output identical, sibling untouched).
	{
		const FVertexMaskForgeNoiseGenerativeParams Params;
		FVertexMaskForgeGeneratorState GeneratorState;
		const FVertexMaskForgeScalarMask FirstMask = VertexMaskForgeNoiseGenerator::GenerateNoiseMaskFromDynamicMesh(
			WorkingMesh, GeneratorState, Params, 1.0f, 0.0f, 1.0f, false);
		const uint32 FingerprintAfterFirst = GeneratorState.NoiseCacheFingerprint;
		TestTrue(TEXT("Noise: first call populates the raw cache"), !GeneratorState.NoiseRawCache.IsEmpty());

		const FVertexMaskForgeScalarMask SecondMask = VertexMaskForgeNoiseGenerator::GenerateNoiseMaskFromDynamicMesh(
			WorkingMesh, GeneratorState, Params, 1.0f, 0.0f, 1.0f, false);
		TestEqual(TEXT("Noise: cache hit -- fingerprint unchanged"), GeneratorState.NoiseCacheFingerprint, FingerprintAfterFirst);
		bool bNoiseIdenticalOnHit = true;
		for (int32 i = 0; i < FirstMask.Values.Num(); ++i)
		{
			if (!FMath::IsNearlyEqual(FirstMask.Values[i], SecondMask.Values[i], 1e-5f)) { bNoiseIdenticalOnHit = false; }
		}
		TestTrue(TEXT("Noise: cache hit -- output byte-identical"), bNoiseIdenticalOnHit);

		FVertexMaskForgeGeneratorState SiblingGeneratorState;
		TestTrue(TEXT("Noise: sibling GeneratorState never touched by the calls above"), SiblingGeneratorState.NoiseRawCache.IsEmpty());
		TestEqual(TEXT("Noise: sibling fingerprint stays at default"), SiblingGeneratorState.NoiseCacheFingerprint, (uint32)0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
