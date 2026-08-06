#pragma once

#include "Containers/Array.h"
#include "Containers/ArrayView.h"
#include "Containers/Map.h"
#include "Math/Color.h"
#include "Misc/Guid.h"
#include "VertexMaskForgeWorkingMeshTypes.h"

struct FVertexMaskForgeWorkingMesh;
class FVertexMaskForgeDynamicLayerStack;

/**
 * M16-K.6D-4: the testable Dynamic Source-Topology composition orchestrator -- computes, in fully
 * caller-owned memory, the semantically composed RGB colors a Dynamic Layer Stack produces over a
 * Source-Topology (triangle-corner) domain, using ONLY the Material Slot caller-owned API confirmed by
 * M16-K.6D-3 (VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskFromDynamicMesh) -- never any
 * stored wrapper, never FVertexMaskForgeInstanceResultStore.
 *
 * WHY A NEW NAMESPACE/FILE, NOT VertexMaskForgePanel::ComputeComposedColorsRGBSourceTopology: that name
 * already exists (VertexMaskForgeMaskTypes.h / SVertexMaskForgePanel.cpp) for the unrelated, fixed
 * 7-generator LEGACY composition path (FVertexMaskForgeMaskLayerParams-based, render-vertex/source-
 * topology agnostic). M16-K.6D-4 explicitly reserves the SAME function name for the NEW Dynamic
 * orchestrator, so this module gives it its own namespace/translation unit entirely outside
 * SVertexMaskForgePanel.cpp -- there is no ambiguity at any call site because the two functions live in
 * two different namespaces, and this new namespace has (as of this checkpoint) no production caller of
 * any kind (see below), structurally guaranteeing it cannot be confused with or silently substituted for
 * the Legacy function.
 *
 * WHY NOT VertexMaskForgeDynamicLayerEvaluator::EvaluateColor / VertexMaskForgeDynamicLayerBatchCompositor
 * ::ComposeColors: both existing Dynamic composition primitives require a FVertexMaskForgeInstanceResultStore
 * to resolve a masked layer's EffectiveMask by MaskInstanceId -- explicitly forbidden for this checkpoint's
 * orchestrator (see the .cpp's own module comment for the full isolation rationale). This module instead
 * reuses the exact same UNDERLYING blend-mode primitive those two do
 * (VertexMaskForgeSequentialEvaluator::EvaluateFillLayerStep, itself a thin wrapper over
 * VertexMaskForgeMaskStackComposer::BlendMaskValueUnclamped) and the exact same Fill-resolution primitive
 * (VertexMaskForgeDynamicLayerEvaluator::TryResolveFillValue) -- zero new blend-mode or Fill math -- but
 * folds per corner using a locally, freshly generated Material Slot scalar array instead of a store lookup.
 *
 * Isolation (structural, not a promise -- see the .cpp for how each is actually satisfied): no
 * SVertexMaskForgePanel reference of any kind; no FVertexMaskForgeInstanceResultStore/WorkingMesh.
 * InstanceResults; no WorkingColors/SourceTopologyWorkingColors/CommittedColors/BaselineColors of any
 * kind; no ApplyComposedColorsRGB/ApplySuppliedSourceTopologyPreviewColors/
 * DeriveValidatedSourceTopologyPreviewColors/DeriveDisplayColors/BuildAcceptTargets; no
 * CurrentPreviewMode/PreviewSource; no preview component/UDynamicMeshComponent mutation of any kind; no
 * transaction/Undo/Redo; no cache/generation-counter/staleness policy of any kind.
 *
 * Not connected to preview, to the K.6D-2 visual seam, or to EVertexMaskForgePreviewSource -- that is
 * M16-K.6D-5's own, explicitly future, scope. As of this checkpoint this function's only callers are its
 * own Automation tests.
 */
namespace VertexMaskForgeDynamicSourceTopologyComposition
{
	/**
	 * Computes one caller-owned FColor per Source-Topology corner (Mesh.TriangleCount() * 3), by folding
	 * Stack's layers, strictly in Stack.GetLayers() array order, over BaseColors -- mirroring
	 * VertexMaskForgeDynamicLayerEvaluator::EvaluateColor's own per-layer semantics (Fill resolution,
	 * masked-layer EffectiveMask multiplication, BlendMode/Opacity fold, per-layer Channel Filter,
	 * single final Clamp01) exactly, without ever constructing or touching a
	 * FVertexMaskForgeInstanceResultStore.
	 *
	 * M19-A: Alpha (the fourth channel) is composed through the exact same fold, gated by each layer's own
	 * bAffectAlpha (default false -- see FVertexMaskForgeLayer's own doc comment). A layer that does not
	 * affect Alpha leaves the composite's Alpha exactly as it already was (Base Alpha, if no earlier layer
	 * in the fold affected it); there is no separate Alpha evaluator, no Accept-side Alpha correction, and
	 * no Alpha-specific Fill Value/Blend Mode/Opacity/generator handling anywhere in this module.
	 *
	 * Validation (all-or-nothing -- either the WHOLE call succeeds with a complete, correct
	 * OutComposedColors, or it fails with OutComposedColors left completely UNTOUCHED, never partially
	 * written, never padded, never truncated):
	 *   - WorkingMesh.Mesh must be valid (non-null); otherwise fails.
	 *   - BaseColors.Num() must equal EXACTLY WorkingMesh.Mesh->TriangleCount() * 3 (the Source-Topology
	 *     corner count); any mismatch fails outright -- no resize, no partial composition.
	 *   - Every ENABLED layer that carries a Mask (TOptional set) must have
	 *     Mask->GeneratorType == EVertexMaskForgeGeneratorType::MaterialSlot,
	 *     EVertexMaskForgeGeneratorType::BoundingBox, or EVertexMaskForgeGeneratorType::DirectionalNormal
	 *     (M16-K.6D-8B/8D-B; any other generator type still fails the WHOLE call rather than being
	 *     silently skipped or treated as Fill-only).
	 *   - Material Slot: VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskFromDynamicMesh is
	 *     called directly (never GenerateMaterialSlotMaskInstanceResult, never
	 *     GenerateStoredResultForMaterialSlotInstance, never any stored wrapper) with that layer's own
	 *     FVertexMaskForgeMaterialSlotParams (SelectedSlotIndex/bInvert); the result must be
	 *     EVertexMaskForgeScalarMaskState::Ready with exactly the expected corner cardinality, or the
	 *     WHOLE call fails. This mask is already corner-domain.
	 *   - Bounding Box (M16-K.6D-8B, Local-space only): VertexMaskForgeBoundingBoxGenerator::
	 *     GenerateBoundingBoxMaskFromDynamicMesh is called directly with that layer's own
	 *     FVertexMaskForgeBoundingBoxParams::Axes, ONLY after confirming bUseUnifiedBounds is false AND
	 *     no enabled axis has bWorldSpace true -- either one fails the WHOLE call (World Space and
	 *     Unified Bounds are explicitly unsupported in this checkpoint: World Space needs a per-component
	 *     transform this orchestrator does not receive, Unified Bounds needs full-selection context this
	 *     orchestrator does not receive; neither is silently reinterpreted as something else). The
	 *     resulting mask is indexed by Dynamic Mesh VertexID, NOT corner -- resolved per corner via that
	 *     corner's own triangle (Mesh.GetTriangle(TriangleID)[Corner]) and read only through
	 *     FVertexMaskForgeScalarMask::TryGetValue (never a direct Values[] index), mirroring the same
	 *     per-generator domain distinction Legacy's own Source-Topology composition already establishes
	 *     for this generator -- see the .cpp for the exact resolution.
	 *   - Directional Normal (M16-K.6D-8D-B, Local-space only): VertexMaskForgeDirectionalNormalGenerator::
	 *     GenerateDirectionalNormalMaskFromDynamicMesh is called directly with that layer's own
	 *     FVertexMaskForgeDirectionalNormalParams (Direction/Angle/Falloff/Blur/bInvert unchanged), ONLY
	 *     after confirming Space == EVertexMaskForgeNormalSpace::Local -- World Space fails the WHOLE call
	 *     (needs a per-component transform this orchestrator does not receive; never silently reinterpreted
	 *     as Local). This mask is already corner-domain, like Material Slot's -- but unlike Material Slot's
	 *     own dense result, an individual corner may legitimately be left unwritten by the generator (a
	 *     missing Normal Overlay element or degenerate normal) while the mask as a whole still reports
	 *     Ready; every corner must have resolved a real value (NumValidValues == the expected corner count)
	 *     or the WHOLE call fails, since Pass 2's corner-domain read is a direct, unconditional index, never
	 *     a TryGetValue lookup.
	 *   - Ambient Occlusion (M16-K.6D-8G-D): VertexMaskForgeAmbientOcclusionGenerator::
	 *     GenerateAmbientOcclusionMaskFromDynamicMesh is called directly, using ComponentTransform (World
	 *     Space -- Ambient Occlusion has no Local-space mode at all, unlike Bounding Box/Directional
	 *     Normal, so no space-rejection check exists for it) and this layer's own persistent cache entry
	 *     (DynamicSourceTopologyAOCachesByLayerId.FindOrAdd(Layer.LayerId)). The resulting mask is indexed
	 *     by Normal Overlay Element ID, NOT corner and NOT Dynamic Mesh VertexID -- resolved per corner via
	 *     that corner's own triangle's Normal Overlay element (NormalOverlay->GetTriangle(TriangleID)[Corner],
	 *     the exact same lookup Legacy's own UpdateWorkingColorsSourceTopology already uses for this
	 *     generator) and read only through FVertexMaskForgeScalarMask::TryGetValue, mirroring Bounding
	 *     Box's own sparse-domain handling.
	 *   - Thickness (M17-TH-DL-B, Source Topology only): VertexMaskForgeThicknessGenerator::
	 *     GenerateThicknessMaskFromDynamicMesh is called directly (never a duplicated raycast/fallback/
	 *     confidence-gate implementation) with that layer's own FVertexMaskForgeThicknessParams
	 *     (MinThickness/MaxThickness/SearchDistance/Bias/Blur/bInvert, unchanged/unreinterpreted) and this
	 *     layer's own persistent cache entry (DynamicSourceTopologyThicknessCachesByLayerId.FindOrAdd(Layer.
	 *     LayerId)). This mask is already corner-domain, like Material Slot's and Directional Normal's --
	 *     and, like Directional Normal, an individual corner may legitimately be left unwritten (no opposing
	 *     surface found, even after the backend's own internal M9 fallback/gate); every corner must have
	 *     resolved a real value (NumValidValues == the expected corner count) or the WHOLE call fails, for
	 *     the identical reason Directional Normal's own doc comment above gives (Pass 2's corner-domain read
	 *     is a direct, unconditional index, never a TryGetValue lookup).
	 * A disabled layer (bEnabled == false) contributes nothing and is never validated for its Mask's
	 * GeneratorType, exactly mirroring EvaluateColor's own "bEnabled==false -> complete no-op" contract.
	 *
	 * An empty Stack (Stack.IsEmpty()) succeeds trivially: OutComposedColors becomes a byte-exact copy of
	 * BaseColors, matching EvaluateColor's own empty-stack passthrough contract exactly.
	 *
	 * Returns true iff composition succeeded and OutComposedColors now holds exactly
	 * WorkingMesh.Mesh->TriangleCount() * 3 caller-owned FColor entries. Returns false, with
	 * OutComposedColors left completely untouched (whatever it held before the call, unmodified), on any
	 * validation failure above.
	 *
	 * M16-K.6D-8G-B: ComponentTransform is the real transform of the specific UStaticMeshComponent whose
	 * source-topology colors are being evaluated (never FTransform::Identity from a production caller --
	 * see each call site's own doc comment for how it is resolved). Test callers exercising the five
	 * transform-independent generators (Material Slot, Bounding Box Local-space, Directional Normal
	 * Local-space, Curvature, Noise) must pass FTransform::Identity.
	 *
	 * M16-K.6D-8G-D: DynamicSourceTopologyAOCachesByLayerId is the SAME per-component, per-stable-LayerId
	 * persistent Ambient Occlusion cache storage established by M16-K.6D-8G-C
	 * (FVertexMaskForgePreviewComponentState::DynamicSourceTopologyAOCachesByLayerId) -- this parameter is
	 * that exact map, passed by the caller (never a fresh/local/temporary map, which would silently defeat
	 * Model D's cross-call persistence contract). An enabled Ambient Occlusion layer resolves its own
	 * cache entry from this map via FindOrAdd(Layer.LayerId), creating a fresh entry only on a genuine
	 * first use for that LayerId -- never keyed by MaskInstanceId, stack index, or any other identity. A
	 * caller with no Ambient Occlusion layers currently enabled may pass an empty map; it is never read
	 * or written in that case. This parameter is a plain, non-nullable reference because both real
	 * production callers always have a genuine, currently-valid owning FVertexMaskForgePreviewComponentState
	 * in scope at their own call site -- there is no legitimate "no component state" case to represent
	 * with a pointer.
	 *
	 * M17-TH-DL-B: DynamicSourceTopologyThicknessCachesByLayerId is the exact sibling of the AO parameter
	 * immediately above, same Model D ownership/lifecycle, for
	 * VertexMaskForgeThicknessGenerator::GenerateThicknessMaskFromDynamicMesh
	 * (FVertexMaskForgePreviewComponentState::DynamicSourceTopologyThicknessCachesByLayerId). Thickness is
	 * corner-domain (like Material Slot/Directional Normal, never Vertex-ID or Normal-Overlay-Element-ID
	 * domain like Bounding Box/Ambient Occlusion) -- see the .cpp's own Thickness branch for the exact
	 * NumValidValues==ExpectedCornerCount density requirement this implies for Pass 2's direct, unconditional
	 * Values[CornerIndex] read.
	 */
	bool ComputeComposedColorsRGBSourceTopology(
		const FVertexMaskForgeWorkingMesh& WorkingMesh,
		const FVertexMaskForgeDynamicLayerStack& Stack,
		TConstArrayView<FColor> BaseColors,
		const FTransform& ComponentTransform,
		TMap<FGuid, FVertexMaskForgeSourceTopologyAOCache>& DynamicSourceTopologyAOCachesByLayerId,
		TMap<FGuid, FVertexMaskForgeSourceTopologyThicknessCache>& DynamicSourceTopologyThicknessCachesByLayerId,
		TArray<FColor>& OutComposedColors);
}
