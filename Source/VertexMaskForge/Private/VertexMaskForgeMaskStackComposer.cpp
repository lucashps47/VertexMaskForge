#include "VertexMaskForgeMaskStackComposer.h"

namespace VertexMaskForgeMaskStackComposer
{
	// --- Blend Mode math (see EVertexMaskForgeBlendMode) -------------------------------------
	// AUDITED (M2 extraction): moved verbatim from VertexMaskForgePanel::ApplyMaskBlendMode /
	// BlendMaskValueUnclamped in SVertexMaskForgePanel.cpp -- no formula, clamp, or ordering change.

	/**
	 * The raw, un-opacitized blend-mode formula for one channel, operating on already-normalized
	 * [0,1] Base ("B")/Mask ("M") values. Pure function -- mutates nothing, never clamps. Linear is
	 * deliberately NOT an alias of Copy, even though lerp(B, M, M) at M=1 equals M exactly like
	 * Copy does -- for M < 1 the two diverge (Linear also factors in B), which is the whole point of
	 * offering it as a distinct mode.
	 */
	float ApplyMaskBlendMode(const float Base, const float Mask, const EVertexMaskForgeBlendMode Mode)
	{
		switch (Mode)
		{
		case EVertexMaskForgeBlendMode::Copy:
			return Mask;
		case EVertexMaskForgeBlendMode::Add:
			return Base + Mask;
		case EVertexMaskForgeBlendMode::Subtract:
			return Base - Mask;
		case EVertexMaskForgeBlendMode::Multiply:
			return Base * Mask;
		case EVertexMaskForgeBlendMode::Overlay:
			return (Base < 0.5f)
				? (2.0f * Base * Mask)
				: (1.0f - 2.0f * (1.0f - Base) * (1.0f - Mask));
		case EVertexMaskForgeBlendMode::Screen:
			return 1.0f - (1.0f - Base) * (1.0f - Mask);
		case EVertexMaskForgeBlendMode::Linear:
			return FMath::Lerp(Base, Mask, Mask);
		default:
			return Mask;
		}
	}

	/**
	 * AUDITED (peer-mask composition checkpoint): the Opacity lerp, WITHOUT any clamp. Renamed from
	 * BlendMaskValue (which used to clamp every call) -- clamping every single-layer step is exactly
	 * what breaks the associativity/commutativity proofs the multi-mask composition below relies on
	 * (demonstrated numerically in the checkpoint report: Add-then-Subtract vs Subtract-then-Add only
	 * agree if intermediate saturation is never clamped away mid-fold). Clamping now happens ONLY at
	 * the two audited boundaries inside ComposeStack -- see that function's own doc comment.
	 */
	float BlendMaskValueUnclamped(const float Base, const float Mask, const EVertexMaskForgeBlendMode Mode, const float Opacity)
	{
		const float BlendResult = ApplyMaskBlendMode(Base, Mask, Mode);
		return FMath::Lerp(Base, BlendResult, Opacity);
	}

	/**
	 * AUDITED (peer-mask composition checkpoint -- supersedes the previous, UI-position-ordered fold
	 * this same function used to implement). Combines an UNORDERED set of mask generators (Bounding
	 * Box, Ambient Occlusion, future Curvature/Thickness/...) into ONE result per channel, starting
	 * from BaselineColor's own channel value ("Base") -- never a neutral 1.0/0.0 seed, and Base is
	 * NEVER itself a peer entry in Layers (no generator, no Enable, no Invert, no Blend Mode, no
	 * Opacity of its own) -- it is simply the accumulator the canonical-order operations below act on.
	 * Proof this must be Base (not a neutral seed): a single Multiply-mode mask at Opacity 0 must
	 * leave the channel completely unchanged ("Opacity 0 = no influence", the pre-existing, still-
	 * approved contract) -- lerp(Base, Base*Mask, 0) = Base only holds when the fold's own base
	 * already equals Base; lerp(1, 1*Mask, 0) = 1 would incorrectly turn the channel white instead.
	 *
	 * CANONICAL ORDER (approved convention -- internal, mathematical, never a UI/artistic stack; see
	 * the checkpoint report for the full derivation and proofs): every enabled mask is processed in
	 * FIXED STAGES, by Blend Mode, in this exact sequence -- Copy, {Add+Subtract combined}, Multiply,
	 * Overlay, Screen, Linear (the EVertexMaskForgeBlendMode enum's own declaration order, with
	 * Add/Subtract merged into one stage). This ordering is NEVER configurable, NEVER derived from
	 * section position in the UI, and NEVER derived from the order masks were enabled in -- the ORDER
	 * of Layers itself (as received from the caller) is what tie-breaks two masks sharing the exact
	 * same Blend Mode (the caller sorts by a fixed, stable generator identifier before resolving
	 * samples -- see VertexMaskForgePanel::ComposeMaskStack -- this function itself never sorts).
	 *
	 * Per stage:
	 *   - Copy: sequential fold (BlendMaskValueUnclamped), Layers order -- proven NON-
	 *     commutative even among only Copy-mode masks (two different Opacity<1 Copy layers do not
	 *     commute), so this is the one stage with no closed-form reduction.
	 *   - Add+Subtract: closed form, order-irrelevant -- Result = R + Sum(AddValue*AddOpacity) -
	 *     Sum(SubValue*SubOpacity). Proven: BlendMaskValueUnclamped(R,M,Add,Op) = R + M*Op and
	 *     (R,M,Subtract,Op) = R - M*Op are both purely additive per-term, so summing every Add/Subtract
	 *     mask's own term, in ANY order, reproduces the exact same total. No clamp within this stage
	 *     (per the checkpoint's explicit instruction) -- the running total may legitimately leave
	 *     [0,1] here.
	 *   - Multiply: closed form, order-irrelevant -- Result = R * Product(lerp(1,MaskValue,Opacity)).
	 *     Proven algebraically associative/commutative among Multiply-mode masks regardless of R's own
	 *     magnitude (the proof needs no assumption that R is normalized), so it safely consumes
	 *     whatever the Add/Subtract stage produced, in or out of [0,1], with no clamp beforehand.
	 *   - CLAMP BOUNDARY: R is clamped to [0,1] here, and ONLY here (plus the final defensive clamp) --
	 *     this is the one clamp the checkpoint's audit proved is actually required: Overlay's branch
	 *     (R < 0.5), and Screen/Linear's (1-R)-based formulas, are only proven to map [0,1] back into
	 *     [0,1] when their OWN input is already in [0,1] (see the checkpoint report's worked
	 *     Add/Subtract-then-Overlay example, which shows what happens without this clamp: Overlay
	 *     receiving R=1.4 produces 1.56, compounding out-of-domain error instead of correcting it).
	 *   - Overlay: sequential fold (BlendMaskValueUnclamped), Layers order -- proven NON-
	 *     commutative (worked counter-example in the checkpoint report: swapping two Overlay masks
	 *     changes the result from 0.84 to 0.36 for the same inputs).
	 *   - Screen: closed form, order-irrelevant -- (1-Result) = (1-R) * Product(1-MaskValue*Opacity).
	 *     Proven algebraically (De Morgan dual of Multiply). Input is already in [0,1] from the clamp
	 *     boundary above (and Overlay's own output stays in [0,1] given [0,1] input -- proven), so no
	 *     further clamp is needed before this stage.
	 *   - Linear: sequential fold (BlendMaskValueUnclamped), Layers order -- proven NON-
	 *     commutative (f(B,M) != f(M,B) whenever B != M and B+M != 1). Proven to stay in [0,1] given
	 *     [0,1] input (B(1-M)+M^2 in [0,1] for B,M in [0,1]), so still no clamp needed entering this
	 *     stage; a final defensive clamp still closes out the whole channel computation.
	 *
	 * AUDITED (non-accumulation preserved): Base is re-read from BaselineColor fresh on EVERY single
	 * call (never from a previous WorkingColors/Accumulator value -- see
	 * VertexMaskForgePanel::UpdateWorkingColors' own doc comment), so regenerating the SAME set of
	 * masks with the SAME parameters always reproduces the EXACT same final result, never drifting
	 * across repeated recompositions.
	 *
	 * A "Fill/Constant" layer is handled entirely by the CALLER (ApplyPreviewToEntry) as a single
	 * Copy@1.0 layer, never combined with any other generator in the same pass -- this function has no
	 * special knowledge of Fill/Constant sources at all; it just sees one Copy-mode resolved sample in
	 * that case, same as any other generator would look if configured that way.
	 *
	 * Channels NOT enabled in the Channel Filter (bFilterR/G/B false) are read verbatim from
	 * CommittedColor -- untouched by any layer or stage. Alpha is always BaselineColor.W,
	 * unconditionally. bOutAnyLayerContributed is true iff Layers is non-empty -- the caller (which
	 * already only includes a layer once its own TryGetValue() succeeded for this vertex/corner) uses
	 * it to decide whether to write this vertex's R/G/B into WorkingColors at all, or leave it exactly
	 * as the CommittedColors copy left it.
	 */
	FVector4f ComposeStack(
		const FVector4f& BaselineColor,
		const FVector4f& CommittedColor,
		TArrayView<const FResolvedMaskLayer> Layers,
		const bool bFilterR, const bool bFilterG, const bool bFilterB,
		bool& bOutAnyLayerContributed)
	{
		bOutAnyLayerContributed = !Layers.IsEmpty();

		if (!bOutAnyLayerContributed)
		{
			return FVector4f(CommittedColor.X, CommittedColor.Y, CommittedColor.Z, BaselineColor.W);
		}

		auto ComposeChannel = [Layers](const float Base) -> float
		{
			float R = Base;

			// Stage 1: Copy -- sequential fold, Layers order (non-commutative, no closed form).
			for (const FResolvedMaskLayer& C : Layers)
			{
				if (C.BlendMode == EVertexMaskForgeBlendMode::Copy)
				{
					R = BlendMaskValueUnclamped(R, C.MaskValue, EVertexMaskForgeBlendMode::Copy, C.Opacity);
				}
			}

			// Stage 2: Add + Subtract -- closed form, order-irrelevant. No clamp within this stage.
			for (const FResolvedMaskLayer& C : Layers)
			{
				if (C.BlendMode == EVertexMaskForgeBlendMode::Add)
				{
					R += C.MaskValue * C.Opacity;
				}
				else if (C.BlendMode == EVertexMaskForgeBlendMode::Subtract)
				{
					R -= C.MaskValue * C.Opacity;
				}
			}

			// Stage 3: Multiply -- closed form, order-irrelevant. Safely consumes an out-of-[0,1] R
			// from Stage 2 (the algebraic proof needs no [0,1] assumption on R).
			for (const FResolvedMaskLayer& C : Layers)
			{
				if (C.BlendMode == EVertexMaskForgeBlendMode::Multiply)
				{
					R *= FMath::Lerp(1.0f, C.MaskValue, C.Opacity);
				}
			}

			// CLAMP BOUNDARY (audited, required): Overlay/Screen/Linear's own formulas are only
			// proven to stay in [0,1] when their input already is -- see this function's own doc
			// comment for the worked counter-example without this clamp.
			R = FMath::Clamp(R, 0.0f, 1.0f);

			// Stage 4: Overlay -- sequential fold, Layers order (non-commutative, no closed form).
			for (const FResolvedMaskLayer& C : Layers)
			{
				if (C.BlendMode == EVertexMaskForgeBlendMode::Overlay)
				{
					R = BlendMaskValueUnclamped(R, C.MaskValue, EVertexMaskForgeBlendMode::Overlay, C.Opacity);
				}
			}

			// Stage 5: Screen -- closed form (De Morgan dual of Multiply), order-irrelevant. No
			// additional clamp needed: R is already in [0,1] from the boundary above, and Overlay's
			// own output stays in [0,1] given [0,1] input (proven).
			for (const FResolvedMaskLayer& C : Layers)
			{
				if (C.BlendMode == EVertexMaskForgeBlendMode::Screen)
				{
					R = 1.0f - (1.0f - R) * (1.0f - C.MaskValue * C.Opacity);
				}
			}

			// Stage 6: Linear -- sequential fold, Layers order (non-commutative, no closed form).
			for (const FResolvedMaskLayer& C : Layers)
			{
				if (C.BlendMode == EVertexMaskForgeBlendMode::Linear)
				{
					R = BlendMaskValueUnclamped(R, C.MaskValue, EVertexMaskForgeBlendMode::Linear, C.Opacity);
				}
			}

			// Final defensive clamp (float precision only -- every stage above is already proven to
			// leave R in [0,1] by this point under normal inputs).
			return FMath::Clamp(R, 0.0f, 1.0f);
		};

		return FVector4f(
			bFilterR ? ComposeChannel(BaselineColor.X) : CommittedColor.X,
			bFilterG ? ComposeChannel(BaselineColor.Y) : CommittedColor.Y,
			bFilterB ? ComposeChannel(BaselineColor.Z) : CommittedColor.Z,
			BaselineColor.W);
	}
}
