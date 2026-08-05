#pragma once

#include "Containers/StaticArray.h"
#include "CoreMinimal.h"
#include "Misc/Guid.h"
#include "Misc/TVariant.h"
#include "VertexMaskForgeMaskTypes.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

/**
 * M16-A: recipe data model foundations for the future Fill Layer / Mask Stack architecture (see the
 * M15/M15.1/M15.2/M15.2.1 architectural audits for the full derivation). Every type in this header is
 * AUTHORING STATE ONLY -- identity, generator selection, parameters, and composition weights, exactly
 * as an artist would set them. None of it is consumed by any runtime path yet: the existing fixed
 * panel/generator/ComposeStack flow is completely unmodified and remains the only thing driving the
 * tool's actual behavior. No evaluator, result storage, or cache exists yet.
 *
 * AUDITED (M15.2.1, corrected): recipe types never contain generated/derived data. MaskValue,
 * EffectiveMask, and Coverage are per-vertex, per-entry-or-component values that a future evaluator
 * computes from these parameters -- they have no single scalar representation that could live here,
 * and none is stored here.
 */

/** Identifies which of the seven existing mask generators a Mask Instance wraps. Deliberately
 *  excludes Fill -- Fill is Fill Layer content (FVertexMaskForgeFillLayer::FillValue), never a Mask
 *  Instance. */
enum class EVertexMaskForgeGeneratorType : uint8
{
	BoundingBox,
	AmbientOcclusion,
	Curvature,
	Noise,
	DirectionalNormal,
	Thickness,
	MaterialSlot,
};

// --- Per-generator authoring parameter structs ---------------------------------------------------
// Each struct holds ONLY that generator's current content/authoring parameters, reproducing the
// exact defaults SVertexMaskForgePanel uses today (verified against source, not assumed). Blend
// Mode, Opacity, and Enabled are Mask Instance concerns, never generator-parameter concerns -- see
// FVertexMaskForgeMaskInstance below.

/** Mirrors SVertexMaskForgePanel::BoundingBoxAxisParams / bUseUnifiedBounds. */
struct FVertexMaskForgeBoundingBoxParams
{
	/** One entry per EVertexMaskForgeBoundsAxis (X/Y/Z) -- reuses the existing, already
	 *  authoring-only FVertexMaskForgeAxisMaskParams verbatim (the same type
	 *  SVertexMaskForgePanel::BoundingBoxAxisParams already uses). */
	TStaticArray<FVertexMaskForgeAxisMaskParams, 3> Axes;

	bool bUseUnifiedBounds = false;
};

/** Mirrors SVertexMaskForgePanel::AOSamples/AOMaxDistance/AOBias/bAOInvert/AOLevelsMin/AOLevelsMax.
 *  Deliberately NOT a reuse of the existing FVertexMaskForgeAOParams (VertexMaskForgeWorkingMeshTypes.h)
 *  -- that struct's own default Samples value (64) does not match the panel's actual current default
 *  (16); reusing it verbatim would silently give a new Mask Instance the wrong default. */
struct FVertexMaskForgeAmbientOcclusionParams
{
	int32 Samples = 16;
	float MaxDistance = 100.0f;
	float Bias = 0.1f;
	bool bInvert = false;
	float LevelsMin = 0.0f;
	float LevelsMax = 1.0f;
};

/** Mirrors SVertexMaskForgePanel::CurvatureType/Multiplier/Blur/LevelsMin/LevelsMax/bInvert. */
struct FVertexMaskForgeCurvatureParams
{
	EVertexMaskForgeCurvatureType Type = EVertexMaskForgeCurvatureType::Concave;
	float Multiplier = 1.0f;
	float Blur = 0.0f;
	float LevelsMin = 0.0f;
	float LevelsMax = 1.0f;
	bool bInvert = false;
};

/** Mirrors SVertexMaskForgePanel's Noise* fields. bNoiseScaleAxesLocked is deliberately excluded --
 *  it is documented at its own panel declaration as a UI-only convenience (drives Scale Y/Z from
 *  Scale X) that is "NOT part of" the generative parameter set, since the generated result is fully
 *  determined by the final Scale X/Y/Z values alone. */
struct FVertexMaskForgeNoiseParams
{
	EVertexMaskForgeNoiseType Type = EVertexMaskForgeNoiseType::FractalPerlin;
	float ScaleX = 1.0f;
	float ScaleY = 1.0f;
	float ScaleZ = 1.0f;
	float OffsetX = 0.0f;
	float OffsetY = 0.0f;
	float OffsetZ = 0.0f;
	int32 Seed = 0;
	int32 Octaves = 4;
	float Roughness = 0.5f;
	float Lacunarity = 2.0f;
	float TurbulenceStrength = 0.5f;
	float Multiplier = 1.0f;
	float Blur = 0.0f;
	float LevelsMin = 0.0f;
	float LevelsMax = 1.0f;
	bool bInvert = false;
};

/** Mirrors SVertexMaskForgePanel::DirectionalNormalSpace/Direction/Angle/Falloff/Blur/
 *  bDirectionalNormalMaskInvert. Space's default is Local -- the field initializer, not the doc
 *  comment beside it (which claims "World"), is what actually runs; the comment is stale in the
 *  source this checkpoint audits, and this struct reproduces the real (Local) default per the
 *  "use the field initializer as ground truth" discipline. */
struct FVertexMaskForgeDirectionalNormalParams
{
	EVertexMaskForgeNormalSpace Space = EVertexMaskForgeNormalSpace::Local;
	EVertexMaskForgeNormalDirection Direction = EVertexMaskForgeNormalDirection::PositiveZ;
	float Angle = 90.0f;
	float Falloff = 45.0f;
	float Blur = 0.0f;
	bool bInvert = false;
};

/** Mirrors SVertexMaskForgePanel::ThicknessMinThickness/MaxThickness/SearchDistance/Bias/Blur/
 *  bThicknessMaskInvert. LevelsMin/LevelsMax (M17-TH-DL-E) are Dynamic-Layers-only additions --
 *  Legacy's own raw panel fields have no Levels equivalent for Thickness -- mirroring the same
 *  field names/defaults/[0,1] semantics already established by FVertexMaskForgeAmbientOcclusionParams/
 *  FVertexMaskForgeCurvatureParams/FVertexMaskForgeNoiseParams above. Applied as a postprocess AFTER
 *  Invert (Invert is baked into the protected backend's own return value before the Dynamic
 *  composition orchestrator ever sees it, unlike AO/Curvature/Noise where the same generator file
 *  owns both steps and orders Levels before Invert -- see VertexMaskForgeDynamicSourceTopologyComposition.cpp's
 *  own Thickness branch for the exact justification). */
struct FVertexMaskForgeThicknessParams
{
	float MinThickness = 0.0f;
	float MaxThickness = 50.0f;
	float SearchDistance = 100.0f;
	float Bias = 0.01f;
	float Blur = 0.0f;
	bool bInvert = false;
	float LevelsMin = 0.0f;
	float LevelsMax = 1.0f;
};

/** Mirrors SVertexMaskForgePanel::SelectedMaterialSlotIndex/bMaterialSlotMaskInvert -- same
 *  index-into-the-selected-mesh's-own-slot-list representation
 *  VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMask already takes today (an existing,
 *  not a new, limitation: the index is only meaningful relative to one specific mesh's material
 *  list, unchanged by this checkpoint). */
struct FVertexMaskForgeMaterialSlotParams
{
	int32 SelectedSlotIndex = 0;
	bool bInvert = false;
};

// --- Tagged union over the seven param structs ---------------------------------------------------

/** Holds exactly one of the seven generators' parameter structs. Never construct or assign this
 *  directly when a specific EVertexMaskForgeGeneratorType is intended -- use
 *  MakeVertexMaskForgeGeneratorParams() or FVertexMaskForgeMaskInstance::Make() so the active
 *  alternative always stays coherent with whatever GeneratorType is stored alongside it. */
using FVertexMaskForgeGeneratorParams = TVariant<
	FVertexMaskForgeBoundingBoxParams,
	FVertexMaskForgeAmbientOcclusionParams,
	FVertexMaskForgeCurvatureParams,
	FVertexMaskForgeNoiseParams,
	FVertexMaskForgeDirectionalNormalParams,
	FVertexMaskForgeThicknessParams,
	FVertexMaskForgeMaterialSlotParams>;

/** Constructs a FVertexMaskForgeGeneratorParams holding the correct, default-initialized alternative
 *  for Type -- the sanctioned way to produce a GeneratorType/active-alternative pairing that is
 *  guaranteed coherent. */
inline FVertexMaskForgeGeneratorParams MakeVertexMaskForgeGeneratorParams(const EVertexMaskForgeGeneratorType Type)
{
	FVertexMaskForgeGeneratorParams Params;
	switch (Type)
	{
	case EVertexMaskForgeGeneratorType::BoundingBox:
		Params.Emplace<FVertexMaskForgeBoundingBoxParams>();
		break;
	case EVertexMaskForgeGeneratorType::AmbientOcclusion:
		Params.Emplace<FVertexMaskForgeAmbientOcclusionParams>();
		break;
	case EVertexMaskForgeGeneratorType::Curvature:
		Params.Emplace<FVertexMaskForgeCurvatureParams>();
		break;
	case EVertexMaskForgeGeneratorType::Noise:
		Params.Emplace<FVertexMaskForgeNoiseParams>();
		break;
	case EVertexMaskForgeGeneratorType::DirectionalNormal:
		Params.Emplace<FVertexMaskForgeDirectionalNormalParams>();
		break;
	case EVertexMaskForgeGeneratorType::Thickness:
		Params.Emplace<FVertexMaskForgeThicknessParams>();
		break;
	case EVertexMaskForgeGeneratorType::MaterialSlot:
	default:
		Params.Emplace<FVertexMaskForgeMaterialSlotParams>();
		break;
	}
	return Params;
}

/**
 * One mask generator's authoring configuration within a Fill Layer's Mask Stack. Contains identity,
 * generator selection, that generator's own parameters, and composition weights -- never a generated
 * result, cache, or per-vertex value (see this header's own module comment).
 */
struct FVertexMaskForgeMaskInstance
{
	/** Stable identity, independent of array position -- never regenerated on copy (copying this
	 *  struct preserves InstanceId; producing a new, distinct instance is a future, explicit
	 *  operation via Make(), never an implicit side effect of copying). */
	FGuid InstanceId;

	EVertexMaskForgeGeneratorType GeneratorType = EVertexMaskForgeGeneratorType::BoundingBox;

	/** Must stay coherent with GeneratorType -- see Make() and MakeVertexMaskForgeGeneratorParams(). */
	FVertexMaskForgeGeneratorParams Params;

	/** Intra-stack composition mode -- how THIS instance's value combines with the other Mask
	 *  Instances in the same Fill Layer's stack (a future sequential evaluator, not ComposeStack).
	 *  Copy is the deliberate default for a newly created instance: at the approved seed=1 ("full
	 *  coverage") starting value, Copy is immediately meaningful and predictable, unlike Add/Screen/
	 *  Overlay, which can be inert until a preceding operation reduces coverage below 1. */
	EVertexMaskForgeBlendMode BlendMode = EVertexMaskForgeBlendMode::Copy;

	/** Intra-stack opacity -- the Lerp weight of this instance's own operation, distinct from a
	 *  future Fill Layer's own Opacity (which governs the whole layer's blend onto the composite
	 *  below, not one mask's blend within its own stack). */
	float Opacity = 1.0f;

	/** AUDITED (M16-I.1): a disabled Mask Instance is treated as semantically ABSENT by resolution and
	 *  generation -- ignored before InstanceId validation, before any result-store lookup, and before
	 *  GeneratorType/Params are ever inspected (see VertexMaskForgeFillLayerResolution::
	 *  EvaluateFillLayerFromKeyedResults and VertexMaskForgeRecipeInstanceGeneration::
	 *  GenerateRequiredInstanceResultsForRecipe). Its own InstanceId/GeneratorType/Params can therefore
	 *  never cause a failure, and it never requires or triggers generation. */
	bool bEnabled = true;

	/** The sanctioned way to create a new Mask Instance: a fresh, unique identity, a coherent
	 *  GeneratorType/Params pairing, and that generator's own default parameters. Default-constructing
	 *  this struct directly (e.g. for container use) is legal and always coherent too (GeneratorType
	 *  defaults to BoundingBox, and TVariant default-constructs its first alternative, which is also
	 *  FVertexMaskForgeBoundingBoxParams) -- it simply leaves InstanceId as an invalid/zero FGuid,
	 *  which Make() is what actually mints a real, unique one. */
	static FVertexMaskForgeMaskInstance Make(const EVertexMaskForgeGeneratorType Type)
	{
		FVertexMaskForgeMaskInstance Instance;
		Instance.InstanceId = FGuid::NewGuid();
		Instance.GeneratorType = Type;
		Instance.Params = MakeVertexMaskForgeGeneratorParams(Type);
		return Instance;
	}
};

/**
 * One Fill Layer: content (FillValue) plus the ordered Mask Stack that will future control where it
 * applies, plus this layer's own composition weights relative to the layers below it. Contains no
 * generated result (EffectiveMask/Coverage/composited color) -- see this header's own module comment.
 */
struct FVertexMaskForgeFillLayer
{
	/** Stable identity, independent of array position -- same "never regenerated on copy" contract
	 *  as FVertexMaskForgeMaskInstance::InstanceId. */
	FGuid LayerId;

	/** RGB only -- Alpha is deliberately not exposed here. Alpha continues to be preserved by the
	 *  existing BaselineColors/CommittedColors lifecycle unchanged in the first milestone; exposing an
	 *  editable Alpha the pipeline cannot yet honor would be dishonest. Default white, matching the
	 *  "FillValue=White reproduces legacy content behavior" derivation. */
	FVector3f FillValue = FVector3f(1.0f, 1.0f, 1.0f);

	/** Inter-layer composition mode -- how this layer's Coverage-gated result blends onto the
	 *  composite of every layer below it (a future evaluator, never ComposeStack -- see the M15.2
	 *  audit's rejection of reusing ComposeStack's stage-grouped wrapper for this purpose). */
	EVertexMaskForgeBlendMode BlendMode = EVertexMaskForgeBlendMode::Copy;

	/** Inter-layer opacity -- distinct from any one Mask Instance's own Opacity within MaskStack. */
	float Opacity = 1.0f;

	bool bEnabled = true;

	/** Visual (array) order is the future evaluation order -- no Blend-Mode-based stage regrouping,
	 *  unlike the legacy ComposeStack. Empty MaskStack means the layer applies uniformly
	 *  (EffectiveMask=1 for every vertex, per the approved empty-stack contract) -- a future evaluator
	 *  concern, not represented by any field here. */
	TArray<FVertexMaskForgeMaskInstance> MaskStack;

	/** The sanctioned way to create a new Fill Layer: a fresh, unique identity and every other field
	 *  at its documented default. */
	static FVertexMaskForgeFillLayer Make()
	{
		FVertexMaskForgeFillLayer Layer;
		Layer.LayerId = FGuid::NewGuid();
		return Layer;
	}
};

/**
 * The session-global authoring recipe: an ordered list of Fill Layers, applied identically to every
 * currently-selected entry (per the approved product decision -- results remain per-entry/
 * per-component, never the recipe itself). Session-only in this milestone: no version number, no
 * serialization, no save/load, no UObject, no transaction support. Owning this is a future
 * (orchestration-checkpoint) decision, not made by this header.
 */
struct FVertexMaskForgeRecipe
{
	TArray<FVertexMaskForgeFillLayer> FillLayers;
};
