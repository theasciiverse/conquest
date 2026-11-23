# Conquest

**"We were playing xtrek before there was X... It was known then as
Conquest, was written in RATFOR and ran on a VAX."** (Michael Erskine)

Four empires share one universe: the Federation, the Romulans, the
Klingons and the Orions. Each starts with a home system around its sun,
Earth, Romulus, Klingus and Orion, and a scatter of neutral worlds in
between: Altair, Jinx, Telos, Janus, Sherman, and a planet called Hell.
You command a starship in real time, beside and against everyone else
on the server. Shoot down enemy ships for kills, bombard enemy planets
until their armies are gone, then beam your own armies down to take
them. Take every core planet and your team has conquered the universe:
your name goes up in lights, you get to speak your last words to every
player in the game, and the universe starts again.

Torpedoes travel, so you lead your target. Phasers are instant but
short. Engines and weapons share one power plant, shields soak up hits
until they do not, and an overheated weapons bank leaves you helpless.
The planets fire back. Somewhere out there a doomsday machine eats
planets, and whoever destroys it earns five kills at a stroke.

## Where it comes from

Jef Poskanzer and Craig Leres wrote Conquest in Ratfor for VAX/VMS in
1983. Jon Trulson played it for hours with friends in the terminal labs
at college, and in the early to mid 1990s, with a multi-user Unixware system
at home, he ported it to C, adding color, keypad steering and function
key macros while keeping the mechanics and balance of the original.
Version 8 split it into a server, `conquestd`, and clients that connect
to it over the network; version 9 let an operator lay out the whole
universe, any number of planets with their own names and orbits, and
kept only the OpenGL client. Jon still works on it.

This edition brings the terminal client back, for today's servers.

## Playing

Run `conquest`. Its menu offers:

- **Play here**: a universe on your own machine. Robots fly for the
  teams nobody is playing: M-5 for the Federation, Colossus for the
  Romulans, Guardian for the Klingons and HAL 9000 for the Orions.
- **Host**: the same universe, open to your local network. The screen
  shows the address friends type.
- **Join**: type a friend's address.

Every server has its own players: pick a name and a password the first
time you join (just make them up), and you are given a team and a
ship. Your universe, kills and options are kept in
`~/.local/state/conquest`.

The terminal must be at least 80x25. In the game, `h` lists every
command. The ones you need first:

- `k` set a course: a planet name, an angle, or `qweadzxc` for the
  eight directions; arrow keys turn too
- `0` to `9`, `=` warp factor (`=` is warp 10)
- `p` fire a torpedo, `P` a burst of three, `f` phasers; Enter on the
  target prompt fires the same way as last time
- `+` and `-` shields up and down, `R` repair, `o` orbit a planet
- `B` bombard the planet you orbit, `b` beam armies up or down
- `i` information on a ship or planet, `M` the long range map
- `m` send a message (`a` for all, a team letter, or a ship number)
- `Q` self-destructs after fifteen seconds, the honourable way out of a
  ship; `Ctrl-\` leaves at once, and a lightning bolt takes the ship;
  `q` leaves from the menu

## Credits

Conquest: Jef Poskanzer and Craig Leres (1983), Jon Trulson (1994 to
today), with contributions from the people in `docs/CREDITS.txt`. MIT
licence. The curses client brought forward to 9.x, the menu and the
packaging: Asciiverse. See `PATCHES.md`.

## Building

`sh asciiverse/build.sh` builds the programs into `av-build/bin/`
(autoconf, automake, libtool, bison, flex and the ncurses headers are
needed); `sh asciiverse/stage.sh` lays out the release in `stage/`.
