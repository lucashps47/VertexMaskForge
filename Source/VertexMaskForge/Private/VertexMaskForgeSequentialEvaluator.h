#pragma once

#include "Containers/ArrayView.h"
#include "CoreMinimal.h"
#include "Math/Vector.h"
#include "VertexMaskForgeMaskTypes.h"

/**
 * M16-B: pure, stateless, strictly sequential (array-order) evaluators for the future Mask Stack and
 * Fill Layer composition -- see the M15/M15.1/M15.2/M15.2.1 architectural audits for the full
 * mathematical derivation of why this is deliberately NOT the same algorithm as the legacy
 * VertexMaskForgeMaskStackComposer::ComposeStack (which groups by Blend Mode into fixed stages and
 * does not respect cross-mode visual reorder).
 *
 * Neither evaluator here executes a mask generator, reads a cache, or knows about
 * FVertexMaskForgeMaskInstance/FVertexMaskForgeFillLayer/FVertexMaskForgeRecipe (VertexMaskForgeRecipeTypes.h)
 * at all -- they operate exclusively on the small, transient evaluation-input structs below, which the
 * caller is responsible for resolving from whatever recipe/result data exists (a future orchestration
 * checkpoint, not this one). This is deliberate: evaluators must remain reusable, testable in complete
 * isolation, and free of any Slate/panel/mesh/component dependency.
 *
 * ComposeStack itself is untouched by this module and remains the sole compositor for the existing
 * fixed panel flow -- these evaluators have no caller anywhere in the current runtime path.
 */

/**
 * One Mask Instance's already-resolved contribution to a Mask Stack evaluation. Transient -- never
 * persisted, never part of the recipe (FVertexMaskForgeMaskInstance carries authoring parameters, not
 * a generated MaskValue).
 */
struct FVertexMaskForgeMaskEvaluationInput
{
	/** The mask generator's already-generated scalar result for this specific vertex/corner (0..1 for
	 *  well-formed input). Never resolved by the evaluator itself. */
	float MaskValue = 0.0f;

	EVertexMaskForgeBlendMode BlendMode = EVertexMaskForgeBlendMode::Copy;

	/** Intra-stack Opacity -- the Lerp weight of this one operation, not a multiplier baked into
	 *  MaskValue ahead of time (see EvaluateMaskStep's own doc comment for why). */
	float Opacity = 1.0f;

	/** AUDITED (M16-B): the evaluator itself skips bEnabled==false entries internally -- callers must
	 *  not rely exclusively on their own pre-filtering for correctness (only as an optional
	 *  optimization). */
	bool bEnabled = true;
};

/**
 * One Fill Layer's already-resolved contribution to a Fill Layer evaluation. Transient -- never
 * persisted, never part of the recipe. EffectiveMask is expected to already be the output of
 * EvaluateMaskStack for this layer's own Mask Stack, at this specific vertex/corner.
 */
struct FVertexMaskForgeLayerEvaluationInput
{
	/** RGB content -- Alpha is deliberately out of scope for this evaluator (see this header's own
	 *  module comment and VertexMaskForgeRecipeTypes.h's own FillValue documentation). */
	FVector3f FillValue = FVector3f(1.0f, 1.0f, 1.0f);

	EVertexMaskForgeBlendMode BlendMode = EVertexMaskForgeBlendMode::Copy;

	/** Inter-layer Opacity -- distinct from any Mask Instance's own intra-stack Opacity. */
	float Opacity = 1.0f;

	/** AUDITED (M16-B): the evaluator itself skips bEnabled==false entries internally, same contract
	 *  as FVertexMaskForgeMaskEvaluationInput::bEnabled. */
	bool bEnabled = true;

	/** The EffectiveMask this layer's own Mask Stack already produced for this vertex/corner (0..1 for
	 *  well-formed input; an empty Mask Stack must resolve to 1.0 before reaching here -- see
	 *  EvaluateMaskStack's own empty-stack contract). */
	float EffectiveMask = 1.0f;
};

namespace VertexMaskForgeSequentialEvaluator
{
	/**
	 * One Mask Stack fold step: Applied = ApplyMaskBlendMode(Result, MaskValue, Mode); NewResult =
	 * Clamp01(Lerp(Result, Applied, Opacity)) -- exactly the approved contract, reusing
	 * VertexMaskForgeMaskStackComposer::BlendMaskValueUnclamped verbatim (no independent
	 * reimplementation of any Blend Mode formula exists in this module). Opacity is applied as the
	 * Lerp weight, never pre-multiplied into MaskValue -- pre-multiplying would silently change what
	 * Multiply/Overlay/Screen/Linear each mean (see the M15.2 audit's own proof).
	 */
	float EvaluateMaskStep(float Result, float MaskValue, EVertexMaskForgeBlendMode Mode, float Opacity);

	/**
	 * Evaluates a full Mask Stack: seed = 1.0 ("full coverage / no restriction" -- continuous with the
	 * empty-stack case below, see the M15.2 audit's own seed derivation), then EvaluateMaskStep folded
	 * once per ENABLED entry, strictly in array order -- no reordering, no grouping by Blend Mode.
	 * Disabled entries (bEnabled==false) are skipped internally, regardless of what the caller may or
	 * may not have already filtered. An empty array, or an array with zero enabled entries, returns
	 * 1.0 directly (never enters the fold) -- this is the SAME value the seed already is, so the
	 * empty/non-empty cases are continuous, never a discontinuity.
	 */
	float EvaluateMaskStack(TConstArrayView<FVertexMaskForgeMaskEvaluationInput> Inputs);

	/**
	 * One Fill Layer fold step: Coverage = Clamp01(Opacity * EffectiveMask); Composite =
	 * Clamp01(Lerp(CompositeBelow, ApplyMaskBlendMode(CompositeBelow, FillValue, Mode), Coverage)) --
	 * applied independently per channel (R, G, B), each channel reusing
	 * VertexMaskForgeMaskStackComposer::BlendMaskValueUnclamped verbatim with FillValue.channel as the
	 * "Mask" operand and Coverage as the Opacity operand. EffectiveMask=0 or Opacity=0 both force
	 * Coverage=0, which guarantees CompositeBelow passthrough unconditionally, for every Blend Mode --
	 * see the M15.2 audit's own proof of this guarantee.
	 */
	FVector3f EvaluateFillLayerStep(const FVector3f& CompositeBelow, const FVector3f& FillValue, EVertexMaskForgeBlendMode Mode, float Coverage);

	/**
	 * Evaluates a full ordered sequence of Fill Layers: starts from BaseValue, then
	 * EvaluateFillLayerStep folded once per ENABLED entry, strictly in array order -- no reordering, no
	 * grouping by Blend Mode (deliberately NOT the legacy ComposeStack algorithm -- see this header's
	 * own module comment). Disabled entries are skipped internally, same contract as
	 * EvaluateMaskStack. An empty array returns BaseValue directly.
	 */
	FVector3f EvaluateFillLayers(const FVector3f& BaseValue, TConstArrayView<FVertexMaskForgeLayerEvaluationInput> Inputs);
}
