# CLAUDE.md — Vertex Mask Forge

## Project

Native Editor-only plugin for Unreal Engine 5.8, for generating and editing
Vertex Colors on Static Meshes.

- **Technical name / repository:** VertexMaskForge
- **Visual name (UI):** Vertex Mask Forge
- **Engine:** Unreal Engine 5.8
- **Test project:** `G:\UnrealProjects\MyProject`
- **Connection to the project:** junction at
  `G:\UnrealProjects\MyProject\Plugins\VertexMaskForge` pointing to this
  repository.

## Fundamental rules

- Editor-only. Must not compile/load in game (Runtime) builds.
- 100% C++ code. No Blueprint.
- 100% Slate interface. No Editor Utility Widget.
- No dependency on Houdini or Houdini Engine.
- Menu entry: `Tools → Custom Tools → Vertex Mask Forge`.
- Vertex Color processing implemented natively in C++, using UE 5.8's
  Dynamic Mesh and Geometry Script APIs when appropriate.

## Development methodology

- Incremental development, in small checkpoints.
- Each checkpoint must compile without errors before moving on to the next.
- Never keep implementing after a build error without resolving it first.
- Before assuming the signature of an API (Dynamic Mesh, Geometry Script,
  Slate, etc.), inspect the actual installed UE 5.8 source code at
  `G:\UE_5.8\Engine`. Do not assume based on previous engine versions.

## Architectural Documentation Protocol

Vertex Mask Forge has living, versioned architectural documentation
alongside the plugin:

- [Docs/VertexMaskForgeArchitecture.md](Docs/VertexMaskForgeArchitecture.md)
  — describes the current architectural state (ownership, domains, color
  lifecycle, composition, generators, integration, limitations).
- [Docs/VertexMaskForgeDecisionLog.md](Docs/VertexMaskForgeDecisionLog.md)
  — records the accepted architectural decisions, their reasons, and the
  questions still open.

### Start of every checkpoint

Before auditing, planning, implementing, or modifying any part of
Vertex Mask Forge:

1. Read both of the documents above in full.
2. Use `VertexMaskForgeArchitecture.md` as the description of the current
   architectural state.
3. Use `VertexMaskForgeDecisionLog.md` to understand decisions already
   accepted, their reasons, and what is still open (never treat an open
   item as decided).
4. After reading, audit in the code and tests only the files, symbols, and
   boundaries actually affected by the checkpoint — do not rediscover the
   entire architecture every time.
5. Validated code and tests remain the factual authority: do not blindly
   trust the documentation if it diverges from what the code/tests actually
   prove.
6. If there is a divergence between documentation and code/tests, do not
   silently follow either version: identify the inconsistency, determine
   the contract actually proven, correct the documentation within the
   checkpoint's scope when appropriate, and report the reconciliation.

### During the checkpoint

- Keep `Current Contract`, `Compatibility Boundary`, `Known Limitation`, and
  `Planned` always clearly separated, as already defined in both documents.
- Never present `Planned` behavior as if it were already implemented.
- Never treat a decision still open as `Accepted`.
- Treat any change of ownership, domain, identity, cardinality,
  composition, persistence, or integration as a possible architectural
  change, even if small.
- Relate new tests to the boundary they freeze whenever this affects the
  documentation.
- Keep the audit limited to the boundary actually affected, after the
  initial reading of both documents.

### Documentation impact review

After implementing and validating, and before any staging/commit,
explicitly evaluate whether the checkpoint changed:

- the current architectural state;
- ownership;
- the render-vertex/source-topology domain;
- identity behavior (`MaskInstanceId`/`LayerId`);
- composition or channel semantics;
- generation/result-store behavior;
- the cache/invalidation contract;
- preview, Accept, Cancel, or persistence behavior;
- any compatibility boundary;
- integration status;
- any limitation (introduced, changed, or resolved);
- the representative tests that protect any boundary;
- any architectural decision (introduced, revised, superseded, or
  rejected);
- whether, in light of what was evaluated above, any documentation update
  is necessary.

The review must end in one of two outcomes, never ambiguous: update
`VertexMaskForgeArchitecture.md` and/or `VertexMaskForgeDecisionLog.md`
per the criteria in the next section, or apply the "Checkpoints with no
documentation impact" protocol.

### When to update each document

Update `VertexMaskForgeArchitecture.md` when: the current architectural
state changes; ownership, domain, identity, lifecycle, or data flow
changes; a Current Contract or Compatibility Boundary changes; the
integration status changes; a limitation arises, changes, or is resolved;
the representative tests protecting an architectural boundary change; or
the documented core symbols/responsibilities change.

Update `VertexMaskForgeDecisionLog.md` when: an architectural decision is
introduced; an `Accepted` decision is materially revised; a decision is
superseded; a previous decision no longer represents the project; or a
question listed in Future Decision Candidates becomes a proven decision.

Do not create a new ADR for local changes with no architectural relevance.

### Same commit

When a code or test change alters a documented contract or decision:

- update the corresponding documentation as part of the same checkpoint;
- include code, tests, and documentation in the same commit;
- never leave the documentation update for a future checkpoint;
- re-validate consistency between implementation, tests, Architecture, and
  Decision Log before the commit.

For baseline metadata, follow exactly the protocol defined in
`VertexMaskForgeArchitecture.md`, section "Architectural Maintenance
Protocol". Never invent or anticipate the hash of a future commit.

### Checkpoints with no documentation impact

When the documentation impact review concludes that no change is
necessary:

- do not edit the documents mechanically;
- explicitly state in the checkpoint's final report that both documents
  were read, that the documentation impact review was performed, that no
  documentation change was necessary, and present a short justification
  based on the boundary affected by the checkpoint.

A checkpoint must not be considered complete without this explicit
statement or without the required documentation updates.

## Restrictions

- Never edit Engine files (`G:\UE_5.8\Engine\...`).
- Never overwrite preexisting changes in the test project or in the
  repository without inspecting them first.
- Do not add binary assets to the repository without real need.
- Never commit without explicit authorization from the user.

## Environment

- Engine installed at: `G:\UE_5.8`
- Test project: `G:\UnrealProjects\MyProject` (`EngineAssociation: 5.8`)
- The test project has its own Git repository, independent of this one.
  Do not initialize or alter Git inside `G:\UnrealProjects\MyProject`.
