#pragma once

#include "Containers/ArrayView.h"
#include "CoreMinimal.h"
#include "Math/Vector4.h"
#include "VertexMaskForgeMaskTypes.h"

/**
 * Generic, stateless mask-stack composition math (M2 extraction). Operates ONLY on already-resolved
 * scalar samples -- it never looks up a mask value itself, never knows which generator produced a
 * sample, never sorts layers, and never knows about render vertices, triangle corners, Static Meshes,
 * Dynamic Meshes, or Nanite/Source-Topology domains. All of that (building the ordered sample sequence
 * via FVertexMaskForgeScalarMask::TryGetValue()/IndexOverride resolution) remains the caller's
 * responsibility -- see VertexMaskForgePanel::ComposeMaskStack in SVertexMaskForgePanel.cpp, the sole
 * wrapper that adapts panel-side layer data into the FResolvedMaskLayer sequence this namespace consumes.
 */
namespace VertexMaskForgeMaskStackComposer
{
	/**
	 * AUDITED (M16-B): the raw, un-opacitized blend-mode formula for one channel, operating on
	 * already-normalized [0,1] Base ("B")/Mask ("M") values. Pure function -- mutates nothing, never
	 * clamps. Moved here from an internal-linkage (panel-file-local `static`) definition inside
	 * VertexMaskForgeMaskStackComposer.cpp so the new sequential evaluators
	 * (VertexMaskForgeSequentialEvaluator.h/.cpp) can reuse the exact same formula ComposeStack itself
	 * calls -- body unchanged, linkage only. See VertexMaskForgeMaskStackComposer.cpp for the full
	 * per-mode derivation/doc comment.
	 */
	float ApplyMaskBlendMode(float Base, float Mask, EVertexMaskForgeBlendMode Mode);

	/**
	 * AUDITED (M16-B): the Opacity lerp, WITHOUT any clamp -- BlendResult = ApplyMaskBlendMode(...),
	 * Result = Lerp(Base, BlendResult, Opacity). Same linkage-only extraction as ApplyMaskBlendMode
	 * above; body unchanged. Both the legacy ComposeStack (unmodified, still stage-grouped) and the new
	 * sequential evaluators call this exact function -- there is only one implementation of these
	 * formulas anywhere in the plugin.
	 */
	float BlendMaskValueUnclamped(float Base, float Mask, EVertexMaskForgeBlendMode Mode, float Opacity);

	/**
	 * ONE mask generator's already-resolved contribution to a single vertex/corner's composition --
	 * exactly the (MaskValue, BlendMode, Opacity) triple the fixed-stage fold in ComposeStack needs,
	 * and nothing else. Deliberately carries no pointer back to the mask that produced this value, no
	 * generator identifier, and no index/domain information -- by the time a caller constructs one of
	 * these, TryGetValue() has already succeeded and the caller's own sort/lookup work is done.
	 */
	struct FResolvedMaskLayer
	{
		EVertexMaskForgeBlendMode BlendMode = EVertexMaskForgeBlendMode::Copy;
		float Opacity = 1.0f;
		float MaskValue = 0.0f;
	};

	/**
	 * Combines an already-ordered sequence of resolved mask samples into ONE composed RGBA result for
	 * a single vertex/corner, starting from BaselineColor's own channel values ("Base") -- never a
	 * neutral 1.0/0.0 seed. See VertexMaskForgePanel::ComposeMaskStack's own (unchanged) doc comment in
	 * SVertexMaskForgePanel.cpp for the full canonical-order/stage-grouping derivation and proofs this
	 * function's fixed-stage fold implements verbatim (Copy, Add+Subtract, Multiply, CLAMP, Overlay,
	 * Screen, Linear); this header only restates the externally-observable contract:
	 *
	 * - Layers is assumed ALREADY in the caller's intended fold order (Copy/Overlay/Linear are
	 *   sequential, non-commutative folds over Layers in the order given -- this function never sorts
	 *   or reorders it).
	 * - Channels NOT enabled in the Channel Filter (bFilterR/G/B false) are read verbatim from
	 *   CommittedColor -- untouched by any layer or stage.
	 * - Alpha is always BaselineColor.W, unconditionally -- never composed, never filtered.
	 * - bOutAnyLayerContributed is set to true iff Layers is non-empty (the caller only ever includes a
	 *   layer once its own TryGetValue() succeeded for this vertex/corner, so a non-empty sequence here
	 *   always means at least one real contribution) -- when false, the returned color's R/G/B are
	 *   CommittedColor's own values (Alpha still BaselineColor.W), i.e. "no change".
	 */
	FVector4f ComposeStack(
		const FVector4f& BaselineColor,
		const FVector4f& CommittedColor,
		TArrayView<const FResolvedMaskLayer> Layers,
		bool bFilterR, bool bFilterG, bool bFilterB,
		bool& bOutAnyLayerContributed);
}
