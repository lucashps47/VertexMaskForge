#include "VertexMaskForgeCurvatureGenerator.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "MeshDescription.h"
#include "StaticMeshResources.h"
#include "VectorUtil.h"
#include "VertexMaskForgeGeneratorUtils.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

namespace
{
	// --- Curvature: topology-based signed curvature analysis + artistic post-processing ------------

	/**
	 * AUDITED (Curvature CLASSIFICATION FIX -- root cause): finds which of TriangleID's own three
	 * winding-ordered edges (Mesh.GetTriEdges: slot 0 = Tri[0]->Tri[1], slot 1 = Tri[1]->Tri[2], slot 2
	 * = Tri[2]->Tri[0] -- confirmed against FDynamicMesh3::AddTriangleInternal/AddTriangleEdge in the
	 * engine source, not assumed) matches EdgeID, and returns that edge's two vertex IDs in THAT
	 * triangle's own winding order (Tri[i] -> Tri[(i+1)%3]).
	 *
	 * This is the fix for the actual root cause of the Convex/Concave misclassification bug:
	 * FDynamicMesh3::GetEdgeV() returns FEdge::Vert, which FDynamicMesh3::ReplaceEdgeVertex() stores as
	 * FMath::Min(a,b)/FMath::Max(a,b) -- i.e. MIN/MAX-VERTEX-ID-SORTED, not oriented by any triangle's
	 * winding (the engine's own doc comment on GetEdgeV even implies this, contrasting it with the
	 * separate GetOrientedBoundaryEdgeV() -- "oriented based on attached triangle (rather than
	 * min-sorted)", which unfortunately only covers BOUNDARY edges, not the interior edges Curvature
	 * needs). The previous implementation used GetEdgeV()'s raw, non-oriented order directly as
	 * EdgeDir, with NO relationship to which triangle was reported as EdgeT.A/EdgeT.B (also an
	 * unspecified internal storage order) -- so for two geometrically-identical folds (e.g. two convex
	 * bevel edges on the same mesh), the computed dihedral sign could come out differently, purely
	 * depending on unrelated internal ID/insertion-order accidents, not on the actual geometry. This
	 * exactly explains the reported symptom: Convex worked on SOME bevels and not others, Concave was
	 * broken, and Both (abs of an already-inconsistently-signed value) showed extra/wrong detail.
	 *
	 * Confirmed against the engine's own established pattern for this exact problem:
	 * GeometryCore/Private/Tessellation/Regularization.cpp (edge-flip decision code) explicitly
	 * re-derives edge direction from Mesh.GetTriangle(Edge.Tri[0]) and the edge's position within that
	 * triangle's winding, rather than trusting Edge.Vert's raw order -- the same technique used here.
	 */
	UE::Geometry::FIndex2i GetTriangleOrientedEdgeVertices(const UE::Geometry::FDynamicMesh3& Mesh, const int32 TriangleID, const int32 EdgeID)
	{
		using namespace UE::Geometry;
		const FIndex3i TriVerts = Mesh.GetTriangle(TriangleID);
		const FIndex3i TriEdges = Mesh.GetTriEdges(TriangleID);
		if (TriEdges.A == EdgeID)
		{
			return FIndex2i(TriVerts.A, TriVerts.B);
		}
		if (TriEdges.B == EdgeID)
		{
			return FIndex2i(TriVerts.B, TriVerts.C);
		}
		// TriEdges.C == EdgeID, by construction (this function is only ever called with an EdgeID
		// already confirmed to belong to TriangleID via Mesh.GetEdgeT).
		return FIndex2i(TriVerts.C, TriVerts.A);
	}

	/** Per-vertex raw curvature magnitudes -- see ComputeRawCurvatureMagnitudes' own doc comment. Both
	 *  arrays are non-negative, sized Mesh.MaxVertexID(), and share the SAME normalization scale (see
	 *  that function for why a single, combined scale is used rather than two independent ones). */
	struct FVertexMaskForgeCurvatureRawResult
	{
		TArray<float> ConvexMagnitude;
		TArray<float> ConcaveMagnitude;
	};

	/**
	 * AUDITED (Curvature CLASSIFICATION FIX): the expensive, GEOMETRY-ONLY analysis half of Curvature
	 * generation -- computes discrete Convex/Concave curvature MAGNITUDES per Dynamic Mesh Vertex ID,
	 * using ONLY the mesh's own topological adjacency (edges/triangles), never spatial proximity, never
	 * a component transform, never Preview Mode/material state.
	 *
	 * ROOT-CAUSE FIX (per the explicit diagnostic requirement -- see GetTriangleOrientedEdgeVertices'
	 * own doc comment for the orientation half of the fix, and the "no signed cancellation" note below
	 * for the other half):
	 *   1. ORIENTATION: EdgeDir is now derived from T0's (EdgeT.A's) own triangle winding
	 *      (GetTriangleOrientedEdgeVertices), NEVER from GetEdgeV()'s raw min/max-sorted order. This
	 *      guarantees the standard invariance property (verified both empirically -- see the temporary
	 *      automation test run for this fix -- and algebraically): swapping which triangle is used as
	 *      reference (T0<->T1, i.e. N0<->N1) walks the shared edge in the OPPOSITE winding direction on
	 *      a consistently-wound (orientable) manifold mesh, which flips EdgeDir too, leaving the
	 *      resulting signed angle IDENTICAL either way -- cross(N1,N0)=-cross(N0,N1) combined with
	 *      EdgeDir->-EdgeDir cancels the two sign flips. Uses the engine's own audited
	 *      VectorUtil::OrientedDihedralAngle(N0, N1, EdgeDir) -- confirmed via source read to compute
	 *      exactly atan2(Edge.(N0^N1), N0.N1) -- rather than reimplementing atan2 by hand.
	 *   2. NO SIGNED CANCELLATION AT THE VERTEX (the "Both shows extra/wrong detail" root cause): each
	 *      edge's signed contribution is classified as Convex or Concave THIS EDGE, individually, BEFORE
	 *      being added to its two endpoints -- accumulated into TWO SEPARATE per-vertex sums
	 *      (ConvexSum/ConcaveSum), never into one signed scalar that a later abs()/max(x,0) would have
	 *      to un-mix. A vertex on a bevel that has both a slightly-convex and a slightly-concave
	 *      incident edge (e.g. an irregular triangulation) now correctly shows SOME of both, rather than
	 *      the two silently netting out to near-zero (or worse, flipping sign) before Convex/Concave
	 *      ever get to see them.
	 *
	 * Per-edge contribution = OrientedDihedralAngle * EdgeLength (edge-length weighting, unchanged from
	 * before -- a standard, legitimate discrete weighting; see this function's own doc note on why
	 * triangle-COUNT alone is not the driver of the result). Each of ConvexSum/ConcaveSum is then
	 * divided by max(VertexAreaSum, Epsilon) (1/3 the summed area of the vertex's own incident
	 * triangles, the standard mixed-Voronoi-area approximation, computed in a single separate pass over
	 * TriangleIndicesItr()) -- this normalizes out local triangle DENSITY, so a denser tessellation of
	 * the same physical surface produces a comparable value rather than one that scales with triangle
	 * count.
	 *
	 * BOUNDARY EDGES: an edge with only ONE adjacent triangle (Mesh.IsBoundaryEdge) contributes NOTHING
	 * to either endpoint -- there is no second face to form a dihedral angle with, and inventing one
	 * would fabricate a false halo along open boundaries.
	 *
	 * DEGENERATE TRIANGLES: a triangle whose GetTriNormal() result fails to reach unit length
	 * (SizeSquared() below a small epsilon -- zero-area/degenerate) is treated as absent for BOTH the
	 * dihedral term of any edge bordering it AND its own area contribution; this can never produce
	 * NaN/Inf, since an invalid normal is detected and skipped before it enters any dot/cross product.
	 *
	 * NON-MANIFOLD EDGES / MULTIPLE ISLANDS: FMeshDescriptionToDynamicMesh::Convert already resolves
	 * non-manifold edges by splitting them into separate manifold vertices during conversion, so
	 * FDynamicMesh3's own edge API (GetEdgeT, at most 2 triangles) already reflects manifold-safe
	 * topology by the time this function runs. Multiple disconnected islands never interact: every
	 * accumulation here is strictly local (a vertex's own incident edges/triangles only) -- no
	 * spatial/global step, no position-based matching anywhere in this function.
	 *
	 * ISOLATED VERTICES: a vertex with zero incident triangles gets VertexAreaSum 0, guarded by the
	 * Epsilon-clamped denominator -- curvature 0 in both arrays, never a division by zero.
	 *
	 * GLOBAL ROBUST NORMALIZATION (explicit requirement -- avoid a single outlier dominating the whole
	 * mesh's contrast): both ConvexSum and ConcaveSum carry units of 1/length and scale with the asset's
	 * own physical size, so they are normalized here by dividing by the 90th PERCENTILE of the COMBINED,
	 * UNION distribution of non-zero |ConvexSum| and |ConcaveSum| magnitudes across the whole mesh
	 * (chosen over the maximum so a single spiky vertex cannot compress the mesh's contrast toward
	 * zero), then clamped to [0, 1]. AUDITED (explicit "document the choice" requirement): a SINGLE,
	 * SHARED scale is used for both arrays -- deliberately NOT two independent percentiles -- so that a
	 * mesh with (for example) much stronger convex bevels than concave grooves still shows the concave
	 * detail at its true RELATIVE intensity rather than being independently re-stretched to look equally
	 * strong; this also means enabling/disabling Concave detail can never rescale Convex's own values or
	 * vice versa, since the shared scale is computed once from BOTH arrays together, not recomputed per
	 * Curvature Type selection (Curvature Type is purely a downstream selector -- see
	 * ApplyCurvatureArtisticParams -- and never touches this cache). With fewer than 8 combined non-zero
	 * samples (a near-flat or tiny mesh), the scale defaults to 1.0 (no amplification).
	 */
	FVertexMaskForgeCurvatureRawResult ComputeRawCurvatureMagnitudes(const UE::Geometry::FDynamicMesh3& Mesh)
	{
		using namespace UE::Geometry;

		const int32 MaxVID = Mesh.MaxVertexID();
		TArray<float> ConvexSum, ConcaveSum, AreaSum;
		ConvexSum.SetNumZeroed(MaxVID);
		ConcaveSum.SetNumZeroed(MaxVID);
		AreaSum.SetNumZeroed(MaxVID);

		constexpr double NormalEpsilonSq = 1e-12;
		constexpr double AngleEpsilon = 1e-6; // radians; below this, treat the fold as flat (neither sign).

		// Pass 1: per-vertex mixed-area accumulation, one pass over triangles.
		for (const int32 TriangleID : Mesh.TriangleIndicesItr())
		{
			const double Area = Mesh.GetTriArea(TriangleID);
			if (!FMath::IsFinite(Area) || Area <= 0.0)
			{
				continue;
			}
			const FIndex3i Tri = Mesh.GetTriangle(TriangleID);
			const float AreaThird = static_cast<float>(Area / 3.0);
			AreaSum[Tri.A] += AreaThird;
			AreaSum[Tri.B] += AreaThird;
			AreaSum[Tri.C] += AreaThird;
		}

		// Pass 2: per-edge ORIENTED signed dihedral angle, classified Convex/Concave BEFORE
		// accumulation -- see this function's own doc comment for both halves of the fix.
		for (const int32 EdgeID : Mesh.EdgeIndicesItr())
		{
			if (Mesh.IsBoundaryEdge(EdgeID))
			{
				continue;
			}
			const FIndex2i EdgeT = Mesh.GetEdgeT(EdgeID);
			if (EdgeT.A == IndexConstants::InvalidID || EdgeT.B == IndexConstants::InvalidID)
			{
				continue;
			}

			const FVector3d N0 = Mesh.GetTriNormal(EdgeT.A);
			const FVector3d N1 = Mesh.GetTriNormal(EdgeT.B);
			if (N0.SquaredLength() < NormalEpsilonSq || N1.SquaredLength() < NormalEpsilonSq)
			{
				// One (or both) adjacent triangle is degenerate -- no valid dihedral term.
				continue;
			}

			// AUDITED (root-cause fix): oriented by T0's (EdgeT.A's) own winding -- never GetEdgeV()'s
			// raw min/max-sorted order. See GetTriangleOrientedEdgeVertices' own doc comment.
			const FIndex2i OrientedEdgeV = GetTriangleOrientedEdgeVertices(Mesh, EdgeT.A, EdgeID);
			FVector3d EdgeDir = Mesh.GetVertex(OrientedEdgeV.B) - Mesh.GetVertex(OrientedEdgeV.A);
			const double EdgeLength = EdgeDir.Length();
			if (!FMath::IsFinite(EdgeLength) || EdgeLength < UE_DOUBLE_KINDA_SMALL_NUMBER)
			{
				continue;
			}
			EdgeDir /= EdgeLength;

			const double SignedDihedral = VectorUtil::OrientedDihedralAngle(N0, N1, EdgeDir);
			if (!FMath::IsFinite(SignedDihedral))
			{
				continue;
			}

			const double SignedContribution = SignedDihedral * EdgeLength;
			if (SignedContribution > AngleEpsilon)
			{
				const float Value = static_cast<float>(SignedContribution);
				ConvexSum[OrientedEdgeV.A] += Value;
				ConvexSum[OrientedEdgeV.B] += Value;
			}
			else if (SignedContribution < -AngleEpsilon)
			{
				const float Value = static_cast<float>(-SignedContribution);
				ConcaveSum[OrientedEdgeV.A] += Value;
				ConcaveSum[OrientedEdgeV.B] += Value;
			}
			// else: within epsilon of flat -- contributes to neither.
		}

		constexpr float AreaEpsilon = 1e-8f;
		FVertexMaskForgeCurvatureRawResult Result;
		Result.ConvexMagnitude.SetNumZeroed(MaxVID);
		Result.ConcaveMagnitude.SetNumZeroed(MaxVID);
		TArray<float> CombinedNonZeroMagnitudes;
		CombinedNonZeroMagnitudes.Reserve(MaxVID * 2);

		for (const int32 VertexID : Mesh.VertexIndicesItr())
		{
			const float SafeArea = FMath::Max(AreaSum[VertexID], AreaEpsilon);
			const float ConvexValue = ConvexSum[VertexID] / SafeArea;
			const float ConcaveValue = ConcaveSum[VertexID] / SafeArea;
			const float SafeConvex = FMath::IsFinite(ConvexValue) ? ConvexValue : 0.0f;
			const float SafeConcave = FMath::IsFinite(ConcaveValue) ? ConcaveValue : 0.0f;
			Result.ConvexMagnitude[VertexID] = SafeConvex;
			Result.ConcaveMagnitude[VertexID] = SafeConcave;
			if (!FMath::IsNearlyZero(SafeConvex))
			{
				CombinedNonZeroMagnitudes.Add(SafeConvex);
			}
			if (!FMath::IsNearlyZero(SafeConcave))
			{
				CombinedNonZeroMagnitudes.Add(SafeConcave);
			}
		}

		float NormalizationScale = 1.0f;
		if (CombinedNonZeroMagnitudes.Num() >= 8)
		{
			CombinedNonZeroMagnitudes.Sort();
			const int32 PercentileIndex = FMath::Clamp(
				FMath::RoundToInt(0.90f * static_cast<float>(CombinedNonZeroMagnitudes.Num() - 1)),
				0, CombinedNonZeroMagnitudes.Num() - 1);
			const float PercentileValue = CombinedNonZeroMagnitudes[PercentileIndex];
			if (PercentileValue > UE_KINDA_SMALL_NUMBER)
			{
				NormalizationScale = PercentileValue;
			}
		}

		for (const int32 VertexID : Mesh.VertexIndicesItr())
		{
			Result.ConvexMagnitude[VertexID] = FMath::Clamp(Result.ConvexMagnitude[VertexID] / NormalizationScale, 0.0f, 1.0f);
			Result.ConcaveMagnitude[VertexID] = FMath::Clamp(Result.ConcaveMagnitude[VertexID] / NormalizationScale, 0.0f, 1.0f);
		}

		return Result;
	}

	/**
	 * AUDITED (Curvature layer): builds the render-vertex correspondence used ONLY for a non-Source-
	 * Topology (non-Nanite) entry -- maps each LOD0 render vertex index to the DYNAMIC MESH VERTEX ID
	 * (ComputeRawCurvatureMagnitudes' own domain) it corresponds to, so that a UV seam's several split
	 * render vertices all read the SAME curvature value (the explicit "propagate to vertex instances"
	 * requirement), rather than each being treated as a topologically isolated point.
	 *
	 * Derived from the SAME TriIDMap + WedgeMap correspondence already audited and relied upon by
	 * ReconstructOmittedColorOverlay/WriteAcceptTargets -- never position-matching. For each Dynamic
	 * Mesh triangle: SourceTriangleID = TriIDMap[TriangleID]; its 3 source VertexInstanceIDs give, via
	 * the SAME ordinal-position WedgeMap lookup WriteAcceptTargets uses (VertexInstances().GetElementIDs()
	 * enumerated in order, LOD0.WedgeMap[ordinal] = render vertex index), the 3 render vertex indices
	 * that correspond -- in the SAME corner order -- to the Dynamic Mesh triangle's own 3 VertexIDs
	 * (Mesh.GetTriangle(TriangleID)). A render vertex that never receives an assignment (WedgeMap
	 * INDEX_NONE, or genuinely absent from the wedge) is left at INDEX_NONE; callers must check for it.
	 */
	TArray<int32> ComputeCurvatureRenderVertexCorrespondence(
		const UE::Geometry::FDynamicMesh3& Mesh,
		const TArray<FTriangleID>& TriIDMap,
		const FMeshDescription& MeshDescription,
		const FStaticMeshLODResources& LOD0)
	{
		using namespace UE::Geometry;

		const int32 NumRenderVerts = static_cast<int32>(LOD0.VertexBuffers.PositionVertexBuffer.GetNumVertices());
		TArray<int32> Correspondence;
		Correspondence.Init(INDEX_NONE, NumRenderVerts);

		if (LOD0.WedgeMap.Num() != MeshDescription.VertexInstances().Num())
		{
			// Same "never approximate" discipline as WriteAcceptTargets -- a stale/mismatched WedgeMap
			// yields an all-INDEX_NONE correspondence rather than a guessed one; callers treat every
			// render vertex as unmapped (falls back to 0.0f via TryGetValue's own bHasValue gate).
			return Correspondence;
		}

		TMap<int32, int32> VertexInstanceToRenderIndex;
		VertexInstanceToRenderIndex.Reserve(MeshDescription.VertexInstances().Num());
		int32 Ordinal = 0;
		for (const FVertexInstanceID InstanceID : MeshDescription.VertexInstances().GetElementIDs())
		{
			const int32 RenderIndex = LOD0.WedgeMap[Ordinal];
			if (RenderIndex != INDEX_NONE)
			{
				VertexInstanceToRenderIndex.Add(InstanceID.GetValue(), RenderIndex);
			}
			++Ordinal;
		}

		for (const int32 TriangleID : Mesh.TriangleIndicesItr())
		{
			if (!TriIDMap.IsValidIndex(TriangleID))
			{
				continue;
			}
			const FTriangleID SourceTriangleID = TriIDMap[TriangleID];
			if (!MeshDescription.IsTriangleValid(SourceTriangleID))
			{
				continue;
			}
			const TArrayView<const FVertexInstanceID> SourceInstances = MeshDescription.GetTriangleVertexInstances(SourceTriangleID);
			if (SourceInstances.Num() != 3)
			{
				continue;
			}
			const FIndex3i DynamicTri = Mesh.GetTriangle(TriangleID);
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				if (const int32* RenderIndex = VertexInstanceToRenderIndex.Find(SourceInstances[Corner].GetValue()))
				{
					if (Correspondence.IsValidIndex(*RenderIndex))
					{
						Correspondence[*RenderIndex] = DynamicTri[Corner];
					}
				}
			}
		}

		return Correspondence;
	}

	/**
	 * AUDITED (Curvature CLASSIFICATION FIX): guarantees WorkingMesh.CurvatureRawConvexCache/
	 * CurvatureRawConcaveCache/CurvatureRenderVertexToDynamicMeshVertex are valid for WorkingMesh.Mesh's
	 * CURRENT geometry -- the ONE place the expensive analysis (ComputeRawCurvatureMagnitudes) and the
	 * render-vertex correspondence (ComputeCurvatureRenderVertexCorrespondence) are ever invoked. Reuses
	 * both verbatim whenever CurvatureCacheFingerprint already matches WorkingMesh.GeometryFingerprint
	 * (see that field's own doc comment) -- Curvature Type/Multiplier/Blur/Levels/Invert/Opacity/Blend
	 * Mode changes never reach this function at all (see OnCurvatureParamChanged, which calls the cheap
	 * ApplyCurvatureArtisticParams path directly), so this only ever runs again after a genuine geometry
	 * change (RefreshSelection building a new WorkingMesh, whose fingerprint differs by construction).
	 * MeshDescription/LOD0 are optional -- omitted (nullptr) for a Source-Topology entry, which has no
	 * use for the render-vertex correspondence at all.
	 */
	void EnsureCurvatureRawCache(
		FVertexMaskForgeWorkingMesh& WorkingMesh,
		const FMeshDescription* MeshDescriptionForCorrespondence,
		const FStaticMeshLODResources* LOD0ForCorrespondence)
	{
		if (!WorkingMesh.Mesh.IsValid())
		{
			return;
		}
		if (WorkingMesh.CurvatureCacheFingerprint == WorkingMesh.GeometryFingerprint
			&& !WorkingMesh.CurvatureRawConvexCache.IsEmpty())
		{
			return;
		}

		FVertexMaskForgeCurvatureRawResult RawResult = ComputeRawCurvatureMagnitudes(*WorkingMesh.Mesh);
		WorkingMesh.CurvatureRawConvexCache = MoveTemp(RawResult.ConvexMagnitude);
		WorkingMesh.CurvatureRawConcaveCache = MoveTemp(RawResult.ConcaveMagnitude);

		if (MeshDescriptionForCorrespondence && LOD0ForCorrespondence)
		{
			WorkingMesh.CurvatureRenderVertexToDynamicMeshVertex = ComputeCurvatureRenderVertexCorrespondence(
				*WorkingMesh.Mesh, WorkingMesh.TriIDMap, *MeshDescriptionForCorrespondence, *LOD0ForCorrespondence);
		}
		else
		{
			WorkingMesh.CurvatureRenderVertexToDynamicMeshVertex.Reset();
		}

		WorkingMesh.CurvatureCacheFingerprint = WorkingMesh.GeometryFingerprint;

		UE_LOG(LogVertexMaskForge, Log,
			TEXT("Vertex Mask Forge: Curvature raw analysis computed (%d vertices)."),
			WorkingMesh.CurvatureRawConvexCache.Num());
	}

	/**
	 * AUDITED (Curvature layer): topological blur -- diffuses Input (already in [0, 1], Dynamic Mesh
	 * Vertex domain) over the mesh's own one-ring vertex adjacency (Mesh.VtxVerticesItr), NEVER by
	 * spatial distance. Double-buffered per iteration (reads only from the PREVIOUS iteration's buffer,
	 * writes only to the next -- never mutates in place while still being read, so results never depend
	 * on vertex enumeration order). Each iteration: NewValue[v] = mean of {Value[v]} UNION {Value[n] for
	 * n in v's one-ring neighbors} -- an unweighted average that includes the vertex's own current value,
	 * which is what keeps a boundary/low-valence vertex (fewer neighbors) from darkening artificially:
	 * it is never diluted by a fabricated zero/missing neighbor, only ever averaged with values that
	 * genuinely exist. A vertex with zero neighbors (isolated) is simply left unchanged.
	 *
	 * BlurAmount's integer part is the number of FULL iterations; the fractional part lerps the last
	 * pre-final-iteration buffer toward one more iteration (BlurAmount 2.5 = 2 full iterations, then
	 * lerp 50% toward a 3rd) -- matches the exact "0.0 sem blur; 1.0 uma iteração; 2.5 duas iterações
	 * completas e 50% da terceira" contract. BlurAmount <= 0 returns Input verbatim (copied), no
	 * allocation churn beyond the one copy. Every value stays in [0, 1] throughout: an average of
	 * numbers already in [0, 1] cannot leave that range.
	 */
	TArray<float> ApplyTopologicalCurvatureBlur(
		const UE::Geometry::FDynamicMesh3& Mesh,
		const TArray<float>& Input,
		const float BlurAmount)
	{
		if (BlurAmount <= 0.0f || Input.IsEmpty())
		{
			return Input;
		}

		const int32 FullIterations = FMath::FloorToInt32(BlurAmount);
		const float FractionalIteration = BlurAmount - static_cast<float>(FullIterations);

		auto RunOneIteration = [&Mesh](const TArray<float>& Src) -> TArray<float>
		{
			TArray<float> Dst = Src;
			for (const int32 VertexID : Mesh.VertexIndicesItr())
			{
				float Sum = Src[VertexID];
				int32 Count = 1;
				for (const int32 NeighborID : Mesh.VtxVerticesItr(VertexID))
				{
					if (Src.IsValidIndex(NeighborID))
					{
						Sum += Src[NeighborID];
						++Count;
					}
				}
				Dst[VertexID] = Sum / static_cast<float>(Count);
			}
			return Dst;
		};

		TArray<float> Current = Input;
		for (int32 Iter = 0; Iter < FullIterations; ++Iter)
		{
			Current = RunOneIteration(Current);
		}

		if (FractionalIteration > UE_KINDA_SMALL_NUMBER)
		{
			TArray<float> OneMore = RunOneIteration(Current);
			for (int32 i = 0; i < Current.Num(); ++i)
			{
				Current[i] = FMath::Lerp(Current[i], OneMore[i], FractionalIteration);
			}
		}

		return Current;
	}

	/**
	 * AUDITED (Curvature CLASSIFICATION FIX): the CHEAP, purely-downstream half of Curvature generation
	 * -- turns the cached, normalized Convex/Concave magnitude arrays (RawConvex/RawConcave, Dynamic
	 * Mesh Vertex domain, both already in [0, 1] -- see ComputeRawCurvatureMagnitudes) into the final
	 * [0, 1] mask value for the SAME domain, via the exact pipeline order specified: Curvature Type ->
	 * Multiplier -> Blur -> Levels -> Invert. Never touches mesh topology/adjacency/normals -- Type is a
	 * per-element SELECTION (no arithmetic that could re-mix Convex and Concave), Multiplier a
	 * per-element scale+clamp, Blur a topological diffusion over the ALREADY-Type/Multiplier-processed
	 * array (reusing Mesh only for its adjacency, not recomputing anything geometric), Levels a
	 * per-element remap, Invert a final per-element `1 - x`. Called by BOTH GenerateCurvatureMask
	 * (render-vertex domain caller then re-indexes this Dynamic-Mesh-Vertex-domain result via the
	 * correspondence cache) and GenerateCurvatureMaskFromDynamicMesh (uses this result directly).
	 *
	 * AUDITED (root-cause fix, "no cancellation" half): Type no longer computes max(Signed,0)/
	 * max(-Signed,0)/abs(Signed) on a single already-summed signed value (the source of the reported
	 * cancellation bug) -- it now SELECTS between the two independently-accumulated, never-mixed
	 * RawConvex/RawConcave arrays. Both = max(RawConvex[i], RawConcave[i]) -- a union that can never
	 * exceed 1 (both inputs are already clamped to [0,1] by the shared normalization) and never lets one
	 * side's magnitude subtract from or cancel the other's, matching the explicit "Both deve ser
	 * exatamente a união visual dos dois" requirement.
	 */
	TArray<float> ApplyCurvatureArtisticParams(
		const UE::Geometry::FDynamicMesh3& Mesh,
		const TArray<float>& RawConvex,
		const TArray<float>& RawConcave,
		const EVertexMaskForgeCurvatureType Type,
		const float Multiplier,
		const float Blur,
		const float LevelsMin,
		const float LevelsMax,
		const bool bInvert)
	{
		const float ClampedMultiplier = FMath::Max(Multiplier, 0.0f);
		const float ClampedBlur = FMath::Clamp(Blur, 0.0f, 10.0f);
		const float ClampedLevelsMin = FMath::Clamp(LevelsMin, 0.0f, 1.0f);
		const float ClampedLevelsMax = FMath::Clamp(LevelsMax, 0.0f, 1.0f);

		TArray<float> TypeAndMultiplied;
		TypeAndMultiplied.SetNumUninitialized(RawConvex.Num());
		for (int32 i = 0; i < RawConvex.Num(); ++i)
		{
			float Magnitude;
			switch (Type)
			{
			case EVertexMaskForgeCurvatureType::Convex:
				Magnitude = RawConvex[i];
				break;
			case EVertexMaskForgeCurvatureType::Concave:
				Magnitude = RawConcave[i];
				break;
			case EVertexMaskForgeCurvatureType::Both:
			default:
				Magnitude = FMath::Max(RawConvex[i], RawConcave[i]);
				break;
			}
			TypeAndMultiplied[i] = FMath::Clamp(Magnitude * ClampedMultiplier, 0.0f, 1.0f);
		}

		TArray<float> Blurred = ApplyTopologicalCurvatureBlur(Mesh, TypeAndMultiplied, ClampedBlur);

		TArray<float> Result;
		Result.SetNumUninitialized(Blurred.Num());
		for (int32 i = 0; i < Blurred.Num(); ++i)
		{
			const float Leveled = VertexMaskForgeGeneratorUtils::ApplyCurvatureLevels(Blurred[i], ClampedLevelsMin, ClampedLevelsMax);
			Result[i] = bInvert ? (1.0f - Leveled) : Leveled;
		}
		return Result;
	}
}

namespace VertexMaskForgeCurvatureGenerator
{
	FVertexMaskForgeScalarMask GenerateCurvatureMask(
		FVertexMaskForgeWorkingMesh& WorkingMesh,
		const FMeshDescription* MeshDescription,
		const FStaticMeshLODResources& LOD0,
		const EVertexMaskForgeCurvatureType Type,
		const float Multiplier,
		const float Blur,
		const float LevelsMin,
		const float LevelsMax,
		const bool bInvert)
	{
		FVertexMaskForgeScalarMask Mask;
		Mask.Source = EVertexMaskForgeScalarMaskSource::Curvature;

		const int32 NumRenderVerts = static_cast<int32>(LOD0.VertexBuffers.PositionVertexBuffer.GetNumVertices());
		Mask.RenderVertexCount = NumRenderVerts;

		if (!WorkingMesh.Mesh.IsValid() || NumRenderVerts <= 0 || !MeshDescription)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}

		EnsureCurvatureRawCache(WorkingMesh, MeshDescription, &LOD0);
		if (WorkingMesh.CurvatureRawConvexCache.IsEmpty() || WorkingMesh.CurvatureRenderVertexToDynamicMeshVertex.Num() != NumRenderVerts)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}

		const TArray<float> DynamicMeshValues = ApplyCurvatureArtisticParams(
			*WorkingMesh.Mesh, WorkingMesh.CurvatureRawConvexCache, WorkingMesh.CurvatureRawConcaveCache,
			Type, Multiplier, Blur, LevelsMin, LevelsMax, bInvert);

		Mask.Values.SetNumZeroed(NumRenderVerts);
		Mask.bHasValue.Init(false, NumRenderVerts);

		double Sum = 0.0;
		for (int32 RenderIndex = 0; RenderIndex < NumRenderVerts; ++RenderIndex)
		{
			const int32 DynamicVertexID = WorkingMesh.CurvatureRenderVertexToDynamicMeshVertex[RenderIndex];
			if (DynamicVertexID == INDEX_NONE || !DynamicMeshValues.IsValidIndex(DynamicVertexID))
			{
				continue;
			}
			const float Value = DynamicMeshValues[DynamicVertexID];
			Mask.Values[RenderIndex] = Value;
			Mask.bHasValue[RenderIndex] = true;
			++Mask.NumValidValues;
			Sum += Value;
			Mask.MinValue = (Mask.NumValidValues == 1) ? Value : FMath::Min(Mask.MinValue, Value);
			Mask.MaxValue = (Mask.NumValidValues == 1) ? Value : FMath::Max(Mask.MaxValue, Value);
			if (Value <= FVertexMaskForgeScalarMask::Tolerance) { ++Mask.NumNearZero; }
			if (Value >= 1.0f - FVertexMaskForgeScalarMask::Tolerance) { ++Mask.NumNearOne; }
		}
		Mask.MeanValue = (Mask.NumValidValues > 0) ? static_cast<float>(Sum / Mask.NumValidValues) : 0.0f;
		Mask.State = (Mask.NumValidValues > 0) ? EVertexMaskForgeScalarMaskState::Ready : EVertexMaskForgeScalarMaskState::Unavailable;

		return Mask;
	}

	FVertexMaskForgeScalarMask GenerateCurvatureMaskFromDynamicMesh(
		FVertexMaskForgeWorkingMesh& WorkingMesh,
		const EVertexMaskForgeCurvatureType Type,
		const float Multiplier,
		const float Blur,
		const float LevelsMin,
		const float LevelsMax,
		const bool bInvert)
	{
		using namespace UE::Geometry;

		FVertexMaskForgeScalarMask Mask;
		Mask.Source = EVertexMaskForgeScalarMaskSource::Curvature;

		if (!WorkingMesh.Mesh.IsValid())
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}
		const FDynamicMesh3& Mesh = *WorkingMesh.Mesh;
		Mask.RenderVertexCount = Mesh.VertexCount();

		EnsureCurvatureRawCache(WorkingMesh, nullptr, nullptr);
		if (WorkingMesh.CurvatureRawConvexCache.IsEmpty())
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}

		const TArray<float> DynamicMeshValues = ApplyCurvatureArtisticParams(
			Mesh, WorkingMesh.CurvatureRawConvexCache, WorkingMesh.CurvatureRawConcaveCache,
			Type, Multiplier, Blur, LevelsMin, LevelsMax, bInvert);

		const int32 MaxVID = Mesh.MaxVertexID();
		Mask.Values.SetNumZeroed(MaxVID);
		Mask.bHasValue.Init(false, MaxVID);

		double Sum = 0.0;
		for (const int32 VertexID : Mesh.VertexIndicesItr())
		{
			if (!DynamicMeshValues.IsValidIndex(VertexID))
			{
				continue;
			}
			const float Value = DynamicMeshValues[VertexID];
			Mask.Values[VertexID] = Value;
			Mask.bHasValue[VertexID] = true;
			++Mask.NumValidValues;
			Sum += Value;
			Mask.MinValue = (Mask.NumValidValues == 1) ? Value : FMath::Min(Mask.MinValue, Value);
			Mask.MaxValue = (Mask.NumValidValues == 1) ? Value : FMath::Max(Mask.MaxValue, Value);
			if (Value <= FVertexMaskForgeScalarMask::Tolerance) { ++Mask.NumNearZero; }
			if (Value >= 1.0f - FVertexMaskForgeScalarMask::Tolerance) { ++Mask.NumNearOne; }
		}
		Mask.MeanValue = (Mask.NumValidValues > 0) ? static_cast<float>(Sum / Mask.NumValidValues) : 0.0f;
		Mask.State = (Mask.NumValidValues > 0) ? EVertexMaskForgeScalarMaskState::Ready : EVertexMaskForgeScalarMaskState::Unavailable;

		return Mask;
	}
}
