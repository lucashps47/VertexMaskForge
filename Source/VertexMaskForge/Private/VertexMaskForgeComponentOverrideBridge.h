#pragma once

#include "CoreMinimal.h"

class UStaticMeshComponent;
struct FVertexMaskForgeSelectedMesh;

/**
 * M20-E -- restoration, from behavioral evidence only (no original M20-C source bytes survive; see the
 * M20-E final report's "M20-C Recovery" section for exactly what evidence this reconstruction relies on
 * and where it necessarily differs from a hypothetical byte-exact restoration), of the interim "Send to
 * Mesh Paint Texture" transient bridge first implemented by M20-C and later deleted (untracked, never
 * committed) by M20-D in favor of the since-abandoned persistent Instance Override route
 * (VertexMaskForgeInstanceOverrideWriter, itself fully removed by this same M20-E checkpoint after
 * M20-D.1 found no exact, engine-backed Source-Topology-to-render-vertex mapping exists for it).
 *
 * Narrow, explicit contract: transfers vertex colors that have ALREADY been consolidated into the
 * Static Mesh asset by the normal Forge Accept route (VertexMaskForgeAcceptWriter) -- read verbatim
 * from the rebuilt LOD 0 FStaticMeshLODResources::VertexBuffers.ColorVertexBuffer, NEVER from Forge's
 * live, possibly-unaccepted WorkingColors -- into a component's persistent OverrideVertexColors ONLY
 * for the synchronous duration of one native "From Vertex" bake
 * (UMeshPaintModeSubsystem::ImportMeshPaintTextureFromVertexColors, MeshPaintEditorMode module,
 * grounded directly against installed UE 5.8 engine source at
 * Engine/Plugins/MeshPainting/Source/MeshPaintEditorMode/Private/MeshPaintModeHelpers.cpp), then
 * restores the component's exact prior override state (presence, byte-for-byte RGBA, PaintedVertices)
 * before returning, on success OR failure. Never modifies, builds, or dirties the Static Mesh asset.
 * Never leaves a persistent Forge-owned override behind. Never toggles Nanite. Never shares one
 * FColorVertexBuffer between two components.
 *
 * Why a temporary override is needed even though the accepted colors already live on the Static Mesh
 * asset itself: ImportMeshPaintTextureFromVertexColors sources its bake from the component's existing
 * per-instance OverrideVertexColors when present, and falls back to the asset's own baked LOD 0 colors
 * only when the component has none (confirmed directly against the installed engine implementation,
 * which passes `bHasPerInstanceVertexColors` and, only when true, a lambda reading
 * `InstanceMeshLODInfo->OverrideVertexColors->VertexColor(Index)` into
 * FStaticMeshLODResourcesToDynamicMesh::Convert). A component with a pre-existing Legacy Vertex Paint
 * override would therefore have that STALE data baked instead of the freshly-accepted Forge result
 * unless this bridge temporarily substitutes the accepted colors first -- which is exactly what it
 * does, then undoes.
 *
 * The native import itself creates (or replaces) the component's own Mesh Paint Texture -- confirmed
 * directly against UMeshPaintModeSubsystem::ImportMeshPaintTextureFromVertexColors's installed
 * implementation, which always calls `NewObject<UMeshPaintVirtualTexture>(StaticMeshComponent)` and
 * `StaticMeshComponent->SetMeshPaintTexture(NewTexture)` once it reaches the end of the function,
 * regardless of whether a Mesh Paint Texture already existed -- so this bridge does not itself require
 * a pre-existing Mesh Paint Texture asset. It DOES require the component/asset's own Mesh Paint Texture
 * *support* to already be enabled -- UStaticMeshComponent::CanMeshPaintTextureColors() (bSupportMeshPainting,
 * bEnableTextureColorMeshPainting, UStaticMesh::CanMeshPaintTextureColors(), and platform Virtual
 * Texture support) -- which this bridge validates but never changes.
 */
namespace VertexMaskForgeComponentOverrideBridge
{
	/** One eligible component targeted by a Send-to-Mesh-Paint-Texture transfer, with the ALREADY-
	 *  ACCEPTED, render-vertex-domain colors read verbatim from its Static Mesh's rebuilt LOD 0
	 *  ColorVertexBuffer. */
	struct FTransferTarget
	{
		TWeakObjectPtr<UStaticMeshComponent> Component;
		FString AssetName;
		/** Render-vertex-order (LOD0) colors, read verbatim from the accepted Static Mesh asset's own
		 *  rebuilt ColorVertexBuffer -- never Forge's live WorkingColors, never re-derived here. */
		TArray<FColor> AcceptedColors;
	};

	/**
	 * Validates every selected component eligible for this transfer and returns the exact set of
	 * {Component, accepted render-vertex-domain colors} to bake. Nothing is written here -- pure
	 * validation + gathering, matching VertexMaskForgeAcceptTargetBuilder/
	 * VertexMaskForgeInstanceOverrideWriter's own validate-before-mutate discipline.
	 *
	 * Eligible: the entry's SourceComponent resolves and is valid; its Static Mesh resolves, has valid
	 * LOD 0 render data, and that LOD 0's ColorVertexBuffer is non-empty. This last check is a genuine
	 * structural sanity guard, NOT a reliable "was this asset ever Accepted" detector -- confirmed
	 * directly against the installed UE 5.8 build path (UStaticMesh::BuildFromMeshDescriptions via
	 * FStaticMeshAttributes::Register(), the same path this module's own test fixtures use): a real
	 * Static Mesh LOD 0's ColorVertexBuffer is essentially NEVER actually empty after a real build --
	 * an asset that was never explicitly painted/Accepted still ends up with a non-empty, uniform-white
	 * buffer, not a zero-vertex one. This function therefore cannot distinguish "genuinely accepted
	 * colors" from "default white" -- see Phase 5 of the M20-E checkpoint spec on why an expensive
	 * comparison against current WorkingColors is deliberately NOT attempted as a substitute, and the
	 * M20-E final report's own Color-Domain Correctness section for the full, honest accounting of this
	 * limitation. UStaticMeshComponent::
	 * CanMeshPaintTextureColors() is true for that component. Source-Topology (Nanite) entries are NOT
	 * excluded by domain the way VertexMaskForgeInstanceOverrideWriter excluded them: this bridge reads
	 * the asset's already-built render-vertex ColorVertexBuffer, which exists identically for Nanite and
	 * non-Nanite assets after a successful Accept -- Nanite's WedgeMap-unavailability constraint, which
	 * blocks a DIFFERENT, render-vertex Accept-domain operation elsewhere in this plugin, is not
	 * relevant to reading an already-built buffer (see the M20-E final report's Color-Domain
	 * Correctness section for the full evidence chain).
	 */
	bool BuildTransferTargets(
		const TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& SelectedMeshes,
		TArray<FTransferTarget>& OutTargets,
		TArray<FText>& OutIneligibleReasons,
		FText& OutErrorText);

	/**
	 * For each target, independently: captures the component's exact current LOD 0 override state
	 * (presence, byte-for-byte RGBA, PaintedVertices), installs a temporary LOD 0 OverrideVertexColors
	 * buffer built from Target.AcceptedColors (CPU-only -- FColorVertexBuffer::InitFromColorArray's own
	 * CPU-side array is all the native "From Vertex" reader touches; no BeginInitResource/render-thread
	 * initialization is performed, matching the fact that the component is kept unregistered for the
	 * whole operation via FComponentReregisterContext so nothing ever samples the temporary buffer on
	 * the GPU), invokes UMeshPaintModeSubsystem::ImportMeshPaintTextureFromVertexColors, then restores
	 * the captured state -- unconditionally, on success OR failure, before returning.
	 *
	 * Never opens its own FScopedTransaction -- the caller owns one transaction spanning the whole
	 * Accept-then-transfer sequence, matching VertexMaskForgeAcceptWriter/VertexMaskForgeInstanceOverrideWriter's
	 * own caller-owns-the-transaction contract. This function's own mutations do not themselves need
	 * Undo/Redo capability, since nothing here is meant to persist past this call -- restoration is
	 * unconditional and synchronous, not Undo-dependent (Component->Modify() is still called before any
	 * mutation, so the transient state change participates correctly if the caller's outer transaction
	 * is later undone mid-operation, e.g. by a crash/reentrancy edge case).
	 *
	 * Requires GEditor (Editor-only). Returns false, with OutErrorText populated, for any target whose
	 * native import step could not be confirmed to have completed (see the .cpp's own
	 * DidImportMeshPaintTextureSucceed for exactly how success is detected -- ImportMeshPaintTextureFromVertexColors
	 * itself returns void). Restoration always runs for every target regardless of the returned bool.
	 *
	 * InNativeImportOverride is a test-only injection seam, default-empty (unbound TFunction) at every
	 * production call site: when unbound, production behavior is unchanged -- the real
	 * UMeshPaintModeSubsystem::ImportMeshPaintTextureFromVertexColors is invoked exactly as documented
	 * above. The real native bake touches the GPU/asset-baking pipeline and is not safe to run inside
	 * an automation fixture (per this checkpoint's own Phase 8 instruction not to invent a test-only
	 * reimplementation of production behavior); tests that need to prove the capture/install/restore
	 * contract instead bind a lightweight callable here (return true/false to simulate native
	 * success/failure), exercising the exact same production capture/install/restore code around it,
	 * up to -- but not including -- the real native call.
	 */
	bool TransferToMeshPaintTexture(
		const TArray<FTransferTarget>& Targets,
		TArray<UStaticMeshComponent*>& OutSucceededComponents,
		FText& OutErrorText,
		TFunction<bool(UStaticMeshComponent&)> InNativeImportOverride = nullptr);
}
