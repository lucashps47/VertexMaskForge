// M16-K.6D-4: see VertexMaskForgeDynamicSourceTopologyComposition.h for the full module contract.
//
// ISOLATION RATIONALE for bypassing VertexMaskForgeDynamicLayerEvaluator::EvaluateColor's masked overload
// and VertexMaskForgeDynamicLayerBatchCompositor::ComposeColors: both require a
// FVertexMaskForgeInstanceResultStore parameter to resolve a masked layer's EffectiveMask by
// MaskInstanceId -- this checkpoint's own instructions forbid this orchestrator from receiving,
// constructing, or touching a FVertexMaskForgeInstanceResultStore of any kind (not even a local,
// function-scoped, never-escaping one), because that type's entire purpose (identity-keyed, mutation-
// tracked, cross-call persistence) is foreign to a purely local, caller-owned, single-call computation --
// introducing one here, even transiently, would blur the exact boundary this checkpoint exists to keep
// sharp. Instead, this module resolves each masked layer's EffectiveMask directly from the freshly
// generated FVertexMaskForgeScalarMask array Pass 1 below produces (positional lookup by corner index,
// never a MaskInstanceId/FGuid lookup of any kind), and reuses the exact same downstream blend-mode fold
// primitives EvaluateColor itself calls (VertexMaskForgeSequentialEvaluator::EvaluateFillLayerStep and
// VertexMaskForgeDynamicLayerEvaluator::TryResolveFillValue) so there is exactly one implementation of
// the actual composition math anywhere in the plugin -- only the "how is EffectiveMask resolved" step
// differs, by necessity.

#include "VertexMaskForgeDynamicSourceTopologyComposition.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "VertexMaskForgeAmbientOcclusionGenerator.h"
#include "VertexMaskForgeBoundingBoxGenerator.h"
#include "VertexMaskForgeColorConversion.h"
#include "VertexMaskForgeCurvatureGenerator.h"
#include "VertexMaskForgeDirectionalNormalGenerator.h"
#include "VertexMaskForgeDynamicLayerEvaluator.h"
#include "VertexMaskForgeDynamicLayerStack.h"
#include "VertexMaskForgeLayerTypes.h"
#include "VertexMaskForgeMaskTypes.h"
#include "VertexMaskForgeMaterialSlotGenerator.h"
#include "VertexMaskForgeNoiseGenerator.h"
#include "VertexMaskForgeRecipeTypes.h"
#include "VertexMaskForgeSequentialEvaluator.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

namespace
{
	// M16-K.6D-8B: this orchestrator's masked layers are no longer all corner-domain -- Material Slot's
	// own generator already produces one value per Source-Topology corner (see
	// GenerateMaterialSlotMaskFromDynamicMesh), but Bounding Box's Source-Topology generator
	// (GenerateBoundingBoxMaskFromDynamicMesh) is indexed by Dynamic Mesh VertexID instead (sparse-safe,
	// TryGetValue-only -- see that function's own "INDEX SAFETY" doc note). Domain is tracked explicitly,
	// per generated layer mask, never inferred from Values.Num() (which could coincidentally equal the
	// corner count for a mesh where VertexCount()*something == TriangleCount()*3) -- this mirrors the
	// same per-generator domain distinction Legacy's own UpdateWorkingColorsSourceTopology already makes
	// (its IndexOverride switch: Material Slot by CornerIndex, Bounding Box/Curvature/Noise by Dynamic
	// Mesh VertexID via Mesh.GetTriangle(TriangleID)[Corner]) -- reimplemented here structurally
	// isolated from that Legacy code (no Legacy composition function is ever called), never copied from
	// it.
	enum class ELayerMaskDomain : uint8
	{
		Corner,
		DynamicMeshVertex,

		// M16-K.6D-8G-D: Ambient Occlusion's own raw mask is indexed by Normal Overlay Element ID, a
		// third domain distinct from both above (see GenerateAmbientOcclusionMaskFromDynamicMesh's own
		// doc comment) -- a hard-edge corner shares its Dynamic Mesh VertexID with its neighbors but has
		// its OWN Normal Overlay element, so reusing the DynamicMeshVertex domain here would silently
		// average/smooth hard edges.
		NormalOverlayElement,
	};

	struct FLayerGeneratedMask
	{
		FVertexMaskForgeScalarMask Mask;
		ELayerMaskDomain Domain = ELayerMaskDomain::Corner;
	};
}

bool VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(
	const FVertexMaskForgeWorkingMesh& WorkingMesh,
	const FVertexMaskForgeDynamicLayerStack& Stack,
	TConstArrayView<FColor> BaseColors,
	const FTransform& ComponentTransform,
	TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache>& DynamicSourceTopologyAOCachesByLayerId,
	TArray<FColor>& OutComposedColors)
{

	if (!WorkingMesh.Mesh.IsValid())
	{
		return false;
	}

	const int32 ExpectedCornerCount = WorkingMesh.Mesh->TriangleCount() * 3;
	if (BaseColors.Num() != ExpectedCornerCount)
	{
		return false;
	}

	const TArray<FVertexMaskForgeLayer>& Layers = Stack.GetLayers();

	// --- Pass 1: resolve every ENABLED masked layer's scalar mask up front, all-or-nothing -- a
	// structural failure (unsupported generator type, or the generator itself not Ready) fails the WHOLE
	// call before any per-corner work begins; nothing is ever partially composed. ---
	TArray<FLayerGeneratedMask> LayerMasks;
	LayerMasks.SetNum(Layers.Num());

	// Lazily built (only if at least one enabled layer actually requests Bounding Box) -- maps this
	// call's own corner-domain index to the underlying Dynamic Mesh VertexID at that corner, walking
	// WorkingMesh.Mesh->TriangleIndicesItr() in order (NEVER assuming TriangleID is dense/contiguous) so
	// "CornerIndex N" means the identical physical corner GenerateMaterialSlotMaskFromDynamicMesh's own
	// CornerIndex assignment already uses (see that function's own TriangleIndicesItr()-driven loop) --
	// both domains agree on what a given CornerIndex physically is.
	TArray<int32> CornerToVertexID;
	bool bCornerToVertexIDBuilt = false;

	// M16-K.6D-8G-D: sibling of CornerToVertexID above, lazily built only if at least one enabled layer
	// actually requests Ambient Occlusion -- maps this call's own corner-domain index to the underlying
	// Normal Overlay ELEMENT ID at that corner (NormalOverlay->GetTriangle(TriangleID)[Corner], never
	// Mesh->GetTriangle), since Ambient Occlusion's raw mask is Normal-Overlay-Element-ID domain, not
	// Dynamic Mesh VertexID domain (see ELayerMaskDomain::NormalOverlayElement's own comment above).
	TArray<int32> CornerToElementID;
	bool bCornerToElementIDBuilt = false;

	// M16-K.6D-8E-B: ONE function-local FVertexMaskForgeGeneratorState, declared here (outside the
	// per-layer loop below) and reused for every enabled Curvature layer THIS call evaluates -- never
	// per-layer, never keyed by LayerId, never the Legacy per-entry FVertexMaskForgeGeneratorState (never
	// reachable from this orchestrator), never static/persistent, never exposed through this function's
	// own public signature, and never retained beyond this single call (destroyed when this function
	// returns). GenerateCurvatureMaskFromDynamicMesh's own EnsureCurvatureRawCache internally gates its
	// expensive raw Convex/Concave computation on this state's own CurvatureCacheFingerprint versus
	// WorkingMesh.GeometryFingerprint -- since that fingerprint never changes within one call, the FIRST
	// enabled Curvature layer computes the raw arrays and every SUBSEQUENT enabled Curvature layer in the
	// same call reuses them for free, while each layer still applies its own Type/Multiplier/Blur/Levels/
	// Invert independently (see the Curvature branch below). This provides WITHIN-CALL sharing only --
	// a later, separate orchestrator invocation (e.g. a subsequent Auto Update Preview recomposition)
	// starts with a fresh, empty CurvatureLocalGeneratorState and recomputes the raw arrays again, even
	// if geometry has not changed; persistent cross-call caching is explicitly deferred, not implemented
	// here.
	FVertexMaskForgeGeneratorState CurvatureLocalGeneratorState;

	for (int32 LayerIndex = 0; LayerIndex < Layers.Num(); ++LayerIndex)
	{
		const FVertexMaskForgeLayer& Layer = Layers[LayerIndex];
		if (!Layer.bEnabled || !Layer.Mask.IsSet())
		{
			continue;
		}

		if (Layer.Mask->GeneratorType == EVertexMaskForgeGeneratorType::MaterialSlot)
		{
			const FVertexMaskForgeMaterialSlotParams* SlotParams = Layer.Mask->Params.TryGet<FVertexMaskForgeMaterialSlotParams>();
			if (!SlotParams)
			{
				// Defensive -- should be unreachable given FVertexMaskForgeDynamicLayerStack's own
				// GeneratorType/Params coherence invariant, but never assumed (mirrors every other
				// Material Slot caller's own defensive check in this codebase).
				return false;
			}

			FVertexMaskForgeScalarMask GeneratedMask = VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskFromDynamicMesh(
				WorkingMesh, SlotParams->SelectedSlotIndex, SlotParams->bInvert);
			if (GeneratedMask.State != EVertexMaskForgeScalarMaskState::Ready || GeneratedMask.Values.Num() != ExpectedCornerCount)
			{
				return false;
			}

			LayerMasks[LayerIndex].Mask = MoveTemp(GeneratedMask);
			LayerMasks[LayerIndex].Domain = ELayerMaskDomain::Corner;
		}
		else if (Layer.Mask->GeneratorType == EVertexMaskForgeGeneratorType::BoundingBox)
		{
			const FVertexMaskForgeBoundingBoxParams* BBoxParams = Layer.Mask->Params.TryGet<FVertexMaskForgeBoundingBoxParams>();
			if (!BBoxParams)
			{
				// Defensive -- mirrors the Material Slot coherence check above.
				return false;
			}

			// M16-K.6D-8B scope: Local-space, per-mesh bounds ONLY. World Space and Unified Bounds are
			// explicitly REJECTED here (whole-call failure), never silently reinterpreted as Local Space
			// or per-mesh bounds -- both require inputs (a component transform; full-selection context)
			// this orchestrator does not receive in this checkpoint. bUseUnifiedBounds is geometric/
			// generator state (NOT a composition concern like Blend/Opacity), checked unconditionally
			// regardless of which axes are enabled, since Unified Bounds changes what "the bounds" even
			// are before any per-axis evaluation begins.
			if (BBoxParams->bUseUnifiedBounds)
			{
				return false;
			}
			for (const FVertexMaskForgeAxisMaskParams& AxisParams : BBoxParams->Axes)
			{
				if (AxisParams.bEnabled && AxisParams.bWorldSpace)
				{
					return false;
				}
			}

			// Every enabled axis has now been confirmed Local-space -- ResolveAxisCoordinate's own
			// Local-space branch never reads ComponentTransform, so FTransform::Identity is safe here
			// ONLY because that request was just proven, never supplied as a guess/placeholder ahead of
			// validation.
			FVertexMaskForgeScalarMask GeneratedMask = VertexMaskForgeBoundingBoxGenerator::GenerateBoundingBoxMaskFromDynamicMesh(
				*WorkingMesh.Mesh, BBoxParams->Axes, FTransform::Identity);
			if (GeneratedMask.State != EVertexMaskForgeScalarMaskState::Ready)
			{
				// Covers "no enabled axes" and "degenerate bounds" alike -- both leave the generator's
				// own State at something other than Ready (Unavailable / DegenerateBounds respectively),
				// exactly as GenerateBoundingBoxMaskFromDynamicMesh's own contract already establishes;
				// no separate check is invented here.
				return false;
			}

			LayerMasks[LayerIndex].Mask = MoveTemp(GeneratedMask);
			LayerMasks[LayerIndex].Domain = ELayerMaskDomain::DynamicMeshVertex;

			if (!bCornerToVertexIDBuilt)
			{
				CornerToVertexID.SetNumUninitialized(ExpectedCornerCount);
				int32 RunningCornerIndex = 0;
				for (const int32 TriangleID : WorkingMesh.Mesh->TriangleIndicesItr())
				{
					const UE::Geometry::FIndex3i Tri = WorkingMesh.Mesh->GetTriangle(TriangleID);
					CornerToVertexID[RunningCornerIndex++] = Tri.A;
					CornerToVertexID[RunningCornerIndex++] = Tri.B;
					CornerToVertexID[RunningCornerIndex++] = Tri.C;
				}
				bCornerToVertexIDBuilt = true;
			}
		}
		else if (Layer.Mask->GeneratorType == EVertexMaskForgeGeneratorType::DirectionalNormal)
		{
			const FVertexMaskForgeDirectionalNormalParams* NormalParams = Layer.Mask->Params.TryGet<FVertexMaskForgeDirectionalNormalParams>();
			if (!NormalParams)
			{
				// Defensive -- mirrors the Material Slot/Bounding Box coherence checks above.
				return false;
			}

			// M16-K.6D-8D-B scope: Local-space only. World Space is explicitly REJECTED here (whole-call
			// failure), never silently reinterpreted as Local -- World Space requires a per-component
			// transform this orchestrator does not receive in this checkpoint, the same class of gap
			// that made Bounding Box's own World Space/Unified Bounds explicitly rejected in M16-K.6D-8B.
			if (NormalParams->Space != EVertexMaskForgeNormalSpace::Local)
			{
				return false;
			}

			// Local-space evaluation never reads ComponentTransform (confirmed directly from
			// GenerateDirectionalNormalMaskFromDynamicMesh's own header doc comment and its Space==Local
			// branch), so FTransform::Identity is safe here ONLY because Local-space was just proven
			// above, never supplied as a guess ahead of validation.
			FVertexMaskForgeScalarMask GeneratedMask = VertexMaskForgeDirectionalNormalGenerator::GenerateDirectionalNormalMaskFromDynamicMesh(
				WorkingMesh, NormalParams->Space, NormalParams->Direction, NormalParams->Angle, NormalParams->Falloff,
				NormalParams->Blur, NormalParams->bInvert, FTransform::Identity);
			if (GeneratedMask.State != EVertexMaskForgeScalarMaskState::Ready
				|| GeneratedMask.Values.Num() != ExpectedCornerCount
				|| GeneratedMask.NumValidValues != ExpectedCornerCount)
			{
				// The third check additionally requires EVERY corner to have resolved a real value --
				// GenerateDirectionalNormalMaskFromDynamicMesh's own contract allows an individual corner
				// to be left unwritten (missing Normal Overlay element, degenerate normal) while still
				// reporting Ready overall (NumValidValues > 0 is its own success threshold). This
				// orchestrator's Corner-domain Pass 2 reads Values[CornerIndex] directly and
				// unconditionally (Material Slot's own established precedent, always dense) -- a hole
				// here would silently read a zero-initialized placeholder as a real computed value,
				// exactly the guess FVertexMaskForgeScalarMask's own contract forbids. Rejecting the
				// whole call on any hole is the safe choice; it introduces no new domain, no per-corner
				// TryGetValue path, and no composition-path change.
				return false;
			}

			LayerMasks[LayerIndex].Mask = MoveTemp(GeneratedMask);
			LayerMasks[LayerIndex].Domain = ELayerMaskDomain::Corner;
		}
		else if (Layer.Mask->GeneratorType == EVertexMaskForgeGeneratorType::Curvature)
		{
			const FVertexMaskForgeCurvatureParams* CurvatureParams = Layer.Mask->Params.TryGet<FVertexMaskForgeCurvatureParams>();
			if (!CurvatureParams)
			{
				// Defensive -- mirrors the Material Slot/Bounding Box/Directional Normal coherence checks
				// above.
				return false;
			}

			// M16-K.6D-8E-B scope: Curvature has no Local/World-space concept and no ComponentTransform
			// dependency at all (confirmed directly from GenerateCurvatureMaskFromDynamicMesh's own
			// signature) -- unlike Bounding Box/Directional Normal, there is no space-rejection check
			// here. Every authoritative field (Type/Multiplier/Blur/LevelsMin/LevelsMax/bInvert) is
			// forwarded unchanged; none are normalized, swapped, or rewritten here -- the generator's own
			// ApplyCurvatureArtisticParams already clamps/defends each field internally (e.g.
			// LevelsMin<=LevelsMax is never enforced by this orchestrator, since ApplyCurvatureLevels'
			// own Max(LevelsMax-LevelsMin, Epsilon) denominator already makes any stored relationship
			// well-defined).
			//
			// CurvatureLocalGeneratorState (declared once, above Pass 1) is reused here across every
			// enabled Curvature layer in THIS call -- see its own declaration comment for the full
			// within-call-sharing/no-cross-call-persistence contract.
			//
			// GenerateCurvatureMaskFromDynamicMesh returns its FVertexMaskForgeScalarMask entirely BY
			// VALUE (its own Values/bHasValue arrays are freshly allocated inside that function every
			// call, from ApplyCurvatureArtisticParams' own freshly-returned-by-value TArray<float> --
			// neither is ever aliased to CurvatureLocalGeneratorState's cached raw arrays) -- so the
			// MoveTemp below is always safe: a later Curvature layer's call can never retroactively
			// mutate an earlier layer's already-stored completed mask.
			FVertexMaskForgeScalarMask GeneratedMask = VertexMaskForgeCurvatureGenerator::GenerateCurvatureMaskFromDynamicMesh(
				WorkingMesh, CurvatureLocalGeneratorState, CurvatureParams->Type, CurvatureParams->Multiplier,
				CurvatureParams->Blur, CurvatureParams->LevelsMin, CurvatureParams->LevelsMax, CurvatureParams->bInvert);
			if (GeneratedMask.State != EVertexMaskForgeScalarMaskState::Ready)
			{
				// Mirrors Bounding Box's own check exactly -- Curvature's output is DynamicMeshVertex-
				// domain and sparse-safe (resolved only via TryGetValue in Pass 2 below), so no separate
				// cardinality check is required here, matching
				// GenerateBoundingBoxMaskFromDynamicMesh's own established precedent.
				return false;
			}

			LayerMasks[LayerIndex].Mask = MoveTemp(GeneratedMask);
			LayerMasks[LayerIndex].Domain = ELayerMaskDomain::DynamicMeshVertex;

			if (!bCornerToVertexIDBuilt)
			{
				CornerToVertexID.SetNumUninitialized(ExpectedCornerCount);
				int32 RunningCornerIndex = 0;
				for (const int32 TriangleID : WorkingMesh.Mesh->TriangleIndicesItr())
				{
					const UE::Geometry::FIndex3i Tri = WorkingMesh.Mesh->GetTriangle(TriangleID);
					CornerToVertexID[RunningCornerIndex++] = Tri.A;
					CornerToVertexID[RunningCornerIndex++] = Tri.B;
					CornerToVertexID[RunningCornerIndex++] = Tri.C;
				}
				bCornerToVertexIDBuilt = true;
			}
		}
		else if (Layer.Mask->GeneratorType == EVertexMaskForgeGeneratorType::Noise)
		{
			const FVertexMaskForgeNoiseParams* NoiseParams = Layer.Mask->Params.TryGet<FVertexMaskForgeNoiseParams>();
			if (!NoiseParams)
			{
				// Defensive -- mirrors the Material Slot/Bounding Box/Directional Normal/Curvature
				// coherence checks above.
				return false;
			}

			// M16-K.6D-8F-B scope: Noise has no Local/World-space concept and no ComponentTransform
			// dependency at all (confirmed directly from GenerateNoiseMaskFromDynamicMesh's own signature)
			// -- no space-rejection check here, mirroring Curvature's own precedent. Every authoritative
			// field is forwarded unchanged; none are normalized, swapped, or rewritten here.
			//
			// UNLIKE Curvature's single shared CurvatureLocalGeneratorState (reused across every enabled
			// Curvature layer in one call, safe because Curvature's raw-cache key is geometry-only), Noise's
			// own raw-cache key additionally includes its full FVertexMaskForgeNoiseGenerativeParams (see
			// that type's own operator==) -- two Noise layers commonly differ in Type/Scale/Offset/Seed/
			// Octaves/Roughness/Lacunarity/TurbulenceStrength/Blur, so sharing one state across them would
			// merely thrash (each layer's own call would invalidate the previous layer's cached raw pattern
			// via that struct's own field-by-field comparison) rather than ever hit a genuine cache reuse.
			// Accordingly, this branch constructs a NEW, INDEPENDENT, function-local
			// FVertexMaskForgeGeneratorState for THIS Noise layer only -- never shared with another Noise
			// layer, never the Legacy per-entry FVertexMaskForgeGeneratorState (never reachable from this
			// orchestrator), never static/persistent, never exposed through this function's own public
			// signature, and never retained beyond this single layer's evaluation (destroyed at the end of
			// this loop iteration). This is a deliberate design difference from Curvature, not a claim that
			// Noise is cheaper or more/less expensive to evaluate -- no comparative performance measurement
			// was made.
			FVertexMaskForgeGeneratorState NoiseLocalGeneratorState;

			FVertexMaskForgeNoiseGenerativeParams GenerativeParams;
			GenerativeParams.NoiseType = NoiseParams->Type;
			GenerativeParams.ScaleX = NoiseParams->ScaleX;
			GenerativeParams.ScaleY = NoiseParams->ScaleY;
			GenerativeParams.ScaleZ = NoiseParams->ScaleZ;
			GenerativeParams.OffsetX = NoiseParams->OffsetX;
			GenerativeParams.OffsetY = NoiseParams->OffsetY;
			GenerativeParams.OffsetZ = NoiseParams->OffsetZ;
			GenerativeParams.Seed = NoiseParams->Seed;
			GenerativeParams.Octaves = NoiseParams->Octaves;
			GenerativeParams.Roughness = NoiseParams->Roughness;
			GenerativeParams.Lacunarity = NoiseParams->Lacunarity;
			GenerativeParams.TurbulenceStrength = NoiseParams->TurbulenceStrength;
			GenerativeParams.Blur = NoiseParams->Blur;

			// GenerateNoiseMaskFromDynamicMesh returns its FVertexMaskForgeScalarMask entirely BY VALUE
			// (freshly allocated inside that function every call, from ApplyNoiseArtisticParams' own
			// freshly-returned-by-value TArray<float> -- never aliased to NoiseLocalGeneratorState's own
			// cached raw array) -- so the MoveTemp below is always safe, exactly like Curvature's own
			// established precedent.
			FVertexMaskForgeScalarMask GeneratedMask = VertexMaskForgeNoiseGenerator::GenerateNoiseMaskFromDynamicMesh(
				WorkingMesh, NoiseLocalGeneratorState, GenerativeParams, NoiseParams->Multiplier,
				NoiseParams->LevelsMin, NoiseParams->LevelsMax, NoiseParams->bInvert);
			if (GeneratedMask.State != EVertexMaskForgeScalarMaskState::Ready)
			{
				// Mirrors Curvature's/Bounding Box's own check exactly -- Noise's output is
				// DynamicMeshVertex-domain and sparse-safe (resolved only via TryGetValue in Pass 2 below),
				// so no separate cardinality check is required here.
				return false;
			}

			LayerMasks[LayerIndex].Mask = MoveTemp(GeneratedMask);
			LayerMasks[LayerIndex].Domain = ELayerMaskDomain::DynamicMeshVertex;

			if (!bCornerToVertexIDBuilt)
			{
				CornerToVertexID.SetNumUninitialized(ExpectedCornerCount);
				int32 RunningCornerIndex = 0;
				for (const int32 TriangleID : WorkingMesh.Mesh->TriangleIndicesItr())
				{
					const UE::Geometry::FIndex3i Tri = WorkingMesh.Mesh->GetTriangle(TriangleID);
					CornerToVertexID[RunningCornerIndex++] = Tri.A;
					CornerToVertexID[RunningCornerIndex++] = Tri.B;
					CornerToVertexID[RunningCornerIndex++] = Tri.C;
				}
				bCornerToVertexIDBuilt = true;
			}
		}
		else if (Layer.Mask->GeneratorType == EVertexMaskForgeGeneratorType::AmbientOcclusion)
		{
			const FVertexMaskForgeAmbientOcclusionParams* AOParams = Layer.Mask->Params.TryGet<FVertexMaskForgeAmbientOcclusionParams>();
			if (!AOParams)
			{
				// Defensive -- mirrors the Material Slot/Bounding Box/Directional Normal/Curvature/Noise
				// coherence checks above.
				return false;
			}

			// M16-K.6D-8G-D scope: Ambient Occlusion is unconditionally World-Space (confirmed directly
			// from GenerateAmbientOcclusionMaskFromDynamicMesh's own doc comment) -- unlike Bounding Box/
			// Directional Normal, it has no Local-space mode to restrict to at all, so no space-rejection
			// check exists here; ComponentTransform (threaded through since M16-K.6D-8G-B, unused by
			// every other generator) is exactly what this branch requires and is passed unmodified.
			//
			// FVertexMaskForgeAOParams (the generator's own raw-input struct, VertexMaskForgeWorkingMeshTypes.h)
			// and FVertexMaskForgeAmbientOcclusionParams (this layer's own Dynamic recipe payload,
			// VertexMaskForgeRecipeTypes.h) are deliberately two distinct types (see the recipe struct's
			// own doc comment: FVertexMaskForgeAmbientOcclusionParams's defaults intentionally diverge from
			// FVertexMaskForgeAOParams's) but share an identical field set -- translated 1:1 here, exactly
			// mirroring how SVertexMaskForgePanel::ApplyPreviewToEntry's own Legacy AO branch builds its
			// own EffectiveAOParams from the panel's separate UI state fields.
			FVertexMaskForgeAOParams RawParams;
			RawParams.Samples = AOParams->Samples;
			RawParams.MaxDistance = AOParams->MaxDistance;
			RawParams.Bias = AOParams->Bias;
			RawParams.bInvert = AOParams->bInvert;
			RawParams.LevelsMin = AOParams->LevelsMin;
			RawParams.LevelsMax = AOParams->LevelsMax;

			// M16-K.6D-8G-D: resolve THIS layer's own persistent cache entry from the caller-supplied,
			// per-component map established by M16-K.6D-8G-C (Model D -- one complete, independent cache
			// per component and stable Dynamic LayerId; tree sharing deliberately deferred). FindOrAdd
			// creates a fresh, default-constructed (cold/invalid) entry on a genuine first use for this
			// LayerId -- GenerateAmbientOcclusionMaskFromDynamicMesh's own internal freshness comparison
			// (Tree layer keyed on mesh/fingerprint/transform; RawValues layer keyed additionally on
			// Samples/MaxDistance/Bias) then correctly treats that as a cold cache and performs a real
			// raycast pass, exactly as it would for a genuinely first-ever null-TUniquePtr call.
			FVertexMaskForgeSourceTopologyAOCache& PersistentCacheEntry =
				DynamicSourceTopologyAOCachesByLayerId.FindOrAdd(Layer.LayerId);

			// ADAPTER (M16-K.6D-8G-D): GenerateAmbientOcclusionMaskFromDynamicMesh's own public interface
			// (never modified -- protected file) requires a TUniquePtr<FVertexMaskForgeSourceTopologyAOCache>&,
			// but Model D's own persistent ownership (M16-K.6D-8G-C) stores each cache BY VALUE, keyed by
			// LayerId, in a plain TMap -- reusing the generator verbatim (never a second AO algorithm,
			// never a duplicated/reshaped cache layout) requires bridging that one interface mismatch at
			// the narrowest possible boundary. This TUniquePtr is a call-scoped adapter only, never itself
			// the persisted state: PersistentCacheEntry's CURRENT content (cold on a genuine first use, or
			// whatever was retained from a prior call otherwise) is moved into it immediately before the
			// call, the generator mutates it in place exactly as it would any other caller's
			// TUniquePtr-owned cache, and the (possibly rebuilt) result is moved back into the SAME
			// persistent map entry immediately after -- so cross-call persistence (composition-only edits
			// reusing RawValues without rebuilding the tree, raw-parameter changes invalidating only
			// RawValues, geometry/transform changes invalidating both) is fully preserved through this map
			// exactly as if it were a native TUniquePtr field.
			TUniquePtr<FVertexMaskForgeSourceTopologyAOCache> TransientCachePtr =
				MakeUnique<FVertexMaskForgeSourceTopologyAOCache>(MoveTemp(PersistentCacheEntry));

			FVertexMaskForgeScalarMask GeneratedMask = VertexMaskForgeAmbientOcclusionGenerator::GenerateAmbientOcclusionMaskFromDynamicMesh(
				TransientCachePtr, *WorkingMesh.Mesh, WorkingMesh.GeometryFingerprint, ComponentTransform, RawParams);

			// Always valid after the call (the generator itself allocates on a null/first-use CachePtr,
			// confirmed from its own source, and never resets an already-valid one back to null) --
			// checked defensively rather than assumed, mirroring this codebase's own established
			// "never assumed, always checked" discipline for defensive branches.
			if (TransientCachePtr.IsValid())
			{
				PersistentCacheEntry = MoveTemp(*TransientCachePtr);
			}

			if (GeneratedMask.State != EVertexMaskForgeScalarMaskState::Ready)
			{
				// Mirrors Bounding Box's/Curvature's/Noise's own check exactly -- Ambient Occlusion's
				// output is Normal-Overlay-Element-ID domain and sparse-safe (resolved only via
				// TryGetValue in Pass 2 below), so no separate cardinality check is required here.
				return false;
			}

			LayerMasks[LayerIndex].Mask = MoveTemp(GeneratedMask);
			LayerMasks[LayerIndex].Domain = ELayerMaskDomain::NormalOverlayElement;

			if (!bCornerToElementIDBuilt)
			{
				// M16-K.6D-8G-D: mirrors CornerToVertexID's own lazy-build pattern exactly (see its own
				// declaration comment above), but resolves each corner's Normal Overlay ELEMENT ID
				// instead of its Dynamic Mesh VERTEX ID -- built here, only once
				// GenerateAmbientOcclusionMaskFromDynamicMesh has already confirmed (via its own Ready
				// state above) that WorkingMesh.Mesh has a valid Normal Overlay with at least one element,
				// so re-deriving the SAME overlay pointer here is guaranteed safe (never a second,
				// independent validity check reinventing the generator's own contract). Uses
				// NormalOverlay->GetTriangle(TriangleID)[Corner], the exact same lookup Legacy's own
				// UpdateWorkingColorsSourceTopology already uses for this generator (SVertexMaskForgePanel.
				// cpp) -- never a new/invented per-corner-normal resolution rule.
				const UE::Geometry::FDynamicMeshNormalOverlay* NormalOverlay = WorkingMesh.Mesh->Attributes()->PrimaryNormals();
				CornerToElementID.SetNumUninitialized(ExpectedCornerCount);
				int32 RunningCornerIndex = 0;
				for (const int32 TriangleID : WorkingMesh.Mesh->TriangleIndicesItr())
				{
					const UE::Geometry::FIndex3i TriElements = NormalOverlay->GetTriangle(TriangleID);
					CornerToElementID[RunningCornerIndex++] = TriElements.A;
					CornerToElementID[RunningCornerIndex++] = TriElements.B;
					CornerToElementID[RunningCornerIndex++] = TriElements.C;
				}
				bCornerToElementIDBuilt = true;
			}
		}
		else
		{
			// Explicit, whole-call failure -- any generator type beyond this checkpoint's own supported
			// set (Material Slot, Bounding Box Local-space, Directional Normal Local-space, Curvature,
			// Noise, Ambient Occlusion) never silently skips or treats itself as Fill-only.
			return false;
		}
	}

	// --- Pass 2: fold, per corner, strictly in Stack order, into a private local buffer -- OutComposedColors
	// is only ever touched by the final MoveTemp below, on success. ---
	TArray<FColor> LocalOutput;
	LocalOutput.SetNumUninitialized(ExpectedCornerCount);

	for (int32 CornerIndex = 0; CornerIndex < ExpectedCornerCount; ++CornerIndex)
	{
		const FVector4f BaseColor = VertexMaskForgeColorConversion::ToLinearColorF(BaseColors[CornerIndex]);
		FVector3f Composite(BaseColor.X, BaseColor.Y, BaseColor.Z);

		for (int32 LayerIndex = 0; LayerIndex < Layers.Num(); ++LayerIndex)
		{
			const FVertexMaskForgeLayer& Layer = Layers[LayerIndex];
			if (!Layer.bEnabled)
			{
				continue;
			}

			float FillValue = 0.0f;
			if (!VertexMaskForgeDynamicLayerEvaluator::TryResolveFillValue(Layer.Fill, FillValue))
			{
				continue;
			}

			// Layer.Mask unset -> EffectiveMask implicitly 1.0, exactly mirroring EvaluateColor's own
			// "ResultStore is NEVER consulted for such a layer" contract (here: neither generated mask
			// array is ever consulted either). Layer.Mask set -> Pass 1 already guaranteed a Ready,
			// domain-tagged LayerMasks[LayerIndex] for every ENABLED masked layer -- looked up according
			// to that mask's own recorded domain, never inferred from array length.
			float EffectiveMask = 1.0f;
			if (Layer.Mask.IsSet())
			{
				const FLayerGeneratedMask& GeneratedLayerMask = LayerMasks[LayerIndex];
				if (GeneratedLayerMask.Domain == ELayerMaskDomain::Corner)
				{
					EffectiveMask = GeneratedLayerMask.Mask.Values[CornerIndex];
				}
				else if (GeneratedLayerMask.Domain == ELayerMaskDomain::DynamicMeshVertex)
				{
					// DynamicMeshVertex domain (Bounding Box/Curvature/Noise) -- resolve this corner's own
					// underlying Dynamic Mesh VertexID, then read the mask ONLY through TryGetValue (never
					// a direct Values[VertexID] index), exactly per FVertexMaskForgeScalarMask's own
					// sparse-domain contract. A miss here means the corner's VertexID was never written by
					// the generator -- stale/invalid topology -- and fails the WHOLE call safely rather
					// than substituting a guessed value; OutComposedColors is still untouched on this path
					// (LocalOutput is never assigned to it before this point).
					const int32 VertexID = CornerToVertexID[CornerIndex];
					if (!GeneratedLayerMask.Mask.TryGetValue(VertexID, EffectiveMask))
					{
						return false;
					}
				}
				else
				{
					// M16-K.6D-8G-D: NormalOverlayElement domain (Ambient Occlusion) -- resolve this
					// corner's own Normal Overlay Element ID via the CornerToElementID map built in Pass 1
					// (never Mesh->GetTriangle, never a Dynamic Mesh VertexID -- see
					// ELayerMaskDomain::NormalOverlayElement's own comment), then read the mask ONLY
					// through TryGetValue, mirroring the DynamicMeshVertex branch's own sparse-domain
					// contract exactly. A miss here (never expected once GenerateAmbientOcclusionMaskFrom
					// DynamicMesh has already reported Ready) fails the WHOLE call safely rather than
					// substituting a guessed value.
					const int32 ElementID = CornerToElementID[CornerIndex];
					if (!GeneratedLayerMask.Mask.TryGetValue(ElementID, EffectiveMask))
					{
						return false;
					}
				}
			}

			const FVector3f PaintValue = FVector3f(FillValue, FillValue, FillValue) * EffectiveMask;
			const FVector3f LayerOutput = VertexMaskForgeSequentialEvaluator::EvaluateFillLayerStep(Composite, PaintValue, Layer.BlendMode, Layer.Opacity);

			Composite.X = Layer.bAffectRed ? LayerOutput.X : Composite.X;
			Composite.Y = Layer.bAffectGreen ? LayerOutput.Y : Composite.Y;
			Composite.Z = Layer.bAffectBlue ? LayerOutput.Z : Composite.Z;
		}

		Composite.X = FMath::Clamp(Composite.X, 0.0f, 1.0f);
		Composite.Y = FMath::Clamp(Composite.Y, 0.0f, 1.0f);
		Composite.Z = FMath::Clamp(Composite.Z, 0.0f, 1.0f);

		const FVector4f FinalColor(Composite.X, Composite.Y, Composite.Z, BaseColor.W);
		LocalOutput[CornerIndex] = VertexMaskForgeColorConversion::ToDisplayFColor(FinalColor);
	}

	OutComposedColors = MoveTemp(LocalOutput);
	return true;
}
