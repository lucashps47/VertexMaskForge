#pragma once

#include "CoreMinimal.h"
#include "Math/Color.h"
#include "MeshTypes.h"

struct FVertexMaskForgeSelectedMesh;
struct FVertexMaskForgePreviewComponentState;
enum class EVertexMaskForgeNormalSpace : uint8;
namespace UE::Geometry { class FDynamicMesh3; }
class UStaticMesh;
struct FMeshDescription;

namespace VertexMaskForgeAcceptTargetBuilder
{
	/** One Static Mesh asset targeted by an Accept operation, with its final, ready-to-write colors. */
	struct FAcceptTarget
	{
		TWeakObjectPtr<UStaticMesh> Mesh;
		FString AssetName;
		/** Render-vertex-order (LOD0), exactly as shown in Preview -- the data actually written. */
		TArray<FColor> FinalColors;
		/** AUDITED (V2-G): needed so WriteAcceptTargets can re-validate Thickness Mask freshness
		 *  (Entry->WorkingMesh.ThicknessCache) immediately before the first Modify() -- see
		 *  AreThicknessGeometrySnapshotsExactlyEquivalent. Null-safe: only dereferenced when
		 *  Entry->WorkingMesh.ThicknessMask.State==Ready AND ThicknessCache is valid. */
		TSharedPtr<FVertexMaskForgeSelectedMesh> Entry;
	};

	/**
	 * AUDITED (Nanite source-topology support): sibling of FAcceptTarget/BuildAcceptTargets/
	 * WriteAcceptTargets for Source-Topology entries (every Nanite-enabled mesh -- see
	 * FVertexMaskForgeSelectedMesh::bUseSourceTopology). Same validate-then-write, all-or-nothing
	 * contract; same divergent-per-instance-baseline blocking rule; same shared-asset dedup (one
	 * target per entry, entries are already 1-per-asset by construction). The only structural
	 * difference: colors are in CORNER domain (see UpdateWorkingColorsSourceTopology), and the commit
	 * itself writes via the TriangleID+corner correspondence (Entry->WorkingMesh.TriIDMap) instead of
	 * FStaticMeshLODResources::WedgeMap -- exactly the route the native UE Paint Vertex Colors tool
	 * uses (FDynamicMeshToMeshDescription::UpdateVertexColors), proven correct for Nanite by the
	 * native-tool audit. Entry is kept alive (TSharedPtr) so WorkingMesh.Mesh/TriIDMap remain valid
	 * from preflight through the write pass.
	 */
	struct FSourceTopologyAcceptTarget
	{
		TWeakObjectPtr<UStaticMesh> Mesh;
		FString AssetName;
		/** Corner-domain colors (Entry->WorkingMesh.Mesh's own TriangleIndicesItr()+corner order),
		 *  exactly as shown in Preview -- the data actually written. */
		TArray<FColor> FinalColors;
		TSharedPtr<FVertexMaskForgeSelectedMesh> Entry;
	};

	/**
	 * AUDITED (Nanite source-topology support, commit preflight correction): full formal validation of
	 * the TriangleID -> source FTriangleID -> VertexInstanceID correspondence a Source-Topology Accept
	 * is about to rely on, BEFORE any write. Checks:
	 *   - TriIDMap has a VALID entry for every triangle actually enumerated by WorkingMesh's own
	 *     TriangleIndicesItr() (not just "non-empty") -- a partially-populated map is refused outright,
	 *     never silently skipped mid-write the way the write loop's own defensive guards do (those
	 *     exist as a last-resort safety net, not as the intended detection point).
	 *   - Each mapped FTriangleID still exists in the LIVE MeshDescription
	 *     (MeshDescription.IsTriangleValid) -- catches a stale TriIDMap if the asset's MeshDescription
	 *     was ever rebuilt/edited since WorkingMesh was built, without WorkingMesh itself being rebuilt.
	 *   - No two WorkingMesh triangles map to the SAME destination FTriangleID (TSet-based uniqueness) --
	 *     two different corners silently writing into the same three VertexInstanceIDs would corrupt the
	 *     write (last-write-wins on a shared slot) without ever raising an error otherwise.
	 *   - Each destination triangle provides EXACTLY three VertexInstanceIDs, each itself still valid in
	 *     the live MeshDescription (MeshDescription.IsVertexInstanceValid).
	 * The corner ORDER itself is not a runtime check here -- it is a structural invariant: both
	 * composition (UpdateWorkingColorsSourceTopology) and the write (WriteSourceTopologyAcceptTargets)
	 * iterate the EXACT SAME Mesh.TriangleIndicesItr() + corner 0..2 sequence, so corner i of a given
	 * TriangleID always means the same thing in both places by construction, not by a value that could
	 * silently drift between them.
	 */
	bool ValidateSourceTopologyCorrespondence(
		const UE::Geometry::FDynamicMesh3& Mesh,
		const TArray<FTriangleID>& TriIDMap,
		const FMeshDescription& MeshDescription,
		const FString& AssetName,
		FText& OutErrorText);

	/**
	 * Validates every SelectedMeshes entry eligible for Accept and, only if ALL of them pass,
	 * returns the exact set of {Static Mesh, final render-order colors} to write. Nothing is written
	 * here -- this function is pure validation + composition, so the caller can guarantee Accept is
	 * all-or-nothing (validate every destination before starting any write).
	 *
	 * An entry is eligible when it has a Ready mask AND at least one live PreviewComponent (Content-
	 * Browser-only entries, with no viewport component, are never eligible -- there is no baseline to
	 * compose from). For an eligible entry:
	 *   - composes render-order colors independently PER SourceComponent, using the same baseline
	 *     priority as the live Preview (per-instance OverrideVertexColors, then asset colors, then
	 *     white -- see UpdateWorkingColors);
	 *   - entries are already 1-per-asset by construction (AddOrUpdateSelectedMesh keys InOutMeshes
	 *     by asset path), so no separate cross-entry dedup by UStaticMesh identity is needed here --
	 *     but if two or more COMPONENTS of that one entry's asset produce DIFFERENT composed results
	 *     (because their per-instance baselines differ), the write target is AMBIGUOUS: Accept is
	 *     blocked for the whole operation (not silently narrowed to one component), the Preview is
	 *     left untouched, and OutErrorText names the asset;
	 *   - otherwise, validates that FStaticMeshLODResources::WedgeMap (the engine's own deterministic
	 *     wedge->render-vertex mapping, exactly as used by UMeshPaintingSubsystem::
	 *     PropagateColorsToRawMesh) is present and matches the current MeshDescription's vertex
	 *     instance count. If it does not, this function refuses -- it NEVER falls back to an
	 *     approximate position-based remap, per the explicit architectural requirement (a stale/
	 *     missing WedgeMap most commonly means the asset needs a Build first).
	 *
	 * AUDITED (recompute-at-Accept fix): this function NEVER calls UpdateWorkingColors, NEVER
	 * re-evaluates GenerateBoundingBoxMask, and NEVER reads BoundingBoxBlendMode/BoundingBoxOpacity/
	 * the Channel Filter -- it only READS each component's State.WorkingColors, exactly as
	 * live regeneration last left it (see UpdateWorkingColors' own doc comment;
	 * ApplyPreviewToEntry is the only other caller, and it is the SAME array). This is what
	 * structurally guarantees Accept can never silently apply a parameter change that was never
	 * actually generated: every parameter change either invalidates the mask (forcing OperationState
	 * back to Idle, which disables Accept entirely until live regeneration re-populates
	 * WorkingColors) or itself synchronously updates WorkingColors before Accept can ever see it
	 * (Channel Filter). A component whose
	 * WorkingColors is empty (never composed yet, or reset by RestoreComponentOriginal because its
	 * own per-instance World Space mask evaluation was degenerate) is skipped -- the exact same
	 * outcome the old per-component mask re-evaluation produced for that case, without needing to
	 * redo the evaluation here.
	 *
	 * AUDITED (Preview-Mode-cannot-affect-Accept fix): takes NO EVertexMaskForgePreviewMode parameter
	 * at all, and reads State.WorkingColors VERBATIM -- never through DeriveDisplayColors -- so the
	 * persisted result is the same real multi-channel RGB regardless of which Preview Mode (Original
	 * Material, Full RGB, Red/Green/Blue Channel) happens to be selected when Accept runs.
	 * RecomputeOperationState() computes PendingChanges identically regardless of CurrentPreviewMode
	 * (see its own doc comment), so this function needs no Preview-Mode check of its own either.
	 */
	bool BuildAcceptTargets(
		const TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& SelectedMeshes,
		bool bDirectionalNormalMaskEnabled,
		EVertexMaskForgeNormalSpace DirectionalNormalSpace,
		TArray<FAcceptTarget>& OutTargets,
		FText& OutErrorText);

	/**
	 * Sibling of BuildAcceptTargets for Source-Topology entries. Only ever produces targets for
	 * entries with bUseSourceTopology == true; BuildAcceptTargets skips those entries entirely (see
	 * its own doc comment), so the two functions' outputs never overlap for the same asset.
	 */
	bool BuildSourceTopologyAcceptTargets(
		const TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& SelectedMeshes,
		bool bDirectionalNormalMaskEnabled,
		EVertexMaskForgeNormalSpace DirectionalNormalSpace,
		TArray<FSourceTopologyAcceptTarget>& OutTargets,
		FText& OutErrorText);
}
