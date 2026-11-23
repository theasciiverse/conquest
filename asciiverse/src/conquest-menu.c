/*
 * conquest: the Asciiverse front door to Conquest.
 *
 * One program for players and hosts.  It opens a menu:
 *
 *   Play here     a universe on this machine; robots fly for the
 *                 teams nobody is playing
 *   Host          the same universe, open to the local network, with
 *                 the addresses friends join with
 *   Join          a server by address
 *
 * It runs conquestd (Jon Trulson's server) and conquest-tty (the
 * curses client) from the folder it lives in.  The universe, the
 * server's configuration and log, and the client's options and log
 * all live in the state folder, $XDG_STATE_HOME/conquest (by default
 * ~/.local/state/conquest):
 *
 *   etc/    conquest.conf and conqinitrc (CONQUEST_ETC)
 *   var/    the universe (conquest.cb) and the server log (CONQUEST_STATE)
 *   user/   your client options and log (CONQUEST_USERDIR)
 *
 * Options:
 *   -play            straight into Play here
 *   -host            straight into Host
 *   -join host[:port]
 *   -port N          the port for Play here and Host (default 1701,
 *                    or the next free one)
 *   -public          also offer the public server list (the metaserver
 *                    at conquest.radscan.com), and announce hosted games
 *                    there.  Off unless given.
 *
 * This file is part of the Asciiverse packaging of Conquest, MIT
 * licensed like Conquest itself.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <curses.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pwd.h>
#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#define DEFAULT_PORT 1701
#define PORT_TRIES 10

static char exedir[PATH_MAX];
static char statedir[PATH_MAX];
static char etcdir[PATH_MAX];
static char vardir[PATH_MAX];
static char userdir[PATH_MAX];
static FILE *logf;

static int publicList = 0;      /* -public */
static int portWanted = 0;      /* -port */

static pid_t serverPid = 0;     /* the conquestd we started, if any */
static int serverPort = 0;
static int serverLan = 0;       /* listening on the network? */

static int cursesOn = 0;

/* ------------------------------------------------------------------ */
/* logging */

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    time_t now = time(NULL);
    char tbuf[32];

    if (!logf)
        return;
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(logf, "%s ", tbuf);
    va_start(ap, fmt);
    vfprintf(logf, fmt, ap);
    va_end(ap);
    fputc('\n', logf);
    fflush(logf);
}

/* ------------------------------------------------------------------ */
/* folders */

static int mkdirs(const char *path)
{
    char tmp[PATH_MAX];
    char *p;

    snprintf(tmp, sizeof(tmp), "%s", path);
    for (p = tmp + 1; *p; p++)
        if (*p == '/')
        {
            *p = 0;
            if (mkdir(tmp, 0700) < 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    if (mkdir(tmp, 0700) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

static void findExeDir(const char *argv0)
{
    char path[PATH_MAX] = "";
    char real[PATH_MAX];
    char *slash;

#if defined(__APPLE__)
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0)
        path[0] = 0;
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n > 0)
        path[n] = 0;
    else
        path[0] = 0;
#endif
    if (!path[0])
        snprintf(path, sizeof(path), "%s", argv0);

    if (realpath(path, real))
        snprintf(path, sizeof(path), "%s", real);

    snprintf(exedir, sizeof(exedir), "%s", path);
    slash = strrchr(exedir, '/');
    if (slash)
        *slash = 0;
    else
        snprintf(exedir, sizeof(exedir), ".");
}

static int setupFolders(void)
{
    const char *xdg = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");

    if (xdg && *xdg)
        snprintf(statedir, sizeof(statedir), "%s/conquest", xdg);
    else if (home && *home)
        snprintf(statedir, sizeof(statedir), "%s/.local/state/conquest", home);
    else
        return -1;

    snprintf(etcdir, sizeof(etcdir), "%s/etc", statedir);
    snprintf(vardir, sizeof(vardir), "%s/var", statedir);
    snprintf(userdir, sizeof(userdir), "%s/user", statedir);

    if (mkdirs(etcdir) || mkdirs(vardir) || mkdirs(userdir))
        return -1;

    char lpath[PATH_MAX];
    snprintf(lpath, sizeof(lpath), "%s/conquest-menu.log", statedir);
    logf = fopen(lpath, "a");

    /* every program we start finds its folders through these */
    setenv("CONQUEST_ETC", etcdir, 1);
    setenv("CONQUEST_STATE", vardir, 1);
    setenv("CONQUEST_LIBEXEC", exedir, 1);
    setenv("CONQUEST_SHARE", exedir, 1);
    setenv("CONQUEST_USERDIR", userdir, 1);

    return 0;
}

static int fileExists(const char *path)
{
    struct stat sb;
    return stat(path, &sb) == 0;
}

static int copyFile(const char *from, const char *to)
{
    char buf[8192];
    size_t n;
    FILE *in = fopen(from, "rb"), *out;

    if (!in)
        return -1;
    if (!(out = fopen(to, "wb")))
    {
        fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in);
    return fclose(out);
}

/* ------------------------------------------------------------------ */
/* running the Conquest programs */

static void cursesStop(void)
{
    if (cursesOn)
    {
        endwin();
        cursesOn = 0;
    }
}

static void cursesStart(void)
{
    if (!cursesOn)
    {
        refresh();
        cursesOn = 1;
    }
}

/* run a program from exedir and wait for it; returns its exit status */
static int runWait(const char *prog, char *const args[], int quiet)
{
    char path[PATH_MAX];
    pid_t pid;
    int status = 0;

    snprintf(path, sizeof(path), "%s/%s", exedir, prog);
    logmsg("run %s", path);

    if ((pid = fork()) < 0)
        return -1;
    if (pid == 0)
    {
        if (quiet)
        {
            int fd = open("/dev/null", O_RDWR);
            if (fd >= 0)
            {
                dup2(fd, 0);
                dup2(fd, 1);
                dup2(fd, 2);
            }
        }
        signal(SIGINT, SIG_DFL);
        execv(path, args);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* set "key=value" in etc/conquest.conf, replacing the existing line */
static void setConf(const char *key, const char *value)
{
    char path[PATH_MAX], tmp[PATH_MAX], line[1024];
    size_t klen = strlen(key);
    FILE *in, *out;
    int done = 0;

    snprintf(path, sizeof(path), "%s/conquest.conf", etcdir);
    snprintf(tmp, sizeof(tmp), "%s/conquest.conf.new", etcdir);
    if (!(in = fopen(path, "r")))
        return;
    if (!(out = fopen(tmp, "w")))
    {
        fclose(in);
        return;
    }
    while (fgets(line, sizeof(line), in))
    {
        if (!strncmp(line, key, klen) && line[klen] == '=')
        {
            fprintf(out, "%s=%s\n", key, value);
            done = 1;
        }
        else
            fputs(line, out);
    }
    if (!done)
        fprintf(out, "%s=%s\n", key, value);
    fclose(in);
    fclose(out);
    rename(tmp, path);
}

/* whether conquest.conf sets key to exactly value */
static int confIs(const char *key, const char *value)
{
    char path[PATH_MAX], line[1024], want[1024];
    FILE *in;
    int found = 0;

    snprintf(path, sizeof(path), "%s/conquest.conf", etcdir);
    snprintf(want, sizeof(want), "%s=%s\n", key, value);
    if (!(in = fopen(path, "r")))
        return 0;
    while (!found && fgets(line, sizeof(line), in))
        found = !strcmp(line, want);
    fclose(in);
    return found;
}

/*
 * The universe is named after the player, the way a server carries its
 * owner's name: "David's Universe", from the first name in the account's
 * full name, else the login name. Friends who join a hosted game see it
 * in the welcome screen.
 */
static void universeName(char *buf, size_t size)
{
    struct passwd *pw = getpwuid(getuid());
    char who[40] = "";
    const char *src = NULL;
    size_t i, len;

    /* CONQUEST_UNIVERSE names it outright (the catalog preview uses it) */
    if (getenv("CONQUEST_UNIVERSE") && getenv("CONQUEST_UNIVERSE")[0])
    {
        snprintf(buf, size, "%s", getenv("CONQUEST_UNIVERSE"));
        return;
    }

    if (pw && pw->pw_gecos && pw->pw_gecos[0] && pw->pw_gecos[0] != ',')
        src = pw->pw_gecos;
    else if (getenv("USER") && getenv("USER")[0])
        src = getenv("USER");
    else if (pw && pw->pw_name)
        src = pw->pw_name;

    /* the first word only: "David Sandquist,,," gives "David" */
    for (i = 0; src && src[i] && src[i] != ' ' && src[i] != ','
             && i < sizeof(who) - 1; i++)
        who[i] = src[i];
    who[i] = 0;

    if (!who[0])
    {
        snprintf(buf, size, "Your Universe");
        return;
    }
    who[0] = (char)toupper((unsigned char)who[0]);
    len = strlen(who);
    snprintf(buf, size, "%s%s Universe", who,
             who[len - 1] == 's' || who[len - 1] == 'S' ? "'" : "'s");
}

/* create the universe the first time */
static int prepareUniverse(void)
{
    char from[PATH_MAX], to[PATH_MAX];
    int fresh;

    /* conqoper -C below already creates an (empty) common block, so
       look for one first */
    snprintf(to, sizeof(to), "%s/conquest.cb", vardir);
    fresh = !fileExists(to);

    snprintf(to, sizeof(to), "%s/conqinitrc", etcdir);
    if (!fileExists(to))
    {
        snprintf(from, sizeof(from), "%s/conqinitrc", exedir);
        if (copyFile(from, to))
        {
            logmsg("cannot copy %s: %s", from, strerror(errno));
            return -1;
        }
    }

    snprintf(to, sizeof(to), "%s/conquest.conf", etcdir);
    if (!fileExists(to))
    {
        char *args[] = { "conqoper", "-C", NULL };
        runWait("conqoper", args, 1);
        if (!fileExists(to))
        {
            logmsg("conqoper -C failed");
            return -1;
        }
        setConf("server_name", "");    /* named just below */
        setConf("server_motd", "Keep your shields up in battle.");
        setConf("server_contact", "");
    }
    /* named after the player; universes made before carried the
       platform's name, and get the player's too */
    if (confIs("server_name", "") || confIs("server_name", "Asciiverse Conquest"))
    {
        char name[70];
        universeName(name, sizeof(name));
        setConf("server_name", name);
    }
    /* robots keep you company, one for each team nobody plays */
    setConf("robot_fill", "1");

    snprintf(to, sizeof(to), "%s/conquest.cb", vardir);
    if (fresh || !fileExists(to))
    {
        char *args[] = { "conqoper", "-I", "e", NULL };
        /* conqoper exits with 1 after initializing, so only a
           missing common block counts as a failure */
        runWait("conqoper", args, 1);
        if (!fileExists(to))
        {
            logmsg("conqoper -I e failed");
            return -1;
        }
        logmsg("a new universe in %s", vardir);
    }
    return 0;
}

/* is something listening on 127.0.0.1:port? */
static int portAnswers(int port)
{
    struct sockaddr_in sa;
    int s = socket(AF_INET, SOCK_STREAM, 0), rv;

    if (s < 0)
        return 0;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    rv = connect(s, (struct sockaddr *)&sa, sizeof(sa));
    close(s);
    return rv == 0;
}

/* can we bind this port (on all interfaces)? */
static int portFree(int port)
{
    struct sockaddr_in sa;
    int s = socket(AF_INET, SOCK_STREAM, 0), rv, on = 1;

    if (s < 0)
        return 0;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    rv = bind(s, (struct sockaddr *)&sa, sizeof(sa));
    close(s);
    return rv == 0;
}

static void stopServer(void)
{
    int i, status;

    if (serverPid <= 0)
        return;

    logmsg("stopping conquestd (pid %d)", (int)serverPid);
    /* conquestd, its client processes and the driver share its
       process group */
    kill(-serverPid, SIGTERM);
    for (i = 0; i < 30; i++)
    {
        if (waitpid(serverPid, &status, WNOHANG) == serverPid)
            break;
        usleep(100000);
    }
    kill(-serverPid, SIGKILL);
    waitpid(serverPid, &status, WNOHANG);
    serverPid = 0;
    serverPort = 0;
}

/* start conquestd; lan = listen on the network, not just this machine */
static int startServer(int lan)
{
    char path[PATH_MAX], portbuf[16], outpath[PATH_MAX];
    int port, i;
    pid_t pid;

    if (serverPid > 0 && serverLan == lan)
        return 0;
    stopServer();

    if (prepareUniverse())
        return -1;

    port = portWanted ? portWanted : DEFAULT_PORT;
    for (i = 0; i < PORT_TRIES && !portFree(port); i++)
        port++;
    if (i == PORT_TRIES)
        return -1;

    snprintf(path, sizeof(path), "%s/conquestd", exedir);
    snprintf(portbuf, sizeof(portbuf), "%d", port);
    snprintf(outpath, sizeof(outpath), "%s/conquestd.out", vardir);

    if ((pid = fork()) < 0)
        return -1;
    if (pid == 0)
    {
        int fd = open("/dev/null", O_RDWR), out;

        setsid();               /* our own process group, for stopServer() */
        if (fd >= 0)
            dup2(fd, 0);
        out = open(outpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out >= 0)
        {
            dup2(out, 1);
            dup2(out, 2);
        }
        signal(SIGINT, SIG_IGN);
        if (lan && publicList)
            execl(path, "conquestd", "-p", portbuf, "-m", (char *)NULL);
        else if (lan)
            execl(path, "conquestd", "-p", portbuf, (char *)NULL);
        else
            execl(path, "conquestd", "-l", "-p", portbuf, (char *)NULL);
        _exit(127);
    }

    serverPid = pid;
    serverPort = port;
    serverLan = lan;
    logmsg("conquestd pid %d on port %d (%s)", (int)pid, port,
           lan ? "local network" : "this machine only");

    /* wait for it to answer */
    for (i = 0; i < 50; i++)
    {
        int status;
        if (portAnswers(port))
            return 0;
        if (waitpid(pid, &status, WNOHANG) == pid)
        {
            serverPid = 0;
            logmsg("conquestd exited early, see %s", outpath);
            return -1;
        }
        usleep(100000);
    }
    stopServer();
    return -1;
}

/* run the client against host:port */
static void playOn(const char *host, int port)
{
    char addr[300];
    char *args[] = { "conquest-tty", "-s", addr, NULL };

    snprintf(addr, sizeof(addr), "%s:%d", host, port);
    cursesStop();
    runWait("conquest-tty", args, 0);
    cursesStart();
}

/* ------------------------------------------------------------------ */
/* the menu */

static const char *logo[] = {
    " CCC    OOO   N   N   QQQ   U   U  EEEEE   SSSS  TTTTT",
    "C   C  O   O  NN  N  Q   Q  U   U  E      S        T",
    "C      O   O  N N N  Q   Q  U   U  EEE     SSS     T",
    "C   C  O   O  N  NN  Q  Q   U   U  E          S    T",
    " CCC    OOO   N   N   QQ Q   UUU   EEEEE  SSSS     T",
};

enum { C_LOGO = 1, C_DIM, C_SEL, C_NOTE, C_WARN };

static void drawLogo(int *row)
{
    int i, w = (int)strlen(logo[0]), col = (COLS - w) / 2;

    if (col < 2)
        col = 2;
    attron(A_BOLD);
    mvaddch(*row, col - 2, ACS_ULCORNER);
    for (i = -1; i <= w; i++)
        addch(ACS_HLINE);
    addch(ACS_URCORNER);
    attroff(A_BOLD);
    (*row)++;
    for (i = 0; i < 5; i++, (*row)++)
    {
        attron(A_BOLD);
        mvaddch(*row, col - 2, ACS_VLINE);
        mvaddch(*row, col + w + 1, ACS_VLINE);
        attroff(A_BOLD);
        attron(COLOR_PAIR(C_LOGO) | A_BOLD);
        mvaddstr(*row, col, logo[i]);
        attroff(COLOR_PAIR(C_LOGO) | A_BOLD);
    }
    attron(A_BOLD);
    mvaddch(*row, col - 2, ACS_LLCORNER);
    for (i = -1; i <= w; i++)
        addch(ACS_HLINE);
    addch(ACS_LRCORNER);
    attroff(A_BOLD);
    (*row)++;
}

static void center(int row, int attr, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int col;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    col = (COLS - (int)strlen(buf)) / 2;
    if (col < 0)
        col = 0;
    attron(attr);
    mvaddstr(row, col, buf);
    attroff(attr);
}

/* the machine's IPv4 addresses other than loopback */
static int lanAddresses(char out[][64], int max)
{
    struct ifaddrs *ifa, *p;
    int n = 0;

    if (getifaddrs(&ifa) < 0)
        return 0;
    for (p = ifa; p && n < max; p = p->ifa_next)
    {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
            continue;
        if (p->ifa_flags & IFF_LOOPBACK)
            continue;
        if (!(p->ifa_flags & IFF_UP))
            continue;
        inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr,
                  out[n], 64);
        n++;
    }
    freeifaddrs(ifa);
    return n;
}

static int tooSmall(void)
{
    if (LINES >= 25 && COLS >= 80)
        return 0;
    erase();
    center(LINES / 2, A_BOLD, "Conquest needs a terminal of at least 80x25");
    center(LINES / 2 + 1, COLOR_PAIR(C_DIM), "(this one is %dx%d)", COLS, LINES);
    refresh();
    return 1;
}

static void message(const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    erase();
    center(LINES / 2 - 1, COLOR_PAIR(C_WARN) | A_BOLD, "%s", buf);
    center(LINES / 2 + 1, COLOR_PAIR(C_DIM), "press any key");
    refresh();
    timeout(-1);
    getch();
}

/* Host: keep the server up, show how to join, play when asked */
static void hostScreen(void)
{
    char addrs[8][64];
    int n, i, row, ch;

    if (startServer(1))
    {
        message("Could not start the server (see %s/conquestd.out)", vardir);
        return;
    }

    for (;;)
    {
        if (tooSmall())
        {
            timeout(500);
            if (getch() == 'q')
                break;
            continue;
        }
        erase();
        row = 1;
        drawLogo(&row);
        row++;
        center(row++, A_BOLD, "Hosting a universe on the local network");
        row++;
        n = lanAddresses(addrs, 8);
        if (n == 0)
            center(row++, COLOR_PAIR(C_WARN),
                   "This machine has no network address right now.");
        else
        {
            center(row++, COLOR_PAIR(C_DIM),
                   "Friends choose Join and type one of these:");
            row++;
            for (i = 0; i < n && row < LINES - 6; i++)
            {
                if (serverPort == DEFAULT_PORT)
                    center(row++, COLOR_PAIR(C_SEL) | A_BOLD, "%s", addrs[i]);
                else
                    center(row++, COLOR_PAIR(C_SEL) | A_BOLD, "%s:%d",
                           addrs[i], serverPort);
            }
            row++;
            center(row++, COLOR_PAIR(C_DIM),
                   "or run  conquest-tty -s <address>  in any terminal");
        }
        if (publicList)
            center(row++, COLOR_PAIR(C_NOTE),
                   "Also listed on the public server list.");
        row++;
        center(row++, 0, "Enter: play in your universe    q: stop hosting");
        refresh();

        timeout(1000);
        ch = getch();
        if (ch == 'q' || ch == 'Q' || ch == 27)
            break;
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER || ch == ' ')
            playOn("localhost", serverPort);
        if (serverPid > 0)
        {
            int status;
            if (waitpid(serverPid, &status, WNOHANG) == serverPid)
            {
                serverPid = 0;
                message("The server stopped (see %s/conquestd.out)", vardir);
                break;
            }
        }
    }
    stopServer();
}

static void playHere(void)
{
    if (startServer(0))
    {
        message("Could not start the server (see %s/conquestd.out)", vardir);
        return;
    }
    playOn("localhost", serverPort);
    stopServer();
}

/* read the last server joined */
static void lastServer(char *buf, size_t len, int save)
{
    char path[PATH_MAX];
    FILE *f;

    snprintf(path, sizeof(path), "%s/last-server", statedir);
    if (save)
    {
        if ((f = fopen(path, "w")))
        {
            fprintf(f, "%s\n", buf);
            fclose(f);
        }
        return;
    }
    buf[0] = 0;
    if ((f = fopen(path, "r")))
    {
        if (fgets(buf, (int)len, f))
            buf[strcspn(buf, "\r\n")] = 0;
        fclose(f);
    }
}

static void joinServer(const char *addr)
{
    char host[256];
    int port = DEFAULT_PORT;
    const char *colon = strrchr(addr, ':');

    snprintf(host, sizeof(host), "%s", addr);
    if (colon)
    {
        host[colon - addr] = 0;
        port = atoi(colon + 1);
        if (port <= 0)
            port = DEFAULT_PORT;
    }
    if (!host[0])
        snprintf(host, sizeof(host), "localhost");
    playOn(host, port);
}

static void publicServers(void)
{
    char *args[] = { "conquest-tty", "-m", NULL };

    cursesStop();
    runWait("conquest-tty", args, 0);
    cursesStart();
}

struct item {
    char key;
    const char *label;
    const char *note;
};

static void menu(void)
{
    struct item items[] = {
        { 'p', "Play here", "a universe on this machine, robots fly for the other teams" },
        { 'h', "Host", "open your universe to the local network" },
        { 'j', "Join", "type the address of a friend's game" },
        { 'f', "Find a game", "the public server list" },
        { 'q', "Quit", "" },
    };
    int nitems = sizeof(items) / sizeof(items[0]);
    int sel = 0, i, row, ch, editing = 0;
    char addr[256];
    size_t alen;

    lastServer(addr, sizeof(addr), 0);

    for (;;)
    {
        if (tooSmall())
        {
            timeout(-1);
            ch = getch();
            if (ch == 'q' || ch == 27)
                return;
            continue;
        }

        erase();
        row = 1;
        drawLogo(&row);
        row++;
        center(row++, COLOR_PAIR(C_DIM),
               "real-time space war for up to twenty captains, since 1983");
        row += 2;

        for (i = 0; i < nitems; i++)
        {
            int col = (COLS - 72) / 2;
            if (items[i].key == 'f' && !publicList)
                continue;
            if (col < 0)
                col = 0;
            if (i == sel)
            {
                attron(COLOR_PAIR(C_SEL) | A_BOLD);
                if (editing && items[i].key == 'j')
                    mvprintw(row, col, "> Join  %s_", addr);
                else
                    mvprintw(row, col, "> %-12s", items[i].label);
                attroff(COLOR_PAIR(C_SEL) | A_BOLD);
            }
            else
                mvprintw(row, col, "  %-12s", items[i].label);
            if (!(editing && items[i].key == 'j'))
            {
                attron(COLOR_PAIR(C_DIM));
                mvprintw(row, col + 16, "%s", items[i].note);
                attroff(COLOR_PAIR(C_DIM));
            }
            row += 2;
        }

        if (editing)
            center(LINES - 2, COLOR_PAIR(C_DIM),
                   "host or host:port, Enter joins, Esc goes back");
        else
            center(LINES - 2, COLOR_PAIR(C_DIM),
                   "arrows and Enter, or the first letter");
        refresh();

        timeout(-1);
        ch = getch();

        if (editing)
        {
            alen = strlen(addr);
            if (ch == 27)
                editing = 0;
            else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER)
            {
                if (alen)
                {
                    lastServer(addr, sizeof(addr), 1);
                    editing = 0;
                    joinServer(addr);
                }
            }
            else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8)
            {
                if (alen)
                    addr[alen - 1] = 0;
            }
            else if (ch >= 32 && ch < 127 && alen < sizeof(addr) - 1
                     && ch != ' ')
            {
                addr[alen] = (char)ch;
                addr[alen + 1] = 0;
            }
            continue;
        }

        if (ch == KEY_UP || ch == 'k' || ch == 'w')
        {
            do
                sel = (sel + nitems - 1) % nitems;
            while (items[sel].key == 'f' && !publicList);
            continue;
        }
        if (ch == KEY_DOWN || ch == '\t' || ch == 's')
        {
            do
                sel = (sel + 1) % nitems;
            while (items[sel].key == 'f' && !publicList);
            continue;
        }
        if (ch == 27)
            return;

        int pick = -1;
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER || ch == ' ')
            pick = sel;
        else
            for (i = 0; i < nitems; i++)
                if (ch == items[i].key && (items[i].key != 'f' || publicList))
                    pick = i;
        if (pick < 0)
            continue;
        sel = pick;

        switch (items[pick].key)
        {
        case 'p':
            playHere();
            break;
        case 'h':
            hostScreen();
            break;
        case 'j':
            editing = 1;
            break;
        case 'f':
            publicServers();
            break;
        case 'q':
            return;
        }
    }
}

static void onSignal(int sig)
{
    stopServer();
    cursesStop();
    _exit(128 + sig);
}

static void usage(void)
{
    fprintf(stderr,
            "usage: conquest [-play | -host | -join host[:port]] [-port N] [-public]\n");
    exit(2);
}

int main(int argc, char *argv[])
{
    const char *mode = NULL, *joinAddr = NULL;
    int i;

    for (i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] == '-')
            a++;
        if (!strcmp(a, "-play"))
            mode = "play";
        else if (!strcmp(a, "-host"))
            mode = "host";
        else if (!strcmp(a, "-join") && i + 1 < argc)
        {
            mode = "join";
            joinAddr = argv[++i];
        }
        else if (!strcmp(a, "-port") && i + 1 < argc)
            portWanted = atoi(argv[++i]);
        else if (!strcmp(a, "-public"))
            publicList = 1;
        else
            usage();
    }

    findExeDir(argv[0]);
    if (setupFolders())
    {
        fprintf(stderr, "conquest: cannot create the state folder\n");
        return 1;
    }
    logmsg("start, programs in %s, state in %s", exedir, statedir);

    signal(SIGTERM, onSignal);
    signal(SIGHUP, onSignal);
    signal(SIGINT, SIG_IGN);    /* the client ignores it too */
    signal(SIGQUIT, SIG_IGN);   /* ^\ leaves the client, not the menu */
    signal(SIGPIPE, SIG_IGN);

    initscr();
    cursesOn = 1;
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    if (has_colors())
    {
        start_color();
        use_default_colors();
        init_pair(C_LOGO, COLOR_RED, -1);
        init_pair(C_DIM, COLOR_CYAN, -1);
        init_pair(C_SEL, COLOR_YELLOW, -1);
        init_pair(C_NOTE, COLOR_GREEN, -1);
        init_pair(C_WARN, COLOR_RED, -1);
    }

    if (mode && !strcmp(mode, "play"))
        playHere();
    else if (mode && !strcmp(mode, "host"))
        hostScreen();
    else if (mode && !strcmp(mode, "join"))
        joinServer(joinAddr);
    else
        menu();

    stopServer();
    cursesStop();
    logmsg("exit");
    return 0;
}
