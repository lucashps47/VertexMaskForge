# Vertex Mask Forge Architecture

## Metadata

- **Document status:** Living source of truth
- **Baseline commit:** `63cfccc4ae9ba3285ab0f1b58d0e37ff534cbcc2`
- **Baseline subject:** `test: prove source-topology dynamic composition integration`
- **Validated suite:** 298/298
- **Last architectural checkpoint:** M16-K.5J

See also: [Decision Log](VertexMaskForgeDecisionLog.md).

---

## 1. Purpose and Scope

**Current Contract.** Vertex Mask Forge is an Editor-only Unreal Engine 5.8 plugin that generates and edits vertex colors on Static Meshes, so that other systems (materials, shaders, tools) can consume artist-authored per-vertex masks. It solves the production problem of needing procedurally-derived, hand-tunable vertex masks (occlusion, curvature, position, normal-direction, thickness, material-slot membership, noise) without a DCC round-trip.

The tool operates entirely inside the Editor: a Slate panel (`SVertexMaskForgePanel`) lets an artist select one or more eligible components in the viewport, generate one or more scalar masks per selection, compose them into a working color buffer, preview the result live on the real components, and either **Accept** (persist into the Static Mesh's source vertex colors) or **Cancel** (discard the session). Nothing in this plugin ships into a packaged game — it has no Runtime module and no gameplay-facing API.

---

## 2. Architectural Overview

**Current Contract**, for the legacy/production path (the only path with a live UI caller and a real Accept target):

```
Selection (viewport components)
  → Working Mesh (per-asset geometry + identity)
  → Generator Result (7 fixed scalar masks, GeneratorState)
  → Layer Stack (fixed GeneratorLayerOrder + Channel Filter)
  → Composition (GeneratorLayerBridge, pure)
  → Working Colors (WorkingStateOwner, applied)
  → Display Colors (Preview Mode reduction, display-only)
  → Accept (writes to the Static Mesh source) / Cancel (discards the session)
```

A second subsystem exists in parallel — the **Dynamic Layer** model (`FVertexMaskForgeDynamicLayerStack`, `VertexMaskForgeDynamicLayerEvaluator`, `VertexMaskForgeDynamicLayerBatchCompositor`, `VertexMaskForgeDynamicMaskGeneration`) — whose composition core (stack identity, per-index evaluation, batch composition) is structurally complete and tested, but **not yet connected** to any panel output. It has a real, editable Slate panel section, real generation for one generator (Material Slot), and a proven, tested composition chain (see [§8 Composition](#8-composition) and [§15 Current Integration Status](#15-current-integration-status)), but it does not feed any preview, `WorkingColors`, or Accept target today. It is shown here deliberately isolated:

```
[Dynamic subsystem, proven but isolated]
DynamicLayerStack identity → real generation → InstanceResults → ComposeColors → local output only
(no arrow into Working Colors / Accept — none exists yet)
```

**Contract established, implementation pending (M16-K.6D-1, ADR-011):** `SVertexMaskForgePanel` now owns an explicit `EVertexMaskForgePreviewSource PreviewSource` member (`Legacy`/`Dynamic`, default `Legacy`) — the authority that will eventually determine which of the two pipelines above `ApplyPreviewToEntry` shows. As of this checkpoint the member exists and defaults to `Legacy`, but **nothing reads it to select behavior and nothing writes it to `Dynamic`** — no UI control exists yet, and the diagram above remains accurate: Dynamic still has no arrow into Working Colors or Accept. See [Decision Log ADR-011](VertexMaskForgeDecisionLog.md#adr-011--authoritative-preview-source-and-non-persistible-dynamic-preview) for the full future contract (a non-persistible transitory buffer for Dynamic, structural — not conditional — isolation from Accept, and the revised checkpoint sequence) this member's *type* now codifies, none of which is implemented yet.

**Contract established (M16-K.6D-2, corrected):** a real Source-Topology visual-only preview seam now exists — `VertexMaskForgePanel::ApplySuppliedSourceTopologyPreviewColors(State, SourceMesh, const TArray<FColor>& SemanticComposedColors, EVertexMaskForgePreviewMode PreviewMode, DebugMaterial, bUseOriginalMaterials, ActorHideStates) -> bool`, a `static` free function private to `SVertexMaskForgePanel.cpp`. It receives colors that are semantically composed (i.e. equivalent to what `WorkingColors` holds immediately after `ApplyComposedColorsRGB`), **before** any Preview Mode display reduction — the M16-K.6D-2 checkpoint's first attempt was rejected specifically because its own "seam" received colors the caller had already reduced through `DeriveDisplayColors`, so its `ComposedColors` parameter was not actually semantically composed. This corrected version validates `SemanticComposedColors.Num()` against the Source Topology corner count (`SourceMesh.TriangleCount() * 3`) completely before any visual mutation — an exact mismatch returns `false` immediately, with no padding, no truncation, no partial application — then derives display colors itself via `VertexMaskForgeDisplayColorDerivation::DeriveDisplayColors` (delegated to the new, directly-testable, non-static `VertexMaskForgePanel::DeriveValidatedSourceTopologyPreviewColors`, declared in `VertexMaskForgeMaskTypes.h`), and applies the result through the existing `EnsureSourceTopologyPreviewComponent`/`ApplySourceTopologyColorsToPreviewComponent` helpers, unchanged. It reads no `FVertexMaskForgeWorkingStateOwner`/`WorkingMesh.InstanceResults`/`FVertexMaskForgePreviewComponentState::InstanceResults` state, never calls `ApplyComposedColorsRGB`/`RestoreFromBaseline`/`RestoreFromCommitted`, and never writes `WorkingColors`/`CommittedColors`/`BaselineColors` (or their `SourceTopology*` siblings) or any Accept-facing store. Today's only caller remains `ApplyPreviewToEntry`'s Legacy Source-Topology branch, which supplies its own just-written `StateOwner->GetWorkingColors()` (the exact semantic composition `ApplyComposedColorsRGB` just applied) and `CurrentPreviewMode`, and explicitly calls `RestorePreviewVisualOnly` if the seam returns `false` — **no Dynamic caller exists yet**, no buffer was created, and `PreviewSource` is still never read anywhere in production code (fixed at its `Legacy` default in practice). The render-vertex sibling (`ApplyPreviewColorsToPreviewComponent`) was deliberately left untouched — this checkpoint's seam is Source-Topology only, matching the M16-K.6D sequence's own Material-Slot-plus-Source-Topology scope; a Render Vertex seam remains **future checkpoint**. M16-K.6D-3 through M16-K.6D-5 (Material Slot API adaptation, testable Dynamic orchestrator, functional UI/wiring) remain **planned**, not implemented.

**Contract established (M16-K.6D-3):** the Material Slot generator's existing, unmodified Source-Topology entry point — `VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskFromDynamicMesh(const FVertexMaskForgeWorkingMesh& WorkingMesh, int32 SelectedSlotIndex, bool bInvert) -> FVertexMaskForgeScalarMask` — is now formally documented as THE caller-owned Source-Topology Material Slot API a future Dynamic orchestrator (M16-K.6D-4) is meant to call directly. Auditing this checkpoint found the function already satisfied every required property without any signature or math change: it takes only read-only inputs (no store, no `WorkingColors`, no `FVertexMaskForgeWorkingStateOwner`), returns its result entirely by value (domain: Source Topology corners, cardinality exactly `Mesh.TriangleCount() * 3`), and reports success/failure unambiguously via `Mask.State` (`Ready`, including a legitimate all-`0.0` result when `SelectedSlotIndex` matches no triangle, versus `Unavailable` for a structural failure — the two are never confused). Three existing call sites already treat it as the single source of truth, each writing to its own store only after confirming `Ready`: `SVertexMaskForgePanel`'s own legacy inline call (`GeneratorState.MaterialSlotMask`), `VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskInstanceResult` (`WorkingMesh.InstanceResults`, Recipe/M16-E identity), and `VertexMaskForgeDynamicMaskGeneration::GenerateStoredResultForMaterialSlotInstance` (`WorkingMesh.InstanceResults`, Dynamic Layer identity) — none of the three were altered by this checkpoint. Nine new direct tests (`VertexMaskForge.MaterialSlotCallerOwned.*`) prove this contract, including byte-for-byte equivalence against `GenerateMaterialSlotMaskInstanceResult`'s own stored result for identical inputs. **No orchestrator was created** (`ComputeComposedColorsRGBSourceTopology`-equivalent remains reserved for M16-K.6D-4), **no buffer was created**, the result is **not connected to the K.6D-2 preview seam**, and `PreviewSource` is still never read anywhere in production code.

**Contract established (M16-K.6D-4):** a new, testable Dynamic Source-Topology composition orchestrator now exists — `VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology(const FVertexMaskForgeWorkingMesh& WorkingMesh, const FVertexMaskForgeDynamicLayerStack& Stack, TConstArrayView<FColor> BaseColors, TArray<FColor>& OutComposedColors) -> bool`, in its own new file pair (`VertexMaskForgeDynamicSourceTopologyComposition.h/.cpp`) and own new namespace, deliberately outside `SVertexMaskForgePanel.cpp` and distinct from the pre-existing Legacy `VertexMaskForgePanel::ComputeComposedColorsRGBSourceTopology` (same reserved name, different namespace — no ambiguity at any call site, and this new function has no production caller of any kind yet). It folds `Stack`'s layers strictly in array order over `BaseColors`, mirroring `VertexMaskForgeDynamicLayerEvaluator::EvaluateColor`'s own per-layer semantics (Fill resolution via the same `TryResolveFillValue`, masked-layer `EffectiveMask` multiplication, `BlendMode`/`Opacity` fold via the same `VertexMaskForgeSequentialEvaluator::EvaluateFillLayerStep`, per-layer Channel Filter, one final `Clamp01`, Alpha carried unconditionally from `BaseColors`) — but resolves a masked layer's `EffectiveMask` from a freshly, directly generated `VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskFromDynamicMesh` array (the M16-K.6D-3 caller-owned API, called directly, never through any stored wrapper) rather than a `FVertexMaskForgeInstanceResultStore` lookup — this orchestrator never receives, constructs, or touches a `FVertexMaskForgeInstanceResultStore` of any kind. Scope is deliberately restricted to a Material-Slot-only vertical slice: an **enabled** layer whose `Mask` is set to any generator type other than `MaterialSlot` fails the **whole call** explicitly (never silently skipped, never treated as Fill-only); a **disabled** layer's `Mask` is never even inspected, exactly mirroring `EvaluateColor`'s own `bEnabled==false` no-op contract. Validation is all-or-nothing: `WorkingMesh.Mesh` must be valid, `BaseColors.Num()` must equal exactly `Mesh.TriangleCount() * 3` (no resize, no partial output), and every enabled masked layer's generation call must return `Ready` at the expected cardinality — any failure leaves `OutComposedColors` **completely untouched** (whatever it held before the call), never partially written. An empty `Stack` succeeds trivially as a byte-exact passthrough of `BaseColors`. Proven by 13 new direct tests (`VertexMaskForge.DynamicSourceTopologyComposition.*`), including an order-of-composition proof (two Fill-only layers with opposite values, added in each of the two possible orders, independently-computed expected results) and structural non-mutation/no-store proofs. **This orchestrator is not connected to preview, to the K.6D-2 visual seam (`ApplySuppliedSourceTopologyPreviewColors`), or to `PreviewSource`** — it has no production caller of any kind; only its own Automation tests call it. M16-K.6D-5 (functional wiring) remains **planned**, not implemented.

**Contract established (M16-K.6D-5):** `PreviewSource` now has its first functional read/write anywhere in production code, and `EVertexMaskForgePreviewSource::Dynamic` is, for the first time, actually shown in the viewport (Source Topology domain, Material Slot only). A minimal `SComboBox<TSharedPtr<EVertexMaskForgePreviewSource>>` (label "Preview Source", options "Legacy"/"Dynamic", default `Legacy`, mirroring the pre-existing Preview Mode combo's own shape) is `PreviewSource`'s sole writer (`SVertexMaskForgePanel::OnPreviewSourceSelectionChanged`); selecting a new value immediately calls the panel's existing `RecomposeWorkingColors()` choke point. `ApplyPreviewToEntry`'s Source-Topology branch reads `PreviewSource` live, once per component, at the very top of the domain block — **before** any Legacy per-component generator work (Bounding Box/AO/Directional Normal World-Space re-evaluation), so a degenerate/failed Legacy generator can never block or affect the Dynamic branch and vice versa:
- **`PreviewSource == Legacy`** (default): the pre-existing composition code is **byte-for-byte unchanged** — same `ComputeComposedColorsRGBSourceTopology` (Legacy, `VertexMaskForgePanel` namespace), same `ApplyComposedColorsRGB`, same seam call supplying `StateOwner->GetWorkingColors()`.
- **`PreviewSource == Dynamic`**: a structurally separate branch calls `VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology` (the M16-K.6D-4 orchestrator, unmodified) directly, with `WorkingMesh`, the panel's own `DynamicLayerStack`, and `StateOwner->GetBaselineColors()` (never `GetWorkingColors()`/`GetCommittedColors()`, which belong to the Legacy lifecycle only) as inputs. On success, the orchestrator's fresh, local `TArray<FColor>` is fed directly to the unmodified M16-K.6D-2 seam (`ApplySuppliedSourceTopologyPreviewColors`) — never `ApplyComposedColorsRGB`, never any color-state array, never any store. On failure (unsupported masked generator, invalid input), `PreviewSource` is **never** silently reverted to `Legacy` (no automatic fallback, per ADR-011) — the affected component's real, original appearance is restored via the existing `RestorePreviewVisualOnly`, and `LastOperationErrorText` reports the failure factually so the status line communicates it while `Dynamic` remains selected.

Every `FVertexMaskForgeDynamicLayerStack` mutator the panel's own Dynamic Layers UI already exposed (`SetLayerFill`/`SetLayerBlendMode`/`SetLayerOpacity`/`SetLayerEnabled`/`SetLayerChannelFilter`/`SetLayerMaskGeneratorType`/`ClearLayerMask`/`SetLayerMaskParams`, plus reorder/add/remove) now additionally calls a new single choke point, `OnDynamicLayerStackMutated()`, which requests a refresh (`RecomposeWorkingColors()`) **only** when `PreviewSource == Dynamic` — editing the Dynamic stack while Legacy is being previewed triggers no recomposition at all, and every one of those edits now updates the Dynamic presentation live while it is selected.

**Accept was Legacy-only through M16-K.6D-6; superseded by M16-K.6D-7B (see below) — Dynamic Accept now exists.** Through M16-K.6D-6, `CanAcceptChanges()` additionally required `PreviewSource == Legacy` and `AcceptPendingChanges()` had a matching defensive early-out for Dynamic. As of M16-K.6D-7B, both are source-aware instead of Dynamic-blocking: see the M16-K.6D-7A/7B paragraph below for the current contract.

**Not covered by any Automation test in this codebase:** the panel-level routing above (`PreviewSource` default/read/write, the `ApplyPreviewToEntry` branch, `OnDynamicLayerStackMutated`, `CanAcceptChanges`/`AcceptPendingChanges`'s Dynamic gate) is entirely private `SVertexMaskForgePanel` code, and no test anywhere in this codebase constructs `SVertexMaskForgePanel` or a live Preview Component (same boundary M16-K.6D-2's own tests already documented). The M16-K.6D-4 orchestrator (13 tests) and the M16-K.6D-2 seam (6 tests) — both unmodified by this checkpoint and both re-confirmed still passing — remain the full extent of this feature's automated proof; the panel-level wiring itself is deferred to M16-K.6D-6's manual validation, per this checkpoint's own explicit scope.

M16-K.6D-5 remains scoped exactly as before: Material Slot is still the only Dynamic generator; the Render Vertex domain has no Dynamic seam; Global Channel Filter/Fill remain Legacy-only and are not yet gated off in the UI while `PreviewSource == Dynamic` (still **planned**, per ADR-011).

**Contract established (M16-K.6D-6, manual validation and defect correction):** the M16-K.6D-1 through -5 vertical slice was independently audited and manually validated end-to-end in the Editor (Groups A–D of the checkpoint's own protocol: session/default behavior, Dynamic Fill, Dynamic Material Slot mask, and stack mutation/sequential order — all PASS). Manual validation surfaced two real defects and one false alarm, all in panel-only UI code, none in the K.6D-4 orchestrator or the K.6D-2 seam (both remain unmodified since M16-K.6D-4/-2 respectively, and both remain fully covered by their own existing automated tests):

- **Reported Opacity "plateau" — investigated, found NOT to be a defect.** A numeric regression test (`VertexMaskForge.DynamicSourceTopologyComposition.CopyBlendOpacityIsContinuousAndMonotonicAcrossFullRange`) proved the orchestrator's own Copy-blend output is continuous and strictly monotonic across the full `[0,1]` Opacity range (independently-computed expected byte values, exact match). The original visual report was a misinterpretation, confirmed by the reporter's own retest. No production code was changed; the regression test is retained permanently.
- **Generator Type dropdown had no reachable "None" row.** Root cause (confirmed directly against UE 5.8 engine source, `Widgets/Views/SListView.h`): `SListView`'s own row-generation loop unconditionally skips any option that fails `TListTypeTraits::IsPtrValid` — a null `TSharedPtr`, the representation the M16-K.6B checkpoint originally chose for "None", can **never** render as a selectable row in any `SComboBox`, in any circumstance. Fixed by changing `DynamicLayerGeneratorTypeOptions`' element type to `TSharedPtr<TOptional<EVertexMaskForgeGeneratorType>>` — "None" is now a *valid* pointer to an *unset* `TOptional`. `EVertexMaskForgeGeneratorType` (no None enumerator) and `FVertexMaskForgeDynamicLayerStack`/`ClearLayerMask` are unchanged — this is a Slate-list-rendering fix local to one combo's option representation. A second, related defect (the combo's `InitiallySelectedItem` was hardcoded to "None" on every row rebuild, desyncing Slate's own internal selection tracking from the layer's real state per `SComboBox::OnSelectionChanged_Internal`'s reselection-suppression rule in `Widgets/Input/SComboBox.h`) was fixed the same way for both the Generator Type and Fill combos: `InitiallySelectedItem` is now resolved from the layer's actual current state at row-construction time, never hardcoded to array index 0.
- **Dynamic Layers panel showed inverted composition priority.** Root cause: `RebuildDynamicLayersList` displayed `DynamicLayerStack.GetLayers()` in forward array order (index 0 at the top), while `VertexMaskForgeDynamicLayerEvaluator::EvaluateColor` and the K.6D-4 orchestrator both fold strictly in that same array order (index 0 first, so the *highest* array index — composited last — wins Copy-mode conflicts) — an established, tested, unmodified contract. The layer shown at the top of the panel therefore had the *lowest* composition priority, opposite of standard painter/layer-stack convention. Fixed by reversing only the panel's own display order (`RebuildDynamicLayersList` now walks the array from the last index to the first) — `FVertexMaskForgeDynamicLayerStack`'s stored array order and the evaluator/orchestrator's fold direction are unchanged.
- **UI corrections requested alongside the above, applied in the same pass:** (1) the Fill combo no longer offers `EVertexMaskForgeLayerFill::None` as a user-facing option (Enabled is the sole disable mechanism now) — `AddLayer`'s own domain default (`Fill=None`, protected by `VertexMaskForgeDynamicLayerStackTests.cpp`) is unchanged; `OnAddDynamicLayerClicked` now calls the existing `SetLayerFill` mutator to resolve a newly added layer to `White` immediately. (2) The per-layer Up/Down buttons were replaced with vertical drag-and-drop reordering, implemented via UE 5.8's own `SDragAndDropVerticalBox`/`FDragAndDropVerticalBoxOp` (SlateCore, `Widgets/SBoxPanel.h` — the same mechanism the Editor's own Landscape Edit Layers stack uses), translating the visually-dropped position through the same reversed-display mapping into a single `FVertexMaskForgeDynamicLayerStack::MoveLayer(LayerId, NewIndex)` call (the existing general "move to an arbitrary final index" seam — `MoveLayerUp`/`MoveLayerDown`, single-step-only, cannot express an arbitrary-distance drag in one action). `MoveLayerUp`/`MoveLayerDown` themselves remain in `FVertexMaskForgeDynamicLayerStack` and fully tested; only their now-unused panel-side UI-glue wrappers were removed.

**Automated coverage:** one new orchestrator-level regression test (14 total in the `DynamicSourceTopologyComposition` group). The panel-level fixes above (combo option representation, display order, drag-and-drop) are entirely `SVertexMaskForgePanel`-private UI code with no non-panel public seam to test against, and drag-and-drop is pure Slate interaction — consistent with every prior K.6D checkpoint's own established automation boundary (no test in this codebase constructs `SVertexMaskForgePanel` or a live Preview Component), these were validated **manually** instead, per this checkpoint's own protocol.

**Still not implemented (unchanged from M16-K.6D-5), independent of Dynamic Accept below:** Global Channel Filter/Fill gating while `PreviewSource == Dynamic` and the Render Vertex domain. Dynamic generator support beyond Material Slot, Bounding Box, and Directional Normal — see the M16-K.6D-8B/8C/8D paragraphs below.

**Contract established (M16-K.6D-7A, non-panel builder seam):** a new, testable Dynamic Source-Topology accept-target builder now exists — `VertexMaskForgeDynamicAcceptTargetBuilder::BuildSourceTopologyAcceptTargets(const TArray<TSharedPtr<FVertexMaskForgeSelectedMesh>>& SelectedMeshes, const FVertexMaskForgeDynamicLayerStack& DynamicLayerStack, TArray<VertexMaskForgeAcceptTargetBuilder::FSourceTopologyAcceptTarget>& OutTargets, FText& OutErrorText) -> bool`, in its own new file pair (`VertexMaskForgeDynamicAcceptTargetBuilder.h/.cpp`). It mirrors `VertexMaskForgeAcceptTargetBuilder::BuildSourceTopologyAcceptTargets`'s own output type and all-or-nothing/per-component-conflict contract, but sources its per-component result from the M16-K.6D-4 orchestrator (`VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology`, called directly, no reimplemented blend/mask math) fed by each component's own `GetBaselineColors()` — **never** `WorkingColors`/`SourceTopologyWorkingColors`/`CommittedColors`, satisfying ADR-011's structural-isolation requirement (this builder is a separate function from, and never calls, either Legacy `VertexMaskForgeAcceptTargetBuilder` builder). An empty or all-disabled `DynamicLayerStack` is a *valid computation* here (byte-exact Baseline passthrough) — this is a computational contract only, distinct from the UI-eligibility contract M16-K.6D-7B adds below. On failure, `OutTargets` is left **completely unchanged** from its caller-supplied value (built into a local array, published only on full success — unlike the Legacy builder, which unconditionally `Reset()`s its output first). Reuses `VertexMaskForgeAcceptTargetBuilder::ValidateSourceTopologyCorrespondence` verbatim for triangle/corner correspondence. Proven by 14 new direct tests (`VertexMaskForge.DynamicAcceptTargetBuilder.*`), including byte-exact parity against a direct orchestrator call, same-asset agreement/divergence, and non-mutation of `DynamicLayerStack`/Baseline/Working/Committed colors. **No production caller existed as of M16-K.6D-7A** — `SVertexMaskForgePanel` was untouched, and `CanAcceptChanges()`/`AcceptPendingChanges()` still unconditionally rejected Dynamic.

**Contract established (M16-K.6D-7B, panel wiring — Dynamic Accept is now production-reachable; manual Editor validation still pending):** `RecomputeOperationState()` is now source-aware. Legacy's branch is byte-for-byte unchanged (the same `GeneratorState.*.Ready` scan). Dynamic's branch computes pending-ness from `bHasSelection && DynamicLayerStack.HasAnyEnabledLayer()` — a live `PreviewComponent` on at least one selected entry, **and** `FVertexMaskForgeDynamicLayerStack::HasAnyEnabledLayer()` (new, M16-K.6D-7B), a small query distinct from `!IsEmpty()`: an empty stack, or a non-empty stack with every layer disabled, both report no enabled layer and therefore never produce Dynamic pending changes. `CanAcceptChanges()` no longer special-cases `PreviewSource` at all (`OperationState == PendingChanges` is sufficient, since `OperationState` is itself now source-aware). `AcceptPendingChanges()`'s target-construction step is now a source branch: Legacy calls its original two builders unchanged; Dynamic calls **only** `VertexMaskForgeDynamicAcceptTargetBuilder::BuildSourceTopologyAcceptTargets` (passing the live `DynamicLayerStack`), with `Targets` (render-vertex) left empty since Dynamic is Source-Topology only. Both branches fully validate before the shared confirmation dialog, the shared single `FScopedTransaction`, and the shared `VertexMaskForgeAcceptWriter::WriteSourceTopologyAcceptTargets`/`BuildModifiedMeshes`/success-teardown path — all reused completely unmodified and source-agnostic, so a Dynamic Accept's write, transaction, Undo/Redo, dirtying, render rebuild, and session-end behavior are identical in mechanism to Legacy's. The confirmation and status text were already source-neutral and required no wording change. No Dynamic Instance Override path exists or was added — this codebase has no distinct Instance Override control at all (removed entirely by an earlier checkpoint); the single Accept path now covers both sources via `FSourceTopologyAcceptTarget` only. **Automated coverage** is necessarily partial: `HasAnyEnabledLayer()` (the new decision primitive) has 3 new direct tests; the M16-K.6D-7A builder's own 14 tests and the Legacy writer/transaction machinery are unmodified and re-confirmed passing; `RecomputeOperationState`/`CanAcceptChanges`/`AcceptPendingChanges` themselves remain entirely private `SVertexMaskForgePanel` code with no automatable seam in this codebase (same established boundary as every prior K.6D checkpoint — no test anywhere constructs `SVertexMaskForgePanel` or a live Preview Component). Dynamic Accept's own manual Editor validation subsequently passed (M16-K.6D-7B).

**Contract established (M16-K.6D-8B, orchestrator support for Local-space Bounding Box):** `VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology` now accepts a second masked-generator type, `BoundingBox`, alongside `MaterialSlot` — Local-space only. Its Pass 1 dispatches by `GeneratorType`: `MaterialSlot` unchanged (corner-domain, via `GenerateMaterialSlotMaskFromDynamicMesh`); `BoundingBox` validates `bUseUnifiedBounds == false` and no enabled axis has `bWorldSpace == true` *before* calling the existing, unmodified `VertexMaskForgeBoundingBoxGenerator::GenerateBoundingBoxMaskFromDynamicMesh` — either unsupported flag fails the whole call deterministically (never silently reinterpreted as Local Space or per-mesh bounds). Because that generator's output is indexed by Dynamic Mesh VertexID (not corner), each generated mask is now tagged with its own domain (`Corner` or `DynamicMeshVertex`, a private orchestrator-local enum) and vertex-domain masks are resolved per corner through that corner's own triangle (`Mesh.GetTriangle(TriangleID)[Corner]`), read only via `FVertexMaskForgeScalarMask::TryGetValue` — mirroring, but never calling, the same per-generator domain distinction Legacy's own Source-Topology composition already makes. World Space and Unified Bounds remain unsupported by design (no per-component transform, no full-selection context reaches this orchestrator) — proven by two dedicated rejection tests. 9 new orchestrator tests total; `VertexMaskForgeDynamicAcceptTargetBuilder` required zero changes (already fully generator-agnostic).

**Contract established (M16-K.6D-8C-B, obsolete panel removal):** the standalone `Layers` presentation section (a fixed Legacy-generator list with Up/Down reorder buttons, built during an earlier, since-superseded UI attempt — never the same as `Dynamic Layers`) was removed from `SVertexMaskForgePanel::Construct()`, along with its exclusively-owned helpers (`RebuildGeneratorLayerList`, `BuildGeneratorLayerRow`, `CanMoveGeneratorLayerUp/Down`, `OnMoveGeneratorLayerUp/Down`, `GeneratorLayerListContainer`). **`GeneratorLayerOrder` itself was retained unchanged** — it remains the real order Legacy composition reads at both of its production call sites; only its now-obsolete UI wrapper was deleted. The `Dynamic Layers` panel and every Legacy generator expander were unaffected.

**Contract established (M16-K.6D-8C-C, Local-space Bounding Box is user-accessible in Dynamic Layers; manually validated PASS):** `Bounding Box` is now a selectable entry in the Dynamic Generator Type combo (`DynamicLayerGeneratorTypeOptions`), alongside `MaterialSlot`. Selecting it uses the existing `SetLayerMaskGeneratorType` contract unchanged — a fresh default `FVertexMaskForgeBoundingBoxParams` (all axes disabled, Local-space, Unified Bounds off), discarding any prior variant, exactly like every other generator switch. Each Dynamic Layer row now has its own **`Generator Parameters`** `SExpandableArea`, visible only when the layer's generator actually has Dynamic-exposed parameters (`MaterialSlot` or `BoundingBox` today; a Fill-only/`None` layer shows no expander) — belonging to that one layer, never a shared panel-level control, and never the panel removed in M16-K.6D-8C-B. The Material Slot editor moved inside this expander with byte-for-byte unchanged mutation semantics. Bounding Box exposes, per axis (X/Y/Z): Enabled, Invert, Position (`[0,1]`), Falloff (label only — the underlying field remains `TransitionWidth`, `[0.001,1]`), Mirror — visually mirroring the Legacy `BuildBoundingBoxAxisRow` layout/ranges, but every control reads/writes only that specific `LayerId`'s own stored parameters via a new `MutateDynamicBoundingBoxAxisParam` helper, which mirrors the Material Slot picker's own six-step identity-validated write path (mask exists, `GeneratorType == BoundingBox`, `Params` is the Bounding Box payload type, `MaskInstanceId` matches the id this row was built for) before copying-mutating-one-field-and-writing back through `SetLayerMaskParams` + `OnDynamicLayerStackMutated()`. No World Space or Unified Bounds control exists anywhere in this UI. If a layer's *already-stored* data requests either (unreachable through this UI, but not otherwise precluded), a concise warning appears and every axis control disables itself — `IsDynamicBoundingBoxLayerLocalSpaceCompatible` is a read-only echo of the orchestrator's own rejection predicate, never a second enforcement point, never a silent normalization. No shared/panel-level Bounding Box state was added — only four new private `SVertexMaskForgePanel` methods. Manually validated end-to-end: independent two-layer parameter state, reorder/deletion preserving correct layer identity, generator-switch reset behavior, live preview, Accept/Cancel, and no Legacy regression.

**Contract established (M16-K.6D-8D-B, orchestrator support for Local-space Directional Normal):** `VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology` now accepts a third masked-generator type, `DirectionalNormal`, alongside `MaterialSlot` and `BoundingBox` — Local-space only. Its Pass 1 dispatch validates `Space == EVertexMaskForgeNormalSpace::Local` *before* calling the existing, unmodified `VertexMaskForgeDirectionalNormalGenerator::GenerateDirectionalNormalMaskFromDynamicMesh` with all six authoritative fields (`Direction`/`Angle`/`Falloff`/`Blur`/`bInvert` unchanged, `FTransform::Identity` supplied only after Local-space is confirmed, since Local-space evaluation never reads it) — World Space fails the whole call deterministically, never silently reinterpreted as Local. The generator's output is already corner-domain, like Material Slot's, so it is tagged `ELayerMaskDomain::Corner` and resolved through the existing direct Pass 2 corner-index read, unchanged — no new domain, no vertex remapping. Because an individual corner may legitimately be left unwritten by the generator (missing Normal Overlay element or degenerate normal) while the mask as a whole still reports `Ready`, Pass 1 additionally requires `NumValidValues` to equal the full expected corner count before accepting the mask, since the corner-domain read is a direct, unconditional index rather than a `TryGetValue` lookup. 5 new orchestrator tests total; `VertexMaskForgeDynamicAcceptTargetBuilder` required zero changes (already fully generator-agnostic).

**Contract established (M16-K.6D-8D-C, Local-space Directional Normal is user-accessible in Dynamic Layers; manually validated PASS):** `Directional Normal` is now a selectable entry in the Dynamic Generator Type combo, alongside `MaterialSlot` and `BoundingBox`. Selecting it uses the existing `SetLayerMaskGeneratorType` contract unchanged — a fresh default `FVertexMaskForgeDirectionalNormalParams` (Local-space, `PositiveZ`, `Angle=90`, `Falloff=45`, `Blur=0`, not inverted), discarding any prior variant. The layer's own `Generator Parameters` expander gained a third sibling block (`BuildDynamicDirectionalNormalLayerParamsBlock`) exposing exactly five artistic controls — Direction (combo, reusing the Legacy `NormalDirectionOptions`/label), Angle (`[0,180]`), Falloff (`[0,Angle]`, its live maximum bound to the layer's own current Angle), Blur (`[0,10]`), Invert — with `Falloff <= Angle` maintained at the UI mutation boundary only (reducing Angle clamps a larger stored Falloff down to match; a newly requested Falloff is clamped to the current Angle). `Space` is deliberately not exposed anywhere in this block — it remains model-owned and hidden, since this checkpoint supports Local Space only. Every control reads/writes only that specific `LayerId`'s own stored parameters via a new `MutateDynamicDirectionalNormalParam` helper, mirroring the Bounding Box axis helper's own six-step identity-validated write path (mask exists, `GeneratorType == DirectionalNormal`, `Params` is the Directional Normal payload type, `MaskInstanceId` matches the id this row was built for) before copying-mutating-and-writing back through `SetLayerMaskParams` + `OnDynamicLayerStackMutated()`. If a layer's *already-stored* data requests World Space (unreachable through this UI, but not otherwise precluded), a concise warning appears and every control in the block disables itself — `IsDynamicDirectionalNormalLayerLocalSpaceCompatible` is a read-only echo of the orchestrator's own rejection predicate, never a second enforcement point, never a silent normalization. No shared/panel-level Directional Normal state was added — only three new private `SVertexMaskForgePanel` methods. No evaluator math was copied into Slate; every edit still flows through the same orchestrator preview and Accept both already share. Manually validated end-to-end (items A–G): selector/defaults, live parameter behavior including the Angle/Falloff invariant and genuinely-nonzero Blur, independent two-layer parameter state with reorder, stable-`LayerId` deletion safety, generator switching, preview/Accept parity, and no Legacy regression.

---

## 3. Domain Model

### Render Vertex Domain

- **Cardinality:** number of render vertices in the selected LOD (`FStaticMeshLODResources`, typically LOD0).
- **Identity:** a plain positional index into the LOD's vertex buffers — one entry per render-buffer vertex, not per unique geometric position; UV/normal/tangent/material-boundary splits routinely duplicate a single geometric position across several render-buffer vertices.
- **Storage:** `FVertexMaskForgeWorkingMesh` references the LOD's own render buffers (positions, normals, material-slot-per-triangle resolved to per-vertex where unambiguous).
- **Baseline:** `BaselineColors` (`FVertexMaskForgePreviewComponentState`).
- **Used when:** the source Static Mesh is a conventional (non-Nanite, or Nanite-with-fallback-editable) asset being edited via its render LOD.

### Source Topology Domain

- **Cardinality:** `TriangleCount * 3` — one entry per triangle **corner**, not per shared vertex.
- **Identity:** a corner index; two corners of two different triangles sharing the same vertex position are still distinct entries.
- **Storage:** `FVertexMaskForgeWorkingMesh::Mesh` (`UE::Geometry::FDynamicMesh3`), built for Nanite/dynamic-mesh editing paths.
- **Baseline:** `SourceTopologyBaselineColors` (`FVertexMaskForgePreviewComponentState`).
- **Used when:** the working geometry is represented as a `FDynamicMesh3` (Nanite meshes, or any path that chose corner-exact resolution over shared-vertex resolution).

### Why domains cannot be implicitly combined

**Known Limitation.** `FVertexMaskForgeInstanceMaskResult` (the payload stored per `MaskInstanceId` in `FVertexMaskForgeInstanceResultStore`) carries no domain tag, and `FVertexMaskForgeInstanceResultStore`'s key is a bare `FGuid`. Nothing in the type system prevents generating a source-topology result and a render-vertex result under the *same* `MaskInstanceId` — the second `StoreOrReplace` call would silently overwrite the first. This was identified and explicitly deferred during the M16-K.5I audit sequence; every checkpoint that touches this store must choose one domain per `MaskInstanceId` by convention, not by any structural guarantee.

### Domain comparison

| | Render Vertex | Source Topology |
|---|---|---|
| Cardinality | Render vertex count | `TriangleCount * 3` |
| Identity granularity | Per render-buffer vertex; geometric positions may be duplicated at attribute seams | Per triangle corner |
| Geometry source | `FStaticMeshLODResources` | `FDynamicMesh3` |
| Baseline array | `BaselineColors` | `SourceTopologyBaselineColors` |
| Committed array | `CommittedColors` | `SourceTopologyCommittedColors` |
| Working array | `WorkingColors` | `SourceTopologyWorkingColors` |
| Typical use case | Conventional render LOD editing | Nanite / dynamic-mesh editing |
| Cross-domain result reuse | Not supported (no domain tag on stored results) | Not supported (same reason) |

---

## 4. Ownership and State

**Current Contract**, confirmed by direct source read:

- **`FVertexMaskForgeWorkingMesh`** — the working geometry payload for one asset entry: the `FDynamicMesh3` (source-topology domain), material-slot-per-triangle mapping, and its own `InstanceResults` store (per-entry, shared across every component of that asset). Owned by exactly one `FVertexMaskForgeWorkingMeshOwner` at a time, installed via `InstallWorkingMesh`.
- **`FVertexMaskForgeWorkingMeshOwner`** — owns geometry **identity** (`ConfiguredStaticMesh`, `LODIndex`, `bUseSourceTopology`) and the currently-installed `FVertexMaskForgeWorkingMesh`, plus a `Provenance` record (`MeshOwnerId`, `Revision`/generation, `ExpectedCardinality`, domain flag) that every attached `FVertexMaskForgeWorkingStateOwner` authenticates against. `GetWorkingMesh()` is **const-only** — there is no mutable accessor; a new/regenerated `FVertexMaskForgeWorkingMesh` must be built standalone and moved in via `InstallWorkingMesh`.
- **`FVertexMaskForgeWorkingStateOwner`** — one instance per placed component (`FVertexMaskForgePreviewComponentState`). Owns that component's `BaselineColors`/`CommittedColors`/`WorkingColors` (and their `SourceTopology*` siblings) and the authoritative alpha metadata over `WorkingColors`. Attaches (weakly) to a `FVertexMaskForgeWorkingMeshOwner`; never owns geometry itself.
- **`FVertexMaskForgeDynamicLayerStack`** — a single, panel-global instance (`SVertexMaskForgePanel`'s own member), not per-entry and not per-component. Owns the ordered list of `FVertexMaskForgeLayer` and their optional `FVertexMaskForgeGeneratorMaskInstance`.
- **`InstanceResults`** (`FVertexMaskForgeInstanceResultStore`) — has **two** real owners: `FVertexMaskForgeWorkingMesh::InstanceResults` (per-entry/asset, intended for transform-independent generators) and `FVertexMaskForgePreviewComponentState::InstanceResults` (per-component, intended for transform-dependent generators). No generic dispatcher chooses between them today — a caller must know which one applies (see [§16 Known Limitations](#16-known-limitations-and-risks)).
- **`LayerId`** (`FVertexMaskForgeLayer::LayerId`, `FGuid`) — identifies a layer's position/configuration inside a `FVertexMaskForgeDynamicLayerStack`. Assigned once by `AddLayer`, never regenerated by rename/move.
- **`MaskInstanceId`** (`FVertexMaskForgeGeneratorMaskInstance::MaskInstanceId`, `FGuid`) — identifies one generator's stored result in `InstanceResults`, independent of `LayerId`. A layer's mask can be replaced with a different generator type, minting a fresh `MaskInstanceId` while `LayerId` stays the same.

### State ownership table

| State/Data | Owner | Domain | Mutability | Main consumer |
|---|---|---|---|---|
| `Mesh` (`FDynamicMesh3`) | `FVertexMaskForgeWorkingMesh` | Source Topology | Rebuilt wholesale on refresh | Source-topology generators, `ComposeColors` result-store domain |
| `InstanceResults` (per-entry) | `FVertexMaskForgeWorkingMesh` | Either (caller-chosen) | Replaced per `MaskInstanceId` via `StoreOrReplace` | `EvaluateColor` / `ComposeColors` |
| `InstanceResults` (per-component) | `FVertexMaskForgePreviewComponentState` | Either (caller-chosen) | Same as above | Same as above |
| `BaselineColors` / `SourceTopologyBaselineColors` | `FVertexMaskForgeWorkingStateOwner` | Matches its own domain flag | Captured once (`InitializeColors`/`EnsureBaselineCaptured`), invalidated on reattach/rebuild | Legacy composition base; Dynamic batch composition base |
| `CommittedColors` / `SourceTopologyCommittedColors` | `FVertexMaskForgeWorkingStateOwner` | Matches its own domain flag | Promoted from `WorkingColors` only via `ApplyComposedColorsRGB(..., Consolidate)` | Global Channel Filter fallback |
| `WorkingColors` / `SourceTopologyWorkingColors` | `FVertexMaskForgeWorkingStateOwner` | Matches its own domain flag | Written by `ApplyComposedColorsRGB` (explicit composed-output application), and by the restoration operations `RestoreFromBaseline`/`RestoreFromCommitted` — never by any pure composition function | Accept target; display derivation input |
| Batch-composed local output | Local to the caller (`TArray<FColor>&` out-parameter) | Caller-chosen | Freshly computed every call, never stored on any owner | Nothing yet in production (Dynamic path only) |
| Display colors | Local, transient | Matches `WorkingColors`' domain | Recomputed every call, never persisted | Viewport visualization only |

---

## 5. Color State Lifecycle

**Current Contract.**

- **Captured/Baseline** — captured once per component, from whichever source is authoritative first (an existing per-instance vertex color override, then the asset's own baked colors, then white). `BaselineColors`/`SourceTopologyBaselineColors` are immutable after capture until the owner is reset or reattached to a different/rebuilt mesh owner.
- **Committed** — still exists in the current model. `CommittedColors`/`SourceTopologyCommittedColors` serve as the fallback value for any channel the global Channel Filter has disabled (see [§9](#9-channel-semantics)), and are promoted to match `WorkingColors` only when `ApplyComposedColorsRGB` is called with `EColorCommitMode::Consolidate` (the real panel's own Fill Black/White path).
- **Working** — the live preview state. `FVertexMaskForgeWorkingStateOwner::ApplyComposedColorsRGB` is the explicit application point for a freshly composed output: it forces Alpha from the current `BaselineColors` regardless of what the caller supplied, validates cardinality, and optionally promotes `CommittedColors` (`Consolidate` mode) or leaves it untouched (`PreviewOnly` mode). `WorkingColors` is also written by the restoration operations `RestoreFromBaseline` (copies from `BaselineColors`, non-consolidating) and `RestoreFromCommitted` (copies from `CommittedColors`, non-consolidating) — these are owner-state mutations in their own right, distinct from composition. What never writes `WorkingColors` is any *pure composition function* (see below).
- **Composed output (local)** — both `VertexMaskForgeGeneratorLayerBridge::ComposeGeneratorLayersSequential` (legacy) and `VertexMaskForgeDynamicLayerBatchCompositor::ComposeColors` (Dynamic) are **pure**: they compute and return a result without mutating any owner state. Composition stays pure until one of the explicit owner-state mutation operations above (`ApplyComposedColorsRGB`, `RestoreFromBaseline`, `RestoreFromCommitted`) is called. `ComposeColors` in particular is explicitly proven (M16-K.5J) to never write `WorkingColors` and to leave `BaselineColors` untouched.
- **Display colors** — `VertexMaskForgeDisplayColorDerivation::DeriveDisplayColors(WorkingColors, PreviewMode)` reduces `WorkingColors` for on-screen visualization (e.g. isolating a single channel). It is stateless, never mutates `WorkingColors`, and its output is applied only to the viewport render state, never stored back into any owner.
- **Accept** — reads `WorkingColors`/`SourceTopologyWorkingColors` directly (never through `DeriveDisplayColors`) and writes into the real `UStaticMesh`'s source `FMeshDescription` vertex-instance colors, inside a single `FScopedTransaction`.
- **Cancel** — does not revert `WorkingColors` to any prior value; it discards the entire session's owning objects (see [§12](#12-persistence-boundaries)).

**Do not confuse** `ComposeColors`/`ComposeGeneratorLayersSequential` (pure computation, no owner-state mutation) with `ApplyComposedColorsRGB`, `RestoreFromBaseline`, and `RestoreFromCommitted` (the explicit owner-state mutation operations that do write `WorkingColors`/`CommittedColors`).

---

## 6. Layer Model

Two distinct layer models coexist (see [§15](#15-current-integration-status) for which is production-active):

**Current Contract — legacy fixed model.** `SVertexMaskForgePanel`'s `GeneratorState` holds exactly seven named `FVertexMaskForgeScalarMask` fields (Bounding Box, Ambient Occlusion, Curvature, Noise, Material Slot, Directional Normal, Thickness). Their composition order is a `GeneratorLayerOrder` permutation (`VertexMaskForgeLayerOrder`), not an arbitrary list. Each legacy layer, as seen by the bridge, is a `FVertexMaskForgeMaskLayerParams` (`Mask`, `BlendMode`, `Opacity`, `IndexOverride`) — **it has no per-layer channel-affect field**.

**Current Contract — Dynamic model.** `FVertexMaskForgeLayer` (owned by `FVertexMaskForgeDynamicLayerStack`) has: `LayerId` (stable identity), `Name`, `bEnabled`, `Fill` (`None`/`Black`/`White`), `BlendMode` (7 modes, shared enum with the legacy model), `Opacity`, `bAffectRed`/`bAffectGreen`/`bAffectBlue` (per-layer channel participation), and an optional `Mask` (`TOptional<FVertexMaskForgeGeneratorMaskInstance>`) carrying `MaskInstanceId`, `GeneratorType`, and `Params`.

A Dynamic layer resolves its per-index result without depending solely on `GeneratorType`: `VertexMaskForgeDynamicLayerEvaluator` looks up the layer's own `Mask->MaskInstanceId` in the supplied `FVertexMaskForgeInstanceResultStore`; a layer with no `Mask` never consults the store at all and contributes only its `Fill` value. Layer identity (`LayerId`) and mask-instance identity (`MaskInstanceId`) are independent: replacing a layer's generator type via `SetLayerMaskGeneratorType` mints a fresh `MaskInstanceId` while `LayerId` is untouched; re-assigning the *same* generator type is a no-op that preserves the existing `MaskInstanceId`.

---

## 7. Generator Result Model

**Current Contract.** `FVertexMaskForgeInstanceMaskResult` holds exactly `Values` (`TArray<float>`) and `bHasValue` (`TBitArray<>`) — no domain tag, no identity (the store's key already provides that), no generator parameters. `FVertexMaskForgeInstanceResultStore` maps `FGuid` (`MaskInstanceId`) → one such result, replacing in place on `StoreOrReplace`.

Four distinct absence/coverage cases exist and are handled differently in code, though all four currently resolve to **zero coverage** at the evaluator:

1. **`MaskInstanceId` absent from the store entirely.** `EvaluateColor`/`ComposeColors` treat this as zero coverage.
2. **Index out of `Values` range for a present instance.** Also zero coverage.
3. **Index within range but `bHasValue[index] == false`.** Also zero coverage.
4. **A structurally valid, `Ready` result whose actual value at that index is `0.0` with `bHasValue == true`** (e.g. Material Slot's real output for a corner/vertex not on the selected slot). This is a legitimate zero-*contribution* result, not an absent one — it is `Ready`, not `Missing`.

**Not frozen by the current code/tests:** a `Pending` (in-progress) generation status. No field, flag, or async contract exists anywhere in `FVertexMaskForgeInstanceMaskResult`/`FVertexMaskForgeInstanceResultStore`; every generation call observed in this codebase is synchronous.

**Current Contract — FailedAfterReady policy.** `VertexMaskForgeDynamicMaskGeneration::GenerateStoredResultForMaterialSlotInstance` only calls `StoreOrReplace` when the underlying generator returns `Ready`. A second, genuinely-failing call for the same `MaskInstanceId` never touches the store — the previous `Ready` result (and everything downstream that reads it) survives completely intact.

---

## 8. Composition

### Legacy/Current Composition Path

- **Entry point:** `VertexMaskForgeGeneratorLayerBridge::ComposeGeneratorLayersSequential`.
- **Input:** a Baseline `FVector4f`, a Committed `FVector4f`, an index, an array of `FVertexMaskForgeMaskLayerParams` (resolved from the seven fixed `GeneratorState` masks via `GeneratorLayerOrder`), and the panel's global `bFilterR`/`bFilterG`/`bFilterB`.
- **Store consumed:** none — each layer's `Mask` pointer is a direct `FVertexMaskForgeScalarMask*`, not a keyed store lookup.
- **Domain:** whichever domain the caller's Baseline/Committed/index already belong to (render-vertex or source-topology) — the function itself is domain-agnostic.
- **Output:** a single composed `FVector4f`.
- **Side effects:** none — pure function.
- **Panel integration:** wired into both real call sites of `ApplyPreviewToEntry` (`ComputeComposedColorsRGB` for render-vertex, `ComputeComposedColorsRGBSourceTopology` for source-topology).
- **Tests:** `GeneratorLayerBridgeTests.cpp` (`EmptyLayers`, `UnresolvedLayerSkipped`, `SingleLayerNumeric`, `StrictArrayOrder`, `ChannelFilter`, `IndexOverride`, `AlphaUntouched`).

### Dynamic Layer Evaluation

- **Entry point:** `VertexMaskForgeDynamicLayerEvaluator::EvaluateColor`.
- **Input:** a Base `FVector4f`, a `FVertexMaskForgeDynamicLayerStack`, a `FVertexMaskForgeInstanceResultStore`, an index.
- **Store consumed:** yes — each masked layer's contribution is looked up by `MaskInstanceId` in the supplied store.
- **Domain:** agnostic; determined entirely by what the caller's store/index represent.
- **Output:** a single composed `FVector4f`.
- **Side effects:** none — pure function; never mutates `Stack` or `ResultStore`.
- **Panel integration:** none.
- **Tests:** `DynamicLayerEvaluatorTests.cpp` (27 tests covering Fill, blend modes, per-layer channel filtering, alpha preservation, ordering, and all four absence/coverage cases from [§7](#7-generator-result-model)).

### Dynamic Batch Composition

- **Entry point:** `VertexMaskForgeDynamicLayerBatchCompositor::ComposeColors`.
- **Input:** `TConstArrayView<FColor> BaseColors`, a `FVertexMaskForgeDynamicLayerStack`, a `FVertexMaskForgeInstanceResultStore`, an `OutColors` out-parameter.
- **Store consumed:** the same `FVertexMaskForgeInstanceResultStore` as above, applied once per element of `BaseColors` via `EvaluateColor`.
- **Domain:** agnostic; determined entirely by what `BaseColors`/`ResultStore` represent.
- **Output:** `OutColors`, wholesale-substituted with exactly `BaseColors.Num()` elements. Safe to call with `BaseColors` and `OutColors` aliasing the same array.
- **Side effects:** none — pure; never mutates `Stack` or `ResultStore`; **never writes `WorkingColors` or `BaselineColors`**.
- **Panel integration:** none.
- **Tests:** `DynamicLayerBatchCompositorTests.cpp` (4 tests) plus `VertexMaskForge.DynamicCompositionSourceTopologyIntegration.RealSourceTopologyBaselineAndRealStoredMaterialSlotResultProduceExpectedColorsAcrossCoveredAndUncoveredCorners` (M16-K.5J).

**M16-K.5J proved, end to end, with real production objects (no hand-built store, no fixture-only shortcuts):**

```
WorkingStateOwner source-topology baseline
  → DynamicLayerStack identity
  → real Material Slot generation
  → WorkingMesh.InstanceResults
  → MoveTemp/InstallWorkingMesh preservation
  → batch ComposeColors
  → byte-exact local output
```

`ComposeColors` does not alter `BaselineColors` or `WorkingColors` — proven both structurally (no such call exists in its implementation) and by explicit before/after array comparison in the M16-K.5J test.

---

## 9. Channel Semantics

**Current Contract — three independent mechanisms, not interchangeable:**

- **Per-layer `bAffectRed`/`bAffectGreen`/`bAffectBlue`** (`FVertexMaskForgeLayer`, Dynamic model only). Applied inside `EvaluateColorInternal`, per layer, per channel: when false for a given channel, that one layer simply does not fold its value into that channel, leaving whatever the prior layers already produced untouched. It never resets a channel to Base or Committed, and it has no legacy equivalent (`FVertexMaskForgeMaskLayerParams` carries no such field).
- **Global Channel Filter (`bChannelFilterR`/`bChannelFilterG`/`bChannelFilterB`)** — panel-level fields on `SVertexMaskForgePanel`, toggled by dedicated checkboxes, passed into both real composition call sites. Applied once, after the whole legacy layer stack has folded: a disabled channel's final value is replaced by the corresponding channel of `CommittedColors`; an enabled channel keeps the fold's own computed value. This mechanism has no Dynamic-model equivalent today.
- **Alpha preservation** — both composition paths leave Alpha untouched by the layer math itself; `ApplyComposedColorsRGB` additionally forces Alpha from the live `BaselineColors` unconditionally, regardless of what a caller's `FinalRGB` happened to carry.
- **Preview Mode / display-only derivation** — `EVertexMaskForgePreviewMode` (`OriginalMaterial`, `RGBVertexColor`, `RedChannel`, `GreenChannel`, `BlueChannel`, `AlphaChannel`) is consumed exclusively by `VertexMaskForgeDisplayColorDerivation::DeriveDisplayColors`, strictly downstream of `WorkingColors`. It never participates in composition and never mutates `WorkingColors`.

---

## 10. Generators

**Current Contract**, all seven fixed generators exist and support both domains at the generator-function level:

| Generator | `MaskGeneratorType` | Domains supported | Result | Dynamic Stack integration | Known limitations |
|---|---|---|---|---|---|
| Bounding Box | `BoundingBox` | Render-vertex + Source-topology | Pure position/bounds scalar | Not yet represented | Compatibility gap only — legacy generator is fully functional |
| Ambient Occlusion | `AmbientOcclusion` | Render-vertex + Source-topology | World-space hemisphere-raycast scalar | Not yet represented | Final mask value stored per-entry (asset) even though the raw raycast cache is per-component — an unresolved ownership tension (see [§16](#16-known-limitations-and-risks)) |
| Curvature | `Curvature` | Render-vertex + Source-topology | Topology-derived scalar, no raycast | Not yet represented | Compatibility gap only |
| Noise | `Noise` | Render-vertex + Source-topology | Position-derived procedural scalar | Not yet represented | Compatibility gap only |
| Directional Normal | `DirectionalNormal` | Render-vertex + Source-topology | Normal-vs-axis-direction scalar | **Implemented, Local-space only** (M16-K.6D-8D-B/8D-C) | World Space is authoritatively rejected, never silently reinterpreted as Local |
| Thickness | `Thickness` | Render-vertex + Source-topology | Asset-Local-Space raycast scalar | Not yet represented | Deliberately never uses `ComponentTransform` (differently-scaled instances would otherwise produce incompatible persisted results) |
| Material Slot | `MaterialSlot` | Render-vertex + Source-topology | Per-vertex/per-corner slot-membership scalar | **Implemented and test-proven** (`GenerateStoredResultForMaterialSlotInstance`) | Render-vertex Dynamic generation path is untested — every existing test/fixture for this function uses the source-topology overload only |

A generator not yet represented by the Dynamic Stack is **still a fully functional part of the tool** through the legacy path — this is a Dynamic-model compatibility gap, not an absence of the capability.

---

## 11. Preview and Display

**Current Contract.**

- **Auto Update Preview** — the panel debounces recomposition via a timer handle (`AutoUpdateDebounceTimerHandle`), cleared explicitly on Cancel.
- **Preview Mode** — see [§9](#9-channel-semantics); a pure display-time reduction of `WorkingColors`, never affecting composition or persistence.
- **Channel Filter's role at composition, not just display** — the global Channel Filter affects the *composed* `WorkingColors` itself (it feeds `ComputeComposedColorsRGB[SourceTopology]`, upstream of `ApplyComposedColorsRGB`), not merely what is displayed. It is not a display-only mechanism.
- **`DisplayColorDerivation`** — stateless, operates only on an already-composed `WorkingColors` buffer; never reads generator/cache/UObject state.
- **Visual restoration vs. state restoration** — these are different operations: `RestoreComponentOriginal` (invoked from Cancel's `DestroyAllPreviews`) restores a real component's visibility/material state, with no color-array involvement at all (no vertex-color write ever happened that would need reverting). `RestoreFromBaseline`/`RestoreFromCommitted` (on `FVertexMaskForgeWorkingStateOwner`) instead restore the **color arrays** (`WorkingColors`), independent of any component visibility concern.
- **Generation-failure behavior** — confirmed only for Material Slot's Dynamic generation path: a failed regeneration leaves the previously stored `Ready` result, and therefore the previously-composed preview, untouched (see [§7](#7-generator-result-model)). Broader failure-preservation behavior for the legacy generators is **not frozen by the current code/tests** as a general cross-generator contract in this document's evidence base.

---

## 12. Persistence Boundaries

| Operation | Reads | Writes | Persistent target | Undo/transaction behavior | Protected by |
|---|---|---|---|---|---|
| Accept | `WorkingColors` / `SourceTopologyWorkingColors` | `UStaticMesh` source `FMeshDescription` vertex-instance colors | The Static Mesh asset itself | One `FScopedTransaction` wraps both the render-vertex and source-topology writers; `Build()`/`Modify()` sequencing keeps rebuild outside the transaction, guarded via a `GUndo` suppression | Not frozen by tests — no dedicated Accept test found in the current suite |
| Cancel | Panel's own session bookkeeping only | Nothing (no color write occurs) | None | No transaction opened; the entire session object graph (`SelectedMeshes`) is discarded, not reverted from a snapshot | Not frozen by tests |
| Instance Override | — | — | — | Confirmed fully removed from production code (only an unrelated, legitimately-named `InstanceOverrideColors` baseline-capture parameter remains) | N/A — historical feature, removed |
| Save to Source Mesh | — | — | — | No distinct feature by this name exists in the current codebase; the closest real operation is Accept itself | N/A — not a real distinct feature today |

---

## 13. Cache and Invalidation Contracts

**Current Contract.** There is no single global "generation counter" governing every cache. Each generator's raw cache instead stores the specific identity/content fields its expensive build depended on, and recomputes when the *current* inputs no longer match:

- **Ambient Occlusion** (`FVertexMaskForgeAOCache` / `FVertexMaskForgeSourceTopologyAOCache`): geometric rebuild triggers on mesh identity, derived-data key, vertex/index counts, and transform mismatch (render-vertex domain); on mesh pointer identity plus a full geometry fingerprint hash (source-topology domain). A second, independent layer of the cache (raw sample values) rebuilds only when sampling parameters change or the geometric layer itself rebuilt.
- **Thickness** (`FVertexMaskForgeThicknessCache` / `FVertexMaskForgeSourceTopologyThicknessCache`): the same identity-based fast-reject as AO, **plus** a full value-by-value geometry snapshot comparison performed immediately before Accept's first mesh modification — the fingerprint alone is explicitly documented as insufficient proof of freshness for an irreversible write.
- **Curvature** (`CurvatureRenderVertexToDynamicMeshVertex`): a static correspondence table rebuilt whenever the working mesh itself is rebuilt (e.g. on Refresh Selection); not independently versioned.

Generator parameters that only affect the final scalar mapping (e.g. blur, levels, invert) recompose without invalidating the raw geometric cache; parameters that change what geometry/sampling produced the cache (transform, LOD, mesh identity) force a rebuild.

---

## 14. Test-Protected Boundaries

| Boundary | Representative test(s) | What is frozen |
|---|---|---|
| Baseline/Committed/Working independence | `VertexMaskForge.WorkingStateOwner.Restore` | `RestoreFromBaseline`/`RestoreFromCommitted` never cross-touch the other array |
| Alpha forced from Baseline on apply | `VertexMaskForge.WorkingStateOwner.ComposedColorsControlled` | `ApplyComposedColorsRGB` ignores the caller's own Alpha |
| Fill Black/White consolidation | `VertexMaskForge.WorkingStateOwner.FillBlackWhite` | R/G/B forced, Alpha preserved, Committed synced |
| Multi-component independence | `VertexMaskForge.WorkingStateOwner.TwoComponentsSharedMesh` | One shared mesh owner, fully independent per-component color buffers |
| Global Channel Filter → Committed fallback | `VertexMaskForge.GeneratorLayerBridge.ChannelFilter` | Disabled global channel = `CommittedColor`; enabled = computed fold |
| Legacy `IndexOverride` resolution | `VertexMaskForge.GeneratorLayerBridge.IndexOverride` | Explicit override takes precedence over the shared index |
| Dynamic per-layer channel isolation | `VertexMaskForge.DynamicLayerEvaluator.ChannelFilterSingleChannel`, `...ChannelFilterAllDisabled` | Per-layer `bAffect*` never resets a channel to Base/Committed |
| Missing `MaskInstanceId` → zero coverage | `VertexMaskForge.DynamicLayerEvaluator.MissingMaskInstanceIdInStoreProducesZeroCoverage` | Absence Case A |
| Present instance, no value at index → zero coverage | `VertexMaskForge.DynamicLayerEvaluator.PresentInstanceWithoutValueAtVertexIndexProducesZeroCoverage` | Absence Case C |
| Out-of-range index → zero coverage | `VertexMaskForge.DynamicLayerBatchCompositor.MultipleIndicesWithDistinctCoveragesAndMissingIndexProduceExpectedColors` | Absence Case B |
| Valid coverage-0 result | `VertexMaskForge.DynamicMaskGeneration.GeneratedMaterialSlotResultDrivesEvaluatedColorAtBothCorners` | Absence Case D — Ready, not Missing |
| FailedAfterReady preservation | `VertexMaskForge.DynamicMaskGeneration.FailedRegenerationPreservesPreviousReadyResult` | A failed regeneration never overwrites a prior Ready result |
| Batch aliasing safety | `VertexMaskForge.DynamicLayerBatchCompositor.AliasedInputAndOutputProduceCorrectResult` | Same array safe as both source and destination |
| Empty stack passthrough | `VertexMaskForge.DynamicLayerBatchCompositor.EmptyStackPreservesAllBaseColorsByteExact` | An empty stack composes to a byte-exact passthrough |
| Real generation-to-composition integration | `VertexMaskForge.DynamicCompositionSourceTopologyIntegration.RealSourceTopologyBaselineAndRealStoredMaterialSlotResultProduceExpectedColorsAcrossCoveredAndUncoveredCorners` | The full real chain in [§8](#8-composition), including move-preservation and non-mutation of Baseline/Working colors |

---

## 15. Current Integration Status

### Integrated in the Production Panel

- Legacy fixed `GeneratorState` (all 7 generators) and `GeneratorLayerOrder`.
- `VertexMaskForgeGeneratorLayerBridge::ComposeGeneratorLayersSequential`.
- Global Channel Filter (`bChannelFilterR/G/B`) and `EVertexMaskForgePreviewMode` / `DisplayColorDerivation`.
- `FVertexMaskForgeWorkingStateOwner` and `FVertexMaskForgeWorkingMeshOwner` (both are the real state behind the panel's `ApplyPreviewToEntry`/Accept call sites).
- Accept and Cancel flows (Accept additionally gated Legacy-only as of M16-K.6D-5 — see below).
- **M16-K.6D-5:** `FVertexMaskForgeDynamicLayerStack` → `VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology` → the M16-K.6D-2 visual-only seam, selected live by `PreviewSource` inside `ApplyPreviewToEntry`'s Source-Topology branch. Presentation-only: never `WorkingColors`, never `ApplyComposedColorsRGB`, never Accept-eligible. See §2 for the full contract.
- **M16-K.6D-8C-C:** Dynamic Layers' Generator Type selector offers `Material Slot` and `Bounding Box` (Local-space only); each layer owns its own parameters and its own `Generator Parameters` expander. See §2 for the full contract.
- **M16-K.6D-8C-B:** the obsolete standalone `Layers` presentation panel (fixed Legacy-generator list, Up/Down buttons — distinct from `Dynamic Layers`) was removed; `GeneratorLayerOrder` and Legacy composition were unaffected. See §2.
- **M16-K.6D-8D-C:** Dynamic Layers' Generator Type selector additionally offers `Directional Normal` (Local-space only); each layer owns its own Direction/Angle/Falloff/Blur/Invert parameters in a third sibling `Generator Parameters` block, with `Space` model-owned and hidden. See §2 for the full contract.

### Implemented and Test-Proven but Not Yet Connected

- As of M16-K.6B/M16-K.6C-2, extended M16-K.6D-8C-C/8D-C, `FVertexMaskForgeDynamicLayerStack`'s UI supports generator-type assignment (`None`/`Material Slot`/`Bounding Box`/`Directional Normal`) and, under the ADR-010 single-asset gate, `FVertexMaskForgeMaterialSlotParams` editing (`SelectedSlotIndex`/`bInvert`) — both now feed the M16-K.6D-5 Dynamic preview live (via `OnDynamicLayerStackMutated()`) whenever `PreviewSource == Dynamic`, in addition to remaining directly editable at any time. As of M16-K.6D-6, the Generator Type combo's "None" option is represented as a valid pointer to an unset `TOptional<EVertexMaskForgeGeneratorType>` (never a null `TSharedPtr`, which UE 5.8's own `SListView` unconditionally excludes from rendering) — see §2's M16-K.6D-6 paragraph for the full root cause. As of M16-K.6D-8C-C, each layer's own `FVertexMaskForgeBoundingBoxParams` (Local-space only) is likewise directly editable per layer, inside that layer's own `Generator Parameters` expander — see §2's M16-K.6D-8C-C paragraph. As of M16-K.6D-8D-C, each layer's own `FVertexMaskForgeDirectionalNormalParams` (Local-space only; `Space` model-owned and hidden) is likewise directly editable per layer, inside a third sibling `Generator Parameters` block — see §2's M16-K.6D-8D-C paragraph.
- `VertexMaskForgeDynamicLayerEvaluator` / `VertexMaskForgeDynamicLayerBatchCompositor` — still have no production caller; the M16-K.6D-5 Dynamic branch calls `VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology` directly instead (see §2), which itself never uses these two or any `FVertexMaskForgeInstanceResultStore`.
- `VertexMaskForgeDynamicMaskGeneration::GenerateStoredResultForMaterialSlotInstance` (Material Slot only, source-topology domain proven) — still has no production caller; the M16-K.6D-5 Dynamic branch calls `VertexMaskForgeMaterialSlotGenerator::GenerateMaterialSlotMaskFromDynamicMesh` directly (via the orchestrator), never this stored wrapper.
- `FVertexMaskForgeInstanceResultStore` as a general mechanism — still only exercised by the Material Slot Dynamic *generation* path above (`GenerateStoredResultForMaterialSlotInstance`'s own tests); never touched by the M16-K.6D-5 preview path.
- The `FVertexMaskForgeRecipe`/`FVertexMaskForgeMaskInstance` type family — a separate, self-contained evaluation library (`VertexMaskForgeRecipeEvaluation`, `VertexMaskForgeRecipeInstanceGeneration`) with zero UI and zero production caller.

**Do not presume** the Dynamic Stack has replaced the Legacy path — both remain fully independent, user-selectable via `PreviewSource`. As of M16-K.6D-7B, Accept is reachable through **either** source (structurally isolated per-source target construction — see §2's M16-K.6D-7A/7B paragraph); Dynamic Accept's manual Editor validation is still pending. `EVertexMaskForgePreviewSource`/`PreviewSource` (M16-K.6D-1, ADR-011) now has its first real reader/writer (M16-K.6D-5), independently manually validated in the Editor (M16-K.6D-6) — see §2 for the exact contract — but Global Channel Filter/Fill are still Legacy-only and not yet gated off in the UI while `PreviewSource == Dynamic` (still planned, per ADR-011).

### Planned

- A panel-level migration strategy from the legacy path to the Dynamic path (not designed).
- **Implementation pending (contract established by ADR-011, partially delivered by M16-K.6D-5):** Global Channel Filter and Fill Black/White becoming unavailable in the UI while `PreviewSource == Dynamic` (Channel Filter/Fill are not gated — see §2). Render Vertex domain support for Dynamic preview. World Space and Unified Bounds for Dynamic Bounding Box (see the M16-K.6D-8B paragraph in §2 for the exact rejection contract). World Space for Dynamic Directional Normal (see the M16-K.6D-8D-B paragraph in §2 for the exact rejection contract). Generators other than Material Slot, Bounding Box, and Directional Normal gaining a Dynamic path. Dynamic Accept exists as of M16-K.6D-7B (see §2) and its manual Editor validation has passed. No reusable Dynamic layer-stack serialization, presets, or Paint Layers exist yet.

---

## 16. Known Limitations and Risks

- **Legacy/Dynamic coexistence is no longer a pure UX risk for preview — it is now a real, user-visible choice (M16-K.6D-5)** — but Global Channel Filter and Fill Black/White remain visible and clickable while `PreviewSource == Dynamic`, even though they only ever affect the Legacy pipeline; a user could reasonably expect them to affect what they are currently viewing. Not yet gated off (planned, per ADR-011).
- **Domain-mixing risk.** `MaskInstanceId` carries no domain tag; generating a source-topology and a render-vertex result under the same id would silently overwrite one with the other (see [§3](#3-domain-model)).
- **Cardinality is caller-supplied, not independently verified.** `InstallWorkingMesh`'s `ExpectedCardinality` parameter is not cross-checked against the installed mesh's actual geometry.
- **AO's per-entry vs. per-component ownership is unresolved.** The final AO mask value is stored per-entry (asset-level) even though its raw raycast cache is per-component (transform-dependent) — a genuine tension between the legacy convention and the newer per-component-store design intent.
- **Dynamic preview is Source-Topology + Material-Slot/Bounding-Box(Local-space)/Directional-Normal(Local-space)-only.** No Render Vertex Dynamic seam exists; no other generator has a Dynamic generation path; `DynamicLayerEvaluator`/`DynamicLayerBatchCompositor`/`FVertexMaskForgeInstanceResultStore` still have no production caller (the M16-K.6D-5 preview path bypasses all three — see §2). Dynamic Bounding Box does not support World Space or Unified Bounds (M16-K.6D-8B/8C-C). Dynamic Directional Normal does not support World Space (M16-K.6D-8D-B/8D-C).
- **The M16-K.6D-5/-6 panel-level routing and UI (`PreviewSource` default/read/write, `ApplyPreviewToEntry`'s branch, `OnDynamicLayerStackMutated`, the Accept gate, the Generator Type/Fill combos' option representation, the Dynamic Layers panel's display order, drag-and-drop reordering) is not covered by any Automation test and never will be under this codebase's own established boundary** — no test anywhere constructs `SVertexMaskForgePanel` or a live Preview Component. This was independently manually validated end-to-end in the Editor as part of M16-K.6D-6 instead (Groups A–D of that checkpoint's own protocol, all PASS). The orchestrator (14 tests, including a permanent Opacity-continuity regression added during M16-K.6D-6's investigation) and the seam (6 tests) it calls remain fully automated and unmodified.
- **Several boundaries are not frozen by any test today**, including: Accept's and Cancel's own top-level flows, Preview Mode/`DisplayColorDerivation`'s behavior beyond its own pure-function contract, and a general (non-Material-Slot) cross-generator FailedAfterReady contract.

---

## 17. Architectural Maintenance Protocol

1. Read this document before starting a checkpoint.
2. Audit only the symbols the checkpoint actually touches — do not re-derive the whole architecture from scratch.
3. Implement the checkpoint.
4. Validate (build + full suite, or the scope the checkpoint actually requires).
5. Update this document in the **same commit** whenever a documented contract changes.
6. Update the [Decision Log](VertexMaskForgeDecisionLog.md) whenever an architectural decision is made or revisited.
7. Update the baseline metadata at the top of this document only **after** validation and commit, never before.
8. Never record a `Planned` item as a `Current Contract`.

### Checkpoint / PR checklist

- Architecture changed?
- Ownership changed?
- Domain changed?
- Identity changed?
- Persistence changed?
- Compatibility boundary changed?
- Tests protecting the contract changed?
- Documentation update required?

---

## 18. Symbol Index

| Concept | Primary symbol | File |
|---|---|---|
| Working geometry payload | `FVertexMaskForgeWorkingMesh` | `VertexMaskForgeWorkingMeshTypes.h` |
| Geometry identity + installed mesh owner | `FVertexMaskForgeWorkingMeshOwner` | `VertexMaskForgeWorkingMeshOwner.h` |
| Per-component color lifecycle owner | `FVertexMaskForgeWorkingStateOwner` | `VertexMaskForgeWorkingStateOwner.h` |
| Per-component color state | `FVertexMaskForgePreviewComponentState` | `VertexMaskForgeWorkingMeshTypes.h` |
| Legacy fixed generator state | `FVertexMaskForgeGeneratorState` | `VertexMaskForgeWorkingMeshTypes.h` |
| Legacy layer composition | `VertexMaskForgeGeneratorLayerBridge::ComposeGeneratorLayersSequential` | `VertexMaskForgeGeneratorLayerBridge.h/.cpp` |
| Legacy per-layer params | `FVertexMaskForgeMaskLayerParams` | `VertexMaskForgeMaskTypes.h` |
| Preview Mode enum | `EVertexMaskForgePreviewMode` | `VertexMaskForgeMaskTypes.h` |
| Preview Source authority (functionally wired, Source Topology/Material Slot only — M16-K.6D-5, ADR-011) | `EVertexMaskForgePreviewSource` / `SVertexMaskForgePanel::PreviewSource` | `SVertexMaskForgePanel.h` |
| Display-only reduction | `VertexMaskForgeDisplayColorDerivation::DeriveDisplayColors` | `VertexMaskForgeDisplayColorDerivation.h/.cpp` |
| Dynamic ordered layer stack | `FVertexMaskForgeDynamicLayerStack` | `VertexMaskForgeDynamicLayerStack.h` |
| Dynamic layer | `FVertexMaskForgeLayer` | `VertexMaskForgeLayerTypes.h` |
| Dynamic mask instance identity | `FVertexMaskForgeGeneratorMaskInstance` | `VertexMaskForgeLayerTypes.h` |
| Generator result payload | `FVertexMaskForgeInstanceMaskResult` | `VertexMaskForgeInstanceResultStore.h` |
| Generator result store | `FVertexMaskForgeInstanceResultStore` | `VertexMaskForgeInstanceResultStore.h` |
| Dynamic per-index composition | `VertexMaskForgeDynamicLayerEvaluator::EvaluateColor` | `VertexMaskForgeDynamicLayerEvaluator.h/.cpp` |
| Dynamic batch composition | `VertexMaskForgeDynamicLayerBatchCompositor::ComposeColors` | `VertexMaskForgeDynamicLayerBatchCompositor.h/.cpp` |
| Dynamic Material Slot generation | `VertexMaskForgeDynamicMaskGeneration::GenerateStoredResultForMaterialSlotInstance` | `VertexMaskForgeDynamicMaskGeneration.h/.cpp` |
| Isolated Recipe/library model | `FVertexMaskForgeRecipe`, `FVertexMaskForgeMaskInstance` | `VertexMaskForgeRecipeTypes.h` |
| Accept read + validation | `VertexMaskForgeAcceptTargetBuilder::BuildAcceptTargets` | `VertexMaskForgeAcceptTargetBuilder.h/.cpp` |
| Accept write | `VertexMaskForgeAcceptWriter::WriteAcceptTargets` | `VertexMaskForgeAcceptWriter.h/.cpp` |
| Panel orchestration | `SVertexMaskForgePanel` | `SVertexMaskForgePanel.h/.cpp` |
| Material Slot generator | `VertexMaskForgeMaterialSlotGenerator` | `VertexMaskForgeMaterialSlotGenerator.h/.cpp` |
| Dynamic Source-Topology composition orchestrator (production caller: `ApplyPreviewToEntry`'s Dynamic branch — M16-K.6D-5) | `VertexMaskForgeDynamicSourceTopologyComposition::ComputeComposedColorsRGBSourceTopology` | `VertexMaskForgeDynamicSourceTopologyComposition.h/.cpp` |
| Dynamic Accept target construction (production caller: `AcceptPendingChanges()`'s Dynamic branch — M16-K.6D-7B) | `VertexMaskForgeDynamicAcceptTargetBuilder::BuildSourceTopologyAcceptTargets` | `VertexMaskForgeDynamicAcceptTargetBuilder.h/.cpp` |
| Dynamic Accept UI-eligibility primitive (production caller: `RecomputeOperationState()` — M16-K.6D-7B) | `FVertexMaskForgeDynamicLayerStack::HasAnyEnabledLayer` | `VertexMaskForgeDynamicLayerStack.h` |
| Dynamic Bounding Box parameter type (layer-owned, Local-space only — M16-K.6D-8B/8C-C) | `FVertexMaskForgeBoundingBoxParams` / `FVertexMaskForgeAxisMaskParams` | `VertexMaskForgeRecipeTypes.h` / `VertexMaskForgeWorkingMeshTypes.h` |
| Dynamic Bounding Box per-layer UI (production caller: `BuildDynamicLayerRow` — M16-K.6D-8C-C) | `SVertexMaskForgePanel::BuildDynamicBoundingBoxLayerParamsBlock` / `BuildDynamicBoundingBoxAxisRow` / `MutateDynamicBoundingBoxAxisParam` | `SVertexMaskForgePanel.h/.cpp` |
| Directional Normal generator (existing, unmodified; production caller for Dynamic: the orchestrator's Pass 1 — M16-K.6D-8D-B) | `VertexMaskForgeDirectionalNormalGenerator::GenerateDirectionalNormalMaskFromDynamicMesh` | `VertexMaskForgeDirectionalNormalGenerator.h/.cpp` |
| Dynamic Directional Normal parameter type (layer-owned, Local-space only — M16-K.6D-8D-B/8D-C) | `FVertexMaskForgeDirectionalNormalParams` | `VertexMaskForgeRecipeTypes.h` |
| Dynamic Directional Normal per-layer UI (production caller: `BuildDynamicLayerRow` — M16-K.6D-8D-C) | `SVertexMaskForgePanel::BuildDynamicDirectionalNormalLayerParamsBlock` / `IsDynamicDirectionalNormalLayerLocalSpaceCompatible` / `MutateDynamicDirectionalNormalParam` | `SVertexMaskForgePanel.h/.cpp` |
