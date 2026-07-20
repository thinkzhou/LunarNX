# LunarNX UI Redesign

## Goal

Redesign LunarNX as a coherent Nintendo Switch Xbox streaming client rather
than a collection of functional Borealis controls. The interface should make
home-console Remote Play and xCloud browsing easy to understand while keeping
stream startup reliable on Ryubing and real Switch hardware.

## What We Already Know

- The current authentication, home, library, stream-start, and stream-overlay
  pages use different visual structures and feel unfinished as one product.
- XStreaming is the primary reference for Xbox information hierarchy, game
  artwork, cloud-library browsing, and connection-stage language.
- wiliwili is the primary reference for stable Borealis Activity navigation,
  content framing, focus behavior, and Switch-friendly lists/cards.
- Moonlight-Switch is the primary reference for a restrained streaming surface,
  destination-page loading, and unobtrusive in-stream controls.
- The dedicated `StreamLoadingActivity` fixed the immediate MainActivity crash
  and must remain page-first and lazily constructed.

## Assumptions (Temporary)

- The redesign should use existing Borealis and NanoVG capabilities without a
  new UI dependency.
- Existing Xbox/xCloud networking and media behavior should remain unchanged.
- The first implementation should establish one visual system across all
  screens instead of redesigning only the home page.

## Open Questions

- None for the MVP. The user selected Approach A.

## Research Notes

### Current LunarNX UI

- `AuthActivity` is a single centered column with no product framing or clear
  distinction between idle, code-ready, polling, and error states.
- `MainActivity` puts identity, Xbox/xCloud source selection, refresh/search,
  sign-out, content, video settings, region settings, and status into one long
  vertical `ScrollingFrame`. Functional controls have equal visual weight even
  when they belong to different workflows.
- Home consoles and xCloud games are rendered as narrow text rows with a Play
  button, so title artwork and target identity do not lead the interaction.
- The current loading Activity has a stronger card hierarchy than the rest of
  the app and therefore feels like a different product.
- The custom `RecyclingList` previously crashed during Ryubing data binding.
  The redesign must keep bounded first-screen content and conservative poster
  loading until that component has an independent reliability pass.

### XStreaming

- Uses an Xbox-green action color on a near-black/navy background.
- Gives consoles and cloud titles card-level identity rather than presenting
  them as settings rows.
- Separates xCloud into its own page with Recently, Stars, Newest, and All
  categories plus search.
- Uses artwork as the primary recognition cue and keeps text/actions beneath
  or overlaid at the lower edge.
- Groups settings by domain and uses short descriptions instead of exposing all
  choices on the home page.

### wiliwili

- Uses stable `Activity` roots and predictable no-animation transitions for
  heavy content/player pages.
- Keeps persistent navigation, page content, and bottom controller hints in
  separate visual regions.
- Uses strong focus borders and modest scale changes, which are easier to read
  from handheld and TV distance than subtle mobile press states.
- Supports dense grids, filters, skeleton/loading states, and side panels
  without flattening everything into one scroll column.

### Moonlight-Switch

- Uses `AppletFrame`/tab navigation for the application shell and dedicated
  content views for hosts, games, and settings.
- Uses compact 150x200 artwork cells with the title anchored at the bottom.
- Keeps settings in standard selector/detail rows, making controller focus and
  current values obvious.
- Treats the stream surface as immersive content: loader while connecting,
  minimal status during playback, optional detailed overlays.

### Constraints From LunarNX

- UI must remain implementable with the existing Borealis/NanoVG stack.
- `MainActivity` must not regain hidden detached full-screen children during
  initial construction.
- Poster downloads must remain deferred and bounded for simulator stability.
- Heavy pages should use `TransitionAnimation::NONE`.
- The current stream/controller/media behavior is out of scope.

## Feasible Approaches

### Approach A: Xbox Editorial + Switch Shell (Recommended)

- Keep a stable Borealis page shell and system footer hints.
- Use a compact top source switch for Remote Play / xCloud, with Settings as a
  separate Activity instead of mixing it into content.
- Remote Play opens with one or two large console cards. xCloud uses bounded
  horizontal/sectioned artwork cards for Recent and New, followed by a compact
  All Games list/search result.
- Establish shared light/dark LunarNX palettes, Xbox-green actions, consistent
  section headers, status pills, cards, empty/error states, and focus radii.
- Redesign authentication, loading, and stream overlays with the same tokens.

Pros: Xbox identity, clear hierarchy, practical with current code, conservative
memory behavior. Cons: requires several reusable UI components and a separate
settings page.

### Approach B: Native Sidebar Workspace

- Use Moonlight-Switch/wiliwili-style `AppletFrame + TabFrame` with sidebar tabs
  for Remote Play, xCloud, Settings, and Account.
- Each tab owns a dedicated content view and is recreated on selection.

Pros: strongest Switch-native navigation and clean separation. Cons: largest
MainActivity refactor, more view lifetime/focus risk, less Xbox-like home page.

### Approach C: XStreaming TV Dashboard

- Make the home page a card dashboard containing consoles and shortcut tiles;
  navigate to separate xCloud and settings Activities.
- Use larger cards and fewer visible items per screen.

Pros: closest to XStreaming and visually obvious. Cons: wastes more 1280x720
space, requires more navigation steps, and scales less gracefully to large
libraries.

## Decision (ADR-lite)

**Context**: LunarNX needs Xbox content identity without sacrificing stable
controller navigation and conservative Switch/Ryubing resource behavior.

**Decision**: Implement Approach A, Xbox Editorial + Switch Shell. Use shared
LunarNX light/dark design tokens and Xbox-green actions; keep a stable
controller-first page shell; make Remote Play and xCloud the primary content
sources; move stream settings into a dedicated Activity; keep heavy page
transitions animation-free; update authentication, startup, and stream HUD to
the same visual language.

**Consequences**: The UI gains a coherent hierarchy and reusable components
without the larger lifetime refactor of `TabFrame`. The first xCloud version
will remain a bounded set of Recent/New/All cards rather than a fully virtualized
infinite grid. A separate reliability task is required before enabling the
custom recycling list for the complete catalog.

## Technical Approach

1. Add shared UI palette/style helpers and install matching Borealis focus
   colors after application initialization.
2. Redesign `AuthActivity` as a branded two-column sign-in page while retaining
   lazy controller creation and existing auth state transitions.
3. Rebuild `MainActivity` content into a compact application header, source
   switch, contextual action bar, content cards, and persistent status area.
4. Add a dedicated settings Activity that edits the current stream launch
   options and returns updates to MainActivity.
5. Restyle `StreamLoadingActivity`, `StreamOverlay`, `PerfOverlay`, and exit
   confirmation using the shared palette.
6. Add structural/focus regressions, build the Switch NRO in Docker, verify BSS,
   and hand off the Ryubing click-through to the user.

## Implementation Plan

- Phase 1: shared design system and reusable UI helpers.
- Phase 2: authentication and main Remote Play/xCloud content hierarchy.
- Phase 3: dedicated settings page and navigation.
- Phase 4: loading/stream overlays, tests, documentation, and Switch validation.

## Expansion Sweep

- Future: favorites, game-detail pages, and richer Xbox identity can reuse the
  same card and page-shell components without changing the initial MVP.
- Related flows: authentication, loading, empty/error states, settings, and the
  stream HUD should be updated together so no page looks legacy.
- Edge cases: offline accounts, empty console/library results, long titles,
  slow poster downloads, focus restoration, and repeated actions need explicit
  visual states.

## Requirements (Evolving)

- Unify typography, spacing, colors, focus states, cards, status messages, and
  footer hints across all user-facing pages.
- Preserve controller-first navigation and readable handheld-mode layouts.
- Keep Activity roots stable; do not add hidden full-screen detached views to
  startup pages.
- Construct network-heavy controllers and streaming pages only when entered.
- Avoid changes to WebRTC, decoding, rendering, audio, and protocol behavior.
- Keep the Switch NRO BSS below 32 MiB.

## Acceptance Criteria (Evolving)

- [ ] Authentication, home/library, stream startup, and stream HUD share one
      recognizable LunarNX visual language.
- [ ] Xbox and xCloud flows have clear, controller-friendly navigation.
- [ ] Long title/library loading and stream startup expose useful status without
      allowing duplicate actions.
- [ ] Switch Docker build and BSS regression pass.
- [ ] User can complete the main flow in Ryubing without a UI crash.

## Definition of Done

- Relevant UI regression tests are added or updated.
- Switch NRO builds in `devkitpro/devkita64:20251117`.
- Focused streaming/session regressions and `git diff --check` pass.
- User performs the final Ryubing click-through; real Switch remains the final
  compatibility target.

## Out of Scope

- Changes to authentication protocols, Xbox APIs, WebRTC negotiation, media
  decode/render pipelines, or controller packet formats.
- New third-party UI frameworks.
- AI-generated raster artwork that does not already belong to Xbox titles or
  LunarNX.

## Technical Notes

- Project UI: `src/ui/`
- XStreaming reference: `github_repos/XStreaming/`
- wiliwili reference: `github_repos/wiliwili/`
- Moonlight-Switch reference: `github_repos/Moonlight-Switch/`
- Project constraints: `AGENTS.md`
