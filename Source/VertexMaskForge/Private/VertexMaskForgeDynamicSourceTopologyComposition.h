#pragma once

#include "Containers/Array.h"
#include "Containers/ArrayView.h"
#include "Math/Color.h"

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
	 * single final Clamp01, Alpha passthrough from BaseColors) exactly, without ever constructing or
	 * touching a FVertexMaskForgeInstanceResultStore.
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
	 */
	bool ComputeComposedColorsRGBSourceTopology(
		const FVertexMaskForgeWorkingMesh& WorkingMesh,
		const FVertexMaskForgeDynamicLayerStack& Stack,
		TConstArrayView<FColor> BaseColors,
		TArray<FColor>& OutComposedColors);
}
