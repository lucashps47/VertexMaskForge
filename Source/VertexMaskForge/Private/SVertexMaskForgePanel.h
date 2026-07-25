#pragma once

#include "Containers/BitArray.h"
#include "Containers/StaticArray.h"
#include "CoreMinimal.h"
#include "Delegates/IDelegateInstance.h"
#include "EditorUndoClient.h"
#include "Engine/TimerHandle.h"
#include "Math/Vector4.h"
#include "MeshTypes.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtr.h"
#include "VertexMaskForgeMaskTypes.h"
#include "Widgets/SCompoundWidget.h"

class UWorld;

class AActor;
class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;
class UDynamicMeshComponent;
template <typename OptionType> class SComboBox;
enum class ECheckBoxState : uint8;
namespace ESelectInfo { enum Type : int; }

namespace UE::Geometry { class FDynamicMesh3; }

/**
 * Opaque per-component Ambient Occlusion cache (world-space-baked occluder geometry + spatial index
 * + last-computed raw per-render-vertex AO values), owned exclusively by one
 * FVertexMaskForgePreviewComponentState. Forward-declared here and fully defined in the .cpp, exactly
 * mirroring FVertexMaskForgeWorkingMesh::Mesh's TUniquePtr<FDynamicMesh3> pattern -- this header never
 * needs GeometryCore's DynamicMeshAABBTree3.h to be complete, only SVertexMaskForgePanel.cpp (which
 * VertexMaskForgeModule.cpp never bypasses; it only ever constructs/destroys SVertexMaskForgePanel
 * itself, whose own destructor is already declared here and defined there for the same reason -- see
 * VertexMaskForgePanel::GenerateAmbientOcclusionMask for the cache's actual contents/contract).
 */
struct FVertexMaskForgeAOCache;

/** Sibling of FVertexMaskForgeAOCache for Source-Topology (Nanite) entries -- see
 *  VertexMaskForgePanel::GenerateAmbientOcclusionMaskFromDynamicMesh in the .cpp for the full contract. */
struct FVertexMaskForgeSourceTopologyAOCache;

/**
 * Opaque Thickness cache (Asset Local Space -- NEVER keyed by ComponentTransform, unlike AOCache).
 * Owned by FVertexMaskForgeWorkingMesh (per-entry/per-asset, not per-component), since Thickness never
 * depends on instance transform. Forward-declared here, fully defined in the .cpp -- see
 * VertexMaskForgePanel::GenerateThicknessMask for the full contract (local spatial tree + raw hit
 * distances + a freshness snapshot compared against the CURRENT asset at Accept pre-write time).
 */
struct FVertexMaskForgeThicknessCache;

/** Sibling of FVertexMaskForgeThicknessCache for Source-Topology (Nanite) entries -- see
 *  VertexMaskForgePanel::GenerateThicknessMaskFromDynamicMesh in the .cpp for the full contract. */
struct FVertexMaskForgeSourceTopologyThicknessCache;

/**
 * Explicit state of the Pending Changes workflow (Accept/Cancel). Never inferred merely from
 * whether a PreviewComponent exists -- see SVertexMaskForgePanel::RecomputeOperationState().
 */
enum class EVertexMaskForgeOperationState : uint8
{
	/** No valid, acceptable Preview exists (no active Preview Mode, or no Ready mask being shown). */
	Idle,

	/** A valid mask is displayed in the transient Preview and can be Accepted. */
	PendingChanges,

	/** Accept is currently writing to Static Mesh asset(s). Synchronous in this checkpoint, but
	 *  modeled explicitly so no other operation (auto-update, another Accept) can interleave. */
	Applying,

	/** The last Accept attempt was blocked or failed; the Preview that was attempted is preserved
	 *  unchanged. Cleared by the next explicit user action (regenerate, Accept, Cancel). */
	Failed,
};

/** Coverage state of the LOD 0 Color Vertex Buffer. */
enum class EVertexMaskForgeVertexColorState : uint8
{
	/** No Color Vertex Buffer, or it has zero entries. */
	None,

	/** Color Vertex Buffer present and its vertex count matches LOD 0's vertex count. */
	Present,

	/** Color Vertex Buffer present but its vertex count does not match LOD 0's vertex count. */
	PartialOrInvalid,
};

/**
 * Read-only technical diagnostics for a single Static Mesh, gathered from its
 * render data. Computed once per refresh; never mutates the mesh.
 */
struct FVertexMaskForgeMeshDiagnostics
{
	/** True once the diagnostics below were successfully computed from valid render data. */
	bool bValid = false;

	int32 NumLODs = 0;
	int32 LOD0NumVertices = 0;
	int32 LOD0NumTriangles = 0;
	int32 NumMaterialSlots = 0;

	EVertexMaskForgeVertexColorState VertexColorState = EVertexMaskForgeVertexColorState::None;
	int32 LOD0NumColorVertices = 0;

	bool bNaniteEnabled = false;
	bool bAllowCPUAccess = false;
};

/**
 * One entry of a mesh's Material Slot list (V2-D), for the Material Slot Mask dropdown. SlotIndex is
 * the REAL, authoritative index into UStaticMesh::GetStaticMaterials() -- the identity used both for
 * lookups and for display; never inferred from name or position. MaterialSlotName/MaterialAssetName
 * are display-only (names can duplicate or be NAME_None -- see VertexMaskForgePanel::
 * GetMaterialSlotLabel).
 */
struct FVertexMaskForgeMaterialSlotInfo
{
	int32 SlotIndex = INDEX_NONE;
	FName MaterialSlotName = NAME_None;
	/** "None" if the slot's material is null. */
	FString MaterialAssetName;
};

/** Result of attempting to build a transient working copy of a mesh's LOD 0. */
enum class EVertexMaskForgeWorkingMeshState : uint8
{
	/** The working mesh was built and is safe to use. */
	Ready,

	/** UStaticMesh::GetMeshDescription(0) returned null. */
	SourceMeshDescriptionUnavailable,

	/** A MeshDescription was obtained, but conversion to FDynamicMesh3 did not produce a usable mesh. */
	ConversionFailed,

	/** The soft-referenced Static Mesh could not be resolved to a valid object. */
	InvalidSource,
};

/** Whether the Dynamic Mesh's per-triangle Material ID attribute mirrors the source's polygon groups. */
enum class EVertexMaskForgeMaterialIDState : uint8
{
	/** The working mesh has no Material ID attribute (only possible if the working copy is not Ready). */
	Unavailable,

	/** The working mesh has no Material ID attribute despite being Ready. */
	Missing,

	/** The working mesh has a Material ID attribute carried over from the source's polygon groups. */
	Preserved,
};

/**
 * Presence state of the working mesh's primary color overlay.
 * Ground truth for Present/Missing/Invalid comes from the LOD 0 RenderData diagnostic
 * (FVertexMaskForgeMeshDiagnostics::VertexColorState), not from whether the converter happened to
 * keep its overlay -- FMeshDescriptionToDynamicMesh::Convert() drops the overlay whenever every
 * source color exactly equals the attribute default (white), which must NOT be conflated with a
 * genuinely colorless source. See ValidateWorkingMesh() / ReconstructOmittedColorOverlay().
 */
enum class EVertexMaskForgeWorkingVertexColorState : uint8
{
	/** No color overlay (only possible if the working copy is not Ready). */
	Unavailable,

	/** RenderData has no Color Vertex Buffer at all: the source genuinely has no vertex colors. */
	Missing,

	/** RenderData's Color Vertex Buffer count does not match LOD 0's vertex count; not auto-corrected. */
	Invalid,

	/**
	 * RenderData proves a full Color Vertex Buffer exists. Elements may all be white -- that is
	 * still Present, including cases where the overlay had to be reconstructed because Convert()
	 * dropped it for being uniformly default-valued.
	 */
	Present,
};

/**
 * Aggregate statistics over a color overlay's elements. Never stores individual colors.
 * Tolerance-based comparisons use FVertexMaskForgeWorkingMesh::ColorChannelTolerance.
 */
struct FVertexMaskForgeColorStats
{
	int32 NumElements = 0;

	FVector4f MinColor = FVector4f(1.f, 1.f, 1.f, 1.f);
	FVector4f MaxColor = FVector4f(0.f, 0.f, 0.f, 0.f);

	/** Elements whose RGB differs from white (1,1,1) by more than the tolerance, on any channel. */
	int32 NumNonWhite = 0;

	/** Elements whose RGB differs from black (0,0,0) by more than the tolerance, on any channel. */
	int32 NumNonBlack = 0;
};

/** Local vs. World Space for VertexMaskForgePanel::ComputeDirectionalNormalRawValue's surface normal --
 *  Local is asset-relative (transform-independent); World transforms the normal by the selected
 *  component's own ComponentToWorld (see GenerateDirectionalNormalMask's own doc comment for the exact,
 *  non-uniform-scale-safe transform used). */
enum class EVertexMaskForgeNormalSpace : uint8
{
	Local,
	World,
};

/** One of the six principal axis directions a surface normal can be compared against -- Unreal's own
 *  convention (X+ Forward, Y+ Right, Z+ Up). See VertexMaskForgePanel::GetNormalDirectionVector for the
 *  exact unit vectors. */
enum class EVertexMaskForgeNormalDirection : uint8
{
	PositiveX,
	NegativeX,
	PositiveY,
	NegativeY,
	PositiveZ,
	NegativeZ,
};

/** Which of FVertexMaskForgeWorkingMesh::CurvatureRawConvexCache/CurvatureRawConcaveCache (or their
 *  union) Curvature Type selects as the mask magnitude -- see
 *  VertexMaskForgePanel::ApplyCurvatureArtisticParams. */
enum class EVertexMaskForgeCurvatureType : uint8
{
	/** Mask = RawConvexCurvature -- convex edges/bulges only; concave regions read as 0 (black). */
	Convex,

	/** Mask = RawConcaveCurvature -- concave cavities/creases only; convex regions read as 0 (black). */
	Concave,

	/** Mask = max(RawConvexCurvature, RawConcaveCurvature) -- a union of the two INDEPENDENTLY
	 *  accumulated magnitude arrays (see FVertexMaskForgeWorkingMesh::CurvatureRawConvexCache's own doc
	 *  comment); convex and concave NEVER cancel each other out, since neither is ever computed as a
	 *  single signed sum in the first place. Default. */
	Both,
};

/** Which procedural noise VertexMaskForgePanel::ComputeRawNoiseValue evaluates -- see that function's
 *  own doc comment for the exact formulas. */
enum class EVertexMaskForgeNoiseType : uint8
{
	/** A single FMath::PerlinNoise3D sample, remapped from [-1,1] to [0,1]. */
	Perlin,

	/** Fractal Brownian Motion: a weighted sum of several Perlin octaves at increasing frequency and
	 *  decreasing amplitude (see Octaves/Roughness/Lacunarity), normalized by the total amplitude
	 *  weight BEFORE the signed-to-[0,1] remap. Default. */
	FractalPerlin,

	/** Billow (V2-A): like FractalPerlin, but each octave's Perlin sample is folded to
	 *  2*abs(Perlin)-1 (still signed) before accumulation -- see
	 *  VertexMaskForgePanel::EvaluateBillow for the exact formula. Multi-octave (Octaves/Roughness/
	 *  Lacunarity apply). */
	Billow,

	/** Ridged (V2-A): each octave contributes pow(1-abs(Perlin), 2), summed directly in [0,1] space
	 *  (no signed remap at the end) -- see VertexMaskForgePanel::EvaluateRidged for the exact formula.
	 *  Multi-octave (Octaves/Roughness/Lacunarity apply). */
	Ridged,

	/** Turbulence (V2-A): domain-warped FractalPerlin -- three FBM-derived warp signals (at fixed,
	 *  mutually-separated constant offsets) displace the sample position by TurbulenceStrength before a
	 *  final FBM evaluation -- see VertexMaskForgePanel::EvaluateTurbulence for the exact formula.
	 *  Multi-octave (Octaves/Roughness/Lacunarity apply); also uses NoiseTurbulenceStrength. */
	Turbulence,

	/** Worley F1 (V2-B): the Euclidean distance, in noise-space cellular units, to the nearest feature
	 *  point of a 3D cellular pattern -- see VertexMaskForgePanel::EvaluateCellularNoise for the shared
	 *  feature-point layout and VertexMaskForgePanel::ComputeRawNoiseValue for the exact RawMask
	 *  formula. Not multi-octave (Octaves/Roughness/Lacunarity/Turbulence Strength are unused). */
	WorleyF1,

	/** Worley F2-F1 (V2-B): the difference between the SECOND- and first-nearest feature-point
	 *  distances of the SAME cellular pattern as WorleyF1 -- produces cell-edge structures, near zero at
	 *  cell boundaries. Not multi-octave. */
	WorleyF2MinusF1,

	/** Voronoi (V2-B): a solid, per-region deterministic value -- every point sharing the same nearest
	 *  feature point (the SAME cellular pattern as WorleyF1/WorleyF2MinusF1) receives EXACTLY the same
	 *  hashed value; changes only when crossing into a different region, never as a function of
	 *  distance. Not multi-octave. */
	Voronoi,

	/** Alligator (V2-C): a distinct celullar type built from the DIFFERENCE between the two largest
	 *  radial-basis-function contributions of the same feature-point layout (never a Worley distance,
	 *  never Voronoi's solid hash) -- see VertexMaskForgePanel::EvaluateBaseAlligator/EvaluateAlligator
	 *  for the exact formulas. Multi-octave (Octaves/Roughness/Lacunarity apply, same contract as
	 *  FractalPerlin/Billow/Ridged/Turbulence). */
	Alligator,
};

/**
 * AUDITED (Noise V1): the GENERATIVE half of the Noise mask's parameters -- everything that changes
 * WHAT the raw procedural pattern actually looks like, as opposed to how it is post-processed
 * artistically (Multiplier/Levels/Invert/Opacity/Blend Mode, which live directly on the panel and are
 * never part of this struct). Snapshotted onto FVertexMaskForgeWorkingMesh::NoiseCacheUsedParams at raw
 * generation time and compared field-by-field on every subsequent live regeneration pass (see
 * VertexMaskForgePanel::EnsureNoiseRawCache) -- ANY difference (including a GeometryFingerprint change)
 * forces a full recompute of NoiseRawCache; an exact match reuses it verbatim, zero re-evaluation.
 */
struct FVertexMaskForgeNoiseGenerativeParams
{
	EVertexMaskForgeNoiseType NoiseType = EVertexMaskForgeNoiseType::FractalPerlin;

	/** Per-axis frequency multiplier. UI range [Epsilon, large]; default 1.0 each. See
	 *  VertexMaskForgePanel::ComputeRawNoiseValue for the exact NoisePosition formula (Scale/100, so
	 *  Scale 1 is approximately one noise unit per meter). */
	float ScaleX = 1.0f;
	float ScaleY = 1.0f;
	float ScaleZ = 1.0f;

	/** Domain offset, in NOISE SPACE (already post-Scale, post-SeedOffset) -- default 0 each. */
	float OffsetX = 0.0f;
	float OffsetY = 0.0f;
	float OffsetZ = 0.0f;

	/** Hashed deterministically into a 3D domain offset (VertexMaskForgePanel::ComputeNoiseSeedOffset)
	 *  -- never FMath::Rand/FRandomStream, never global state. Default 0. */
	int32 Seed = 0;

	/** FractalPerlin only. UI range [1, 8]; default 4. */
	int32 Octaves = 4;

	/** FractalPerlin only: per-octave amplitude multiplier. UI range [0, 1]; default 0.5. */
	float Roughness = 0.5f;

	/** FractalPerlin/Billow/Ridged/Turbulence only: per-octave frequency multiplier. UI minimum 1.0;
	 *  default 2.0. */
	float Lacunarity = 2.0f;

	/** Turbulence only: domain-warp displacement strength, in noise space. UI range [0, 5]; default 0.5.
	 *  Harmless/unused for every other Noise Type (mirrors Octaves/Roughness/Lacunarity's own
	 *  "harmless when Perlin" contract) but still part of the cache key below, so switching a mesh in or
	 *  out of Turbulence, or tweaking Strength while already on Turbulence, is never missed. */
	float TurbulenceStrength = 0.5f;

	/** V2-C: universal pre-Multiplier blur radius factor (Radius = Blur*0.5, noise-space units) --
	 *  applies to ALL nine Noise Types (see VertexMaskForgePanel::ComputeRawNoiseValue's seven-tap
	 *  kernel). GENERATIVE, not artistic: it requires re-evaluating the procedural field at six extra
	 *  offset positions, so it belongs in the raw cache key exactly like Scale/Offset/Seed, never treated
	 *  as a cheap downstream reprocessing step like Multiplier/Levels/Invert. UI range [0, 1]; default
	 *  0.0 -- Blur <= 0 takes an exact short-circuit path (see ComputeRawNoiseValue) that reproduces
	 *  every existing Noise Type's pre-V2-C output bit-for-bit. */
	float Blur = 0.0f;

	bool operator==(const FVertexMaskForgeNoiseGenerativeParams& Other) const
	{
		return NoiseType == Other.NoiseType
			&& ScaleX == Other.ScaleX && ScaleY == Other.ScaleY && ScaleZ == Other.ScaleZ
			&& OffsetX == Other.OffsetX && OffsetY == Other.OffsetY && OffsetZ == Other.OffsetZ
			&& Seed == Other.Seed
			&& Octaves == Other.Octaves && Roughness == Other.Roughness && Lacunarity == Other.Lacunarity
			&& TurbulenceStrength == Other.TurbulenceStrength
			&& Blur == Other.Blur;
	}
	bool operator!=(const FVertexMaskForgeNoiseGenerativeParams& Other) const { return !(*this == Other); }
};

/**
 * Panel-level parameters for the Ambient Occlusion mask source (SVertexMaskForgePanel::AOSamples/
 * AOMaxDistance/AOBias/bAOInvert), snapshotted onto FVertexMaskForgeScalarMask::UsedAOParams at
 * generation time -- exactly the same "snapshot for diagnostics, composition only reads Values"
 * relationship FVertexMaskForgeAxisMaskParams already has via UsedAxisParams. Clamped defensively
 * again inside VertexMaskForgePanel::GenerateAmbientOcclusionMask itself (never trusts the UI clamp
 * alone -- see that function's own doc comment).
 */
struct FVertexMaskForgeAOParams
{
	/** Hemisphere sample count per render vertex. UI range [8, 256]; default 64. */
	int32 Samples = 64;

	/** Maximum occluder search distance, in Unreal units (World Space -- see
	 *  VertexMaskForgePanel::GenerateAmbientOcclusionMask for why this must be World Space, not Local,
	 *  under non-uniform component scale). UI range (0, 10000]; default 100.0. */
	float MaxDistance = 100.0f;

	/** Ray origin offset along the vertex normal, in Unreal units (same World Space as MaxDistance),
	 *  applied BEFORE the acceleration-structure query so it never requires rebuilding the tree. UI
	 *  range [0.001, 10.0]; default 0.1. */
	float Bias = 0.1f;

	/**
	 * AUDITED (AO Levels + vanilla inversion): user-facing Invert, applied LAST, AFTER Levels Min/Max
	 * -- see VertexMaskForgePanel::ApplyAOLevelsAndInvert for the exact pipeline (RawAO -> BaseAO ->
	 * Levels -> this). Never requires recomputing raw samples or rebuilding the tree -- purely
	 * compositional, like Levels below.
	 */
	bool bInvert = false;

	/**
	 * AUDITED (AO Levels): values in the vanilla BaseAO (see ApplyAOLevelsAndInvert) at or below this
	 * threshold become 0 (black). UI range [0, 1]; default 0.0 (no compression at the low end).
	 * Purely compositional -- changing this never requires recomputing raw samples or rebuilding the
	 * tree; see GenerateAmbientOcclusionMask's own cache contract.
	 */
	float LevelsMin = 0.0f;

	/**
	 * AUDITED (AO Levels): values in the vanilla BaseAO (see ApplyAOLevelsAndInvert) at or above this
	 * threshold become 1 (white). UI range [0, 1]; default 1.0 (no compression at the high end). With
	 * both LevelsMin=0 and LevelsMax=1 (the defaults), Levels is a no-op and BaseAO passes through
	 * unchanged. Purely compositional -- same cache-safety contract as LevelsMin.
	 */
	float LevelsMax = 1.0f;
};

/** Which local/world axis one FVertexMaskForgeAxisMaskParams evaluates. */
enum class EVertexMaskForgeBoundsAxis : uint8
{
	X,
	Y,
	Z,
};

/**
 * Independent parameters for ONE axis of the Bounding Box Mask (the "Local X" / "Local Y" /
 * "Local Z" rows in the UI). Panel-level (SVertexMaskForgePanel::BoundingBoxAxisParams, indexed by
 * EVertexMaskForgeBoundsAxis) -- shared across every selected entry, exactly like the single-axis
 * Position/TransitionWidth/bInvert panel members this replaces.
 */
struct FVertexMaskForgeAxisMaskParams
{
	bool bEnabled = false;
	float Position = 0.5f;
	float TransitionWidth = 1.0f;
	bool bInvert = false;

	/** Adds a second, symmetric gradient in the opposite direction, combined with the base gradient
	 *  by maximum (see VertexMaskForgePanel::GenerateBoundingBoxMask). Never alters Position or
	 *  TransitionWidth. */
	bool bMirror = false;

	/** False (default): evaluate this axis in the Static Mesh's own local space. True: evaluate in
	 *  World Space, using the specific previewed instance's own ComponentTransform. Independent per
	 *  axis and independent of Position/TransitionWidth/bInvert/bMirror. */
	bool bWorldSpace = false;
};

/**
 * Per-axis bounds, either INDIVIDUAL (one Static Mesh's own LOD0, in its own chosen space) or
 * COLLECTIVE (Unified Bounds -- the union across every participating component's render vertices,
 * in that axis's chosen space), depending on which pass produced it. Domain-shape only; carries no
 * opinion about which mode produced it -- see VertexMaskForgePanel::GenerateBoundingBoxMask's
 * CollectiveBounds parameter and VertexMaskForgePanel::ComputeCollectiveAxisBounds.
 */
struct FVertexMaskForgeAxisBoundsResult
{
	double MinCoord = 0.0;
	double MaxCoord = 0.0;
	bool bDegenerate = false;

	/**
	 * Only meaningful for an enabled Local-space axis (bWorldSpace == false) when these bounds are
	 * COLLECTIVE (Unified Bounds on). World-space direction that every participating component's own
	 * local axis maps to -- validated compatible across all participants (see
	 * VertexMaskForgePanel::ResolveSharedLocalAxis) -- used to project each component's own
	 * world-space render-vertex positions onto one shared coordinate line without discarding
	 * translation between instances. Normalized. Zero vector / unused for World-space axes and for
	 * Individual (non-Unified) bounds.
	 */
	FVector SharedLocalAxisDirection = FVector::ZeroVector;

	/** World-space length of the (pre-normalization) transformed local axis vector for the same
	 *  axis -- i.e. the shared scale along that axis. See SharedLocalAxisDirection. */
	double SharedLocalAxisScale = 1.0;
};

/**
 * A transient per-vertex scalar mask (e.g. the Bounding Box Mask), owned by exactly one
 * FVertexMaskForgeWorkingMesh. Exists only in memory; never written to the Primary Color Overlay,
 * MeshDescription, RenderData, or the source asset.
 *
 * AUDITED: the Bounding Box Mask and the Ambient Occlusion Mask (the two spatial generators that
 * exist so far) are both computed directly in RENDER VERTEX order (see
 * VertexMaskForgePanel::GenerateBoundingBoxMask / GenerateAmbientOcclusionMask), so for them,
 * Values/bHasValue are indexed by Render Vertex Index and are always dense (Values.Num() ==
 * bHasValue.Num() == RenderVertexCount == the LOD's PositionVertexBuffer.GetNumVertices(), every
 * slot written). This struct itself stays domain-agnostic (a future, genuinely topology-dependent
 * generator could still index by Dynamic Mesh Vertex ID and leave bHasValue sparse) -- always use
 * TryGetValue() rather than indexing Values directly, since an unwritten slot is meaningless, not
 * zero, whichever domain produced it.
 */
struct FVertexMaskForgeScalarMask
{
	/** Small explicit tolerance for the Near Zero / Near One counters, matching color-diagnostic precedent. */
	static constexpr float Tolerance = 1.0f / 255.0f;

	EVertexMaskForgeScalarMaskState State = EVertexMaskForgeScalarMaskState::NotGenerated;

	/** Indexed by Render Vertex Index for the Bounding Box Mask (see the struct-level audit note). */
	TArray<float> Values;

	/** Parallel to Values: true only at indices that were actually written. */
	TBitArray<> bHasValue;

	/** Safe accessor: returns false (and leaves OutValue untouched) for any ID not actually stored. */
	bool TryGetValue(int32 VertexID, float& OutValue) const
	{
		if (!bHasValue.IsValidIndex(VertexID) || !bHasValue[VertexID])
		{
			return false;
		}
		OutValue = Values[VertexID];
		return true;
	}

	int32 NumValidValues = 0;
	float MinValue = 0.f;
	float MaxValue = 0.f;
	float MeanValue = 0.f;

	/** Values within Tolerance of 0.0 / 1.0, respectively. */
	int32 NumNearZero = 0;
	int32 NumNearOne = 0;

	/** Which generator produced this mask -- so the UI never mislabels a Fill result as Bounding Box. */
	EVertexMaskForgeScalarMaskSource Source = EVertexMaskForgeScalarMaskSource::BoundingBox;

	/**
	 * Snapshot of the per-axis parameters used to generate this mask (Source == BoundingBox only;
	 * left default-constructed, all bEnabled == false, for Constant sources), indexed by
	 * EVertexMaskForgeBoundsAxis -- purely for diagnostic display (which axes/Mirror/World Space
	 * were active). Never affects composition; composition only ever reads Values/bHasValue.
	 */
	TStaticArray<FVertexMaskForgeAxisMaskParams, 3> UsedAxisParams;

	/** Snapshot of the AO parameters used to generate this mask (Source == AmbientOcclusion only;
	 *  left default-constructed otherwise) -- same diagnostic-only relationship as UsedAxisParams. */
	FVertexMaskForgeAOParams UsedAOParams;

	// --- Temporary diagnostics (audited render-vertex-order fix) ---------------------------
	// Added to make the Values.Num() == PositionVertexBuffer.GetNumVertices() invariant directly
	// verifiable; RenderVertexCount/bUnifiedBounds/SelectionMeshCount below are diagnostic-only
	// fields (no longer surfaced in the UI since the Selected Static Meshes panel was removed, but
	// still computed and available for logging/future diagnostics).

	/** LOD0 PositionVertexBuffer.GetNumVertices() at generation time; must equal Values.Num(). */
	int32 RenderVertexCount = 0;

	/** True if this mask (Source == BoundingBox only) was generated against a collective, Unified
	 *  Bounds domain rather than this one mesh's own individual bounds. Diagnostic display only. */
	bool bUnifiedBounds = false;

	/** Total number of meshes in the selection at generation time (diagnostic display only; not a
	 *  per-mesh count -- Individual and Unified masks generated from the same selection both report
	 *  the same selection size, matching the checkpoint's diagnostic examples). */
	int32 SelectionMeshCount = 0;
};

/**
 * A transient, independent working copy of a Static Mesh's LOD 0, used as the basis for future
 * mask generators. Never written back to the source asset. Owns its FDynamicMesh3 by pointer so
 * the (relatively heavy) mesh data is never copied when this struct is moved.
 */
struct FVertexMaskForgeWorkingMesh
{
	/** Small explicit tolerance used for the non-white / non-black element counts, in 0-1 color space. */
	static constexpr float ColorChannelTolerance = 1.0f / 255.0f;

	FVertexMaskForgeWorkingMesh() = default;

	/**
	 * Declared here, defined in the .cpp (after DynamicMesh3.h is included) so that
	 * TUniquePtr<FDynamicMesh3>'s destructor is only instantiated where FDynamicMesh3 is a
	 * complete type. This struct owns that lifetime itself; callers must not rely on some other
	 * class's destructor to make this safe.
	 */
	~FVertexMaskForgeWorkingMesh();

	/** Exclusive ownership of the FDynamicMesh3 via TUniquePtr; copying would be ambiguous/unsafe. */
	FVertexMaskForgeWorkingMesh(const FVertexMaskForgeWorkingMesh&) = delete;
	FVertexMaskForgeWorkingMesh& operator=(const FVertexMaskForgeWorkingMesh&) = delete;

	/** Declared here, defined in the .cpp for the same reason as the destructor. */
	FVertexMaskForgeWorkingMesh(FVertexMaskForgeWorkingMesh&&);
	FVertexMaskForgeWorkingMesh& operator=(FVertexMaskForgeWorkingMesh&&);

	EVertexMaskForgeWorkingMeshState State = EVertexMaskForgeWorkingMeshState::InvalidSource;

	/** Null unless State == Ready. Explicit, exclusive ownership; never shared or persisted elsewhere. */
	TUniquePtr<UE::Geometry::FDynamicMesh3> Mesh;

	/**
	 * AUDITED (Nanite source-topology support, AO cache robustness fix): a content fingerprint of Mesh
	 * (positions + normal overlay elements, combined via GetTypeHash/HashCombine), computed ONCE when
	 * Mesh is built (see VertexMaskForgePanel::ComputeDynamicMeshGeometryFingerprint /
	 * BuildWorkingMeshForStaticMesh) and never recomputed afterward -- Mesh itself is never mutated
	 * in place after this point (the one defensive exception, filling in a missing Normal Overlay, also
	 * happens BEFORE this fingerprint is taken). Used as part of FVertexMaskForgeSourceTopologyAOCache's
	 * identity check: vertex/triangle COUNTS alone are not sufficient (two genuinely different
	 * geometries can share the same counts), so the cache compares this fingerprint instead of/in
	 * addition to counts. Zero only if Mesh itself is null/not Ready.
	 */
	uint32 GeometryFingerprint = 0;

	/**
	 * AUDITED (Nanite source-topology support): Dynamic Mesh TriangleID -> source FMeshDescription
	 * FTriangleID, exactly as produced by FMeshDescriptionToDynamicMesh::Convert (see ConvertToDynamicMesh
	 * in the .cpp). Persisted here (computed once, at working-mesh build time) so the Source-Topology
	 * Accept commit path (WriteSourceTopologyAcceptTargets) can re-derive the same TriangleID -> source
	 * FVertexInstanceID correspondence ReconstructOmittedColorOverlay already proved correct, without
	 * re-running the conversion. Valid against Mesh->GetMeshDescription(0) directly: MeshDescriptionCopy
	 * (the conversion input) is an EXACT copy of the source, so FTriangleID/FVertexInstanceID values are
	 * identical in both. Empty unless State == Ready.
	 */
	TArray<FTriangleID> TriIDMap;

	int32 DynamicVertexCount = 0;
	int32 DynamicTriangleCount = 0;

	/** MeshDescription triangle count, for comparison against DynamicTriangleCount. */
	int32 SourceTriangleCount = 0;

	/** Max(0, SourceTriangleCount - DynamicTriangleCount): triangles dropped or welded away by conversion. */
	int32 DiscardedTriangleCount = 0;

	/** Result of FDynamicMesh3::CheckValidity() with permissive options, in ReturnOnly (non-asserting) mode. */
	bool bTopologyValid = false;

	EVertexMaskForgeMaterialIDState MaterialIDState = EVertexMaskForgeMaterialIDState::Unavailable;
	int32 DistinctMaterialIDCount = 0;

	/**
	 * True if every triangle's Material ID falls within [0, NumMaterialSlots) of the source mesh.
	 * Note: bUseCompactedPolygonGroupIDValues only compacts Polygon Group IDs into contiguous
	 * Section Indices -- it does not by itself prove a Section Index equals the correct Material
	 * Slot index on assets with manually remapped sections. This flag only catches out-of-range
	 * IDs, not silent slot mismatches within range; IDs are never remapped or corrected here.
	 */
	bool bMaterialIDsInRange = true;

	EVertexMaskForgeWorkingVertexColorState VertexColorState = EVertexMaskForgeWorkingVertexColorState::Unavailable;
	FVertexMaskForgeColorStats ColorStats;

	/**
	 * The Bounding Box slot's mask (across up to 3 axes), OR a Constant Fill result -- the two are
	 * mutually exclusive WITHIN this one field/slot (Source discriminates which). This is the
	 * ENTRY-LEVEL reference: generated using the first live PreviewComponent's transform (or
	 * FTransform::Identity if none), used for gating (Ready check), the row summary text, and
	 * Content-Browser-only entries. When Source == BoundingBox and at least one axis uses World
	 * Space, actual Preview/Accept composition RE-EVALUATES the mask per component with that
	 * component's own transform (see ApplyPreviewToEntry / BuildAcceptTargets) rather than reusing
	 * this shared reference -- so two instances of this asset can legitimately show different
	 * results, exactly like a divergent per-instance OverrideVertexColors baseline already does. A
	 * fresh FVertexMaskForgeWorkingMesh is always constructed on Refresh Selection (see
	 * BuildWorkingMeshForStaticMesh), so this starts at NotGenerated automatically every time the
	 * working mesh itself is rebuilt -- there is no separate invalidation step needed for that case.
	 * Parameter-change invalidation is handled by the panel explicitly resetting this field.
	 *
	 * AUDITED (composition-stack checkpoint): NO LONGER mutually exclusive with AmbientOcclusionMask
	 * below -- Bounding Box and Ambient Occlusion are independent, optional STACK LAYERS that can both
	 * be Ready and both contribute to composition at once (see VertexMaskForgePanel::ApplyPreviewToEntry
	 * building an ordered Layers list from whichever of the two slots below are Ready, and
	 * ComposeMaskStack/UpdateWorkingColors applying each enabled layer sequentially). Only a Constant
	 * Fill result (Source == ConstantWhite/ConstantBlack, stored in THIS field) remains a hard override
	 * that supersedes both slots entirely for that one pass -- see ApplyPreviewToEntry's own doc note.
	 */
	FVertexMaskForgeScalarMask BoundingBoxMask;

	/**
	 * The Ambient Occlusion slot's ENTRY-LEVEL mask -- populated ONLY as a cheap, geometry-cache-free
	 * VALIDATION result (NumRenderVerts/triangles/normal-buffer sanity, and UsedAOParams snapshot with
	 * Samples resolved to the full, user-chosen value -- see RunAutoUpdatePreview, the tool's single
	 * live-regeneration entry point). Values/bHasValue are ALWAYS left empty here -- this field is NEVER used to
	 * read actual per-vertex AO values; ApplyPreviewToEntry unconditionally re-evaluates the REAL,
	 * per-component result (using that component's own AOCache) from UsedAOParams, exactly once per
	 * component, every time. This is what guarantees Ambient Occlusion is ever computed in exactly ONE
	 * place (ApplyPreviewToEntry) -- see that function's own doc comment for the audited fix this
	 * replaced (a redundant entry-level real computation that could double the raycast cost per
	 * update). Independent of BoundingBoxMask -- see that field's own doc comment on the composition
	 * stack. Reset together with BoundingBoxMask by the same invalidation points (parameter changes,
	 * RefreshSelection).
	 */
	FVertexMaskForgeScalarMask AmbientOcclusionMask;

	/**
	 * The Curvature slot's ENTRY-LEVEL mask -- and, UNLIKE AmbientOcclusionMask above, this DOES hold
	 * the REAL, final Values/bHasValue directly usable by every component: Curvature is a pure function
	 * of the asset's own local-space topology (see EVertexMaskForgeScalarMaskSource::Curvature's own
	 * doc comment) with no per-component/per-transform variation, so there is no per-component
	 * re-evaluation step for it in ApplyPreviewToEntry the way Bounding Box's World Space axes and
	 * Ambient Occlusion require -- this entry-level result IS the per-component contribution, for every
	 * tracked component of this entry. Populated by VertexMaskForgePanel::GenerateCurvatureMask
	 * (render-vertex domain) or GenerateCurvatureMaskFromDynamicMesh (Source-Topology/Nanite domain),
	 * exactly mirroring BoundingBoxMask's own domain split. Reset together with BoundingBoxMask/
	 * AmbientOcclusionMask (parameter changes, RefreshSelection) -- see CurvatureRawConvexCache below for
	 * what is DELIBERATELY NOT reset by a Curvature Type/Multiplier/Blur/Levels/Invert change.
	 */
	FVertexMaskForgeScalarMask CurvatureMask;

	/**
	 * AUDITED (Curvature CLASSIFICATION FIX): the EXPENSIVE, geometry-only half of Curvature generation,
	 * cached separately from CurvatureMask.Values so that Curvature Type/Multiplier/Blur/Levels/Invert/
	 * Opacity/Blend Mode changes -- all cheap, purely-downstream reprocessing -- never repeat the
	 * adjacency/dihedral-angle analysis. Indexed by DYNAMIC MESH VERTEX ID (WorkingMesh.Mesh's own
	 * domain -- the "source mesh vertex" a UV seam/hard edge's several render vertices/corners all
	 * share).
	 *
	 * TWO SEPARATE non-negative magnitude arrays (Convex and Concave), NOT one signed scalar -- see
	 * VertexMaskForgePanel::ComputeRawCurvatureMagnitudes' own doc comment for why a single signed sum
	 * per vertex allowed positive and negative per-edge contributions to cancel BEFORE Curvature Type
	 * ever saw them (the actual root cause of the Convex/Concave misclassification bug this fix
	 * addresses), and for the exact algorithm/normalization. Both arrays share the SAME normalization
	 * scale (computed once, together) and are both in [0, 1]. Empty until Curvature is generated at
	 * least once for this entry.
	 *
	 * Valid ONLY when CurvatureCacheFingerprint == GeometryFingerprint (see that field's own doc
	 * comment) -- compared, never assumed, every time Curvature is (re)generated; a mismatch (including
	 * the initial 0/0 state on a freshly built WorkingMesh, which never matches a real, non-zero
	 * GeometryFingerprint) forces a full recompute. A brand new FVertexMaskForgeWorkingMesh (built fresh
	 * on every RefreshSelection) always starts with this empty and CurvatureCacheFingerprint at 0, so a
	 * genuine geometry change is caught automatically, with no separate invalidation step needed.
	 */
	TArray<float> CurvatureRawConvexCache;
	TArray<float> CurvatureRawConcaveCache;

	/**
	 * AUDITED (Curvature layer): render-vertex-domain correspondence cache, valid ONLY for a non-Source-
	 * Topology (non-Nanite) entry -- maps each LOD0 render vertex index to the DYNAMIC MESH VERTEX ID
	 * (CurvatureRawConvexCache/CurvatureRawConcaveCache's own domain) it was converted from, derived once
	 * from WorkingMesh.TriIDMap + the source MeshDescription's own VertexInstance->Vertex/WedgeMap
	 * correspondence (see VertexMaskForgePanel::ComputeCurvatureRenderVertexCorrespondence). This is what
	 * makes a UV seam's several split render vertices all read the SAME cached curvature value, rather
	 * than each being (mis)treated as topologically isolated. Rebuilt together with the raw caches (same
	 * fingerprint check); empty and unused for a Source-Topology entry, which looks up the raw caches
	 * directly by Dynamic Mesh Vertex ID per triangle corner instead (see
	 * UpdateWorkingColorsSourceTopology).
	 */
	TArray<int32> CurvatureRenderVertexToDynamicMeshVertex;

	/** GeometryFingerprint CurvatureRawConvexCache/CurvatureRawConcaveCache/
	 *  CurvatureRenderVertexToDynamicMeshVertex were last built from -- see CurvatureRawConvexCache's own
	 *  doc comment for the exact reuse/recompute rule. */
	uint32 CurvatureCacheFingerprint = 0;

	/**
	 * AUDITED (Noise V1): the Noise slot's ENTRY-LEVEL mask -- same "holds REAL, final values directly
	 * usable by every component" contract as CurvatureMask (see that field's own doc comment): Noise is
	 * a pure function of local-space position, never a component transform, so there is no per-component
	 * re-evaluation step for it in ApplyPreviewToEntry either. Populated by
	 * VertexMaskForgePanel::GenerateNoiseMask (render-vertex domain) or GenerateNoiseMaskFromDynamicMesh
	 * (Source-Topology/Nanite domain).
	 */
	FVertexMaskForgeScalarMask NoiseMask;

	/**
	 * AUDITED (Noise V1): the RAW procedural pattern -- FMath::PerlinNoise3D/FBM already reduced to
	 * [0, 1] (signed*0.5+0.5, saturated) but with NO Multiplier/Levels/Invert applied yet -- cached
	 * separately from NoiseMask.Values so that those three purely-artistic controls never repeat the
	 * per-vertex Perlin evaluation. UNLIKE Curvature's raw cache, this is NOT purely geometric: it also
	 * depends on the GENERATIVE parameters (see FVertexMaskForgeNoiseGenerativeParams) that determine
	 * WHAT pattern is sampled -- so reuse requires BOTH GeometryFingerprint AND NoiseCacheUsedParams to
	 * still match (see NoiseCacheFingerprint's own doc comment). Domain matches whichever generator
	 * (GenerateNoiseMask/GenerateNoiseMaskFromDynamicMesh) last populated it for this entry -- render
	 * vertex index for a non-Source-Topology entry, Dynamic Mesh Vertex ID for a Source-Topology one --
	 * exactly like NoiseMask.Values' own domain, since bUseSourceTopology never changes at runtime for a
	 * given entry.
	 */
	TArray<float> NoiseRawCache;

	/** GeometryFingerprint NoiseRawCache was last built from -- compared, never assumed, alongside
	 *  NoiseCacheUsedParams (see NoiseRawCache's own doc comment) every time Noise is (re)generated. */
	uint32 NoiseCacheFingerprint = 0;

	/** Generative parameters NoiseRawCache was last built from -- see NoiseRawCache's own doc comment.
	 *  Compared by value (FVertexMaskForgeNoiseGenerativeParams::operator==) every regeneration. */
	FVertexMaskForgeNoiseGenerativeParams NoiseCacheUsedParams;

	// --- Material Slot Mask (V2-D) -----------------------------------------------------------

	/**
	 * The Material Slot Mask slot's ENTRY-LEVEL mask -- same "holds REAL, final values directly usable
	 * by every component" contract as CurvatureMask/NoiseMask, EXCEPT this one is corner-EXACT in the
	 * Source-Topology domain (Values sized Mesh.TriangleCount()*3, indexed by CornerIndex directly --
	 * see GenerateMaterialSlotMaskFromDynamicMesh) rather than Dynamic Mesh Vertex ID, because two
	 * corners sharing a position/VertexID on opposite sides of a material boundary must be able to read
	 * different values. Populated by VertexMaskForgePanel::GenerateMaterialSlotMask (render-vertex
	 * domain) or GenerateMaterialSlotMaskFromDynamicMesh (Source-Topology domain).
	 */
	FVertexMaskForgeScalarMask MaterialSlotMask;

	/** This entry's mesh's real Material Slots (from UStaticMesh::GetStaticMaterials()), rebuilt every
	 *  BuildWorkingMeshForStaticMesh call -- the source of truth the panel's dropdown is built from. */
	TArray<FVertexMaskForgeMaterialSlotInfo> MaterialSlotOptions;

	/**
	 * AUDITED (V2-D, M0-A): Dynamic Mesh TriangleID -> REAL Static Material Slot index, resolved via
	 * TriIDMap -> source FTriangleID -> source FPolygonID -> PolygonGroupID -> PolygonGroupMaterialSlotName
	 * -> name-matched index in GetStaticMaterials() -- NEVER the compacted MaterialID attribute value
	 * directly (see EnsureMaterialIDAttribute's own doc note on bUseCompactedPolygonGroupIDValues not
	 * proving Section Index == Material Slot index). INDEX_NONE for any triangle whose Polygon Group
	 * could not be resolved unambiguously to exactly one Static Material Slot (see
	 * bMaterialSlotResolutionValid below). Sized Mesh->MaxTriangleID(); built once in
	 * BuildWorkingMeshForStaticMesh, alongside TriIDMap.
	 */
	TArray<int32> DynamicTriangleToMaterialSlot;

	/** False if ANY Polygon Group actually used by this mesh's triangles could not be resolved to
	 *  exactly one Static Material Slot by name (ambiguous duplicate name, NAME_None with more than one
	 *  candidate, or no match at all) -- see DynamicTriangleToMaterialSlot's own doc comment. When
	 *  false, Material Slot Mask must refuse to generate (State stays Unavailable) rather than risk a
	 *  silently wrong slot assignment; every other generator is completely unaffected. */
	bool bMaterialSlotResolutionValid = true;

	/**
	 * AUDITED (V2-D, M0-B): LOD0 Render Vertex Index -> REAL Static Material Slot index, derived from
	 * FStaticMeshLODResources::Sections (Section.MaterialIndex + its own IndexBuffer triangle range) --
	 * the RenderData authority for the non-Nanite/render-vertex domain (MeshDescription's Polygon Groups
	 * are not indexed by render vertex at all). INDEX_NONE for a render vertex this mesh's Sections
	 * never reference, OR for one explicitly detected as referenced by MORE THAN ONE distinct
	 * MaterialIndex (see bRenderVertexMaterialSlotAmbiguous) -- never guessed via first/last/min/max.
	 * Sized to LOD0's own render vertex count; built once in BuildWorkingMeshForStaticMesh.
	 */
	TArray<int32> RenderVertexToMaterialSlot;

	/** True if BuildMaterialSlotLookups found at least one render vertex referenced by triangles from
	 *  Sections with two (or more) DIFFERENT MaterialIndex values -- see RenderVertexToMaterialSlot's
	 *  own doc comment. When true, the non-Nanite Material Slot Mask must refuse to generate for THIS
	 *  entry (State stays Unavailable, with a specific diagnostic) rather than risk bleeding between
	 *  slots; every other generator, and the Source-Topology domain, are completely unaffected. */
	bool bRenderVertexMaterialSlotAmbiguous = false;

	// --- Directional Normal Mask (V2-E) --------------------------------------------------------

	/**
	 * The Directional Normal Mask slot's ENTRY-LEVEL reference -- dual contract depending on the
	 * panel's current Space setting (mirrors CurvatureMask/AmbientOcclusionMask's own split, chosen
	 * dynamically):
	 *  - LOCAL Space: holds the REAL, final per-element values, exactly like CurvatureMask/NoiseMask/
	 *    MaterialSlotMask (transform-independent -- every component of this entry shares it directly).
	 *  - WORLD Space: VALIDATION ONLY (Values/bHasValue left empty, same "State decides Ready, never
	 *    read for real values" contract as AmbientOcclusionMask) -- ApplyPreviewToEntry re-evaluates the
	 *    REAL result per component, using each component's own transform, exactly like Bounding Box's
	 *    own World Space axes and Ambient Occlusion.
	 * Render-vertex domain sized like BoundingBoxMask/CurvatureMask; Source-Topology domain is CORNER-
	 * EXACT (Mesh.TriangleCount()*3, like MaterialSlotMask -- NEVER collapsed to Dynamic Mesh Vertex ID,
	 * since two corners at the same position can legitimately have different split normals).
	 */
	FVertexMaskForgeScalarMask DirectionalNormalMask;

	/**
	 * True if BuildWorkingMeshForStaticMesh (or a later per-component check) found this entry's live
	 * PreviewComponents producing DIFFERENT effective World-Space normal-transform results for the SAME
	 * underlying asset (see VertexMaskForgePanel::HasConflictingWorldSpaceNormalTransforms) -- only
	 * meaningful when Directional Normal Mask is enabled AND Space == World; checked live at generation
	 * and Accept time (never cached stale), never affects Local Space or any other generator.
	 */
	bool bDirectionalNormalWorldSpaceConflict = false;

	/**
	 * Thickness Mask (V2-G) -- Asset Local Space, transform-independent, so (unlike
	 * DirectionalNormalMask's World Space branch) always holds REAL final values directly usable by
	 * every component of this entry, same "computed ONCE PER ENTRY" contract as CurvatureMask/NoiseMask.
	 * Render-vertex domain for a non-Source-Topology entry, CORNER-EXACT (Mesh.TriangleCount()*3) for a
	 * Source-Topology entry -- never collapsed to Dynamic Mesh Vertex ID, same reason as
	 * DirectionalNormalMask/MaterialSlotMask.
	 */
	FVertexMaskForgeScalarMask ThicknessMask;

	/** Non-Nanite Thickness cache (local spatial tree + raw hit distances + freshness snapshot). Lives
	 *  here (per-entry), never in FVertexMaskForgePreviewComponentState, because Thickness has zero
	 *  Component Transform dependency -- see FVertexMaskForgeThicknessCache's own doc comment. */
	TUniquePtr<FVertexMaskForgeThicknessCache> ThicknessCache;

	/** Source-Topology sibling of ThicknessCache -- see FVertexMaskForgeSourceTopologyThicknessCache. */
	TUniquePtr<FVertexMaskForgeSourceTopologyThicknessCache> SourceTopologyThicknessCache;
};

// EVertexMaskForgePreviewMode is now defined in VertexMaskForgeMaskTypes.h (M3 extraction, moved so
// VertexMaskForgeDisplayColorDerivation.h/.cpp can use it without including this header) -- see that
// header for the enum's own doc comment.

/**
 * Centralized per-Actor "temporarily hidden in editor" ownership. Hiding is necessarily Actor-level
 * (UE 5.8 has no transient-safe component-level visibility flag -- see the audit note on
 * ActivatePreviewForComponent), so when two or more previewed components belong to the SAME Actor
 * (either two entries in one FVertexMaskForgeSelectedMesh::PreviewComponents, or components from
 * two different FVertexMaskForgeSelectedMesh entries), naively snapshotting/restoring
 * IsTemporarilyHiddenInEditor() per-component would let one component's cleanup un-hide an Actor
 * that another still-active preview depends on staying hidden. This struct instead records the
 * Actor's original flag value once (on first acquire) and a reference count of how many active
 * previews currently depend on the Actor being hidden; only the release that brings the count to
 * zero actually restores the original value. Keyed externally by TWeakObjectPtr<AActor> in
 * SVertexMaskForgePanel::ActorHideStates.
 */
struct FVertexMaskForgeActorHideState
{
	bool bOriginalHiddenInEditor = false;
	int32 RefCount = 0;
};

/**
 * Non-destructive preview state for one selected UStaticMeshComponent.
 *
 * IMPORTANT (audited): UStaticMeshComponent::Serialize() unconditionally re-serializes LODData
 * (which holds OverrideVertexColors and PaintedVertices) via a custom `Ar << LODData`, regardless
 * of LODData's own `UPROPERTY(transient)` tag, and OverrideMaterials is an ordinary (non-transient)
 * UPROPERTY. This means a Save/Save All that occurs while a preview is active WOULD persist any
 * of those properties if they were written directly onto the real, selected component -- even
 * though the plugin never calls Modify()/MarkPackageDirty() itself, a package that is already
 * dirty (or being autosaved) can still be written to disk mid-preview. For that reason this struct
 * never mutates the selected component's own OverrideMaterials/OverrideVertexColors/PaintedVertices
 * at all: it tracks a separate, transient, RF_Transient duplicate component instead (see
 * EnsurePreviewComponent()), so there is nothing on the real component for a Save to persist.
 */
struct FVertexMaskForgePreviewComponentState
{
	FVertexMaskForgePreviewComponentState() = default;

	/**
	 * Declared here, defined in the .cpp (after FVertexMaskForgeAOCache is a complete type) --
	 * exactly the same reason and pattern as FVertexMaskForgeWorkingMesh's own destructor/move
	 * operations, now needed here too because of the AOCache member (TUniquePtr<FVertexMaskForgeAOCache>).
	 */
	~FVertexMaskForgePreviewComponentState();

	/** Exclusive ownership of AOCache via TUniquePtr; copying would be ambiguous/unsafe. Every actual
	 *  usage site in the .cpp already only ever moves this struct (TArray::Add(MoveTemp(...)) at
	 *  construction, by-reference iteration everywhere else), so deleting copy costs nothing real. */
	FVertexMaskForgePreviewComponentState(const FVertexMaskForgePreviewComponentState&) = delete;
	FVertexMaskForgePreviewComponentState& operator=(const FVertexMaskForgePreviewComponentState&) = delete;

	/** Declared here, defined in the .cpp for the same reason as the destructor. */
	FVertexMaskForgePreviewComponentState(FVertexMaskForgePreviewComponentState&&);
	FVertexMaskForgePreviewComponentState& operator=(FVertexMaskForgePreviewComponentState&&);

	/** The real, selected component. Read-only source for mesh/transform; never mutated. */
	TWeakObjectPtr<UStaticMeshComponent> SourceComponent;

	/**
	 * Transient duplicate created only while preview is active, destroyed on cleanup. Outer is
	 * GetTransientPackage() and it is never added to SourceComponent's owning Actor's component
	 * list, so no Actor/level serialization path can ever discover or write it, independent of
	 * RF_Transient. Overridden colors/materials live ONLY on this object.
	 *
	 * IMPORTANT (audited): RF_Transient only excludes an object from serialization; it does NOT by
	 * itself keep the object reachable for Garbage Collection. TStrongObjectPtr is UE's supported
	 * scoped-root mechanism for exactly this "keep a dynamically created UObject alive outside the
	 * normal owning-UPROPERTY graph" case (see UObject/StrongObjectPtr.h). It is released via
	 * .Reset() in RestoreComponentOriginal, so unlike AddToRoot() it never leaks a permanent root
	 * reference -- a Collect Garbage while preview is active cannot collect this object.
	 */
	TStrongObjectPtr<UStaticMeshComponent> PreviewComponent;

	/** True while PreviewComponent is active. */
	bool bOverrideActive = false;

	/**
	 * True while this State currently holds one reference-count token in
	 * SVertexMaskForgePanel::ActorHideStates for HiddenOwner. Tracked independently of
	 * SourceComponent's validity so a release can still happen correctly even if SourceComponent (or
	 * its owning Actor) is destroyed while preview is active -- see RestoreComponentOriginal.
	 */
	bool bHasAcquiredActorHide = false;

	/** The Actor this State acquired a hide token for, captured at acquire time. */
	TWeakObjectPtr<AActor> HiddenOwner;

	/**
	 * Immutable-after-capture snapshot of this component's effective input Vertex Colors at the
	 * START of the current operation/session, in render-vertex order (LOD0-sized). Empty until the
	 * first composition of a session.
	 *
	 * AUDITED (baseline-snapshot fix): captured EXACTLY ONCE per session by
	 * VertexMaskForgePanel::UpdateWorkingColors, the first time it is called for this component this
	 * session (detected by BaselineColors being empty or stale-sized) -- from SourceComponent's own
	 * pre-existing OverrideVertexColors, or the Source Static Mesh's own LOD0 colors, or white (see
	 * UpdateWorkingColors' own doc comment for the exact priority). After that single capture, this
	 * array is NEVER written to again for the rest of the session -- UpdateWorkingColors' every later
	 * call reads it, never re-reads SourceComponent's OverrideVertexColors or the asset's RenderData.
	 * This is a genuine snapshot, not an assumption that the live sources happen not to change during
	 * the session.
	 *
	 * Reset (emptied) together with CommittedColors/WorkingColors only when the session concludes --
	 * see RestoreComponentOriginal, called by DestroyAllPreviews() from Cancel, Accept (success),
	 * RefreshSelection (before rebuilding), and World cleanup -- so a brand new session always starts
	 * from a fresh capture, never a stale one from a
	 * concluded operation (whose baseline may since have changed, e.g. Accept just wrote new colors
	 * onto this exact component/asset).
	 */
	TArray<FColor> BaselineColors;

	/**
	 * The last result EXPLICITLY CONSOLIDATED for this component, in render-vertex order
	 * (LOD0-sized). Seeded from BaselineColors at the same moment BaselineColors itself is captured
	 * (see that field's doc comment), then only ever overwritten by
	 * VertexMaskForgePanel::UpdateWorkingColors when called with bCommit == true --
	 * exclusively a Fill White/Black action (see UpdateAllPreviews' own doc
	 * comment for the exhaustive list of which triggers commit and which do not).
	 *
	 * AUDITED (Channel Filter toggle fix): this is what WorkingColors is rebuilt FROM on every single
	 * recomposition (see WorkingColors' own doc comment) -- so a channel that is toggled OFF in the
	 * Channel Filter before ever being consolidated correctly reverts to whatever this array already
	 * holds for it (BaselineColors, if never consolidated; or an earlier Fill's result,
	 * if it was), rather than freezing whatever transient value a previous, uncommitted recomposition
	 * happened to leave behind.
	 *
	 * Reset (emptied) together with BaselineColors/WorkingColors -- see BaselineColors' own doc
	 * comment for every reset point.
	 */
	TArray<FColor> CommittedColors;

	/**
	 * Multi-channel TRANSIENT preview result for the CURRENT operation/session on this component, in
	 * render-vertex order (LOD0-sized), holding the RAW composited RGBA (before any Preview Mode
	 * display reduction). Empty until the first composition of a session. This is what the transient
	 * PreviewComponent's OverrideVertexColors is set from (ApplyPreviewToEntry), and what Accept reads
	 * to build its FinalColors -- "the preview currently shown".
	 *
	 * AUDITED (Channel Filter toggle fix): REBUILT FROM CommittedColors on EVERY single call to
	 * VertexMaskForgePanel::UpdateWorkingColors (WorkingColors = CommittedColors, verbatim, at the
	 * start of every call) -- never carried forward from this array's own previous value. Each
	 * channel currently enabled in the Channel Filter is then overwritten with a fresh
	 * BlendMaskValue(BaselineColors.Channel, mask, BlendMode, Opacity, ...) -- ALWAYS computed from
	 * BaselineColors, never from CommittedColors or this array's own prior value, which is what
	 * prevents the same channel from ever accumulating across repeated recompositions. A channel NOT
	 * currently enabled is simply whatever CommittedColors already holds for it -- so disabling a
	 * channel immediately, visibly reverts it to its last CONSOLIDATED state (BaselineColors if never
	 * consolidated), fixing the prior bug where a disabled channel froze at a transient, uncommitted
	 * value instead. Alpha is refreshed from BaselineColors' own Alpha unconditionally on every call
	 * (Blend Mode/Opacity never write Alpha).
	 *
	 * Reset (emptied) together with BaselineColors/CommittedColors -- see BaselineColors' own doc
	 * comment for every reset point.
	 *
	 * Preview (ApplyPreviewToEntry) and Accept (BuildAcceptTargets) both read this SAME array --
	 * Accept itself NEVER calls UpdateWorkingColors; it only READs it as-is, so Accept always persists
	 * exactly the last effectively-generated/shown preview, never a silently-recomposed approximation
	 * of pending, ungenerated UI parameters.
	 */
	TArray<FColor> WorkingColors;

	/**
	 * Per-component Ambient Occlusion memoization: the world-space-baked occluder tree (rebuilt only
	 * when this component's geometry/transform actually changes) and the last-computed raw
	 * per-render-vertex AO values (rebuilt only when Samples/MaxDistance/Bias actually change) -- see
	 * VertexMaskForgePanel::GenerateAmbientOcclusionMask for the full cache contract. Null until AO is
	 * generated at least once for this component. Lazily created inside GenerateAmbientOcclusionMask;
	 * never read or written anywhere else.
	 *
	 * Reset (destroyed) together with BaselineColors/CommittedColors/WorkingColors -- see
	 * BaselineColors' own doc comment for every reset point -- so a brand new session always rebuilds
	 * AO from scratch rather than risking a stale tree from a concluded operation. Within one active
	 * session this is deliberately NOT invalidated by Blend Mode/Opacity/Channel Filter/Preview Mode
	 * changes (those never touch this field at all), which is what lets repeated recomposition reuse
	 * the expensive raycast results instead of recomputing them on every unrelated parameter change.
	 */
	TUniquePtr<FVertexMaskForgeAOCache> AOCache;

	/** Sibling of AOCache, used ONLY when this entry is in Source-Topology mode (see
	 *  FVertexMaskForgeSelectedMesh::bUseSourceTopology). Same reset/lifecycle points as AOCache. */
	TUniquePtr<FVertexMaskForgeSourceTopologyAOCache> SourceTopologyAOCache;

	/**
	 * AUDITED (Nanite source-topology support): transient duplicate used ONLY when this entry is in
	 * Source-Topology mode, in place of PreviewComponent (a UStaticMeshComponent, which Nanite's
	 * renderer never reads OverrideVertexColors from). Renders WorkingMesh.Mesh directly (the SAME
	 * FDynamicMesh3 that will be committed), with its own Primary Color Overlay set from
	 * SourceTopologyWorkingColors -- the exact mechanism UE's own Paint Vertex Colors tool uses for its
	 * live preview (a UDynamicMeshComponent, never a UStaticMeshComponent -- see the native-tool audit).
	 * Same ownership/lifetime discipline as PreviewComponent (TStrongObjectPtr, RF_Transient, outer
	 * GetTransientPackage(), attached to SourceComponent for transform propagation only, never added to
	 * any Actor's serialized component list). Never both PreviewComponent and this are active for the
	 * same State at once -- see ApplyPreviewToEntry's domain branch.
	 */
	TStrongObjectPtr<UDynamicMeshComponent> SourceTopologyPreviewComponent;

	/**
	 * AUDITED (Nanite source-topology support): per-TRIANGLE-CORNER (not per render vertex, not per
	 * Dynamic Mesh vertex) baseline/committed/working colors -- domain mirrors exactly what gets
	 * committed (MeshDescription VertexInstanceColors, one slot per triangle corner), so two corners
	 * that happen to share a vertex position (a UV seam or hard edge) NEVER collapse onto a single
	 * slot, preserving their independently-authored baseline colors through composition exactly like
	 * the render-vertex arrays already do for render vertices. Same reset/seeding contract as
	 * BaselineColors/CommittedColors/WorkingColors (see those fields' own doc comments) -- only the
	 * indexing domain differs. Built/consumed by VertexMaskForgePanel::UpdateWorkingColorsSourceTopology.
	 * Reset together with the render-vertex arrays and AOCache/SourceTopologyAOCache at the same session
	 * end points.
	 */
	TArray<FColor> SourceTopologyBaselineColors;
	TArray<FColor> SourceTopologyCommittedColors;
	TArray<FColor> SourceTopologyWorkingColors;
};

namespace VertexMaskForgePanel
{
	/**
	 * AUDITED (V2-E CORRECTIVE PASS): compares EVERY live component's own World-Space normal MATRIX
	 * against a single reference matrix (the first valid one found) using full-matrix proportionality.
	 * Consumed both by the live UI conflict diagnostic (RunAutoUpdatePreview) and by the Accept target
	 * builders (VertexMaskForgeAcceptTargetBuilder::BuildAcceptTargets/BuildSourceTopologyAcceptTargets)
	 * as a re-check immediately before Accept is allowed to proceed -- see the definition in
	 * SVertexMaskForgePanel.cpp for the full algorithm doc comment.
	 */
	bool HasConflictingWorldSpaceNormalTransforms(
		const TArray<FVertexMaskForgePreviewComponentState>& PreviewComponents,
		float& OutMaxRelativeDeviation);
}

/**
 * One unique Static Mesh found in the current selection.
 * Kept small and self-contained for this checkpoint; safe to relocate to a
 * shared header once processing is introduced.
 */
struct FVertexMaskForgeSelectedMesh
{
	/** Editor-safe soft reference to the asset. Does not force it to stay loaded. */
	TSoftObjectPtr<UStaticMesh> Mesh;

	/** Cached display name, so rows do not need to resolve the soft pointer. */
	FString AssetName;

	/** Cached full asset path string, used both for display and as the dedup key. */
	FString AssetPathString;

	FVertexMaskForgeMeshDiagnostics Diagnostics;

	FVertexMaskForgeWorkingMesh WorkingMesh;

	/**
	 * True iff this entry's Static Mesh has Nanite enabled (Mesh->IsNaniteEnabled()) -- see
	 * VertexMaskForgePanel::ShouldUseSourceTopology. Computed once per Refresh Selection / Build
	 * Working Meshes, never inferred ad hoc elsewhere.
	 *
	 * AUDITED (domain-selection correction): deliberately NOT conditioned on WedgeMap validity. A
	 * Nanite mesh with an explicit High Res Source Model DOES have a valid WedgeMap on LOD 0, but
	 * Nanite's runtime renderer still never reads per-instance OverrideVertexColors (that limitation is
	 * about Nanite rendering, not about WedgeMap availability) -- routing such a mesh back to the
	 * render-vertex/OverrideVertexColors preview path would silently show nothing again, the exact bug
	 * this whole feature fixes. So EVERY Nanite-enabled mesh uses Source-Topology mode, unconditionally:
	 * Bounding Box/Ambient Occlusion are generated against WorkingMesh.Mesh (the SOURCE-topology
	 * FDynamicMesh3) via GenerateBoundingBoxMaskFromDynamicMesh / GenerateAmbientOcclusionMaskFromDynamicMesh
	 * -- the same domain UE's own Paint Vertex Colors tool uses, proven correct for Nanite by the
	 * native-tool audit. Render-Vertex mode is reserved for non-Nanite meshes only in this phase.
	 */
	bool bUseSourceTopology = false;

	/**
	 * Static Mesh Components in the tracked scene selection that reference this asset, with their
	 * non-destructive preview state. Populated only by CollectViewportSelection -- the sole selection
	 * source (Actors and directly-selected Components in the level; Content Browser asset selection
	 * is never consulted anywhere in this panel), so this is never empty for an entry that exists at
	 * all (every entry comes from at least one real, placed UStaticMeshComponent).
	 */
	TArray<FVertexMaskForgePreviewComponentState> PreviewComponents;
};

class SVertexMaskForgePanel : public SCompoundWidget, public FEditorUndoClient
{
public:
	SLATE_BEGIN_ARGS(SVertexMaskForgePanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Declared here and defined in the .cpp so FDynamicMesh3 only needs to be complete there. */
	virtual ~SVertexMaskForgePanel() override;

	/**
	 * FEditorUndoClient. Registered/unregistered via GEditor->RegisterForUndo/UnregisterForUndo in
	 * Construct()/the destructor. See HandlePostUndoRedo's own doc comment for the full contract --
	 * this is panel-state resync ONLY, never a substitute for Accept's own transactional write. */
	virtual void PostUndo(bool bSuccess) override;
	virtual void PostRedo(bool bSuccess) override;

private:
	void HandlePostUndoRedo(bool bSuccess, bool bIsRedo);

	/**
	 * Bound to USelection::SelectionChangedEvent (registered once in Construct(), removed in the
	 * destructor via SelectionChangedDelegateHandle). Fires for Actor/Component/BSP scene selection
	 * changes only; Content Browser asset selection is a completely separate system and was never
	 * routed through this delegate, matching this panel no longer consulting the Content Browser at
	 * all (see CollectViewportSelection, the only collector left). NewSelection (which USelection
	 * instance changed) is unused.
	 *
	 * AUDITED (UX1, explicit Edit Vertex Mask session entry): a scene selection change NEVER starts or
	 * retargets an editing session by itself -- RefreshSelection() (the function that actually builds
	 * WorkingMeshes/PreviewComponents and enters Editing) is called ONLY from OnEditVertexMaskClicked()
	 * now. While bIsEditingVertexMask is true, an active session's targets (SelectedMeshes) must never
	 * be silently retargeted or discarded just because the scene selection changed underneath it -- so
	 * this does NOT call anything that touches SelectedMeshes in that case; it only records that a
	 * candidate resync is owed, via bSceneSelectionChangedDuringActiveOperation, so the panel can catch
	 * up automatically -- without requiring another viewport/World Outliner click -- the moment the
	 * session actually concludes (see SyncSelectionIfChangedDuringOperation). While NOT editing, this
	 * only refreshes the lightweight candidate list used to enable/disable the "Edit Vertex Mask"
	 * button (RefreshCandidateSelection()) -- never WorkingMeshes, never previews, never hides anything.
	 */
	void OnEditorSelectionChanged(UObject* NewSelection);

	/**
	 * Re-queries the scene (viewport/World Outliner) selection and rebuilds the working set --
	 * i.e. STARTS an editing session (WorkingMeshes, baseline colors, and -- once a generator produces
	 * a Ready mask -- PreviewComponents/hidden originals, all via the existing UpdateAllPreviews/
	 * ApplyPreviewToEntry lifecycle, entirely unchanged by UX1). Called ONLY from
	 * OnEditVertexMaskClicked() now -- see that function's own doc comment. Left otherwise unmodified
	 * by UX1: still the single, existing lifecycle entry point the button reuses verbatim.
	 */
	void RefreshSelection();

	/**
	 * Lightweight Idle-state counterpart to RefreshSelection(): re-derives CandidateMeshes from the
	 * CURRENT scene selection via CollectViewportSelection()/UpdateMeshDiagnostics() only -- never
	 * BuildWorkingMeshes(), never DestroyAllPreviews()/UpdateAllPreviews(), never touches SelectedMeshes.
	 * Used purely to keep the "Edit Vertex Mask" button's enabled state and the Idle status text
	 * current; has no effect on any session, preview, generator, or asset.
	 */
	void RefreshCandidateSelection();

	/**
	 * Set true by OnEditorSelectionChanged() when the scene selection changes while
	 * bIsEditingVertexMask is true (i.e. while a session is active against targets captured at the
	 * moment "Edit Vertex Mask" was clicked). Consumed -- and cleared -- by
	 * SyncSelectionIfChangedDuringOperation(), called at the tail of exactly the two actions that can
	 * legitimately conclude a session: OnCancelChangesClicked() and AcceptPendingChanges() (on
	 * success). Deliberately NOT consumed by any other RecomputeOperationState() call site (e.g.
	 * ordinary mask regeneration flipping PendingChanges<->Idle mid-session), so an incidental state
	 * flip unrelated to actually concluding the session never triggers an unwanted resync.
	 */
	bool bSceneSelectionChangedDuringActiveOperation = false;

	/**
	 * If the scene selection changed while the session that just concluded was active (see
	 * bSceneSelectionChangedDuringActiveOperation), re-derives CandidateMeshes from the CURRENT scene
	 * selection via RefreshCandidateSelection() and clears the flag -- no extra viewport/World Outliner
	 * click required to re-enable "Edit Vertex Mask" for the new selection. No-ops if the selection
	 * never changed during the just-concluded session. AUDITED (UX1): no longer calls RefreshSelection()
	 * -- concluding a session must never automatically start another one (see requirement #6).
	 *
	 * MUST only be called AFTER the session's own targets have been fully validated/written (Accept) or
	 * fully discarded (Cancel) against the ORIGINAL SelectedMeshes, and after bIsEditingVertexMask has
	 * already been cleared -- calling this any earlier would let a resync silently retarget or interrupt
	 * an in-flight session, which must never happen.
	 */
	void SyncSelectionIfChangedDuringOperation();

	/**
	 * AUDITED (UX1, explicit Edit Vertex Mask session entry): the ONLY thing that starts an editing
	 * session now. True from the moment "Edit Vertex Mask" is clicked (RefreshSelection() has captured
	 * the then-current scene selection into SelectedMeshes) until Accept succeeds or Cancel runs.
	 * Independent of OperationState -- a session can be Editing with OperationState still Idle (no
	 * generator enabled/Ready yet); OperationState only tracks "is there a Ready mask to Accept",
	 * never "does a session exist". While true: OnEditorSelectionChanged() never touches SelectedMeshes;
	 * while false: SelectedMeshes is always empty (no WorkingMeshes, no PreviewComponents, no hidden
	 * originals, no generator/live-preview/Accept/Cancel activity -- all of those already read from
	 * SelectedMeshes and are therefore no-ops by construction while this is false).
	 */
	bool bIsEditingVertexMask = false;

	/**
	 * Idle-state-only candidate list, kept current by RefreshCandidateSelection() (called from
	 * Construct()'s tail and from OnEditorSelectionChanged() while not editing). Used exclusively to
	 * decide whether "Edit Vertex Mask" is enabled (CanEditVertexMask()) and for the Idle status text
	 * (GetEditSessionStatusText()) -- never read by any generator, composition, preview, or Accept/
	 * Cancel code, which all operate on SelectedMeshes (the session's own targets) instead. Left empty
	 * while a session is active (not maintained during Editing -- see OnEditorSelectionChanged).
	 */
	TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>> CandidateMeshes;

	/** True only while Idle (not already editing) and at least one valid candidate target is present. */
	bool CanEditVertexMask() const { return !bIsEditingVertexMask && !CandidateMeshes.IsEmpty(); }

	/**
	 * "Edit Vertex Mask" button handler -- the sole entry point into an editing session. Re-validates
	 * the selection is still eligible, then reuses RefreshSelection() UNCHANGED (it already re-queries
	 * the CURRENT scene selection and runs the existing WorkingMeshes/preview/baseline lifecycle) to
	 * capture a snapshot of the currently selected valid target(s) -- ALL of them, via the same
	 * CollectViewportSelection precedence/filtering every other entry point already uses, so multi-
	 * target sessions are preserved exactly as before. Idempotent against repeated clicks:
	 * CanEditVertexMask() (bound to the button's IsEnabled) is false the instant bIsEditingVertexMask
	 * becomes true, so a session can never be started twice.
	 */
	FReply OnEditVertexMaskClicked();

	/** Idle: reports the current candidate count ("no valid selection" / "N target(s) selected, click
	 *  Edit Vertex Mask to begin"). Editing: reports the ACTIVE SESSION's own target count (SelectedMeshes),
	 *  never the (possibly since-changed) external scene selection, to avoid ambiguity during Editing. */
	FText GetEditSessionStatusText() const;

	/**
	 * Gathers unique Static Meshes from UStaticMeshComponents in the CURRENT SCENE selection -- the
	 * ONLY selection source this panel consults (Content Browser asset selection was removed
	 * entirely; a UStaticMesh can only participate via a real, placed UStaticMeshComponent found
	 * here). Every candidate component, from either source below, passes through one centralized
	 * eligibility gate (IsEligibleComponent in the .cpp) that explicitly rejects invalid/pending-kill
	 * objects and this plugin's own transient preview components (by outer package and RF_Transient,
	 * not merely by the fact that they are never registered with any USelection set).
	 *
	 * Precedence between the two scene selection sources is DETERMINISTIC, not additive (audited
	 * against the UE 5.8 Level Editor's own click-handling source -- see the .cpp for the full
	 * evidence):
	 *   - If GEditor->GetSelectedComponents() contains ANY UStaticMeshComponent (an explicit
	 *     component-selection is active), those components ALONE are the targets -- their owning
	 *     Actor(s) are never auto-expanded to all of their other components, even though the owning
	 *     Actor legitimately remains present in GetSelectedActors() at the same time.
	 *   - Otherwise, falls back to every valid UStaticMeshComponent owned by every selected Actor
	 *     (GEditor->GetSelectedActors(), Actor->GetComponents<UStaticMeshComponent>()) -- the
	 *     pre-existing, already-validated Actor-granularity behavior.
	 * Never touches the Content Browser. See VertexMaskForgePanel::AddOrUpdateSelectedMesh for the
	 * per-component dedup (by pointer identity, never by UStaticMesh).
	 */
	void CollectViewportSelection(
		TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& InOutMeshes,
		TMap<FString, int32>& InOutPathToIndex) const;

	/**
	 * Resolves each entry's Static Mesh and computes its diagnostics.
	 * Soft pointers are only resolved here, for the duration of the refresh;
	 * no raw pointer is kept afterwards.
	 */
	void UpdateMeshDiagnostics(TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& InOutMeshes) const;

	/**
	 * Builds a transient, independent working copy (FDynamicMesh3) of each entry's LOD 0 and
	 * validates it. The source Static Mesh is only resolved for the duration of this call, and
	 * the source FMeshDescription copy does not outlive it either; only the resulting working
	 * mesh and its computed statistics are kept on the entry.
	 */
	void BuildWorkingMeshes(TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& InOutMeshes) const;

	// --- Bounding Box Mask (Local X / Local Y / Local Z, each with independent Local/World Space) --

	/**
	 * Builds one axis's UI row: a first line with the Enable checkbox labeled "Local X"/"Local Y"/
	 * "Local Z" (no separate directional text), then a second line with Position/Falloff/Invert/
	 * Mirror/World Space controls, all bound via lambdas capturing Axis by value). Called 3 times
	 * from Construct(); keeps the 3 axis rows from triplicating Slate code even though the UI itself
	 * has 3 copies. The visible "Falloff" label maps to the TransitionWidth field/parameter, which
	 * keeps its name -- see FVertexMaskForgeAxisMaskParams::TransitionWidth.
	 */
	TSharedRef<SWidget> BuildBoundingBoxAxisRow(EVertexMaskForgeBoundsAxis Axis, const FText& Title);

	/**
	 * Shared handler for a DISCRETE per-axis control change (Enable/Mirror/World Space -- anything
	 * that isn't a continuously-dragged slider and isn't per-axis Invert, see OnAxisInvertChanged for
	 * that exception): invalidates the current mask, cancels any pending debounce (a stale
	 * continuous-slider callback must never apply after a discrete change), and always regenerates
	 * immediately.
	 */
	void OnAxisParamChangedDiscrete();

	/**
	 * AUDITED (BBox Invert exception, follow-up audit): per-axis Invert's OWN handler, deliberately
	 * NOT OnAxisParamChangedDiscrete. Each axis's Invert flag is baked into GenerateBoundingBoxMask's
	 * per-axis gradient BEFORE the max-combination across axes -- three independent per-axis flags
	 * cannot be correctly represented as a single post-hoc, composition-time invert (see
	 * ComposeMaskStack), so Option A (preserve raw un-inverted values, invert during composition) was
	 * judged disproportionate for this axis-based design; Option B is implemented instead: Invert
	 * regenerates BoundingBoxMask immediately (regeneration itself is cheap here -- unlike Ambient
	 * Occlusion, Bounding Box has no persistent geometry cache to needlessly rebuild). Calls
	 * RunAutoUpdatePreview with bIncludeAO false so Ambient Occlusion's own slot/AOCache are never
	 * touched, not even a harmless re-validation.
	 */
	void OnAxisInvertChanged(int32 AxisIndex, ECheckBoxState NewState);

	/**
	 * AUDITED (raw/composition separation checkpoint): resets ONLY every selected entry's
	 * BoundingBoxMask slot back to NotGenerated (AmbientOcclusionMask is completely untouched), without
	 * touching the working mesh (FDynamicMesh3) itself. Called whenever a Bounding Box RAW/geometric
	 * parameter changes (axis Position/Falloff/Invert/Mirror/World Space/Enable, Unified Bounds) --
	 * NEVER for a purely compositional change (Blend Mode, Opacity -- see RecomposeWorkingColors
	 * instead). Never touches a Constant Fill mask's meaning -- Fill results are independent of these
	 * axis parameters.
	 */
	void InvalidateBoundingBoxRawMask();

	/**
	 * AUDITED (renamed for precision per explicit follow-up audit -- was InvalidateAORawMask, which
	 * was misleading). Resets ONLY the ENTRY-LEVEL DERIVED slot, FVertexMaskForgeWorkingMesh::
	 * AmbientOcclusionMask (a cheap Ready/Unavailable + UsedAOParams snapshot -- see that field's own
	 * doc comment; it never holds real per-vertex Values). Does NOT invalidate FVertexMaskForgeAOCache::
	 * RawValues (the actual raycast results) -- those are preserved; GenerateAmbientOcclusionMask's own
	 * cache key (Mesh identity/DerivedDataKey/counts/Transform/Samples/MaxDistance/Bias) transparently
	 * decides, the next time it runs, whether RawValues still match and can be reused (zero raycasts)
	 * or must be recomputed. BoundingBoxMask is completely untouched either way. Called when an AO RAW
	 * parameter changes (Samples, Max Distance, Bias) or when AO Enable turns on for an entry with no
	 * valid derived slot yet (see OnAOEnableChanged -- an entry that already has a Ready derived slot
	 * is deliberately NOT passed through this function at all, so its AOCache is never even queried).
	 * Never for AO Blend Mode/Opacity/Invert/Enable-when-already-Ready -- see RecomposeWorkingColors.
	 */
	void InvalidateAODerivedMask();

	/**
	 * AUDITED (raw/composition separation checkpoint): the PURE composition path -- Blend Mode,
	 * Opacity, Invert (Bounding Box or Ambient Occlusion), Enable/Disable of an already-Ready layer,
	 * Channel Filter, Preview Mode. Touches NEITHER BoundingBoxMask NOR AmbientOcclusionMask/AOCache --
	 * simply calls UpdateAllPreviews(false), which re-reads all compositional state live every call.
	 * Recomposes immediately and correctly with ZERO raycasts and ZERO Tree rebuilds.
	 */
	void RecomposeWorkingColors();

	/** Panel-level parameters for each of the 3 axes, indexed by EVertexMaskForgeBoundsAxis. Shared
	 *  across every selected entry; per-instance World Space evaluation reads a component's own
	 *  transform separately (see GenerateBoundingBoxMask) -- these parameters themselves never vary
	 *  per entry or per component. AUDITED (pre-modularization UI/defaults pass): every axis starts
	 *  disabled (bEnabled == false, the struct's own default) -- a fresh panel generates nothing
	 *  automatically until the user explicitly enables an axis. */
	TStaticArray<FVertexMaskForgeAxisMaskParams, 3> BoundingBoxAxisParams;

	// --- Bounding Box Mask: Blend Mode + Opacity --------------------------------------------

	TSharedRef<SWidget> OnGenerateBlendModeRow(TSharedPtr<EVertexMaskForgeBlendMode> InOption) const;

	/**
	 * Blend Mode is PURE composition -- it never affects the underlying mask's raw Values, only how
	 * ComposeMaskStack reads them -- so this recomposes immediately via RecomposeWorkingColors(),
	 * exactly like Channel Filter/Preview Mode.
	 */
	void OnBlendModeSelectionChanged(TSharedPtr<EVertexMaskForgeBlendMode> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetBlendModeButtonText() const;

	TArray<TSharedPtr<EVertexMaskForgeBlendMode>> BlendModeOptions;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgeBlendMode>>> BlendModeComboBox;

	/** How the Bounding Box Mask's scalar value combines with each enabled channel's existing value.
	 *  Default Copy: with Opacity 1.0, exactly reproduces the tool's pre-Blend-Mode behavior. */
	EVertexMaskForgeBlendMode BoundingBoxBlendMode = EVertexMaskForgeBlendMode::Copy;

	/** Opacity slider/spinbox's inline lambda calls RecomposeWorkingColors() directly (raw/composition
	 *  separation checkpoint) -- PURE composition, recomposes immediately regardless of Auto Update
	 *  Preview, no separate named handler needed. Clamped to [0, 1]; default 1.0 (full effect of the
	 *  Blend Mode -- matches the pre-Blend-Mode behavior when Mode == Copy). */
	float BoundingBoxOpacity = 1.0f;

	// --- Ambient Occlusion Mask --------------------------------------------------------------
	// AUDITED (composition-stack checkpoint): bAOEnabled is NO LONGER mutually exclusive with
	// Bounding Box -- it simply controls whether the Ambient Occlusion layer participates in the
	// composition stack (see ApplyPreviewToEntry). Bounding Box participates whenever at least one
	// axis is enabled, completely independently of bAOEnabled. Both can be Ready and both compose
	// together; either can be the only one active; both can be off. See GetActiveMaskSourceText() for
	// the UI readout of which layer(s) currently participate.

	ECheckBoxState GetAOEnableState() const { return bAOEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }

	/** AUDITED (re-examined per explicit follow-up audit): turning OFF is always pure composition.
	 *  Turning ON is pure composition TOO when every entry already has a Ready AmbientOcclusionMask
	 *  (reuses it verbatim, zero raycasts) -- only entries WITHOUT a valid derived mask trigger real
	 *  (re)generation, always immediate. Enabling, by itself, is never treated as a reason to
	 *  invalidate an already-valid mask. See the .cpp definition for the exact per-entry decision. */
	void OnAOEnableChanged(ECheckBoxState NewState);

	ECheckBoxState GetAOInvertState() const { return bAOInvert ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }

	/** Invert never touches the AO acceleration structure or the raw per-vertex samples (applied only
	 *  when FVertexMaskForgeScalarMask::Values is populated from the cached raw values -- see
	 *  VertexMaskForgePanel::GenerateAmbientOcclusionMask) -- still invalidates/regenerates through the
	 *  normal discrete-parameter path so the composed Preview picks up the new Values immediately. */
	void OnAOInvertChanged(ECheckBoxState NewState);

	/** Describes which layer(s) currently participate in the composition stack -- e.g. "Bounding Box +
	 *  Ambient Occlusion", "Bounding Box only", "Ambient Occlusion only", "None". */
	FText GetActiveMaskSourceText() const;

	TSharedRef<SWidget> OnGenerateAOBlendModeRow(TSharedPtr<EVertexMaskForgeBlendMode> InOption) const;

	/** Pure composition -- same treatment as OnBlendModeSelectionChanged (Bounding Box's own Blend
	 *  Mode combo): recomposes immediately via RecomposeWorkingColors(). */
	void OnAOBlendModeSelectionChanged(TSharedPtr<EVertexMaskForgeBlendMode> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetAOBlendModeButtonText() const;

	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgeBlendMode>>> AOBlendModeComboBox;

	/** How the Ambient Occlusion layer's scalar value combines with the RUNNING accumulated channel
	 *  value (i.e. whatever the Bounding Box layer, if also enabled, already produced -- see
	 *  ComposeMaskStack) -- independent of BoundingBoxBlendMode. Default Copy, matching
	 *  BoundingBoxBlendMode's own default/rationale. */
	EVertexMaskForgeBlendMode AOBlendMode = EVertexMaskForgeBlendMode::Copy;

	/** Same contract/range/default as BoundingBoxOpacity, independent of it. */
	float AOOpacity = 1.0f;

	/** Default false: preserves the tool's pre-AO behavior exactly (Ambient Occlusion does not
	 *  participate) until the user explicitly opts in. */
	bool bAOEnabled = false;

	bool bAOInvert = false;

	/** Clamped again in GenerateAmbientOcclusionMask itself; UI SpinBox already clamps to the same
	 *  [8, 256] range. */
	int32 AOSamples = 16;

	/** Unreal units (World Space); clamped again in GenerateAmbientOcclusionMask to (0, 10000]. */
	float AOMaxDistance = 100.0f;

	/** Unreal units (World Space); clamped again in GenerateAmbientOcclusionMask to [0.001, 10.0]. */
	float AOBias = 0.1f;

	/** AUDITED (AO Levels): see FVertexMaskForgeAOParams::LevelsMin's own doc comment for the exact
	 *  pipeline. UI range [0, 1]; default 0.0. Purely compositional -- see OnAOLevelsChanged. */
	float AOLevelsMin = 0.0f;

	/** AUDITED (AO Levels): see FVertexMaskForgeAOParams::LevelsMax's own doc comment. UI range [0, 1];
	 *  default 1.0. Purely compositional -- see OnAOLevelsChanged. */
	float AOLevelsMax = 1.0f;

	/** Same treatment as OnAOInvertChanged -- Levels never touches the AO acceleration structure or the
	 *  raw per-vertex/per-element samples (applied only when Values is populated from RawValues -- see
	 *  ApplyAOLevelsAndInvert), but still invalidates/regenerates through the normal discrete-parameter
	 *  path so the composed Preview picks up the new Values immediately. */
	void OnAOLevelsChanged();

	// --- Curvature Mask ----------------------------------------------------------------------
	// A third, independent, optional composition-stack layer -- structurally a peer of Bounding Box
	// and Ambient Occlusion (see EVertexMaskForgeScalarMaskSource::Curvature and ComposeMaskStack's own
	// doc comment: "no fixed role", any future generator plugs in exactly the same way). UNLIKE Ambient
	// Occlusion, Curvature never depends on a component's transform (see FVertexMaskForgeWorkingMesh::
	// CurvatureMask's own doc comment) -- it is generated exactly once per entry, real values included,
	// and every tracked component of that entry reuses the SAME entry-level result directly.

	ECheckBoxState GetCurvatureEnableState() const { return bCurvatureEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }

	/** Same enable/disable contract as OnAOEnableChanged (see its own doc comment): turning OFF is
	 *  always pure composition; turning ON reuses an already-Ready entry immediately (zero re-analysis)
	 *  and always regenerates immediately if genuinely needed. */
	void OnCurvatureEnableChanged(ECheckBoxState NewState);

	TSharedRef<SWidget> OnGenerateCurvatureTypeRow(TSharedPtr<EVertexMaskForgeCurvatureType> InOption) const;
	void OnCurvatureTypeSelectionChanged(TSharedPtr<EVertexMaskForgeCurvatureType> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetCurvatureTypeButtonText() const;

	TArray<TSharedPtr<EVertexMaskForgeCurvatureType>> CurvatureTypeOptions;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgeCurvatureType>>> CurvatureTypeComboBox;

	/** Convex/Concave/Both -- see EVertexMaskForgeCurvatureType. Default Both (per explicit requirement):
	 *  convex and concave regions both show, without cancelling each other out. */
	EVertexMaskForgeCurvatureType CurvatureType = EVertexMaskForgeCurvatureType::Concave;

	/** UI range [0, 10], default 1.0, clamped again in ApplyCurvatureArtisticParams to >= 0. Scales the
	 *  Type-extracted magnitude BEFORE Blur/Levels -- 0 removes the layer's contribution entirely, 1 is
	 *  the original analyzed intensity, >1 amplifies it. Never Opacity's role (see OnCurvatureParamChanged). */
	float CurvatureMultiplier = 1.0f;

	/** UI range [0, 10], default 0.0 (no blur). Integer part = full topological-averaging iterations;
	 *  fractional part = a further lerp toward one more iteration -- see
	 *  VertexMaskForgePanel::ApplyTopologicalCurvatureBlur for the exact algorithm. Purely a downstream
	 *  reprocessing of the cached raw magnitudes; never re-derives adjacency/dihedral angles. */
	float CurvatureBlur = 0.0f;

	/** Same semantics as AOLevelsMin, applied to the Type/Multiplier/Blur result (see
	 *  VertexMaskForgePanel::ApplyCurvatureLevels) -- UI range [0, 1], default 0.0. Purely compositional;
	 *  see OnCurvatureParamChanged. */
	float CurvatureLevelsMin = 0.0f;

	/** Same semantics as AOLevelsMax -- UI range [0, 1], default 1.0. */
	float CurvatureLevelsMax = 1.0f;

	ECheckBoxState GetCurvatureInvertState() const { return bCurvatureInvert ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }

	/** Applied LAST, after Levels Min/Max, over the already-leveled [0,1] result (Mask = 1 - Mask) --
	 *  see OnCurvatureParamChanged's own doc comment for the exact pipeline order. Same treatment as
	 *  OnAOInvertChanged: never touches the cached raw Convex/Concave magnitudes, purely compositional,
	 *  routed through the same shared OnCurvatureParamChanged reprocess. Default false. */
	void OnCurvatureInvertChanged(ECheckBoxState NewState);

	bool bCurvatureInvert = false;

	/**
	 * Shared handler for Curvature Type / Multiplier / Blur / Levels Min / Levels Max / Invert: ALL of
	 * these are cheap, PURELY DOWNSTREAM reprocessing of the already-cached raw Convex/Concave magnitude
	 * arrays (see FVertexMaskForgeWorkingMesh::CurvatureRawConvexCache's own doc comment) -- none of
	 * them ever re-run the adjacency/dihedral-angle analysis or touch GeometryFingerprint, so (unlike
	 * Bounding Box axis parameters or AO Samples/Max Distance/Bias) this is UNCONDITIONAL and immediate,
	 * exactly like OnAOInvertChanged/OnAOLevelsChanged. Regenerates
	 * every SelectedMeshes entry's CurvatureMask directly from its cached raw magnitudes (entry-level,
	 * real values -- see FVertexMaskForgeWorkingMesh::CurvatureMask), then recomposes. Pipeline order:
	 * raw Convex/Concave -> Curvature Type -> Multiplier -> Blur -> Levels Min/Max -> Invert -> (Opacity
	 * + Blend Mode, applied downstream by ComposeMaskStack, unchanged).
	 */
	void OnCurvatureParamChanged();

	TSharedRef<SWidget> OnGenerateCurvatureBlendModeRow(TSharedPtr<EVertexMaskForgeBlendMode> InOption) const;
	void OnCurvatureBlendModeSelectionChanged(TSharedPtr<EVertexMaskForgeBlendMode> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetCurvatureBlendModeButtonText() const;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgeBlendMode>>> CurvatureBlendModeComboBox;

	/** Same contract/range as BoundingBoxBlendMode/AOBlendMode. Default Copy -- preserves this tool's
	 *  established "every new layer defaults to Copy" convention (BoundingBoxBlendMode and AOBlendMode
	 *  both default to Copy; Add was considered but rejected specifically to keep that convention
	 *  consistent -- see the Curvature implementation report). */
	EVertexMaskForgeBlendMode CurvatureBlendMode = EVertexMaskForgeBlendMode::Copy;

	/** Same contract/range/default as BoundingBoxOpacity/AOOpacity, independent of them. Applied ONLY
	 *  during final composition (ComposeMaskStack), never to CurvatureMask.Values itself -- Opacity 0
	 *  leaves the layer with no influence without erasing what Bounding Box/Ambient Occlusion already
	 *  produced (same contract every existing layer's Opacity already has). */
	float CurvatureOpacity = 1.0f;

	bool bCurvatureEnabled = false;

	// --- Noise Mask (V1: procedural 3D Perlin/FBM, Local Space only) -------------------------
	// A fourth, independent, optional composition-stack layer -- structurally a peer of Bounding Box,
	// Ambient Occlusion, and Curvature (see EVertexMaskForgeScalarMaskSource::Noise and
	// ComposeMaskStack's own doc comment: "no fixed role"). Like Curvature, Noise never depends on a
	// component's transform (pure function of LOCAL-SPACE position) -- generated exactly once per
	// entry, real values included, reused directly by every tracked component of that entry. UNLIKE
	// Curvature, its raw pattern DOES depend on artist-chosen generative parameters (Scale/Offset/Seed/
	// Octaves/Roughness/Lacunarity/Type) -- see FVertexMaskForgeNoiseGenerativeParams and
	// FVertexMaskForgeWorkingMesh::NoiseRawCache's own doc comments for the resulting two-tier
	// invalidation contract (generative params gate a real, always-immediate regeneration, exactly
	// like Bounding Box axis params / AO Samples-MaxDistance-Bias; Multiplier/Levels/Invert/
	// Opacity/Blend Mode are cheap, immediate, unconditional reprocessing, exactly like Curvature's own
	// Type/Multiplier/Blur/Levels/Invert).

	ECheckBoxState GetNoiseEnableState() const { return bNoiseEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }

	/** Same enable/disable contract as OnAOEnableChanged/OnCurvatureEnableChanged (see their own doc
	 *  comments): turning OFF is always pure composition; turning ON reuses an already-Ready entry
	 *  immediately (zero re-evaluation) and always regenerates immediately if genuinely needed. */
	void OnNoiseEnableChanged(ECheckBoxState NewState);

	TSharedRef<SWidget> OnGenerateNoiseTypeRow(TSharedPtr<EVertexMaskForgeNoiseType> InOption) const;
	void OnNoiseTypeSelectionChanged(TSharedPtr<EVertexMaskForgeNoiseType> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetNoiseTypeButtonText() const;

	TArray<TSharedPtr<EVertexMaskForgeNoiseType>> NoiseTypeOptions;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgeNoiseType>>> NoiseTypeComboBox;

	/** Perlin or FractalPerlin -- see EVertexMaskForgeNoiseType. Default FractalPerlin (per explicit
	 *  requirement). GENERATIVE -- see OnNoiseGenerativeParamChanged. */
	EVertexMaskForgeNoiseType NoiseType = EVertexMaskForgeNoiseType::FractalPerlin;

	/** Per-axis frequency multiplier. UI minimum a small positive epsilon (never exactly 0 -- see
	 *  ComputeRawNoiseValue's own clamp). Default 1.0 each. GENERATIVE. */
	float NoiseScaleX = 1.0f;
	float NoiseScaleY = 1.0f;
	float NoiseScaleZ = 1.0f;

	/** UI/workflow-only toggle (V2-A adjustment) -- when true, Scale Y/Z are driven from Scale X and
	 *  disabled for direct editing (see OnNoiseScaleXChanged/OnNoiseScaleAxesLockChanged). NOT part of
	 *  FVertexMaskForgeNoiseGenerativeParams: the generative result is already fully determined by the
	 *  final Scale X/Y/Z values themselves, so this flag has nothing left to contribute to the cache key.
	 *  Default false -- preserves the exact pre-existing independent-axes behavior. */
	bool bNoiseScaleAxesLocked = false;

	ECheckBoxState GetNoiseScaleAxesLockState() const { return bNoiseScaleAxesLocked ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }

	/** Turning ON immediately snaps Y/Z to the current X (at most one InvalidateNoiseRawMask + one Auto
	 *  Update, only if a value actually changed). Turning OFF only flips the flag -- no value changes, so
	 *  no invalidation and no Auto Update (see OnNoiseScaleAxesLockChanged's own doc comment). */
	void OnNoiseScaleAxesLockChanged(ECheckBoxState NewState);

	/** Dedicated, atomic handler for the Scale X spin box -- when bNoiseScaleAxesLocked is true, also
	 *  applies the same final value to Scale Y and Scale Z in the SAME call, so a single edit invalidates
	 *  Raw Noise and requests Auto Update exactly ONCE (never three times via three separate handler
	 *  calls). When unlocked, behaves exactly like the pre-existing Scale X handler. */
	void OnNoiseScaleXChanged(float NewValue);

	/** Domain offset in noise space. Default 0 each. GENERATIVE. */
	float NoiseOffsetX = 0.0f;
	float NoiseOffsetY = 0.0f;
	float NoiseOffsetZ = 0.0f;

	/** Hashed deterministically into a 3D domain offset -- never engine RNG state. Default 0. GENERATIVE. */
	int32 NoiseSeed = 0;

	/** FractalPerlin only. UI range [1, 8]; default 4. GENERATIVE. */
	int32 NoiseOctaves = 4;

	/** FractalPerlin only. UI range [0, 1]; default 0.5. GENERATIVE. */
	float NoiseRoughness = 0.5f;

	/** FractalPerlin/Billow/Ridged/Turbulence only. UI minimum 1.0; default 2.0. GENERATIVE. */
	float NoiseLacunarity = 2.0f;

	/** Turbulence only. UI range [0, 5]; default 0.5. GENERATIVE. */
	float NoiseTurbulenceStrength = 0.5f;

	/** True for every Noise Type whose raw pattern is a weighted sum of octaves (FractalPerlin, Billow,
	 *  Ridged, Turbulence, Alligator) -- false for the single-sample Perlin AND for the three V2-B
	 *  cellular types (WorleyF1/WorleyF2MinusF1/Voronoi are not multi-octave; Octaves/Roughness/
	 *  Lacunarity are unused by ComputeRawNoiseValue's cellular branch). Small, localized helper so the
	 *  Octaves/Roughness/Lacunarity IsEnabled bindings don't each repeat their own type comparison. */
	bool UsesFractalParameters() const
	{
		switch (NoiseType)
		{
		case EVertexMaskForgeNoiseType::FractalPerlin:
		case EVertexMaskForgeNoiseType::Billow:
		case EVertexMaskForgeNoiseType::Ridged:
		case EVertexMaskForgeNoiseType::Turbulence:
		case EVertexMaskForgeNoiseType::Alligator:
			return true;
		default:
			return false;
		}
	}

	/**
	 * Shared handler for ALL generative Noise parameters (Type/Scale XYZ/Offset XYZ/Seed/Octaves/
	 * Roughness/Lacunarity/Turbulence Strength): these change WHAT the raw pattern looks like, so -- exactly like Bounding
	 * Box axis parameters (OnAxisParamChangedDiscrete) and AO's Samples/Max Distance/Bias
	 * (InvalidateAODerivedMask) -- this invalidates every selected entry's NoiseMask (NOT NoiseRawCache
	 * itself; EnsureNoiseRawCache's own GeometryFingerprint+params comparison decides reuse lazily, the
	 * next time Noise is actually (re)generated) and always regenerates immediately.
	 */
	void OnNoiseGenerativeParamChanged();

	/** Same contract/range as BoundingBoxOpacity/AOOpacity/CurvatureMultiplier's own scale role -- UI
	 *  range [0, 10], default 1.0. ARTISTIC -- see OnNoiseArtisticParamChanged. */
	float NoiseMultiplier = 1.0f;

	/** V2-C: universal blur, applied to the raw procedural field BEFORE Multiplier/Levels/Invert (see
	 *  ComputeRawNoiseValue's seven-tap kernel). UI range [0, 1]; default 0.0. GENERATIVE (unlike
	 *  Multiplier right next to it in the UI) -- see OnNoiseGenerativeParamChanged. Positioned in the UI
	 *  immediately after Multiplier (Seed -> Multiplier -> Blur) per the explicit layout requirement,
	 *  even though it is evaluated FIRST in the actual pipeline. */
	float NoiseBlur = 0.0f;

	/** Same semantics as AOLevelsMin/CurvatureLevelsMin. UI range [0, 1]; default 0.0. ARTISTIC. */
	float NoiseLevelsMin = 0.0f;

	/** Same semantics as AOLevelsMax/CurvatureLevelsMax. UI range [0, 1]; default 1.0. ARTISTIC. */
	float NoiseLevelsMax = 1.0f;

	ECheckBoxState GetNoiseInvertState() const { return bNoiseInvert ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }

	/** Applied LAST, after Levels Min/Max, over the already-leveled [0,1] result (Mask = 1 - Mask) --
	 *  same treatment as OnCurvatureInvertChanged/OnAOInvertChanged. Default false. ARTISTIC. */
	void OnNoiseInvertChanged(ECheckBoxState NewState);

	bool bNoiseInvert = false;

	/**
	 * Shared handler for Multiplier / Levels Min / Levels Max / Invert: ALL cheap, PURELY DOWNSTREAM
	 * reprocessing of the already-cached NoiseRawCache (see that field's own doc comment) -- never the
	 * per-vertex Perlin/FBM evaluation, never GeometryFingerprint, never the generative-params
	 * comparison. So this is UNCONDITIONAL and immediate, exactly
	 * like OnCurvatureParamChanged. Regenerates every SelectedMeshes entry's NoiseMask directly from its
	 * cached NoiseRawCache (entry-level, real values -- see FVertexMaskForgeWorkingMesh::NoiseMask),
	 * then recomposes.
	 */
	void OnNoiseArtisticParamChanged();

	TSharedRef<SWidget> OnGenerateNoiseBlendModeRow(TSharedPtr<EVertexMaskForgeBlendMode> InOption) const;
	void OnNoiseBlendModeSelectionChanged(TSharedPtr<EVertexMaskForgeBlendMode> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetNoiseBlendModeButtonText() const;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgeBlendMode>>> NoiseBlendModeComboBox;

	/** Same contract/range/default (Copy) as BoundingBoxBlendMode/AOBlendMode/CurvatureBlendMode --
	 *  preserves the established "every new layer defaults to Copy" convention. ARTISTIC (pure
	 *  composition, see RecomposeWorkingColors). */
	EVertexMaskForgeBlendMode NoiseBlendMode = EVertexMaskForgeBlendMode::Copy;

	/** Same contract/range/default as BoundingBoxOpacity/AOOpacity/CurvatureOpacity, independent of
	 *  them. Applied ONLY during final composition (ComposeMaskStack), never to NoiseMask.Values itself.
	 *  ARTISTIC. */
	float NoiseOpacity = 1.0f;

	bool bNoiseEnabled = false;

	/**
	 * AUDITED (Noise V1): resets ONLY every selected entry's NoiseMask slot back to NotGenerated (never
	 * touches NoiseRawCache/NoiseCacheFingerprint/NoiseCacheUsedParams directly -- see NoiseRawCache's
	 * own doc comment for why that is safe: the next real generation re-derives reuse-or-recompute from
	 * the fingerprint+params comparison on its own). Same "raw/composition separation" contract as
	 * InvalidateBoundingBoxRawMask/InvalidateAODerivedMask -- called ONLY by generative parameter
	 * changes (OnNoiseGenerativeParamChanged), never by artistic ones.
	 */
	void InvalidateNoiseRawMask();

	// --- Material Slot Mask (V2-D) -----------------------------------------------------------
	// A fifth, independent, optional composition-stack layer -- a structural peer of Bounding Box, AO,
	// Curvature, and Noise (see EVertexMaskForgeScalarMaskSource::MaterialSlot). Transform-independent
	// like Curvature/Noise (generated once per entry, real values, no per-component re-evaluation).
	// V1 SCOPE: requires exactly one selected mesh (the dropdown represents "the slots of THAT mesh") --
	// see IsMaterialSlotMaskAvailableForSelection.

	ECheckBoxState GetMaterialSlotMaskEnableState() const { return bMaterialSlotMaskEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }

	/** Same enable/disable contract as OnCurvatureEnableChanged/OnNoiseEnableChanged: turning OFF is
	 *  always pure composition; turning ON reuses an already-Ready entry immediately and always
	 *  regenerates immediately if genuinely needed. */
	void OnMaterialSlotMaskEnableChanged(ECheckBoxState NewState);
	bool bMaterialSlotMaskEnabled = false;

	/** True only when exactly one entry is selected -- the V1 scope requirement (see this section's own
	 *  header comment). Never true for zero or multiple selected meshes, regardless of bMaterialSlotMaskEnabled. */
	bool IsMaterialSlotMaskAvailableForSelection() const { return SelectedMeshes.Num() == 1; }

	/** Short, user-facing reason Material Slot Mask is currently unavailable/invalid for the current
	 *  single-mesh selection (empty if available and valid). Never touches any other generator. */
	FText GetMaterialSlotMaskDiagnosticText() const;

	TSharedRef<SWidget> OnGenerateMaterialSlotRow(TSharedPtr<FVertexMaskForgeMaterialSlotInfo> InOption) const;
	void OnMaterialSlotSelectionChanged(TSharedPtr<FVertexMaskForgeMaterialSlotInfo> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetMaterialSlotButtonText() const;

	/** Rebuilt every RefreshSelection from the single selected entry's WorkingMesh.MaterialSlotOptions
	 *  (see ReconcileMaterialSlotSelection) -- never stale across a mesh/selection change. */
	TArray<TSharedPtr<FVertexMaskForgeMaterialSlotInfo>> MaterialSlotOptions;
	TSharedPtr<SComboBox<TSharedPtr<FVertexMaskForgeMaterialSlotInfo>>> MaterialSlotComboBox;

	/** Real index into GetStaticMaterials() for the currently selected mesh. GENERATIVE -- see
	 *  OnMaterialSlotMaskGenerativeParamChanged. Reconciled (never left stale/out-of-range) by
	 *  ReconcileMaterialSlotSelection every RefreshSelection. */
	int32 SelectedMaterialSlotIndex = 0;

	/**
	 * Shared handler for Material Slot Mask's generative parameters (which slot is selected, Invert --
	 * both change WHAT the raw binary mask looks like): invalidates the entry's MaterialSlotMask (reset
	 * to NotGenerated) and always regenerates immediately -- same contract as
	 * OnNoiseGenerativeParamChanged.
	 */
	void OnMaterialSlotMaskGenerativeParamChanged();
	void InvalidateMaterialSlotMaskRawMask();

	ECheckBoxState GetMaterialSlotMaskInvertState() const { return bMaterialSlotMaskInvert ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }
	void OnMaterialSlotMaskInvertChanged(ECheckBoxState NewState);
	bool bMaterialSlotMaskInvert = false;

	TSharedRef<SWidget> OnGenerateMaterialSlotMaskBlendModeRow(TSharedPtr<EVertexMaskForgeBlendMode> InOption) const;
	void OnMaterialSlotMaskBlendModeSelectionChanged(TSharedPtr<EVertexMaskForgeBlendMode> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetMaterialSlotMaskBlendModeButtonText() const;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgeBlendMode>>> MaterialSlotMaskBlendModeComboBox;

	/** Same "every new layer defaults to Copy" convention as every other generator. ARTISTIC (pure
	 *  composition, see RecomposeWorkingColors) -- never regenerates the raw mask. */
	EVertexMaskForgeBlendMode MaterialSlotMaskBlendMode = EVertexMaskForgeBlendMode::Copy;

	/** Same contract/range/default as BoundingBoxOpacity/AOOpacity/CurvatureOpacity/NoiseOpacity,
	 *  independent of them. Applied ONLY during final composition (ComposeMaskStack), never to
	 *  MaterialSlotMask.Values itself. ARTISTIC. */
	float MaterialSlotMaskOpacity = 1.0f;

	/**
	 * AUDITED (V2-D): rebuilds MaterialSlotOptions (the dropdown's own list) from the single selected
	 * entry's WorkingMesh.MaterialSlotOptions, and validates/reconciles SelectedMaterialSlotIndex
	 * against it -- preserves the previous index if it still exists in the new list, otherwise falls
	 * back to the first available slot (index 0 of GetStaticMaterials()), never leaving a stale index
	 * from a different mesh selected. Called at the end of RefreshSelection, unconditionally (cheap:
	 * only touches this section's own UI-facing arrays, never any other generator's state). A zero- or
	 * multi-mesh selection clears MaterialSlotOptions to empty (IsMaterialSlotMaskAvailableForSelection
	 * already gates the UI/generation in that case).
	 */
	void ReconcileMaterialSlotSelection();

	// --- Directional Normal Mask (V2-E) --------------------------------------------------------
	// A sixth, independent, optional composition-stack layer -- structural peer of Bounding Box/AO/
	// Curvature/Noise/Material Slot. VISUAL panel position is Curvature -> Directional Normal -> Noise
	// (see Construct()); enum value is appended AFTER MaterialSlot to preserve numeric stability of
	// every existing EVertexMaskForgeScalarMaskSource value (the two orderings are independent, per the
	// explicit "position and enum are decoupled" allowance).

	ECheckBoxState GetDirectionalNormalMaskEnableState() const { return bDirectionalNormalMaskEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }
	void OnDirectionalNormalMaskEnableChanged(ECheckBoxState NewState);
	bool bDirectionalNormalMaskEnabled = false;

	TSharedRef<SWidget> OnGenerateNormalSpaceRow(TSharedPtr<EVertexMaskForgeNormalSpace> InOption) const;
	void OnNormalSpaceSelectionChanged(TSharedPtr<EVertexMaskForgeNormalSpace> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetNormalSpaceButtonText() const;
	TArray<TSharedPtr<EVertexMaskForgeNormalSpace>> NormalSpaceOptions;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgeNormalSpace>>> NormalSpaceComboBox;

	/** UI default World (per the explicit suggested default) -- GENERATIVE, see
	 *  OnDirectionalNormalMaskGenerativeParamChanged. */
	EVertexMaskForgeNormalSpace DirectionalNormalSpace = EVertexMaskForgeNormalSpace::Local;

	TSharedRef<SWidget> OnGenerateNormalDirectionRow(TSharedPtr<EVertexMaskForgeNormalDirection> InOption) const;
	void OnNormalDirectionSelectionChanged(TSharedPtr<EVertexMaskForgeNormalDirection> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetNormalDirectionButtonText() const;
	TArray<TSharedPtr<EVertexMaskForgeNormalDirection>> NormalDirectionOptions;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgeNormalDirection>>> NormalDirectionComboBox;

	/** Default Z+ (Up), per the explicit suggested default. GENERATIVE. */
	EVertexMaskForgeNormalDirection DirectionalNormalDirection = EVertexMaskForgeNormalDirection::PositiveZ;

	/** Degrees, UI range [0, 180]; default 90. GENERATIVE. */
	float DirectionalNormalAngle = 90.0f;

	/** Degrees, UI range [0, 180]; default 45 -- internally clamped to [0, Angle] at generation time
	 *  (see ComputeDirectionalNormalRawValue), never causing a division by zero or NaN. GENERATIVE. */
	float DirectionalNormalFalloff = 45.0f;

	/**
	 * Topological smoothing of the raw Directional Normal Mask, applied BEFORE Invert -- same algorithm
	 * shape, range, default, and "whole number = full iterations, fractional part blends toward one
	 * more" semantics as CurvatureBlur (see ApplyTopologicalCurvatureBlur's own doc comment), adapted to
	 * a domain-appropriate adjacency (render-vertex index-buffer adjacency for non-Nanite, corner/
	 * triangle-neighbor adjacency for Source-Topology -- see VertexMaskForgePanel::
	 * ApplyAdjacencyTopologicalBlur for why CurvatureBlur's own Dynamic-Mesh-Vertex-ID adjacency
	 * cannot be reused directly without collapsing split normals at hard edges/UV seams). UI range
	 * [0, 10]; default 0.0 -- Blur <= 0 is an exact no-op (bit-for-bit identical to no Blur at all).
	 * GENERATIVE.
	 */
	float DirectionalNormalBlur = 0.0f;

	/**
	 * Shared handler for Directional Normal Mask's generative parameters (Space/Direction/Angle/
	 * Falloff -- all change WHAT the raw angular pattern looks like): invalidates the entry's
	 * DirectionalNormalMask and always regenerates immediately -- same contract as
	 * OnNoiseGenerativeParamChanged/OnMaterialSlotMaskGenerativeParamChanged. Never touches
	 * AO/Curvature/Noise/Material Slot state or caches.
	 */
	void OnDirectionalNormalMaskGenerativeParamChanged();
	void InvalidateDirectionalNormalMaskRawMask();

	ECheckBoxState GetDirectionalNormalMaskInvertState() const { return bDirectionalNormalMaskInvert ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }
	void OnDirectionalNormalMaskInvertChanged(ECheckBoxState NewState);
	bool bDirectionalNormalMaskInvert = false;

	TSharedRef<SWidget> OnGenerateDirectionalNormalMaskBlendModeRow(TSharedPtr<EVertexMaskForgeBlendMode> InOption) const;
	void OnDirectionalNormalMaskBlendModeSelectionChanged(TSharedPtr<EVertexMaskForgeBlendMode> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetDirectionalNormalMaskBlendModeButtonText() const;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgeBlendMode>>> DirectionalNormalMaskBlendModeComboBox;

	/** Same "every new layer defaults to Copy" convention as every other generator. ARTISTIC (pure
	 *  composition, see RecomposeWorkingColors) -- never regenerates the raw mask. */
	EVertexMaskForgeBlendMode DirectionalNormalMaskBlendMode = EVertexMaskForgeBlendMode::Copy;

	/** Same contract/range/default as every other generator's own Opacity. Applied ONLY during final
	 *  composition, never to DirectionalNormalMask.Values itself. ARTISTIC. */
	float DirectionalNormalMaskOpacity = 1.0f;

	/** Short, user-facing reason Directional Normal Mask is currently invalid/blocked (empty if valid) --
	 *  e.g. degenerate normal, unresolved corner mapping, degenerate World transform, or a World-Space
	 *  multi-instance conflict (see FVertexMaskForgeWorkingMesh::bDirectionalNormalWorldSpaceConflict). */
	FText GetDirectionalNormalMaskDiagnosticText() const;

	// --- Thickness Mask (V2-G) ---------------------------------------------------------------
	// A seventh, independent, optional composition-stack layer -- structural peer of every generator
	// above. VISUAL panel position is Directional Normal -> Thickness -> Noise (see Construct()); enum
	// value is appended after DirectionalNormal (see EVertexMaskForgeScalarMaskSource's own doc note).
	// Asset Local Space ONLY -- never a seletor, never depends on Component/Actor Transform.

	ECheckBoxState GetThicknessMaskEnableState() const { return bThicknessMaskEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }
	void OnThicknessMaskEnableChanged(ECheckBoxState NewState);
	bool bThicknessMaskEnabled = false;

	TSharedRef<SWidget> OnGenerateThicknessMaskBlendModeRow(TSharedPtr<EVertexMaskForgeBlendMode> InOption) const;
	void OnThicknessMaskBlendModeSelectionChanged(TSharedPtr<EVertexMaskForgeBlendMode> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetThicknessMaskBlendModeButtonText() const;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgeBlendMode>>> ThicknessMaskBlendModeComboBox;

	/** Same "every new layer defaults to Copy" convention as every other generator. ARTISTIC. */
	EVertexMaskForgeBlendMode ThicknessMaskBlendMode = EVertexMaskForgeBlendMode::Copy;

	/** Same contract/range/default as every other generator's own Opacity. ARTISTIC. */
	float ThicknessMaskOpacity = 1.0f;

	/** Measured thickness (local-space units) at or below this value reads as white (1.0) after
	 *  normalization, before Invert. UI range [0, 10000]; default 0.0. GENERATIVE (renormalizes only --
	 *  never triggers a new raycast, see GenerateThicknessMask's own cache contract). */
	float ThicknessMinThickness = 0.0f;

	/** Measured thickness at or above this value reads as black (0.0) after normalization, before
	 *  Invert. UI range [0, 10000]; default 50.0 -- strictly below the default SearchDistance (100.0) so
	 *  the initial range is a genuinely useful, non-degenerate interval (not merely "corrected" by
	 *  SanitizeThicknessParams at generation time) and the raycast retains headroom beyond the visible
	 *  saturation point. Sanitized together with MinThickness/SearchDistance/Bias at generation time --
	 *  see VertexMaskForgePanel::SanitizeThicknessParams. GENERATIVE (renormalizes only). */
	float ThicknessMaxThickness = 50.0f;

	/** Maximum physical raycast distance from the origin surface, in local-space units -- distinct from
	 *  MaxThickness (the artistic saturation point): a hit beyond MaxThickness but within SearchDistance
	 *  is still a valid "black" measurement, not a missing one. UI range [0, 10000]; default 100.0.
	 *  GENERATIVE (triggers a new raycast -- see GenerateThicknessMask's own cache contract). */
	float ThicknessSearchDistance = 100.0f;

	/** Ray origin offset along -Normal (into the mesh), local-space units, used ONLY to avoid self-hit;
	 *  reconstructed back out of the measured distance (MeasuredThickness = HitT + EffectiveBias) so it
	 *  never artistically shifts the result -- see ComputeThicknessRawValue. UI range [0.001, 10.0];
	 *  default 0.01. GENERATIVE (triggers a new raycast). */
	float ThicknessBias = 0.01f;

	/** Topological smoothing of the normalized Thickness mask, applied BEFORE Invert -- same algorithm/
	 *  adjacency/seam-awareness as DirectionalNormalBlur (reuses BuildCornerAdjacency/
	 *  BuildRenderVertexAdjacency/ApplyAdjacencyTopologicalBlur verbatim). UI range [0, 10]; default 0.0
	 *  -- Blur <= 0 is an exact no-op. GENERATIVE (post-processing only -- never triggers a new raycast).
	 */
	float ThicknessBlur = 0.0f;

	/**
	 * Shared handler for Thickness's raycast-affecting generative parameters (SearchDistance/Bias):
	 * invalidates the entry's ThicknessMask (raw hit distances + everything downstream) and always
	 * regenerates immediately. Never touches AO/Curvature/Noise/Material Slot/Directional Normal state
	 * or caches.
	 */
	void OnThicknessRaycastParamChanged();

	/** Shared handler for Min/Max Thickness/Blur (post-raycast only -- renormalizes/re-blurs the
	 *  already-cached raw hit distances, never repeats the raycast). */
	void OnThicknessPostProcessParamChanged();

	void InvalidateThicknessMaskRawMask();

	ECheckBoxState GetThicknessMaskInvertState() const { return bThicknessMaskInvert ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }
	void OnThicknessMaskInvertChanged(ECheckBoxState NewState);
	bool bThicknessMaskInvert = false;

	/** Short, user-facing reason Thickness Mask is currently invalid/blocked/partial (empty if fully
	 *  valid) -- e.g. no opposite surface found, degenerate geometry, Source-Topology mapping invalid. */
	FText GetThicknessMaskDiagnosticText() const;

	// --- Fill White / Fill Black utility masks ----------------------------------------------

	FReply OnFillWhiteClicked();
	FReply OnFillBlackClicked();

	/**
	 * Shared implementation for both Fill buttons: cancels any pending live-update debounce first (a
	 * Fill must never be overwritten moments later by a stale regeneration), then, for every
	 * SelectedMeshes entry that passes the SAME entry-level validity gating as live generation
	 * (WorkingMesh Ready, resolvable Static Mesh, valid LOD 0 render data), generates a dense
	 * constant-valued mask (VertexMaskForgePanel::GenerateConstantMask) and assigns it to that
	 * entry's mask. An entry that fails validation here is left COMPLETELY UNTOUCHED (its previous
	 * mask, if any, is preserved) rather than reset to Unavailable -- per the explicit "preserve the
	 * last valid Preview on failure" requirement. Ends with UpdateAllPreviews(), which recomposes/
	 * reapplies the transient Preview (reusing the exact same ApplyPreviewToEntry/UpdateWorkingColors
	 * path as every other mask) and marks Pending Changes via RecomputeOperationState(). Fill White/
	 * Black are explicit, standalone actions on the current base -- they are never required to make a
	 * live parameter change visible, and never repair/resize any buffer that live generation itself
	 * should already have prepared.
	 */
	void RunConstantFill(float ConstantValue, EVertexMaskForgeScalarMaskSource Source, const FText& SuccessMessage);

	/** Enabled only when at least one selected entry has a Ready working mesh, and not while Applying. */
	bool CanRunFill() const;

	FText GetMaskActionStatusText() const { return LastMaskActionStatusText; }

	/** Success/partial-failure message for the last Fill action; cleared by the next mask-changing
	 *  action (Fill, parameter change, Refresh Selection, Accept, Cancel). */
	FText LastMaskActionStatusText;

	// --- Live preview update (debounced automatic regeneration) ----------------------------

	/**
	 * Arms (or restarts) a short one-shot debounce timer via GEditor's FTimerManager -- not a
	 * per-frame Tick override -- so a burst of slider events (SSpinBox fires OnValueChanged
	 * continuously while dragging) collapses into a single regeneration ~150ms after the LAST
	 * event. Calling FTimerManager::SetTimer() again with the same FTimerHandle before it fires
	 * clears and re-adds it (confirmed in TimerManager.cpp), which is exactly "a new change resets
	 * the wait" -- the LAST value received before the timer fires is always what gets published, never
	 * an intermediate one. Uses FTimerDelegate::CreateSP (weak-safe: a pending timer harmlessly no-ops
	 * if this widget is destroyed first), but the timer is also explicitly cleared in
	 * OnWorldCleanup() and the destructor rather than relying on that alone. This is the tool's ONLY
	 * automatic-regeneration path -- there is no manual "Generate Mask" alternative; every parameter
	 * change either regenerates immediately (cheap/discrete changes) or arms this debounce (expensive/
	 * continuous changes), unconditionally.
	 */
	void ScheduleAutoUpdatePreview();

	/**
	 * Regenerates every enabled generator's mask for every eligible entry using the CURRENT
	 * parameters -- this IS the tool's single live-regeneration entry point (there is no separate
	 * manual "Generate Mask" action; every parameter-change callback in this panel funnels here,
	 * either immediately or via ScheduleAutoUpdatePreview's debounce). On a per-entry, per-generator
	 * basis: if the new mask comes back Ready, it replaces the entry's mask; otherwise
	 * (DegenerateBounds/Invalid/Unavailable, including "generator disabled") the entry's EXISTING mask
	 * is left untouched -- regeneration must never destroy a valid Preview on a transient failure.
	 * Never runs while Applying (guarded defensively; not reachable in practice since Accept is
	 * synchronous). Called by the debounce timer, immediately for discrete parameter changes
	 * (Enable/Invert/Mirror/World Space/raycast-affecting parameters), and once from RefreshSelection()
	 * when the newly selected entry/entries already have at least one generator enabled.
	 *
	 * bIncludeAO (AUDITED, BBox Invert exception -- default true, preserves all pre-existing call
	 * sites' behavior unchanged): when false, the Ambient Occlusion slot (AmbientOcclusionMask) is
	 * left COMPLETELY untouched for every entry -- not re-validated, not cleared, not re-snapshotted.
	 * Used exclusively by OnAxisInvertChanged (BBox per-axis Invert's immediate-regeneration
	 * exception), so that regenerating BoundingBoxMask immediately can never have any observable
	 * effect -- not even a harmless entry-level re-validation -- on Ambient Occlusion.
	 */
	void RunAutoUpdatePreview(bool bIncludeAO = true);

	FTimerHandle AutoUpdateDebounceTimerHandle;

	// --- Unified Bounds (global, all 3 axes, all selected meshes) --------------------------

	ECheckBoxState GetUnifiedBoundsState() const { return bUseUnifiedBounds ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }

	/**
	 * Toggling Unified Bounds never recomputes just one mesh: it cancels any pending debounce,
	 * invalidates every entry's current Bounding-Box-sourced mask (InvalidateBoundingBoxRawMask()), and
	 * unconditionally immediately regenerates every eligible entry as one coherent batch
	 * (RunAutoUpdatePreview(), which itself computes the collective domain once, if applicable, and
	 * reuses it for every participant).
	 */
	void OnUnifiedBoundsChanged(ECheckBoxState NewState);

	/**
	 * False (default) preserves the tool's previously-validated behavior exactly: each component
	 * normalizes its own render vertices against its OWN individual per-axis bounds (see
	 * VertexMaskForgePanel::GenerateBoundingBoxMask's internal bounds pass). True: every enabled
	 * axis is normalized against a COLLECTIVE domain -- the union of that axis's coordinate across
	 * every participating component's render vertices (see VertexMaskForgePanel::
	 * ComputeCollectiveAxisBounds) -- computed fresh before each batch (every live regeneration,
	 * Preview refresh, and Accept validation), never cached across calls,
	 * consistent with the rest of the panel's "always recompute, never stale" design. Global for all
	 * 3 axes and the whole selection -- there is no per-axis or per-mesh Unified Bounds mode.
	 */
	bool bUseUnifiedBounds = false;

	// --- Preview (Preview Mode + Channel Filter) --------------------------------------------

	TSharedRef<SWidget> OnGeneratePreviewModeRow(TSharedPtr<EVertexMaskForgePreviewMode> InOption) const;
	void OnPreviewModeSelectionChanged(TSharedPtr<EVertexMaskForgePreviewMode> NewSelection, ESelectInfo::Type SelectInfo);
	FText GetPreviewModeButtonText() const;

	ECheckBoxState GetChannelFilterRState() const { return bChannelFilterR ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }
	void OnChannelFilterRChanged(ECheckBoxState NewState);
	ECheckBoxState GetChannelFilterGState() const { return bChannelFilterG ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }
	void OnChannelFilterGChanged(ECheckBoxState NewState);
	ECheckBoxState GetChannelFilterBState() const { return bChannelFilterB ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }
	void OnChannelFilterBChanged(ECheckBoxState NewState);

	FText GetPreviewStatusText() const;

	/**
	 * Applies or restores preview visualization for every selected entry, based on the current
	 * CurrentPreviewMode / Channel Filter / each entry's BoundingBoxMask state. Idempotent and
	 * side-effect-free with respect to the mask itself -- never generates or invalidates it.
	 *
	 * bCommit (audited, Channel Filter toggle fix): forwarded unchanged to ApplyPreviewToEntry /
	 * UpdateWorkingColors -- true ONLY for an explicit Fill White/Black action (promotes the
	 * freshly-composed WorkingColors to CommittedColors); false for every other trigger (live
	 * regeneration, Channel Filter toggle, Preview Mode change, RefreshSelection, mask invalidation)
	 * so none of those can silently consolidate a transient edit. See
	 * FVertexMaskForgePreviewComponentState::CommittedColors' own doc comment for the full contract.
	 */
	void UpdateAllPreviews(bool bCommit);

	/**
	 * Applies or restores preview for one entry, per current mode/filter/mask state.
	 * CollectiveBoundsPtr is non-null only when bUseUnifiedBounds is on AND UpdateAllPreviews()
	 * successfully computed a collective domain for this batch -- passed through unchanged to every
	 * per-component GenerateBoundingBoxMask() re-evaluation (see the audit note there); nullptr means
	 * "use each component's own individual bounds" (bUseUnifiedBounds off, or this entry's mask
	 * Source isn't BoundingBox, in which case it's simply unused). bCommit: see UpdateAllPreviews'
	 * own doc comment; forwarded unchanged to UpdateWorkingColors.
	 */
	void ApplyPreviewToEntry(
		const TSharedPtr<FVertexMaskForgeSelectedMesh>& Entry,
		const TStaticArray<FVertexMaskForgeAxisBoundsResult, 3>* CollectiveBoundsPtr,
		bool bCommit);

	/** Restores original materials and vertex colors on every tracked component of one entry --
	 *  session-end variant (resets BaselineColors/CommittedColors/WorkingColors/AOCache too). Idempotent.
	 *  Only called from DestroyAllPreviews() (Cancel, Accept, RefreshSelection, World cleanup) -- a
	 *  genuine session end. See RestorePreviewForEntryVisualOnly for the mid-session/compositional case. */
	void RestorePreviewForEntry(FVertexMaskForgeSelectedMesh& Entry);

	/** AUDITED (raw/composition separation checkpoint): visual-only restore on every tracked component
	 *  of one entry -- BaselineColors/CommittedColors/WorkingColors/AOCache are left completely
	 *  untouched (see VertexMaskForgePanel::RestorePreviewVisualOnly). Used by ApplyPreviewToEntry's
	 *  own early-return paths (Preview Mode == Original Material, working mesh not Ready, Mesh/
	 *  RenderData/debug material failed to resolve) -- none of these are a session end or a genuine
	 *  geometric invalidation, so the AO geometry cache must survive them untouched. */
	void RestorePreviewForEntryVisualOnly(FVertexMaskForgeSelectedMesh& Entry);

	/**
	 * Restores every currently-tracked entry's preview components. Called before Refresh Selection
	 * replaces SelectedMeshes, and from the destructor, so no transient override ever outlives the
	 * entries or the panel.
	 */
	void DestroyAllPreviews();

	/** Lazily loads the engine's built-in vertex-color debug material (never a new persistent asset). */
	UMaterialInterface* GetPreviewDebugMaterial();

	// --- Pending Changes: Accept / Cancel ---------------------------------------------------

	EVertexMaskForgeOperationState GetOperationState() const { return OperationState; }

	/** Recomputes OperationState (Idle/PendingChanges only -- see enum doc) from current data: at
	 *  least one entry with an active PreviewComponent and a Ready mask, independent of the current
	 *  Preview Mode (Preview Mode only controls presentation, never persistence -- see
	 *  ApplyPreviewToEntry's bUseOriginalMaterials). Never touches LastOperationErrorText (callers
	 *  that need to report a failure set it themselves, AFTER calling anything that ends in this, so a
	 *  fresh message from this same pass is never clobbered). No-ops while Applying. */
	void RecomputeOperationState();

	bool CanAcceptChanges() const { return OperationState == EVertexMaskForgeOperationState::PendingChanges; }
	FReply OnAcceptChangesClicked();

	/**
	 * True if any currently selected entry's already-resolved Static Mesh has Nanite enabled
	 * (UStaticMesh::IsNaniteEnabled()). Cheap: reads Entry->Mesh.Get() only (never forces a
	 * synchronous load) -- by the time an entry exists in SelectedMeshes, RefreshSelection/
	 * BuildWorkingMeshes has already resolved it, so this is a pointer check, not an asset load.
	 */
	bool HasNaniteMeshInSelection() const;

	/** Visible only while HasNaniteMeshInSelection() is true -- shown next to the Accept row, explains
	 *  that Accept for a Nanite-enabled mesh writes to the Source Static Mesh asset and affects every
	 *  instance using it. */
	EVisibility GetNaniteNoticeVisibility() const { return HasNaniteMeshInSelection() ? EVisibility::Visible : EVisibility::Collapsed; }

	/** AUDITED (UX1): Cancel must be able to end ANY active session, not only one with a Ready mask to
	 *  discard -- e.g. a session started via "Edit Vertex Mask" where no generator was enabled yet
	 *  still has WorkingMeshes/baseline state that Cancel should be able to walk away from. Gated on
	 *  bIsEditingVertexMask (the session flag) rather than OperationState. */
	bool CanCancelChanges() const { return bIsEditingVertexMask; }
	FReply OnCancelChangesClicked();

	FText GetOperationStatusText() const;

	/**
	 * Validates every eligible entry, confirms the destination with the user, and -- only if both
	 * succeed -- writes permanently to the Static Mesh asset(s) inside one FScopedTransaction. See
	 * the .cpp for the full validate-then-write contract (VertexMaskForgePanel::BuildAcceptTargets /
	 * WriteAcceptTargets). Returns true only on a fully successful Accept.
	 */
	bool AcceptPendingChanges();

	/** Records the reason the last Accept (or auto-update regeneration) was blocked/failed, shown in
	 *  GetOperationStatusText(). Cleared explicitly at the START of each fresh attempt -- never by
	 *  RecomputeOperationState(), so it survives whatever UpdateAllPreviews() call follows within the
	 *  same attempt. */
	FText LastOperationErrorText;

	EVertexMaskForgeOperationState OperationState = EVertexMaskForgeOperationState::Idle;

	/**
	 * Bound to FWorldDelegates::OnWorldCleanup (registered once in Construct(), removed in the
	 * destructor via WorldCleanupDelegateHandle). Fires at the very start of UWorld::CleanupWorld()
	 * -- e.g. on level change/reload, or PIE end -- while World and its actors/components are still
	 * fully valid, and BEFORE UWorld::ClearWorldComponents() or any actor/component teardown. This is
	 * the correct point to synchronously restore/destroy every preview whose SourceComponent (or
	 * PreviewComponent, or hidden Actor) belongs to World, so this panel never keeps a
	 * TStrongObjectPtr (or an AActor::bHiddenEdTemporary override) referencing a World that is about
	 * to go away. Never calls RefreshSelection or recreates previews -- pure cleanup, safe to run
	 * during editor shutdown; does not assume GEditor is otherwise valid.
	 */
	void OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);

	FDelegateHandle WorldCleanupDelegateHandle;

	/** See OnEditorSelectionChanged's doc comment. Registered/removed the same way as, and for the
	 *  same reason as, WorldCleanupDelegateHandle above. */
	FDelegateHandle SelectionChangedDelegateHandle;

	/**
	 * AUDITED (V2-E corrective pass, transform freshness): GEngine->OnActorMoved() -- the engine's own
	 * official notification that an Actor's viewport gizmo move/rotate/scale has FINISHED (confirmed by
	 * reading AActor::PostEditMove(bFinished=true) in ActorEditor.cpp, which unconditionally calls
	 * GEngine->BroadcastOnActorMoved(this) -- the same call chain LevelEditorViewport.cpp's own
	 * TrackingStopped path uses when a gizmo drag completes). Fires ONCE per completed drag (never
	 * continuously mid-drag, so no extra debounce is needed beyond the existing live-regeneration
	 * mechanism), for ANY actor moved anywhere in the level -- OnActorMovedForDirectionalNormal filters
	 * to only the actors actually relevant to this panel's own tracked components. Registered/removed
	 * the same way as WorldCleanupDelegateHandle/SelectionChangedDelegateHandle above.
	 */
	void OnActorMovedForDirectionalNormal(AActor* Actor);

	FDelegateHandle ActorMovedDelegateHandle;

	/**
	 * Centralized per-Actor hide ref-counting for the whole panel (not per-entry), since components
	 * from different FVertexMaskForgeSelectedMesh entries can share the same owning Actor. See
	 * FVertexMaskForgeActorHideState.
	 */
	TMap<TWeakObjectPtr<AActor>, FVertexMaskForgeActorHideState> ActorHideStates;

	TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>> SelectedMeshes;

	// Preview Mode / Channel Filter are panel/session-transient: never saved on the asset, never
	// invalidate or regenerate BoundingBoxMask, and are fully independent of one another.
	EVertexMaskForgePreviewMode CurrentPreviewMode = EVertexMaskForgePreviewMode::OriginalMaterial;
	TArray<TSharedPtr<EVertexMaskForgePreviewMode>> PreviewModeOptions;
	TSharedPtr<SComboBox<TSharedPtr<EVertexMaskForgePreviewMode>>> PreviewModeComboBox;

	bool bChannelFilterR = true;
	bool bChannelFilterG = true;
	bool bChannelFilterB = true;

	/** Resolved lazily via GetPreviewDebugMaterial(); weak because it is an asset the plugin does not own. */
	TWeakObjectPtr<UMaterialInterface> PreviewDebugMaterial;
};
