Title: build: quote SRCROOT in the Bump Version script phase
Branch: Wolfcare:fix/build-quote-srcroot -> mrmidi:main

## Problem and change

The ASFWDriver "Bump Version" pre-build phase runs `ls ${SRCROOT}` and
`$SRCROOT/bump.sh` unquoted. When the checkout path contains a space, the shell
splits it and every Xcode build fails in PhaseScriptExecution, for example:

```
ls: /Volumes/Macintosh: No such file or directory
.../Script-866DDB9CFAE1D443061EF15F.sh: line 3: /Volumes/Macintosh: No such file or directory
```

Both are quoted in `project.yml`, and `ASFW.xcodeproj` is regenerated from it.

## Prior discussion

None. This is a two-line quoting fix with no behaviour change.

## Architecture and scope

Build configuration only (`project.yml` and the generated project). No driver or app code.

## Testing and evidence

- `./build.sh` from `/Volumes/Macintosh HD/.../Firewire lives/ASFireWire` (two spaces in the path) fails before and succeeds after.
- `xcodegen generate` on this branch leaves no diff, so the committed project matches `project.yml`.

## Regression risks

None expected: quoting a path that has no spaces changes nothing.
