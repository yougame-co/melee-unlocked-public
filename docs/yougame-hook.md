# The YouGame hook

Melee Unlocked carries a small hook that lets the game work with the YouGame app
(https://yougame.co) when it is installed on the same computer. Through it the game can use
YouGame's online rows (Ranked, Unranked, Friends), friends and invites, sign-in, and YouGame's
button over the game.

## When it connects

The hook talks to the YouGame app in one of two ways:

1. **The YouGame app (or its launcher) started the game.** The app hands the game its own
   connection when it starts it. This always works and does not depend on the setting below.
2. **The player started the game, and "YouGame in game" is on.** The game then looks for a
   running YouGame app on this computer (a local connection, never over the network) at start and
   every few seconds after, and connects when it finds one.

With "YouGame in game" off, a game the player started does not look for the YouGame app at all:
no connection, no YouGame button over the game, nothing started in the background. The Online
menu still shows the YouGame rows (in builds with `MELEE_YOUGAME_COMING_SOON`, on by default in
CMake). Pressing one of them turns "YouGame in game" on (saved for next
time), starts looking for the app right away, and carries on the way that row always does
(open YouGame, or offer to get it, then sign in and play). No restart is needed.

## The setting

"YouGame in game" is a player setting, kept in the game's `settings.ini` as
`yougame_in_game 1` (on) or `yougame_in_game 0` (off). It can also be switched in
Settings > Online. When the file has no such line, the build's default applies. Once the player
sets it, their choice wins over the build default.

The game's log says at start which value is in effect and where it came from, for example
`yougame in game: off (build default off)` or `yougame in game: on (setting)`.

## The build default

The default comes from a CMake option:

```
-DMELEE_YOUGAME_IN_GAME_DEFAULT=ON|OFF
```

(on the Mac build script: `--yougame-in-game-default`). **Its default in the source is OFF**, so a
build made from this source without the option stays quiet until the player asks for YouGame.

YouGame's own published builds pass `-DMELEE_YOUGAME_IN_GAME_DEFAULT=ON`, so a player who
downloads one, presses Online and picks a YouGame row gets the YouGame app, signs in and plays.

Other teams building and shipping their own copy of the game should leave the option OFF, unless
they want their players to see YouGame in the game.
