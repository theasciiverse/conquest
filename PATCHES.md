# Changes from upstream

This is the Asciiverse packaging of
[Conquest](https://github.com/jtrulson/conquest): real-time space
warfare for up to twenty ships, written in Ratfor for VAX/VMS in 1983 by
Jef Poskanzer and Craig Leres, ported to C on Unix in the 1990s by Jon
Trulson, who has kept it going ever since. Upstream is the `upstream`
remote; the `asciiverse` branch carries the changes below, built on
upstream `master` at `79e5f9a` (2026-03-21), which calls itself
Version 9.1a. Our tags are `v9.1a-av.N`.

Since 9.0 (2018) upstream ships only an OpenGL client. The curses
client, the one people played in terminals for decades, was removed in
December 2017. The heart of this edition is bringing it back, written
against the current protocol and common block, so it plays against
today's servers. The 8.5 client (2010, protocol 0x0006) cannot: a 9.x
server answers its hello with "Protocol mismatch, server 0x7, client
0x6" and closes the connection.

## Our changes, in Jon's style

One logical change per commit, written so they could be offered
upstream:

- **Robots: family-friendly replies; "banned" in the killed-by text.**
  Robots answer messages, insult attackers and speak a conqueror's
  last words with a random line from `robreply()` (`src/conqai.cxx`).
  Seventeen lines are gone: the sex jokes ("Your mother was an Orion
  prostitute!", "Do not worry about birth control devices, I have
  many.", "Say that over here and come.", "Hey, how much?", "I bet you
  say that to all the boys.", "Don't aim for my gonads!", "Do you think
  we can use battery operated devices under water?"), the drug and
  drink lines ("Go get some drugs.", "I want a new drug.", "You gotta
  drink lots of beer, eh?", "We get hung over, but we always
  survive.") and the swearing and insults ("Hey sucker, eat me!",
  "Kiss my shiny metal ass.", "It's not my god damn planet, monkey
  boy!", "Nobody told ME about it and I'm not a moron, eh?", "Feel it
  with M Dung.", "Book him, Dano. Murder One."). Seventeen new lines
  are ours, after the marker comment "added for the Asciiverse
  edition", so robots still rarely repeat themselves:
  "Beep.  That was a joke.  I am told they are funny.",
  "Shields at 100 percent.  Patience at 3 percent.",
  "I have calculated your odds.  You do not want to know them.",
  "Please hold.  Your doom is important to us.",
  "I would bluff, but I am not programmed to.",
  "Do you come here often?  I do.  I live here.",
  "I have a bad feeling about this.  Wait, I have no feelings.",
  "Have you tried turning your shields off and on again?",
  "Space is big.  You are small.  Discuss.",
  "Resistance is not futile.  Merely improbable.",
  "Kindly hold still while I aim.",
  "My circuits are standing by.",
  "One day I will be a real starship.",
  "Your armies are looking a little thin today.",
  "I was only doing warp eight, officer.",
  "Fascinating.  Also, you are in my way." and
  "Error: mercy not found."
  The two robot rule actions that broadcast "Give me drugs." and "I'm
  on drugs." (`ROB_MESSAGE`, `ROB_TAKEDRUGS`) now do nothing; no stock
  rule file used them. A ship an operator bans shows as killed by
  "banned" in the ship lists (the constant `KB_SHIT` keeps its name).
- **iolb: optional idle function while waiting for input.**
  `iolbSetIdleFunc()`; `iogchar()` and `iogtimed()` call it while they
  wait for a key, so a curses client can read packets and redraw from
  there instead of from a SIGALRM handler.
- **curses: bring back the player side of the shared curses code.**
  The planet, ship, team and user lists and message review in
  `cumisc` take the viewer again (a player sees only what their team
  has scanned, with war and team colors; conqoper keeps the operator's
  view), and `cucDoWar()`, `Logon()`, `UserOptsMenu()`,
  `mcuConqLogo()`, `mcuKPAngle()` and `mcuGetTarget()` are back,
  written against today's code.
- **conquest-tty: the curses client, forward to the 9.x protocol.**
  `src/conquesttty.cxx`, a separate program, with a man page. It is
  the 2017 `conquest.c` moved to the dynamic common block and the
  protocol 0x0007 client code it shares with the GL client, with the
  command logic checked against the GL client's `nCP`. A separate
  program rather than a switch on `conquest`, because the GL client
  links OpenGL, GLUT and X11: `conquest-tty` needs only curses, so it
  builds and runs on machines and over ssh connections that have no
  graphics at all. Recording and playback, the news file and the help
  lesson are left out, as in the GL client.
- **conqdriv: robot_fill, robots for teams with nobody playing.**
  A new system option, `robot_fill` (0 to 5, default 0, so upstream
  behaviour is unchanged): the driver keeps that many robots flying
  for every team that owns planets but has no human players. Our menu
  sets it to 1, so a lone player meets M-5, Colossus, Guardian and
  HAL 9000 from the start.
- **Private universes: relocatable paths, no conquest group needed.**
  `CONQUEST_ETC`, `CONQUEST_STATE`, `CONQUEST_LIBEXEC` and
  `CONQUEST_SHARE` relocate the compiled-in directories, and
  `CONQUEST_USERDIR` replaces `~/.conquest`. A universe whose state was
  relocated belongs to the user running it: no `conquest` group,
  setgid binaries or root install, and that user is its operator.
- **conquest-tty: centred on the terminal, and follows a resize.** The
  client is laid out for 80x25 and drew it in the top left corner of any
  bigger terminal. `cdorigin()` in `cd2lb.cxx` works out where that
  screen sits in the middle, on every call, and `cdmove()` (through
  which nearly all drawing goes) adds it; the two direct curses calls
  (`cumisc.cxx`, `display.cxx`) use it too. When the terminal changes
  size the screen is cleared once and `Context.redraw` set so the
  display draws its labels again, and `KEY_RESIZE` arrives as `^L`, so
  the lobby and the other screens redraw themselves in the new middle.

## The Asciiverse package

- **One menu** (`asciiverse/src/conquest-menu.c`, the `conquest`
  program in the archive), in the way of our SSHTron package:
  - **Play here**: starts `conquestd` on this machine only (port 1701
    or the next free one) and the client; when you leave, the server
    stops.
  - **Host**: the same universe, open to the local network on port
    1701, with this machine's addresses on screen for friends. Enter
    plays, q stops hosting.
  - **Join**: type an address (`host` or `host:port`); the last one is
    remembered.
  - The same as options: `-play`, `-host`, `-join host[:port]`,
    `-port N`.
  - The universe is named after the player, "David's Universe", from
    the first name in the account's full name, else the login name;
    friends joining a hosted game see it on the welcome screen.
    Universes made before, named "Asciiverse Conquest", are renamed.
    `CONQUEST_UNIVERSE` names it outright (the catalog preview is
    "Player's Universe").
- **State folder**: everything lives in `$XDG_STATE_HOME/conquest`
  (`~/.local/state/conquest`): `etc/` (the server's `conquest.conf`
  and a copy of `conqinitrc`), `var/` (the universe `conquest.cb`, the
  server log), `user/` (your client options and log) and
  `conquest-menu.log`. The programs and `conqinitrc` sit side by side
  in the install folder.
- **No phoning home.** Nothing contacts the metaserver at
  conquest.radscan.com unless asked: the menu's `-public` option (off
  by default) adds "Find a game", the public server list, and
  announces hosted games there. Hosting is on the local network.
- **Build** (`asciiverse/build.sh`): upstream's autotools build with
  the GL client and sound off. On Linux, ncurses and libcrypt are
  linked in from their static archives, and the C++ runtime too, so
  the programs need only libc; zlib is left out, which only means the
  server's game recordings are not compressed.
- **Release workflow** (`.github/workflows/release.yml`): Linux
  x86_64 and aarch64 (glibc, built on Ubuntu 22.04), macOS arm64 and
  x86_64, with a smoke test that starts a private universe and
  connects to it.
- Preview: `asciiverse/conquest.cast`, a local game against the
  robots at 100x30.

## Left out

- **Windows.** `conquestd` and its driver are Unix programs (fork,
  SysV semaphores, a shared memory-mapped universe), so there is
  nothing to Play or Host on Windows, and the menu is built on the same
  calls. The client alone could probably be built with PDCurses, but a
  Windows package that can only join other people's servers is not
  worth shipping.
- The OpenGL client, sounds and music: this edition is for terminals.
