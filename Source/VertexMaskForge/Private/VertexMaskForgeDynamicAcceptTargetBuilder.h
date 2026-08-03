#pragma once

#include "CoreMinimal.h"
#include "VertexMaskForgeAcceptTargetBuilder.h"

struct FVertexMaskForgeSelectedMesh;
class FVertexMaskForgeDynamicLayerStack;

/**
 * M16-K.6D-7A: the non-panel Dynamic Source-Topology accept-target construction seam -- the READ/compose
 * half of a future Dynamic Accept, mirroring VertexMaskForgeAcceptTargetBuilder::BuildSourceTopologyAcceptTargets'
 * own shape (same output type, same all-or-nothing/per-component-conflict contract), but sourcing its
 * per-component result from the Dynamic pipeline instead of Legacy's own `WorkingColors`.
 *
 * WHY A SEPARATE BUILDER, NOT A MODIFICATION OF THE EXISTING ONE: ADR-011 (Docs/VertexMaskForgeDecisionLog.md)
 * requires the Dynamic composed result to remain structurally unreachable from
 * VertexMaskForgeAcceptTargetBuilder::BuildAcceptTargets/BuildSourceTopologyAcceptTargets -- both of those
 * read `FVertexMaskForgeWorkingStateOwner::GetWorkingColors()`/`GetSourceTopologyWorkingColors()`
 * verbatim, and the Dynamic pipeline is explicitly forbidden from ever writing either. This module never
 * reads WorkingColors/SourceTopologyWorkingColors/CommittedColors/BaselineColors' Legacy siblings of any
 * kind (it reads ONLY BaselineColors, via the same `FVertexMaskForgeWorkingStateOwner::GetBaselineColors()`
 * accessor `ApplyPreviewToEntry`'s own Dynamic branch already uses for live preview) -- isolation from
 * Legacy's own persisted/committed state remains structural, not conditional.
 *
 * PRODUCTION REACHABILITY (M16-K.6D-7A): this module has NO production caller. SVertexMaskForgePanel is
 * completely unmodified by this checkpoint -- CanAcceptChanges()/AcceptPendingChanges() still
 * unconditionally reject PreviewSource == Dynamic, exactly as before. Only this checkpoint's own
 * Automation tests call this function. Panel wiring, Accept eligibility for Dynamic, the confirmation
 * dialog, and actually reaching VertexMaskForgeAcceptWriter are ALL explicitly out of scope here -- see
 * M16-K.6D-7A's own checkpoint description for the full deferred list.
 *
 * SINGLE SEMANTIC EVALUATION PATH: this module calls
 * VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology directly, once
 * per eligible component, with that component's own `GetBaselineColors()` and the caller-supplied
 * `FVertexMaskForgeDynamicLayerStack` -- the SAME two inputs (plus the entry's own WorkingMesh) the live
 * Dynamic preview branch already uses. No blend formula, layer iteration, Fill resolution, or Material
 * Slot generation is reimplemented here -- this module is a thin, caller-owned wrapper around the
 * orchestrator's own already-tested, unmodified math, exactly mirroring how the Legacy builder is a thin
 * wrapper around already-composed `WorkingColors` rather than a second composition implementation.
 */
namespace VertexMaskForgeDynamicAcceptTargetBuilder
{
	/**
	 * Validates every SelectedMeshes entry eligible for a Dynamic Accept and, only if ALL of them pass,
	 * returns the exact set of {Static Mesh, final Source-Topology corner-order colors} to write. Nothing
	 * is written here -- pure validation + composition, matching
	 * VertexMaskForgeAcceptTargetBuilder::BuildSourceTopologyAcceptTargets' own contract shape exactly.
	 *
	 * An entry is considered here iff Entry->bUseSourceTopology (Dynamic today is Source-Topology only --
	 * see Docs/VertexMaskForgeArchitecture.md §16) and it has at least one PreviewComponent. Unlike the
	 * Legacy builder, there is deliberately NO GeneratorState.*.State==Ready pre-filter -- that state
	 * belongs exclusively to the Legacy pipeline and has no meaning for Dynamic. An EMPTY DynamicLayerStack,
	 * or a non-empty stack with every layer disabled, is VALID input here and produces the orchestrator's
	 * own byte-exact Baseline passthrough for every eligible component -- this function only decides
	 * whether the COMPUTATION is valid, never whether the UI should currently allow Accept (that eligibility
	 * decision belongs entirely to a future panel-wiring checkpoint, not to this seam).
	 *
	 * For an eligible entry, composes each component's result independently via
	 * VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology (that
	 * component's own GetBaselineColors(), the entry's own WorkingMesh, and the single caller-supplied
	 * DynamicLayerStack) and requires agreement across every component of that entry -- mirroring
	 * BuildSourceTopologyAcceptTargets' own divergent-per-instance-baseline rule exactly: if two components
	 * of the same asset produce different results (their captured Baselines differ), the write target is
	 * AMBIGUOUS and Accept is blocked for the WHOLE operation (OutTargets left exactly as it was before this
	 * call), never narrowed to one component, never silently resolved by picking the first/last result.
	 *
	 * Also reuses VertexMaskForgeAcceptTargetBuilder::ValidateSourceTopologyCorrespondence (the same
	 * TriIDMap -> FTriangleID -> VertexInstanceID formal validation the Legacy Source-Topology builder
	 * already performs) so this function's own output is already safe for a future writer to consume,
	 * without duplicating that validation logic.
	 *
	 * Fails (returns false, OutTargets left completely unchanged from its state at the moment this
	 * function was called) on: an invalid/unresolvable Static Mesh or working topology; a Baseline
	 * cardinality mismatch or any other orchestrator-detected failure (unsupported masked generator
	 * type, invalid input) for any component; a stale/invalid triangle-corner correspondence; or a
	 * same-asset multi-component conflict. A component with no captured Baseline yet (colors not
	 * initialized) is skipped, not fatal -- mirrors the Legacy builder's own "nothing composed yet for
	 * this component" skip. An entry with zero components surviving that skip is itself skipped (not an
	 * error) -- also mirroring the Legacy builder.
	 */
	bool BuildSourceTopologyAcceptTargets(
		const TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& SelectedMeshes,
		const FVertexMaskForgeDynamicLayerStack& DynamicLayerStack,
		TArray<VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget>& OutTargets,
		FText& OutErrorText);
}
