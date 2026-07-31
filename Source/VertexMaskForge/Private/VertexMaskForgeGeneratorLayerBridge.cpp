#include "VertexMaskForgeGeneratorLayerBridge.h"

#include "VertexMaskForgeSequentialEvaluator.h"
#include "VertexMaskForgeWorkingMeshTypes.h" // FVertexMaskForgeScalarMask::TryGetValue -- complete type needed here.

namespace VertexMaskForgeGeneratorLayerBridge
{
	FVector4f ComposeGeneratorLayersSequential(
		const FVector4f& BaselineColor,
		const FVector4f& CommittedColor,
		const int32 VertexIndex,
		const TArrayView<const VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams> SortedLayers,
		const bool bFilterR, const bool bFilterG, const bool bFilterB,
		bool& bOutAnyLayerContributed)
	{
		// AUDITED (M16-J final): resolved ONCE per vertex, identical lookup/skip semantics to the legacy
		// ComposeMaskStack -- LookupIndex prefers a layer's own IndexOverride when set (Nanite/
		// Source-Topology domains, e.g. Ambient Occlusion by Normal Overlay Element ID, Material Slot/
		// Directional Normal by corner index), falling back to the shared VertexIndex otherwise. A null
		// Mask pointer or a failed TryGetValue silently skips ONLY that one layer at THIS one vertex.
		TArray<FVertexMaskForgeLayerEvaluationInput, TInlineAllocator<8>> ResolvedLayers;
		for (const VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams& Layer : SortedLayers)
		{
			float MaskValue = 0.0f;
			const int32 LookupIndex = (Layer.IndexOverride >= 0) ? Layer.IndexOverride : VertexIndex;
			if (!Layer.Mask || !Layer.Mask->TryGetValue(LookupIndex, MaskValue))
			{
				continue;
			}

			// AUDITED (M16-J final): implicit white FillValue -- makes PaintValue (FillValue *
			// EffectiveMask, computed inside EvaluateFillLayers) exactly equal to this generator's own
			// resolved scalar, broadcast to all 3 channels. See this file's own header module comment.
			FVertexMaskForgeLayerEvaluationInput Input;
			Input.FillValue = FVector3f(1.0f, 1.0f, 1.0f);
			Input.BlendMode = Layer.BlendMode;
			Input.Opacity = Layer.Opacity;
			Input.bEnabled = true;
			Input.EffectiveMask = MaskValue;
			ResolvedLayers.Add(Input);
		}

		// AUDITED (M16-J final): same "iff Layers is non-empty" contract as the legacy ComposeStack's own
		// bOutAnyLayerContributed.
		bOutAnyLayerContributed = !ResolvedLayers.IsEmpty();

		FVector4f Result;
		Result.W = BaselineColor.W;

		if (!bOutAnyLayerContributed)
		{
			// Same "no change" fallback as the legacy ComposeStack: R/G/B verbatim from CommittedColor.
			Result.X = CommittedColor.X;
			Result.Y = CommittedColor.Y;
			Result.Z = CommittedColor.Z;
			return Result;
		}

		// AUDITED (M16-J final): folds from BaselineColor -- the same "Base" seed the legacy ComposeStack
		// itself used, never CommittedColor, never a neutral 1.0/0.0 seed.
		const FVector3f BaseVec(BaselineColor.X, BaselineColor.Y, BaselineColor.Z);
		const FVector3f Composite = VertexMaskForgeSequentialEvaluator::EvaluateFillLayers(BaseVec, ResolvedLayers);

		// AUDITED (M16-J final): Channel Filter -- a channel NOT enabled is read verbatim from
		// CommittedColor, untouched by any layer -- identical contract to the legacy ComposeStack's own.
		Result.X = bFilterR ? Composite.X : CommittedColor.X;
		Result.Y = bFilterG ? Composite.Y : CommittedColor.Y;
		Result.Z = bFilterB ? Composite.Z : CommittedColor.Z;
		return Result;
	}
}
