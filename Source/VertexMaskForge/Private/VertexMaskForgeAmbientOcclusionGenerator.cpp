#include "VertexMaskForgeAmbientOcclusionGenerator.h"

#include "Async/ParallelFor.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "Engine/StaticMesh.h"
#include "HAL/PlatformTime.h"
#include "StaticMeshResources.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

namespace
{
	/**
	 * Deterministic, COSINE-WEIGHTED hemisphere sample directions (Z-up local tangent frame: X/Y span
	 * the hemisphere's base, Z is the pole/normal direction), via Malley's method (uniform disk sample
	 * -> project to hemisphere) combined with a golden-angle Fibonacci spiral for the disk sample
	 * itself -- no random number generator, no seed, no dependency on evaluation order, so
	 * regenerating with the same Samples count always reproduces the EXACT same directions (Test D's
	 * "regenerate with equal parameters produces identical results" requirement).
	 *
	 * AUDITED (sampling-quality fix, re-examined per explicit correction): previously UNIFORM over the
	 * hemisphere (Z = 1 - T); now COSINE-WEIGHTED (R = sqrt(T), Z = sqrt(1 - T)), which concentrates
	 * more samples near the normal direction and fewer at grazing angles -- physically appropriate for
	 * AO (a grazing-angle occluder contributes less to perceived occlusion than one near the normal),
	 * and reduces the visible noise/banding a uniform distribution produced at low Samples counts.
	 * Computed ONCE per GenerateAmbientOcclusionMask call (not once per render vertex); reoriented AND
	 * rotated per-vertex by the caller -- see ComputeDeterministicScrambleAngle and the caller's own
	 * SAMPLING doc note for why a per-vertex azimuthal rotation is applied on top of this fixed set.
	 */
	TArray<FVector> BuildHemisphereSampleDirections(const int32 NumSamples)
	{
		TArray<FVector> Directions;
		Directions.Reserve(NumSamples);

		const double GoldenAngle = PI * (3.0 - FMath::Sqrt(5.0));
		for (int32 i = 0; i < NumSamples; ++i)
		{
			const double U1 = (static_cast<double>(i) + 0.5) / static_cast<double>(NumSamples);
			const double R = FMath::Sqrt(U1);
			const double Theta = GoldenAngle * static_cast<double>(i);
			const double Z = FMath::Sqrt(FMath::Max(0.0, 1.0 - U1));
			Directions.Add(FVector(R * FMath::Cos(Theta), R * FMath::Sin(Theta), Z));
		}
		return Directions;
	}

	/**
	 * Deterministic azimuthal scramble angle (radians, [0, 2*PI)) for one render vertex's sample set,
	 * derived from a QUANTIZED WORLD POSITION key -- the same "quantize to a grid, hash the cell"
	 * idiom BuildPositionBuckets already uses elsewhere in this file for stable geometric grouping,
	 * applied here for a different purpose (a per-position pseudo-random-but-deterministic rotation,
	 * not vertex matching).
	 *
	 * AUDITED (sampling-quality fix): deliberately keyed by POSITION, never by render vertex INDEX --
	 * FVector::FindBestAxisVectors picks a tangent basis that can discontinuously "flip" between
	 * neighboring vertices with only slightly different normals, which combined with every vertex
	 * reusing the IDENTICAL, unrotated sample set previously produced visible coherent banding /
	 * structured patterns across the surface (the artifact reported in manual testing). Rotating each
	 * vertex's samples by a position-derived angle breaks that coherence. Keying by POSITION (not
	 * index) is what keeps hard-edge/UV-seam-coincident render vertices (same position, different
	 * render vertex IDs) receiving the SAME scramble, so a seam never introduces an artificial
	 * discontinuity in the noise pattern that isn't actually present in the underlying geometry --
	 * exactly the requirement that seam-splitting must never change AO behavior (see the function's
	 * SELF-HIT doc note for the same principle applied to occlusion itself).
	 */
	float ComputeDeterministicScrambleAngle(const FVector& WorldPos)
	{
		constexpr double QuantizeScale = 1000.0; // 1/1000 Unreal unit grid.
		const FIntVector Key(
			FMath::RoundToInt(WorldPos.X * QuantizeScale),
			FMath::RoundToInt(WorldPos.Y * QuantizeScale),
			FMath::RoundToInt(WorldPos.Z * QuantizeScale));
		const uint32 Hash = HashCombine(HashCombine(GetTypeHash(Key.X), GetTypeHash(Key.Y)), GetTypeHash(Key.Z));
		return (static_cast<float>(Hash) / static_cast<float>(MAX_uint32)) * 2.0f * UE_PI;
	}

	/**
	 * AUDITED (AO Levels + vanilla inversion checkpoint): the ONE shared, pure post-processing step
	 * both GenerateAmbientOcclusionMask (render-vertex) and GenerateAmbientOcclusionMaskFromDynamicMesh
	 * (Source-Topology) call to turn a single cached RawAO sample into the final composed value --
	 * never duplicated, never diverges between the two domains. Does no geometry, no raycasts, no cache
	 * access; a pure scalar transform, safe to call every recomposition regardless of Auto Update
	 * Preview, cache hit/miss, or which domain called it.
	 *
	 * PIPELINE (confirmed against the existing composition order before writing this; see the
	 * checkpoint report for the full confirmation):
	 *   1. RawAO: the raw hemisphere-occlusion fraction from AOCache.RawValues (or
	 *      FVertexMaskForgeSourceTopologyAOCache.RawValues) -- convention unchanged: 0 = exposed
	 *      (no occluders), 1 = fully occluded/cavity.
	 *   2. BaseAO = 1 - RawAO -- the NEW vanilla inversion (checkpoint requirement): baked
	 *      unconditionally into the AO layer's own interpretation, independent of the user-facing
	 *      Invert checkbox, which keeps its default (false/unchecked) meaning and serialized value.
	 *      With Invert left OFF, BaseAO is now exactly what previously required Invert ON to see.
	 *   3. LevelsMin/LevelsMax: saturate((BaseAO - LevelsMin) / max(LevelsMax - LevelsMin, Epsilon)) --
	 *      a standard black/white-point remap over BaseAO. Defaults (Min=0, Max=1) make this an exact
	 *      no-op (Denom=1, numerator unchanged, saturate is a no-op since BaseAO is already in [0,1]),
	 *      so existing sessions/serialized state that predate this field (defaulting to 0/1) are
	 *      visually unaffected -- see the checkpoint report.
	 *   4. User Invert (bInvert): applied LAST, over the ALREADY-leveled result -- FinalAO = bInvert ?
	 *      (1 - LeveledAO) : LeveledAO. This is what makes Invert "invert the new vanilla result" rather
	 *      than "toggle between old and new vanilla" -- the vanilla flip in step 2 is unconditional and
	 *      structural, not something Invert ever cancels back out to the OLD pre-checkpoint behavior.
	 *
	 * DIVIDE-BY-ZERO / NaN SAFETY: LevelsMax <= LevelsMin (including exactly equal) is handled by
	 * clamping the denominator to a small Epsilon rather than rejecting the input or clamping
	 * LevelsMin/LevelsMax themselves -- the UI keeps showing exactly what the user set (never silently
	 * snapped), and the composed result becomes a hard step (everything at or above LevelsMin reads as
	 * white) instead of NaN/Inf/undefined -- a deterministic, artist-legible degenerate case rather than
	 * a crash or a silently-wrong value.
	 */
	float ApplyAOLevelsAndInvert(const float RawAO, const float LevelsMin, const float LevelsMax, const bool bInvert)
	{
		constexpr float Epsilon = 1e-4f;

		const float BaseAO = 1.0f - RawAO;

		const float Denom = FMath::Max(LevelsMax - LevelsMin, Epsilon);
		const float LeveledAO = FMath::Clamp((BaseAO - LevelsMin) / Denom, 0.0f, 1.0f);

		return bInvert ? (1.0f - LeveledAO) : LeveledAO;
	}
}

namespace VertexMaskForgeAmbientOcclusionGenerator
{
	bool IsAmbientOcclusionInputValid(const FStaticMeshLODResources& LOD0)
	{
		const int32 NumRenderVerts = static_cast<int32>(LOD0.VertexBuffers.PositionVertexBuffer.GetNumVertices());
		return NumRenderVerts > 0
			&& LOD0.IndexBuffer.GetNumIndices() >= 3
			&& static_cast<int32>(LOD0.VertexBuffers.StaticMeshVertexBuffer.GetNumVertices()) == NumRenderVerts;
	}

	bool IsAmbientOcclusionInputValidForDynamicMesh(const UE::Geometry::FDynamicMesh3* Mesh)
	{
		return Mesh != nullptr
			&& Mesh->VertexCount() > 0 && Mesh->TriangleCount() > 0
			&& Mesh->HasAttributes() && Mesh->Attributes()->PrimaryNormals() != nullptr
			&& Mesh->Attributes()->PrimaryNormals()->ElementCount() > 0;
	}

	FVertexMaskForgeScalarMask GenerateAmbientOcclusionMask(
		TUniquePtr<FVertexMaskForgeAOCache>& CachePtr,
		const UStaticMesh* Mesh,
		const FStaticMeshLODResources& LOD0,
		const FTransform& ComponentTransform,
		const FVertexMaskForgeAOParams& RawParams)
	{
		FVertexMaskForgeScalarMask Mask;
		Mask.Source = EVertexMaskForgeScalarMaskSource::AmbientOcclusion;

		// Never trust the UI clamp alone (same defensive posture as every other generator's inputs).
		FVertexMaskForgeAOParams Params = RawParams;
		Params.Samples = FMath::Clamp(Params.Samples, 8, 256);
		Params.MaxDistance = FMath::Clamp(Params.MaxDistance, 0.01f, 10000.0f);
		Params.Bias = FMath::Clamp(Params.Bias, 0.001f, 10.0f);
		// AUDITED (AO Levels): clamped independently to [0,1] each -- LevelsMax <= LevelsMin is a valid,
		// deterministic degenerate case (see ApplyAOLevelsAndInvert's own doc comment), not something to
		// reorder or reject here.
		Params.LevelsMin = FMath::Clamp(Params.LevelsMin, 0.0f, 1.0f);
		Params.LevelsMax = FMath::Clamp(Params.LevelsMax, 0.0f, 1.0f);
		Mask.UsedAOParams = Params;

		const FPositionVertexBuffer& RenderPositions = LOD0.VertexBuffers.PositionVertexBuffer;
		const FStaticMeshVertexBuffer& RenderTangents = LOD0.VertexBuffers.StaticMeshVertexBuffer;
		const int32 NumRenderVerts = static_cast<int32>(RenderPositions.GetNumVertices());
		Mask.RenderVertexCount = NumRenderVerts;

		if (NumRenderVerts <= 0
			|| LOD0.IndexBuffer.GetNumIndices() < 3
			|| static_cast<int32>(RenderTangents.GetNumVertices()) != NumRenderVerts)
		{
			// No geometry, no triangles, or a normal buffer that doesn't match the render vertex
			// count (partial/invalid, same treatment as a mismatched Color Vertex Buffer elsewhere in
			// this file) -- never guessed or index-clamped.
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}

		if (!CachePtr.IsValid())
		{
			CachePtr = MakeUnique<FVertexMaskForgeAOCache>();
		}
		FVertexMaskForgeAOCache& Cache = *CachePtr;

		const double GenerationStartSeconds = FPlatformTime::Seconds();

		// Layer 1: occluder geometry + spatial index. See the function's own CACHE doc comment for
		// exactly what each comparison catches.
		//
		// AUDITED (DerivedDataKey false-mismatch fix, per explicit root-cause confirmation): the key
		// comparison is now a PLAIN equality (FString::operator== already treats "both empty" as
		// equal, which is the mathematically correct answer -- two unknown/unavailable keys are not
		// evidence of a change). The PREVIOUS policy additionally required the CURRENT key to be
		// non-empty, which meant any asset whose LOD0.DerivedDataKey is never populated (confirmed by
		// the checkpoint's own diagnostic logs: oldEmpty=true, newEmpty=true, equal=true, logged as a
		// "mismatch" every single call) could NEVER hit the Tree cache -- a permanent, silent full
		// rebuild on every call, not a real geometry change. The corrected policy, exactly as
		// specified:
		//   - old empty + new empty            -> compatible (this is the case that was broken)
		//   - old non-empty + new non-empty, == -> compatible
		//   - one empty, other non-empty        -> incompatible (a key appeared or disappeared)
		//   - both non-empty, different         -> incompatible (real content change)
		// DerivedDataKey is therefore an OPTIONAL, best-effort field within the cache key -- never an
		// absolute veto. Safety against reimport/rebuild does NOT depend on this field alone even when
		// it is unavailable: Mesh identity, NumRenderVerts, NumIndices, and Transform are independent,
		// already-audited fields in the SAME AND-chain (see below) -- a reimport that changes vertex/
		// triangle counts is still caught by bVertCountMatches/bIndexCountMatches regardless of
		// DerivedDataKey's availability. A reimport that preserves both counts AND leaves
		// DerivedDataKey empty is a known, accepted residual gap for that specific (rare) asset
		// category -- not solvable by any stable identifier already available on this code path
		// without inventing a new one, which was explicitly out of scope for this fix.
		const int32 NumIndices = LOD0.IndexBuffer.GetNumIndices();
		const bool bMeshMatches = Cache.CachedMesh.Get() == Mesh;
		const bool bKeyMatches = Cache.CachedDerivedDataKey == LOD0.DerivedDataKey;
		const bool bVertCountMatches = Cache.CachedNumRenderVerts == NumRenderVerts;
		const bool bIndexCountMatches = Cache.CachedNumIndices == NumIndices;
		const bool bTransformMatches = Cache.CachedTransform.Equals(ComponentTransform, 1e-5);
		const bool bTreeStillValid = Cache.bTreeValid
			&& bMeshMatches
			&& bKeyMatches
			&& bVertCountMatches
			&& bIndexCountMatches
			&& bTransformMatches;

		// Fires ONLY on a genuine miss -- Cache.bTreeValid was already true (a Tree existed from a
		// PREVIOUS call) but at least one field no longer matches. Never fires on the legitimate first
		// build of a session. One compact Verbose line, no addresses/hashes/full transform dumps.
		if (Cache.bTreeValid && !bTreeStillValid)
		{
			TArray<FString> Reasons;
			if (!bMeshMatches) { Reasons.Add(TEXT("Mesh changed")); }
			if (!bKeyMatches) { Reasons.Add(TEXT("DerivedDataKey changed")); }
			if (!bVertCountMatches || !bIndexCountMatches) { Reasons.Add(TEXT("Vertex/index count changed")); }
			if (!bTransformMatches) { Reasons.Add(TEXT("Transform changed")); }
			UE_LOG(LogVertexMaskForge, Verbose,
				TEXT("Vertex Mask Forge: AO Tree cache miss: %s"), *FString::Join(Reasons, TEXT(", ")));
		}

		if (!bTreeStillValid)
		{
			Cache.WorldMesh = MakeUnique<UE::Geometry::FDynamicMesh3>();
			Cache.WorldMesh->EnableTriangleGroups();

			for (int32 i = 0; i < NumRenderVerts; ++i)
			{
				const FVector WorldPos = ComponentTransform.TransformPosition(FVector(RenderPositions.VertexPosition(i)));
				Cache.WorldMesh->AppendVertex(WorldPos);
			}

			const int32 NumTriangles = NumIndices / 3;
			for (int32 TriIndex = 0; TriIndex < NumTriangles; ++TriIndex)
			{
				const int32 I0 = static_cast<int32>(LOD0.IndexBuffer.GetIndex(TriIndex * 3 + 0));
				const int32 I1 = static_cast<int32>(LOD0.IndexBuffer.GetIndex(TriIndex * 3 + 1));
				const int32 I2 = static_cast<int32>(LOD0.IndexBuffer.GetIndex(TriIndex * 3 + 2));
				if (I0 == I1 || I1 == I2 || I0 == I2)
				{
					continue; // Degenerate triangle (zero area): never fed to the occluder tree.
				}
				Cache.WorldMesh->AppendTriangle(I0, I1, I2);
			}

			Cache.Tree = MakeUnique<UE::Geometry::FDynamicMeshAABBTree3>(Cache.WorldMesh.Get());
			Cache.CachedMesh = Mesh;
			Cache.CachedDerivedDataKey = LOD0.DerivedDataKey;
			Cache.CachedTransform = ComponentTransform;
			Cache.CachedNumRenderVerts = NumRenderVerts;
			Cache.CachedNumIndices = NumIndices;
			Cache.bTreeValid = true;
			Cache.bValuesValid = false; // Geometry changed: any previously-cached raw values are stale.
		}

		// Layer 2: raw occlusion fraction per render vertex. Rebuilt only if Samples/MaxDistance/Bias
		// actually changed (or Layer 1 was just rebuilt above).
		const bool bSamplesMatch = Cache.CachedSamples == Params.Samples;
		const bool bMaxDistanceMatches = FMath::IsNearlyEqual(Cache.CachedMaxDistance, Params.MaxDistance, 1e-4f);
		const bool bBiasMatches = FMath::IsNearlyEqual(Cache.CachedBias, Params.Bias, 1e-6f);
		const bool bValuesStillValid = Cache.bValuesValid && bSamplesMatch && bMaxDistanceMatches && bBiasMatches;

		// Fires only on a genuine miss where the Tree survived (if the Tree itself was just rebuilt
		// above, RawValues are correctly and unconditionally invalidated too -- not a separate,
		// unexplained miss, so it is not logged again here).
		if (bTreeStillValid && Cache.bValuesValid && !bValuesStillValid)
		{
			TArray<FString> Reasons;
			if (!bSamplesMatch) { Reasons.Add(FString::Printf(TEXT("Samples %d -> %d"), Cache.CachedSamples, Params.Samples)); }
			if (!bMaxDistanceMatches) { Reasons.Add(TEXT("MaxDistance changed")); }
			if (!bBiasMatches) { Reasons.Add(TEXT("Bias changed")); }
			UE_LOG(LogVertexMaskForge, Verbose,
				TEXT("Vertex Mask Forge: AO RawValues cache miss: %s"), *FString::Join(Reasons, TEXT(", ")));
		}
		if (!bValuesStillValid)
		{
			const TArray<FVector> LocalSampleDirs = BuildHemisphereSampleDirections(Params.Samples);
			const UE::Geometry::FDynamicMesh3& WorldMesh = *Cache.WorldMesh;
			const UE::Geometry::FDynamicMeshAABBTree3& Tree = *Cache.Tree;
			const FMatrix NormalMatrix = ComponentTransform.ToMatrixWithScale().Inverse().GetTransposed();

			Cache.RawValues.SetNumUninitialized(NumRenderVerts);

			UE::Geometry::IMeshSpatial::FQueryOptions Options;
			Options.MaxDistance = Params.MaxDistance;

			// AUDITED (parallelization): see the function's own PARALLELIZATION doc comment for the
			// source-level confirmation this is safe. Tree/WorldMesh/RenderTangents/NormalMatrix/
			// LocalSampleDirs/Options are read-only; each worker writes only Cache.RawValues[i].
			ParallelFor(NumRenderVerts, [&](const int32 i)
			{
				const FVector WorldPos = WorldMesh.GetVertex(i);

				const FVector4f LocalNormal4 = RenderTangents.VertexTangentZ(i);
				FVector WorldNormal = NormalMatrix.TransformVector(FVector(LocalNormal4));
				if (!WorldNormal.Normalize())
				{
					// Degenerate render normal (pathological content only): fall back to this
					// vertex's own first incident triangle's geometric normal, never a guess.
					WorldNormal = FVector::UpVector;
					for (const int32 TriID : WorldMesh.VtxTrianglesItr(i))
					{
						WorldNormal = WorldMesh.GetTriNormal(TriID);
						break;
					}
				}

				FVector TangentX, TangentY;
				WorldNormal.FindBestAxisVectors(TangentX, TangentY);

				const FVector Origin = WorldPos + WorldNormal * Params.Bias;

				// AUDITED (sampling-quality fix): position-keyed deterministic scramble -- see
				// ComputeDeterministicScrambleAngle's own doc comment.
				const float ScrambleAngle = ComputeDeterministicScrambleAngle(WorldPos);
				const float CosS = FMath::Cos(ScrambleAngle);
				const float SinS = FMath::Sin(ScrambleAngle);

				int32 NumOccluded = 0;
				int32 NumValidSamples = 0;
				for (const FVector& LocalDir : LocalSampleDirs)
				{
					const float RotatedX = LocalDir.X * CosS - LocalDir.Y * SinS;
					const float RotatedY = LocalDir.X * SinS + LocalDir.Y * CosS;
					const FVector WorldDir = TangentX * RotatedX + TangentY * RotatedY + WorldNormal * LocalDir.Z;
					const FVector NormalizedDir = WorldDir.GetSafeNormal();
					if (NormalizedDir.IsNearlyZero())
					{
						continue;
					}
					++NumValidSamples;

					// AUDITED (self-hit fix, re-examined): any-hit, trusting Bias alone -- see the
					// function's own SELF-HIT doc comment for why FindNearestHitTriangle+epsilon was
					// reverted.
					const FRay3d Ray(FVector3d(Origin), FVector3d(NormalizedDir), /*bDirectionIsNormalized=*/true);
					if (Tree.TestAnyHitTriangle(Ray, Options))
					{
						++NumOccluded;
					}
				}

				const float AO = (NumValidSamples > 0)
					? (static_cast<float>(NumOccluded) / static_cast<float>(NumValidSamples))
					: 0.0f;
				Cache.RawValues[i] = FMath::Clamp(AO, 0.0f, 1.0f);
			});

			Cache.CachedSamples = Params.Samples;
			Cache.CachedMaxDistance = Params.MaxDistance;
			Cache.CachedBias = Params.Bias;
			Cache.bValuesValid = true;

			// Genuine anomaly only (never routine): a raycast pass that produced a NaN or
			// out-of-[0,1] value indicates bad mesh/normal data, not a normal cache event.
			int32 NumNaNOrOutOfRange = 0;
			for (const float RawValue : Cache.RawValues)
			{
				if (!FMath::IsFinite(RawValue) || RawValue < 0.0f || RawValue > 1.0f)
				{
					++NumNaNOrOutOfRange;
				}
			}
			if (NumNaNOrOutOfRange > 0)
			{
				UE_LOG(LogVertexMaskForge, Warning,
					TEXT("Vertex Mask Forge: AO raycast produced %d NaN/out-of-range value(s) out of %d render vert(s) -- check mesh/normal data."),
					NumNaNOrOutOfRange, NumRenderVerts);
			}

			// Explicit operation summary: fires only when a real raycast pass actually ran (never on
			// a RawValues cache hit).
			UE_LOG(LogVertexMaskForge, Log,
				TEXT("Vertex Mask Forge: AO generated: %d vertices, %d samples, %.1f ms"),
				NumRenderVerts, Params.Samples, (FPlatformTime::Seconds() - GenerationStartSeconds) * 1000.0);
		}

		// Populate Mask.Values from the cached raw values, applying Invert here (display/compose
		// contract only -- never touches Cache.RawValues, the tree, or triggers a rebuild of either).
		Mask.Values.SetNumUninitialized(NumRenderVerts);
		Mask.bHasValue.Init(true, NumRenderVerts);

		double Sum = 0.0;
		float MinValue = 1.f;
		float MaxValue = 0.f;
		int32 NumNearZero = 0;
		int32 NumNearOne = 0;

		for (int32 i = 0; i < NumRenderVerts; ++i)
		{
			const float Raw = Cache.RawValues[i];
			const float Value = ApplyAOLevelsAndInvert(Raw, Params.LevelsMin, Params.LevelsMax, Params.bInvert);
			Mask.Values[i] = Value;

			Sum += Value;
			MinValue = FMath::Min(MinValue, Value);
			MaxValue = FMath::Max(MaxValue, Value);
			if (FMath::IsNearlyZero(Value, FVertexMaskForgeScalarMask::Tolerance))
			{
				++NumNearZero;
			}
			if (FMath::IsNearlyEqual(Value, 1.f, FVertexMaskForgeScalarMask::Tolerance))
			{
				++NumNearOne;
			}
		}

		Mask.NumValidValues = NumRenderVerts;
		Mask.MinValue = MinValue;
		Mask.MaxValue = MaxValue;
		Mask.MeanValue = static_cast<float>(Sum / NumRenderVerts);
		Mask.NumNearZero = NumNearZero;
		Mask.NumNearOne = NumNearOne;
		Mask.State = EVertexMaskForgeScalarMaskState::Ready;
		return Mask;
	}

	FVertexMaskForgeScalarMask GenerateAmbientOcclusionMaskFromDynamicMesh(
		TUniquePtr<FVertexMaskForgeSourceTopologyAOCache>& CachePtr,
		const UE::Geometry::FDynamicMesh3& SourceMesh,
		const uint32 SourceGeometryFingerprint,
		const FTransform& ComponentTransform,
		const FVertexMaskForgeAOParams& RawParams)
	{
		using namespace UE::Geometry;

		FVertexMaskForgeScalarMask Mask;
		Mask.Source = EVertexMaskForgeScalarMaskSource::AmbientOcclusion;

		FVertexMaskForgeAOParams Params = RawParams;
		Params.Samples = FMath::Clamp(Params.Samples, 8, 256);
		Params.MaxDistance = FMath::Clamp(Params.MaxDistance, 0.01f, 10000.0f);
		Params.Bias = FMath::Clamp(Params.Bias, 0.001f, 10.0f);
		// AUDITED (AO Levels): clamped independently to [0,1] each -- LevelsMax <= LevelsMin is a valid,
		// deterministic degenerate case (see ApplyAOLevelsAndInvert's own doc comment), not something to
		// reorder or reject here.
		Params.LevelsMin = FMath::Clamp(Params.LevelsMin, 0.0f, 1.0f);
		Params.LevelsMax = FMath::Clamp(Params.LevelsMax, 0.0f, 1.0f);
		Mask.UsedAOParams = Params;

		const FDynamicMeshNormalOverlay* NormalOverlay =
			(SourceMesh.HasAttributes() && SourceMesh.Attributes()->PrimaryNormals() != nullptr)
			? SourceMesh.Attributes()->PrimaryNormals() : nullptr;
		if (!NormalOverlay || NormalOverlay->ElementCount() <= 0
			|| SourceMesh.VertexCount() <= 0 || SourceMesh.TriangleCount() <= 0)
		{
			// EnsureNormalOverlay (called once at working-mesh build time) guarantees a Normal Overlay
			// exists for any Ready working mesh -- reaching here means the mesh itself has no geometry.
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}
		const int32 NumElements = NormalOverlay->MaxElementID();
		Mask.RenderVertexCount = NormalOverlay->ElementCount(); // Domain note: Normal Overlay element count -- see doc comment.

		if (!CachePtr.IsValid())
		{
			CachePtr = MakeUnique<FVertexMaskForgeSourceTopologyAOCache>();
		}
		FVertexMaskForgeSourceTopologyAOCache& Cache = *CachePtr;

		const double GenerationStartSeconds = FPlatformTime::Seconds();

		const bool bMeshMatches = Cache.CachedSourceMesh == &SourceMesh;
		const bool bFingerprintMatches = Cache.CachedGeometryFingerprint == SourceGeometryFingerprint;
		const bool bTransformMatches = Cache.CachedTransform.Equals(ComponentTransform, 1e-5);
		const bool bTreeStillValid = Cache.bTreeValid
			&& bMeshMatches && bFingerprintMatches && bTransformMatches;

		if (Cache.bTreeValid && !bTreeStillValid)
		{
			TArray<FString> Reasons;
			if (!bMeshMatches) { Reasons.Add(TEXT("Source mesh changed")); }
			if (bMeshMatches && !bFingerprintMatches) { Reasons.Add(TEXT("Geometry/normals changed")); }
			if (!bTransformMatches) { Reasons.Add(TEXT("Transform changed")); }
			UE_LOG(LogVertexMaskForge, Verbose,
				TEXT("Vertex Mask Forge: AO (Source Topology) Tree cache miss: %s"), *FString::Join(Reasons, TEXT(", ")));
		}

		if (!bTreeStillValid)
		{
			Cache.WorldMesh = MakeUnique<FDynamicMesh3>();
			Cache.WorldMesh->EnableTriangleGroups();

			// Explicit VertexID -> dense WorldMesh index remap -- never assumes SourceMesh's own
			// VertexIDs are already dense/compact (see the function's own INDEX SAFETY doc note).
			TArray<int32> VertexIdToWorldIndex;
			VertexIdToWorldIndex.Init(INDEX_NONE, SourceMesh.MaxVertexID());
			for (const int32 VertexID : SourceMesh.VertexIndicesItr())
			{
				const FVector WorldPos = ComponentTransform.TransformPosition(FVector(SourceMesh.GetVertex(VertexID)));
				VertexIdToWorldIndex[VertexID] = Cache.WorldMesh->AppendVertex(WorldPos);
			}
			for (const int32 TriangleID : SourceMesh.TriangleIndicesItr())
			{
				const FIndex3i Tri = SourceMesh.GetTriangle(TriangleID);
				Cache.WorldMesh->AppendTriangle(
					VertexIdToWorldIndex[Tri.A], VertexIdToWorldIndex[Tri.B], VertexIdToWorldIndex[Tri.C]);
			}

			Cache.Tree = MakeUnique<FDynamicMeshAABBTree3>(Cache.WorldMesh.Get());
			Cache.CachedSourceMesh = &SourceMesh;
			Cache.CachedGeometryFingerprint = SourceGeometryFingerprint;
			Cache.CachedTransform = ComponentTransform;
			Cache.bTreeValid = true;
			Cache.bValuesValid = false;
		}

		const bool bSamplesMatch = Cache.CachedSamples == Params.Samples;
		const bool bMaxDistanceMatches = FMath::IsNearlyEqual(Cache.CachedMaxDistance, Params.MaxDistance, 1e-4f);
		const bool bBiasMatches = FMath::IsNearlyEqual(Cache.CachedBias, Params.Bias, 1e-6f);
		const bool bValuesStillValid = Cache.bValuesValid && bSamplesMatch && bMaxDistanceMatches && bBiasMatches;

		if (bTreeStillValid && Cache.bValuesValid && !bValuesStillValid)
		{
			TArray<FString> Reasons;
			if (!bSamplesMatch) { Reasons.Add(FString::Printf(TEXT("Samples %d -> %d"), Cache.CachedSamples, Params.Samples)); }
			if (!bMaxDistanceMatches) { Reasons.Add(TEXT("MaxDistance changed")); }
			if (!bBiasMatches) { Reasons.Add(TEXT("Bias changed")); }
			UE_LOG(LogVertexMaskForge, Verbose,
				TEXT("Vertex Mask Forge: AO (Source Topology) RawValues cache miss: %s"), *FString::Join(Reasons, TEXT(", ")));
		}

		if (!bValuesStillValid)
		{
			const TArray<FVector> LocalSampleDirs = BuildHemisphereSampleDirections(Params.Samples);
			const FDynamicMesh3& WorldMesh = *Cache.WorldMesh;
			const FDynamicMeshAABBTree3& Tree = *Cache.Tree;
			// Same non-uniform-scale-correct normal transform GenerateAmbientOcclusionMask itself uses.
			const FMatrix NormalMatrix = ComponentTransform.ToMatrixWithScale().Inverse().GetTransposed();

			// Explicit VertexID -> dense WorldMesh index remap, rebuilt here too (independent of the
			// Tree-rebuild branch above, since a RawValues-only miss -- e.g. Bias changed -- must not
			// require rebuilding WorldMesh/Tree, but still needs this same lookup for Origin positions).
			// AUDITED: WorldMesh's own vertex order was assigned by AppendVertex during the (possibly
			// earlier) Tree-build pass above, in SourceMesh.VertexIndicesItr() order -- rebuilding the
			// same mapping by re-walking that same iterator is deterministic and exact, since
			// AppendVertex returns sequential indices 0,1,2,... in call order.
			TArray<int32> VertexIdToWorldIndex;
			VertexIdToWorldIndex.Init(INDEX_NONE, SourceMesh.MaxVertexID());
			{
				int32 NextWorldIndex = 0;
				for (const int32 VertexID : SourceMesh.VertexIndicesItr())
				{
					VertexIdToWorldIndex[VertexID] = NextWorldIndex++;
				}
			}

			Cache.RawValues.SetNumUninitialized(NumElements);

			IMeshSpatial::FQueryOptions Options;
			Options.MaxDistance = Params.MaxDistance;

			// AUDITED (parallelization): identical safety argument as GenerateAmbientOcclusionMask's
			// own PARALLELIZATION doc comment -- Tree/WorldMesh/LocalSampleDirs/Options/NormalOverlay/
			// NormalMatrix/VertexIdToWorldIndex are read-only; each worker writes only Cache.RawValues[e].
			ParallelFor(NumElements, [&](const int32 ElementID)
			{
				if (!NormalOverlay->IsElement(ElementID))
				{
					// Sparse slot: never written, never read (see TryGetValue/bHasValue on the returned
					// Mask -- this array itself has no "has value" bit, so an unused slot is simply
					// never touched; downstream code only ever reads indices proven IsElement() true).
					return;
				}

				const int32 ParentVertexID = NormalOverlay->GetParentVertex(ElementID);
				const int32 WorldIndex = VertexIdToWorldIndex.IsValidIndex(ParentVertexID) ? VertexIdToWorldIndex[ParentVertexID] : INDEX_NONE;
				if (WorldIndex == INDEX_NONE)
				{
					Cache.RawValues[ElementID] = 0.0f;
					return;
				}

				const FVector WorldPos = WorldMesh.GetVertex(WorldIndex);

				const FVector3f LocalNormal = NormalOverlay->GetElement(ElementID);
				FVector WorldNormal = NormalMatrix.TransformVector(FVector(LocalNormal));
				if (!WorldNormal.Normalize())
				{
					// Degenerate normal (pathological content only): fall back to this vertex's own
					// first incident triangle's geometric normal, never a guess.
					WorldNormal = FVector::UpVector;
					for (const int32 TriID : WorldMesh.VtxTrianglesItr(WorldIndex))
					{
						WorldNormal = WorldMesh.GetTriNormal(TriID);
						break;
					}
				}

				FVector TangentX, TangentY;
				WorldNormal.FindBestAxisVectors(TangentX, TangentY);

				const FVector Origin = WorldPos + WorldNormal * Params.Bias;

				const float ScrambleAngle = ComputeDeterministicScrambleAngle(WorldPos);
				const float CosS = FMath::Cos(ScrambleAngle);
				const float SinS = FMath::Sin(ScrambleAngle);

				int32 NumOccluded = 0;
				int32 NumValidSamples = 0;
				for (const FVector& LocalDir : LocalSampleDirs)
				{
					const float RotatedX = LocalDir.X * CosS - LocalDir.Y * SinS;
					const float RotatedY = LocalDir.X * SinS + LocalDir.Y * CosS;
					const FVector WorldDir = TangentX * RotatedX + TangentY * RotatedY + WorldNormal * LocalDir.Z;
					const FVector NormalizedDir = WorldDir.GetSafeNormal();
					if (NormalizedDir.IsNearlyZero())
					{
						continue;
					}
					++NumValidSamples;

					const FRay3d Ray(FVector3d(Origin), FVector3d(NormalizedDir), /*bDirectionIsNormalized=*/true);
					if (Tree.TestAnyHitTriangle(Ray, Options))
					{
						++NumOccluded;
					}
				}

				const float AO = (NumValidSamples > 0)
					? (static_cast<float>(NumOccluded) / static_cast<float>(NumValidSamples))
					: 0.0f;
				Cache.RawValues[ElementID] = FMath::Clamp(AO, 0.0f, 1.0f);
			});

			Cache.CachedSamples = Params.Samples;
			Cache.CachedMaxDistance = Params.MaxDistance;
			Cache.CachedBias = Params.Bias;
			Cache.bValuesValid = true;

			int32 NumNaNOrOutOfRange = 0;
			for (const int32 ElementID : NormalOverlay->ElementIndicesItr())
			{
				const float RawValue = Cache.RawValues[ElementID];
				if (!FMath::IsFinite(RawValue) || RawValue < 0.0f || RawValue > 1.0f)
				{
					++NumNaNOrOutOfRange;
				}
			}
			if (NumNaNOrOutOfRange > 0)
			{
				UE_LOG(LogVertexMaskForge, Warning,
					TEXT("Vertex Mask Forge: AO (Source Topology) raycast produced %d NaN/out-of-range value(s) out of %d element(s) -- check mesh/normal data."),
					NumNaNOrOutOfRange, NormalOverlay->ElementCount());
			}

			UE_LOG(LogVertexMaskForge, Log,
				TEXT("Vertex Mask Forge: AO (Source Topology) generated: %d normal element(s), %d samples, %.1f ms"),
				NormalOverlay->ElementCount(), Params.Samples, (FPlatformTime::Seconds() - GenerationStartSeconds) * 1000.0);
		}

		Mask.Values.SetNumZeroed(NumElements);
		Mask.bHasValue.Init(false, NumElements);

		double Sum = 0.0;
		float MinValue = 1.f;
		float MaxValue = 0.f;
		int32 NumNearZero = 0;
		int32 NumNearOne = 0;
		int32 NumValid = 0;

		for (const int32 ElementID : NormalOverlay->ElementIndicesItr())
		{
			const float Raw = Cache.RawValues[ElementID];
			const float Value = ApplyAOLevelsAndInvert(Raw, Params.LevelsMin, Params.LevelsMax, Params.bInvert);
			Mask.Values[ElementID] = Value;
			Mask.bHasValue[ElementID] = true;
			++NumValid;

			Sum += Value;
			MinValue = FMath::Min(MinValue, Value);
			MaxValue = FMath::Max(MaxValue, Value);
			if (FMath::IsNearlyZero(Value, FVertexMaskForgeScalarMask::Tolerance))
			{
				++NumNearZero;
			}
			if (FMath::IsNearlyEqual(Value, 1.f, FVertexMaskForgeScalarMask::Tolerance))
			{
				++NumNearOne;
			}
		}

		Mask.NumValidValues = NumValid;
		Mask.MinValue = MinValue;
		Mask.MaxValue = MaxValue;
		Mask.MeanValue = NumValid > 0 ? static_cast<float>(Sum / NumValid) : 0.f;
		Mask.NumNearZero = NumNearZero;
		Mask.NumNearOne = NumNearOne;
		Mask.State = EVertexMaskForgeScalarMaskState::Ready;
		return Mask;
	}
}
