# srcreadme.ps1 (melee-native, 10-20): writes a build zip's source/README.txt for its real upstream tag (GPL: the exact
# source ships in every zip). With -Branch, also names source/branch.patch, the test build's own change.
param([Parameter(Mandatory)][string]$Stage, [Parameter(Mandatory)][string]$Tag, [switch]$Branch)
$b = if ($Branch) { "`n  git apply branch.patch          # this test build's own change" } else { "" }
$text = @"
Melee Unlocked for YouGame: the Source Port of Melee Unlocked (GPL-3.0-or-later,
https://github.com/Hero88go/melee-unlocked) with YouGame online play.

Source: tag $Tag of https://github.com/Hero88go/melee-unlocked with yougame-online.patch applied$(if ($Branch) { ', then branch.patch' }):

  git clone https://github.com/Hero88go/melee-unlocked.git && cd melee-unlocked
  git checkout -b yougame-online $Tag && git am --keep-cr yougame-online.patch
  git rm -q --ignore-unmatch -- port/slippi_sys/GameFiles/GALE01/GameSetup_gui.dat port/slippi_sys/GameFiles/GALE01/slpCSS.dat port/slippi_sys_general/GameFiles/GALE01/GameSetup_gui.dat port/slippi_sys_general/GameFiles/GALE01/slpCSS.dat port/slippi_sys_playback/GameFiles/GALE01/GameSetup_gui.dat port/slippi_sys_playback/GameFiles/GALE01/slpCSS.dat # The patch leaves these deletions out: binary deletions carry the whole file; rebuilt from your own disc via port/sys_deltas.$b

Build as docs/build-source-port.md describes. The game data comes from your own Melee NTSC 1.02 disc;
nothing from the game is included.
"@
[IO.File]::WriteAllText("$Stage\source\README.txt", ($text -replace "`r`n", "`n"), (New-Object System.Text.UTF8Encoding $false))
