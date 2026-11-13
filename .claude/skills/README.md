# `.claude/skills/`

Procedures Claude can invoke on demand. Each skill is a directory containing a
`SKILL.md`; Claude reads the `description:` in every skill's frontmatter each
session and loads the body only when the task matches.

Use a skill for a **multi-step procedure with a decision in it**. Use
`.claude/rules/` for a constraint Claude should apply while editing matching
files.

## Existing skills

| Skill | Use when |
|---|---|
| `optimize-message-path/` | Asked to speed up, profile or reduce latency on a WebRTC message path (an SDP apply, `Close`, a channel or encoder teardown), asked why a signalling-thread stall happens, or asked what could be optimised in a given function. Produces a hop table, a recipe match, the guard, and a PR write-up in the house format. |

If you add a skill here, the same PR MUST add a row to this table.

## When to add a skill here

- The task has several steps and a judgement call in the middle. If it is one
  instruction, it is a rule.
- The procedure has a house-specific shape — a template to fill in, a fixed
  verification, a catalogue to match against.
- Claude has repeatedly done the task in a way that needed correcting.

## When NOT to add a skill here

- It is a constraint, not a procedure → `.claude/rules/`.
- It is a single shell command → put it in `CLAUDE.md` under build commands.
- It restates the review checklist → that lives in `.cursor/BUGBOT.md`, which
  Bugbot reads and this directory is not.

## File schema

```markdown
---
name: skill-name
description: What it does, and the situations that should trigger it. Claude
  matches on this string, so name the symptoms and the phrasings a request will
  actually use, plus an explicit "Do not use for ..." clause.
---

# Skill Name

## When to use
## When NOT to use

## Steps

Numbered, with the real commands and the real file:line references. Include a
checklist block so progress is trackable.

## Additional resources

- For X, see [X.md](X.md)
```

Conventions:

- Directory and `name:` are lowercase kebab-case and identical.
- `SKILL.md` holds the procedure. Reference material that is consulted at one
  specific step — a catalogue, a schema dump, a long example — goes in a
  sibling file in the same directory, linked from `Additional resources`, so
  Claude reads it only when it gets there.
- Cite `file:line` against `origin/main`, and prefer quoting real code over
  describing it.
