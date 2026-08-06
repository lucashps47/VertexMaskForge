#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"
#include "Misc/Optional.h"
#include "VertexMaskForgeMaskTypes.h"
#include "VertexMaskForgeRecipeTypes.h"

/**
 * M16-K.3A: explicit representation of a dynamic layer's Fill content. None is NOT a placeholder or a
 * disabled/zero-opacity trick -- it means "this layer has no content of its own yet" (a brand new layer,
 * or one whose mask has been removed without a Fill set). Black/White are the only two scalar Fill
 * values this checkpoint defines; there is no arbitrary-color Fill and no FVector3f FillValue here --
 * resolving Fill (and the future "mask present, no Fill" / "mask absent, EffectiveMask == 1" cases) into
 * an actual composed value is explicitly deferred to M16-K.3B. Nothing in this enum or in
 * FVertexMaskForgeLayer below is read by any composition/evaluator/bridge code yet.
 */
enum class EVertexMaskForgeLayerFill : uint8
{
	/** No content -- the layer does not contribute. A freshly added layer starts here. */
	None,

	/** Scalar Fill value 0.0, once M16-K.3B resolves it. */
	Black,

	/** Scalar Fill value 1.0, once M16-K.3B resolves it. */
	White,
};

/**
 * M16-K.3A: one entry in the future dynamic layer stack (see FVertexMaskForgeDynamicLayerStack). Pure
 * value data -- no Slate, no generator/mask reference, no cache, no evaluation. LayerId is the only
 * identity; position is never stored here, only implied by this layer's index inside the owning stack's
 * array (see FVertexMaskForgeDynamicLayerStack's own doc comment on why no OrderIndex field exists).
 *
 * All fields are public, mirroring VertexMaskForgePanel::FVertexMaskForgeMaskLayerParams' own
 * fully-public, no-role, structurally-identical-peers convention (VertexMaskForgeMaskTypes.h).
 *
 * M16-K.3A corrective audit: LayerId being a public field is safe specifically because
 * FVertexMaskForgeDynamicLayerStack never exposes a mutable pointer/reference to an element it owns --
 * every public accessor (FindLayerById, GetLayers) is const-only, so external code can only ever read
 * LayerId, never reassign it or replace the whole layer via `*Ptr = OtherLayer` (the implicitly-defined
 * copy-assignment operator would copy every member, private or not, so making LayerId private alone,
 * without also removing mutable element access, would NOT have closed this). A caller building its own,
 * separate FVertexMaskForgeLayer value (e.g. an "expected" value in a test) may of course set any field
 * freely -- that is just constructing ordinary value data, not mutating the stack's own internal state.
 * RenameLayer exists specifically so Name can be changed without any mutable element access being
 * exposed; there is no controller-owned mutation for the other composition fields yet (Fill/BlendMode/
 * Opacity/channels) because nothing in this checkpoint reads or invalidates them -- see
 * FVertexMaskForgeDynamicLayerStack's own doc comment.
 *
 * Selection (which layer a UI/controller currently has active) is deliberately NOT a field here or on
 * the stack -- see FVertexMaskForgeDynamicLayerStack's own doc comment.
 */
/**
 * M16-K.5B: identity + configuration for ONE procedural mask generator attached to a Dynamic Layer.
 * Deliberately NOT a reuse of FVertexMaskForgeMaskInstance (VertexMaskForgeRecipeTypes.h) -- that struct
 * belongs to the separate Recipe/Mask-Stack authoring model and carries its own BlendMode/Opacity/
 * bEnabled (intra-stack composition weights meaningful only when multiple Mask Instances combine within
 * one Fill Layer's own Mask Stack). A Dynamic Layer has no Mask Stack of its own -- at most one mask per
 * layer -- so those three fields would be redundant with, and could conflict with, FVertexMaskForgeLayer's
 * OWN BlendMode/Opacity/bEnabled (which already govern how the whole layer's resolved content combines
 * with the composite below it). This struct is PURELY configurational: identity + which generator +
 * that generator's own parameters, and nothing else.
 *
 * GeneratorType/Params reuse EVertexMaskForgeGeneratorType/FVertexMaskForgeGeneratorParams
 * (VertexMaskForgeRecipeTypes.h) verbatim, unmodified -- the same 7-generator enum and TVariant tagged
 * union the Recipe model already established and one real generator (Material Slot) already consumes in
 * production. No new enum, no new variant, no duplicated parameter structs.
 *
 * Never contains: a cache, a generated/derived scalar value, a fingerprint, a Working Mesh reference, a
 * UObject/component/asset reference, a Slate type, or a persistent pointer/reference of any kind -- see
 * this struct's own field list, which is exhaustive.
 */
struct FVertexMaskForgeGeneratorMaskInstance
{
	/** Stable identity, independent of LayerId -- see FVertexMaskForgeDynamicLayerStack::
	 *  SetLayerMaskGeneratorType's own doc comment for exactly when a fresh id is minted (assign, or
	 *  replace with a different GeneratorType) versus preserved (same-type reassignment is a no-op;
	 *  rename/reorder/move never touch it). Never regenerated by copy -- copying a
	 *  FVertexMaskForgeGeneratorMaskInstance (e.g. as part of copying its owning layer) preserves
	 *  MaskInstanceId, exactly mirroring FVertexMaskForgeMaskInstance::InstanceId's own "never
	 *  regenerated on copy" contract. */
	FGuid MaskInstanceId;

	/** Which of the 7 real generators this instance wraps. No None/absent value exists on this enum by
	 *  design (VertexMaskForgeRecipeTypes.h) -- "no mask" is represented by the owning layer's
	 *  TOptional<FVertexMaskForgeGeneratorMaskInstance> being unset, never by a sentinel enumerator here. */
	EVertexMaskForgeGeneratorType GeneratorType = EVertexMaskForgeGeneratorType::BoundingBox;

	/** Must stay coherent with GeneratorType -- always produced via MakeVertexMaskForgeGeneratorParams(),
	 *  never hand-assembled with a mismatched alternative. Purely inert configuration this checkpoint:
	 *  nothing reads it, evaluates it, or executes the generator it describes. */
	FVertexMaskForgeGeneratorParams Params = MakeVertexMaskForgeGeneratorParams(EVertexMaskForgeGeneratorType::BoundingBox);
};

struct FVertexMaskForgeLayer
{
	/** Stable identity, assigned once by FVertexMaskForgeDynamicLayerStack::AddLayer via FGuid::NewGuid()
	 *  and never regenerated by any stack operation (rename/move/etc. all preserve it). Never Source, an
	 *  enum, or an index. */
	FGuid LayerId;

	/** User-facing label. FString (not FText) -- this is session/domain data, not localized UI text; the
	 *  future controller/UI layer is responsible for any FText wrapping. Duplicates across layers are
	 *  explicitly allowed (identity is LayerId, never Name) -- see
	 *  FVertexMaskForgeDynamicLayerStackTests.cpp's DuplicateNamesRemainDistinct. Empty string is also
	 *  explicitly allowed; no domain-level naming policy is imposed here. */
	FString Name;

	/** Whether this layer currently contributes to composition. Independent of Fill -- a layer can be
	 *  Enabled with Fill=None (contributes nothing, but for a different reason: absence of content, not
	 *  a deliberate off switch). Composition semantics for the bEnabled/Fill interaction are for M16-K.3B
	 *  to define; this checkpoint only stores the bit. */
	bool bEnabled = true;

	/** See EVertexMaskForgeLayerFill. Defaults to None -- a freshly constructed/added layer is empty and
	 *  neutral, never defaulting to White or relying on Opacity 0 / bEnabled=false as a substitute. */
	EVertexMaskForgeLayerFill Fill = EVertexMaskForgeLayerFill::None;

	/** Reuses the plugin's existing, already-generic EVertexMaskForgeBlendMode (VertexMaskForgeMaskTypes.h)
	 *  -- no new blend-mode enum is introduced here; the seven modes (Copy/Add/Subtract/Multiply/Overlay/
	 *  Screen/Linear) are already layer-agnostic and already the type VertexMaskForgeSequentialEvaluator
	 *  consumes. Default Copy, matching every other per-layer BlendMode default in the codebase. */
	EVertexMaskForgeBlendMode BlendMode = EVertexMaskForgeBlendMode::Copy;

	/** Finite [0, 1] contract (see FVertexMaskForgeDynamicLayerStack::IsValid, which checks both the range
	 *  and finiteness -- NaN/Infinity are rejected, never silently accepted -- but does not clamp on
	 *  write: this field is public, so an out-of-contract value can be constructed directly; it will only
	 *  be caught by an explicit IsValid() call, exactly like every other public-field domain struct in
	 *  this plugin (e.g. FVertexMaskForgeMaskLayerParams::Opacity). No clamping is performed by this
	 *  checkpoint's evaluator, because this checkpoint does not touch the evaluator at all. */
	float Opacity = 1.0f;

	/** Per-layer Channel Filter -- explicit defaults, all RGB channels affected. Mirrors the panel's
	 *  existing global bChannelFilterR/G/B bools (SVertexMaskForgePanel.h), now per-layer instead of
	 *  per-panel; see the M16-K.3R audit's Channel Filter migration plan for how these two are meant to
	 *  coexist during the transition. */
	bool bAffectRed = true;
	bool bAffectGreen = true;
	bool bAffectBlue = true;

	/** M19-A: per-layer Alpha ownership, the fourth Channel Filter bit. Defaults to false -- unlike
	 *  bAffectRed/Green/Blue, a freshly added layer does NOT affect Alpha by default. This is intentional
	 *  and asymmetric: every pre-M19 layer, and every layer created before the artist explicitly opts in
	 *  via the future M19-B UI, must leave Alpha exactly as untouched as it was before Alpha became a real
	 *  composed channel (see VertexMaskForgeDynamicSourceTopologyComposition.cpp's Pass 2 fold). The
	 *  artist-facing checkbox and Alt+click isolation for this bit are deferred to M19-B; this checkpoint
	 *  only makes the bit real and wires it through composition. */
	bool bAffectAlpha = false;

	/** M16-K.5B: this layer's optional procedural mask generator instance -- owned exclusively by value,
	 *  by this layer (never a pointer/reference, never a second parallel container elsewhere). Unset
	 *  (TOptional empty) means "no mask", the default for every newly-added layer, including the Base
	 *  Layer -- see FVertexMaskForgeDynamicLayerStack::MakeInitialStack/AddLayer, neither of which sets
	 *  this. Copying/moving a FVertexMaskForgeLayer copies/moves this field along with every other one --
	 *  no code anywhere needs to keep a second structure in sync, because there isn't one. Purely inert
	 *  this checkpoint: VertexMaskForgeDynamicLayerEvaluator::EvaluateColor does not read this field (see
	 *  its own doc comment) -- EffectiveMask remains implicitly 1.0 regardless of whether a layer carries
	 *  a mask instance. */
	TOptional<FVertexMaskForgeGeneratorMaskInstance> Mask;
};

/**
 * M16-K.4A: the pure, Slate-free CHANNEL-EXCLUSIVITY DECISION behind the Dynamic Layers UI's discreet
 * per-row tint (SVertexMaskForgePanel::GetDynamicLayerChannelTint, the only caller). Deliberately factored
 * out of the panel so it is directly unit-testable without constructing any Slate widget -- the panel-side
 * function is a thin wrapper that resolves a layer by LayerId and maps this enum to the actual
 * presentation FSlateColor/alpha (a UI concern, kept out of this Slate-free header).
 *
 * Exactly one active channel (bAffect* true, the other three false) resolves to that channel's own value;
 * zero, two, three, or four active channels resolve to Default -- never a combined/blended color. This is
 * pure derivation from the four flags only; it has no knowledge of FVertexMaskForgeLayer, FGuid,
 * FVertexMaskForgeDynamicLayerStack, or any domain identity -- purely a function of (bool, bool, bool, bool).
 *
 * M19-B: implements the FUTURE DECISION this enum's own comment previously only documented -- an
 * Alpha-only layer (R=G=B=false, A=true) resolves to White, analogous to how exactly-one-of-RGB resolves
 * to that channel's color; every other combination that includes Alpha alongside any RGB channel (or
 * alongside another disabled/enabled mix) still resolves to Default, exactly like every other non-exclusive
 * RGB combination already did.
 */
enum class EVertexMaskForgeDynamicLayerChannelTint : uint8
{
	/** Zero, two, three, or four of R/G/B/A active -- the row's normal, untinted appearance. */
	Default,

	/** Exactly R active. */
	Red,

	/** Exactly G active. */
	Green,

	/** Exactly B active. */
	Blue,

	/** Exactly A active (R=G=B=false, A=true). */
	White,
};

/** Pure derivation -- see EVertexMaskForgeDynamicLayerChannelTint's own doc comment for the full contract. */
inline EVertexMaskForgeDynamicLayerChannelTint ResolveDynamicLayerChannelTint(const bool bAffectRed, const bool bAffectGreen, const bool bAffectBlue, const bool bAffectAlpha)
{
	const int32 ActiveChannelCount = (bAffectRed ? 1 : 0) + (bAffectGreen ? 1 : 0) + (bAffectBlue ? 1 : 0) + (bAffectAlpha ? 1 : 0);
	if (ActiveChannelCount != 1)
	{
		return EVertexMaskForgeDynamicLayerChannelTint::Default;
	}

	if (bAffectRed)
	{
		return EVertexMaskForgeDynamicLayerChannelTint::Red;
	}
	if (bAffectAlpha)
	{
		return EVertexMaskForgeDynamicLayerChannelTint::White;
	}
	if (bAffectGreen)
	{
		return EVertexMaskForgeDynamicLayerChannelTint::Green;
	}
	if (bAffectBlue)
	{
		return EVertexMaskForgeDynamicLayerChannelTint::Blue;
	}
	// ActiveChannelCount == 1 and bAffectRed/bAffectGreen/bAffectBlue/bAffectAlpha are all accounted for
	// above -- this line is unreachable, kept only as a defensive, never-taken fallback.
	return EVertexMaskForgeDynamicLayerChannelTint::Default;
}

/**
 * M16-K.4B: pure, Slate-free literal color for the four EXCLUSIVE-channel tint kinds only (FLinearColor
 * is a Core, not Slate, type -- no widget/style dependency here). Deliberately does NOT handle
 * EVertexMaskForgeDynamicLayerChannelTint::Default -- the correct default/neutral presentation color is
 * FStyleColors::Panel (a real Slate style color, requiring SlateCore), chosen specifically to reproduce
 * SBorder's pre-M16-K.4A "Border" brush appearance exactly (see SVertexMaskForgePanel::
 * GetDynamicLayerChannelTint's own doc comment for the full root-cause explanation of why the default
 * must be Panel and must NOT be White once the row's BorderImage is "WhiteBrush") -- that substitution
 * happens at the one real call site, not here, so this Slate-free header is never given a reason to
 * depend on SlateCore/Styling. Calling this with Default is a caller error; it returns transparent black
 * defensively rather than crashing, but no real call site should ever do so.
 *
 * M19-B: White (Alpha-only) uses the EXACT SAME TintAlpha parameter as Red/Green/Blue -- there is no
 * separate Alpha tint opacity constant anywhere in this function or its caller.
 */
inline FLinearColor GetDynamicLayerChannelTintColor(const EVertexMaskForgeDynamicLayerChannelTint Tint, const float TintAlpha)
{
	switch (Tint)
	{
	case EVertexMaskForgeDynamicLayerChannelTint::Red:
		return FLinearColor(1.0f, 0.0f, 0.0f, TintAlpha);
	case EVertexMaskForgeDynamicLayerChannelTint::Green:
		return FLinearColor(0.0f, 1.0f, 0.0f, TintAlpha);
	case EVertexMaskForgeDynamicLayerChannelTint::Blue:
		return FLinearColor(0.0f, 0.0f, 1.0f, TintAlpha);
	case EVertexMaskForgeDynamicLayerChannelTint::White:
		return FLinearColor(1.0f, 1.0f, 1.0f, TintAlpha);
	case EVertexMaskForgeDynamicLayerChannelTint::Default:
	default:
		return FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
	}
}

/** M16-K.4C: identifies which of a Dynamic Layer's four Channel Filter checkboxes was clicked -- used
 *  only to parameterize ResolveDynamicLayerChannelToggle below, never stored, never a substitute for
 *  LayerId/FGuid identity. M19-A: extended with Alpha; the toggle-helper contract below is symmetric
 *  across all four channels. The artist-facing Alpha checkbox itself remains deferred to M19-B -- adding
 *  this enumerator only makes the pure decision helper Alpha-capable ahead of that widget existing. */
enum class EVertexMaskForgeDynamicLayerChannel : uint8
{
	Red,
	Green,
	Blue,
	Alpha,
};

/** Pure output of ResolveDynamicLayerChannelToggle -- the final Channel Filter state to apply via
 *  FVertexMaskForgeDynamicLayerStack::SetLayerChannelFilter (a single, already-atomic call.) M19-A:
 *  bAffectAlpha defaults to false here, matching FVertexMaskForgeLayer::bAffectAlpha's own default --
 *  never true, since this result type only ever mirrors an existing layer's current state or a computed
 *  toggle outcome, never an independent "fresh" default. */
struct FVertexMaskForgeDynamicLayerChannelToggleResult
{
	bool bAffectRed = true;
	bool bAffectGreen = true;
	bool bAffectBlue = true;
	bool bAffectAlpha = false;
};

/**
 * M16-K.4C: pure, Slate-free Channel Solo decision -- Substance-Painter-style Alt-click behavior for a
 * Dynamic Layer's R/G/B/A Channel Filter checkboxes. Deliberately separate from both Slate (no widget/
 * modifier-key type appears here -- bAltDown is a plain bool the caller already resolved) and from
 * FVertexMaskForgeDynamicLayerStack (this function never touches a layer or a stack; it only computes
 * what SHOULD be applied, via the existing SetLayerChannelFilter call, at the one real call site in
 * SVertexMaskForgePanel::BuildDynamicLayerRow).
 *
 * M19-A: extended from three channels (R/G/B) to four (R/G/B/A), symmetrically -- Alpha participates in
 * exactly the same normal-toggle and Alt-isolate rules as R/G/B below. This function owns ONLY channel
 * ownership state; it has no knowledge of, and must never gain knowledge of, Preview Mode, mesh display
 * colors, viewport presentation, or the layer-row tint (EVertexMaskForgeDynamicLayerChannelTint) -- the
 * row-tint derivation and the future Alpha checkbox widget are both deferred to M19-B.
 *
 * Contract:
 *   - bAltDown == false (normal click): ClickedChannel is set to bRequestedChecked (the checkbox's own
 *     new state); the other three channels are returned UNCHANGED from bCurrentRed/Green/Blue/Alpha.
 *     Identical to the pre-M16-K.4C behavior -- multiple channels, or zero channels, remain fully
 *     reachable.
 *   - bAltDown == true (Alt-click, "solo"): ClickedChannel is forced to true; the other three are forced
 *     to false -- REGARDLESS of bRequestedChecked. This is the specific rule that makes Alt-clicking an
 *     already-solo channel a no-op rather than a toggle-off: since SCheckBox always requests the OPPOSITE
 *     of its current checked state on click, Alt-clicking a channel that is already the sole active one
 *     would otherwise request bRequestedChecked=false for it -- this function ignores that request
 *     entirely under Alt and forces ClickedChannel to true unconditionally, so the row can never end up
 *     with zero active channels from an Alt-click.
 * Always returns exactly one of the two following shapes: normal-click's "one channel changed, three
 * preserved", or Alt-click's "exactly the clicked channel true, the other three false" -- never a partial
 * or ambiguous state.
 */
inline FVertexMaskForgeDynamicLayerChannelToggleResult ResolveDynamicLayerChannelToggle(
	const bool bCurrentRed, const bool bCurrentGreen, const bool bCurrentBlue, const bool bCurrentAlpha,
	const EVertexMaskForgeDynamicLayerChannel ClickedChannel,
	const bool bRequestedChecked,
	const bool bAltDown)
{
	FVertexMaskForgeDynamicLayerChannelToggleResult Result;

	if (bAltDown)
	{
		Result.bAffectRed = (ClickedChannel == EVertexMaskForgeDynamicLayerChannel::Red);
		Result.bAffectGreen = (ClickedChannel == EVertexMaskForgeDynamicLayerChannel::Green);
		Result.bAffectBlue = (ClickedChannel == EVertexMaskForgeDynamicLayerChannel::Blue);
		Result.bAffectAlpha = (ClickedChannel == EVertexMaskForgeDynamicLayerChannel::Alpha);
		return Result;
	}

	Result.bAffectRed = bCurrentRed;
	Result.bAffectGreen = bCurrentGreen;
	Result.bAffectBlue = bCurrentBlue;
	Result.bAffectAlpha = bCurrentAlpha;
	switch (ClickedChannel)
	{
	case EVertexMaskForgeDynamicLayerChannel::Red:
		Result.bAffectRed = bRequestedChecked;
		break;
	case EVertexMaskForgeDynamicLayerChannel::Green:
		Result.bAffectGreen = bRequestedChecked;
		break;
	case EVertexMaskForgeDynamicLayerChannel::Blue:
		Result.bAffectBlue = bRequestedChecked;
		break;
	case EVertexMaskForgeDynamicLayerChannel::Alpha:
		Result.bAffectAlpha = bRequestedChecked;
		break;
	}
	return Result;
}
