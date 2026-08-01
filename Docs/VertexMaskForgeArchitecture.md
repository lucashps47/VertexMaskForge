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
| Directional Normal | `DirectionalNormal` | Render-vertex + Source-topology | Normal-vs-axis-direction scalar | Not yet represented | Compatibility gap only |
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
- Accept and Cancel flows.

### Implemented and Test-Proven but Not Yet Connected

- `FVertexMaskForgeDynamicLayerStack` — has a real, editable UI section. As of M16-K.6B this includes generator-type assignment per layer (`None`/`Material Slot`, via `SetLayerMaskGeneratorType`/`ClearLayerMask`) — `Material Slot` is the only generator type exposed, matching the only generator with real Dynamic generation (see the table above). As of M16-K.6C-2, when a layer's assigned generator is `Material Slot`, its `FVertexMaskForgeMaterialSlotParams` (`SelectedSlotIndex`/`bInvert`) are also editable, exclusively via `SetLayerMaskParams`, but only under the ADR-010 single-asset gate: editing is enabled only when `SelectedMeshes.Num() == 1` and that one asset entry's `WorkingMesh.MaterialSlotOptions` is non-empty; the picker's options come from that one entry's own `MaterialSlotOptions`; a stored `SelectedSlotIndex` that does not match any current option is shown explicitly (never clamped, never silently reselected). With zero, multiple, or slot-less assets the section stays visible with an explicit inline message and no editable controls. No production composition call site reads the stack, generates anything, or feeds `WorkingColors`/preview — both generator-type assignment and parameter editing remain configuration-only.
- `VertexMaskForgeDynamicLayerEvaluator` / `VertexMaskForgeDynamicLayerBatchCompositor`.
- `VertexMaskForgeDynamicMaskGeneration::GenerateStoredResultForMaterialSlotInstance` (Material Slot only, source-topology domain proven).
- `FVertexMaskForgeInstanceResultStore` as a general mechanism (only exercised by the Material Slot Dynamic path above).
- The `FVertexMaskForgeRecipe`/`FVertexMaskForgeMaskInstance` type family — a separate, self-contained evaluation library (`VertexMaskForgeRecipeEvaluation`, `VertexMaskForgeRecipeInstanceGeneration`) with zero UI and zero production caller.

**Do not presume** the Dynamic Stack has replaced or is about to silently replace the legacy path — no code wires the two together, and no mutual-exclusion/authority mechanism between them exists yet.

### Planned

- A panel-level migration strategy from the legacy path to the Dynamic path (not designed).
- An explicit authority/mode-selection mechanism to prevent the two composition paths from ever running simultaneously once Dynamic gets a real caller.

---

## 16. Known Limitations and Risks

- **Legacy/Dynamic coexistence is currently a UX risk, not a data risk.** The Dynamic Layers UI is real and editable, but edits to it have no observable effect on preview, `WorkingColors`, or Accept — a user can reasonably expect otherwise.
- **Domain-mixing risk.** `MaskInstanceId` carries no domain tag; generating a source-topology and a render-vertex result under the same id would silently overwrite one with the other (see [§3](#3-domain-model)).
- **Cardinality is caller-supplied, not independently verified.** `InstallWorkingMesh`'s `ExpectedCardinality` parameter is not cross-checked against the installed mesh's actual geometry.
- **AO's per-entry vs. per-component ownership is unresolved.** The final AO mask value is stored per-entry (asset-level) even though its raw raycast cache is per-component (transform-dependent) — a genuine tension between the legacy convention and the newer per-component-store design intent.
- **Dynamic has no panel connection at all.** No render/preview call site reads `DynamicLayerStack`, `EvaluateColor`, or `ComposeColors`.
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
