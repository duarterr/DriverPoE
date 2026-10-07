# CLAUDE.md

Guidance for Claude Code when working in this repository.

## Writing style — de-slop with `unslop`

When drafting or editing prose for this repo (README files, docs, commit/PR descriptions,
comments meant for humans), use the `unslop` skill to strip AI-writing tells and keep the
author's voice.

Source: https://github.com/theclaymethod/unslop

Unslop is an agent skill that detects and removes formulaic AI-generated writing patterns
while preserving meaning and voice. It has four modes:

- **teach** — builds a personalized voice profile from writing samples
- **cleanup** — flags AI tells as reviewable suggestions (no silent edits)
- **rewrite** — diagnoses a draft, then rebuilds it with safeguards
- **mimic** — generates text in the taught voice, then validates it against the removal gates

Detection runs in three layers (phrase-level triggers → structural/rhythm patterns →
outline/"silhouette" scoring), with protections for quotes, code blocks, factual content
(numbers, names, dates), and genre-appropriate conventions. Scanners suggest candidates;
the agent still checks each one in context before changing anything.

Trigger this whenever asked to "humanize", "de-slop", or "make text sound human", or when
reviewing text with obvious AI tells (e.g. "Here's the thing:", "Let that sink in").
