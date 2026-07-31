#include "VertexMaskForgeDynamicLayerEvaluator.h"

#include "VertexMaskForgeInstanceResultStore.h"
#include "VertexMaskForgeSequentialEvaluator.h"

namespace
{
	// M16-K.5D: the one real fold, shared by both public overloads below. ResultStore == nullptr is the
	// legacy (two-argument) mode -- Mask is never consulted, reproducing the pre-K.5D behavior exactly.
	// A non-null ResultStore is the new mode -- ONLY then is Layer.Mask->MaskInstanceId ever looked up.
	FVector4f EvaluateColorInternal(const FVector4f& BaseColor, const FVertexMaskForgeDynamicLayerStack& Stack, const FVertexMaskForgeInstanceResultStore* ResultStore, const int32 VertexIndex)
	{
		FVector3f Composite(BaseColor.X, BaseColor.Y, BaseColor.Z);

		for (const FVertexMaskForgeLayer& Layer : Stack.GetLayers())
		{
			if (!Layer.bEnabled)
			{
				continue;
			}

			float FillValue = 0.0f;
			if (!VertexMaskForgeDynamicLayerEvaluator::TryResolveFillValue(Layer.Fill, FillValue))
			{
				// Fill::None (or, defensively, an invalid enumerator) -- no content, complete no-op
				// regardless of BlendMode/Opacity/Channel Filter.
				continue;
			}

			// EffectiveMask defaults to 1.0 -- the correct value both for a layer with no Mask (Mask is
			// never consulted in that case, regardless of ResultStore) and for the legacy overload
			// (ResultStore == nullptr), reproducing the pre-K.5D "Mask always ignored" behavior exactly.
			// Only a layer WITH a Mask, evaluated through the new overload, ever overrides this -- and
			// then only to the real stored value, or to 0.0f (never back to 1.0) when that value is
			// absent for any reason (unresolved MaskInstanceId or missing VertexIndex are treated
			// identically as zero coverage -- see this checkpoint's own architectural decision).
			float EffectiveMask = 1.0f;
			if (Layer.Mask.IsSet() && ResultStore != nullptr)
			{
				EffectiveMask = 0.0f;

				float StoredValue = 0.0f;
				const FVertexMaskForgeInstanceMaskResult* Result = ResultStore->Find(Layer.Mask->MaskInstanceId);
				if (Result != nullptr && Result->TryGetValue(VertexIndex, StoredValue))
				{
					EffectiveMask = StoredValue;
				}
			}

			const FVector3f PaintValue(FillValue * EffectiveMask, FillValue * EffectiveMask, FillValue * EffectiveMask);
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
		return EvaluateColorInternal(BaseColor, Stack, nullptr, INDEX_NONE);
	}

	FVector4f EvaluateColor(const FVector4f& BaseColor, const FVertexMaskForgeDynamicLayerStack& Stack, const FVertexMaskForgeInstanceResultStore& ResultStore, const int32 VertexIndex)
	{
		return EvaluateColorInternal(BaseColor, Stack, &ResultStore, VertexIndex);
	}
}
