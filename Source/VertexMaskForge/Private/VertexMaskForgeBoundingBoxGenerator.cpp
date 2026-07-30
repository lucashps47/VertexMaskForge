#include "VertexMaskForgeBoundingBoxGenerator.h"

#include "Components/StaticMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "VertexMaskForgeWorkingMeshTypes.h"
#include "VertexMaskForgeWorkingStateOwner.h"

#define LOCTEXT_NAMESPACE "SVertexMaskForgePanel"

namespace
{
	// --- Bounding Box Mask (Local X / Local Y / Local Z, each Local- or World-Space, Mirror) -----

	/** Selects the coordinate of P along Axis. */
	float GetAxisCoordinate(const FVector3f& P, const EVertexMaskForgeBoundsAxis Axis)
	{
		switch (Axis)
		{
		case EVertexMaskForgeBoundsAxis::X:
			return P.X;
		case EVertexMaskForgeBoundsAxis::Y:
			return P.Y;
		case EVertexMaskForgeBoundsAxis::Z:
		default:
			return P.Z;
		}
	}

	/**
	 * The already-validated Local Z base-gradient formula (unchanged), now shared by every axis:
	 *   Lower = Position - SafeTransitionWidth * 0.5
	 *   Gradient = clamp((T - Lower) / SafeTransitionWidth, 0, 1)
	 * T is a normalized coordinate in [0,1] (see GenerateBoundingBoxMask); NOT Invert -- that is
	 * applied by the caller to this function's result. Mirror (also applied by the caller) does NOT
	 * call this function twice and take a maximum (that compressed the achievable range -- see the
	 * audit note at the Mirror call site); it instead remaps T itself into a symmetric "tent" domain
	 * BEFORE this single call, so this function's own formula and meaning are unchanged either way.
	 */
	float EvaluateAxisBaseGradient(const float T, const float Position, const float SafeTransitionWidth)
	{
		const float Lower = Position - SafeTransitionWidth * 0.5f;
		return FMath::Clamp((T - Lower) / SafeTransitionWidth, 0.f, 1.f);
	}

#if !UE_BUILD_SHIPPING
	/**
	 * One-time runtime sanity check (non-shipping builds only) of the Mirror remap's symmetry, per
	 * the explicit checkpoint requirement: Mask(0)==Mask(1), Mask(0.25)==Mask(0.75), and the full
	 * 0-1 range is reached (Mask(0)==0, Mask(0.5)==1) at the representative default Position=0.5,
	 * TransitionWidth=1.0. Purely diagnostic (UE_LOG only); never affects composition or generation.
	 * Runs once (guarded by a static bool), the first time GenerateBoundingBoxMask processes an
	 * enabled Mirror axis.
	 */
	void VerifyMirrorSymmetryOnce()
	{
		static bool bVerified = false;
		if (bVerified)
		{
			return;
		}
		bVerified = true;

		constexpr float Position = 0.5f;
		constexpr float TransitionWidth = 1.0f;
		const float SampleTs[5] = { 0.f, 0.25f, 0.5f, 0.75f, 1.f };
		float Values[5];
		for (int32 i = 0; i < 5; ++i)
		{
			const float MirroredT = 1.f - FMath::Abs(2.f * SampleTs[i] - 1.f);
			Values[i] = EvaluateAxisBaseGradient(MirroredT, Position, TransitionWidth);
		}

		constexpr float Tolerance = 1e-4f;
		const bool bEdgesMatch = FMath::IsNearlyEqual(Values[0], Values[4], Tolerance);
		const bool bQuartersMatch = FMath::IsNearlyEqual(Values[1], Values[3], Tolerance);
		const bool bFullRange = FMath::IsNearlyEqual(Values[0], 0.f, Tolerance) && FMath::IsNearlyEqual(Values[2], 1.f, Tolerance);

		if (!bEdgesMatch || !bQuartersMatch || !bFullRange)
		{
			UE_LOG(LogVertexMaskForge, Warning,
				TEXT("Vertex Mask Forge: Mirror symmetry self-check FAILED -- Mask(0)=%.4f Mask(0.25)=%.4f Mask(0.5)=%.4f Mask(0.75)=%.4f Mask(1)=%.4f"),
				Values[0], Values[1], Values[2], Values[3], Values[4]);
		}
		else
		{
			UE_LOG(LogVertexMaskForge, Verbose,
				TEXT("Vertex Mask Forge: Mirror symmetry self-check passed -- Mask(0)=%.4f Mask(0.25)=%.4f Mask(0.5)=%.4f Mask(0.75)=%.4f Mask(1)=%.4f"),
				Values[0], Values[1], Values[2], Values[3], Values[4]);
		}
	}
#endif

	/** The Static Mesh's own unit local axis vector for Axis: (1,0,0) / (0,1,0) / (0,0,1). */
	FVector GetLocalAxisUnitVector(const EVertexMaskForgeBoundsAxis Axis)
	{
		switch (Axis)
		{
		case EVertexMaskForgeBoundsAxis::X:
			return FVector(1.0, 0.0, 0.0);
		case EVertexMaskForgeBoundsAxis::Y:
			return FVector(0.0, 1.0, 0.0);
		case EVertexMaskForgeBoundsAxis::Z:
		default:
			return FVector(0.0, 0.0, 1.0);
		}
	}

	/**
	 * AUDITED (Unified Bounds + Local Space fix): for ONE enabled Local-space axis with more than
	 * one participating component, resolves a SHARED LOCAL AXIS -- the world-space direction and
	 * scale that every participant's own local Axis maps to -- so Unified Local can preserve
	 * translation between instances (see ResolveAxisCoordinate) while still tracking the meshes'
	 * shared orientation rather than the world's fixed XYZ.
	 *
	 * Compatibility is checked on the ACTUAL TRANSFORMED AXIS VECTOR
	 * (ParticipantTransforms[i].TransformVector(UnitAxis)) -- not on the whole Rotation/Scale
	 * generically -- so a mismatch on an axis nobody cares about (e.g. differing X scale) never
	 * blocks Unified Local on a different, actually-compatible axis (e.g. Z). Comparing the vectors
	 * directly also inherently rejects "approximately parallel but flipped" cases (a vector and its
	 * negation differ by 2x their length, always outside any reasonable tolerance), satisfying the
	 * "não aceite eixos... com sentido invertido incompatível" requirement without special-casing it.
	 *
	 * Deterministic regardless of selection order: every participant is compared against
	 * participant[0] purely as a symmetric equivalence check (if all equal the first, all are
	 * mutually equal by transitivity, so the SAME shared vector -- within tolerance -- results
	 * regardless of which participant happened to be first).
	 */
	bool ResolveSharedLocalAxis(
		const TArray<FTransform>& ParticipantTransforms,
		const EVertexMaskForgeBoundsAxis Axis,
		const TCHAR* AxisName,
		FVector& OutSharedDirection,
		double& OutSharedScale,
		FText& OutErrorText)
	{
		check(ParticipantTransforms.Num() > 0);

		const FVector UnitAxis = GetLocalAxisUnitVector(Axis);
		const FVector ReferenceVector = ParticipantTransforms[0].TransformVector(UnitAxis);
		const double ReferenceLength = ReferenceVector.Size();

		if (!FMath::IsFinite(ReferenceLength) || ReferenceLength <= UE_DOUBLE_SMALL_NUMBER)
		{
			OutErrorText = FText::Format(
				LOCTEXT("UnifiedBoundsLocalDegenerateAxisFormat",
					"Unified Bounds: Local {0} axis has zero scale on one or more selected instances."),
				FText::FromString(AxisName));
			return false;
		}

		// Relative tolerance (proportional to the reference vector's own length) covers both
		// direction and magnitude (scale) in one comparison -- a vector differing in orientation OR
		// in scale from the reference both fail Equals() at this tolerance.
		constexpr double RelativeTolerance = 1e-3;
		const double AbsoluteTolerance = RelativeTolerance * ReferenceLength;

		for (int32 i = 1; i < ParticipantTransforms.Num(); ++i)
		{
			const FVector Vector = ParticipantTransforms[i].TransformVector(UnitAxis);
			if (!Vector.Equals(ReferenceVector, AbsoluteTolerance))
			{
				OutErrorText = FText::Format(
					LOCTEXT("UnifiedBoundsLocalIncompatibleFormat",
						"Unified Bounds requires compatible Local {0} axes. Enable World Space for {0}."),
					FText::FromString(AxisName));
				return false;
			}
		}

		OutSharedDirection = ReferenceVector.GetSafeNormal();
		OutSharedScale = ReferenceLength;
		return true;
	}

	/**
	 * THE single shared coordinate resolver, used identically by Phase A
	 * (ComputeCollectiveAxisBounds, collecting CollectiveMin/Max) and Phase B
	 * (GenerateBoundingBoxMask, evaluating every render vertex) -- never two separate
	 * implementations, per the explicit requirement that Phase A and Phase B must never operate in
	 * different spaces.
	 *
	 * AUDITED (root cause of Unified Local ignoring instance translation): previously, an enabled
	 * Local-space axis always read LocalPosition[Axis] directly, in BOTH Individual and Unified
	 * modes. LocalPosition is the Static Mesh ASSET's own object-space coordinate -- it has no
	 * notion of "where this component is placed" at all, so two instances of the SAME asset always
	 * produced the exact same raw local coordinate range regardless of how far apart they actually
	 * are in the level. Collecting/evaluating that way inevitably collapsed every instance onto the
	 * same [0, AssetExtent] range instead of composing their real relative placement.
	 *
	 * Four cases now:
	 *   - Individual Local (bWorldSpace=false, bUseUnifiedBounds=false): LocalPosition[Axis]
	 *     unchanged -- exactly preserves the already-validated single-mesh behavior; translation
	 *     between components never participates (there is no "between components" in this mode).
	 *   - Individual World / Unified World (bWorldSpace=true): WorldPosition[Axis], where
	 *     WorldPosition = ComponentTransform.TransformPosition(LocalPosition) -- the full affine
	 *     transform (translation+rotation+scale), unchanged from before.
	 *   - Unified Local (bWorldSpace=false, bUseUnifiedBounds=true): WorldPosition is still computed
	 *     (so translation between instances DOES participate), then projected onto the SHARED local
	 *     axis direction (SharedLocalAxisDirection, resolved once by ResolveSharedLocalAxis and
	 *     validated compatible across every participant) and divided by SharedLocalAxisScale. This
	 *     is what lets Local orientation stay meaningfully different from World orientation (a
	 *     rotated selection's "Local Z" still follows the meshes' own shared up-axis, not the
	 *     world's), while still composing translation between instances correctly.
	 * The absolute origin used for the dot product is arbitrary and does not need to be subtracted
	 * out: T = (Coord - Min) / (Max - Min) is invariant to any additive offset applied uniformly to
	 * every Coord, Min, and Max alike, so introducing an arbitrary pivot would add complexity without
	 * changing the result.
	 */
	double ResolveAxisCoordinate(
		const FVector3f& LocalPosition,
		const FTransform& ComponentTransform,
		const EVertexMaskForgeBoundsAxis Axis,
		const bool bWorldSpace,
		const bool bUseUnifiedBounds,
		const FVector& SharedLocalAxisDirection,
		const double SharedLocalAxisScale)
	{
		if (!bWorldSpace && !bUseUnifiedBounds)
		{
			return static_cast<double>(GetAxisCoordinate(LocalPosition, Axis));
		}

		const FVector WorldPosition = ComponentTransform.TransformPosition(FVector(LocalPosition));

		if (bWorldSpace)
		{
			return static_cast<double>(GetAxisCoordinate(FVector3f(WorldPosition), Axis));
		}

		// Unified Local.
		return FVector::DotProduct(WorldPosition, SharedLocalAxisDirection) / SharedLocalAxisScale;
	}
}

namespace VertexMaskForgeBoundingBoxGenerator
{
	FVertexMaskForgeScalarMask GenerateBoundingBoxMask(
		const FStaticMeshLODResources& LOD0,
		const TStaticArray<FVertexMaskForgeAxisMaskParams, 3>& AxisParams,
		const FTransform& ComponentTransform,
		const TStaticArray<FVertexMaskForgeAxisBoundsResult, 3>* CollectiveBounds)
	{
		FVertexMaskForgeScalarMask Mask;
		Mask.Source = EVertexMaskForgeScalarMaskSource::BoundingBox;
		Mask.UsedAxisParams = AxisParams;
		Mask.bUnifiedBounds = (CollectiveBounds != nullptr);

		const FPositionVertexBuffer& RenderPositions = LOD0.VertexBuffers.PositionVertexBuffer;
		const int32 NumRenderVerts = static_cast<int32>(RenderPositions.GetNumVertices());
		Mask.RenderVertexCount = NumRenderVerts;

		if (NumRenderVerts <= 0)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}

		bool bAnyAxisEnabled = false;
		for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			if (AxisParams[AxisIndex].bEnabled)
			{
				bAnyAxisEnabled = true;
				break;
			}
		}
		if (!bAnyAxisEnabled)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}

#if !UE_BUILD_SHIPPING
		for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			if (AxisParams[AxisIndex].bEnabled && AxisParams[AxisIndex].bMirror)
			{
				VerifyMirrorSymmetryOnce();
				break;
			}
		}
#endif

		// Pass 1 (per enabled axis, in that axis's own chosen space): bounds over EVERY render
		// vertex -- the global bounding box of the whole LOD in that space, not per-piece, and never
		// approximated from just the local AABB corners for World Space (see the function doc).
		//
		// AUDITED (Unified Bounds): if CollectiveBounds is supplied (non-null), it was already fully
		// computed and validated by ComputeCollectiveAxisBounds() across every participating
		// component BEFORE this call -- skip this mesh's own individual bounds pass entirely and use
		// the shared collective domain instead. This is the ONLY difference between Individual and
		// Unified Bounds: everything from here on (normalization, Mirror, Invert, clamp, axis
		// combination, composition) is the exact same code path regardless of which bounds were used.
		TStaticArray<FVertexMaskForgeAxisBoundsResult, 3> IndividualBounds;
		const TStaticArray<FVertexMaskForgeAxisBoundsResult, 3>& AxisBounds = CollectiveBounds ? *CollectiveBounds : IndividualBounds;

		constexpr double MinExtent = 1e-5;

		if (!CollectiveBounds)
		{
			for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
			{
				if (!AxisParams[AxisIndex].bEnabled)
				{
					continue;
				}
				const EVertexMaskForgeBoundsAxis Axis = static_cast<EVertexMaskForgeBoundsAxis>(AxisIndex);
				const bool bWorldSpace = AxisParams[AxisIndex].bWorldSpace;
				FVertexMaskForgeAxisBoundsResult& BoundsResult = IndividualBounds[AxisIndex];
				BoundsResult.MinCoord = TNumericLimits<double>::Max();
				BoundsResult.MaxCoord = TNumericLimits<double>::Lowest();

				for (int32 i = 0; i < NumRenderVerts; ++i)
				{
					const FVector3f LocalPosition = RenderPositions.VertexPosition(i);
					// bUseUnifiedBounds=false: Individual bounds -- Local reads LocalPosition[Axis]
					// directly (unchanged, single-mesh behavior), World reads WorldPosition[Axis].
					// SharedLocalAxisDirection/Scale are unused in this branch (Individual mode never
					// has a "shared" axis -- there is only one component).
					const double Coord = ResolveAxisCoordinate(
						LocalPosition, ComponentTransform, Axis, bWorldSpace, /*bUseUnifiedBounds=*/false,
						FVector::ZeroVector, 1.0);
					BoundsResult.MinCoord = FMath::Min(BoundsResult.MinCoord, Coord);
					BoundsResult.MaxCoord = FMath::Max(BoundsResult.MaxCoord, Coord);
				}

				const double Extent = BoundsResult.MaxCoord - BoundsResult.MinCoord;
				if (!FMath::IsFinite(Extent) || Extent <= MinExtent)
				{
					BoundsResult.bDegenerate = true;
				}
			}
		}

		for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			if (AxisParams[AxisIndex].bEnabled && AxisBounds[AxisIndex].bDegenerate)
			{
				Mask.State = EVertexMaskForgeScalarMaskState::DegenerateBounds;
				return Mask;
			}
		}

		// Dense by construction: render vertex indices are already compact (0..NumRenderVerts-1), so
		// every slot is written below.
		Mask.Values.SetNumZeroed(NumRenderVerts);
		Mask.bHasValue.Init(true, NumRenderVerts);

		double Sum = 0.0;
		float MinValue = 1.f;
		float MaxValue = 0.f;
		int32 NumNearZero = 0;
		int32 NumNearOne = 0;
		bool bAllFinite = true;
		bool bAllInRange = true;

		for (int32 i = 0; i < NumRenderVerts; ++i)
		{
			const FVector3f LocalPosition = RenderPositions.VertexPosition(i);

			// Combine every enabled axis's own AxisMask by maximum -- each axis fully completes its
			// own Local/World selection, bounds, normalization, Mirror, Invert, and clamp BEFORE
			// contributing to CombinedMask (see the function doc for the exact order).
			float CombinedMask = 0.f;
			for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
			{
				const FVertexMaskForgeAxisMaskParams& Params = AxisParams[AxisIndex];
				if (!Params.bEnabled)
				{
					continue;
				}
				const EVertexMaskForgeBoundsAxis Axis = static_cast<EVertexMaskForgeBoundsAxis>(AxisIndex);
				const FVertexMaskForgeAxisBoundsResult& BoundsResult = AxisBounds[AxisIndex];

				// AUDITED (Unified Bounds + Local Space fix): SAME resolver as Phase A
				// (ComputeCollectiveAxisBounds) and as Phase 1 just above -- bUseUnifiedBounds mirrors
				// whether CollectiveBounds was supplied to this call at all (Unified Local needs the
				// SharedLocalAxisDirection/Scale that Phase A already resolved and stored on
				// BoundsResult; Individual Local/either World branch ignore them).
				const double Coord = ResolveAxisCoordinate(
					LocalPosition, ComponentTransform, Axis, Params.bWorldSpace, /*bUseUnifiedBounds=*/CollectiveBounds != nullptr,
					BoundsResult.SharedLocalAxisDirection, BoundsResult.SharedLocalAxisScale);

				const double Extent = BoundsResult.MaxCoord - BoundsResult.MinCoord;
				const float T = static_cast<float>((Coord - BoundsResult.MinCoord) / Extent);

				// Epsilon guard against a zero (or near-zero) Transition Width, per the checkpoint spec.
				const float SafeTransitionWidth = FMath::Max(Params.TransitionWidth, 1e-4f);

				// AUDITED (Mirror normalization fix): the original formula --
				// max(EvaluateAxisBaseGradient(T,...), EvaluateAxisBaseGradient(1-T,...)) -- evaluates
				// EvaluateAxisBaseGradient() TWICE, each still across the FULL [0,1] domain, then takes
				// their maximum. Since EvaluateAxisBaseGradient() is monotonically non-decreasing in
				// its first argument, for any T the larger of {T, 1-T} is always >= 0.5, so the
				// maximum's own MINIMUM (at T=0.5, where both arguments equal 0.5) is
				// EvaluateAxisBaseGradient(0.5, Position, Width) -- 0.5 with the default Position=0.5/
				// Width=1.0 -- and it never goes lower. The whole result is therefore compressed into
				// [EvaluateAxisBaseGradient(0.5,...), 1], never reaching the low end of 0-1 the way the
				// non-Mirror gradient does.
				//
				// Fix: remap T itself into a "tent" domain that ALREADY spans the full 0-1 range
				// within EACH half, then evaluate EvaluateAxisBaseGradient() exactly ONCE on that
				// remapped coordinate (not twice, not maxed) -- MirroredT = 1 - |2T - 1|. MirroredT is
				// 0 at T=0 and T=1 (the two edges), rises to 1 at T=0.5 (the center), and is exactly
				// symmetric about T=0.5 by construction, so:
				//   - Mask(0) == Mask(1) (both use MirroredT=0);
				//   - Mask(0.25) == Mask(0.75) (both use MirroredT=0.5);
				//   - each half traverses the FULL 0-1 range of EvaluateAxisBaseGradient(), matching
				//     the non-Mirror gradient's own amplitude exactly, just folded at the center;
				//   - continuous at the center (MirroredT peaks smoothly at T=0.5, no value jump).
				// Position/TransitionWidth keep their existing meaning: they still shape a single
				// EvaluateAxisBaseGradient() curve, now over the tent-shaped domain instead of T
				// directly -- Position shifts where the transition sits between center and edge,
				// TransitionWidth still controls how sharp that transition is.
				const float EvaluationT = Params.bMirror ? (1.f - FMath::Abs(2.f * T - 1.f)) : T;
				float AxisMask = EvaluateAxisBaseGradient(EvaluationT, Params.Position, SafeTransitionWidth);

				if (Params.bInvert)
				{
					AxisMask = 1.f - AxisMask;
				}
				AxisMask = FMath::Clamp(AxisMask, 0.f, 1.f);

				CombinedMask = FMath::Max(CombinedMask, AxisMask);
			}

			if (!FMath::IsFinite(CombinedMask))
			{
				bAllFinite = false;
			}
			if (CombinedMask < 0.f || CombinedMask > 1.f)
			{
				bAllInRange = false;
			}

			Mask.Values[i] = CombinedMask;

			Sum += CombinedMask;
			MinValue = FMath::Min(MinValue, CombinedMask);
			MaxValue = FMath::Max(MaxValue, CombinedMask);

			if (FMath::IsNearlyZero(CombinedMask, FVertexMaskForgeScalarMask::Tolerance))
			{
				++NumNearZero;
			}
			if (FMath::IsNearlyEqual(CombinedMask, 1.f, FVertexMaskForgeScalarMask::Tolerance))
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

		// Integrity checks: never silently hide inconsistent output. The mandatory invariant
		// (Mask.Values.Num() == PositionVertexBuffer.GetNumVertices()) is enforced by construction
		// above (dense SetNumZeroed(NumRenderVerts)); NumValidValues == RenderVertexCount is checked
		// explicitly here as well so a future edit that reintroduces sparsity is caught immediately.
		if (!bAllFinite || !bAllInRange || Mask.NumValidValues != NumRenderVerts || Mask.Values.Num() != NumRenderVerts)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Invalid;
			return Mask;
		}

		Mask.State = EVertexMaskForgeScalarMaskState::Ready;
		return Mask;
	}

	bool ComputeCollectiveAxisBounds(
		const TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& SelectedMeshes,
		const TStaticArray<FVertexMaskForgeAxisMaskParams, 3>& AxisParams,
		const bool bForGeneration,
		TStaticArray<FVertexMaskForgeAxisBoundsResult, 3>& OutBounds,
		FText& OutErrorText)
	{
		OutBounds = TStaticArray<FVertexMaskForgeAxisBoundsResult, 3>();

		struct FParticipant
		{
			const FStaticMeshLODResources* LOD0 = nullptr;
			FTransform Transform;
		};
		TArray<FParticipant> Participants;

		for (const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry : SelectedMeshes)
		{
			if (!Entry.IsValid())
			{
				continue;
			}

			const bool bParticipates = bForGeneration
				? (Entry->MeshOwner->GetWorkingMesh().State == EVertexMaskForgeWorkingMeshState::Ready)
				: (Entry->GeneratorState.BoundingBoxMask.Source == EVertexMaskForgeScalarMaskSource::BoundingBox
					&& Entry->GeneratorState.BoundingBoxMask.State == EVertexMaskForgeScalarMaskState::Ready);
			if (!bParticipates)
			{
				continue;
			}

			const UStaticMesh* Mesh = Entry->Mesh.LoadSynchronous();
			if (!IsValid(Mesh) || !Mesh->HasValidRenderData(/*bCheckLODForVerts=*/true, /*LODIndex=*/0))
			{
				continue;
			}
			const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
			if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
			{
				continue;
			}

			for (const TUniquePtr<FVertexMaskForgeWorkingStateOwner>& StateOwner : Entry->PreviewComponents)
			{
				const UStaticMeshComponent* SourceComponent = StateOwner->GetPreviewState().GetSourceComponent().Get();
				if (!IsValid(SourceComponent))
				{
					continue;
				}

				FParticipant P;
				P.LOD0 = &RenderData->LODResources[0];
				P.Transform = SourceComponent->GetComponentTransform();
				Participants.Add(P);
			}
		}

		if (Participants.IsEmpty())
		{
			OutErrorText = LOCTEXT("UnifiedBoundsNoParticipants", "Unified Bounds: no eligible components to collect a collective domain from.");
			return false;
		}

		static const TCHAR* AxisNames[3] = { TEXT("X"), TEXT("Y"), TEXT("Z") };
		constexpr double MinExtent = 1e-5;

		for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			if (!AxisParams[AxisIndex].bEnabled)
			{
				continue;
			}
			const EVertexMaskForgeBoundsAxis Axis = static_cast<EVertexMaskForgeBoundsAxis>(AxisIndex);
			const bool bWorldSpace = AxisParams[AxisIndex].bWorldSpace;

			FVertexMaskForgeAxisBoundsResult& BoundsResult = OutBounds[AxisIndex];
			BoundsResult.MinCoord = TNumericLimits<double>::Max();
			BoundsResult.MaxCoord = TNumericLimits<double>::Lowest();

			// AUDITED (Unified Bounds + Local Space fix): resolve the SHARED local axis (direction +
			// scale) ONCE per axis here in Phase A, store it on BoundsResult, and use it verbatim in
			// Phase B (GenerateBoundingBoxMask) via the same ResolveAxisCoordinate() call -- Phase A
			// and Phase B must never operate in different spaces.
			if (!bWorldSpace)
			{
				TArray<FTransform> ParticipantTransforms;
				ParticipantTransforms.Reserve(Participants.Num());
				for (const FParticipant& P : Participants)
				{
					ParticipantTransforms.Add(P.Transform);
				}
				if (!ResolveSharedLocalAxis(
					ParticipantTransforms, Axis, AxisNames[AxisIndex],
					BoundsResult.SharedLocalAxisDirection, BoundsResult.SharedLocalAxisScale, OutErrorText))
				{
					return false;
				}
			}

			for (const FParticipant& P : Participants)
			{
				const FPositionVertexBuffer& RenderPositions = P.LOD0->VertexBuffers.PositionVertexBuffer;
				const int32 NumRenderVerts = static_cast<int32>(RenderPositions.GetNumVertices());
				for (int32 i = 0; i < NumRenderVerts; ++i)
				{
					const FVector3f LocalPosition = RenderPositions.VertexPosition(i);
					const double Coord = ResolveAxisCoordinate(
						LocalPosition, P.Transform, Axis, bWorldSpace, /*bUseUnifiedBounds=*/true,
						BoundsResult.SharedLocalAxisDirection, BoundsResult.SharedLocalAxisScale);
					BoundsResult.MinCoord = FMath::Min(BoundsResult.MinCoord, Coord);
					BoundsResult.MaxCoord = FMath::Max(BoundsResult.MaxCoord, Coord);
				}
			}

			const double Extent = BoundsResult.MaxCoord - BoundsResult.MinCoord;
			if (!FMath::IsFinite(Extent) || Extent <= MinExtent)
			{
				BoundsResult.bDegenerate = true;
			}
		}

		for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			if (AxisParams[AxisIndex].bEnabled && OutBounds[AxisIndex].bDegenerate)
			{
				OutErrorText = FText::Format(
					LOCTEXT("UnifiedBoundsDegenerateFormat",
						"Unified Bounds: the collective {0} extent across the selection is too small to normalize safely."),
					FText::FromString(AxisNames[AxisIndex]));
				return false;
			}
		}

		return true;
	}

	FVertexMaskForgeScalarMask GenerateBoundingBoxMaskFromDynamicMesh(
		const UE::Geometry::FDynamicMesh3& SourceMesh,
		const TStaticArray<FVertexMaskForgeAxisMaskParams, 3>& AxisParams,
		const FTransform& ComponentTransform)
	{
		using namespace UE::Geometry;

		FVertexMaskForgeScalarMask Mask;
		Mask.Source = EVertexMaskForgeScalarMaskSource::BoundingBox;
		Mask.UsedAxisParams = AxisParams;
		Mask.bUnifiedBounds = false; // Never Unified in this domain -- see the function's own doc comment.

		if (SourceMesh.VertexCount() <= 0)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}
		const int32 NumVerts = SourceMesh.VertexCount();
		const int32 ArraySize = SourceMesh.MaxVertexID();
		Mask.RenderVertexCount = NumVerts; // Domain note: Dynamic Mesh vertex count -- see doc comment.

		bool bAnyAxisEnabled = false;
		for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			if (AxisParams[AxisIndex].bEnabled)
			{
				bAnyAxisEnabled = true;
				break;
			}
		}
		if (!bAnyAxisEnabled)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Unavailable;
			return Mask;
		}

#if !UE_BUILD_SHIPPING
		for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			if (AxisParams[AxisIndex].bEnabled && AxisParams[AxisIndex].bMirror)
			{
				VerifyMirrorSymmetryOnce();
				break;
			}
		}
#endif

		constexpr double MinExtent = 1e-5;

		TStaticArray<FVertexMaskForgeAxisBoundsResult, 3> AxisBounds;
		for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			if (!AxisParams[AxisIndex].bEnabled)
			{
				continue;
			}
			const EVertexMaskForgeBoundsAxis Axis = static_cast<EVertexMaskForgeBoundsAxis>(AxisIndex);
			const bool bWorldSpace = AxisParams[AxisIndex].bWorldSpace;
			FVertexMaskForgeAxisBoundsResult& BoundsResult = AxisBounds[AxisIndex];
			BoundsResult.MinCoord = TNumericLimits<double>::Max();
			BoundsResult.MaxCoord = TNumericLimits<double>::Lowest();

			for (const int32 VertexID : SourceMesh.VertexIndicesItr())
			{
				const FVector3f LocalPosition(SourceMesh.GetVertex(VertexID));
				// Individual bounds only in this domain (never Unified) -- bUseUnifiedBounds is always
				// false, matching Individual mode's own contract in GenerateBoundingBoxMask.
				const double Coord = ResolveAxisCoordinate(
					LocalPosition, ComponentTransform, Axis, bWorldSpace, /*bUseUnifiedBounds=*/false,
					FVector::ZeroVector, 1.0);
				BoundsResult.MinCoord = FMath::Min(BoundsResult.MinCoord, Coord);
				BoundsResult.MaxCoord = FMath::Max(BoundsResult.MaxCoord, Coord);
			}

			const double Extent = BoundsResult.MaxCoord - BoundsResult.MinCoord;
			if (!FMath::IsFinite(Extent) || Extent <= MinExtent)
			{
				BoundsResult.bDegenerate = true;
			}
		}

		for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			if (AxisParams[AxisIndex].bEnabled && AxisBounds[AxisIndex].bDegenerate)
			{
				Mask.State = EVertexMaskForgeScalarMaskState::DegenerateBounds;
				return Mask;
			}
		}

		Mask.Values.SetNumZeroed(ArraySize);
		Mask.bHasValue.Init(false, ArraySize);

		double Sum = 0.0;
		float MinValue = 1.f;
		float MaxValue = 0.f;
		int32 NumNearZero = 0;
		int32 NumNearOne = 0;
		bool bAllFinite = true;
		bool bAllInRange = true;

		for (const int32 VertexID : SourceMesh.VertexIndicesItr())
		{
			const FVector3f LocalPosition(SourceMesh.GetVertex(VertexID));

			float CombinedMask = 0.f;
			for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
			{
				const FVertexMaskForgeAxisMaskParams& Params = AxisParams[AxisIndex];
				if (!Params.bEnabled)
				{
					continue;
				}
				const EVertexMaskForgeBoundsAxis Axis = static_cast<EVertexMaskForgeBoundsAxis>(AxisIndex);
				const FVertexMaskForgeAxisBoundsResult& BoundsResult = AxisBounds[AxisIndex];

				const double Coord = ResolveAxisCoordinate(
					LocalPosition, ComponentTransform, Axis, Params.bWorldSpace, /*bUseUnifiedBounds=*/false,
					FVector::ZeroVector, 1.0);

				const double Extent = BoundsResult.MaxCoord - BoundsResult.MinCoord;
				const float T = static_cast<float>((Coord - BoundsResult.MinCoord) / Extent);

				const float SafeTransitionWidth = FMath::Max(Params.TransitionWidth, 1e-4f);
				const float EvaluationT = Params.bMirror ? (1.f - FMath::Abs(2.f * T - 1.f)) : T;
				float AxisMask = EvaluateAxisBaseGradient(EvaluationT, Params.Position, SafeTransitionWidth);

				if (Params.bInvert)
				{
					AxisMask = 1.f - AxisMask;
				}
				AxisMask = FMath::Clamp(AxisMask, 0.f, 1.f);

				CombinedMask = FMath::Max(CombinedMask, AxisMask);
			}

			if (!FMath::IsFinite(CombinedMask))
			{
				bAllFinite = false;
			}
			if (CombinedMask < 0.f || CombinedMask > 1.f)
			{
				bAllInRange = false;
			}

			Mask.Values[VertexID] = CombinedMask;
			Mask.bHasValue[VertexID] = true;

			Sum += CombinedMask;
			MinValue = FMath::Min(MinValue, CombinedMask);
			MaxValue = FMath::Max(MaxValue, CombinedMask);

			if (FMath::IsNearlyZero(CombinedMask, FVertexMaskForgeScalarMask::Tolerance))
			{
				++NumNearZero;
			}
			if (FMath::IsNearlyEqual(CombinedMask, 1.f, FVertexMaskForgeScalarMask::Tolerance))
			{
				++NumNearOne;
			}
		}

		Mask.NumValidValues = NumVerts;
		Mask.MinValue = MinValue;
		Mask.MaxValue = MaxValue;
		Mask.MeanValue = static_cast<float>(Sum / NumVerts);
		Mask.NumNearZero = NumNearZero;
		Mask.NumNearOne = NumNearOne;

		if (!bAllFinite || !bAllInRange || Mask.NumValidValues != NumVerts || Mask.Values.Num() != ArraySize)
		{
			Mask.State = EVertexMaskForgeScalarMaskState::Invalid;
			return Mask;
		}

		Mask.State = EVertexMaskForgeScalarMaskState::Ready;
		return Mask;
	}
}

#undef LOCTEXT_NAMESPACE
