# Chase Maker — Playtest Checklist

For the build dated **2026-05-14**, after the multi-source + tabs +
chase wizard + session save/load pass, **plus the post-feedback
update** (Pos column dropped, Chase tab restyled with a live
opacity×gamma composite preview, drag-and-drop from OS / AE Project
panel).

Goal of this checklist: confirm everything that should work does, and
catch regressions before the AE-side comp-builder (step 7) lands.
Items marked **[REG]** are regressions against the previous build; if
they fail, something foundational broke.

**Build / install:**
- Windows: latest `ChaseMaker.aex` at
  `C:\Program Files\Adobe\Adobe After Effects 2025\Support Files\Plug-ins\ChaseMaker\`
  (auto-installed by the build).
- macOS: `ChaseMaker.plugin` ad-hoc signed and installed at
  `/Applications/Adobe After Effects 2025/Plug-ins/ChaseMaker/`
  on the Mac mini via the tar-over-ssh build loop.

Quit AE, relaunch, open `Window → Chase Maker`. The panel title shows
a build stamp — confirm it matches your most recent build.

Reference scene path:
`D:\Dropbox\David Carney\Blender Troubleshoot\Bad Romance (Curtains)\04_Renders\01_Components\Passes\Wall_Curtains_v1_0001.exr`

---

## 1. Panel chrome & tabs

- [ ] Panel opens under `Window → Chase Maker`.
- [ ] Top toolbar shows: `Save session` / `Load session` /
      session-status / `Write luminosity sidecar` (when an EXR is
      loaded).
- [ ] Tab bar shows **Sources**, **Staging**, and a trailing `+`
      button.
- [ ] No "Open EXR..." button at the top of the panel anymore — that
      moved into the Sources tab as `Add Source...`.
- [ ] Panel resizes cleanly (drag the AE panel edges). No flicker, no
      crash.
- [ ] Close the panel via AE's tab close, reopen via Window menu —
      previously-loaded sources and chases are still there.

## 2. Sources tab

- [ ] Sources tab is the default landing tab on a fresh panel.
- [ ] `Add Source...` opens a file picker. Filter dropdown shows
      `OpenEXR (*.exr)`, `PNG (*.png)`, `All files (*.*)`.
- [ ] **Drag an EXR file from Explorer/Finder onto the panel** — the
      file is added as a new source (multi-source append). Status
      line flips to "Dropped 1 file(s) — scanning..." then scan
      completes normally.
- [ ] **Drag a footage item from AE's Project panel onto our panel**
      — if AE's drag includes a file path, it adds the file as a new
      source. If it doesn't (some custom Adobe format), the status
      line shows the available pasteboard format names (e.g.
      `[Apple files promise pasteboard type]`). Copy that status
      line and send it back so we can add explicit support for the
      format AE actually uses.
- [ ] **[REG]** Pick the reference EXR; the panel shows
      "Scanning... Enumerating layers..." then "Scan complete." It
      should NOT crash AE (regression check for the modal-in-frame
      bug).
- [ ] After scan: a row appears in the sources table with the
      filename, image size (5760×1440), layer count, and an `ACTIVE`
      marker.
- [ ] `Add Source...` again, pick **any other EXR or PNG**. A second
      row appears below the first. Both stay loaded.
- [ ] Click `Use` on the second source → its label flips to `ACTIVE`;
      the first source's `Use` button reappears.
- [ ] Click `Remove` on the inactive source → it disappears from the
      list. The remaining source stays `ACTIVE`.
- [ ] Hovering the filename shows the full path in a tooltip.
- [ ] If layer scan flagged skipped layers, the count is visible as a
      hover tooltip on the `Layers` cell.

## 3. Staging tab (active source) — regression block

Switch to the **Staging** tab with one source loaded.

- [ ] **[REG]** Layer table renders rows. Columns: `#`, `On`, `Layer`,
      `Tags`, `X/Y`. (If 2+ sources loaded, an extra `Src` column
      appears.) **The old `Pos` bar column is gone** — that space is
      now devoted to the Layer name + Tags pills.
- [ ] **[REG]** Centroid scatter canvas on the right shows yellow dots
      for included layers, gray for excluded. Image-aspect aware.
- [ ] **[REG]** Sort combo cycles all 8 modes; ordering updates
      instantly (no re-scan).
- [ ] **[REG]** `reverse` checkbox flips the active sort.
- [ ] **[REG]** `Random` mode shows a `Re-shuffle` button; clicking
      it picks a new arrangement.
- [ ] **[REG]** Play preview button cycles through included layers at
      the configured `ms/light` rate; canvas dot + table row both
      light up for the current layer.
- [ ] **[REG]** Thumbnail (top of canvas pane) tracks the previewed
      or selected layer.
- [ ] **[REG]** `Exclude matching` / `Include matching` substring
      filter toggles `included` for matching rows.
- [ ] **[REG]** `All on` / `All off` clear/set inclusion for every row.
- [ ] **[REG]** Clicking a row selects it (cool-blue tint + canvas
      ring); selection survives sort-mode change (FNV-keyed).
- [ ] **[REG]** Skipped section at the bottom collapses/expands and
      lists each skipped layer with its reason.
- [ ] **[REG]** `Write luminosity sidecar` writes
      `<exr>.luminosity.json` next to the active EXR; status flips to
      "saved".

## 4. Hide unchecked

- [ ] In the Staging filter row, `Hide unchecked` checkbox is visible.
- [ ] Uncheck a few rows in the table.
- [ ] Toggle `Hide unchecked` ON — those rows disappear from the table
      (but they still exist in the data; counts stay accurate).
- [ ] Toggle OFF — the rows return in their previous position.

## 5. Binds (right-click → Bind together)

- [ ] Right-click any row in the Staging table. A popup appears with
      `Bind...`, `Tag...`, `Position override...` submenus.
- [ ] Pick `Bind... → Bind with selected row (creates new bind)`. The
      row picks up a faint blue tint (the bind's row color) and a
      `[Bind N]` prefix on the layer name.
- [ ] Right-click a SECOND row, pick `Bind... → Bind 1` (the bind you
      just created). The second row joins the same bind, also tinted.
- [ ] Right-click one of those rows, pick `Bind... → Bind 1` again to
      toggle it off. The row leaves the bind.
- [ ] Create a second bind on different rows. Each gets its own row
      tint.

## 6. Tags (right-click → Tag)

- [ ] Right-click a row, pick `Tag...`. The submenu shows a text input
      `new tag name` and a `Create + assign` button, followed by
      existing tags.
- [ ] Type `Uplights` and click `Create + assign`. The tag appears as
      a colored pill in the `Tags` column on that row.
- [ ] Right-click another row, pick `Tag... → Uplights`. A pill
      appears on that row too.
- [ ] Right-click again, pick `Tag... → Uplights` again — the pill
      disappears from that row.
- [ ] Create a second tag (e.g. `Fire`). Assign to a row that already
      has `Uplights`. Both pills render side by side.
- [ ] Tag colors are stable (same color across rows for the same tag).

## 7. Position override

- [ ] Right-click a row → `Position override...`. Two sliders appear
      (`X`, `Y`).
- [ ] Drag `X` and `Y`. The dot for that layer on the centroid canvas
      moves to the new position in real time.
- [ ] Switch sort mode to "Centroid (Left to Right)". The row with
      the overridden position sorts according to its override's X
      (override beats the scanned cx).
- [ ] Pick `Clear override` in the same submenu. The dot snaps back
      to the scanned centroid.

## 8. Manual reorder (drag & drop)

- [ ] In the Staging table, **drag a row** (click + hold on the
      number column / row body, drag up or down). A tooltip shows
      "Move: <layer name>".
- [ ] Drop on another row. The two rows swap positions (or the
      dragged row moves above/below the drop target).
- [ ] Switching to a different sort mode and back re-applies the
      algorithm — the manual order is overwritten if you re-sort.
      (Manual order is a one-shot rearrange; not yet persisted as a
      separate "manual" sort.)

## 9. Chase tab + wizard

The Chase tab was redesigned to mirror the Staging tab's two-pane
layout — stages list on the left, **live composite preview** + a
centroid map on the right, with timing sliders and a play/scrub
transport above. The composite shows the actual opacity × gamma
envelope at the current playhead, summed from each stage's thumbnails.

- [ ] Click the trailing **`+`** in the tab bar. A new tab appears
      titled `New chase` and is auto-selected.
- [ ] The tab's content is the wizard form, not the editor.
- [ ] Template dropdown shows: `Left to Right`, `Top to Bottom`,
      `Center Out`, `3 Step Chase`, `Random`, `Custom`.
- [ ] Pick `Left to Right`. Confirm `Hit duration` and `Step duration`
      sliders default to the last-used values (or 30 / 4 on first
      use).
- [ ] Click `Create chase`. The wizard form collapses; the chase
      editor takes over.
- [ ] Tab title updates to the template name (e.g. `Left to Right`).
- [ ] In the editor: chase name is editable; `Sort` defaults to
      `Centroid X`; `reverse` is off; all timing sliders are visible.
- [ ] `Stages` list at the bottom shows N rows, one per included
      layer (with the active source's tag-filtered subset, sorted by
      the chase's sort mode).
- [ ] Change `Hit duration` slider → next time you create a chase,
      the wizard prefills with this value (last-used wins).
- [ ] Click `+` again, pick `3 Step Chase`, hit `Create chase`. The
      resulting chase has roughly N/3 stages (not N), each containing
      a group of layers that fire together.
- [ ] Switch between chase tabs by clicking — each preserves its own
      timing, sort, stages.
- [ ] Click the `x` on a chase tab → it disappears.
- [ ] Inside a chase editor, click `Delete chase` → the tab disappears,
      Staging tab becomes active.
- [ ] In the wizard, `Cancel` deletes the just-created chase and
      returns to Staging.

### 9a. Chase live preview

- [ ] In the chase editor, the right pane shows a **Composite preview**
      image. Initially (playhead at frame 0) it's mostly black (all
      stages inactive at t=0 with attack > 0).
- [ ] Click `Play` — the playhead advances and the composite lights
      up showing the active stage(s)' contribution at each frame.
      Looks like the chase actually playing, no precaching wait.
- [ ] Drag the **scrubber** to a mid-chase frame. Composite updates
      immediately; the matching stage row in the stages table
      lights up (warm tint, proportional to envelope value).
- [ ] Drag a slider mid-playback (`Hit duration`, `Attack`, `Step`,
      etc.) — composite reflects the new envelope live.
- [ ] **3 Step Chase** preview shows multiple lights firing
      simultaneously per stage (verify: when one stage is active,
      multiple table rows are tinted, and the composite has
      several bright regions, not just one).
- [ ] When `Opacity peak` is reduced (e.g. to 30%), composite gets
      dimmer accordingly. When `Gamma baseline` is raised (towards
      `Gamma peak`), the off-peak "dim" between hits looks less
      dim.
- [ ] Switch chase tabs — each chase shows its own composite, not
      the previous one's. Switching back resumes from where you
      left off.

**Performance note:** the composite re-uploads to GPU every frame
during playback. If the panel feels sluggish, that's the suspect;
flag it. Expected: smooth at 24-60fps on the dev rig.

## 10. Session save / load

- [ ] Load the reference EXR, create a couple of tags, a bind, a
      position override, and two chases.
- [ ] Click `Save session` (top toolbar). A Save dialog opens with
      default filename `session.chasemaker.json`.
- [ ] Save it next to the reference EXR. The toolbar shows
      `Session: session.chasemaker.json`.
- [ ] Open the directory in Explorer. You should see:
      - `session.chasemaker.json` (the session file)
      - `session.<chase_name>.chase.json` for each chase
- [ ] Open the session JSON in a text editor — readable, contains
      `sources`, `binds`, `tags`, `chases`, `position_overrides`.
- [ ] Click `Save session` again. The dialog does NOT re-prompt — it
      overwrites the same path silently. Status flips to "Session
      saved."
- [ ] Quit AE, relaunch. Open the panel. State is empty (we don't
      auto-load).
- [ ] Click `Load session`. Pick the saved JSON.
- [ ] Panel re-scans the source EXR (status briefly shows scanning).
      After it completes:
      - Sources tab shows the source.
      - Staging tab shows tags, binds, and the position override
        correctly applied.
      - Chase tabs reappear with names + stage lists intact.
- [ ] LayerRefs (binds, tags, chase stages) resolve correctly — no
      "(missing)" placeholders in the stage list.

## 11. Save-load edge cases

- [ ] Load a session whose EXR has moved on disk → re-scan fails;
      status shows the error. Other session data (binds, tags, etc.)
      still loads with `(missing)` placeholders in stages.
- [ ] Re-save the session over the same file — no per-chase
      `.chase.json` files for deleted chases are left orphaned
      (current implementation does NOT clean up old chase files
      automatically; see "Known limitations" below).

## 12. Multi-source

- [ ] Load two different EXRs as sources.
- [ ] Switch which is active via the Sources tab `Use` button.
- [ ] Staging table updates to show the newly active source's layers.
- [ ] An extra `Src` column appears showing the source id when 2+
      sources are loaded.
- [ ] Tags and binds can include layers from either source (right-
      click rows in either active source; pills/tints persist when
      switching).
- [ ] Save session. Reload. Both sources scan; both sets of layers
      come back; cross-source binds/tags still resolve.

## 13. Sidecar / luminosity export (regression)

- [ ] **[REG]** Per-source `.luminosity.json` still writes correctly
      via the top-toolbar button. Format unchanged from before this
      build (version: 1, same fields).

## 14. Known limitations not to test (yet)

These are deliberately deferred — don't file as bugs:

- **AE Project panel drag format** — Windows + Mac both register OS
  drop targets and accept file-URL drops. If AE's drag from the
  Project panel includes a file path (likely on at least one
  platform), it'll Just Work. If AE uses a custom format (e.g.
  Adobe-internal item-handle pasteboard), our drop logs the format
  names to the status line — capture that and we'll add explicit
  support next iteration. File-from-Finder/Explorer drops always
  work.
- **AEGP comp builder for chases** — the chase JSON exports correctly
  via session save, but ChaseMaker itself does NOT yet create the AE
  comps + apply Demux effects + bake keyframes. The companion JSX
  executor is the next step. Until then, the `.chase.json` files are
  the deliverable.
- **`.aep` XMP pointer to the session file** — also deferred. The
  session JSON lives alongside the AE project as a sidecar but the
  `.aep` does NOT yet store a path/checksum pointer.
- **Per-light timing variation** — chase timing is uniform across all
  stages of a chase. No per-stage or per-light overrides yet.
- **Manual order persistence** — drag-to-reorder works for the
  current view but isn't saved as a "manual" sort mode; switching
  sort modes overwrites the drag order.
- **Old per-chase `.chase.json` cleanup** — renaming/deleting a chase
  doesn't remove the file the previous name wrote. Stale files
  accumulate alongside the session file.
- **Notarization on Mac** — still ad-hoc signed. Run `xattr -dr
  com.apple.quarantine ...` if Gatekeeper blocks.

## 15. Report back

Anything that fails — short note with the step number and what
actually happened. For crashes, the build stamp in the panel title
helps anchor which build it was.

For UX rough edges that aren't strictly bugs (clunky popup behavior,
poor visual contrast, etc.), call them out too — easier to address
in the next pass than after a release.
