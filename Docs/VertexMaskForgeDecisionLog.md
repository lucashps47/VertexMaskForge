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

## Future Decision Candidates

The following are open questions this project has identified but not yet decided. Listed without a preferred solution.

- Migration strategy for connecting the Dynamic composition subsystem to the production panel (and what happens to the legacy path afterward).
- A representable policy for a `Pending` (in-progress) generation status, if one is ever needed.
- Cross-generator FailedAfterReady and failed-regeneration preservation policy — whether ADR-005's Material-Slot-only contract should extend to other generators/generation paths, and if so, how.
- Per-generator domain/store choice for Dynamic generation beyond Material Slot — in particular, resolving Ambient Occlusion's per-entry-vs-per-component ownership tension noted in the architecture doc.
- Whether/how the Dynamic Layer Stack should be persisted (today it is session-only, panel-global, non-serialized).
- Whether/how Recipes/Layers should be serialized for reuse across sessions or assets.
- Final removal (or permanent retention) of the legacy fixed `GeneratorState` composition path.
- Definitive Accept/Cancel behavior once/if the Dynamic path gains a real caller (today's Accept/Cancel contracts are defined only in terms of the legacy path's `WorkingColors`).
