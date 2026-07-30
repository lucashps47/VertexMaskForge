#include "VertexMaskForgeSequentialEvaluator.h"

#include "VertexMaskForgeMaskStackComposer.h"

namespace VertexMaskForgeSequentialEvaluator
{
	float EvaluateMaskStep(const float Result, const float MaskValue, const EVertexMaskForgeBlendMode Mode, const float Opacity)
	{
		// Reuses the SAME primitive ComposeStack itself calls -- no independent formula exists here.
		const float Blended = VertexMaskForgeMaskStackComposer::BlendMaskValueUnclamped(Result, MaskValue, Mode, Opacity);
		return FMath::Clamp(Blended, 0.0f, 1.0f);
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
		return Result;
	}

	FVector3f EvaluateFillLayerStep(const FVector3f& CompositeBelow, const FVector3f& FillValue, const EVertexMaskForgeBlendMode Mode, const float Coverage)
	{
		// Same primitive, applied independently per channel -- FillValue.Channel plays the role
		// MaskValue plays in EvaluateMaskStep; Coverage plays the role Opacity plays there.
		return FVector3f(
			FMath::Clamp(VertexMaskForgeMaskStackComposer::BlendMaskValueUnclamped(CompositeBelow.X, FillValue.X, Mode, Coverage), 0.0f, 1.0f),
			FMath::Clamp(VertexMaskForgeMaskStackComposer::BlendMaskValueUnclamped(CompositeBelow.Y, FillValue.Y, Mode, Coverage), 0.0f, 1.0f),
			FMath::Clamp(VertexMaskForgeMaskStackComposer::BlendMaskValueUnclamped(CompositeBelow.Z, FillValue.Z, Mode, Coverage), 0.0f, 1.0f));
	}

	FVector3f EvaluateFillLayers(const FVector3f& BaseValue, const TConstArrayView<FVertexMaskForgeLayerEvaluationInput> Inputs)
	{
		FVector3f Composite = BaseValue;
		for (const FVertexMaskForgeLayerEvaluationInput& Input : Inputs)
		{
			if (!Input.bEnabled)
			{
				continue;
			}
			const float Coverage = FMath::Clamp(Input.Opacity * Input.EffectiveMask, 0.0f, 1.0f);
			Composite = EvaluateFillLayerStep(Composite, Input.FillValue, Input.BlendMode, Coverage);
		}
		return Composite;
	}
}
