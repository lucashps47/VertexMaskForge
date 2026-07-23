#pragma once

#include "Containers/BitArray.h"
#include "Containers/StaticArray.h"
#include "CoreMinimal.h"
#include "Delegates/IDelegateInstance.h"
#include "Engine/TimerHandle.h"
#include "Math/Vector4.h"
#include "MeshTypes.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtr.h"
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

	/** GenerateAmbientOcclusionMask: per-render-vertex hemisphere-raycast occlusion fraction.
	 *  Convention (before Invert): 0.0 = exposed (no occluders found), 1.0 = fully occluded/cavity --
	 *  i.e. the OPPOSITE of the common "black = occluded" texture-baking convention. FVertexMaskForgeAOParams::bInvert
	 *  swaps it. */
	AmbientOcclusion,
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

	/** Applied AFTER the raw occlusion fraction is computed (Result = 1 - AO) -- never requires
	 *  recomputing raw samples or rebuilding the tree; see GenerateAmbientOcclusionMask. */
	bool bInvert = false;
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
	 * Samples already resolved to either the full user-chosen value (explicit Generate Mask) or the
	 * interactive cap (Auto Update Preview) -- see OnGenerateBoundingBoxMaskClicked/
	 * RunAutoUpdatePreview). Values/bHasValue are ALWAYS left empty here -- this field is NEVER used to
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
};

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
	 * Accept as Instance Override (success), RefreshSelection (before rebuilding), and World cleanup
	 * -- so a brand new session always starts from a fresh capture, never a stale one from a
	 * concluded operation (whose baseline may since have changed, e.g. Accept just wrote new colors
	 * onto this exact component/asset).
	 */
	TArray<FColor> BaselineColors;

	/**
	 * The last result EXPLICITLY CONSOLIDATED for this component, in render-vertex order
	 * (LOD0-sized). Seeded from BaselineColors at the same moment BaselineColors itself is captured
	 * (see that field's doc comment), then only ever overwritten by
	 * VertexMaskForgePanel::UpdateWorkingColors when called with bCommit == true -- exclusively an
	 * explicit Generate Mask click or a Fill White/Black action (see UpdateAllPreviews' own doc
	 * comment for the exhaustive list of which triggers commit and which do not).
	 *
	 * AUDITED (Channel Filter toggle fix): this is what WorkingColors is rebuilt FROM on every single
	 * recomposition (see WorkingColors' own doc comment) -- so a channel that is toggled OFF in the
	 * Channel Filter before ever being consolidated correctly reverts to whatever this array already
	 * holds for it (BaselineColors, if never consolidated; or an earlier Generate Mask/Fill's result,
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
	 * PreviewComponent's OverrideVertexColors is set from (ApplyPreviewToEntry), and what Accept /
	 * Accept as Instance Override read to build their FinalColors -- "the preview currently shown".
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
	 * Preview (ApplyPreviewToEntry), Accept (BuildAcceptTargets), and Accept as Instance Override
	 * (BuildInstanceOverrideTargets) all read this SAME array -- Accept and Accept as Instance
	 * Override themselves NEVER call UpdateWorkingColors; they only READ it as-is, so Accept always
	 * persists exactly the last effectively-generated/shown preview, never a silently-recomposed
	 * approximation of pending, ungenerated UI parameters.
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
	 * render-vertex/OverrideVertexColors preview path would silently show nothing again, and Accept as
	 * Instance Override would silently produce no visible effect, the exact bug this whole feature
	 * fixes. So EVERY Nanite-enabled mesh uses Source-Topology mode, unconditionally, for both
	 * behaviors: Bounding Box/Ambient Occlusion are generated against WorkingMesh.Mesh (the SOURCE-
	 * topology FDynamicMesh3) via GenerateBoundingBoxMaskFromDynamicMesh /
	 * GenerateAmbientOcclusionMaskFromDynamicMesh -- the same domain UE's own Paint Vertex Colors tool
	 * uses, proven correct for Nanite by the native-tool audit -- and Accept as Instance Override is
	 * never valid (see HasNaniteMeshInSelection). Render-Vertex mode is reserved for non-Nanite meshes
	 * only in this phase.
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

class SVertexMaskForgePanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVertexMaskForgePanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Declared here and defined in the .cpp so FDynamicMesh3 only needs to be complete there. */
	virtual ~SVertexMaskForgePanel() override;

private:
	/**
	 * Bound to USelection::SelectionChangedEvent (registered once in Construct(), removed in the
	 * destructor via SelectionChangedDelegateHandle) -- the sole automatic trigger for
	 * RefreshSelection() now that the manual "Refresh Selection" button no longer exists. Fires for
	 * Actor/Component/BSP scene selection changes only; Content Browser asset selection is a
	 * completely separate system and was never routed through this delegate, matching this panel no
	 * longer consulting the Content Browser at all (see CollectViewportSelection, the only collector
	 * left). NewSelection (which USelection instance changed) is unused.
	 *
	 * Deferred-sync contract (replaces the old manual Refresh Selection button's YesNoCancel prompt):
	 * while OperationState != Idle, an operation is pending against ORIGINAL targets that must never
	 * be silently retargeted or discarded just because the scene selection changed underneath it --
	 * so this does NOT call RefreshSelection() in that case. It also does NOT destroy previews, does
	 * NOT touch SelectedMeshes, and does NOT create any transaction or mark anything dirty; it only
	 * records that a sync is owed, via bSceneSelectionChangedDuringActiveOperation, so that the panel
	 * can catch up automatically -- without requiring another viewport/World Outliner click -- the
	 * moment the pending operation is actually resolved (see SyncSelectionIfChangedDuringOperation).
	 */
	void OnEditorSelectionChanged(UObject* NewSelection);

	/** Re-queries the scene (viewport/World Outliner) selection and rebuilds the working set. */
	void RefreshSelection();

	/**
	 * Set true by OnEditorSelectionChanged() when the scene selection changes while OperationState !=
	 * Idle (i.e. while an operation is pending against targets captured before that change).
	 * Consumed -- and cleared -- by SyncSelectionIfChangedDuringOperation(), called at the tail of
	 * exactly the three actions that can legitimately conclude a session and return OperationState to
	 * Idle: OnCancelChangesClicked(), AcceptPendingChanges() (on success), and
	 * AcceptPendingChangesAsInstanceOverride() (on success). Deliberately NOT consumed by any other
	 * RecomputeOperationState() call site (e.g. ordinary mask regeneration flipping PendingChanges<->
	 * Idle), so an incidental state flip unrelated to actually concluding a session never triggers an
	 * unwanted resync/rebuild.
	 */
	bool bSceneSelectionChangedDuringActiveOperation = false;

	/**
	 * If the scene selection changed while the operation that just concluded was pending (see
	 * bSceneSelectionChangedDuringActiveOperation), re-syncs SelectedMeshes with the CURRENT scene
	 * selection via RefreshSelection() and clears the flag -- no extra viewport/World Outliner click
	 * required. No-ops (no rebuild, no flicker, no clobbered status text) if the selection never
	 * changed during the just-concluded operation.
	 *
	 * MUST only be called AFTER the operation's own targets have been fully validated/written (Accept
	 * / Accept as Instance Override) or fully discarded (Cancel) against the ORIGINAL SelectedMeshes,
	 * and after OperationState has already settled back to Idle -- calling this any earlier would let
	 * a resync silently retarget or interrupt an in-flight operation, which must never happen.
	 */
	void SyncSelectionIfChangedDuringOperation();

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
	 * that exception): invalidates the current mask, then, if Auto Update Preview is on, cancels any
	 * pending debounce (a stale continuous-slider callback must never apply after a discrete change)
	 * and regenerates immediately.
	 */
	void OnAxisParamChangedDiscrete();

	/**
	 * AUDITED (BBox Invert exception, follow-up audit): per-axis Invert's OWN handler, deliberately
	 * NOT OnAxisParamChangedDiscrete. Each axis's Invert flag is baked into GenerateBoundingBoxMask's
	 * per-axis gradient BEFORE the max-combination across axes -- three independent per-axis flags
	 * cannot be correctly represented as a single post-hoc, composition-time invert (see
	 * ComposeMaskStack), so Option A (preserve raw un-inverted values, invert during composition) was
	 * judged disproportionate for this axis-based design; Option B is implemented instead: Invert
	 * ALWAYS regenerates BoundingBoxMask immediately, unconditionally bypassing the normal
	 * Auto-Update-gated "wait for Generate Mask" contract every other Bounding Box raw parameter still
	 * follows (regeneration itself is cheap here -- unlike Ambient Occlusion, Bounding Box has no
	 * persistent geometry cache to needlessly rebuild). Calls RunAutoUpdatePreview with bIncludeAO
	 * false so Ambient Occlusion's own slot/AOCache are never touched, not even a harmless re-validation.
	 */
	void OnAxisInvertChanged(int32 AxisIndex, ECheckBoxState NewState);

	/** Processes every selected entry's working mesh, generating or clearing its Bounding Box Mask. */
	FReply OnGenerateBoundingBoxMaskClicked();

	/**
	 * AUDITED (raw/composition separation checkpoint): resets ONLY every selected entry's
	 * BoundingBoxMask slot back to NotGenerated (AmbientOcclusionMask is completely untouched), without
	 * touching the working mesh (FDynamicMesh3) itself. Called whenever a Bounding Box RAW/geometric
	 * parameter changes (axis Position/Falloff/Invert/Mirror/World Space/Enable, Unified Bounds) --
	 * NEVER for a purely compositional change (Blend Mode, Opacity -- see RecomposeWorkingColors
	 * instead). Calls UpdateAllPreviews(false) synchronously ONLY when Auto Update Preview is off (see
	 * the .cpp definition for why); the user must click Generate Mask again in that case, or
	 * ScheduleAutoUpdatePreview()/RunAutoUpdatePreview() take over automatically otherwise. Never
	 * touches a Constant Fill mask's meaning -- Fill results are independent of these axis parameters.
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
	 * Recomposes immediately and correctly with ZERO raycasts and ZERO Tree rebuilds, identically
	 * whether Auto Update Preview is on or off.
	 */
	void RecomposeWorkingColors();

	/** Panel-level parameters for each of the 3 axes, indexed by EVertexMaskForgeBoundsAxis. Shared
	 *  across every selected entry; per-instance World Space evaluation reads a component's own
	 *  transform separately (see GenerateBoundingBoxMask) -- these parameters themselves never vary
	 *  per entry or per component. Z starts bEnabled == true (matching the previously-validated
	 *  single-axis default); X and Y start disabled, so a fresh panel reproduces the exact prior
	 *  Local-Z-only behavior until the user explicitly enables another axis. */
	TStaticArray<FVertexMaskForgeAxisMaskParams, 3> BoundingBoxAxisParams;

	// --- Bounding Box Mask: Blend Mode + Opacity --------------------------------------------

	TSharedRef<SWidget> OnGenerateBlendModeRow(TSharedPtr<EVertexMaskForgeBlendMode> InOption) const;

	/**
	 * Treated exactly like a discrete axis parameter change (see OnAxisParamChangedDiscrete):
	 * invalidates the current mask/preview, then, if Auto Update Preview is on, regenerates
	 * immediately. Deliberately NOT treated like Channel Filter/Preview Mode (which always recompose
	 * immediately via UpdateAllPreviews() regardless of Auto Update Preview) -- Blend Mode is a
	 * generation-adjacent parameter whose change must wait for Generate Mask when Auto Update is off,
	 * per this checkpoint's explicit requirement.
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
	 *  (reuses it verbatim, zero raycasts, works immediately even with Auto Update Preview off) --
	 *  only entries WITHOUT a valid derived mask trigger real (re)generation, gated by Auto Update
	 *  Preview like any other raw parameter. Enabling, by itself, is never treated as a reason to
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

	/** Same treatment as OnBlendModeSelectionChanged (Bounding Box's own Blend Mode combo) -- a
	 *  discrete, generation-adjacent change; waits for Generate Mask when Auto Update Preview is off. */
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
	int32 AOSamples = 64;

	/** Unreal units (World Space); clamped again in GenerateAmbientOcclusionMask to (0, 10000]. */
	float AOMaxDistance = 100.0f;

	/** Unreal units (World Space); clamped again in GenerateAmbientOcclusionMask to [0.001, 10.0]. */
	float AOBias = 0.1f;

	// --- Fill White / Fill Black utility masks ----------------------------------------------

	FReply OnFillWhiteClicked();
	FReply OnFillBlackClicked();

	/**
	 * Shared implementation for both Fill buttons: cancels any pending Auto Update Preview debounce
	 * first (a Fill must never be overwritten moments later by a stale regeneration), then, for every
	 * SelectedMeshes entry that passes the SAME entry-level validity gating as Generate Mask
	 * (WorkingMesh Ready, resolvable Static Mesh, valid LOD 0 render data), generates a dense
	 * constant-valued mask (VertexMaskForgePanel::GenerateConstantMask) and assigns it to that
	 * entry's mask. Unlike a manual Generate Mask click, an entry that fails validation here is left
	 * COMPLETELY UNTOUCHED (its previous mask, if any, is preserved) rather than reset to
	 * Unavailable -- per the explicit "preserve the last valid Preview on failure" requirement.
	 * Ends with UpdateAllPreviews(), which recomposes/reapplies the transient Preview (reusing the
	 * exact same ApplyPreviewToEntry/UpdateWorkingColors path as every other mask) and marks Pending
	 * Changes via RecomputeOperationState().
	 */
	void RunConstantFill(float ConstantValue, EVertexMaskForgeScalarMaskSource Source, const FText& SuccessMessage);

	/** Enabled only when at least one selected entry has a Ready working mesh, and not while Applying. */
	bool CanRunFill() const;

	FText GetMaskActionStatusText() const { return LastMaskActionStatusText; }

	/** Success/partial-failure message for the last Generate Mask / Fill action; cleared by the next
	 *  mask-changing action (Generate Mask, Fill, parameter change, Refresh Selection, Accept, Cancel). */
	FText LastMaskActionStatusText;

	// --- Auto Update Preview (debounced automatic regeneration) ----------------------------

	ECheckBoxState GetAutoUpdatePreviewState() const { return bAutoUpdatePreview ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }
	void OnAutoUpdatePreviewChanged(ECheckBoxState NewState);

	/**
	 * Arms (or restarts) a short one-shot debounce timer via GEditor's FTimerManager -- not a
	 * per-frame Tick override -- so a burst of slider events (SSpinBox fires OnValueChanged
	 * continuously while dragging) collapses into a single regeneration ~150ms after the LAST
	 * event. Calling FTimerManager::SetTimer() again with the same FTimerHandle before it fires
	 * clears and re-adds it (confirmed in TimerManager.cpp), which is exactly "a new change resets
	 * the wait". No-ops if Auto Update Preview is off. Uses FTimerDelegate::CreateSP (weak-safe: a
	 * pending timer harmlessly no-ops if this widget is destroyed first), but the timer is also
	 * explicitly cleared in OnWorldCleanup() and the destructor rather than relying on that alone.
	 */
	void ScheduleAutoUpdatePreview();

	/**
	 * Regenerates the Bounding Box Mask for every eligible entry using the CURRENT per-axis
	 * parameters. On a per-entry basis: if the new mask comes back Ready, it replaces the entry's
	 * mask; otherwise (DegenerateBounds/Invalid/Unavailable, including "no axis enabled") the
	 * entry's EXISTING mask is left untouched -- an auto-triggered regeneration must never destroy a
	 * valid Preview, unlike a manual Generate Mask click (OnGenerateBoundingBoxMaskClicked), which
	 * still unconditionally overwrites, per the already-validated checkpoint behavior. Never runs
	 * while Applying (guarded defensively; not reachable in practice since Accept is synchronous).
	 * Called by the debounce timer, or immediately for discrete parameter changes
	 * (Enable/Invert/Mirror/World Space).
	 *
	 * bIncludeAO (AUDITED, BBox Invert exception -- default true, preserves all pre-existing call
	 * sites' behavior unchanged): when false, the Ambient Occlusion slot (AmbientOcclusionMask) is
	 * left COMPLETELY untouched for every entry -- not re-validated, not cleared, not re-snapshotted.
	 * Used exclusively by OnAxisInvertChanged (BBox per-axis Invert's immediate-regeneration
	 * exception), so that regenerating BoundingBoxMask immediately (bypassing the normal Auto-Update-
	 * gated wait) can never have any observable effect -- not even a harmless entry-level
	 * re-validation -- on Ambient Occlusion.
	 */
	void RunAutoUpdatePreview(bool bIncludeAO = true);

	bool bAutoUpdatePreview = true;
	FTimerHandle AutoUpdateDebounceTimerHandle;

	// --- Unified Bounds (global, all 3 axes, all selected meshes) --------------------------

	ECheckBoxState GetUnifiedBoundsState() const { return bUseUnifiedBounds ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; }

	/**
	 * Toggling Unified Bounds never recomputes just one mesh: it cancels any pending debounce,
	 * invalidates every entry's current Bounding-Box-sourced mask (InvalidateBoundingBoxRawMask()), and
	 * -- only if Auto Update Preview is on -- immediately regenerates every eligible entry as one
	 * coherent batch (RunAutoUpdatePreview(), which itself computes the collective domain once, if
	 * applicable, and reuses it for every participant). If Auto Update Preview is off, parameters are
	 * updated and results invalidated, but no new Preview is generated until the user clicks Generate
	 * Mask -- same contract as every other axis parameter.
	 */
	void OnUnifiedBoundsChanged(ECheckBoxState NewState);

	/**
	 * False (default) preserves the tool's previously-validated behavior exactly: each component
	 * normalizes its own render vertices against its OWN individual per-axis bounds (see
	 * VertexMaskForgePanel::GenerateBoundingBoxMask's internal bounds pass). True: every enabled
	 * axis is normalized against a COLLECTIVE domain -- the union of that axis's coordinate across
	 * every participating component's render vertices (see VertexMaskForgePanel::
	 * ComputeCollectiveAxisBounds) -- computed fresh before each batch (Generate Mask click, Auto
	 * Update regeneration, every Preview refresh, and Accept validation), never cached across calls,
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
	 * UpdateWorkingColors -- true ONLY for an explicit Generate Mask click or a Fill White/Black
	 * action (both promote the freshly-composed WorkingColors to CommittedColors); false for every
	 * other trigger (Auto Update Preview, Channel Filter toggle, Preview Mode change, RefreshSelection,
	 * mask invalidation) so none of those can silently consolidate a transient edit. See
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
	 *  Only called from DestroyAllPreviews() (Cancel, Accept, Accept as Instance Override, RefreshSelection,
	 *  World cleanup) -- a genuine session end. See RestorePreviewForEntryVisualOnly for the
	 *  mid-session/compositional case. */
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

	/** Recomputes OperationState (Idle/PendingChanges only -- see enum doc) from current data: a
	 *  Preview Mode other than OriginalMaterial, and at least one entry with an active
	 *  PreviewComponent and a Ready mask. Never touches LastOperationErrorText (callers that need to
	 *  report a failure set it themselves, AFTER calling anything that ends in this, so a fresh
	 *  message from this same pass is never clobbered). No-ops while Applying. */
	void RecomputeOperationState();

	bool CanAcceptChanges() const { return OperationState == EVertexMaskForgeOperationState::PendingChanges; }
	FReply OnAcceptChangesClicked();

	/**
	 * Alternative conclusion to the SAME PendingChanges session Accept would otherwise conclude:
	 * writes the current Preview result as permanent per-instance OverrideVertexColors on each
	 * selected UStaticMeshComponent, and NEVER touches the Source Static Mesh asset (no
	 * Mesh->Modify(), GetMeshDescription(), CommitMeshDescription(), Build(), or
	 * MarkPackageDirty() on the Static Mesh -- see VertexMaskForgePanel::WriteInstanceOverrideTargets
	 * in the .cpp for the audited, engine-sourced justification). Shares Accept's PendingChanges gate
	 * -- both are valid, mutually exclusive ways to conclude the same session; this one never calls
	 * into AcceptPendingChanges() or vice versa.
	 *
	 * AUDITED (Nanite): additionally requires !HasNaniteMeshInSelection() -- Nanite's runtime renderer
	 * never reads FStaticMeshComponentLODInfo::OverrideVertexColors (confirmed against the UE 5.8
	 * Mesh Vertex Paint Tool / Nanite rendering source), so writing an instance override onto a
	 * Nanite-enabled component would silently produce no visible result. Accept (writing to the
	 * Source Static Mesh) is the only route that can affect a Nanite mesh's rendering.
	 */
	bool CanAcceptAsInstanceOverride() const { return OperationState == EVertexMaskForgeOperationState::PendingChanges && !HasNaniteMeshInSelection(); }
	FReply OnAcceptAsInstanceOverrideClicked();

	/**
	 * True if any currently selected entry's already-resolved Static Mesh has Nanite enabled
	 * (UStaticMesh::IsNaniteEnabled()). Cheap: reads Entry->Mesh.Get() only (never forces a
	 * synchronous load) -- by the time an entry exists in SelectedMeshes, RefreshSelection/
	 * BuildWorkingMeshes has already resolved it, so this is a pointer check, not an asset load.
	 */
	bool HasNaniteMeshInSelection() const;

	/** Explains why the button is disabled when HasNaniteMeshInSelection() is true; the normal
	 *  tooltip otherwise. */
	FText GetAcceptAsInstanceOverrideTooltip() const;

	/** Visible only while HasNaniteMeshInSelection() is true -- shown next to the Accept row. */
	EVisibility GetNaniteNoticeVisibility() const { return HasNaniteMeshInSelection() ? EVisibility::Visible : EVisibility::Collapsed; }

	bool CanCancelChanges() const { return OperationState == EVertexMaskForgeOperationState::PendingChanges || OperationState == EVertexMaskForgeOperationState::Failed; }
	FReply OnCancelChangesClicked();

	FText GetOperationStatusText() const;

	/**
	 * Validates every eligible entry, confirms the destination with the user, and -- only if both
	 * succeed -- writes permanently to the Static Mesh asset(s) inside one FScopedTransaction. See
	 * the .cpp for the full validate-then-write contract (VertexMaskForgePanel::BuildAcceptTargets /
	 * WriteAcceptTargets). Returns true only on a fully successful Accept.
	 */
	bool AcceptPendingChanges();

	/**
	 * Validates every eligible component, confirms the (non-destructive, asset-safe) destination with
	 * the user, and -- only if both succeed -- writes permanent OverrideVertexColors directly onto
	 * each selected UStaticMeshComponent inside one FScopedTransaction (see
	 * VertexMaskForgePanel::BuildInstanceOverrideTargets / WriteInstanceOverrideTargets). Unlike
	 * AcceptPendingChanges(), never deduplicates by UStaticMesh -- two components sharing one asset
	 * can end up with different FinalColors and are written independently. Returns true only on a
	 * fully successful write.
	 */
	bool AcceptPendingChangesAsInstanceOverride();

	/**
	 * Independent of Accept / Accept as Instance Override -- never requires, reads, or clears
	 * PendingChanges. Removes the LOD0 Instance Vertex Color override (via
	 * VertexMaskForgePanel::HasRemovableLOD0Override / BuildRemoveInstanceOverrideTargets, see the
	 * .cpp) from every SELECTED component that currently has one, restoring that component's own
	 * appearance to whatever Vertex Colors are stored in its Source Static Mesh. Enabled only while
	 * OperationState == Idle (an unresolved PendingChanges OR Failed preview decision must be
	 * concluded via Accept/Accept as Instance Override/Cancel first -- this deliberately never
	 * discards a pending Preview itself) AND at least one selected component currently has a
	 * removable override -- both re-evaluated live on every call (same as CanAcceptChanges /
	 * CanCancelChanges already are), so Undo/Redo and Refresh Selection are reflected on the very
	 * next Slate tick with no extra notification/delegate registration needed.
	 */
	bool CanRemoveInstanceOverride() const;
	FReply OnRemoveInstanceOverrideClicked();

	/**
	 * Validates every selected component with a removable override, confirms the (non-destructive,
	 * asset-safe) removal with the user, and -- only if both succeed -- removes each one's LOD0
	 * OverrideVertexColors via UStaticMeshComponent::RemoveInstanceVertexColorsFromLOD(0) inside one
	 * FScopedTransaction (see VertexMaskForgePanel::BuildRemoveInstanceOverrideTargets /
	 * RemoveInstanceOverrideTargets). Never touches any Static Mesh asset. Never deduplicates by
	 * UStaticMesh -- every selected component with an override is its own independent target.
	 */
	bool RemoveInstanceOverrides();

	/** Records the reason the last Accept (or auto-update regeneration) was blocked/failed, shown in
	 *  GetOperationStatusText(). Cleared explicitly at the START of each fresh attempt -- never by
	 *  RecomputeOperationState(), so it survives whatever UpdateAllPreviews() call follows within the
	 *  same attempt. */
	FText LastOperationErrorText;

	/** Success message for the last successful "Accept as Instance Override", shown by
	 *  GetOperationStatusText() while Idle (native Accept has no equivalent persistent success text
	 *  today -- its Idle state already reads as "No pending changes." either way). Cleared at the
	 *  start of every fresh Accept / Accept as Instance Override / Remove Instance Override / Cancel
	 *  attempt. Mutually exclusive with LastRemoveOverrideStatusText -- whichever action ran most
	 *  recently clears the other, so GetOperationStatusText() never shows a stale message from the
	 *  other action. */
	FText LastInstanceOverrideStatusText;

	/** Success message for the last successful "Remove Instance Override", shown by
	 *  GetOperationStatusText() while Idle. Cleared at the start of every fresh Accept / Accept as
	 *  Instance Override / Remove Instance Override / Cancel attempt -- see
	 *  LastInstanceOverrideStatusText's doc for the mutual-exclusion contract with this field. */
	FText LastRemoveOverrideStatusText;

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
