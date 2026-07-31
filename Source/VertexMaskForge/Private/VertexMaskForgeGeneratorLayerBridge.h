#pragma once

#include "Containers/ArrayView.h"
#include "CoreMinimal.h"
#include "Math/Vector4.h"
#include "VertexMaskForgeMaskTypes.h"

/**
 * M16-J final: pure, stateless bridge/adapter between the panel's existing, already-sorted
 * TArrayView<VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams> (built identically to before this
 * checkpoint -- see ApplyPreviewToEntry's own Layers construction, unchanged) and
 * VertexMaskForgeSequentialEvaluator::EvaluateFillLayers, the new strictly-sequential (array-order) fold
 * that REPLACES the legacy VertexMaskForgeMaskStackComposer::ComposeStack (fixed-stage, Blend-Mode-grouped)
 * at the panel's two real composition call sites (ComputeComposedColorsRGB /
 * ComputeComposedColorsRGBSourceTopology, both in SVertexMaskForgePanel.cpp).
 *
 * Deliberately thin and generic -- like the legacy ComposeMaskStack/ComposeStack pair before it, this
 * module never reads GeneratorState, InstanceResults, or any panel/UI state directly, and never touches
 * Recipe/FillLayerResolution/InstanceResultStore (that pipeline is explicitly OUT OF SCOPE for this
 * checkpoint -- see the M16-J final planning report's own scope boundary); it only ever sees the same
 * (Mask pointer, BlendMode, Opacity, IndexOverride) tuples the panel already resolves per generator, plus
 * one VertexIndex, exactly mirroring the legacy ComposeMaskStack's own signature and per-vertex
 * lookup/skip semantics so both real call sites need only swap which function they call.
 *
 * Each successfully-resolved generator layer becomes exactly ONE Fill Layer input to the sequential
 * evaluator, with an IMPLICIT WHITE FillValue (1,1,1) -- see VertexMaskForgeSequentialEvaluator.h's own
 * PaintValue contract: with a white FillValue, PaintValue == EffectiveMask exactly (the generator's own
 * resolved scalar), broadcast to all 3 channels -- so this bridge introduces no new artistic concept, it
 * only threads each generator's existing scalar output through the new fold instead of the old one. The
 * Mask Stack side of the new evaluator (VertexMaskForgeSequentialEvaluator::EvaluateMaskStack, i.e.
 * multiple procedural masks stacked WITHIN one Fill Layer) is not exercised here -- every one of the
 * panel's 7 generators becomes its own single-instance Fill Layer instead.
 */
namespace VertexMaskForgeGeneratorLayerBridge
{
	/**
	 * Resolves each entry of SortedLayers at VertexIndex (or its own IndexOverride, if >= 0) exactly
	 * like the legacy ComposeMaskStack did -- a null Mask or a failed TryGetValue silently skips that
	 * ONE layer for THIS one vertex/corner only (never a hard failure, never affects any other layer or
	 * vertex) -- then folds the resolved layers, strictly in SortedLayers' own array order (never
	 * re-sorted, never grouped by Blend Mode), via VertexMaskForgeSequentialEvaluator::EvaluateFillLayers,
	 * starting from BaselineColor's own channel values -- the same "Base" seed the legacy ComposeStack
	 * itself used, never CommittedColor, never a neutral seed.
	 *
	 * Channel Filter (bFilterR/G/B): a channel NOT enabled is read verbatim from CommittedColor,
	 * untouched by any layer -- identical contract to the legacy ComposeStack's own. Alpha is always
	 * BaselineColor.W, unconditionally -- never composed here, matching ComposeStack; the caller
	 * (ApplyComposedColorsRGB) re-forces Alpha from its own Baseline anyway, so this is defense-in-depth,
	 * not the sole guarantee.
	 *
	 * bOutAnyLayerContributed is set to true iff at least one layer successfully resolved a value at
	 * this VertexIndex (independent of Channel Filter) -- same contract as the legacy ComposeStack's own.
	 * When false, the returned color's R/G/B are CommittedColor's own values (Alpha still
	 * BaselineColor.W), i.e. "no change" -- identical fallback to the legacy function.
	 */
	FVector4f ComposeGeneratorLayersSequential(
		const FVector4f& BaselineColor,
		const FVector4f& CommittedColor,
		int32 VertexIndex,
		TArrayView<const VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams> SortedLayers,
		bool bFilterR, bool bFilterG, bool bFilterB,
		bool& bOutAnyLayerContributed);
}
