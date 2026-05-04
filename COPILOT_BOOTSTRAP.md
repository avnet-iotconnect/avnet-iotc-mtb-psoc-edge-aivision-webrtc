## fork-aivision session bootstrap

PILOT.md is the shared handoff between Copilot and Claude Code. Always reflects current state.

---

**1. Read Claude Code project memory**
`~/.claude/projects/-var-b-shared-work-mtw-fork-aivision-fork-aivision/memory/`
Read MEMORY.md (index), then the files it lists.

**2. Execute /refs**
Defined in `.claude/commands/refs.md`. Read that file and follow it.
Core: read PILOT.md, GUIDELINES.md, OPEN_QUESTIONS.md, CONTRIBUTING.md.
STYLE-C.md (repo root) — load only when editing/creating source files we own.

**3. /commands as slash commands**
`.claude/commands/<name>.md` defines `/<name>`. Execute on user input.

**4. File reads before edits**
At session start: always read from disk — summaries and prior output drift from disk state.
Mid-session: use `stat` to check mtime before a second edit; re-read only if mtime changed.
If a file you expected to be implemented is still a stub: write from scratch, don't patch.

**5. Build check**
  cd /var/b/shared/work/mtw/fork-aivision/fork-aivision
  make build TOOLCHAIN=GCC_ARM CONFIG=Debug 2>&1 | tail -20
Tail is used to limit token burn. If build broken, stop and report unless an easy fix or asked to fix a broken build.

**6. Bootstrap report — then wait**
Report: active milestone + "next session starts here" pointer, build status, relevant OPEN_QUESTIONS.
Then stop. Do not plan or start work until the user asks.
