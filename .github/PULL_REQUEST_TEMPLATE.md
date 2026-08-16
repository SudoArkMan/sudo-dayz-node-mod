# What this changes

<!-- One paragraph. What was wrong, and what it does now. -->

Fixes #

## Test suite

Run all 19 suites and paste the tail of the output. This is the part that gets
read first.

```
cmake --build build/cli
for f in build/cli/tests/*.exe; do "$f" resources || echo "FAILED: $f"; done
```

<details>
<summary>Suite output</summary>

```
paste here
```

</details>

- [ ] All 19 suites pass.
- [ ] I closed the running application before building, so these numbers are
      not from a build that failed to link.

If any suite skipped itself, say which and why. The corpus suites skip when
there is no DayZ installation or no mounted `P:` drive, which is fine, but it
means those numbers were not checked by this run.

## The round trip

`importtest` has to come back at zero changed files, and every class whose file
ends its lines one way has to come back ending them the same way. A pull
request that moves either off zero is refused. `CONTRIBUTING.md` explains why.

- [ ] `importtest` reports zero scripts moved, with bare newlines and with CRLF.
- [ ] `endingLost` is still 0.

If your change moves the corpus floors, give the before and after. Those are
allowed to move, and moving them up is most of what there is to do.

| | Before | After |
| --- | --- | --- |
| methods as nodes (`lowertest`) | | |
| classes back line for line (`importtest`) | | |

## Interface

Only if this changes something the application draws.

- [ ] Screenshot attached. `--screenshot out.png`, or the `--shot` option on
      whichever suite covers the panel.
- [ ] Sentence case, no all-caps, no wide letter spacing.

## Checks

- [ ] Plain ASCII. No em-dashes, curly quotes, ellipsis characters, arrows or
      middots, in code, comments, interface strings or documentation.
- [ ] Any number I wrote in a comment or in the description says what produced
      it.
- [ ] No new Qt module. The build links Core, Gui, Widgets and Network only,
      and that list is part of the licensing position in
      `THIRD-PARTY-NOTICES.md` and of what a release has to deploy. If this
      adds one, say why here.
- [ ] Any new source file is in the right object library tier in
      `CMakeLists.txt`, and no console test target gained Qt Widgets.
- [ ] Nothing under `build/` or any deployed Qt DLL is committed.
- [ ] No line endings changed. The fixtures under `resources/` are frozen in
      `.gitattributes` because tests compare their bytes.
- [ ] `CHANGELOG.md` updated under `[Unreleased]`, if this is something a user
      would notice.
