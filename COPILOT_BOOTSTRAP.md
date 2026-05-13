# fork-aivision session bootstrap

This file is read at the start of a session to recover context. It works for
any agent (Copilot's Claude, Copilot's GPT, or Claude Code). Follow the
numbered steps. Do **not** start work until the user explicitly asks.

---

## 1. Read the active session handoff

`work/reference/HANDOFF.md` — the most recent end-of-session summary. Tells
you what landed last, what the next concrete step is, and which constraints
carry forward. **Always read this first.**

## 2. Read the project context bundle

These three files are the stable project context, equivalent to what Claude
Code's `/refs` skill loads:

- `work/reference/PILOT.md` — active milestone / S-series plan. Lives,
  mutates often.
- `work/reference/GUIDELINES.md` — stable project facts (platform quirks,
  copy-from-upstream policy, role assignments, AWS/IoTC integration
  shape). Outlives any single milestone.
- `work/reference/OPEN_QUESTIONS.md` — small live list of open items.

Other files under `work/reference/` (frozen milestone archives `PILOT_M*.md`,
`n6-analysis.md`, `WEBRTC_TASK.md`, `UPSTREAM_CANDIDATES.md`, etc.) are
on-demand references — load only when relevant.

## 3. Read the project-memory bundle

The Claude Code project-memory directory mirrors what a long-running agent
would have remembered across sessions. Read its index, then each entry it
lists, so a fresh agent walks in with the same accumulated context:

**Path:** `~/.claude/projects/-var-b-shared-work-mtw-fork-aivision-fork-aivision/memory/`

**Read:** `MEMORY.md` (index — names each entry and one-line description),
then read every `.md` file it points to. Today the bundle contains:

- `user_profile.md` — who the user is, role, preferences.
- `project_third_party_layout.md` — third_party/ submodule convention.
- `project_n6_reference_status.md` — what the N6 reference port is and isn't
  authoritative for.
- `reference_layout.md` — which docs `/refs` loads vs. on-demand vs. archive.
- `feedback_session_handoff.md` — write a compact handoff at end-of-session.
- `feedback_init_function_stack.md` — `app_*_init` runs on the tiny
  pre-scheduler stack; task creation only, real work on the FreeRTOS task.
- `feedback_newlib_nano_no_llu.md` — newlib-nano `%llu`/`%lld`/`%z` traps.
- `feedback_explicit_includes.md` — user preference: each file `#include`s
  the header for every type it uses directly, rather than leaning on
  transitive includes.

If the memory directory is not accessible (e.g., you're a Copilot agent
without filesystem access to that path), the index is reproduced inline
above — treat the bullet list as the bundle.

## 4. Read STYLE-C.md only when editing source

`STYLE-C.md` (repo root) — concrete code formatting rules. Don't load at
bootstrap; load when you're about to create or edit a source file we own.

## 5. Reading discipline

- **At session start:** always read files from disk. Summaries (including
  this file) drift from current state.
- **Mid-session:** if you're about to edit a file you already read, trust
  the in-context state unless something happened that would invalidate it.
- **If a file you expected to be implemented is still a stub:** write from
  scratch, don't try to patch.

## 6. Don't auto-build

The build command is **not** part of bootstrap. The user runs builds when
appropriate. If you need to know build status, ask — don't kick one off.

## 7. Bootstrap report — then wait

After reading the files above, report briefly:

- Current S-step status (where HANDOFF.md says we left off, what's next).
- Anything in `OPEN_QUESTIONS.md` that's active.
- Anything in HANDOFF.md flagged as a constraint or carry-forward that
  affects what you might do next.

Then **stop and wait for the user to direct you.** Don't plan, don't
propose, don't start work.

---

## Worked example

User says: *"load COPILOT_BOOTSTRAP.md and resume session in /refs HANDOFF.md"*

You should:

1. Read this file.
2. Read `work/reference/HANDOFF.md`.
3. Read `work/reference/PILOT.md`, `GUIDELINES.md`, `OPEN_QUESTIONS.md`.
4. Read the memory bundle (or treat the inline list as it).
5. Report status + next step + any active open questions. Stop.
