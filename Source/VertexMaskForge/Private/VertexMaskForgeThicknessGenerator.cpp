#include "VertexMaskForgeThicknessGenerator.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "VertexMaskForgeGeneratorUtils.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

namespace
{
	// --- Thickness Mask (V2-G): local-space raycast-based measured thickness ------------------------

	/**
	 * Canonicalized-zero bitwise position key (V2-G corrective audit) -- plain FVector3f cannot be used
	 * directly as a TMap key: GetTypeHash(FVector3f) is a bitwise CRC while operator== is numeric
	 * (-0.0f==+0.0f is true but their bits differ), a real hash/equality contract violation. Zero is
	 * canonicalized to +0.0f BEFORE hashing so -0/+0 always share a key; NaN/Inf are rejected entirely
	 * (TryMake returns false). No spatial tolerance -- only numerically identical (post-canonicalization)
	 * positions share a key.
	 */
	struct FThicknessPositionKey
	{
		uint32 XBits = 0, YBits = 0, ZBits = 0;

		static float CanonicalizeComponent(float Value) { return Value == 0.0f ? 0.0f : Value; }

		static bool TryMake(const FVector3f& Position, FThicknessPositionKey& OutKey)
		{
			if (!FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y) || !FMath::IsFinite(Position.Z))
			{
				return false;
			}
			OutKey.XBits = BitCast<uint32>(CanonicalizeComponent(Position.X));
			OutKey.YBits = BitCast<uint32>(CanonicalizeComponent(Position.Y));
			OutKey.ZBits = BitCast<uint32>(CanonicalizeComponent(Position.Z));
			return true;
		}

		bool operator==(const FThicknessPositionKey& Other) const
		{
			return XBits == Other.XBits && YBits == Other.YBits && ZBits == Other.ZBits;
		}

		friend uint32 GetTypeHash(const FThicknessPositionKey& Key)
		{
			uint32 Hash = GetTypeHash(Key.XBits);
			Hash = HashCombine(Hash, GetTypeHash(Key.YBits));
			return HashCombine(Hash, GetTypeHash(Key.ZBits));
		}
	};


	/**
	 * Buckets every vertex of Mesh by exact local-space position (FThicknessPositionKey) -- used to
	 * build the self-hit incident-triangle exclusion set (V2-G corrective audit). A non-finite position
	 * is never inserted (that vertex is excluded from self-hit exclusion entirely; its own raycast is
	 * already invalid upstream via the origin-position/normal checks).
	 */
	TMap<FThicknessPositionKey, TArray<int32>> BuildThicknessPositionBuckets(const UE::Geometry::FDynamicMesh3& Mesh)
	{
		TMap<FThicknessPositionKey, TArray<int32>> Buckets;
		Buckets.Reserve(Mesh.VertexCount());
		for (const int32 VertexID : Mesh.VertexIndicesItr())
		{
			const FVector3d P = Mesh.GetVertex(VertexID);
			FThicknessPositionKey Key;
			if (FThicknessPositionKey::TryMake(FVector3f(P), Key))
			{
				Buckets.FindOrAdd(Key).Add(VertexID);
			}
		}
		return Buckets;
	}

	/**
	 * Union of Mesh.VtxTrianglesItr() over every vertex sharing OriginVertexID's EXACT position (via
	 * Buckets), deduplicated and sorted ascending by TriangleID -- deterministic self-hit exclusion,
	 * covering hard edges/UV seams/split-vertex duplicates at the same position, never just the origin
	 * vertex's own incident triangles alone (a wedge/hard-edge corner touches several triangles at that
	 * exact point that must ALL be excluded, not only the one the origin corner itself belongs to).
	 */
	TArray<int32> BuildThicknessIncidentTriangleExclusion(
		const UE::Geometry::FDynamicMesh3& Mesh,
		const TMap<FThicknessPositionKey, TArray<int32>>& Buckets,
		const int32 OriginVertexID)
	{
		TArray<int32> Result;
		const FVector3d P = Mesh.GetVertex(OriginVertexID);
		FThicknessPositionKey Key;
		if (!FThicknessPositionKey::TryMake(FVector3f(P), Key))
		{
			return Result;
		}
		const TArray<int32>* CoincidentVertices = Buckets.Find(Key);
		if (!CoincidentVertices)
		{
			return Result;
		}
		TSet<int32> Unique;
		for (const int32 VID : *CoincidentVertices)
		{
			for (const int32 TID : Mesh.VtxTrianglesItr(VID))
			{
				Unique.Add(TID);
			}
		}
		Result = Unique.Array();
		Result.Sort();
		return Result;
	}

	/** Result of a single element's Thickness raycast -- see ComputeThicknessRawValue. */
	struct FThicknessRaycastResult
	{
		bool bHasValue = false;
		float MeasuredThickness = 0.0f;
		bool bOriginNormalInvalid = false;
		int32 NumOrientationRejectedCandidates = 0;
	};

	/**
	 * Core Thickness raycast (V2-G), shared verbatim by both non-Nanite and Source-Topology domains --
	 * both operate on a plain UE::Geometry::FDynamicMesh3+FDynamicMeshAABBTree3, so this function has no
	 * domain-specific logic at all.
	 *
	 * SELF-HIT (corrective audit, closes the "HitT stays pinned near Bias" bug): Origin is offset INSIDE
	 * the mesh (P - N*EffectiveBias, not +N*EffectiveBias -- offsetting outward makes the ray immediately
	 * re-strike its own origin face at t~=Bias, independent of the true opposite-wall distance), and
	 * every triangle incident to any vertex sharing Origin's exact position (ExcludedTriangleIDs, from
	 * BuildThicknessIncidentTriangleExclusion) is excluded via TriangleFilterF -- not just the single
	 * source triangle, since a wedge/hard-edge corner touches several triangles at that exact point that
	 * could each produce a spurious near-zero hit even with the inward offset.
	 *
	 * HITS: FindAllHitTriangles (not FindNearestHitTriangle) within RayMaxDistance, sorted by Distance
	 * ascending then TriangleId ascending (deterministic tie-break), so the search continues past a
	 * rejected candidate to the next -- a self-intersecting or wrong-orientation hit can never hide a
	 * legitimate farther one.
	 *
	 * ORIENTATION: Dot(HitGeometricNormal, RayDirection) > OrientationEpsilon -- HitGeometricNormal is
	 * Mesh.GetTriNormal(HitTriangleId), the TRIANGLE's geometric normal, never an interpolated/tangent-
	 * space/overlay normal. A hit failing this increments NumOrientationRejectedCandidates and the search
	 * continues -- it never contaminates MeasuredThickness.
	 *
	 * MEASUREMENT: MeasuredThickness = HitT + EffectiveBias -- reconstructs the segment skipped by the
	 * inward offset. EffectiveBias never appears anywhere else; it cannot artistically shift the result.
	 */
	FThicknessRaycastResult ComputeThicknessRawValue(
		const UE::Geometry::FDynamicMesh3& Mesh,
		const UE::Geometry::FDynamicMeshAABBTree3& Tree,
		const FVector3d& OriginSurfacePosition,
		const FVector3d& OriginNormal,
		const double EffectiveBias,
		const double RayMaxDistance,
		const TArray<int32>& ExcludedTriangleIDs)
	{
		using namespace UE::Geometry;

		FThicknessRaycastResult Result;

		FVector3d N = OriginNormal;
		if (N.ContainsNaN() || N.IsNearlyZero() || !N.Normalize())
		{
			Result.bOriginNormalInvalid = true;
			return Result;
		}

		const FVector3d Origin = OriginSurfacePosition - N * EffectiveBias;
		const FVector3d Direction = -N;
		const FRay3d Ray(Origin, Direction, /*bDirectionIsNormalized=*/true);

		IMeshSpatial::FQueryOptions Options;
		Options.MaxDistance = RayMaxDistance;
		Options.TriangleFilterF = [&ExcludedTriangleIDs](int32 TriangleID)
		{
			return !ExcludedTriangleIDs.Contains(TriangleID);
		};

		TArray<MeshIntersection::FHitIntersectionResult> Hits;
		if (!Tree.FindAllHitTriangles(Ray, Hits, Options) || Hits.IsEmpty())
		{
			return Result;   // no hit within RayMaxDistance -- bHasValue stays false
		}

		Hits.Sort([](const MeshIntersection::FHitIntersectionResult& A, const MeshIntersection::FHitIntersectionResult& B)
		{
			if (A.Distance != B.Distance) { return A.Distance < B.Distance; }
			return A.TriangleId < B.TriangleId;
		});

		constexpr double OrientationEpsilon = 1e-4;
		for (const MeshIntersection::FHitIntersectionResult& Hit : Hits)
		{
			if (!FMath::IsFinite(Hit.Distance) || Hit.Distance < 0.0)
			{
				continue;
			}
			const FVector3d HitNormal = Mesh.GetTriNormal(Hit.TriangleId);
			if (HitNormal.ContainsNaN())
			{
				continue;
			}
			if (FVector3d::DotProduct(HitNormal, Direction) <= OrientationEpsilon)
			{
				++Result.NumOrientationRejectedCandidates;
				continue;
			}

			Result.bHasValue = true;
			Result.MeasuredThickness = static_cast<float>(Hit.Distance + EffectiveBias);
			return Result;
		}

		return Result;   // every candidate rejected (orientation) -- bHasValue stays false
	}

	/** Effective (post-sanitization) Thickness parameters, always in double -- see SanitizeThicknessParams. */
	struct FThicknessSanitizedParams
	{
		double Min = 0.0, Max = 100.0, Search = 100.0, Bias = 0.01, RayMaxDistance = 100.0;
	};

	double SanitizeFiniteOrDefault(float Raw, float Default)
	{
		return FMath::IsFinite(Raw) ? static_cast<double>(Raw) : static_cast<double>(Default);
	}

	/**
	 * Sanitization cascade (V2-G, corrective audit -- final closed form). All arithmetic in double,
	 * matching the native precision of the raycast pipeline (IMeshSpatial::FQueryOptions::MaxDistance is
	 * already double) -- this alone closes the float32-ULP range-collapse bug the audit found (ULP of
	 * float32 at 1e6 is ~0.119, LARGER than RangeEpsilon=1e-4; ULP of double at 1e6 is ~2.2e-10, 14
	 * orders of magnitude smaller). Min is clamped to DomainMax-RangeEpsilon, NOT DomainMax, reserving
	 * exactly the budget RangeEpsilon needs so Max/Search can never be pushed past DomainMax by the
	 * following FMath::Max step -- see the audit's own proof (MaximumAllowedMin trick).
	 * Guarantees (proven in the audit report, re-derived here in code):
	 *   0 <= Min <= DomainMax-RangeEpsilon; Min+RangeEpsilon <= Max <= DomainMax; Max <= Search <=
	 *   DomainMax; MaximumAllowedBias >= MinBiasClamp; MinBiasClamp <= Bias < Search; Denom=Max-Min > 0;
	 *   RayMaxDistance = Search-Bias >= NumericalTolerance+RayDistanceEpsilon > 0.
	 * NaN/Inf inputs are replaced by finite defaults BEFORE any FMath::Clamp/Max call (FMath::Clamp does
	 * NOT reliably sanitize NaN -- NaN compares false to both bounds, so Clamp(NaN,lo,hi) returns NaN
	 * unchanged). FLT_MAX/DBL_MAX are finite and are absorbed normally by the DomainMax clamp.
	 */
	FThicknessSanitizedParams SanitizeThicknessParams(
		const float RawMinThickness, const float RawMaxThickness, const float RawSearchDistance, const float RawBias)
	{
		constexpr double RangeEpsilon = 1e-4;
		constexpr double MinBiasClamp = 0.001;
		constexpr double NumericalTolerance = 1e-4;
		constexpr double RayDistanceEpsilon = 1e-4;
		constexpr double DomainMax = 1.0e6;
		static_assert(DomainMax > MinBiasClamp + NumericalTolerance + RayDistanceEpsilon, "DomainMax too small relative to the fixed Bias/tolerance floors");

		const double MaximumAllowedMin = DomainMax - RangeEpsilon;

		double Min = SanitizeFiniteOrDefault(RawMinThickness, 0.0f);
		double Max = SanitizeFiniteOrDefault(RawMaxThickness, 100.0f);
		double Search = SanitizeFiniteOrDefault(RawSearchDistance, 100.0f);
		double Bias = SanitizeFiniteOrDefault(RawBias, 0.01f);

		Min = FMath::Clamp(Min, 0.0, MaximumAllowedMin);
		Max = FMath::Clamp(Max, 0.0, DomainMax);
		Max = FMath::Max(Max, Min + RangeEpsilon);

		Search = FMath::Clamp(Search, 0.0, DomainMax);
		Search = FMath::Max3(Search, Max, MinBiasClamp + NumericalTolerance + RayDistanceEpsilon);

		const double MaximumAllowedBias = Search - NumericalTolerance - RayDistanceEpsilon;
		Bias = FMath::Clamp(Bias, MinBiasClamp, MaximumAllowedBias);

		FThicknessSanitizedParams Result;
		Result.Min = Min;
		Result.Max = Max;
		Result.Search = Search;
		Result.Bias = Bias;
		Result.RayMaxDistance = Search - Bias;
		return Result;
	}
}

namespace VertexMaskForgeThicknessGenerator
{
	// AUDITED (M6, Extract Shared Working-Mesh Domain Types): AreThicknessGeometrySnapshotsExactlyEquivalent/
	// IsThicknessSourceTopologyContentUnchanged now live in VertexMaskForgeWorkingMeshTypes (declared in
	// VertexMaskForgeWorkingMeshTypes.h, already transitively included via SVertexMaskForgePanel.h) --
	// no forward declaration needed here anymore. Used here as a GENERATION-time re-verification, not
	// only at Accept (see the corrective audit's own requirement: a Mesh-pointer+DerivedDataKey+count
	// match is a FAST REJECT only, never sufficient alone to reuse cached geometry/raw distances -- the
	// same rule that already applies at Accept must apply during generation/preview too).

	FVertexMaskForgeScalarMask GenerateThicknessMask(
		TUniquePtr<FVertexMaskForgeThicknessCache>& CachePtr,
		const UStaticMesh* Mesh,
		const FStaticMeshLODResources& LOD0,
		const float RawMinThickness,
		const float RawMaxThickness,
		const float RawSearchDistance,
		const float RawBias,
		const float Blur,
		const bool bInvert)
	{
		using namespace UE::Geometry;

		FVertexMaskForgeScalarMask MaskResult;
		MaskResult.Source = EVertexMaskForgeScalarMaskSource::Thickness;

		const FPositionVertexBuffer& RenderPositions = LOD0.VertexBuffers.PositionVertexBuffer;
		const FStaticMeshVertexBuffer& RenderTangents = LOD0.VertexBuffers.StaticMeshVertexBuffer;
		const int32 NumRenderVerts = static_cast<int32>(RenderPositions.GetNumVertices());
		MaskResult.RenderVertexCount = NumRenderVerts;

		if (NumRenderVerts <= 0 || LOD0.IndexBuffer.GetNumIndices() < 3
			|| static_cast<int32>(RenderTangents.GetNumVertices()) != NumRenderVerts)
		{
			MaskResult.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return MaskResult;
		}

		if (!CachePtr.IsValid())
		{
			CachePtr = MakeUnique<FVertexMaskForgeThicknessCache>();
		}
		FVertexMaskForgeThicknessCache& Cache = *CachePtr;

		// Layer 1: local-space geometry + spatial index + freshness snapshot. Rebuilt whenever Mesh
		// identity/DerivedDataKey/counts no longer match -- NEVER Transform (Asset Local Space).
		//
		// AUDITED (V2-G corrective pass, cache correctness): Mesh/DerivedDataKey/count equality is a
		// FAST REJECT ONLY, exactly like Accept's own gate -- a match here NEVER by itself proves the
		// cached LocalMesh/Tree/RawDistances are still valid to reuse (DerivedDataKey can be empty/
		// unreliable for some assets, per AO's own documented residual gap, and this is generation/
		// preview code that can run many times per second during Auto Update, so it must be at least as
		// safe as the Accept-time gate, never weaker). When the fast checks pass, the FULL semantic
		// snapshot comparison (the SAME one Accept uses, never a separate/weaker one) still runs before
		// the cache is trusted -- a coincidental match on the cheap fields alone can never cause stale
		// geometry/tree/distances to be silently reused.
		const int32 NumIndices = LOD0.IndexBuffer.GetNumIndices();
		const bool bCheapKeyMatches = Cache.bTreeValid
			&& Cache.CachedMesh.Get() == Mesh
			&& Cache.CachedDerivedDataKey == LOD0.DerivedDataKey
			&& Cache.CachedNumRenderVerts == NumRenderVerts
			&& Cache.CachedNumIndices == NumIndices;
		const bool bTreeStillValid = bCheapKeyMatches
			&& VertexMaskForgeWorkingMeshTypes::AreThicknessGeometrySnapshotsExactlyEquivalent(Cache, LOD0);

		if (!bTreeStillValid)
		{
			Cache.LocalMesh = MakeUnique<FDynamicMesh3>();
			Cache.LocalMesh->EnableTriangleGroups();
			Cache.LocalMesh->EnableAttributes();
			Cache.LocalMesh->Attributes()->SetNumNormalLayers(1);
			FDynamicMeshNormalOverlay* NormalOverlay = Cache.LocalMesh->Attributes()->PrimaryNormals();

			Cache.SnapshotPositions.SetNumUninitialized(NumRenderVerts);
			Cache.SnapshotTangentZ.SetNumUninitialized(NumRenderVerts);

			// CORRECTION (corrective audit, section on Normal ElementID safety): initialized to
			// INDEX_NONE, never assumed == RenderVertexIndex; the ACTUAL ElementID returned by
			// AppendElement is what gets stored and later used by SetTriangle.
			TArray<int32> NormalElementByRenderVertex;
			NormalElementByRenderVertex.Init(INDEX_NONE, NumRenderVerts);

			for (int32 i = 0; i < NumRenderVerts; ++i)
			{
				const FVector3f Pos = RenderPositions.VertexPosition(i);
				Cache.LocalMesh->AppendVertex(FVector3d(Pos));   // sequential 0..N-1 -- DynamicVertexID == i
				Cache.SnapshotPositions[i] = Pos;

				const FVector4f TangentZ4 = RenderTangents.VertexTangentZ(i);
				const FVector3f TangentZ(TangentZ4.X, TangentZ4.Y, TangentZ4.Z);
				Cache.SnapshotTangentZ[i] = TangentZ;

				if (FMath::IsFinite(TangentZ.X) && FMath::IsFinite(TangentZ.Y) && FMath::IsFinite(TangentZ.Z) && !TangentZ.IsNearlyZero())
				{
					const int32 ElementID = NormalOverlay->AppendElement(TangentZ);
					NormalOverlay->SetParentVertex(ElementID, i);
					NormalElementByRenderVertex[i] = ElementID;
				}
				// else: NormalElementByRenderVertex[i] stays INDEX_NONE -- no element created, never a
				// guessed/fallback normal that could silently change the raycast direction.
			}

			Cache.SnapshotTriangles.Reset();
			int32 NumDegenerateDiscarded = 0;
			const int32 NumTriangles = NumIndices / 3;
			for (int32 TriIndex = 0; TriIndex < NumTriangles; ++TriIndex)
			{
				const int32 I0 = static_cast<int32>(LOD0.IndexBuffer.GetIndex(TriIndex * 3 + 0));
				const int32 I1 = static_cast<int32>(LOD0.IndexBuffer.GetIndex(TriIndex * 3 + 1));
				const int32 I2 = static_cast<int32>(LOD0.IndexBuffer.GetIndex(TriIndex * 3 + 2));
				if (I0 == I1 || I1 == I2 || I0 == I2)
				{
					++NumDegenerateDiscarded;
					continue;
				}
				if (VertexMaskForgeWorkingMeshTypes::IsThicknessTriangleDegenerate(FVector3d(Cache.SnapshotPositions[I0]), FVector3d(Cache.SnapshotPositions[I1]), FVector3d(Cache.SnapshotPositions[I2])))
				{
					++NumDegenerateDiscarded;
					continue;
				}

				const int32 TID = Cache.LocalMesh->AppendTriangle(I0, I1, I2);
				if (TID < 0)
				{
					++NumDegenerateDiscarded;
					continue;
				}
				const int32 E0 = NormalElementByRenderVertex[I0];
				const int32 E1 = NormalElementByRenderVertex[I1];
				const int32 E2 = NormalElementByRenderVertex[I2];
				if (E0 != INDEX_NONE && E1 != INDEX_NONE && E2 != INDEX_NONE)
				{
					NormalOverlay->SetTriangle(TID, FIndex3i(E0, E1, E2));
				}
				// else: triangle exists geometrically (needed for other corners' raycasts to hit it) but
				// its own overlay entry stays unset -- IsSetTriangle(TID) reports false, never a guessed
				// normal association.
				Cache.SnapshotTriangles.Add(FIntVector(I0, I1, I2));
			}

			Cache.Tree = MakeUnique<FDynamicMeshAABBTree3>(Cache.LocalMesh.Get());
			Cache.CachedMesh = Mesh;
			Cache.CachedDerivedDataKey = LOD0.DerivedDataKey;
			Cache.CachedNumRenderVerts = NumRenderVerts;
			Cache.CachedNumIndices = NumIndices;
			Cache.CachedGeometryFingerprint = VertexMaskForgeGeneratorUtils::ComputeDynamicMeshGeometryFingerprint(*Cache.LocalMesh);
			Cache.bTreeValid = true;
			Cache.bValuesValid = false;
			Cache.NumDegenerateTrianglesDiscarded = NumDegenerateDiscarded;
		}

		const FThicknessSanitizedParams Params = SanitizeThicknessParams(RawMinThickness, RawMaxThickness, RawSearchDistance, RawBias);

		// Layer 2: raw measured distances. Rebuilt only when SearchDistance/Bias actually changed, or
		// Layer 1 was just rebuilt above.
		const bool bValuesStillValid = Cache.bValuesValid
			&& FMath::IsNearlyEqual(Cache.CachedSearchDistance, static_cast<float>(Params.Search), 1e-4f)
			&& FMath::IsNearlyEqual(Cache.CachedBias, static_cast<float>(Params.Bias), 1e-6f);

		if (!bValuesStillValid)
		{
			const FDynamicMesh3& LocalMesh = *Cache.LocalMesh;
			const FDynamicMeshAABBTree3& Tree = *Cache.Tree;
			const TMap<FThicknessPositionKey, TArray<int32>> Buckets = BuildThicknessPositionBuckets(LocalMesh);

			Cache.RawDistances.SetNumZeroed(NumRenderVerts);
			Cache.bRawValid.Init(false, NumRenderVerts);
			Cache.NumInvalidOriginNormal = 0;
			Cache.NumNoHit = 0;
			Cache.NumOrientationRejections = 0;

			for (int32 i = 0; i < NumRenderVerts; ++i)
			{
				const FVector3f TangentZ = Cache.SnapshotTangentZ[i];
				if (!FMath::IsFinite(TangentZ.X) || !FMath::IsFinite(TangentZ.Y) || !FMath::IsFinite(TangentZ.Z) || TangentZ.IsNearlyZero())
				{
					++Cache.NumInvalidOriginNormal;
					continue;
				}

				const TArray<int32> Excluded = BuildThicknessIncidentTriangleExclusion(LocalMesh, Buckets, i);
				const FThicknessRaycastResult RaycastResult = ComputeThicknessRawValue(
					LocalMesh, Tree, FVector3d(Cache.SnapshotPositions[i]), FVector3d(TangentZ),
					Params.Bias, Params.RayMaxDistance, Excluded);

				if (RaycastResult.bOriginNormalInvalid)
				{
					++Cache.NumInvalidOriginNormal;
					continue;
				}
				Cache.NumOrientationRejections += RaycastResult.NumOrientationRejectedCandidates;
				if (!RaycastResult.bHasValue)
				{
					++Cache.NumNoHit;
					continue;
				}

				Cache.RawDistances[i] = RaycastResult.MeasuredThickness;
				Cache.bRawValid[i] = true;
			}

			Cache.CachedSearchDistance = static_cast<float>(Params.Search);
			Cache.CachedBias = static_cast<float>(Params.Bias);
			Cache.bValuesValid = true;
		}

		// Normalize -> thin=white flip -> Blur -> Invert. Never cached; always cheap relative to the raycast.
		TArray<float> RawMaskValues;
		RawMaskValues.SetNumZeroed(NumRenderVerts);
		TArray<bool> bHasRaw;
		bHasRaw.Init(false, NumRenderVerts);
		const double Denom = Params.Max - Params.Min;
		for (int32 i = 0; i < NumRenderVerts; ++i)
		{
			if (!Cache.bRawValid[i]) { continue; }
			const double Normalized = FMath::Clamp((static_cast<double>(Cache.RawDistances[i]) - Params.Min) / Denom, 0.0, 1.0);
			RawMaskValues[i] = static_cast<float>(1.0 - Normalized);
			bHasRaw[i] = true;
		}

		TArray<float> BlurredValues = RawMaskValues;
		if (Blur > 0.0f)
		{
			const TArray<TArray<int32>> Adjacency = VertexMaskForgeGeneratorUtils::BuildRenderVertexAdjacency(LOD0, NumRenderVerts);
			BlurredValues = VertexMaskForgeGeneratorUtils::ApplyAdjacencyTopologicalBlur(Adjacency, RawMaskValues, bHasRaw, Blur);
		}

		MaskResult.Values.SetNumZeroed(NumRenderVerts);
		MaskResult.bHasValue.Init(false, NumRenderVerts);
		double Sum = 0.0;
		for (int32 i = 0; i < NumRenderVerts; ++i)
		{
			if (!bHasRaw[i]) { continue; }
			const float Final = bInvert ? (1.0f - BlurredValues[i]) : BlurredValues[i];
			MaskResult.Values[i] = Final;
			MaskResult.bHasValue[i] = true;
			++MaskResult.NumValidValues;
			Sum += Final;
			MaskResult.MinValue = (MaskResult.NumValidValues == 1) ? Final : FMath::Min(MaskResult.MinValue, Final);
			MaskResult.MaxValue = (MaskResult.NumValidValues == 1) ? Final : FMath::Max(MaskResult.MaxValue, Final);
			if (Final <= FVertexMaskForgeScalarMask::Tolerance) { ++MaskResult.NumNearZero; }
			if (Final >= 1.0f - FVertexMaskForgeScalarMask::Tolerance) { ++MaskResult.NumNearOne; }
		}
		MaskResult.MeanValue = (MaskResult.NumValidValues > 0) ? static_cast<float>(Sum / MaskResult.NumValidValues) : 0.0f;
		// AUDITED (per explicit spec): a structurally-valid mesh with zero qualifying hits is still
		// Ready (never Unavailable) -- Unavailable is reserved for the structural early-outs above (no
		// geometry/triangles/buffer mismatch). NumValidValues==0 with State==Ready is a real, diagnosable
		// state ("no opposite surface found"), not a failure -- see GetThicknessMaskDiagnosticText.
		MaskResult.State = EVertexMaskForgeScalarMaskState::Ready;
		return MaskResult;
	}

	FVertexMaskForgeScalarMask GenerateThicknessMaskFromDynamicMesh(
		TUniquePtr<FVertexMaskForgeSourceTopologyThicknessCache>& CachePtr,
		const FVertexMaskForgeWorkingMesh& WorkingMesh,
		const float RawMinThickness,
		const float RawMaxThickness,
		const float RawSearchDistance,
		const float RawBias,
		const float Blur,
		const bool bInvert)
	{
		using namespace UE::Geometry;

		FVertexMaskForgeScalarMask MaskResult;
		MaskResult.Source = EVertexMaskForgeScalarMaskSource::Thickness;

		if (!WorkingMesh.Mesh.IsValid())
		{
			MaskResult.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return MaskResult;
		}
		const FDynamicMesh3& SourceMesh = *WorkingMesh.Mesh;
		const int32 NumCorners = SourceMesh.TriangleCount() * 3;
		MaskResult.RenderVertexCount = NumCorners;
		if (NumCorners <= 0)
		{
			MaskResult.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return MaskResult;
		}
		const FDynamicMeshNormalOverlay* SourceNormalOverlay =
			(SourceMesh.HasAttributes() && SourceMesh.Attributes()->PrimaryNormals() != nullptr)
			? SourceMesh.Attributes()->PrimaryNormals() : nullptr;
		if (!SourceNormalOverlay || SourceNormalOverlay->ElementCount() <= 0)
		{
			MaskResult.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return MaskResult;
		}

		if (!CachePtr.IsValid())
		{
			CachePtr = MakeUnique<FVertexMaskForgeSourceTopologyThicknessCache>();
		}
		FVertexMaskForgeSourceTopologyThicknessCache& Cache = *CachePtr;

		// AUDITED (V2-G corrective pass, cache correctness): pointer identity of &SourceMesh is a FAST
		// REJECT ONLY -- never trusted alone, and NEVER paired with WorkingMesh.GeometryFingerprint (a
		// value computed once, elsewhere, at RefreshSelection time, that this function has no way to
		// prove is still in sync with SourceMesh's CURRENT content). Instead, the fingerprint is
		// RECOMPUTED FRESH, right here, directly from SourceMesh as it exists at this exact call -- a
		// genuine content check, not a reused/possibly-stale value. This is the SAME function
		// (ComputeDynamicMeshGeometryFingerprint) already proven to hash position+normal+connectivity;
		// recomputing it costs O(V+T) (no raycasting), bounded and far cheaper than the raycast pass
		// itself, so this runs safely even during interactive Auto Update.
		const uint32 CurrentFingerprint = VertexMaskForgeGeneratorUtils::ComputeDynamicMeshGeometryFingerprint(SourceMesh);
		const bool bTreeStillValid = Cache.bTreeValid
			&& Cache.CachedSourceMesh == &SourceMesh
			&& Cache.CachedGeometryFingerprint == CurrentFingerprint;

		if (!bTreeStillValid)
		{
			Cache.LocalMesh = MakeUnique<FDynamicMesh3>();
			Cache.LocalMesh->EnableTriangleGroups();

			TArray<int32> VertexIdToLocalIndex;
			VertexIdToLocalIndex.Init(INDEX_NONE, SourceMesh.MaxVertexID());
			for (const int32 VertexID : SourceMesh.VertexIndicesItr())
			{
				VertexIdToLocalIndex[VertexID] = Cache.LocalMesh->AppendVertex(SourceMesh.GetVertex(VertexID));
			}

			int32 NumDegenerateDiscarded = 0;
			for (const int32 TriangleID : SourceMesh.TriangleIndicesItr())
			{
				const FIndex3i Tri = SourceMesh.GetTriangle(TriangleID);
				if (VertexMaskForgeWorkingMeshTypes::IsThicknessTriangleDegenerate(SourceMesh.GetVertex(Tri.A), SourceMesh.GetVertex(Tri.B), SourceMesh.GetVertex(Tri.C)))
				{
					++NumDegenerateDiscarded;
					continue;
				}
				Cache.LocalMesh->AppendTriangle(VertexIdToLocalIndex[Tri.A], VertexIdToLocalIndex[Tri.B], VertexIdToLocalIndex[Tri.C]);
			}

			Cache.Tree = MakeUnique<FDynamicMeshAABBTree3>(Cache.LocalMesh.Get());
			Cache.CachedSourceMesh = &SourceMesh;
			Cache.CachedGeometryFingerprint = CurrentFingerprint;
			Cache.bTreeValid = true;
			Cache.bValuesValid = false;
			Cache.NumDegenerateTrianglesDiscarded = NumDegenerateDiscarded;
		}

		const FThicknessSanitizedParams Params = SanitizeThicknessParams(RawMinThickness, RawMaxThickness, RawSearchDistance, RawBias);

		const bool bValuesStillValid = Cache.bValuesValid
			&& FMath::IsNearlyEqual(Cache.CachedSearchDistance, static_cast<float>(Params.Search), 1e-4f)
			&& FMath::IsNearlyEqual(Cache.CachedBias, static_cast<float>(Params.Bias), 1e-6f);

		if (!bValuesStillValid)
		{
			const FDynamicMesh3& LocalMesh = *Cache.LocalMesh;
			const FDynamicMeshAABBTree3& Tree = *Cache.Tree;
			const TMap<FThicknessPositionKey, TArray<int32>> Buckets = BuildThicknessPositionBuckets(LocalMesh);

			TArray<int32> VertexIdToLocalIndex;
			VertexIdToLocalIndex.Init(INDEX_NONE, SourceMesh.MaxVertexID());
			{
				int32 NextLocal = 0;
				for (const int32 VertexID : SourceMesh.VertexIndicesItr())
				{
					VertexIdToLocalIndex[VertexID] = NextLocal++;
				}
			}

			Cache.RawDistances.SetNumZeroed(NumCorners);
			Cache.bRawValid.Init(false, NumCorners);
			Cache.NumInvalidOriginNormal = 0;
			Cache.NumNoHit = 0;
			Cache.NumOrientationRejections = 0;

			int32 CornerIndex = 0;
			for (const int32 TriangleID : SourceMesh.TriangleIndicesItr())
			{
				const FIndex3i VertTri = SourceMesh.GetTriangle(TriangleID);
				const FIndex3i NormalTri = SourceNormalOverlay->IsSetTriangle(TriangleID)
					? SourceNormalOverlay->GetTriangle(TriangleID) : FIndex3i(INDEX_NONE, INDEX_NONE, INDEX_NONE);

				for (int32 Corner = 0; Corner < 3; ++Corner, ++CornerIndex)
				{
					const int32 ElementID = NormalTri[Corner];
					if (ElementID == INDEX_NONE || !SourceNormalOverlay->IsElement(ElementID))
					{
						++Cache.NumInvalidOriginNormal;
						continue;
					}
					const FVector3f LocalNormal3f = SourceNormalOverlay->GetElement(ElementID);
					const FVector3d Origin = SourceMesh.GetVertex(VertTri[Corner]);
					const int32 OriginLocalVertexID = VertexIdToLocalIndex[VertTri[Corner]];

					const TArray<int32> Excluded = BuildThicknessIncidentTriangleExclusion(LocalMesh, Buckets, OriginLocalVertexID);
					const FThicknessRaycastResult RaycastResult = ComputeThicknessRawValue(
						LocalMesh, Tree, Origin, FVector3d(LocalNormal3f), Params.Bias, Params.RayMaxDistance, Excluded);

					if (RaycastResult.bOriginNormalInvalid)
					{
						++Cache.NumInvalidOriginNormal;
						continue;
					}
					Cache.NumOrientationRejections += RaycastResult.NumOrientationRejectedCandidates;
					if (!RaycastResult.bHasValue)
					{
						++Cache.NumNoHit;
						continue;
					}

					Cache.RawDistances[CornerIndex] = RaycastResult.MeasuredThickness;
					Cache.bRawValid[CornerIndex] = true;
				}
			}

			Cache.CachedSearchDistance = static_cast<float>(Params.Search);
			Cache.CachedBias = static_cast<float>(Params.Bias);
			Cache.bValuesValid = true;
		}

		TArray<float> RawMaskValues;
		RawMaskValues.SetNumZeroed(NumCorners);
		TArray<bool> bHasRaw;
		bHasRaw.Init(false, NumCorners);
		const double Denom = Params.Max - Params.Min;
		for (int32 i = 0; i < NumCorners; ++i)
		{
			if (!Cache.bRawValid[i]) { continue; }
			const double Normalized = FMath::Clamp((static_cast<double>(Cache.RawDistances[i]) - Params.Min) / Denom, 0.0, 1.0);
			RawMaskValues[i] = static_cast<float>(1.0 - Normalized);
			bHasRaw[i] = true;
		}

		TArray<float> BlurredValues = RawMaskValues;
		if (Blur > 0.0f)
		{
			const TArray<TArray<int32>> Adjacency = VertexMaskForgeGeneratorUtils::BuildCornerAdjacency(SourceMesh, SourceNormalOverlay, NumCorners);
			BlurredValues = VertexMaskForgeGeneratorUtils::ApplyAdjacencyTopologicalBlur(Adjacency, RawMaskValues, bHasRaw, Blur);
		}

		MaskResult.Values.SetNumZeroed(NumCorners);
		MaskResult.bHasValue.Init(false, NumCorners);
		double Sum = 0.0;
		for (int32 i = 0; i < NumCorners; ++i)
		{
			if (!bHasRaw[i]) { continue; }
			const float Final = bInvert ? (1.0f - BlurredValues[i]) : BlurredValues[i];
			MaskResult.Values[i] = Final;
			MaskResult.bHasValue[i] = true;
			++MaskResult.NumValidValues;
			Sum += Final;
			MaskResult.MinValue = (MaskResult.NumValidValues == 1) ? Final : FMath::Min(MaskResult.MinValue, Final);
			MaskResult.MaxValue = (MaskResult.NumValidValues == 1) ? Final : FMath::Max(MaskResult.MaxValue, Final);
			if (Final <= FVertexMaskForgeScalarMask::Tolerance) { ++MaskResult.NumNearZero; }
			if (Final >= 1.0f - FVertexMaskForgeScalarMask::Tolerance) { ++MaskResult.NumNearOne; }
		}
		MaskResult.MeanValue = (MaskResult.NumValidValues > 0) ? static_cast<float>(Sum / MaskResult.NumValidValues) : 0.0f;
		MaskResult.State = EVertexMaskForgeScalarMaskState::Ready;
		return MaskResult;
	}
}
