# Work-Items & Traceability

This repo follows the same scheme as its sibling projects (surfer, city_gen), so a single
convention spans the ecosystem. Practice: `~/.claude/docs/commit-traceability.md`.

## The layers

| Layer | Home | Id example |
|-------|------|-----------|
| **Epic** ≈ a phase | a section in [`docs/ROADMAP.md`](../ROADMAP.md) | `phase-trust` |
| **Feature** | `docs/features/<slug>.md` | `reconnect-resilience` |
| **Story / Task** | a checklist entry inside the feature doc | — |

## The rules

1. **Flat, unique, standalone slugs.** Kebab-case, meaningful alone (`icy-metadata`, not
   `stations/metadata/icy`). No hierarchy encoded in the string.
2. **Relate by naming the parent.** A feature's frontmatter carries `epic: <phase-id>`. One
   search on any id finds its parent (named in its own doc) and its children (which name it).
3. **Commit at task granularity, cite the item served.** `[<slug>]` in the subject or a
   trailer. Chore/planning commits with no work-item cite the driving `Dxx` decision instead.
4. **Keep the linkage healthy.** A new work-item declares its id and parent, and gets a row
   in the index below.

## Frontmatter template

```markdown
---
id: <slug>
type: feature
epic: phase-trust
design: [docs/design/architecture.md]   # optional down-link
status: planned | in-progress | done
---
```

## Traversal

- **Backward** — "what work went into X?" → `git log --grep="\[<slug>\]"`
- **Forward** — "what was this file serving?" → `git log -- <path>`, read the cited slug,
  open its doc, follow `epic:` upward to the roadmap.

## Index

| Id | Type | Parent | Status |
|----|------|--------|--------|
| `phase-poc` | epic | — | ✅ done — `4267806`, verified on 4.6 + 4.7, 330s soak clean |
| `phase-trust` | epic | — | planned |
| [`fake-icecast-server`](fake-icecast-server.md) | feature | `phase-trust` | planned — **keystone**, unblocks testing *and* perf |
| [`unit-test-suite`](unit-test-suite.md) | feature | `phase-trust` | planned |
| [`reconnect-resilience`](reconnect-resilience.md) | feature | `phase-trust` | planned |
| `phase-measure` | epic | — | planned |
| [`perf-harness`](perf-harness.md) | feature | `phase-measure` | planned — see `design/performance.md` for the here-vs-surfer split |
| `phase-stations` | epic | — | planned |
| [`tls-streams`](tls-streams.md) | feature | `phase-stations` | planned |
| [`url-resolution`](url-resolution.md) | feature | `phase-stations` | planned |
| [`icy-metadata`](icy-metadata.md) | feature | `phase-stations` | planned |
| `phase-ship` | epic | — | planned |
| [`surfer-integration`](surfer-integration.md) | feature | `phase-ship` | planned |
| [`platform-matrix`](platform-matrix.md) | feature | `phase-ship` | planned |
| [`stream-gain-normalization`](stream-gain-normalization.md) | feature | `phase-ship` | planned |
| `phase-reach` | epic | — | planned |
| [`opus-codec`](opus-codec.md) | feature | `phase-reach` | planned |
| [`web-audio-fallback`](web-audio-fallback.md) | feature | `phase-reach` | planned — D3 |
| [`audiostreamplayback-migration`](audiostreamplayback-migration.md) | feature | `phase-reach` | ⏸ contingency only (D2) — do not start without a measured trigger |
