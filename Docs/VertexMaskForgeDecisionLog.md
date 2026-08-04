# Vertex Mask Forge Architectural Decision Log

This file records architectural decisions and the reasoning behind them, as evidenced by the current code and test suite. [VertexMaskForgeArchitecture.md](VertexMaskForgeArchitecture.md) describes the state those decisions produced; this file records *why* and *when in the project's own checkpoint sequence* each decision was made. Entries here are historical — do not edit an ADR to match a later change; instead add a new ADR and mark supersession.

---

## ADR-001 — Separate Baseline and Working Color State

- **Status:** Accepted
- **Decision:** `BaselineColors`, `CommittedColors`, and `WorkingColors` (and their `SourceTopology*` siblings) are three independently-addressable arrays on `FVertexMaskForgePreviewComponentState`, not one array with implicit derivation.
- **Context:** Composition needs a stable, non-accumulating reference point (Baseline) to recompute from on every recomposition, a separate settled reference point for Channel-Filter fallback and Fill consolidation (Committed), and a live, display/Accept-facing result (Working).
- **Consequences:** Any recomposition can be re-derived cleanly from Baseline without accumulating error across repeated calls; Committed and Working can diverge intentionally (e.g. mid-edit preview vs. last consolidated state) without corrupting Baseline.
- **Protected by:** `VertexMaskForge.WorkingStateOwner.Restore`, `VertexMaskForge.WorkingStateOwner.ComposedColorsControlled`.
- **Supersedes:** —
- **Superseded by:** —

---

## ADR-002 — Treat Render Vertices and Source Topology as Distinct Domains

- **Status:** Accepted
- **Decision:** Every color-state array (Baseline/Committed/Working) and every fixed generator's entry point exists in two parallel forms — a render-vertex form and a source-topology (`FDynamicMesh3` triangle-corner) form — rather than one unified representation. The Dynamic model's own result payload/store (`FVertexMaskForgeInstanceMaskResult`/`FVertexMaskForgeInstanceResultStore`) does **not** follow this pattern: it is domain-agnostic by construction (no domain tag, no typed variant), and relies entirely on caller discipline to stay within one domain per `MaskInstanceId` (see [Architecture §3](VertexMaskForgeArchitecture.md#3-domain-model)).
- **Context:** Render-buffer vertex identity (per render-buffer vertex index, with geometric positions routinely duplicated at attribute seams) and source-topology identity (per triangle corner, e.g. for Nanite) are not interchangeable; a shared geometric position can legitimately need different values across the render-buffer vertices or triangle corners that share it.
- **Consequences:** Every new color-state array or fixed-generator entry point must decide which domain it targets; nothing automatically bridges the two. The Dynamic result store's lack of a domain tag is a real, separately-tracked risk (see Known Limitations in the architecture doc), not something this decision itself resolves.
- **Protected by:** The `WorkingMeshDomainSplit` test family, and every generator's paired render-vertex/`FromDynamicMesh` entry points.
- **Supersedes:** —
- **Superseded by:** —

---

## ADR-003 — Identify Dynamic Mask Results by MaskInstanceId

- **Status:** Accepted
- **Decision:** A Dynamic generator's result is keyed by a `FGuid` (`MaskInstanceId`) that is stable for the lifetime of the `FVertexMaskForgeGeneratorMaskInstance` that owns it — preserved across reorder, rename, and confirmed parameter edits (`SetLayerMaskParams`) — and independent of the layer's own `LayerId` and of array position. A **new** `MaskInstanceId` is minted only when `SetLayerMaskGeneratorType` actually changes a layer's generator type (assigning a first mask, or replacing one type with a different one); re-assigning the same type is a no-op that preserves the existing id. `LayerId` remains independent of `MaskInstanceId` throughout.
- **Context:** Layer order/position is mutable (reorder, remove, re-insert); a result keyed by index would silently become stale or misattributed after any reorder.
- **Consequences:** A layer and its mask's result can be correctly re-associated after any stack mutation or parameter edit that preserves the generator type, at the cost of the store having no domain awareness of its own (see ADR-002's consequence and the architecture doc's Known Limitations).
- **Protected by:** The `SetLayerMaskGeneratorType`/`SetLayerMaskParams` contract tests inside `DynamicLayerStackTests.cpp`, and the `MaskInstanceId`-preservation assertion in `VertexMaskForge.DynamicMaskGeneration.GeneratedMaterialSlotResultDrivesEvaluatedColorAtBothCorners`.
- **Supersedes:** —
- **Superseded by:** —

---

## ADR-004 — Keep Composition Pure Until Explicit Application

- **Status:** Accepted
- **Decision:** Every composition function (`ComposeGeneratorLayersSequential`, `EvaluateColor`, `ComposeColors`) is a pure computation over its inputs, returning or writing only to caller-owned output — none of them mutate any owner's stored color state. Within the initialized color-state composition lifecycle, applying a freshly composed output requires `ApplyComposedColorsRGB` (gated by a chosen `EColorCommitMode`), while `RestoreFromBaseline` and `RestoreFromCommitted` are explicit restoration mutations. This decision does not attempt to enumerate initialization, capture, reset, or reattachment mutations performed by the owner — those belong to the owner's broader lifecycle, not to the composition-purity contract this ADR freezes.
- **Context:** Decoupling "compute a candidate result" from "commit it" lets a caller preview, discard, or repeatedly recompute without side effects, and lets a checkpoint prove composition correctness without needing to also prove persistence correctness in the same test.
- **Consequences:** Any future orchestrator that wants Dynamic composition to actually affect a preview must explicitly call `ApplyComposedColorsRGB` — this is a deliberate, not accidental, gap today.
- **Protected by:** `VertexMaskForge.WorkingStateOwner.ComposedColorsControlled`, and `VertexMaskForge.DynamicCompositionSourceTopologyIntegration.RealSourceTopologyBaselineAndRealStoredMaterialSlotResultProduceExpectedColorsAcrossCoveredAndUncoveredCorners`'s own explicit non-mutation assertions.
- **Supersedes:** —
- **Superseded by:** —

---

## ADR-005 — Preserve the Previous Ready Material Slot Result Across Failed Dynamic Regeneration

- **Status:** Accepted
- **Decision:** Restricted explicitly to the Dynamic Material Slot generation path implemented by `VertexMaskForgeDynamicMaskGeneration::GenerateStoredResultForMaterialSlotInstance`: a generation call that fails never overwrites a previously-stored `Ready` result for the same `MaskInstanceId` — the store is left exactly as it was.
- **Context:** A transient or genuine failure during regeneration (e.g. a temporarily invalid parameter state) should not degrade an already-correct preview to a blank/zero-coverage one.
- **Consequences:** This contract is proven only for `GenerateStoredResultForMaterialSlotInstance`. It is **not** generalized to any other generator, present or future, and no other generation path is claimed to share this behavior. A general cross-generator FailedAfterReady policy is an open question (see Future Decision Candidates), not a decided contract.
- **Protected by:** `VertexMaskForge.DynamicMaskGeneration.FailedRegenerationPreservesPreviousReadyResult`.
- **Supersedes:** —
- **Superseded by:** —

---

## ADR-006 — Separate Global Channel Filtering from Per-Layer Channel Participation

- **Status:** Accepted
- **Decision:** The panel's global Channel Filter (`bChannelFilterR/G/B`, legacy-only) and a Dynamic layer's own `bAffectRed/Green/Blue` are two structurally independent mechanisms with different owners, different scopes, and different fallback behavior — never treated as the same state or as convertible into one another, in the code as it exists today.
- **Context:** An earlier architectural audit in this project's history conflated the two; a corrective audit established by direct code inspection that the legacy layer struct (`FVertexMaskForgeMaskLayerParams`) has no per-layer channel field at all, and that the global filter's `CommittedColors` fallback is fused into the legacy bridge's own function, not a separate later stage.
- **Consequences:** Any future Dynamic-to-panel integration must make an explicit architectural decision about whether, where, and in what order the global filter participates alongside per-layer channel participation; the current code does not freeze that integration policy, and this decision does not presume or mandate one.
- **Protected by:** `VertexMaskForge.GeneratorLayerBridge.ChannelFilter` (global), `VertexMaskForge.DynamicLayerEvaluator.ChannelFilterSingleChannel`/`ChannelFilterAllDisabled` (per-layer).
- **Supersedes:** —
- **Superseded by:** —

---

## ADR-007 — Preserve Stable Layer Ordering

- **Status:** Accepted
- **Decision:** Both layer models guarantee that reordering operations preserve every other layer's relative order and identity — never a destructive rebuild, never an implicit renumbering that could silently reassign one layer's configuration to another's slot.
- **Context:** The legacy model expresses order via a fixed 7-slot enum permutation (`GeneratorLayerOrder`); the Dynamic model expresses it via index position with identity-preserving move operations (`MoveLayerUp`/`MoveLayerDown`/`MoveLayer`, resolved by `LayerId`, never a cached index).
- **Consequences:** Reordering is safe to expose directly to interactive UI without risking silent data corruption on repeated moves.
- **Protected by:** `VertexMaskForge.DynamicLayerStack.MoveUpAndMoveDown`, `VertexMaskForge.DynamicLayerStack.RepeatedMovesPreservePermutation`; legacy ordering protected by the `VertexMaskForge.PanelCompositionIntegration.*` reorder tests.
- **Supersedes:** —
- **Superseded by:** —

---

## ADR-008 — Keep the Dynamic Composition Subsystem Isolated Until Its Contracts Are Proven

- **Status:** Accepted
- **Decision:** Every Dynamic-model building block (`FVertexMaskForgeDynamicLayerStack`, `VertexMaskForgeDynamicLayerEvaluator`, `VertexMaskForgeDynamicLayerBatchCompositor`, `VertexMaskForgeDynamicMaskGeneration`, `FVertexMaskForgeInstanceResultStore`) was built and independently proven by its own dedicated test suite with **zero production composition/output caller** — no production code path connects Dynamic evaluation or composition to `WorkingColors`, preview, or Accept — one small checkpoint at a time, rather than wiring any of it into the panel's output prematurely. (The Dynamic Layers Slate UI section is itself a real production caller of the *stack's own editing API* — `AddLayer`, `SetLayer*`, reorder — but that UI's edits do not reach evaluation or composition; see [Architecture §15](VertexMaskForgeArchitecture.md#15-current-integration-status).)
- **Context:** A sequence of read-only architectural audits (culminating in M16-K.5I and its corrective rounds) established that the smallest-safe-step discipline this project already follows for generator work applies equally to composition infrastructure — integrating before every structural boundary (domain handling, identity, absence-case semantics) is independently proven would risk baking in unproven assumptions.
- **Consequences:** The Dynamic subsystem is fully real and tested but currently inert with respect to any visible output — this is intentional, not an oversight (see [Architecture §15](VertexMaskForgeArchitecture.md#15-current-integration-status)).
- **Protected by:** The entire `DynamicLayerStackTests`, `DynamicLayerEvaluatorTests`, `DynamicLayerBatchCompositorTests`, `DynamicMaskGenerationTests`, and `InstanceResultStoreTests` suites, none of which reference `SVertexMaskForgePanel`.
- **Supersedes:** —
- **Superseded by:** —

---

## ADR-009 — Prove Dynamic Source-Topology Integration Before Panel Migration

- **Status:** Accepted
- **Decision:** Before any panel wiring is attempted, prove — with real production objects, not hand-built fixtures — that the full chain from a real `FVertexMaskForgeWorkingStateOwner` baseline through real Material Slot generation, real store move-preservation (`InstallWorkingMesh`), to real batch composition produces byte-exact, non-mutating output, restricted to the one domain (source-topology) where every underlying API is already exercised by existing tests without inventing new fixtures.
- **Context:** An audit of the render-vertex Material Slot generation path found no existing test or fixture anywhere in the codebase constructs a real `FStaticMeshLODResources` for it; inventing one for this checkpoint would have introduced untested fixture machinery. The source-topology path reuses only already-proven construction patterns.
- **Consequences:** M16-K.5J's proof is deliberately scoped to source-topology + Material Slot only; render-vertex Dynamic generation and any other generator's Dynamic integration remain unproven and are not implied by this result.
- **Protected by:** `VertexMaskForge.DynamicCompositionSourceTopologyIntegration.RealSourceTopologyBaselineAndRealStoredMaterialSlotResultProduceExpectedColorsAcrossCoveredAndUncoveredCorners`.
- **Supersedes:** —
- **Superseded by:** —

---

## ADR-010 — Restrict Dynamic Material Slot Editing to Single-Mesh Selections

- **Status:** Accepted
- **Decision:** Editing (and any future evaluation) of a Dynamic layer's `FVertexMaskForgeMaterialSlotParams` is supported only when `SelectedMeshes.Num() == 1` — i.e. exactly one unique Static Mesh asset is represented across the current selection (`SelectedMeshes` is deduplicated by asset path; multiple selected components referencing the same asset still collapse to that one entry, per `AddOrUpdateSelectedMesh`'s own dedup key). `SelectedSlotIndex` remains the payload's sole persisted identity (a raw index, never a name); the future UI's list of selectable indices must be built from that one asset entry's own `WorkingMesh.MaterialSlotOptions`. With zero or more than one such asset entry, Material Slot editing (and generation) is unavailable — the system does not compute an intersection or union of slots across meshes, does not pick an arbitrary mesh from the selection, does not silently apply the same raw index across heterogeneous layouts, and does not clamp an out-of-range index to a fallback such as `0`. An index that is negative or outside `MaterialSlotOptions` for that one asset entry must be surfaced explicitly as invalid/unavailable, never silently accepted. No sentinel value for "no slot selected" is introduced into the payload; `Clear`/reassignment continue to follow the model's own existing defaults (`SelectedSlotIndex=0, bInvert=false`, minted fresh by `SetLayerMaskGeneratorType`/`MakeVertexMaskForgeGeneratorParams`). This decision governs UI/orchestration policy only — it does not itself implement the gate, any validation UI, or any reconciliation logic.
- **Context:** A M16-K.6C implementation attempt exposed `SelectedSlotIndex` via an open-domain (`[0, INT32_MAX]`) spin box with no reference to any mesh's actual material slot count. A corrective audit found that `SelectedSlotIndex`'s real valid domain is per-entry (`[0, WorkingMesh.MaterialSlotOptions.Num())`, confirmed directly in `VertexMaskForgeMaterialSlotGenerator.cpp`'s own `IsValidIndex` guard, which resolves to `EVertexMaskForgeScalarMaskState::Unavailable` outside that range), while `FVertexMaskForgeDynamicLayerStack` is panel-global — so a single panel-wide index has no single well-defined meaning across a heterogeneous multi-mesh selection. `SetLayerMaskParams` itself validates only that the `Params` variant's active alternative matches `GeneratorType`, never the field's semantic validity against any mesh — so the model provides no structural guard against this ambiguity on its own. The identical constraint is already implemented in the legacy path: `SVertexMaskForgePanel::IsMaterialSlotMaskAvailableForSelection() const { return SelectedMeshes.Num() == 1; }`, with `ReconcileMaterialSlotSelection()` building its slot list from `SelectedMeshes[0]`'s own `WorkingMesh.MaterialSlotOptions` and leaving the list empty for zero/multiple entries — an existing legacy constraint, not a new one invented by this decision.
- **Consequences:** The future Material Slot parameter-editing UI (and any future single-mesh-scoped Dynamic generation orchestration) must reuse this exact single-mesh eligibility gate rather than inventing a new one. No change to `FVertexMaskForgeMaterialSlotParams`, `SetLayerMaskParams`, the Material Slot generator, or any result store is required by this decision — it constrains only how a future UI/orchestrator is allowed to use the already-existing API. The M16-K.6C UI attempt was reverted before commit as a direct consequence of this decision.
- **Alternatives considered:**
  - *Raw index applied indiscriminately across multiple meshes* (the reverted M16-K.6C approach) — rejected: cannot distinguish a valid index from one that is out of range or means something different for another mesh; no domain feedback possible.
  - *Intersection of valid slot indices across all selected meshes* — rejected for now: no precedent, no test coverage, and silently narrows what indices are even offered without the user necessarily understanding why; a real design in its own right, not a small fix.
  - *Identity by material slot name instead of index* — rejected for now: `FVertexMaskForgeMaterialSlotParams` has no name field; this would be a payload/model contract change, not a UI-only change, and slot names are not guaranteed unique or stable in the way this project's existing code already treats them as "just a label."
  - *Composite identity (name + index, or per-mesh index map)* — rejected for now: same reasoning — a real payload/model contract change, out of scope for restoring a working, precedented editing gate.
  - *Per-entry (rather than panel-global) Dynamic configuration for this one parameter* — rejected for now: would special-case Material Slot's storage location differently from every other Dynamic layer property, without a broader decision about whether `DynamicLayerStack` itself should become per-entry.
  - *Single-mesh-only, mirroring the legacy precedent* — **selected**: reuses an established precedent already implemented in the current legacy flow (`IsMaterialSlotMaskAvailableForSelection`); requires no change to the payload, generator, or result-store model; keeps the fix small and reversible; gives a concrete, non-ambiguous domain (`WorkingMesh.MaterialSlotOptions` of the one asset entry) for a future UI to build against.
- **Protected by:** No automated test yet — this ADR records a policy decision for implementation in a future checkpoint, not an implemented and tested contract. The legacy precedent it mirrors (`IsMaterialSlotMaskAvailableForSelection`) has no dedicated automation test of its own either (confirmed by symbol search during the corrective audit).
- **Supersedes:** —
- **Superseded by:** —

---

## ADR-011 — Authoritative Preview Source and Non-Persistible Dynamic Preview

- **Status:** Accepted
- **Decision:** A new explicit, panel-owned enum, `EVertexMaskForgePreviewSource` (`Legacy`, `Dynamic`), is the sole authority for which pipeline's semantically composed colors the preview currently shows. Owner: `SVertexMaskForgePanel` (member `PreviewSource`); lifetime: the panel instance's own, identical to `CurrentPreviewMode`'s lifetime. Default: `Legacy`. Only an explicit user action on a dedicated future control may change it — never inferred from Dynamic Layer Stack contents, assignment presence, generator availability, result-store contents, Accept state, Fill state, Material Slot selection, tab/section expansion, the last-edited control, or generation success/failure. **This checkpoint (M16-K.6D-1) introduces only the type and the member, both inert** — no function reads `PreviewSource` to select behavior yet, no function writes it to `Dynamic`, and no UI control exists to change it. The remainder of this ADR records the *contract* this authority establishes for future checkpoints — it is a decision about what must eventually hold, not a description of code that exists today.
  - **Legacy contract (current, unchanged):** `Legacy → WorkingColors → preview → Accept`. `WorkingColors`/`SourceTopologyWorkingColors` remain the sole semantically persistible buffer, written only by `ApplyComposedColorsRGB`/`RestoreFromBaseline`/`RestoreFromCommitted` (ADR-004), read verbatim by `BuildAcceptTargets` (unchanged). Returning from `Dynamic` to `Legacy` must trigger a full Legacy recomposition through the existing `RecomposeWorkingColors`/`ApplyPreviewToEntry` path — `WorkingColors` must never be assumed to have remained visually or semantically current while `Dynamic` was selected, because Legacy composition does not run while Dynamic is being shown (future checkpoint).
  - **Dynamic contract (future, not yet implemented):** `Dynamic → local/transitory buffer → preview visual only`. This buffer must never write `WorkingColors`/`SourceTopologyWorkingColors`, must never be reachable by `BuildAcceptTargets`, and must never become persistible merely because it was displayed. It must not be owned by `FVertexMaskForgeWorkingStateOwner` (its exact owner/shape is deliberately deferred — see Future Decision Candidates and the M16-K.6D-2 checkpoint). On generation/composition failure, or when the ADR-010 single-asset gate is not satisfied, any previously-shown Dynamic visual must be discarded (no stale result left on screen) — `PreviewSource` itself must NOT silently revert to `Legacy` on failure (no automatic fallback); the Dynamic Layer Stack's own configuration must survive unavailability untouched (indisponibilidade never mutates `SelectedSlotIndex`/`bInvert`/any other Params).
  - **Preview Mode / Channel Filter:** Preview Mode (`EVertexMaskForgePreviewMode`) is a display-only reduction that must apply to whichever pipeline `PreviewSource` currently selects — it never participates in choosing the pipeline itself. The global Channel Filter (`bChannelFilterR/G/B`) remains Legacy-only (ADR-006 is not superseded by this ADR); it must not be repurposed as a general Dynamic display filter, and must become unavailable in the UI while `PreviewSource == Dynamic` (future checkpoint — not implemented here).
  - **Fill:** Fill Black/White (`OnFillWhiteClicked`/`OnFillBlackClicked` → `RunConstantFill`) remains Legacy-only and must become unavailable in the UI while `PreviewSource == Dynamic` (future checkpoint — not implemented here). This is unrelated to `FVertexMaskForgeLayer::Fill` (the Dynamic model's own, differently-scoped per-layer field), which this ADR does not touch.
  - **Accept and Cancel:** isolation of Accept from any Dynamic-sourced buffer is a **structural** guarantee (the buffer's own owner/lifetime, per the Dynamic contract above, never intersects `BuildAcceptTargets`'s inputs) — not a runtime `if (Dynamic) { ... }` guard inside Accept. As an additional UX safeguard (not the primary barrier), Accept must become unavailable while `PreviewSource == Dynamic` in a future checkpoint, so a user is never shown an Accept affordance that would persist something other than what they are currently viewing. Cancel's contract is unaffected and must continue to work identically regardless of `PreviewSource`. Accept, Cancel, Undo, and Redo are **not modified by this checkpoint**.
  - **Future extensibility:** this contract must not block a future Render Vertex domain — extending `EVertexMaskForgePreviewSource`'s effective scope to Render Vertex, or introducing additional values, is left to whichever future checkpoint actually connects it, via explicit new values or a deliberately revisited abstraction, never by silently overloading `Legacy`/`Dynamic`'s existing meaning. Render Vertex is not implemented by this ADR.
- **Context:** M16-K.6D (rejected, see the M16-K.6D-R1 recovery) attempted to wire Dynamic Material Slot + Source Topology composition directly into `ApplyPreviewToEntry`, introducing an unauthorized `bool bDynamicPreviewActive` and writing the Dynamic composed result into the same `WorkingColors` buffer Accept reads verbatim — creating an unreviewed path by which a Dynamic preview could become persistible without any change to Accept's own code. The M16-K.6D-R1 recovery audit confirmed no authority for choosing between Legacy and Dynamic existed anywhere in the codebase (`EVertexMaskForgePreviewMode` is display-only; the Dynamic Layers UI section has no visibility gate and coexists unconditionally alongside Legacy's own "Layers" section), and that `BuildAcceptTargets` reads `WorkingColors`/`SourceTopologyWorkingColors` verbatim with no origin metadata to distinguish Legacy-produced from any other source. The M16-K.6D-R2 audit compared candidate authority models and recommended `EVertexMaskForgePreviewSource` (a preview-source/pipeline-selection enum) over an "Active Editing Surface" framing (misleading, since Legacy and Dynamic configuration UI coexist and remain simultaneously editable regardless of which pipeline is previewed) or continuing with no authority at all (which structurally cannot support any future Dynamic connection safely).
- **Consequences:** Every future checkpoint that wires Dynamic composition into any real output must read and respect `PreviewSource` rather than inferring activation from Dynamic Layer Stack state. `BuildAcceptTargets`/`VertexMaskForgeAcceptWriter` need no changes for this ADR's structural guarantee to hold, because the future Dynamic buffer is never given a code path into them. The UX-level "Accept unavailable while Dynamic" safeguard, and the Channel-Filter/Fill unavailability while Dynamic, remain explicitly unimplemented — future checkpoints, not covered by this ADR's own code.
- **Alternatives considered:**
  - *`bool bDynamicPreviewActive`* (the rejected M16-K.6D attempt) — rejected: binary bool cannot express a third state cleanly if one is ever needed (e.g. a future third pipeline), and the name conflates "is Dynamic active" with "is Dynamic even wired," inviting exactly the premature-inference bugs this ADR exists to prevent.
  - *Infer from Dynamic Layer Stack contents (layer/assignment/result-store existence)* — rejected: would silently turn a previously configuration-only edit (M16-K.6B/K.6C-2's own established contract) into one with a real preview effect, without any explicit user action; also fails the moment a user configures Dynamic layers while intending to keep previewing Legacy.
  - *Reuse `EVertexMaskForgePreviewMode`* — rejected: that enum is a display-only reduction of whichever pipeline is already selected (`OriginalMaterial`/`RGBVertexColor`/per-channel), not a pipeline selector; conflating the two would break the existing, tested, pure display-derivation contract (ADR-004's own composition-purity boundary).
  - *Automatic fallback to Legacy on Dynamic failure* — rejected: masks a real failure as if nothing happened, and risks silently showing/persisting Legacy's `WorkingColors` (last real composition) as though it were the user's current, intentional choice.
  - *Write the Dynamic result into `WorkingColors`* (the rejected M16-K.6D attempt) — rejected: makes Dynamic preview persistible via Accept without any change to Accept's own code, confirmed as a real (not hypothetical) risk by the M16-K.6D-R1 audit's direct trace of `BuildAcceptTargets`.
  - *A runtime condition inside Accept as the only barrier against persisting Dynamic* — rejected in favor of structural isolation (the Dynamic buffer's own owner/lifetime never reaching Accept's inputs at all): a condition is one omitted `if` away from silently regressing; structural non-reachability cannot regress by omission.
  - *Introduce the visual control (checkbox/tab/switcher) in this same checkpoint* — rejected: this checkpoint's own scope is the authority contract only; a control that lets the user select `Dynamic` before any real Dynamic output exists would be a dead/misleading affordance, and was explicitly out of scope per this checkpoint's own instructions.
  - *Store the future Dynamic buffer on `FVertexMaskForgeWorkingStateOwner` immediately* — rejected for this checkpoint: `WorkingStateOwner` is ADR-001's own carefully-scoped three-array (Baseline/Committed/Working) owner; adding a fourth, non-persistible array to it now, before its exact contract (per-entry vs. per-component vs. local-per-call) is decided, risks conflating two different lifetime/ownership questions in one change. Deferred to a dedicated future checkpoint (M16-K.6D-2/-3 in the current sequencing).
  - **Selected: `EVertexMaskForgePreviewSource` enum, panel-owned, default `Legacy`, written only by future explicit user action, structural (not conditional) isolation from Accept** — smallest change that establishes the authority contract without implementing any of the behavior it will eventually gate, matching this checkpoint's own single objective.
- **Protected by:** No automated test — this checkpoint introduces an inert type and a default-initialized member with zero behavioral surface (no reader, no writer, no UI); build success (the type compiles, the member initializes to `Legacy`) is the only verification this checkpoint's own scope supports. Future checkpoints that give this authority real behavior must add their own tests for those transitions.
- **Supersedes:** —
- **Superseded by:** —

---

## ADR-012 — Layer-Owned Bounding Box as the Second Dynamic Generator, Local-Space Only

- **Status:** Accepted
- **Decision:** Bounding Box is exposed as the second selectable Dynamic Layer generator (after Material Slot), following the same layer-owned model established for Material Slot rather than any shared/panel-level control:
  - **Ownership:** each Dynamic layer's Bounding Box parameters live entirely in that layer's own `FVertexMaskForgeGeneratorParams` variant (`FVertexMaskForgeBoundingBoxParams`, the type already established for the future Recipe/Mask-Instance model, reused unchanged) — never a second copy, never panel-owned state, never the Legacy `SVertexMaskForgePanel::BoundingBoxAxisParams` member. Two Bounding Box layers hold fully independent values.
  - **Evaluator reuse:** the existing, unmodified `VertexMaskForgeBoundingBoxGenerator::GenerateBoundingBoxMaskFromDynamicMesh` is the sole evaluation path — no axis/gradient/mirror/invert/combination formula is duplicated in the orchestrator or in panel UI code, at any layer.
  - **Local-space only:** `VertexMaskForgeDynamicSourceTopologyComposition`'s Pass 1 rejects the whole composition call if any enabled axis requests World Space or if Unified Bounds is requested — deterministically, never silently reinterpreted as Local Space or per-mesh bounds. The Dynamic panel UI exposes no control capable of creating either request. This is a genuine, intentional scope boundary, not an oversight: World Space needs a per-component transform this orchestrator does not receive; Unified Bounds needs full-selection context it does not receive.
  - **Stable identity for Slate callbacks:** every per-layer, per-axis control captures the layer's stable `LayerId` (`FGuid`) by value, resolving the current layer/mask fresh at callback-execution time — the same pattern Material Slot already established, extended here rather than reinvented, since a captured array index or raw pointer cannot survive reorder/deletion/rebuild safely.
  - **Incompatible stored state is surfaced, never normalized:** if a layer's already-stored parameters request World Space or Unified Bounds (unreachable through this UI, but not otherwise structurally precluded), the UI shows a concise warning and disables that layer's axis controls rather than silently rewriting the data or pretending it is valid. Rejection remains solely the orchestrator's responsibility (see above) — the UI warning is a read-only echo of that same predicate, never a second enforcement point.
  - **Single authoritative evaluation path for preview and Accept:** both `ApplyPreviewToEntry`'s Dynamic branch and `VertexMaskForgeDynamicAcceptTargetBuilder` call the same orchestrator against the same live `FVertexMaskForgeDynamicLayerStack` — Bounding Box requires zero change to either, since both were already generator-agnostic.
- **Context:** M16-K.6D-8A audited the existing Legacy Bounding Box UI/generator and the Dynamic Layers panel, finding: `FVertexMaskForgeBoundingBoxParams`/`FVertexMaskForgeAxisMaskParams` already existed and were already fully wired through the generic parts of `FVertexMaskForgeDynamicLayerStack` (`SetLayerMaskGeneratorType`, `SetLayerMaskParams`, `MakeVertexMaskForgeGeneratorParams`); the only genuinely new work needed was (1) the orchestrator's own masked-generator dispatch, and (2) a layer-owned Slate presentation. The audit also found a domain mismatch requiring explicit handling: Bounding Box's Source-Topology generator output is indexed by Dynamic Mesh VertexID, not corner, unlike Material Slot's already-corner-domain output — resolved in M16-K.6D-8B by tagging each generated mask with its own domain and resolving vertex-domain masks per corner through that corner's own triangle. The same audit found an unrelated, obsolete standalone `Layers` panel (a fixed Legacy-generator list with Up/Down buttons, from an earlier misunderstood implementation) that shared no code with Dynamic Layers and needed removing before adding new Dynamic UI, to avoid confusing the two — done separately in M16-K.6D-8C-B, with `GeneratorLayerOrder` (Legacy's own real composition order) explicitly retained.
- **Consequences:** Any future Dynamic generator (beyond Material Slot/Bounding Box) should follow this same layer-owned-parameter, orchestrator-dispatch, stable-`LayerId`-callback model rather than introducing a new pattern per generator. World Space and Unified Bounds support for Dynamic Bounding Box, if pursued, requires extending the orchestrator's own inputs (a per-component transform; a precomputed selection-wide bounds context respectively) — genuinely separate, larger checkpoints, not a UI-only change. No Dynamic layer-stack serialization, presets, or "Paint Layers" concept exists yet; this ADR does not introduce one.
- **Alternatives considered:**
  - *Extract a shared Legacy/Dynamic Bounding Box widget-construction helper* — rejected: the 8C-A audit found every Legacy control's callback body inline-coupled to panel-global `BoundingBoxAxisParams` and Legacy-only invalidation entry points (`InvalidateBoundingBoxRawMask`, `RunAutoUpdatePreview`); only the generic Slate *shape* (widget types, ranges, labels) was reusable, not the callback bodies — extraction would have been broader and riskier than a small, purpose-built Dynamic-only builder.
  - *Show World Space/Unified Bounds controls disabled with a tooltip* — rejected in favor of omitting them entirely: a disabled-but-visible control bound to a field that could already be `true` in stored data risks implying a false state or requiring extra defensive display logic; omission cannot be misread as "supported but greyed out."
  - *Silently reinterpret an incompatible stored layer as Local-space, or clamp `bWorldSpace`/`bUseUnifiedBounds` to false on load* — rejected: would be a silent data rewrite the user never asked for, and would hide a real, currently-inexpressible-through-the-UI state rather than surfacing it.
  - *Capture an array index or row pointer in each axis control's callback* — rejected: proven unsafe by the same reasoning ADR/precedent already established for Material Slot (M16-K.6C-2-FIX) — reorder, deletion, and rebuild all invalidate a captured index/pointer; stable `LayerId` does not.
- **Protected by:** 9 orchestrator tests (`VertexMaskForge.DynamicSourceTopologyComposition.BoundingBox*`, M16-K.6D-8B) covering Local-space X/Y/Z evaluation, invert/mirror, multi-axis combination, falloff, two independent layers with reorder, and explicit World Space/Unified Bounds rejection with output-preservation-on-failure. The per-layer Slate UI itself (selector option presence, warning display, control enable/disable, stable-`LayerId` callback wiring) has no automatable seam in this codebase — no test anywhere constructs `SVertexMaskForgePanel` or a live Preview Component (the same established boundary as every prior Dynamic Layers UI checkpoint) — validated manually instead (M16-K.6D-8C-C, PASS: independent two-layer state, reorder/deletion identity, generator-switch behavior, live preview, Accept/Cancel, no Legacy regression).
- **Supersedes:** —
- **Superseded by:** —

---

## Future Decision Candidates

The following are open questions this project has identified but not yet decided. Listed without a preferred solution.

- Migration strategy for connecting the Dynamic composition subsystem to the production panel (and what happens to the legacy path afterward).
- A representable policy for a `Pending` (in-progress) generation status, if one is ever needed.
- Cross-generator FailedAfterReady and failed-regeneration preservation policy — whether ADR-005's Material-Slot-only contract should extend to other generators/generation paths, and if so, how.
- Per-generator domain/store choice for Dynamic generation beyond Material Slot — in particular, resolving Ambient Occlusion's per-entry-vs-per-component ownership tension noted in the architecture doc.
- Whether/how the Dynamic Layer Stack should be persisted (today it is session-only, panel-global, non-serialized).
- Whether/how Recipes/Layers should be serialized for reuse across sessions or assets.
- Final removal (or permanent retention) of the legacy fixed `GeneratorState` composition path.
- Whether/how Dynamic Bounding Box gains World Space support (requires threading a per-component transform into the orchestrator).
- Whether/how Dynamic Bounding Box gains Unified Bounds support (requires a precomputed, selection-wide bounds context reaching the orchestrator).
- Whether a reusable "Paint Layers" / saved-preset concept is ever built on top of the Dynamic Layer Stack.
- Which Dynamic generator (if any) is exposed next, and whether the layer-owned-parameter/orchestrator-dispatch model ADR-012 establishes needs revisiting for a generator whose parameters are not a small fixed struct (e.g. Noise's larger parameter surface).
- Definitive Accept/Cancel behavior once/if the Dynamic path gains a real caller (today's Accept/Cancel contracts are defined only in terms of the legacy path's `WorkingColors`).
