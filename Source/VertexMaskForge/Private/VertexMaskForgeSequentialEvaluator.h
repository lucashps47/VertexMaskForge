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
	 * AUDITED (M16-J final -- authoritative clamp-policy decision): one Mask Stack fold STEP, UNCLAMPED --
	 * Result' = BlendMaskValueUnclamped(Result, MaskValue, Mode, Opacity), reusing
	 * VertexMaskForgeMaskStackComposer::BlendMaskValueUnclamped verbatim (no independent reimplementation
	 * of any Blend Mode formula exists in this module). Opacity is applied as the Lerp weight, never
	 * pre-multiplied into MaskValue -- pre-multiplying would silently change what Multiply/Overlay/
	 * Screen/Linear each mean (see the M15.2 audit's own proof). Deliberately NOT clamped here -- this is
	 * a per-step primitive; ONLY EvaluateMaskStack (the fold's own public, final entry point) clamps, and
	 * only once, after every step has run. Calling this function directly and expecting a [0,1] result is
	 * a caller error -- use EvaluateMaskStack for that guarantee.
	 */
	float EvaluateMaskStep(float Result, float MaskValue, EVertexMaskForgeBlendMode Mode, float Opacity);

	/**
	 * Evaluates a full Mask Stack: seed = 1.0 ("full coverage / no restriction" -- continuous with the
	 * empty-stack case below, see the M15.2 audit's own seed derivation), then EvaluateMaskStep folded
	 * once per ENABLED entry, strictly in array order, UNCLAMPED at every intermediate step -- no
	 * reordering, no grouping by Blend Mode. AUDITED (M16-J final): Clamp01 is applied EXACTLY ONCE, to
	 * the fold's final accumulator, after the last enabled entry -- never between steps (this replaces
	 * this function's own prior per-step-clamp behavior; see VertexMaskForgeSequentialEvaluatorTests.cpp
	 * for the characterization proving the two policies diverge whenever an intermediate step would have
	 * left [0,1]). Disabled entries (bEnabled==false) are skipped internally, regardless of what the
	 * caller may or may not have already filtered. An empty array, or an array with zero enabled entries,
	 * returns 1.0 directly (never enters the fold) -- this is the SAME value the seed already is, so the
	 * empty/non-empty cases are continuous, never a discontinuity.
	 */
	float EvaluateMaskStack(TConstArrayView<FVertexMaskForgeMaskEvaluationInput> Inputs);

	/**
	 * AUDITED (M16-J final -- authoritative semantic decision): one Fill Layer fold STEP, UNCLAMPED --
	 * LayerOutput = BlendMaskValueUnclamped(CompositeBelow, PaintValue, Mode, LayerOpacity), applied
	 * independently per channel (R, G, B), each channel reusing
	 * VertexMaskForgeMaskStackComposer::BlendMaskValueUnclamped verbatim with PaintValue.channel as the
	 * "Mask" operand and LayerOpacity (the layer's own Opacity, NOT a Coverage term derived from
	 * EffectiveMask) as the Opacity operand. PaintValue is the CALLER's responsibility to have already
	 * computed as FillValue * EffectiveMask (see EvaluateFillLayers, the only real caller) -- this step
	 * function itself never multiplies EffectiveMask into anything; it only ever sees PaintValue and
	 * LayerOpacity as two independent parameters, exactly mirroring EvaluateMaskStep's own
	 * (Result, MaskValue, Mode, Opacity) shape. Deliberately NOT clamped here -- same per-step/final-only
	 * clamp split as EvaluateMaskStep/EvaluateMaskStack above; use EvaluateFillLayers for a [0,1]
	 * guarantee.
	 */
	FVector3f EvaluateFillLayerStep(const FVector3f& CompositeBelow, const FVector3f& PaintValue, EVertexMaskForgeBlendMode Mode, float LayerOpacity);

	/**
	 * M19-A: four-channel sibling of EvaluateFillLayerStep above, covering Alpha (W) alongside R/G/B (X/Y/Z)
	 * -- LayerOutput = BlendMaskValueUnclamped(CompositeBelow, PaintValue, Mode, LayerOpacity), applied
	 * independently per channel, all four channels reusing VertexMaskForgeMaskStackComposer::
	 * BlendMaskValueUnclamped verbatim (the SAME primitive the three-channel overload above already calls
	 * -- no independent or duplicated Alpha blend math exists anywhere in this module). Deliberately a
	 * SEPARATE overload rather than a widened FVector3f -> FVector4f signature on the existing function:
	 * the three-channel EvaluateFillLayerStep/EvaluateFillLayers/FVertexMaskForgeLayerEvaluationInput are
	 * also called by VertexMaskForgeGeneratorLayerBridge::ComposeGeneratorLayersSequential and
	 * VertexMaskForgeFillLayerResolution::EvaluateFillLayerFromKeyedResults -- both operate on the separate,
	 * fixed-slot Recipe/Mask Stack model (VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams /
	 * FVertexMaskForgeFillLayer), unrelated to the Dynamic Layers/Source Topology Alpha contract this
	 * checkpoint (M19-A) implements. Widening the shared three-channel function in place would force
	 * mechanical, out-of-scope changes onto those two unrelated production call sites; this overload keeps
	 * their FVector3f contract completely untouched while still sharing the one real blend primitive.
	 * Deliberately NOT clamped here -- same per-step/final-only clamp split as every other evaluator
	 * primitive in this module; the caller (ComputeComposedColorsRGBSourceTopology's own Pass 2 fold)
	 * clamps once, at the end of each corner, exactly like it already does for R/G/B.
	 */
	FVector4f EvaluateFillLayerStep(const FVector4f& CompositeBelow, const FVector4f& PaintValue, EVertexMaskForgeBlendMode Mode, float LayerOpacity);

	/**
	 * Evaluates a full ordered sequence of Fill Layers: starts from BaseValue, then, per ENABLED entry,
	 * strictly in array order: PaintValue = FillValue * EffectiveMask (component-wise -- a white FillValue
	 * (1,1,1) makes PaintValue literally equal to the broadcast EffectiveMask scalar; a black FillValue
	 * (0,0,0) makes PaintValue exactly zero, regardless of EffectiveMask); LayerOutput =
	 * EvaluateFillLayerStep(CompositeBelow, PaintValue, BlendMode, LayerOpacity), UNCLAMPED; LayerOutput
	 * becomes CompositeBelow for the next layer. AUDITED (M16-J final): Clamp01 (component-wise) is
	 * applied EXACTLY ONCE, to the fold's final accumulator, after the last enabled entry -- never between
	 * layers (deliberately NOT the legacy ComposeStack algorithm, and NOT this function's own prior
	 * per-layer-clamp behavior -- see this header's own module comment and
	 * VertexMaskForgeSequentialEvaluatorTests.cpp for the characterization proving the two clamp policies
	 * diverge). Disabled entries are skipped internally, same contract as EvaluateMaskStack. An empty
	 * array returns BaseValue directly, UNCLAMPED -- BaseValue is the caller's own (already-valid) color,
	 * never re-clamped here just because the array happened to be empty (see EvaluateFillLayerResolution's
	 * own callers, which always pass an already-[0,1] CommittedColors sample as BaseValue).
	 */
	FVector3f EvaluateFillLayers(const FVector3f& BaseValue, TConstArrayView<FVertexMaskForgeLayerEvaluationInput> Inputs);
}
