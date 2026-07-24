#pragma once

#include "CoreMinimal.h"

/** Result of attempting to generate a scalar mask on a working mesh. */
enum class EVertexMaskForgeScalarMaskState : uint8
{
	/** No generation has been attempted yet, or a prior result was invalidated. */
	NotGenerated,

	/** Generation succeeded and the values below are safe to use. */
	Ready,

	/** No valid working mesh (or no vertices) was available to generate from. */
	Unavailable,

	/** The mesh's local-space extent along the mask's axis is too small to normalize safely. */
	DegenerateBounds,

	/** Generation ran but produced values that failed validation (non-finite, out of [0,1], or a
	 *  valid-value count mismatch). Surfaced rather than hidden; never auto-corrected. */
	Invalid,
};

/** Which generator produced a FVertexMaskForgeScalarMask -- purely for UI/diagnostic labeling. */
enum class EVertexMaskForgeScalarMaskSource : uint8
{
	/** GenerateBoundingBoxMask: combination (by maximum) of up to 3 independent axis gradients. */
	BoundingBox,

	/** GenerateConstantMask(1.0): every render vertex set to 1.0 (Fill White). */
	ConstantWhite,

	/** GenerateConstantMask(0.0): every render vertex set to 0.0 (Fill Black). */
	ConstantBlack,

	/**
	 * GenerateAmbientOcclusionMask: per-render-vertex hemisphere-raycast occlusion fraction.
	 *
	 * AUDITED (AO Levels + vanilla inversion checkpoint): RawAO (the raw cached raycast fraction,
	 * unchanged: 0.0 = exposed, 1.0 = fully occluded/cavity) is no longer what composition/Preview
	 * ever sees directly. VertexMaskForgePanel::ApplyAOLevelsAndInvert (the single, shared, pure
	 * post-processing step both domains call) first flips it into BaseAO = 1 - RawAO -- i.e. the
	 * VANILLA convention is now the common "black = occluded" texture-baking one, unconditionally,
	 * independent of FVertexMaskForgeAOParams::bInvert (which keeps its own default/false meaning:
	 * "invert the new vanilla result", not "toggle between old and new vanilla"). Levels Min/Max are
	 * then applied to BaseAO, and bInvert is applied LAST, over the leveled result. See
	 * ApplyAOLevelsAndInvert's own doc comment for the exact pipeline and rationale.
	 */
	AmbientOcclusion,

	/**
	 * GenerateCurvatureMask / GenerateCurvatureMaskFromDynamicMesh: a signed, topology-based curvature
	 * measure (discrete mean-curvature-normal approximation from adjacent face dihedral angles),
	 * reduced by Curvature Type (Convex/Concave/Both -- see EVertexMaskForgeCurvatureType), scaled by
	 * Multiplier, smoothed by a topological Blur, and remapped by Levels Min/Max -- see
	 * VertexMaskForgePanel::ApplyCurvatureArtisticParams for the exact pipeline. Values are always in
	 * [0, 1] (0 = flat/no curvature of the selected Type, 1 = maximum). Computed ONCE PER ENTRY (never
	 * per component -- unlike Bounding Box's World Space axes and Ambient Occlusion, Curvature is a
	 * pure function of the asset's own local-space topology and never depends on a component's
	 * transform), so every component of the same asset shares the identical Curvature contribution.
	 */
	Curvature,

	/**
	 * GenerateNoiseMask / GenerateNoiseMaskFromDynamicMesh: a procedural 3D Perlin/FBM noise sampled at
	 * each vertex's own LOCAL-SPACE position (see EVertexMaskForgeNoiseType and
	 * FVertexMaskForgeNoiseGenerativeParams), reduced 0-1, scaled by Multiplier, remapped by Levels
	 * Min/Max, and optionally inverted -- see VertexMaskForgePanel::ApplyNoiseArtisticParams for the
	 * exact pipeline. Computed ONCE PER ENTRY (never per component -- Noise, like Curvature, is a pure
	 * function of the asset's own local-space geometry and never depends on a component's transform),
	 * so every component of the same asset shares the identical Noise contribution.
	 */
	Noise,

	/**
	 * GenerateMaterialSlotMask / GenerateMaterialSlotMaskFromDynamicMesh (V2-D): a binary mask -- 1.0
	 * for every corner/render vertex resolved to belong to the chosen Static Material Slot, 0.0
	 * everywhere else (optionally complemented by Invert) -- see VertexMaskForgePanel::
	 * GetMaterialSlotForCorner/BuildMaterialSlotLookups for the exact Polygon Group -> real slot
	 * resolution. A true generator/layer like Curvature/Noise (participates in ComposeMaskStack with
	 * its own Blend Mode/Opacity), NOT a global eligibility filter -- it never suppresses or restores
	 * baseline colors on its own; a slot the user wants untouched is achieved artistically, by the
	 * user's own Blend Mode choice (e.g. Multiply), exactly like any other mask. Corner-EXACT in the
	 * Source-Topology domain (never collapsed to Dynamic Mesh Vertex ID, unlike Curvature/Noise) so two
	 * corners sharing a position/VertexID on opposite sides of a material boundary can correctly read
	 * different values. Computed ONCE PER ENTRY (transform-independent, like Curvature/Noise).
	 */
	MaterialSlot,

	/**
	 * GenerateDirectionalNormalMask / GenerateDirectionalNormalMaskFromDynamicMesh (V2-E): a smooth
	 * angular mask -- 1.0 where the surface normal is aligned with the chosen axis direction (within
	 * Angle degrees, optionally with a smoothstep Falloff transition), 0.0 outside that cone, optionally
	 * complemented by Invert -- see VertexMaskForgePanel::ComputeDirectionalNormalRawValue for the exact
	 * formula. A true generator/layer like Curvature/Noise/MaterialSlot. LOCAL Space is transform-
	 * independent (computed ONCE PER ENTRY, like Curvature/Noise); WORLD Space depends on each
	 * component's own transform (re-evaluated PER COMPONENT, like Bounding Box's own World Space axes
	 * and Ambient Occlusion -- see ApplyPreviewToEntry). Appended here (after MaterialSlot, not in
	 * visual panel order) to preserve every existing enum value's numeric stability -- see this enum's
	 * own append-only contract.
	 */
	DirectionalNormal,

	/**
	 * GenerateThicknessMask / GenerateThicknessMaskFromDynamicMesh (V2-G): a smooth mask driven by the
	 * measured local-space distance to the nearest qualifying opposite surface along -Normal (thin =
	 * white, thick = black, before Invert) -- see VertexMaskForgePanel::ComputeThicknessRawValue for the
	 * exact raycast/normalization pipeline. Asset Local Space ONLY -- never depends on any component's
	 * transform, so unlike Directional Normal World Space this has no per-instance conflict concept.
	 * Appended here (after DirectionalNormal, not in visual panel order) per this enum's own append-only
	 * contract.
	 */
	Thickness,
};

/**
 * How a mask layer's scalar value (M) combines with the channel's existing value (B) before Opacity
 * is applied. Panel-level today (SVertexMaskForgePanel::BoundingBoxBlendMode), shared by the one
 * Bounding Box Mask layer that exists in this checkpoint; the per-layer field this enum would
 * belong to in a future mask stack (see VertexMaskForgePanel::ComposeMaskLayer's doc comment) does
 * not exist yet -- see that function for the "prepared for, not implemented" boundary.
 *
 * All formulas below operate on already-normalized [0,1] scalars and are the RAW blend result
 * (see VertexMaskForgePanel::ApplyMaskBlendMode) -- Opacity and the final clamp are applied
 * afterwards, uniformly, by VertexMaskForgePanel::BlendMaskValue, never per-mode.
 */
enum class EVertexMaskForgeBlendMode : uint8
{
	/** R = M. With Opacity 1.0, exactly reproduces the tool's pre-Blend-Mode behavior (the mask
	 *  value replaces the channel outright). Default. */
	Copy,

	/** R = B + M (no intermediate clamp -- see BlendMaskValue). */
	Add,

	/** R = B - M (no intermediate clamp -- see BlendMaskValue). */
	Subtract,

	/** R = B * M. */
	Multiply,

	/** R = (B < 0.5) ? (2*B*M) : (1 - 2*(1-B)*(1-M)). */
	Overlay,

	/** R = 1 - (1-B) * (1-M). */
	Screen,

	/** R = lerp(B, M, M). Deliberately NOT an alias of Copy -- see ApplyMaskBlendMode. */
	Linear,
};

/** Forward declaration only -- FVertexMaskForgeMaskLayerParams below holds a non-owning pointer to
 *  it, never a value member, so the full definition (which stays in SVertexMaskForgePanel.h for this
 *  checkpoint) is not required here. */
struct FVertexMaskForgeScalarMask;

namespace VertexMaskForgePanel
{
	/**
	 * ONE mask generator's contribution to the composition -- Bounding Box, Ambient Occlusion, and any
	 * future generator (Curvature, Thickness, ...) are STRUCTURALLY IDENTICAL peers here: this struct
	 * carries no notion of "spatial" vs "content", no fixed role, and no UI-derived position. Already
	 * resolved to the PER-COMPONENT effective mask for this call (World Space Bounding Box axes and
	 * Ambient Occlusion are both re-evaluated per component by the caller -- see ApplyPreviewToEntry --
	 * before being wrapped here). Mask is a non-owning pointer into a caller-owned
	 * FVertexMaskForgeScalarMask that must outlive the UpdateWorkingColors call it is passed to.
	 * Mask->Source doubles as the fixed, stable "generator identifier" used to break ties between two
	 * masks sharing the same Blend Mode (see ComposeMaskStack) -- never reused for anything else, never
	 * exposed in the UI, never configurable.
	 */
	struct FVertexMaskForgeMaskLayerParams
	{
		const FVertexMaskForgeScalarMask* Mask = nullptr;
		EVertexMaskForgeBlendMode BlendMode = EVertexMaskForgeBlendMode::Copy;
		float Opacity = 1.0f;

		/**
		 * AUDITED (Nanite source-topology support): when >= 0, ComposeMaskStack looks up THIS value
		 * instead of its own shared VertexIndex parameter for this one layer -- needed because Source-
		 * Topology masks are not all indexed by the same domain (Bounding Box by Dynamic Mesh Vertex ID;
		 * Ambient Occlusion by Normal Overlay Element ID, to preserve hard-edge AO correctness -- see
		 * GenerateAmbientOcclusionMaskFromDynamicMesh's own doc comment). -1 (default) preserves the
		 * original, single-shared-index behavior every existing (render-vertex domain) caller already
		 * relies on -- unchanged, since every layer there is indexed identically by render vertex index.
		 */
		int32 IndexOverride = -1;
	};
}

/**
 * What the artist currently sees in the viewport for Vertex Color preview purposes.
 * Purely a session/panel-level setting -- never saved on the UStaticMesh, never affects the
 * Bounding Box Mask or its per-axis parameters.
 */
enum class EVertexMaskForgePreviewMode : uint8
{
	/** Restore the mesh's own materials; no debug visualization is shown. */
	OriginalMaterial,

	/** Show the temporarily composed RGBA color (mask blended into the Channel Filter's channels). */
	RGBVertexColor,

	/** Show the composed color's Red channel as grayscale (R, R, R, 1). */
	RedChannel,

	/** Show the composed color's Green channel as grayscale (G, G, G, 1). */
	GreenChannel,

	/** Show the composed color's Blue channel as grayscale (B, B, B, 1). */
	BlueChannel,

	/** Show the composed color's Alpha channel as grayscale (A, A, A, 1) -- visualization only, never
	 *  alters the real RGBA values held in the composed Preview/Accept data. */
	AlphaChannel,
};
