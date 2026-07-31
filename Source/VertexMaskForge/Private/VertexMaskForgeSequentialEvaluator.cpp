#include "VertexMaskForgeSequentialEvaluator.h"

#include "VertexMaskForgeMaskStackComposer.h"

namespace VertexMaskForgeSequentialEvaluator
{
	float EvaluateMaskStep(const float Result, const float MaskValue, const EVertexMaskForgeBlendMode Mode, const float Opacity)
	{
		// Reuses the SAME primitive ComposeStack itself calls -- no independent formula exists here.
		// AUDITED (M16-J final): deliberately UNCLAMPED -- see this function's own header doc comment.
		return VertexMaskForgeMaskStackComposer::BlendMaskValueUnclamped(Result, MaskValue, Mode, Opacity);
	}

	float EvaluateMaskStack(const TConstArrayView<FVertexMaskForgeMaskEvaluationInput> Inputs)
	{
		float Result = 1.0f;
		for (const FVertexMaskForgeMaskEvaluationInput& Input : Inputs)
		{
			if (!Input.bEnabled)
			{
				continue;
			}
			Result = EvaluateMaskStep(Result, Input.MaskValue, Input.BlendMode, Input.Opacity);
		}
		// AUDITED (M16-J final): the ONLY clamp in this fold -- applied once, to the final accumulator,
		// never per-step. For an empty/all-disabled input, Result is still the unclamped seed (1.0),
		// already in range, so this is a no-op there -- the empty/non-empty cases remain continuous.
		return FMath::Clamp(Result, 0.0f, 1.0f);
	}

	FVector3f EvaluateFillLayerStep(const FVector3f& CompositeBelow, const FVector3f& PaintValue, const EVertexMaskForgeBlendMode Mode, const float LayerOpacity)
	{
		// Same primitive, applied independently per channel -- PaintValue.Channel plays the role
		// MaskValue plays in EvaluateMaskStep; LayerOpacity plays the role Opacity plays there.
		// AUDITED (M16-J final): deliberately UNCLAMPED -- see this function's own header doc comment.
		return FVector3f(
			VertexMaskForgeMaskStackComposer::BlendMaskValueUnclamped(CompositeBelow.X, PaintValue.X, Mode, LayerOpacity),
			VertexMaskForgeMaskStackComposer::BlendMaskValueUnclamped(CompositeBelow.Y, PaintValue.Y, Mode, LayerOpacity),
			VertexMaskForgeMaskStackComposer::BlendMaskValueUnclamped(CompositeBelow.Z, PaintValue.Z, Mode, LayerOpacity));
	}

	FVector3f EvaluateFillLayers(const FVector3f& BaseValue, const TConstArrayView<FVertexMaskForgeLayerEvaluationInput> Inputs)
	{
		// AUDITED (M16-J final): an empty stack returns BaseValue verbatim, UNCLAMPED -- BaseValue is
		// always the caller's own already-valid color (e.g. CommittedColors[i]); this function never
		// re-clamps it just because the array happened to be empty (see this function's own header doc
		// comment).
		if (Inputs.IsEmpty())
		{
			return BaseValue;
		}

		FVector3f Composite = BaseValue;
		for (const FVertexMaskForgeLayerEvaluationInput& Input : Inputs)
		{
			if (!Input.bEnabled)
			{
				continue;
			}
			// AUDITED (M16-J final -- authoritative semantic decision): PaintValue = FillValue *
			// EffectiveMask, computed HERE, component-wise, BEFORE the blend -- never a Coverage term
			// folded into the Opacity operand instead (that was this function's own prior behavior; see
			// EvaluateFillLayerStep's own header doc comment for the exact prior-vs-current distinction).
			// A white FillValue (1,1,1) makes PaintValue literally equal to the broadcast EffectiveMask
			// scalar (item 9 of the authoritative decision); a black FillValue (0,0,0) makes PaintValue
			// exactly zero regardless of EffectiveMask (item 10).
			const FVector3f PaintValue = Input.FillValue * Input.EffectiveMask;
			Composite = EvaluateFillLayerStep(Composite, PaintValue, Input.BlendMode, Input.Opacity);
		}
		// AUDITED (M16-J final): the ONLY clamp in this fold -- applied once, component-wise, to the
		// final accumulator, never per-layer.
		return FVector3f(
			FMath::Clamp(Composite.X, 0.0f, 1.0f),
			FMath::Clamp(Composite.Y, 0.0f, 1.0f),
			FMath::Clamp(Composite.Z, 0.0f, 1.0f));
	}
}
