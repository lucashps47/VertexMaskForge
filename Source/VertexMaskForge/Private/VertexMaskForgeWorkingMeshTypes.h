#pragma once

#include "Containers/BitArray.h"
#include "Containers/StaticArray.h"
#include "CoreMinimal.h"
#include "Math/Color.h"
#include "MeshTypes.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtr.h"
#include "VertexMaskForgeInstanceResultStore.h"
#include "VertexMaskForgeMaskTypes.h"
#include "VertexMaskForgeWorkingColorsProvenance.h"

class AActor;
class UStaticMesh;
class UStaticMeshComponent;
class FVertexMaskForgeWorkingMeshOwner;
class FVertexMaskForgeWorkingStateOwner;

// AUDITED (M16-J.0): full include, not a forward declaration -- FVertexMaskForgePreviewComponentState's
// own inline `= default` constructor instantiates TStrongObjectPtr<UDynamicMeshComponent>
// (SourceTopologyPreviewComponent below), which requires the complete type at that point regardless of
// Unity Build blob grouping. Previously relied on SVertexMaskForgePanel.cpp's own include of this header
// happening to share a Unity blob with the first instantiation -- a latent fragility exposed (not
// introduced) by this checkpoint's own new translation units shifting Unity blob boundaries.
#include "Components/DynamicMeshComponent.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"

struct FMeshDescription;
struct FStaticMeshLODResources;

// AUDITED (M6, Extract Shared Working-Mesh Domain Types): shared across SVertexMaskForgePanel.cpp,
// VertexMaskForgeAcceptTargetBuilder.cpp, and VertexMaskForgeAcceptWriter.cpp -- a per-TU
// DEFINE_LOG_CATEGORY_STATIC would collide under Unity Build (all three .cpp files can be textually
// merged into one translation unit), so this category is declared EXTERN here and defined exactly
// once, in SVertexMaskForgePanel.cpp (DEFINE_LOG_CATEGORY). Same name, same default/compile-time
// verbosity as before -- only the declaration's home moved.
DECLARE_LOG_CATEGORY_EXTERN(LogVertexMaskForge, Log, All);

/**
 * Opaque per-component Ambient Occlusion cache (world-space-baked occluder geometry + spatial index
 * + last-computed raw per-render-vertex AO values), owned exclusively by one
 * FVertexMaskForgePreviewComponentState.
 *
 * AUDITED (M6, Extract Shared Working-Mesh Domain Types): fully defined here (not forward-declared/
 * defined-in-.cpp as originally, pre-M6) because both SVertexMaskForgePanel.cpp (which populates its
 * fields directly) and VertexMaskForgeWorkingMeshTypes.cpp (whose freshness-comparison helpers read
 * its fields) are now separate translation units that each need the complete type -- see
 * VertexMaskForgePanel::GenerateAmbientOcclusionMask for the cache's actual contents/contract.
 *
 * Two independently-invalidated layers, exactly matching the checkpoint's cache-invalidation
 * contract (see that function's own doc comment for the full matrix of what does/doesn't invalidate
 * each layer):
 *   - WorldMesh/Tree: the occluder geometry and its spatial index, baked in WORLD SPACE for this
 *     specific component's transform (see GenerateAmbientOcclusionMask for why World Space, not
 *     Local, is required for correctness under non-uniform scale). Rebuilt whenever ANY of
 *     CachedMesh/CachedDerivedDataKey/CachedNumRenderVerts/CachedNumIndices/CachedTransform no
 *     longer match -- see GenerateAmbientOcclusionMask's own doc comment (AUDITED, geometry cache
 *     key fix) for exactly what each field catches and why.
 *   - RawValues: the raw (pre-Invert) occlusion fraction per render vertex, in [0, 1]. Rebuilt only
 *     when Samples/MaxDistance/Bias actually change, OR when WorldMesh/Tree above was just rebuilt
 *     (new geometry/transform invalidates any previously-computed samples too).
 */
struct FVertexMaskForgeAOCache
{
	TUniquePtr<UE::Geometry::FDynamicMesh3> WorldMesh;
	TUniquePtr<UE::Geometry::FDynamicMeshAABBTree3> Tree;
	bool bTreeValid = false;

	/** Geometric identity WorldMesh/Tree were last built from -- ALL fields below must match the
	 *  CURRENT inputs for a cache hit; see GenerateAmbientOcclusionMask for the exact comparison.
	 *  AUDITED (DerivedDataKey false-mismatch fix): CachedDerivedDataKey is compared by PLAIN equality
	 *  -- "both empty" IS a match (an asset whose LOD never populates this field is not, by itself,
	 *  evidence of a change), never an automatic veto; Mesh/NumRenderVerts/NumIndices/Transform remain
	 *  independent safeguards in the same AND-chain regardless of this field's availability. */
	TWeakObjectPtr<const UStaticMesh> CachedMesh;
	FString CachedDerivedDataKey;
	int32 CachedNumRenderVerts = 0;
	int32 CachedNumIndices = 0;
	FTransform CachedTransform = FTransform::Identity;

	TArray<float> RawValues;
	int32 CachedSamples = 0;
	float CachedMaxDistance = 0.f;
	float CachedBias = 0.f;
	bool bValuesValid = false;
};

/**
 * AUDITED (Nanite source-topology support): sibling of FVertexMaskForgeAOCache, used ONLY for entries
 * in Source-Topology mode (every Nanite-enabled Static Mesh -- see
 * FVertexMaskForgeSelectedMesh::bUseSourceTopology). Same two-layer contract (WorldMesh/Tree geometric
 * cache + RawValues per-parameter cache) as FVertexMaskForgeAOCache, but keyed against WorkingMesh.Mesh
 * (the FDynamicMesh3 built from the SOURCE MeshDescription) instead of FStaticMeshLODResources.
 *
 * AUDITED (cache-key robustness fix): identity is CachedSourceMesh (the FDynamicMesh3 pointer, address
 * only, never dereferenced when stale) AND CachedGeometryFingerprint (see
 * VertexMaskForgePanel::ComputeDynamicMeshGeometryFingerprint) -- vertex/triangle COUNTS alone are
 * insufficient (two genuinely different geometries, or the same geometry with only its normals
 * changed, can share identical counts), so the fingerprint (hash of every vertex position AND every
 * Normal Overlay element) is what actually proves the geometry AND the normals used by the AO rays
 * are unchanged. Never used for, and never shared with, an entry in the render-vertex domain.
 */
struct FVertexMaskForgeSourceTopologyAOCache
{
	TUniquePtr<UE::Geometry::FDynamicMesh3> WorldMesh;
	TUniquePtr<UE::Geometry::FDynamicMeshAABBTree3> Tree;
	bool bTreeValid = false;

	/** Compared by address only -- WorkingMesh.Mesh is rebuilt (new object) on every Refresh Selection,
	 *  and this cache is always destroyed together with the session (see
	 *  FVertexMaskForgePreviewComponentState::SourceTopologyAOCache) before a new WorkingMesh could
	 *  ever be built while this pointer is still alive, so a stale address is never dereferenced. */
	const UE::Geometry::FDynamicMesh3* CachedSourceMesh = nullptr;
	uint32 CachedGeometryFingerprint = 0;
	FTransform CachedTransform = FTransform::Identity;

	/** Indexed by Normal Overlay Element ID -- see GenerateAmbientOcclusionMaskFromDynamicMesh's own
	 *  doc comment for why (hard-edge preservation: two corners at the same vertex position but with
	 *  different normals must never share a raw AO value). Sized to the overlay's MaxElementID(),
	 *  sparse-safe (an index that was never IsElement() is simply never written or read). */
	TArray<float> RawValues;
	int32 CachedSamples = 0;
	float CachedMaxDistance = 0.f;
	float CachedBias = 0.f;
	bool bValuesValid = false;
};

/**
 * Non-Nanite Thickness cache (V2-G) -- Asset Local Space, NEVER keyed by ComponentTransform (unlike
 * FVertexMaskForgeAOCache). Owned by FVertexMaskForgeWorkingMesh (per-entry/per-asset, not
 * per-component), since Thickness never depends on instance transform.
 *
 * AUDITED (M6, Extract Shared Working-Mesh Domain Types): fully defined here for the same
 * two-translation-unit reason as FVertexMaskForgeAOCache above (see that struct's doc comment).
 *
 * Two cached layers, same shape as AOCache's own contract:
 *   - LocalMesh/Tree: a private copy of LOD0's render-vertex geometry, built ONCE from local-space
 *     positions (no transform), with a NormalOverlay carrying tangent-Z per render vertex (1:1, never
 *     shared -- see GenerateThicknessMask's own NORMAL OVERLAY doc note) and with geometrically
 *     degenerate triangles excluded before AppendTriangle. Rebuilt whenever Mesh identity/
 *     DerivedDataKey/counts no longer match.
 *   - RawDistances: MeasuredThickness (already Bias-reconstructed) per render vertex, or invalid if no
 *     qualifying hit was found. Rebuilt only when SearchDistance/Bias change, or Layer 1 was rebuilt.
 * Min/Max Thickness/Blur/Invert are recomputed fresh every call directly from RawDistances -- same
 * "cheap enough to just recompute" precedent as AO's own Levels/Invert and Directional Normal's own
 * Blur/Invert (neither of which cache their own post-processing stage either).
 *
 * FRESHNESS SNAPSHOT: SnapshotPositions/SnapshotTangentZ (by RenderVertexIndex) and SnapshotTriangles
 * (by TriangleOrdinal, storing render-vertex-index triples -- never Dynamic-Mesh-internal IDs) are
 * captured alongside LocalMesh, compared SEMANTICALLY (value-by-value, keyed by source-content-derived
 * indices, never by Dynamic-Mesh-VertexID/NormalElementID) against freshly-read LOD0 data immediately
 * before Accept's first Modify() -- see AreThicknessGeometrySnapshotsExactlyEquivalent.
 * CachedGeometryFingerprint is a uint32 FAST-REJECT ONLY (see ComputeDynamicMeshGeometryFingerprint) --
 * a match NEVER by itself proves freshness; the full snapshot comparison is always still required.
 */
struct FVertexMaskForgeThicknessCache
{
	TUniquePtr<UE::Geometry::FDynamicMesh3> LocalMesh;
	TUniquePtr<UE::Geometry::FDynamicMeshAABBTree3> Tree;
	bool bTreeValid = false;

	TWeakObjectPtr<const UStaticMesh> CachedMesh;
	FString CachedDerivedDataKey;
	int32 CachedNumRenderVerts = 0;
	int32 CachedNumIndices = 0;

	TArray<FVector3f> SnapshotPositions;
	TArray<FVector3f> SnapshotTangentZ;
	TArray<FIntVector> SnapshotTriangles;
	uint32 CachedGeometryFingerprint = 0;

	TArray<float> RawDistances;
	TBitArray<> bRawValid;
	float CachedSearchDistance = 0.f;
	float CachedBias = 0.f;
	bool bValuesValid = false;

	int32 NumInvalidOriginNormal = 0;
	int32 NumDegenerateTrianglesDiscarded = 0;
	int32 NumIncidentOnlyRejections = 0;
	int32 NumOrientationRejections = 0;
	int32 NumNoHit = 0;
};

/**
 * Source-Topology sibling of FVertexMaskForgeThicknessCache. Same two-layer contract, but LocalMesh is
 * built from WorkingMesh.Mesh (the FDynamicMesh3 already in Asset Local Space -- see
 * GenerateThicknessMaskFromDynamicMesh's own doc comment for why a private copy, not WorkingMesh.Mesh
 * itself, is still built: degenerate-triangle exclusion must never mutate/filter the SHARED
 * WorkingMesh.Mesh that every other generator also depends on). CORNER-EXACT domain
 * (Mesh.TriangleCount()*3), matching DirectionalNormalMask/MaterialSlotMask -- never Vertex-ID-domain
 * like FVertexMaskForgeSourceTopologyAOCache's own ElementID choice (see the audit report's own
 * rationale for choosing CornerIndex here instead).
 */
struct FVertexMaskForgeSourceTopologyThicknessCache
{
	TUniquePtr<UE::Geometry::FDynamicMesh3> LocalMesh;
	TUniquePtr<UE::Geometry::FDynamicMeshAABBTree3> Tree;
	bool bTreeValid = false;

	const UE::Geometry::FDynamicMesh3* CachedSourceMesh = nullptr;
	uint32 CachedGeometryFingerprint = 0;

	TArray<float> RawDistances;   // indexed by CornerIndex (Mesh.TriangleCount()*3)
	TBitArray<> bRawValid;
	float CachedSearchDistance = 0.f;
	float CachedBias = 0.f;
	bool bValuesValid = false;

	int32 NumInvalidOriginNormal = 0;
	int32 NumDegenerateTrianglesDiscarded = 0;
	int32 NumIncidentOnlyRejections = 0;
	int32 NumOrientationRejections = 0;
	int32 NumNoHit = 0;
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
 * opinion about which mode produced it -- see VertexMaskForgeBoundingBoxGenerator::GenerateBoundingBoxMask's
 * CollectiveBounds parameter and VertexMaskForgeBoundingBoxGenerator::ComputeCollectiveAxisBounds.
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
	 * VertexMaskForgeBoundingBoxGenerator::ResolveSharedLocalAxis) -- used to project each component's own
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

/** Which of FVertexMaskForgeWorkingMesh::CurvatureRawConvexCache/CurvatureRawConcaveCache (or their
 *  union) Curvature Type selects as the mask magnitude -- see
 *  VertexMaskForgeCurvatureGenerator::ApplyCurvatureArtisticParams. */
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

/**
 * A transient, independent working copy of a Static Mesh's LOD 0, used as the basis for future
 * mask generators. Never written back to the source asset. Owns its FDynamicMesh3 by pointer so
 * the (relatively heavy) mesh data is never copied when this struct is moved.
 */
struct FVertexMaskForgeWorkingMesh
{
	// M16-J.0B (rejection-corrective pass): FVertexMaskForgeWorkingMeshOwner is the only class allowed
	// to wholesale-replace an installed Working Mesh (via its own private move-assignment access below)
	// -- see that operator's own doc comment for why. It has no access to anything else here; every
	// individual field below remains a plain public member, exactly as before, since none of them
	// (other than Provenance, itself already separately encapsulated -- see
	// FVertexMaskForgeWorkingMeshProvenance's own doc comment) are part of the authenticated identity
	// contract this checkpoint protects.
	friend class FVertexMaskForgeWorkingMeshOwner;

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

	/** Declared here, defined in the .cpp for the same reason as the destructor. Move CONSTRUCTION stays
	 *  public (it only ever creates a new, independent instance -- e.g. binding a temporary into
	 *  FVertexMaskForgeWorkingMeshOwner::InstallWorkingMesh's `&&` parameter -- it never overwrites an
	 *  owner's already-installed storage). */
	FVertexMaskForgeWorkingMesh(FVertexMaskForgeWorkingMesh&&);

private:
	/**
	 * M16-J.0B (rejection-corrective pass): move-ASSIGNMENT is private, friended only to
	 * FVertexMaskForgeWorkingMeshOwner. Assigning THROUGH a mutable FVertexMaskForgeWorkingMesh&
	 * reference (e.g. FVertexMaskForgeSelectedMesh::WorkingMesh) can mutate individual fields freely
	 * (that is the whole point -- BoundingBoxMask/CurvatureMask/NoiseMask/MaterialSlotMask/
	 * DirectionalNormalMask/ThicknessMask/their raw caches/InstanceResults are ordinary, unauthenticated
	 * generator/cache state, never part of this checkpoint's contract), but wholesale-REPLACING the
	 * entire object (`SomeWorkingMesh = MoveTemp(Other);`) would silently discard the currently-
	 * installed Provenance without going through FVertexMaskForgeWorkingMeshOwner::InstallWorkingMesh's
	 * own generation-stamping -- exactly the "substituir a WorkingMesh inteira" gap this fix closes.
	 * BuildWorkingMeshes' only real call site already goes through InstallWorkingMesh, never this
	 * operator directly.
	 */
	FVertexMaskForgeWorkingMesh& operator=(FVertexMaskForgeWorkingMesh&&);

public:
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

	// M16-J.0B.1 (WorkingMesh Domain Split): BoundingBoxMask/AmbientOcclusionMask/CurvatureMask/its raw
	// caches/NoiseMask/its raw cache/MaterialSlotMask/DirectionalNormalMask/its conflict flag/ThicknessMask/
	// its caches all MOVED to FVertexMaskForgeGeneratorState (defined immediately after this struct) --
	// see that struct's own module comment for why: they are Generator Artistic State (freely mutable,
	// never authenticated, never gated by generation), structurally distinct from this struct's own
	// Authenticated Geometry/Identity contract (State/Mesh/GeometryFingerprint/TriIDMap/Provenance/the
	// Material-Slot LOOKUP TABLES below, which stay here because they are built ONCE by the builder,
	// alongside the rest of this struct's identity fields, and never touched by any per-invocation
	// generator code afterward).

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

	// --- Directional Normal Mask (V2-E) / Thickness Mask (V2-G) --------------------------------
	// DirectionalNormalMask/bDirectionalNormalWorldSpaceConflict/ThicknessMask/ThicknessCache/
	// SourceTopologyThicknessCache all moved to FVertexMaskForgeGeneratorState -- see this struct's own
	// leading AUDITED note above.

	/**
	 * M16-C: per-entry keyed instance-result storage for the future Mask Stack architecture -- see
	 * VertexMaskForgeInstanceResultStore.h's own module comment. Holds results for whichever future
	 * Mask Instances are transform-independent (computed once per entry, shared across every component
	 * of this asset -- the same "computed ONCE PER ENTRY" contract CurvatureMask/NoiseMask/
	 * MaterialSlotMask/ThicknessMask already have above). Empty and unused in this checkpoint: no
	 * generator writes to it, no orchestration reads from it, and it has no effect on any existing
	 * named mask/cache field above. Moves naturally with this struct (already move-only; TMap moves
	 * trivially) -- no change was needed to the move constructor/assignment operator, both still
	 * `= default` in the .cpp.
	 */
	FVertexMaskForgeInstanceResultStore InstanceResults;

	/**
	 * M16-J.0 / M16-J.0A / M16-J.0A.1: persistent provenance identity for this Working Mesh -- WHO/WHAT
	 * it was authoritatively built from (Static Mesh, domain, LOD, generation Revision, expected
	 * cardinality -- no component: this Working Mesh is shared across every component of its
	 * FVertexMaskForgeSelectedMesh entry, see FVertexMaskForgeWorkingMeshOwner's own module comment).
	 * See VertexMaskForgeWorkingColorsProvenance.h's own module comment. Invalid (IsValid() == false) by
	 * default -- a freshly built FVertexMaskForgeWorkingMesh carries no provenance until
	 * FVertexMaskForgeWorkingMeshOwner::InstallWorkingMesh stamps it internally (see
	 * VertexMaskForgeWorkingMeshOwner.h); nothing in the existing legacy flow sets this yet, and no
	 * public factory can fabricate a valid one (Provenance's fields are private, friended only to
	 * FVertexMaskForgeWorkingMeshOwner). Moves naturally with this struct (plain TWeakObjectPtr/POD
	 * fields only) -- no change was needed to the move constructor/assignment operator, both still
	 * `= default` in the .cpp.
	 */
	FVertexMaskForgeWorkingMeshProvenance Provenance;
};

/**
 * M16-J.0B.1 (WorkingMesh Domain Split): Generator Artistic State -- every per-generator mask RESULT and
 * its purely-derived cache, extracted out of FVertexMaskForgeWorkingMesh so that class can remain a
 * const-exposed, MeshOwner-authenticated Identity/Geometry type (see that struct's own leading AUDITED
 * note). None of the fields here are part of any authenticated identity contract: they are freely
 * mutable by the real generator call sites (BoundingBoxGenerator, AmbientOcclusionGenerator,
 * CurvatureGenerator, NoiseGenerator, MaterialSlotGenerator, DirectionalNormalGenerator,
 * ThicknessGenerator, all still living in SVertexMaskForgePanel.cpp/their own generator .cpp files,
 * completely UNCHANGED in algorithm), never gated by generation, never touch Provenance, never touch
 * InstanceResults. One instance per FVertexMaskForgeSelectedMesh entry (see that struct's own
 * GeneratorState field) -- exactly the same "one per entry, shared by every component" lifetime
 * FVertexMaskForgeWorkingMesh's own masks already had before this split, just now in its own type.
 *
 * Move-only (ThicknessCache/SourceTopologyThicknessCache are TUniquePtr) -- same reasoning and pattern as
 * FVertexMaskForgeWorkingMesh's own special members.
 */
struct FVertexMaskForgeGeneratorState
{
	FVertexMaskForgeGeneratorState() = default;
	~FVertexMaskForgeGeneratorState() = default;

	FVertexMaskForgeGeneratorState(const FVertexMaskForgeGeneratorState&) = delete;
	FVertexMaskForgeGeneratorState& operator=(const FVertexMaskForgeGeneratorState&) = delete;

	FVertexMaskForgeGeneratorState(FVertexMaskForgeGeneratorState&&) = default;
	FVertexMaskForgeGeneratorState& operator=(FVertexMaskForgeGeneratorState&&) = default;

	/** See FVertexMaskForgeWorkingMesh::BoundingBoxMask's own (former) doc comment -- unchanged contract,
	 *  only the owning struct changed. Same for every other field below: unchanged contract/semantics,
	 *  moved verbatim out of FVertexMaskForgeWorkingMesh. */
	FVertexMaskForgeScalarMask BoundingBoxMask;
	FVertexMaskForgeScalarMask AmbientOcclusionMask;
	FVertexMaskForgeScalarMask CurvatureMask;
	TArray<float> CurvatureRawConvexCache;
	TArray<float> CurvatureRawConcaveCache;
	TArray<int32> CurvatureRenderVertexToDynamicMeshVertex;
	uint32 CurvatureCacheFingerprint = 0;
	FVertexMaskForgeScalarMask NoiseMask;
	TArray<float> NoiseRawCache;
	uint32 NoiseCacheFingerprint = 0;
	FVertexMaskForgeNoiseGenerativeParams NoiseCacheUsedParams;
	FVertexMaskForgeScalarMask MaterialSlotMask;
	FVertexMaskForgeScalarMask DirectionalNormalMask;
	bool bDirectionalNormalWorldSpaceConflict = false;
	FVertexMaskForgeScalarMask ThicknessMask;
	TUniquePtr<FVertexMaskForgeThicknessCache> ThicknessCache;
	TUniquePtr<FVertexMaskForgeSourceTopologyThicknessCache> SourceTopologyThicknessCache;
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
	friend class FVertexMaskForgeWorkingStateOwner;

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

	// --- M16-J.0B (rejection-corrective pass): read-only accessors for every field that moved to
	// `private` below. These fields are the ones a future M16-J boundary must authenticate -- target
	// component identity, the three color arrays (in both domains), InstanceResults, and the alpha
	// authority itself -- so no external code (not even through a mutable reference to this whole
	// struct) can reassign them outside FVertexMaskForgeWorkingStateOwner's own controlled API. This is
	// enforced by the compiler (private + friend), not by convention or a method name. Fields that are
	// NOT part of this authenticated contract (PreviewComponent, AOCache, HiddenOwner, bOverrideActive,
	// bHasAcquiredActorHide, SourceTopologyPreviewComponent, SourceTopologyAOCache -- all panel-owned
	// visual/session bookkeeping, explicitly out of scope for the owner infrastructure) remain plain
	// public fields below, unchanged.
	TWeakObjectPtr<UStaticMeshComponent> GetSourceComponent() const { return SourceComponent; }
	const TArray<FColor>& GetBaselineColors() const { return BaselineColors; }
	const TArray<FColor>& GetCommittedColors() const { return CommittedColors; }
	const TArray<FColor>& GetWorkingColors() const { return WorkingColors; }
	const TArray<FColor>& GetSourceTopologyBaselineColors() const { return SourceTopologyBaselineColors; }
	const TArray<FColor>& GetSourceTopologyCommittedColors() const { return SourceTopologyCommittedColors; }
	const TArray<FColor>& GetSourceTopologyWorkingColors() const { return SourceTopologyWorkingColors; }
	const FVertexMaskForgeInstanceResultStore& GetInstanceResults() const { return InstanceResults; }
	const FVertexMaskForgeWorkingColorsAuthority& GetWorkingColorsAuthority() const { return WorkingColorsAuthority; }

private:
	/** The real, selected component. Read-only source for mesh/transform; never mutated except by
	 *  FVertexMaskForgeWorkingStateOwner::ConfigureTarget (private, friended -- see GetSourceComponent()
	 *  above for the public read-only accessor). */
	TWeakObjectPtr<UStaticMeshComponent> SourceComponent;

public:
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

private:
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

public:
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

private:
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

	/**
	 * M16-C: per-component keyed instance-result storage for the future Mask Stack architecture -- see
	 * VertexMaskForgeInstanceResultStore.h's own module comment. Holds results for whichever future
	 * Mask Instances are transform-dependent (re-evaluated per component -- the same reason
	 * AOCache/SourceTopologyAOCache already live here rather than on FVertexMaskForgeWorkingMesh).
	 * Empty and unused in this checkpoint: no generator writes to it, no orchestration reads from it,
	 * and it has no effect on any existing named cache/color field above. Moves naturally with this
	 * struct (already move-only; TMap moves trivially) -- no change was needed to the move
	 * constructor/assignment operator, both still `= default` in the .cpp.
	 */
	FVertexMaskForgeInstanceResultStore InstanceResults;

	/**
	 * M16-J.0 / M16-J.0A / M16-J.0A.1: explicit authority over THIS component state's own WorkingColors/
	 * SourceTopologyWorkingColors buffer (selected by the authority's own GetUseSourceTopology()) --
	 * proves the buffer's alpha channel is currently meaningful and which FVertexMaskForgeWorkingMeshOwner's
	 * provenance it corresponds to (see Authority::GetMeshOwnerId()), instead of merely having the
	 * "right" Num(). See VertexMaskForgeWorkingColorsProvenance.h's own module comment. Invalid
	 * (IsValid() == false) by default -- nothing in the existing legacy flow establishes this yet; it has
	 * no effect on any existing named color field above until
	 * FVertexMaskForgeWorkingStateOwner::InitializeColors stamps it internally (see
	 * VertexMaskForgeWorkingStateOwner.h) -- no public factory can fabricate a valid one (Authority's
	 * fields are private, friended only to FVertexMaskForgeWorkingStateOwner). Moves naturally with this
	 * struct (plain TWeakObjectPtr/POD fields only) -- no change was needed to the move constructor/
	 * assignment operator, both still `= default` in the .cpp.
	 */
	FVertexMaskForgeWorkingColorsAuthority WorkingColorsAuthority;
};

namespace VertexMaskForgeWorkingMeshTypes
{
	/**
	 * AUDITED (V2-E CORRECTIVE PASS): the world-space normal-transform matrix (inverse-transpose of the
	 * component's 3x3 linear part) used to transform a local-space normal into World Space -- see
	 * TransformNormalToWorldSpace/HasConflictingWorldSpaceNormalTransforms for consumers. Returns false
	 * (never a guessed/fallback matrix) if the transform's scale is degenerate or the resulting matrix
	 * contains a non-finite element.
	 *
	 * AUDITED (M6, Extract Shared Working-Mesh Domain Types): single shared implementation, called both
	 * by HasConflictingWorldSpaceNormalTransforms below (moved here) and by the Directional Normal
	 * generation functions that remain in SVertexMaskForgePanel.cpp (GenerateDirectionalNormalMask /
	 * GenerateDirectionalNormalMaskFromDynamicMesh) -- see the definition in
	 * VertexMaskForgeWorkingMeshTypes.cpp for the full algorithm doc comment. Body unchanged from its
	 * pre-M6 form; only linkage (no longer file-static) and location changed.
	 */
	bool ComputeWorldSpaceNormalMatrix(const FTransform& ComponentTransform, FMatrix& OutNormalMatrix);

	/**
	 * AUDITED (V2-E CORRECTIVE PASS): compares EVERY live component's own World-Space normal MATRIX
	 * against a single reference matrix (the first valid one found) using full-matrix proportionality.
	 * Consumed both by the live UI conflict diagnostic (RunAutoUpdatePreview) and by the Accept target
	 * builder (VertexMaskForgeAcceptTargetBuilder::BuildAcceptTargets/BuildSourceTopologyAcceptTargets)
	 * as a re-check immediately before Accept is allowed to proceed -- see the definition in
	 * VertexMaskForgeWorkingMeshTypes.cpp for the full algorithm doc comment.
	 */
	bool HasConflictingWorldSpaceNormalTransforms(
		const TArray<TUniquePtr<FVertexMaskForgeWorkingStateOwner>>& PreviewComponents,
		float& OutMaxRelativeDeviation);

	/**
	 * AUDITED (M6, Extract Shared Working-Mesh Domain Types): scale-aware degenerate-triangle test used
	 * by AreThicknessGeometrySnapshotsExactlyEquivalent below -- see the definition in
	 * VertexMaskForgeWorkingMeshTypes.cpp for the full algorithm doc comment. Single shared
	 * implementation, called both from here and from the Thickness generation functions that remain in
	 * SVertexMaskForgePanel.cpp (GenerateThicknessMask / GenerateThicknessMaskFromDynamicMesh) -- same
	 * "moved because the sole alternative to one shared implementation is duplication" rationale as
	 * ComputeWorldSpaceNormalMatrix above. Body unchanged from its pre-M6 form; only linkage (no longer
	 * file-static) and location changed.
	 */
	bool IsThicknessTriangleDegenerate(const FVector3d& P0, const FVector3d& P1, const FVector3d& P2);

	/**
	 * AUDITED (M5, Extract Accept Writer; M6, Extract Shared Working-Mesh Domain Types): full deep
	 * geometry-freshness comparison for the Thickness Mask cache (render-vertex domain) -- see the
	 * definition in VertexMaskForgeWorkingMeshTypes.cpp for the full algorithm doc comment. Consumed
	 * both by Thickness generation (as a re-verification, not only at Accept) and by
	 * VertexMaskForgeAcceptWriter::WriteAcceptTargets as the write-time freshness gate.
	 */
	bool AreThicknessGeometrySnapshotsExactlyEquivalent(
		const FVertexMaskForgeThicknessCache& Cache,
		const FStaticMeshLODResources& CurrentLOD0);

	/**
	 * AUDITED (M5, Extract Accept Writer; M6, Extract Shared Working-Mesh Domain Types): Source-Topology
	 * sibling of AreThicknessGeometrySnapshotsExactlyEquivalent -- see the definition in
	 * VertexMaskForgeWorkingMeshTypes.cpp for the full algorithm doc comment. Consumed only by
	 * VertexMaskForgeAcceptWriter::WriteSourceTopologyAcceptTargets as the write-time freshness gate.
	 */
	bool IsThicknessSourceTopologyContentUnchanged(
		const UE::Geometry::FDynamicMesh3& OldMesh,
		const TArray<FTriangleID>& TriIDMap,
		const FMeshDescription& CurrentMeshDescription);
}

/**
 * One unique Static Mesh found in the current selection.
 * Kept small and self-contained for this checkpoint; safe to relocate to a
 * shared header once processing is introduced.
 */
struct FVertexMaskForgeSelectedMesh
{
	/**
	 * M16-J.0B: declared here, defined in VertexMaskForgeWorkingMeshTypes.cpp -- MeshOwner's constructor
	 * body (MakeShared<FVertexMaskForgeWorkingMeshOwner>()) needs FVertexMaskForgeWorkingMeshOwner to be
	 * a complete type, which this header deliberately does not include (would create a circular include:
	 * VertexMaskForgeWorkingMeshOwner.h/.cpp already include THIS header for FVertexMaskForgeWorkingMesh)
	 * -- same reasoning as FVertexMaskForgeWorkingMesh's own out-of-line special members below.
	 */
	FVertexMaskForgeSelectedMesh();

	/** Declared here, defined in the .cpp for the same reason as the constructor (TUniquePtr<
	 *  FVertexMaskForgeWorkingStateOwner> element destruction needs the complete type). */
	~FVertexMaskForgeSelectedMesh();

	/** Editor-safe soft reference to the asset. Does not force it to stay loaded. */
	TSoftObjectPtr<UStaticMesh> Mesh;

	/** Cached display name, so rows do not need to resolve the soft pointer. */
	FString AssetName;

	/** Cached full asset path string, used both for display and as the dedup key. */
	FString AssetPathString;

	FVertexMaskForgeMeshDiagnostics Diagnostics;

	/**
	 * M16-J.0B: owns this entry's single, shared Working Mesh lifecycle -- geometry, generation,
	 * Provenance -- see FVertexMaskForgeWorkingMeshOwner's own module comment for why geometry ownership
	 * lives here (per-ENTRY), never per-component. TSharedPtr so FVertexMaskForgeWorkingStateOwner
	 * instances (one per component of this entry) can attach to it via TWeakPtr -- see
	 * FVertexMaskForgeWorkingStateOwner::AttachToMeshOwner. Always valid (constructed in this struct's
	 * own constructor) -- never null for a real entry.
	 */
	TSharedPtr<FVertexMaskForgeWorkingMeshOwner> MeshOwner;

	/**
	 * M16-J.0B.1 corrective pass: there is deliberately NO persistent WorkingMesh reference member here
	 * anymore. A prior revision of this checkpoint bound one in the constructor
	 * (WorkingMesh(MeshOwner->GetWorkingMesh())); an audit correctly flagged that as an unproven-lifetime
	 * alias pattern -- a reference member is exactly the shape of bug that silently dangles the moment
	 * anyone adds a copy/move path to this struct in the future, and proving it safe today by pointing at
	 * this struct's CURRENT accidental non-copyability (a TUniquePtr element in PreviewComponents below,
	 * a user-declared destructor suppressing the implicit move) is a fragile argument to keep re-deriving
	 * forever. Every real call site now obtains a fresh, short-lived const view at its own point of use:
	 *
	 *     const FVertexMaskForgeWorkingMesh& WorkingMesh = Entry->MeshOwner->GetWorkingMesh();
	 *
	 * MeshOwner (below) is the single source of truth; GetWorkingMesh() is O(1) and side-effect-free, so
	 * there is no performance reason to cache the result beyond one function's own scope. See
	 * FVertexMaskForgeWorkingMeshOwner::GetWorkingMesh's own doc comment for why ITS returned reference is
	 * safe to use this way: it aliases a member that lives inside the SAME heap allocation as MeshOwner's
	 * own pointee (a TSharedPtr<FVertexMaskForgeWorkingMeshOwner> control block never relocates the
	 * pointee), so a short-lived local view taken from it is valid for as long as the caller also holds
	 * (directly or transitively) a reference to that same MeshOwner -- which every real call site does,
	 * via Entry.
	 */

	/**
	 * M16-J.0B.1 (WorkingMesh Domain Split): this entry's Generator Artistic State (BoundingBoxMask,
	 * CurvatureMask, NoiseMask, MaterialSlotMask, DirectionalNormalMask, ThicknessMask, their raw caches
	 * -- see FVertexMaskForgeGeneratorState's own module comment). Owned directly BY VALUE (not via an
	 * owner/generation contract -- this state was never part of the authenticated identity this
	 * checkpoint protects), default-constructed fresh for every entry -- since a brand new
	 * FVertexMaskForgeSelectedMesh is always constructed on RefreshSelection (never reused), this
	 * automatically satisfies the exact same "every mask starts at NotGenerated on a fresh Working Mesh"
	 * invariant the pre-split design already had, just expressed through a fresh GeneratorState instead
	 * of a fresh WorkingMesh.
	 */
	FVertexMaskForgeGeneratorState GeneratorState;

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
	 *
	 * M16-J.0B: one FVertexMaskForgeWorkingStateOwner per component (owns that component's own
	 * BaselineColors/CommittedColors/WorkingColors/authority lifecycle -- see that class's own module
	 * comment), held by TUniquePtr for a stable heap address regardless of this array's own reallocation
	 * (StateOwnerId uses that address; also required since FVertexMaskForgeWorkingStateOwner is itself
	 * non-copyable/non-movable). Every StateOwner in this array is attached (AttachToMeshOwner) to THIS
	 * entry's own MeshOwner above -- never a different entry's.
	 */
	TArray<TUniquePtr<FVertexMaskForgeWorkingStateOwner>> PreviewComponents;
};
