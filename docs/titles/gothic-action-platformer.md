# Gothic action platformer

Title: PPSA35399 - Castlevania: Belmont's Curse.

Records for this title, newest first. The bring-up manual keeps the
invariants, methodology, frontier and an index of every record. Titles are
named by PS5 content ID and public name (`AGENTS.md`).

## The new-game video presents black (2026-11-16, open)

Intel Arc A770, Mesa ANV on Xe, 60 fps present limit, strict runs.

- **Observed.** The title reaches its studio logo, the notice text and the title menu with the correct image.
  After New Game, the video that follows presents black with only its skip prompt drawn, about 30-70 s into
  a scripted run.
- **Excluded: the gamma-twin target alias.** The fix that made another title's studio video visible (a
  UNORM render target sampled as sRGB, `13fafeac5`) does not change this title: the same route still
  presents black, so the cause lies elsewhere in the video chain.
- **Next.** Follow the chain stage by stage as for that title (player frames, guest copy, sampled texture,
  conversion pass, the target it writes and the pass that reads it), with addresses taken from each run.
