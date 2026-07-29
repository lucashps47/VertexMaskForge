#include "VertexMaskForgeDirectionalNormalGenerator.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "StaticMeshResources.h"
#include "VertexMaskForgeGeneratorUtils.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

namespace
{
	// --- Directional Normal Mask (V2-E) --------------------------------------------------------------

	/** Unreal's own axis convention (X+ Forward, Y+ Right, Z+ Up). Never inferred from bounding box. */
	FVector GetNormalDirectionVector(const EVertexMaskForgeNormalDirection Direction)
	{
		switch (Direction)
		{
		case EVertexMaskForgeNormalDirection::PositiveX: return FVector(1.0, 0.0, 0.0);
		case EVertexMaskForgeNormalDirection::NegativeX: return FVector(-1.0, 0.0, 0.0);
		case EVertexMaskForgeNormalDirection::PositiveY: return FVector(0.0, 1.0, 0.0);
		case EVertexMaskForgeNormalDirection::NegativeY: return FVector(0.0, -1.0, 0.0);
		case EVertexMaskForgeNormalDirection::PositiveZ: return FVector(0.0, 0.0, 1.0);
		case EVertexMaskForgeNormalDirection::NegativeZ:
		default:
			return FVector(0.0, 0.0, -1.0);
		}
	}

	/**
	 * AUDITED (V2-E): the exact per-element formula -- Alignment = clamp(dot(Normal,Direction),-1,1);
	 * AngleDegrees = degrees(acos(Alignment)); OuterAngle = clamp(UserAngle,0,180); EffectiveFalloff =
	 * clamp(UserFalloff,0,OuterAngle); InnerAngle = OuterAngle-EffectiveFalloff. EffectiveFalloff <=
	 * epsilon: hard cutoff (AngleDegrees<=OuterAngle ? 1:0). Otherwise: RawMask =
	 * 1-smoothstep(InnerAngle,OuterAngle,AngleDegrees), smoothstep's own denominator
	 * (OuterAngle-InnerAngle) equals EffectiveFalloff by construction, always > epsilon in this branch --
	 * never a division by zero. Both Normal and Direction are assumed ALREADY unit-length (callers
	 * validate/normalize before calling, per the "never normalize blindly" requirement) -- this function
	 * itself never normalizes, so a non-unit input is a caller bug, not a runtime guess here.
	 */
	float ComputeDirectionalNormalRawValue(const FVector& UnitNormal, const FVector& UnitDirection, const float UserAngle, const float UserFalloff)
	{
		const double Alignment = FMath::Clamp(FVector::DotProduct(UnitNormal, UnitDirection), -1.0, 1.0);
		const double AngleDegrees = FMath::RadiansToDegrees(FMath::Acos(Alignment));

		const float OuterAngle = FMath::Clamp(UserAngle, 0.0f, 180.0f);
		const float EffectiveFalloff = FMath::Clamp(UserFalloff, 0.0f, OuterAngle);

		float RawMask;
		if (EffectiveFalloff <= UE_KINDA_SMALL_NUMBER)
		{
			RawMask = (AngleDegrees <= static_cast<double>(OuterAngle)) ? 1.0f : 0.0f;
		}
		else
		{
			const float InnerAngle = OuterAngle - EffectiveFalloff;
			const float T = FMath::Clamp(static_cast<float>((AngleDegrees - InnerAngle) / EffectiveFalloff), 0.0f, 1.0f);
			const float Smoothstep = T * T * (3.0f - 2.0f * T);
			RawMask = 1.0f - Smoothstep;
		}

		if (!FMath::IsFinite(RawMask))
		{
			RawMask = 0.0f;
		}
		return FMath::Clamp(RawMask, 0.0f, 1.0f);
	}


	/** Applies OutNormalMatrix (see ComputeWorldSpaceNormalMatrix) to LocalNormal and normalizes the
	 *  result -- returns false (never a guessed/fallback vector) if the input fails to normalize
	 *  (degenerate/zero local normal, or a pathological matrix). */
	bool TransformNormalToWorldSpace(const FMatrix& NormalMatrix, const FVector& LocalNormal, FVector& OutWorldNormal)
	{
		FVector WorldNormal = NormalMatrix.TransformVector(LocalNormal);
		// IsFinite checked BEFORE Normalize() -- a pathological (near-singular-in-practice) matrix could
		// produce a non-finite component that Normalize()'s own zero-length check would not catch.
		if (!FMath::IsFinite(WorldNormal.X) || !FMath::IsFinite(WorldNormal.Y) || !FMath::IsFinite(WorldNormal.Z))
		{
			return false;
		}
		if (!WorldNormal.Normalize())
		{
			return false;
		}
		OutWorldNormal = WorldNormal;
		return true;
	}
}

namespace VertexMaskForgeDirectionalNormalGenerator
{
	FVertexMaskForgeScalarMask GenerateDirectionalNormalMask(
		const FStaticMeshLODResources& LOD0,
		const EVertexMaskForgeNormalSpace Space,
		const EVertexMaskForgeNormalDirection Direction,
		const float Angle,
		const float Falloff,
		const float Blur,
		const bool bInvert,
		const FTransform& ComponentTransform)
	{
		FVertexMaskForgeScalarMask Mask;
		Mask.Source = EVertexMaskForgeScalarMaskSource::DirectionalNormal;

		const int32 NumRenderVerts = static_cast<int32>(LOD0.VertexBuffers.PositionVertexBuffer.GetNumVertices());
		Mask.RenderVertexCount = NumRenderVerts;
		if (NumRenderVerts <= 0)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}

		FMatrix WorldNormalMatrix = FMatrix::Identity;
		bool bWorldTransformValid = true;
		if (Space == EVertexMaskForgeNormalSpace::World)
		{
			bWorldTransformValid = VertexMaskForgeWorkingMeshTypes::ComputeWorldSpaceNormalMatrix(ComponentTransform, WorldNormalMatrix);
		}
		if (!bWorldTransformValid)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Invalid;
			return Mask;
		}

		const FVector DirectionVector = GetNormalDirectionVector(Direction);
		const FStaticMeshVertexBuffer& RenderTangents = LOD0.VertexBuffers.StaticMeshVertexBuffer;

		// Pass 1: compute the raw, pre-Blur, pre-Invert value for every render vertex. Blur is a
		// NEIGHBORHOOD operation, so the full raw array must exist before it can run -- see
		// ApplyAdjacencyTopologicalBlur's own doc comment for why this can't be folded into a single loop.
		TArray<float> RawValues;
		TArray<bool> bHasRawValue;
		RawValues.SetNumZeroed(NumRenderVerts);
		bHasRawValue.Init(false, NumRenderVerts);
		for (int32 i = 0; i < NumRenderVerts; ++i)
		{
			const FVector4f LocalNormal4 = RenderTangents.VertexTangentZ(i);
			FVector Normal(LocalNormal4.X, LocalNormal4.Y, LocalNormal4.Z);
			if (!FMath::IsFinite(Normal.X) || !FMath::IsFinite(Normal.Y) || !FMath::IsFinite(Normal.Z) || !Normal.Normalize())
			{
				continue; // Degenerate render normal -- element left unwritten, never guessed.
			}

			if (Space == EVertexMaskForgeNormalSpace::World)
			{
				FVector WorldNormal;
				if (!TransformNormalToWorldSpace(WorldNormalMatrix, Normal, WorldNormal))
				{
					continue;
				}
				Normal = WorldNormal;
			}

			RawValues[i] = ComputeDirectionalNormalRawValue(Normal, DirectionVector, Angle, Falloff);
			bHasRawValue[i] = true;
		}

		// Blur <= 0 is an exact no-op: ApplyAdjacencyTopologicalBlur returns Input unchanged, so the
		// result below is bit-for-bit identical to the pre-Blur V2-E behavior.
		TArray<float> BlurredValues = RawValues;
		if (Blur > 0.0f)
		{
			const TArray<TArray<int32>> Adjacency = VertexMaskForgeGeneratorUtils::BuildRenderVertexAdjacency(LOD0, NumRenderVerts);
			BlurredValues = VertexMaskForgeGeneratorUtils::ApplyAdjacencyTopologicalBlur(Adjacency, RawValues, bHasRawValue, Blur);
		}

		// Pass 2: apply Invert (same order as CurvatureBlur/NoiseBlur -- Blur before Invert) and
		// accumulate stats.
		Mask.Values.SetNumZeroed(NumRenderVerts);
		Mask.bHasValue.Init(false, NumRenderVerts);

		double Sum = 0.0;
		for (int32 i = 0; i < NumRenderVerts; ++i)
		{
			if (!bHasRawValue[i])
			{
				continue;
			}
			const float Final = bInvert ? (1.0f - BlurredValues[i]) : BlurredValues[i];

			Mask.Values[i] = Final;
			Mask.bHasValue[i] = true;
			++Mask.NumValidValues;
			Sum += Final;
			Mask.MinValue = (Mask.NumValidValues == 1) ? Final : FMath::Min(Mask.MinValue, Final);
			Mask.MaxValue = (Mask.NumValidValues == 1) ? Final : FMath::Max(Mask.MaxValue, Final);
			if (Final <= FVertexMaskForgeScalarMask::Tolerance) { ++Mask.NumNearZero; }
			if (Final >= 1.0f - FVertexMaskForgeScalarMask::Tolerance) { ++Mask.NumNearOne; }
		}
		Mask.MeanValue = (Mask.NumValidValues > 0) ? static_cast<float>(Sum / Mask.NumValidValues) : 0.0f;
		Mask.State = (Mask.NumValidValues > 0) ? EVertexMaskForgeScalarMaskState::Ready : EVertexMaskForgeScalarMaskState::Unavailable;

		return Mask;
	}

	FVertexMaskForgeScalarMask GenerateDirectionalNormalMaskFromDynamicMesh(
		const FVertexMaskForgeWorkingMesh& WorkingMesh,
		const EVertexMaskForgeNormalSpace Space,
		const EVertexMaskForgeNormalDirection Direction,
		const float Angle,
		const float Falloff,
		const float Blur,
		const bool bInvert,
		const FTransform& ComponentTransform)
	{
		using namespace UE::Geometry;

		FVertexMaskForgeScalarMask Mask;
		Mask.Source = EVertexMaskForgeScalarMaskSource::DirectionalNormal;

		if (!WorkingMesh.Mesh.IsValid())
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}
		const FDynamicMesh3& Mesh = *WorkingMesh.Mesh;
		const int32 NumCorners = Mesh.TriangleCount() * 3;
		Mask.RenderVertexCount = NumCorners;
		if (NumCorners <= 0)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}

		const FDynamicMeshNormalOverlay* NormalOverlay =
			(Mesh.HasAttributes() && Mesh.Attributes()->PrimaryNormals() != nullptr)
			? Mesh.Attributes()->PrimaryNormals() : nullptr;
		if (!NormalOverlay || NormalOverlay->ElementCount() <= 0)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}

		FMatrix WorldNormalMatrix = FMatrix::Identity;
		bool bWorldTransformValid = true;
		if (Space == EVertexMaskForgeNormalSpace::World)
		{
			bWorldTransformValid = VertexMaskForgeWorkingMeshTypes::ComputeWorldSpaceNormalMatrix(ComponentTransform, WorldNormalMatrix);
		}
		if (!bWorldTransformValid)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Invalid;
			return Mask;
		}

		const FVector DirectionVector = GetNormalDirectionVector(Direction);

		// Pass 1: compute the raw, pre-Blur, pre-Invert value for every corner (see
		// GenerateDirectionalNormalMask's identical two-pass rationale).
		TArray<float> RawValues;
		TArray<bool> bHasRawValue;
		RawValues.SetNumZeroed(NumCorners);
		bHasRawValue.Init(false, NumCorners);
		{
			int32 CornerIndex = 0;
			for (const int32 TriangleID : Mesh.TriangleIndicesItr())
			{
				const FIndex3i NormalTri = NormalOverlay->IsSetTriangle(TriangleID)
					? NormalOverlay->GetTriangle(TriangleID)
					: FIndex3i(INDEX_NONE, INDEX_NONE, INDEX_NONE);

				for (int32 Corner = 0; Corner < 3; ++Corner, ++CornerIndex)
				{
					const int32 ElementID = NormalTri[Corner];
					if (ElementID == INDEX_NONE || !NormalOverlay->IsElement(ElementID))
					{
						continue;
					}
					const FVector3f LocalNormal3f = NormalOverlay->GetElement(ElementID);
					FVector Normal(LocalNormal3f.X, LocalNormal3f.Y, LocalNormal3f.Z);
					if (!FMath::IsFinite(Normal.X) || !FMath::IsFinite(Normal.Y) || !FMath::IsFinite(Normal.Z) || !Normal.Normalize())
					{
						continue;
					}

					if (Space == EVertexMaskForgeNormalSpace::World)
					{
						FVector WorldNormal;
						if (!TransformNormalToWorldSpace(WorldNormalMatrix, Normal, WorldNormal))
						{
							continue;
						}
						Normal = WorldNormal;
					}

					RawValues[CornerIndex] = ComputeDirectionalNormalRawValue(Normal, DirectionVector, Angle, Falloff);
					bHasRawValue[CornerIndex] = true;
				}
			}
		}

		// Blur <= 0 is an exact no-op (see ApplyAdjacencyTopologicalBlur).
		TArray<float> BlurredValues = RawValues;
		if (Blur > 0.0f)
		{
			const TArray<TArray<int32>> Adjacency = VertexMaskForgeGeneratorUtils::BuildCornerAdjacency(Mesh, NormalOverlay, NumCorners);
			BlurredValues = VertexMaskForgeGeneratorUtils::ApplyAdjacencyTopologicalBlur(Adjacency, RawValues, bHasRawValue, Blur);
		}

		// Pass 2: apply Invert (Blur before Invert, matching CurvatureBlur/NoiseBlur) and accumulate stats.
		Mask.Values.SetNumZeroed(NumCorners);
		Mask.bHasValue.Init(false, NumCorners);

		double Sum = 0.0;
		for (int32 CornerIndex = 0; CornerIndex < NumCorners; ++CornerIndex)
		{
			if (!bHasRawValue[CornerIndex])
			{
				continue;
			}
			const float Final = bInvert ? (1.0f - BlurredValues[CornerIndex]) : BlurredValues[CornerIndex];

			Mask.Values[CornerIndex] = Final;
			Mask.bHasValue[CornerIndex] = true;
			++Mask.NumValidValues;
			Sum += Final;
			Mask.MinValue = (Mask.NumValidValues == 1) ? Final : FMath::Min(Mask.MinValue, Final);
			Mask.MaxValue = (Mask.NumValidValues == 1) ? Final : FMath::Max(Mask.MaxValue, Final);
			if (Final <= FVertexMaskForgeScalarMask::Tolerance) { ++Mask.NumNearZero; }
			if (Final >= 1.0f - FVertexMaskForgeScalarMask::Tolerance) { ++Mask.NumNearOne; }
		}
		Mask.MeanValue = (Mask.NumValidValues > 0) ? static_cast<float>(Sum / Mask.NumValidValues) : 0.0f;
		Mask.State = (Mask.NumValidValues > 0) ? EVertexMaskForgeScalarMaskState::Ready : EVertexMaskForgeScalarMaskState::Unavailable;

		return Mask;
	}
}
