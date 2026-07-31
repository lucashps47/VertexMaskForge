#include "VertexMaskForgeDynamicLayerEvaluator.h"

#include "VertexMaskForgeSequentialEvaluator.h"

namespace VertexMaskForgeDynamicLayerEvaluator
{
	bool TryResolveFillValue(const EVertexMaskForgeLayerFill Fill, float& OutValue)
	{
		switch (Fill)
		{
		case EVertexMaskForgeLayerFill::None:
			// No content -- distinct from Black (which IS content, value 0.0f). See this function's own
			// header doc comment.
			return false;
		case EVertexMaskForgeLayerFill::Black:
			OutValue = 0.0f;
			return true;
		case EVertexMaskForgeLayerFill::White:
			OutValue = 1.0f;
			return true;
		default:
			// Defensively unreachable through FVertexMaskForgeDynamicLayerStack's own public API (its
			// SetLayerFill rejects any non-enumerator value) -- degrades to a no-op layer, never a crash.
			return false;
		}
	}

	FVector4f EvaluateColor(const FVector4f& BaseColor, const FVertexMaskForgeDynamicLayerStack& Stack)
	{
		FVector3f Composite(BaseColor.X, BaseColor.Y, BaseColor.Z);

		for (const FVertexMaskForgeLayer& Layer : Stack.GetLayers())
		{
			if (!Layer.bEnabled)
			{
				continue;
			}

			float FillValue = 0.0f;
			if (!TryResolveFillValue(Layer.Fill, FillValue))
			{
				// Fill::None (or, defensively, an invalid enumerator) -- no content, complete no-op
				// regardless of BlendMode/Opacity/Channel Filter.
				continue;
			}

			// EffectiveMask is implicitly 1.0 this checkpoint (no procedural mask exists yet), so
			// PaintValue is simply the resolved Fill value broadcast to all 3 channels -- never Fill
			// multiplied by Opacity here, since EvaluateFillLayerStep already applies Opacity itself.
			const FVector3f PaintValue(FillValue, FillValue, FillValue);
			const FVector3f StepResult = VertexMaskForgeSequentialEvaluator::EvaluateFillLayerStep(Composite, PaintValue, Layer.BlendMode, Layer.Opacity);

			// Per-layer Channel Filter, applied immediately after this one layer's step -- a disabled
			// channel keeps exactly what the composite already was before this layer, so the NEXT layer
			// still folds from the correct running value for that channel.
			Composite.X = Layer.bAffectRed ? StepResult.X : Composite.X;
			Composite.Y = Layer.bAffectGreen ? StepResult.Y : Composite.Y;
			Composite.Z = Layer.bAffectBlue ? StepResult.Z : Composite.Z;
		}

		// The ONLY clamp in this fold -- applied once, component-wise, to the final composite, after every
		// layer has been folded. Alpha is never clamped here -- carried verbatim from BaseColor.W.
		return FVector4f(
			FMath::Clamp(Composite.X, 0.0f, 1.0f),
			FMath::Clamp(Composite.Y, 0.0f, 1.0f),
			FMath::Clamp(Composite.Z, 0.0f, 1.0f),
			BaseColor.W);
	}
}
