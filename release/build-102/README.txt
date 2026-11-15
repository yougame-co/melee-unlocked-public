Build 102 release deltas (offline preparation)

The build branch does not contain the packaging workflows. These patches keep their changes
separate from the game source instead of importing incomplete CI workflows or old source patches.

windows-gecko-check.patch applies to yg-ci-console-101 at
8720591b3ab5847b510a415e5007924e1dfc3697. It adds the reported, non-fatal GECKO-CHECK after both
game DLL builds, using that job's verified WinLibs objdump and nm and both AVX2/SSE2 debug files.
The existing artifact upload includes logs/gecko-check.log. Apply on the next Windows CI branch.

linux-build-labels.patch applies to yg-ci-linux-101 at
b7805297157afc6933e8be95285c92346893863c. It changes the two build 101 workflow labels to build 102;
the upstream version remains 0.9.120 and its existing Linux GECKO-CHECK remains intact.

mk-mac102-src.sh is the build 102 copy of the build 101 Mac source recipe. It uses
0.9.120-online-test.102 and yg-build-102, requires the new artifact id and both full source commits,
and refuses to reuse a build 101 source pin. It has not been run against a build 102 artifact.
No packaging, publishing or network operation was performed while preparing these files.

Mac quiet start needs the first shipped Mac YouGame version containing --yougame-tray. The supplied
header/README do not provide that version and explicitly forbid assuming the Windows floor. Pass
--yougame-mac-tray-from X.Y.Z to port/mac/build_full.py once that version is known. Until then the
Mac hook can inspect/offer YouGame but does not start an installed copy.
