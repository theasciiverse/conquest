//
// Author: Jon Trulson <jon@radscan.com>
// Copyright (c) 1994-2018 Jon Trulson
//
// The MIT License
//
// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use, copy,
// modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
// BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
// ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

/* conquest-tty - the curses (text terminal) Conquest client.
 *
 * This is the curses client that was removed in 9.0 (it lived in
 * conquest.c until December 2017), brought forward to the current
 * protocol and common block.  It shares the client protocol code
 * (client.cxx, cproc.cxx) with the GL client, and the curses
 * display and list code (display.cxx, cumisc.cxx, cuclient.cxx) with
 * conqoper.
 *
 * Unlike the old client it does not use SIGALRM: incoming packets
 * are read and the display is updated from an iolb idle function,
 * called while waiting for keyboard input (iolbSetIdleFunc()).
 */

#include "c_defs.h"

#include <string>
#include <algorithm>

#include "global.h"
#include "conf.h"
#include "conqdef.h"
#include "cb.h"
#include "conqlb.h"
#include "rndlb.h"
#include "conqutil.h"
#include "conqunix.h"
#include "context.h"
#include "color.h"
#include "ui.h"
#include "options.h"
#include "cd2lb.h"
#include "iolb.h"
#include "cumisc.h"
#include "cuclient.h"
#include "ibuf.h"
#include "conqnet.h"
#include "packet.h"
#include "display.h"
#include "client.h"
#include "clientlb.h"
#include "clntauth.h"
#include "udp.h"
#include "meta.h"
#include "conqinit.h"
#include "serverflags.h"
#include "info.h"
#include "cprintf.h"
#include "conquest.h"

#include "fmt/format.h"

#define CONQUEST_TTY_NAME "Conquest TTY"

static char cbuf[BUFFER_SIZE_1024]; /* general purpose buffer */

/* true when the ship display may be updated from the idle function */
static bool dspOk = false;
/* set when the connection to the server has been lost */
static bool serverDead = false;
/* the user wants to leave */
static bool doLeave = false;

static const char *abt = "...aborted...";

/* the last firing directions */
static real lastblast = 0.0;
static real lastphase = 0.0;

static void conqend(void);

static void printUsage()
{
    printf("Usage: conquest-tty [-s server[:port]] [-m] [-M metaserver]\n");
    printf("                    [-u] [-v]\n\n");
    printf("    -s server[:port] connect to <server> at <port>\n");
    printf("                      default: localhost:%d\n", CN_DFLT_PORT);
    printf("    -m               query the metaserver for a list of servers\n");
    printf("    -M metaserver    specify alternate <metaserver> to contact.\n");
    printf("                      default: %s\n", META_DFLT_SERVER);
    printf("    -u               do not attempt to establish a UDP link to the server.\n");
    printf("    -v               be more verbose.  More '-v's increase verbosity\n\n");

    printf("The terminal must be at least 80 columns by 25 lines.\n");

    return;
}

/* read any waiting packets from the server */
static void readPackets(void)
{
    char buf[PKT_MAXSIZE];
    int rv;

    if (serverDead || pktNoNetwork())
        return;

    while ((rv = pktRead(buf, PKT_MAXSIZE, 0)) > 0)
        processPacket(buf);

    if (rv < 0)
    {
        utLog("%s: pktRead failed (%d), server connection lost",
              __FUNCTION__, rv);
        serverDead = true;
    }

    return;
}

/*  astservice - the idle function
 *
 *  Called by iolb while waiting for keyboard input.  It reads any
 *  packets from the server, drives the local universe, shows new
 *  messages and updates the ship display at the user's update rate.
 */
static void astservice(void)
{
    static uint32_t iterstart = 0;
    static uint32_t lastdisp = 0;
    uint32_t iternow;
    const uint32_t iterwait = 50; /* ms */
    real tdelta;
    uint64_t now;

    readPackets();

    iternow = clbGetMillis();
    tdelta = (real)iternow - (real)iterstart;

    /* drive the local universe */
    if (tdelta > iterwait)
    {
        clbPlanetDrive(tdelta / 1000.0);
        clbTorpDrive(tdelta / 1000.0);
        iterstart = iternow;
    }

    /* send a UDP keepalive packet if it's time */
    sendUDPKeepAlive(iternow);

    /* Don't do anything else if we're not supposed to. */
    if ( ! dspOk || Context.snum < 0 )
        return;

    /* one display update per (1 / updsec) seconds */
    if (Context.updsec < 1 || Context.updsec > 10)
        Context.updsec = 5;
    if ((iternow - lastdisp) < (1000 / Context.updsec))
        return;
    lastdisp = iternow;

    /* See if we can display a new message. */
    if ( Context.msgok )
    {
        if ( utDeltaGrand( Context.msgrand, &now ) >= NEWMSG_GRAND )
            if ( utGetMsg( Context.snum, &cbShips[Context.snum].lastmsg ) )
            {
                if (mcuReadMsg( cbShips[Context.snum].lastmsg,
                                MSG_MSG ) == true)
                {
                    if (!(cbMsgs[cbShips[Context.snum].lastmsg].from == MSG_FROM_SHIP
                          && (int)cbMsgs[cbShips[Context.snum].lastmsg].fromDetail == Context.snum) )
                    {
                        if (UserConf.MessageBell)
                            cdbeep();
                    }

                    Context.msgrand = now;
                }
            }
    }

    /* Perform one ship display update. */
    display( Context.snum );

    return;
}

/* update the ship display right away (after a command) */
static void redisplay(void)
{
    if ( dspOk && clbStillAlive( Context.snum ) )
        display( Context.snum );

    return;
}

/* wait for a number of seconds, keeping the display going */
static void idleWait(real seconds)
{
    uint32_t start = clbGetMillis();

    while ((clbGetMillis() - start) < (uint32_t)(seconds * 1000.0))
    {
        astservice();
        utSleep( ITER_SECONDS );
    }

    return;
}

static int connectServer(char *remotehost, uint16_t remoteport)
{
    int s;
    struct sockaddr_in sa;
    struct hostent *hp;
    int lin;

    if (!remotehost)
        return false;

    /* display the logo */
    lin = mcuConqLogo();

    lin += 5;

    if ((hp = gethostbyname(remotehost)) == NULL)
    {
        utLog("conquest-tty: %s: no such host", remotehost);

        cprintf(lin, 0, ALIGN_CENTER, "%s: no such host",
                remotehost);
        mcuPutPrompt(MTXT_DONE, MSG_LIN2 );
        cdrefresh();
        iogchar();

        return false;
    }

    /* put host's address and address type into socket structure */
    memcpy((char *)&sa.sin_addr, (char *)hp->h_addr, hp->h_length);
    sa.sin_family = hp->h_addrtype;

    sa.sin_port = htons(remoteport);

    if ((s = socket(AF_INET, SOCK_STREAM, 0 )) < 0)
    {
        utLog("socket: %s", strerror(errno));
        cprintf(lin, 0, ALIGN_CENTER, "socket: %s", strerror(errno));
        mcuPutPrompt(MTXT_DONE, MSG_LIN2 );
        cdrefresh();
        iogchar();
        return false;
    }

    utLog("Connecting to host: %s, port %d",
          remotehost, remoteport);

    cprintf(lin++, 0, ALIGN_CENTER, "Connecting to %s:%d",
            remotehost, remoteport);
    cdrefresh();

    /* connect to the remote server */
    if (connect(s, (const struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
        utLog("connect %s:%d: %s", remotehost, remoteport, strerror(errno));

        lin++;
        cprintf(lin++, 0, ALIGN_CENTER, "connect: %s", strerror(errno));
        cprintf(lin++, 0, ALIGN_CENTER,
                "Is there a conquestd server running there?");
        mcuPutPrompt(MTXT_DONE, MSG_LIN2 );
        cdrefresh();
        iogchar();
        close(s);

        return false;
    }

    cprintf(lin++, 0, ALIGN_CENTER, "Connected!");
    cdrefresh();

    serverDead = false;
    cInfo.sock = s;
    cInfo.servaddr = sa;

    pktSetSocketFds(cInfo.sock, PKT_SOCKFD_NOCHANGE);
    /* we won't setup the udp fd until we've negotiated successfully */
    pktSetNodelay();

    return true;
}

/* display the servers in a metaserver list and let the user pick one.
 * Returns the index, or -1 */
static int selectServer(metaServerVec_t& metaServerList)
{
    int i, lin, col, ch;
    int nums = metaServerList.size();
    int first = 0;
    const int perPage = MSG_LIN1 - 6;

    while (true)
    {
        cdclear();
        cprintf(1, 0, ALIGN_CENTER, "#%d#%s", LabelColor,
                "C O N Q U E S T   S E R V E R S");
        lin = 3;
        col = 2;
        for (i = first; i < nums && i < first + perPage; i++)
        {
            cprintf(lin++, col, ALIGN_NONE,
                    "#%d#%c#%d# - #%d#%-30.30s #%d#%s:%d #%d#(%d/%d ships)",
                    InfoColor, 'a' + (i - first), LabelColor,
                    NoColor, metaServerList[i].servername.c_str(),
                    SpecialColor, metaServerList[i].altaddr.c_str(),
                    metaServerList[i].port,
                    LabelColor,
                    metaServerList[i].numactive + metaServerList[i].numvacant
                    + metaServerList[i].numrobot,
                    metaServerList[i].numtotal);
        }

        cdclrl(MSG_LIN1, 2);
        cprintf(MSG_LIN1, 0, ALIGN_CENTER, "#%d#%s", InfoColor,
                "Press a letter to connect, [SPACE] for more, [ESC] to quit.");
        cdrefresh();

        ch = iogchar();

        if (ch == TERM_ABORT || ch == 'Q')
            return -1;

        if (ch == ' ')
        {
            first += perPage;
            if (first >= nums)
                first = 0;
            continue;
        }

        if (ch >= 'a' && ch < 'a' + perPage)
        {
            i = first + (ch - 'a');
            if (i < nums)
                return i;
        }

        cdbeep();
    }

    return -1;                  /* NOTREACHED */
}

/*  gretds - block letter "greetings..." */
static void gretds()
{
    int col,lin;
    static const char *g1=" GGG   RRRR   EEEEE  EEEEE  TTTTT   III   N   N   GGG    SSSS";
    static const char *g2="G   G  R   R  E      E        T      I    NN  N  G   G  S";
    static const char *g3="G      RRRR   EEE    EEE      T      I    N N N  G       SSS";
    static const char *g4="G  GG  R  R   E      E        T      I    N  NN  G  GG      S  ..  ..  ..";
    static const char *g5=" GGG   R   R  EEEEE  EEEEE    T     III   N   N   GGG   SSSS   ..  ..  ..";

    col = (int)(Context.maxcol - strlen(g5)) / (int)2;
    lin = 1;
    cprintf( lin,col,ALIGN_NONE,"#%d#%s", InfoColor, g1);
    lin++;
    cprintf( lin,col,ALIGN_NONE,"#%d#%s", InfoColor, g2);
    lin++;
    cprintf( lin,col,ALIGN_NONE,"#%d#%s", InfoColor, g3);
    lin++;
    cprintf( lin,col,ALIGN_NONE,"#%d#%s", InfoColor, g4);
    lin++;
    cprintf( lin,col,ALIGN_NONE,"#%d#%s", InfoColor, g5);

    return;

}

/* is the first char of a string a vowel? */
static bool vowel( char ch )
{
    switch (toupper(ch))
    {
    case 'A':
    case 'E':
    case 'I':
    case 'O':
    case 'U':
        return true;
    default:
        return false;
    }
}

/*  welcome - entry routine */
static bool welcome(void)
{
    int team, col = 0;
    char name[MAX_USERNAME];
    static const char *sorry1="I'm sorry, but the game is closed for repairs right now.";
    static const char *sorry2="I'm sorry, but there is no room for a new player right now.";
    static const char *sorryn="Please try again some other time.  Thank you.";
    static const char *selected_str="You have been selected to command a";
    static const char *starship_str=" starship.";
    static const char *prepare_str="Prepare to be beamed aboard...";
    int pkttype;
    char buf[PKT_MAXSIZE];
    bool done = false;

    if (!Logon(name))
    {
        utLog("conquest-tty: Logon failed or cancelled.");
        return false;
    }

    clientLastServerAckCode = 0;
    clientStatReceived = false;

    while (!done)
    {
        /* now look for SP_CLIENTSTAT or SP_ACK */
        if ((pkttype = pktRead(buf, PKT_MAXSIZE, 30)) <= 0)
        {
            utLog("welcome: read SP_CLIENTSTAT or SP_ACK failed: %d", pkttype);
            return false;
        }

        switch (pkttype)
        {
        case SP_CLIENTSTAT:
            if (PKT_PROCSP(buf))
                done = true;
            else
            {
                utLog("welcome: invalid CLIENTSTAT");
                return false;
            }
            break;
        case SP_ACK:
            PKT_PROCSP(buf);
            done = true;
            break;
        default:
            utLog("welcome: got unexpected packet type %d. Ignoring.",
                  pkttype);
            break;
        }
    }

    if ( pkttype == SP_CLIENTSTAT && (sClientStat.flags & SPCLNTSTAT_FLAG_NEW) )
    {
        /* Must be a new player. */
        cdclear();
        cdredo();
        if ( sStat.serverFlags & SERVER_F_CLOSED )
	{
            /* Can't enroll if the game is closed. */
            cprintf(MSG_LIN2/2,col,ALIGN_CENTER,"#%d#%s", InfoColor, sorry1 );
            cprintf(MSG_LIN2/2+1,col,ALIGN_CENTER,"#%d#%s", InfoColor, sorryn );
            cdmove( 1, 1 );
            cdrefresh();
            utSleep( 2.0 );
            return false;
	}

        team = sClientStat.team;
        gretds();			/* 'GREETINGS' */

        cprintf(MSG_LIN2/2,0,ALIGN_CENTER,"#%d#%s%s #%d#%s #%d#%s",
                InfoColor, selected_str,
                vowel(cbTeams[team].name[0]) ? "n" : "",
                CQC_A_BOLD, cbTeams[team].name,
                InfoColor, starship_str);
        cprintf(MSG_LIN2/2+1,0,ALIGN_CENTER,"#%d#%s",
                InfoColor, prepare_str );
        cdmove( 1, 1 );
        cdrefresh();
        utSleep( 3.0 );
    }

    if (pkttype == SP_ACK)	/* some problem was detected */
    {
        cdclear();
        cdredo();
        switch (clientLastServerAckCode)
	{
	case PERR_CLOSED:
            cprintf(MSG_LIN2/2,col,ALIGN_CENTER,"#%d#%s", InfoColor, sorry1 );
            cprintf(MSG_LIN2/2+1,col,ALIGN_CENTER,"#%d#%s", InfoColor, sorryn );
            break;
	case PERR_REGISTER:
            cprintf(MSG_LIN2/2,col,ALIGN_CENTER,"#%d#%s", InfoColor, sorry2 );
            cprintf(MSG_LIN2/2+1,col,ALIGN_CENTER,"#%d#%s", InfoColor, sorryn );
            break;
	case PERR_NOSHIP:
            cprintf(MSG_LIN2/2, 0, ALIGN_CENTER, "#%d#%s", InfoColor,
                    "I'm sorry, but there are no ships available right now.");
            cprintf(MSG_LIN2/2+1, 0, ALIGN_CENTER, "#%d#%s", InfoColor, sorryn);
            break;
	default:
            utLog("welcome: unexpected ACK code %d", clientLastServerAckCode);
            break;
	}
        cdmove( 1, 1 );
        cdrefresh();
        utSleep( 2.0 );
        return false;
    }

    /* if we are here, then we recieved a client stat pkt, and now must wait
       for a user packet for this user. */
    uint32_t startTime = clbGetMillis();
    bool gotUser = false;
    while ((clbGetMillis() - startTime) < 30000)
    {
        int rv;

        if ((rv = pktRead(buf, PKT_MAXSIZE, 1)) < 0)
        {
            utLog("welcome: pktRead returned error: %d", rv);
            return false;
        }

        if (rv == 0)
            continue;

        PKT_PROCSP(buf);
        if (rv == SP_USER)
        {
            gotUser = true;
            break;
        }
    }

    if (!gotUser || Context.snum < 0 || Context.unum < 0)
    {
        utLog("welcome: never received an SP_USER packet, or invalid "
              "snum/unum (%d/%d)", Context.snum, Context.unum);
        return false;
    }

    /* ready to rock. */
    return true;
}

/*  selectentry - select a system to enter */
static bool selectentry( uint8_t esystem )
{
    int i;
    int ch;
    bool owned[NUM_PLAYERTEAMS];
    std::string pbuf;

    /* First figure out which systems we can enter from. */
    for ( i = 0; i < NUM_PLAYERTEAMS; i++ )
        owned[i] = (esystem & (1 << i)) ? true : false;

    /* Prompt for a decision. */
    pbuf = "Enter which system";
    for ( i = 0; i < NUM_PLAYERTEAMS; i++ )
        if ( owned[i] )
        {
            pbuf += ", ";
            pbuf += cbTeams[i].name;
        }

    /* Change first comma to a colon. */
    size_t pos = pbuf.find(',');
    if (pos != std::string::npos)
        pbuf[pos] = ':';

    cdclrl( MSG_LIN1, 2 );
    cdputc( pbuf.c_str(), MSG_LIN1 );
    cdmove( 1, 1 );
    cdrefresh();

    while ( clbStillAlive( Context.snum ) && !serverDead )
    {
        if ( ! iogtimed( &ch, 1.0 ) )
            continue;

        switch  ( ch )
	{
	case TERM_NORMAL:
	case TERM_ABORT:	/* doesn't like the choices ;-) */
            sendCommand(CPCMD_ENTER, 0);
            return false;
            break;
	case TERM_EXTRA:
            /* Enter the home system. */
            sendCommand(CPCMD_ENTER, (uint16_t)(1 << cbShips[Context.snum].team));
            return true;
            break;
	default:
            for ( i = 0; i < NUM_PLAYERTEAMS; i++ )
                if ( cbTeams[i].teamchar == (char)toupper( ch ) && owned[i] )
                {
                    /* Found a good one. */
                    sendCommand(CPCMD_ENTER, (uint16_t)(1 << i));
                    return true;
                }
            /* Didn't get a good one; complain and try again. */
            cdbeep();
            cdrefresh();
            break;
	}
    }

    return false;	    /* can get here because of clbStillAlive() */

}

/*  newship - after a CPCMD_ENTER, wait for a ClientStat from the server
 *  (indicating our possibly new ship), or an ACK indicating a problem.
 */
static bool newship(void)
{
    uint32_t start = clbGetMillis();
    int ch;

    /* wait up to 30 seconds for the server */
    while ((clbGetMillis() - start) < 30000)
    {
        readPackets();

        if (serverDead)
            return false;

        if (clientLastServerAckCode)
        {
            utLog("newship: CPCMD_ENTER failed (ack code %d)",
                  clientLastServerAckCode);

            cdclear();
            cdredo();
            if (clientLastServerAckCode == PERR_FLYING)
            {
                cprintf(5,0,ALIGN_CENTER,"#%d#%s",InfoColor,
                        "You're already playing on another ship.");
                cbShips[Context.snum].status = SS_RESERVED;
            }
            else
                cprintf(5,0,ALIGN_CENTER,"#%d#Unexpected server reply, code %d",
                        InfoColor, clientLastServerAckCode);

            mcuPutPrompt( "--- press any key ---", MSG_LIN2 );
            cdrefresh();
            iogchar();
            return false;
        }

        if (clientStatReceived)
        {
            if (!sClientStat.esystem)
                return true;    /* we are done */

            /* otherwise, need to prompt for system to enter */
            return selectentry(sClientStat.esystem);
        }

        iogtimed(&ch, ITER_SECONDS);
    }

    utLog("newship: timed out waiting for a clientStat packet");
    return false;
}

/* review messages for a ship */
static void doReviewMsgs( int snum )
{
    int ch;
    int lstmsg;		/* saved last msg in case new ones come in */

    if (snum < 0 || snum >= cbLimits.maxShips())
        return;

    lstmsg = cbShips[snum].lastmsg;	/* don't want lstmsg changing
                                           while reading old ones. */

    if ( ! mcuReviewMsgs( snum, lstmsg ) )
    {
        uiPutMsg( "There are no old messages.", MSG_LIN1 );
        mcuPutPrompt( MTXT_MORE, MSG_LIN2 );
        cdrefresh();
        while ( ! iogtimed( &ch, 1.0 ) && clbStillAlive( Context.snum ) )
            ;
        cdclrl( MSG_LIN1, 2 );
    }

    return;
}

/* display the planet list for a ship */
static void doPlanetList( int snum )
{
    if (snum >= 0 && snum < cbLimits.maxShips())
        mcuPlanetList( cbShips[snum].team, snum );
    else		/* then use user team if user doen't have a ship yet */
        mcuPlanetList( cbUsers[Context.unum].team, -1 );

    return;
}

/* display the team list */
static void doTeamList( int team )
{
    int ch;

    cdclear();
    while ( clbStillAlive( Context.snum ) && !serverDead )
    {
        mcuTeamList( team );
        mcuPutPrompt( MTXT_DONE, MSG_LIN2 );
        cdrefresh();
        if ( iogtimed( &ch, 1.0 ) )
            break;
    }

    return;
}

/*  dowarp - set warp factor */
static void dowarp( int snum, real warp )
{
    cdclrl( MSG_LIN2, 1 );

    /* Handle ship limitations. */
    warp = std::min( warp, (real)cbShipTypes[cbShips[snum].shiptype].warpMax );

    if (!sendCommand(CPCMD_SETWARP, (uint16_t)warp))
        return;

    sprintf( cbuf, "Warp %d.", (int) warp );
    uiPutMsg( cbuf, MSG_LIN1 );

    return;
}

/*  doalloc - change weapon/engine allocations */
static void doalloc( int snum )
{
    char ch;
    int alloc;
    int dwalloc = 0;
    static const char *pmt="New weapons allocation: (30-70) ";

    cdclrl( MSG_LIN1, 2 );
    cbuf[0] = 0;
    ch = (char)cdgetx( pmt, MSG_LIN1, 1, TERMS, cbuf, MAX_MSGLINE_LENGTH, true );

    switch (ch)
    {
    case TERM_EXTRA:
        dwalloc = cbShips[snum].engalloc;
        break;

    case TERM_NORMAL:
        alloc = 0;
        utSafeCToI( &alloc, cbuf, 0 );			/* ignore status */
        if ( alloc != 0 )
	{
            if ( alloc < 30 )
                alloc = 30;
            else if ( alloc > 70 )
                alloc = 70;
            dwalloc = alloc;
	}
        else
	{
            cdclrl( MSG_LIN1, 1 );
            return;
	}

        break;

    default:
        cdclrl( MSG_LIN1, 1 );
        return;
    }

    sendCommand(CPCMD_ALLOC, (uint16_t)dwalloc);

    cdclrl( MSG_LIN1, 1 );

    return;

}

/*  doautopilot - handle the autopilot */
static void doautopilot( int snum )
{
    int ch;
    static const char *conf="Press [TAB] to engage autopilot:";

    cdclrl( MSG_LIN1, 2 );
    cbuf[0] = 0;
    if ( cdgetx( conf, MSG_LIN1, 1, TERMS, cbuf, MAX_MSGLINE_LENGTH,
                 true ) != TERM_EXTRA )
    {
        cdclrl( MSG_LIN1, 1 );
        return;
    }

    clientLastServerAckCode = 0;
    sendCommand(CPCMD_AUTOPILOT, 1); /* turn over command to a machine */

    while ( clbStillAlive( Context.snum ) && !serverDead )
    {
        if (clientLastServerAckCode)
            break;

        /* Get a character. */
        if ( ! iogtimed( &ch, 1.0 ) )
            continue;		/* next . echo */

        if (ch == TERM_ABORT)
            break;
        else if (ch == TERM_REDRAW)
        {
            cdredo();
            Context.redraw = true;
        }
        else
        {
            uiPutMsg( "Press [ESC] to abort autopilot.", MSG_LIN1 );
            cdbeep();
            cdrefresh();
        }
    }

    sendCommand(CPCMD_AUTOPILOT, 0);

    cdclrl( MSG_LIN1, 2 );

    return;
}

/* wait for the server to finish (or refuse) a bombing or beaming run,
 * aborting it on a keypress. cmd is CPCMD_BOMB or CPCMD_BEAM */
static void waitForServer( uint8_t cmd )
{
    while(true)
    {
        if ( ! clbStillAlive( Context.snum ) || serverDead )
            return;

        if ( iochav() )
	{
            uiPutMsg( abt, MSG_LIN1 );
            sendCommand(cmd, 0); /* stop! */
            ioeat();
            break;
	}

        if (clientLastServerAckCode)
	{
            sendCommand(cmd, 0); /* make sure */
            break;
	}

        astservice();
        utSleep( ITER_SECONDS );
    }

    cdrefresh();

    return;
}

/*  dobeam - beam armies up or down */
static void dobeam( int snum )
{
    int pnum, num, upmax, downmax, capacity, beamax, i;
    bool dirup = false;
    int ch;
    char buf[MAX_MSGLINE_LENGTH];
    real rkills;
    bool done = false;
    static const char *lastfew="Fleet orders prohibit removing the last three armies.";

    cdclrl( MSG_LIN1, 2 );

    /* all of these checks are performed server-side as well, but we also
       check here for the obvious problems so we can save time without
       bothering the server for something it will refuse anyway. */

    /* Check for allowability. */
    if ( cbShips[snum].warp >= 0.0 )
    {
        uiPutMsg( "We must be orbiting a planet to use the transporter.",
                   MSG_LIN1 );
        return;
    }

    // the assumption here is that if you are orbiting (warp < 0), you
    // are locked onto a planet...
    pnum = cbShips[snum].lockDetail;
    if ( cbShips[snum].armies > 0 )
    {
        if ( cbPlanets[pnum].type == PlanetType::Sun )
	{
            uiPutMsg( "Idiot!  Our armies will fry down there!", MSG_LIN1 );
            return;
	}
        else if ( cbPlanets[pnum].type == PlanetType::Moon )
	{
            uiPutMsg( "Fool!  Our armies will suffocate down there!",
                       MSG_LIN1 );
            return;
	}
        else if ( cbPlanets[pnum].team == Team::God )
	{
            uiPutMsg(
                "GOD->you: YOUR ARMIES AREN'T GOOD ENOUGH FOR THIS PLANET.",
                MSG_LIN1 );
            return;
	}
    }

    i = cbPlanets[pnum].uninhabtime;
    if ( i > 0 )
    {
        sprintf( cbuf, "This planet is uninhabitable for %d more minute%s.",
                 i, (i != 1) ? "s" : "" );
        uiPutMsg( cbuf, MSG_LIN1 );
        return;
    }

    /* can take empty planets */
    if ( cbPlanets[pnum].team != cbShips[snum].team &&
         cbPlanets[pnum].team != Team::SelfRuled &&
         cbPlanets[pnum].team != Team::NoTeam )
        if ( ! cbShips[snum].war[cbPlanets[pnum].team] && cbPlanets[pnum].armies != 0)
        {
            uiPutMsg( "But we are not at war with this planet!", MSG_LIN1 );
            return;
        }

    if ( cbShips[snum].armies == 0 &&
         cbPlanets[pnum].team == cbShips[snum].team && cbPlanets[pnum].armies <= MIN_BEAM_ARMIES )
    {
        uiPutMsg( lastfew, MSG_LIN1 );
        return;
    }

    rkills = cbShips[snum].kills;

    if ( rkills < (real)1.0 )
    {
        uiPutMsg(
            "Fleet orders prohibit beaming armies until you have a kill.",
            MSG_LIN1 );
        return;
    }

    /* Figure out what can be beamed. */
    downmax = cbShips[snum].armies;
    if ( clbSPWar(snum,pnum) ||
         cbPlanets[pnum].team == Team::SelfRuled ||
         cbPlanets[pnum].team == Team::NoTeam ||
         cbPlanets[pnum].team == Team::God ||
         cbPlanets[pnum].armies == 0 )
    {
        upmax = 0;
    }
    else
    {
        capacity = (int)std::min( rkills * 2,
                                  (real)cbShipTypes[cbShips[snum].shiptype].armyMax );
        upmax = std::min( cbPlanets[pnum].armies - MIN_BEAM_ARMIES,
                          capacity - cbShips[snum].armies );
    }

    /* If there are armies to beam but we're selfwar... */
    if ( upmax > 0 && selfwar(snum) && cbShips[snum].team == cbPlanets[pnum].team )
    {
        if ( downmax <= 0 )
	{
            sprintf(cbuf, "The arm%s reluctant to beam aboard a pirate vessel.",
                    (upmax == 1) ? "y is" : "ies are");
            uiPutMsg( cbuf, MSG_LIN1 );
            return;
	}
        upmax = 0;
    }

    /* Figure out which direction to beam. */
    if ( upmax <= 0 && downmax <= 0 )
    {
        uiPutMsg( "There is no one to beam.", MSG_LIN1 );
        return;
    }

    if ( upmax <= 0 )
        dirup = false;
    else if ( downmax <= 0 )
        dirup = true;
    else
    {
        uiPutMsg( "Beam [up or down] ", MSG_LIN1 );
        cdrefresh();
        done = false;
        while ( clbStillAlive( Context.snum ) && !done && !serverDead )
	{
            if ( ! iogtimed( &ch, 1.0 ) )
                continue;	/* next */

            switch ( (char)tolower( ch ) )
	    {
	    case 'u':
                dirup = true;
                done = true;
                break;
	    case 'd':
	    case TERM_EXTRA:
                dirup = false;
                done = true;
                break;
	    default:
                uiPutMsg( abt, MSG_LIN1 );
                return;
	    }
	}
        if (!done)
            return;
    }

    if ( dirup )
        beamax = upmax;
    else
        beamax = downmax;

    /* Figure out how many armies should be beamed. */
    sprintf( cbuf, "Beam %s [1-%d] ", (dirup) ? "up" : "down", beamax );
    cdclrl( MSG_LIN1, 1 );
    buf[0] = 0;
    ch = cdgetx( cbuf, MSG_LIN1, 1, TERMS, buf, MAX_MSGLINE_LENGTH, true );
    if ( ch == TERM_ABORT )
    {
        uiPutMsg( abt, MSG_LIN1 );
        return;
    }
    else if ( ch == TERM_EXTRA && buf[0] == 0 )
        num = beamax;
    else
    {
        utDeleteBlanks( buf );
        if (!utIsDigits(buf))
	{
            uiPutMsg( abt, MSG_LIN1 );
            return;
	}
        num = 0;
        utSafeCToI( &num, buf, 0 );			/* ignore status */
        if ( num < 1 || num > beamax )
	{
            uiPutMsg( abt, MSG_LIN1 );
            return;
	}
    }

    /* now we start the phun. */
    clientLastServerAckCode = 0;

    /* detail is (armies & 0x00ff), 0x8000 set if beaming down */
    sendCommand(CPCMD_BEAM,
                (dirup) ? (uint16_t)(num & 0x00ff):
                (uint16_t)((num & 0x00ff) | 0x8000));

    waitForServer(CPCMD_BEAM);

    return;

}

/*  dobomb - bombard a planet */
static void dobomb( int snum )
{
    int pnum;
    char  buf[MAX_MSGLINE_LENGTH];

    SFCLR(snum, SHIP_F_REPAIR);

    cdclrl( MSG_LIN1, 2 );

    /* Check for allowability. */
    if ( cbShips[snum].warp >= 0.0 )
    {
        uiPutMsg( "We must be orbiting a planet to bombard it.", MSG_LIN1 );
        return;
    }
    pnum = cbShips[snum].lockDetail;
    if ( cbPlanets[pnum].type == PlanetType::Sun
         || cbPlanets[pnum].type == PlanetType::Moon
         || cbPlanets[pnum].team == Team::NoTeam
         || cbPlanets[pnum].armies == 0 )
    {
        uiPutMsg( "There is no one there to bombard.", MSG_LIN1 );
        return;
    }
    if ( cbPlanets[pnum].team == cbShips[snum].team )
    {
        uiPutMsg( "We can't bomb our own armies!", MSG_LIN1 );
        return;
    }

    if ( cbPlanets[pnum].team != Team::SelfRuled
         && cbPlanets[pnum].team != Team::God )
        if ( ! cbShips[snum].war[cbPlanets[pnum].team] )
        {
            uiPutMsg( "But we are not at war with this planet!", MSG_LIN1 );
            return;
        }

    /* Confirm. */
    sprintf( cbuf, "Press [TAB] to bombard %s, %d armies:",
             cbPlanets[pnum].name, cbPlanets[pnum].armies );
    cdclrl( MSG_LIN1, 2 );
    buf[0] = 0;
    if ( cdgetx( cbuf, MSG_LIN1, 1, TERMS, buf, MAX_MSGLINE_LENGTH,
                 true) != TERM_EXTRA )
    {
        cdclrl( MSG_LIN1, 2 );
        return;
    }

    clientLastServerAckCode = 0; /* quit if something happens server-side */
    sendCommand(CPCMD_BOMB, 1);	/* start bombing */

    waitForServer(CPCMD_BOMB);

    return;

}

/*  dotorp - launch torpedoes (num = 1, or 3 for a burst) */
static void dotorp( int snum, int num )
{
    real dir;

    cdclrl( MSG_LIN2, 1 );

    if ( mcuGetTarget( (num > 1) ? "Torpedo burst: " : "Launch torpedo: ",
                       MSG_LIN1, 1, &dir, lastblast ) )
    {
        lastblast = lastphase = dir;
        /* the server will do the real checks */
        if ( num == 1 && ! clbCheckLaunch( snum, 1 ) )
            uiPutMsg( ">TUBES EMPTY<", MSG_LIN2 );

        sendFireTorps(num, dir);
        cdclrl( MSG_LIN1, 1 );
    }
    else
        uiPutMsg( "Invalid targeting information.", MSG_LIN1 );

    return;

}

/*  dophase - do a fire phasers command */
static void dophase( int snum, bool same )
{
    real dir;

    cdclrl( MSG_LIN2, 1 );

    if ( same )
        dir = lastphase;
    else if ( mcuGetTarget( "Fire phasers: ", MSG_LIN1, 1, &dir, lastphase ) )
        lastblast = lastphase = dir;
    else
    {
        uiPutMsg( "Invalid targeting information.", MSG_LIN1 );
        return;
    }

    cdclrl( MSG_LIN1, 1 );
    uiPutMsg( "Firing phasers...", MSG_LIN2 );
    sendCommand(CPCMD_FIREPHASER, (uint16_t)(dir * 100.0));

    return;

}

/*  docloak - cloaking device control */
static void docloak( int snum )
{
    static const char *pmt="Press [TAB] to engage cloaking device: ";

    cdclrl( MSG_LIN1, 2 );

    if ( SCLOAKED(snum) )
    {
        sendCommand(CPCMD_CLOAK, 0);
        uiPutMsg( "Cloaking device disengaged.", MSG_LIN1 );
        return;
    }

    cbuf[0] = 0;
    if ( cdgetx( pmt, MSG_LIN1, 1, TERMS, cbuf, MAX_MSGLINE_LENGTH,
                 true) == TERM_EXTRA )
        sendCommand(CPCMD_CLOAK, 0);

    cdclrl( MSG_LIN1, 1 );

    return;

}

/*  dorefit - attempt to change the nature of things */
static void dorefit( int snum )
{
    int ch, pnum;
    int oldstype = 0, stype = 0;
    bool leave;
    static const char *ntp="We must be orbiting a team owned planet to refit.";
    static const char *nek="You must have at least one kill to refit.";
    static const char *conf="Press [TAB] to change, [ENTER] to accept: ";
    static const char *cararm="You cannot refit while carrying armies";

    cdclrl( MSG_LIN2, 1 );

    /* Check for allowability. */
    if ( oneplace( cbShips[snum].kills ) < MIN_REFIT_KILLS )
    {
        uiPutMsg( nek, MSG_LIN1 );
        return;
    }

    if (cbShips[snum].warp >= 0.0 || cbShips[snum].lock != LOCK_PLANET)
    {
        uiPutMsg( ntp, MSG_LIN1 );
        return;
    }

    pnum = cbShips[snum].lockDetail;
    if (pnum >= cbLimits.maxPlanets()
        || cbPlanets[pnum].team != cbShips[snum].team)
    {
        uiPutMsg( ntp, MSG_LIN1 );
        return;
    }

    if (cbShips[snum].armies != 0)
    {
        uiPutMsg( cararm, MSG_LIN1 );
        return;
    }

    /* we have at least 1 kill, and are */
    /* orbiting a team owned planet */
    stype = oldstype = cbShips[snum].shiptype;
    leave = false;

    while ( clbStillAlive( snum ) && !leave && !serverDead )
    {
        /* Display the current options. */
        sprintf(cbuf, "Refit ship type: %s", cbShipTypes[stype].name);
        uiPutMsg(cbuf, MSG_LIN1);
        uiPutMsg(conf, MSG_LIN2);
        cdrefresh();

        /* Get a character. */
        if ( ! iogtimed( &ch, 1.0 ) )
            continue; /* next; */

        switch ( ch )
        {
        case TERM_EXTRA:
            stype = mod( stype + 1, MAXNUMSHIPTYPES );
            break;

        case TERM_NORMAL:
            leave = true;
            break;

        case TERM_ABORT:
            /* Decided to abort; restore things. */
            stype = oldstype;
            leave = true;
            break;

        default:
            cdbeep();
            break;
        }
    }

    cdclrl( MSG_LIN1, 2 );

    if (stype == oldstype || !clbStillAlive( snum ))
        return;

    /* Now wait it out... */
    sprintf(cbuf, "Refitting ship. This will take %d seconds to complete...",
            REFIT_GRAND/1000);
    uiPutMsg( cbuf, MSG_LIN1 );
    sendCommand(CPCMD_REFIT, (uint16_t)stype);
    cdrefresh();

    uint32_t start = clbGetMillis();
    while ( (clbGetMillis() - start) < REFIT_GRAND )
    {
        /* See if we're still alive. */
        if ( ! clbStillAlive( snum ) || serverDead )
            return;

        astservice();
        utSleep( ITER_SECONDS );
    }

    cdclrl( MSG_LIN1, 2 );

    return;

}

/*  docoup - attempt to rise from the ashes */
static void docoup( int snum )
{
    int i, pnum;
    static const char *nhp="We must be orbiting our home planet to attempt a coup.";
    static const char *conf="Press [TAB] to try it: ";

    cdclrl( MSG_LIN2, 1 );

    /* some checks we will do locally, the rest will have to be handled by
       the server */
    /* Check for allowability. */
    if ( oneplace( cbShips[snum].kills ) < MIN_COUP_KILLS )
    {
        uiPutMsg(
            "Fleet orders require three kills before a coup can be attempted.",
            MSG_LIN1 );
        return;
    }
    for ( i = 0; i < cbLimits.maxPlanets(); i++ )
        if ( cbPlanets[i].team == cbShips[snum].team && cbPlanets[i].armies > 0 )
        {
            uiPutMsg( "We don't need to coup, we still have armies left!",
                       MSG_LIN1 );
            return;
        }
    if ( cbShips[snum].warp >= 0.0 || cbShips[snum].lock != LOCK_PLANET)
    {
        uiPutMsg( nhp, MSG_LIN1 );
        return;
    }
    pnum = (int)cbShips[snum].lockDetail;
    if ( pnum != cbTeams[cbShips[snum].team].homeplanet )
    {
        uiPutMsg( nhp, MSG_LIN1 );
        return;
    }
    if ( cbPlanets[pnum].armies > MAX_COUP_ENEMY_ARMIES )
    {
        uiPutMsg( "The enemy is still too strong to attempt a coup.",
                   MSG_LIN1 );
        return;
    }
    i = cbPlanets[pnum].uninhabtime;
    if ( i > 0 )
    {
        sprintf( cbuf, "This planet is uninhabitable for %d more minutes.",
                 i );
        uiPutMsg( cbuf, MSG_LIN1 );
        return;
    }

    /* at this point, we will just send the request, and let the server
       deal with it */

    /* Confirm. */
    cdclrl( MSG_LIN1, 1 );
    cbuf[0] = 0;
    if ( cdgetx( conf, MSG_LIN1, 1, TERMS, cbuf, MAX_MSGLINE_LENGTH,
                 true) != TERM_EXTRA )
    {
        uiPutMsg( abt, MSG_LIN1 );
        return;
    }

    uiPutMsg( "Attempting coup...", MSG_LIN1 );
    sendCommand(CPCMD_COUP, 0);

    return;

}

/*  docourse - set course */
static void docourse( int snum )
{
    int j, sorpnum = 0, xsorpnum = 0, count = 0;
    InfoNear what;
    InfoSpecial token;
    real dir = 0.0, appx, appy;
    int ch;
    courseLock_t newlock = LOCK_NONE;
    uint16_t newlockDetail = 0;

    cdclrl( MSG_LIN1, 2 );

    cbuf[0] = 0;
    ch = cdgetx( "Come to course: ", MSG_LIN1, 1, TERMS, cbuf,
                 MAX_MSGLINE_LENGTH, true );

    std::string buf = cbuf;
    utDeleteBlanks( buf );
    if ( ch == TERM_ABORT || buf.empty() )
    {
        cdclrl( MSG_LIN1, 1 );
        return;
    }

    what = NEAR_ERROR;

    // we need to treat a search for a ship a bit specially...
    if ( buf.size() > 1 && buf[0] == 's' && ::isdigit(buf[1]) )
    {
        std::string substr = buf.substr(1);
        // make sure they are all digits...
        if (utIsDigits(substr))
        {
            /* Ship. */
            if ( utSafeCToI( &sorpnum, substr) )
                what = NEAR_SHIP;
        }
    }
    else
    {
        InfoNear near;

        if (utIsDigits(buf))
        {
            /* Raw angle. */
            cdclrl( MSG_LIN1, 1 );
            if ( utSafeCToI( &j, buf) )
            {
                what = NEAR_DIRECTION;
                dir = (real)utMod360( (real)( j ) );
            }
        }
        else if ( utArrowsToDir( buf.c_str(), &dir ) )
            what = NEAR_DIRECTION;
        else if ( utIsSpecial( buf, &near, &token, &count ) )
        {
            if ( clbFindSpecial( snum, token, count, &sorpnum, &xsorpnum ) )
                what = near;
            else
                what = NEAR_NONE;
        }
        else if ( clbPlanetMatch( buf, &sorpnum, false ) )
            what = NEAR_PLANET;
    }

    switch ( what )
    {
    case NEAR_SHIP:
        if ( sorpnum < 0 || sorpnum >= cbLimits.maxShips() )
	{
            uiPutMsg( "No such ship.", MSG_LIN2 );
            return;
	}
        if ( sorpnum == snum )
	{
            cdclrl( MSG_LIN1, 1 );
            return;
	}
        if ( cbShips[sorpnum].status != SS_LIVE )
	{
            uiPutMsg( "Not found.", MSG_LIN2 );
            return;
	}

        if ( SCLOAKED(sorpnum) )
	{
            if ( cbShips[sorpnum].warp <= 0.0 )
	    {
                uiPutMsg( "Sensors are unable to lock on.", MSG_LIN2 );
                return;
	    }
	}

        appx = cbShips[sorpnum].x;
        appy = cbShips[sorpnum].y;

        dir = utAngle( cbShips[snum].x, cbShips[snum].y, appx, appy );

        /* Give info if he used TAB. */
        if ( ch == TERM_EXTRA )
            infoShip( sorpnum, snum, true );
        else
            cdclrl( MSG_LIN1, 1 );
        break;
    case NEAR_PLANET:
        dir = utAngle( cbShips[snum].x, cbShips[snum].y,
                       cbPlanets[sorpnum].x, cbPlanets[sorpnum].y );
        if ( ch == TERM_EXTRA )
	{
            newlock = LOCK_PLANET;
            newlockDetail = sorpnum;
            infoPlanet( "Now locked on to ", sorpnum, snum, true );
	}
        else
            infoPlanet( "Setting course for ", sorpnum, snum, true );
        break;
    case NEAR_DIRECTION:
        cdclrl( MSG_LIN1, 1 );
        break;
    case NEAR_NONE:
        uiPutMsg( "Not found.", MSG_LIN2 );
        return;
        break;
    default:
        /* This includes NEAR_ERROR. */
        uiPutMsg( "I don't understand.", MSG_LIN2 );
        return;
        break;
    }

    sendSetCourse(cInfo.sock, newlock, newlockDetail, dir);

    return;

}

/*  dodet - detonate enemy torps */
static void dodet( int snum )
{
    cdclrl( MSG_LIN2, 1 );

    if ( cbShips[snum].wfuse > 0 )
        uiPutMsg( "Weapons are currently overloaded.", MSG_LIN1 );
    else if ( clbUseFuel( snum, DETONATE_FUEL, true, false ) )
    {				/* we don't really use fuel here on the
				   client*/
        uiPutMsg( "detonating...", MSG_LIN1 );
        sendCommand(CPCMD_DETENEMY, 0);
    }
    else
        uiPutMsg( "Not enough fuel to fire detonators.", MSG_LIN1 );

    return;

}

/*  dodistress - send an emergency distress call */
static void dodistress( int snum )
{
    static const char *pmt="Press [TAB] to send an emergency distress call: ";

    cdclrl( MSG_LIN1, 2 );

    cbuf[0] = 0;
    if ( cdgetx( pmt, MSG_LIN1, 1, TERMS, cbuf, MAX_MSGLINE_LENGTH,
                 true) == TERM_EXTRA )
        sendCommand(CPCMD_DISTRESS, (uint16_t)UserConf.DistressToFriendly);

    cdclrl( MSG_LIN1, 1 );

    return;

}

/*  dohelp - display a list of commands */
static void dohelp( void )
{
    int lin, col, tlin;
    int ch;
    static bool FirstTime = true;
    static char sfmt[MAX_MSGLINE_LENGTH * 2];

    if (FirstTime)
    {
        FirstTime = false;
        sprintf(sfmt,
                "#%d#%%-9s#%d#%%s",
                InfoColor,
                LabelColor);
    }

    cdclear();

    cprintf(1,0,ALIGN_CENTER, "#%d#%s", LabelColor, "CONQUEST COMMANDS");

    lin = 3;

    /* Display the left side. */
    tlin = lin;
    col = 4;
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "0-9,=", "set warp factor (= is 10)");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "A", "change w/e allocations");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "b", "beam armies");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "B", "bombard a planet");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "C", "cloaking device");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "d,*", "detonate enemy torpedoes");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "D", "detonate your own torpedoes");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "E", "send emergency distress call");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "f", "fire phasers");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "F", "fire phasers, same direction");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "h", "this");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "H", "user history");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "i", "information");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "k", "set course");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "K", "try a coup");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "L", "review old messages");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "m", "send a message");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "M", "short/long range sensor toggle");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "N", "change your name");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "[SPACE]", "toggle map/lrscan");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "arrows", "turn to that heading");

    /* Now do the right side. */
    tlin = lin;
    col = 44;
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "O", "options menu");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "o", "come into orbit");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "p", "launch photon torpedo");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "P", "launch photon torpedo burst");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "Q", "initiate self-destruct");
    if (sStat.serverFlags & SERVER_F_REFIT)
        cprintf(tlin++,col,ALIGN_NONE,sfmt, "r", "refit ship to new type");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "R", "enter repair mode");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "S", "more user statistics");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "t", "engage tractor beams");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "T", "team list");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "u", "un-engage tractor beams");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "U", "user statistics");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "W", "set war or peace");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "-", "lower shields");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "+", "raise shields");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "/", "player list");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "?", "planet list");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "^L", "refresh the screen");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "[ENTER]", "get last info");
    cprintf(tlin++,col,ALIGN_NONE,sfmt, "[TAB]", "get next last info");

    mcuPutPrompt( MTXT_DONE, MSG_LIN2 );
    cdrefresh();
    while ( ! iogtimed( &ch, 1.0 ) && clbStillAlive( Context.snum )
            && !serverDead )
        ;

    return;

}

/*  doinfo - do an info command */
static void doinfo( int snum )
{
    char ch;

    cdclrl( MSG_LIN1, 2 );

    cbuf[0] = 0;
    ch = (char)cdgetx( "Information on: ", MSG_LIN1, 1, TERMS, cbuf,
                       MAX_MSGLINE_LENGTH, true);
    if ( ch == TERM_ABORT )
    {
        cdclrl( MSG_LIN1, 1 );
        return;
    }

    info( cbuf, ch, true );

    return;

}

/*  doorbit - orbit the ship and print a message */
static void doorbit( int snum )
{
    int pnum;

    if ( ( cbShips[snum].warp == ORBIT_CW ) || ( cbShips[snum].warp == ORBIT_CCW ) )
        infoPlanet( "But we are already orbiting ",
                    cbShips[snum].lockDetail, snum, true );
    else if ( ! clbFindOrbit( snum, &pnum ) )
    {
        sprintf( cbuf, "We are not close enough to orbit, %s.",
                 cbShips[snum].alias );
        uiPutMsg( cbuf, MSG_LIN1 );
        cdclrl( MSG_LIN2, 1 );
    }
    else if ( cbShips[snum].warp > MAX_ORBIT_WARP )
    {
        sprintf( cbuf, "We are going too fast to orbit, %s.",
                 cbShips[snum].alias );
        uiPutMsg( cbuf, MSG_LIN1 );
        sprintf( cbuf, "Maximum orbital insertion velocity is warp %.1f.",
                 oneplace(MAX_ORBIT_WARP) );
        uiPutMsg( cbuf, MSG_LIN2 );
    }
    else
    {
        sendCommand(CPCMD_ORBIT, 0);
        infoPlanet( "Coming into orbit around ", pnum, snum, true );
    }

    return;

}

/*  doselfdest - execute a self-destruct command */
static void doselfdest(int snum)
{
    static const char *pmt="Press [TAB] to initiate self-destruct sequence: ";

    cdclrl( MSG_LIN1, 2 );

    if ( SCLOAKED(snum) )
    {
        uiPutMsg( "The cloaking device is using all available power.",
                   MSG_LIN1 );
        return;
    }

    cbuf[0] = 0;
    if ( cdgetx( pmt, MSG_LIN1, 1, TERMS, cbuf, MAX_MSGLINE_LENGTH,
                 true) != TERM_EXTRA )
    {
        /* Chickened out. */
        cdclrl( MSG_LIN1, 1 );
        return;
    }

    cdclrl( MSG_LIN1, 1 );

    clientLastServerAckCode = 0;
    sendCommand(CPCMD_DESTRUCT, 1); /* blow yourself up */

    while ( true )
    {
        if ( ! clbStillAlive( Context.snum ) || serverDead
             || (clientStatLastFlags & SPCLNTSTAT_FLAG_KILLED) )
            return;			/* Died in the process. */

        if ( iochav() )
	{
            /* Got a new character. */
            cdclrl( MSG_LIN1, 2 );
            if ( iogchar() == TERM_ABORT )
	    {
                sendCommand(CPCMD_DESTRUCT, 0);
                uiPutMsg( "Self destruct has been cancelled.", MSG_LIN1 );
                return;
	    }
            else
	    {
                uiPutMsg( "Press [ESC] to abort self destruct.", MSG_LIN1 );
                cdbeep();
                cdrefresh();
	    }
	}

        if (clientLastServerAckCode)
        {
            sendCommand(CPCMD_DESTRUCT, 0); /* make sure */
            break;
        }

        astservice();
        utSleep( ITER_SECONDS );
    } /* end while */

    return;

}

/*  doshields - raise or lower shields */
static void doshields( int snum, int up )
{

    if (!sendCommand(CPCMD_SETSHIELDS, (uint16_t)up))
        return;

    if ( up )
    {
        SFCLR(snum, SHIP_F_REPAIR);
        uiPutMsg( "Shields raised.", MSG_LIN1 );
    }
    else
        uiPutMsg( "Shields lowered.", MSG_LIN1 );

    cdclrl( MSG_LIN2, 1 );

    return;

}

/*  dotow - attempt to tow another ship */
static void dotow( int snum )
{
    char ch;
    int other;

    cdclrl( MSG_LIN1, 2 );

    if ( STOWEDBY(snum) )
    {
        sprintf(cbuf, "But we are being towed by %s!",
                utShipStr(cbShips[snum].towedby).c_str());
        uiPutMsg( cbuf, MSG_LIN2 );
        return;
    }
    if ( STOWING(snum) )
    {
        /* another tow command will cancel our current tow */
        sendCommand(CPCMD_TOW, cbShips[snum].towing);
        return;
    }

    cbuf[0] = 0;
    ch = (char)cdgetx( "Tow which ship? ", MSG_LIN1, 1, TERMS, cbuf,
                       MAX_MSGLINE_LENGTH, true);
    cdclrl( MSG_LIN1, 1 );
    if ( ch == TERM_ABORT )
        return;

    if (utSafeCToI( &other, cbuf, 0 ))
        sendCommand(CPCMD_TOW, (uint16_t)other);

    return;

}

/* run a full screen command (lists and such) from the ship display,
 * then redraw */
#define FULLSCREEN(x) do {                      \
        Context.redraw = true;                  \
        dspOk = false;                          \
        x;                                      \
        dspOk = true;                           \
        redisplay();                            \
    } while (0)

/*  command - execute a user's command */
static void command( int ch )
{
    int i;
    real x;
    int snum = Context.snum;

    if (mcuKPAngle(ch, &x))	/* hit a keypad/arrow key */
    {				/* change course */
        cdclrl( MSG_LIN1, 2 );
        sendSetCourse(cInfo.sock, LOCK_NONE, 0, x);
        return;
    }

    switch ( ch )
    {
    case '0':           /* - '9', '=' :set warp factor */
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
    case '=':
        if ( ch == '=' )
            x = 10.0;
        else
	{
            i = ch - '0';
            x = (real) (i);
	}
        dowarp( snum, x );
        break;
    case 'a':				/* autopilot */
        if ( UAUTOPILOT(cbShips[snum].unum) )
            doautopilot( snum );
        else
	{
            cdbeep();
            uiPutMsg( "Type h for help.", MSG_LIN2 );
	}
        break;
    case 'A':				/* change allocation */
        doalloc( snum );
        break;
    case 'b':				/* beam armies */
        dobeam( snum );
        break;
    case 'B':				/* bombard a planet */
        dobomb( snum );
        break;
    case 'C':				/* cloak control */
        docloak( snum );
        break;
    case 'd':				/* detonate enemy torps */
    case '*':
        dodet( snum );
        break;
    case 'D':				/* detonate own torps */
        cdclrl( MSG_LIN2, 1 );
        sendCommand(CPCMD_DETSELF, 0);
        uiPutMsg( "Detonating...", MSG_LIN1 );
        break;
    case 'E':				/* emergency distress call */
        dodistress( snum );
        break;
    case 'f':				/* phasers */
        dophase( snum, false );
        break;
    case 'F':				/* phasers, same direction */
        dophase( snum, true );
        break;
    case 'h':
        FULLSCREEN(dohelp());
        break;
    case 'H':
        FULLSCREEN(mcuHistList());
        break;
    case 'i':				/* information */
        doinfo( snum );
        break;
    case 'k':				/* set course */
        docourse( snum );
        break;
    case 'K':				/* coup */
        docoup( snum );
        break;
    case 'L':				/* review old messages */
        doReviewMsgs( snum );
        break;
    case 'm':				/* send a message */
        cucSendMsg( MSG_FROM_SHIP, snum, UserConf.Terse, true );
        break;
    case 'M':				/* strategic/tactical map */
        if (SMAP(snum))
            SFCLR(snum, SHIP_F_MAP);
        else
            SFSET(snum, SHIP_F_MAP);
        redisplay();
        break;
    case 'N':				/* change pseudonym */
        cucPseudo( Context.unum, snum );
        break;
    case 'O':
        FULLSCREEN(UserOptsMenu(Context.unum));
        break;
    case 'o':				/* orbit nearby planet */
        doorbit( snum );
        break;
    case 'P':				/* photon torpedo burst */
        dotorp( snum, 3 );
        break;
    case 'p':				/* photon torpedoes */
        dotorp( snum, 1 );
        break;
    case 'Q':				/* self destruct */
        doselfdest( snum );
        break;
    case 'r':				/* refit */
        if (sStat.serverFlags & SERVER_F_REFIT)
            dorefit( snum );
        else
            cdbeep();
        break;
    case 'R':				/* repair mode */
        if ( ! SCLOAKED(snum) )
	{
            cdclrl( MSG_LIN1, 2 );
            sendCommand(CPCMD_REPAIR, 0);
	}
        else
	{
            cdclrl( MSG_LIN2, 1 );
            uiPutMsg(
                "You cannot repair while the cloaking device is engaged.",
                MSG_LIN1 );
	}
        break;
    case 't':				/* tow */
        dotow( snum );
        break;
    case 'S':				/* more user stats */
        FULLSCREEN(mcuUserStats( false, snum ));
        break;
    case 'T':				/* team list */
        FULLSCREEN(doTeamList( cbShips[snum].team ));
        break;
    case 'u':				/* un-tractor */
        sendCommand(CPCMD_UNTOW, 0);
        break;
    case 'U':				/* user stats */
        FULLSCREEN(mcuUserList( false, snum ));
        break;
    case 'W':				/* war and peace */
        cucDoWar( snum );
        break;
    case '-':				/* shields down */
        doshields( snum, false );
        redisplay();
        break;
    case '+':				/* shields up */
        doshields( snum, true );
        redisplay();
        break;
    case '/':				/* player list */
        FULLSCREEN(mcuPlayList( false, false, snum ));
        break;
    case '?':				/* planet list */
        FULLSCREEN(doPlanetList( snum ));
        break;
    case TERM_REDRAW:			/* clear and redisplay */
        cdredo();
        Context.redraw = true;
        redisplay();
        break;

    case TERM_NORMAL:		/* Have [ENTER] act like 'I[ENTER]'  */
    case KEY_ENTER:
    case '\n':
        ibufPut("i\r");		/* (get last info) */
        break;

    case ' ':
        UserConf.DoLocalLRScan = !UserConf.DoLocalLRScan;
        break;

    case TERM_EXTRA:		/* Have [TAB] act like 'i\t' */
        ibufPut("i\t");		/* (get next last info) */
        break;

    case TERM_RELOAD:		/* have server resend current universe */
        sendCommand(CPCMD_RELOAD, 0);
        utLog("client: sent CPCMD_RELOAD");
        break;

    case -1:			/* really nothing, move along */
    case TERM_ABORT:
        break;

        /* nothing. */
    default:
        cdbeep();
        uiPutMsg( "Type h for help.", MSG_LIN2 );
    }

    return;

}

/*  dead - announce to a user that s/he is dead */
static void dead( int snum )
{
    int i;
    uint16_t detail;
    killedBy_t kb;
    int ch;
    static const char *ywkb="You were killed by ";
    char buf[BUFFER_SIZE_256];
    Ship_t eShip;

    /* If something is wrong, don't do anything. */
    if ( snum < 0 || snum >= cbLimits.maxShips() )
    {
        utLog("dead: snum < 0 || snum >= maxShips (%d)", snum);
        return;
    }

    /* (Quickly) clear the screen. */
    cdclear();
    cdredo();
    cdrefresh();

    kb = cbShips[snum].killedBy;
    detail = cbShips[snum].killedByDetail;

    if (kb == KB_SHIP && detail < cbLimits.maxShips())
        eShip = cbShips[detail];        /* get copy of killers ship */
    else
        memset((void *)&eShip, 0, sizeof(Ship_t));

    /* Figure out why we died. */
    switch ( kb )
    {
    case KB_SELF:
        cprintf(8,0,ALIGN_CENTER,"#%d#%s", RedLevelColor,
		"You scuttled yourself.");
        break;
    case KB_NEGENB:
        cprintf(8,0,ALIGN_CENTER,"#%d#%s", RedLevelColor,
		"You were destroyed by the negative energy barrier.");
        break;
    case KB_CONQUER:
        cprintf(8,0,ALIGN_CENTER,"#%d#%s", RedLevelColor,
                "Y O U   C O N Q U E R E D   T H E   U N I V E R S E ! ! !");
        break;
    case KB_NEWGAME:
        cprintf(8,0,ALIGN_CENTER,"#%d#%s", RedLevelColor,
                "N E W   G A M E !");
        break;
    case KB_EVICT:
        cprintf(8,0,ALIGN_CENTER,"#%d#%s", RedLevelColor,
                "Closed for repairs.");
        break;
    case KB_SHIT:
        cprintf(8,0,ALIGN_CENTER,"#%d#%s", RedLevelColor,
                "You are no longer allowed to play.");
        break;
    case KB_GOD:
        cprintf(8,0,ALIGN_CENTER,"#%d#%s", RedLevelColor,
                "You were killed by an act of GOD.");
        break;
    case KB_DOOMSDAY:
        cprintf(8,0,ALIGN_CENTER,"#%d#%s", RedLevelColor,
                "You were eaten by the doomsday machine.");
        break;
    case KB_GOTDOOMSDAY:
        cprintf(8,0,ALIGN_CENTER,"#%d#%s", RedLevelColor,
                "You destroyed the doomsday machine!");
        break;
    case KB_LIGHTNING:
        cprintf(8,0,ALIGN_CENTER,"#%d#%s", RedLevelColor,
                "You were destroyed by a lightning bolt.");
        break;

    case KB_SHIP:
        if ( detail < cbLimits.maxShips() )
	{
            std::string shipBuf = utShipStr((int)detail);

            cprintf( 8,0,ALIGN_CENTER,
                     "#%d#You were kill number #%d#%.1f #%d#for #%d#%s #%d#(#%d#%s#%d#)%s",
                     InfoColor, CQC_A_BOLD, eShip.kills,
                     InfoColor, CQC_A_BOLD, eShip.alias,
                     InfoColor, CQC_A_BOLD, shipBuf.c_str(),
                     InfoColor,
                     (eShip.status != SS_LIVE) ? ", who also died." : "." );
	}
        break;

    case KB_PLANET:
        if ( detail < cbLimits.maxPlanets() )
	{
            cprintf(8,0,ALIGN_CENTER,"#%d#%s#%d#%s%s#%d#%s",
                    InfoColor, ywkb, CQC_A_BOLD, cbPlanets[detail].name, "'s ",
                    InfoColor,
                    (cbPlanets[detail].type == PlanetType::Sun) ?
                    "solar radiation." : "planetary defenses.");
	}
        break;

    default:
        /* We were unable to determine the cause of death. */
        utLog("dead: %s was killed by %d(%d).", utShipStr(snum).c_str(),
              (int)kb, (int)detail);

        cprintf(8,0,ALIGN_CENTER,"#%d#%s%s",
                RedLevelColor, ywkb,
                "nothing in particular.  (How strange...)");
    }

    if ( kb == KB_NEWGAME )
    {
        cprintf( 10,0,ALIGN_CENTER,
                 "#%d#Universe conquered by #%d#%s #%d#for the #%d#%s #%d#team.",
		 InfoColor, CQC_A_BOLD, cbConqInfo->conqueror,
		 InfoColor, CQC_A_BOLD, cbConqInfo->conqteam, LabelColor );
    }
    else if ( kb == KB_SELF )
    {
        i = cbShips[snum].armies;
        if ( i > 0 )
	{
            if ( i == 1 )
		cprintf(10,0,ALIGN_CENTER,
                        "#%d#The #%d#%s #%d#you were carrying %s not amused.",
			LabelColor, CQC_A_BOLD, "army", LabelColor, "was");
            else
		cprintf(10,0,ALIGN_CENTER,
                        "#%d#The #%d#%d %s #%d#you were carrying %s not amused.",
			LabelColor, CQC_A_BOLD, i, "armies", LabelColor, "were");
	}
    }
    else if ( kb == KB_SHIP && detail < cbLimits.maxShips())
    {
        if ( eShip.status == SS_LIVE )
	{
            cprintf( 10,0,ALIGN_CENTER,
                     "#%d#He had #%d#%d%% #%d#shields and #%d#%d%% #%d#damage.",
                     InfoColor, CQC_A_BOLD, (int)round(eShip.shields),
                     InfoColor, CQC_A_BOLD, (int)round(eShip.damage),
                     InfoColor );
	}
    }
    cprintf(12,0,ALIGN_CENTER,
            "#%d#You got #%d#%.1f #%d#this time.",
            InfoColor, CQC_A_BOLD, oneplace(cbShips[snum].kills), InfoColor );
    cdmove( 1, 1 );
    cdrefresh();

    /* check the clientstat's flags to see if last words are needed */
    if (clientStatLastFlags & SPCLNTSTAT_FLAG_CONQUER)
    {
        idleWait(4.0);
        do
	{
            cdclear();
            cdredo();
            buf[0] = 0;

            ch = cdgetx( "Any last words? ",
                         14, 1, TERMS, buf, MAX_LASTWORDS, true );

            cdclear();
            cdredo();

            if ( buf[0] != 0 )
	    {
                cprintf( 13,0,ALIGN_CENTER, "#%d#%s",
                         InfoColor, "Your last words are entered as:");
                cprintf( 14,0,ALIGN_CENTER, "#%d#%c%s%c",
                         YellowLevelColor, '"', buf, '"' );
	    }
            else
                cprintf( 14,0,ALIGN_CENTER,"#%d#%s", InfoColor,
                         "You have chosen to NOT leave any last words:" );

            ch = mcuGetCX( "Press [TAB] to confirm:", 16, 0,
                           TERMS, cbuf, 10 );
	}
        while ( ch != TERM_EXTRA ); /* until . while */

        /* now we just send a message */
        sendMessage(MSG_TO_GOD, 0, buf);
    }

    /* set the ship reserved (locally).  The server has already done this
       but we may not have processed any packets since we died.  This keeps
       menu() from booting us out of the game. */
    cbShips[Context.snum].status = SS_RESERVED;

    ioeat();
    mcuPutPrompt( MTXT_DONE, MSG_LIN2 );
    cdrefresh();
    while ( ! iogtimed( &ch, 1.0 ) && !serverDead )
        ;

    cdmove( 1, 1 );

    return;

}

/*  play - play the game */
static bool play()
{
    int ch;

    /* Can't carry on without a vessel. */
    if ( !newship() )
        return false;

    clientStatLastFlags = 0;
    lastblast = cbShips[Context.snum].lastblast;
    lastphase = cbShips[Context.snum].lastphase;

    cbShips[Context.snum].sdfuse = 0;	/* zero self destruct fuse */
    utGrand( &Context.msgrand );	/* initialize message timer */
    Context.redraw = true;		/* want redraw first time */
    Context.msgok = true;		/* ok to get messages */
    cdclear();			/* clear the display */
    cdredo();			/*  (quickly) */

    /* need to tell the server to resend all the data it already
       sent in menu, since our ship might have changed. */
    sendCommand(CPCMD_RELOAD, 0);

    readPackets();
    display( Context.snum );	/* update the screen manually */
    dspOk = true;		/* ok to display */

    /* While we're alive, field commands and process them.  Inbound
       packets are handled by astservice() while waiting for keys. */
    /* when we become 'killed', then dead processing can begin. */
    while ( !(clientStatLastFlags & SPCLNTSTAT_FLAG_KILLED) && !serverDead )
    {
        if ( ! iogtimed( &ch, 0.5 ) )
            continue;

        /* only process commands if we are live (the server will ignore
           them anyway) */
        if (cbShips[Context.snum].status == SS_LIVE)
        {
            if (ch >= KEY_F(1) && ch <= KEY_F(MAX_MACROS)
                && ibufExpandMacro((ch - KEY_F(0)) - 1))
            {
                while (ibufCount())
                {
                    ch = ibufGetc();
                    command( ch );
                }
            }
            else
            {
                do
                {
                    command( ch );
                } while (ibufCount() && (ch = ibufGetc()));
            }
        }
    } /* while stillalive */

    if (serverDead)
    {
        dspOk = false;
        return false;
    }

    /* watch yourself die for a little bit */
    idleWait( 3.0 );

    dspOk = false;

    dead( Context.snum );

    return true;

}

/*  conqds - display background for Conquest */
static void conqds()
{
    int i, col, lin;
    extern char *ConquestVersion;
    extern char *ConquestDate;
    static bool FirstTime = true;
    static char sfmt[MAX_MSGLINE_LENGTH * 2];

    if (FirstTime)
    {
        FirstTime = false;
        sprintf(sfmt,
                "#%d#(#%d#%%c#%d#) - %%s",
                LabelColor,
                InfoColor,
                LabelColor);
    }

    /* Display the logo. */
    lin = mcuConqLogo();

    if ( sStat.serverFlags & SERVER_F_CLOSED )
        cprintf(lin,0,ALIGN_CENTER,"#%d#%s",RedLevelColor,"The game is closed.");
    else
        cprintf( lin,0,ALIGN_CENTER,"#%d#%s (%s)",YellowLevelColor,
                 ConquestVersion, ConquestDate);

    lin++;
    cprintf(lin,0,ALIGN_CENTER,"#%d#%s", MagentaColor, sHello.servername);
    lin++;
    cprintf(lin,0,ALIGN_CENTER,"#%d#%s", NoColor, sHello.motd);
    lin += 2;
    cprintf(lin,0,ALIGN_CENTER,"#%d#%s",NoColor, "Options:");

    col = 13;
    lin+=2;
    i = lin;
    cprintf(lin++,col,ALIGN_NONE,sfmt, 'e', "enter the game");
    cprintf(lin++,col,ALIGN_NONE,sfmt, 'S', "more user statistics");
    cprintf(lin++,col,ALIGN_NONE,sfmt, 'T', "team statistics");
    cprintf(lin++,col,ALIGN_NONE,sfmt, 'U', "user statistics");
    cprintf(lin++,col,ALIGN_NONE,sfmt, 'L', "review messages");
    cprintf(lin++,col,ALIGN_NONE,sfmt, 'W', "set war or peace");
    cprintf(lin++,col,ALIGN_NONE,sfmt, 'H', "user history");

    col = 48;
    lin = i;
    cprintf(lin++,col,ALIGN_NONE,sfmt, 'N', "change your name");
    cprintf(lin++,col,ALIGN_NONE,sfmt, 'O', "options menu");
    cprintf(lin++,col,ALIGN_NONE,sfmt, 'r', "resign your commission");
    if (sStat.serverFlags & SERVER_F_SWITCHTEAM)
        cprintf(lin++,col,ALIGN_NONE,sfmt, 's', "switch teams");
    cprintf(lin++,col,ALIGN_NONE,sfmt, '/', "player list");
    cprintf(lin++,col,ALIGN_NONE,sfmt, '?', "planet list");
    cprintf(lin++,col,ALIGN_NONE,sfmt, 'q', "exit the program");

    return;

}

/* switch to the next enabled team */
static void switchTeam(void)
{
    std::vector<int> enabledTeams = clbGetEnabledTeams();
    int snum = Context.snum;

    /* we'll update local data here anyway, even though it will be
       overwritten on the next ship update.  Improves perceived
       response time. */
    if (!enabledTeams.size())
    {
        cbShips[snum].team =
            static_cast<Team::Team>(mod( cbShips[snum].team + 1,
                                         NUM_PLAYERTEAMS ));
    }
    else
    {
        int idx = -1;
        for (int i=0; i<enabledTeams.size(); i++)
        {
            if (cbShips[snum].team == enabledTeams[i])
            {
                idx = i;
                break;
            }
        }

        if (idx == -1)
        {
            cdbeep();
            return;
        }

        int team = (idx + 1) % enabledTeams.size();
        cbShips[snum].team = static_cast<Team::Team>(enabledTeams[team]);
    }

    cbShips[snum].shiptype = cbTeams[cbShips[snum].team].shiptype;
    cbUsers[Context.unum].team = cbShips[snum].team;
    cbShips[snum].war[cbShips[snum].team] = false;
    cbUsers[Context.unum].war[cbUsers[Context.unum].team] = false;
    sendCommand(CPCMD_SWITCHTEAM, (uint16_t)cbShips[snum].team);

    return;
}

/*  menu - main user menu */
static void menu(void)
{
    int i, lin, col;
    int ch;
    bool redraw;
    std::string ubuf;
    static const char *if1="Suddenly  a  sinister,  wraithlike  figure appears before you";
    static const char *if2="seeming to float in the air.  In a low,  sorrowful  voice  he";
    static const char *if3="says, \"Alas, the very nature of the universe has changed, and";
    static const char *if4="your ship cannot be found.  All must now pass away.\"  Raising";
    static const char *if5="his  oaken  staff  in  farewell,  he fades into the spreading";
    static const char *if6="darkness.  In his place appears a  tastefully  lettered  sign";
    static const char *if7="reading:";
    static const char *if8="INITIALIZATION FAILURE";
    static const char *if9="The darkness becomes all encompassing, and your vision fails.";

    /* Initialize statistics. */
    initstats( &cbShips[Context.snum].ctime, &cbShips[Context.snum].etime );

    /* now look for our ship packet before we get started.  It should be a
       full SP_SHIP packet for this first time */
    uint32_t startTime = clbGetMillis();
    bool gotShip = false;
    while ((clbGetMillis() - startTime) < 30000)
    {
        int rv;
        char buf[PKT_MAXSIZE];

        if ((rv = pktRead(buf, PKT_MAXSIZE, 1)) < 0)
        {
            utLog("menu: pktRead failed: %d", rv);
            return;
        }

        if (rv == 0)
            continue;

        PKT_PROCSP(buf);
        if (rv == SP_SHIP)
        {
            gotShip = true;
            break;
        }
    }

    if (!gotShip)
    {
        utLog("menu: never received the initial SP_SHIP packet");
        return;
    }

    if (cInfo.tryUDP && !cInfo.doUDP)
    {
        utLog("menu: requesting a UDP connection from the server");
        sendCommand(CPCMD_UDP, 1);
    }

    doLeave = false;
    redraw = true;
    clientLastServerAckCode = 0;

    do
    {
        dspOk = false;

        /* process any packets, look for trouble */
        readPackets();

        if (serverDead)
        {
            cdclear();
            cdredo();
            cprintf(MSG_LIN2/2, 0, ALIGN_CENTER, "#%d#%s", RedLevelColor,
                    "The server closed the connection.");
            mcuPutPrompt( MTXT_DONE, MSG_LIN2 );
            cdrefresh();
            ioeat();
            iogchar();
            return;
        }

        if ( clientLastServerAckCode == PERR_LOSE )	/* again, Jorge? */
	{
            /* We reincarnated or else something bad happened. */
            lin = 7;
            col = 11;
            cdclear();
            cdredo();
            cprintf( lin++,col,ALIGN_NONE,"#%d#%s", RedLevelColor, if1);
            cprintf( lin++,col,ALIGN_NONE,"#%d#%s", RedLevelColor, if2);
            cprintf( lin++,col,ALIGN_NONE,"#%d#%s", RedLevelColor, if3);
            cprintf( lin++,col,ALIGN_NONE,"#%d#%s", RedLevelColor, if4);
            cprintf( lin++,col,ALIGN_NONE,"#%d#%s", RedLevelColor, if5);
            cprintf( lin++,col,ALIGN_NONE,"#%d#%s", RedLevelColor, if6);
            cprintf( lin,col,ALIGN_NONE,"#%d#%s", RedLevelColor, if7);
            lin+=2;
            cprintf( lin,col,ALIGN_NONE,"#%d#%s", RedLevelColor | CQC_A_BLINK, if8);
            lin+=2;
            cprintf( lin,col,ALIGN_NONE,"#%d#%s", RedLevelColor, if9);
            ioeat();
            cdmove( 1, 1 );
            cdrefresh();
            idleWait( 3.0 );
            return;
	}

        if ( clientLastServerAckCode == PERR_IDLETIMEOUT )
        {
            cdclear();
            cdredo();
            cprintf(MSG_LIN2/2, 0, ALIGN_CENTER, "#%d#%s", RedLevelColor,
                    "Menu idle timeout after 10 minutes of inactivity");
            mcuPutPrompt( MTXT_DONE, MSG_LIN2 );
            cdrefresh();
            ioeat();
            iogchar();
            return;
        }

        if ( redraw )
	{
            cdclear();
            conqds();
            redraw = false;
	}
        else
            cdclrl( MSG_LIN1, 2 );

        clbUserline( -1, -1, ubuf, false, true );
        uiPutColor(LabelColor);
        cdputs( ubuf.c_str(), MSG_LIN1, 1 );
        clbUserline( Context.unum, -1, ubuf, false, true );
        uiPutColor(CQC_A_BOLD);
        cdputs( ubuf.c_str(), MSG_LIN2, 1 );
        uiPutColor(0);
        cdmove( 1, 1 );
        cdrefresh();

        /* wait up to a second for something to happen */
        if ( ! iogtimed( &ch, 1.0 ) )
            continue;

        clientLastServerAckCode = 0;

        switch ( ch )
        {
        case 'e':
            if (sendCommand(CPCMD_ENTER, 0))
            {
                clientStatReceived = false;
                clientLastServerAckCode = 0;
                play();
            }
            redraw = true;
            break;
        case 'H':
            mcuHistList();
            redraw = true;
            break;
        case 'L':
            doReviewMsgs( Context.snum );
            break;
        case 'N':
            cucPseudo( Context.unum, Context.snum );
            break;
        case 'O':
            UserOptsMenu(Context.unum);
            redraw = true;
            break;
        case 'r':
            for ( i = 0; i < cbLimits.maxShips(); i++ )
                if ( cbShips[i].status == SS_LIVE ||
                     cbShips[i].status == SS_ENTERING )
                    if ( cbShips[i].unum == Context.unum )
                        break;
            if ( i < cbLimits.maxShips() )
                cdbeep();
            else
            {
                cdclrl( MSG_LIN1, 2 );
                cdrefresh();
                if ( mcuConfirm() )
                {
                    sendCommand(CPCMD_RESIGN, 0);
                    doLeave = true;
                }
            }
            break;
        case 's':
            if (!(sStat.serverFlags & SERVER_F_SWITCHTEAM))
                cdbeep();
            else
            {
                switchTeam();
                redraw = true;
            }
            break;
        case 'S':
            mcuUserStats( false, -1 );
            redraw = true;
            break;
        case 'T':
            doTeamList( cbShips[Context.snum].team );
            redraw = true;
            break;
        case 'U':
            mcuUserList( false, -1 );
            redraw = true;
            break;
        case 'W':
            cucDoWar( Context.snum );
            redraw = true;
            break;
        case 'q':
        case 'Q':
            doLeave = true;
            break;
        case '/':
            mcuPlayList( false, false, -1 );
            redraw = true;
            break;
        case '?':
            doPlanetList( -1 );
            redraw = true;
            break;
        case TERM_REDRAW:	/* ^L */
            cdredo();
            redraw = true;
            break;
        case ' ':
        case TERM_NORMAL:
            /* Do nothing. */
            break;
        default:
            cdbeep();
            break;
        }
    }
    while ( !doLeave && !serverDead );

    return;

}

static void handleSignal(int sig)
{
#if !defined(MINGW)
    switch(sig)
    {
    case SIGQUIT:
    case SIGTERM:
    case SIGHUP:
    case SIGPIPE:
        utLog("handleSignal: Exiting on signal %d", sig);
        cdclear();
        cdrefresh();
        cdend();
        conqend();		/* sends a disconnect packet */

        exit(0);
        break;

    default:
        break;
    }
#endif

    return;
}

static void catchSignals(void)
{
#if !defined(MINGW)
    signal(SIGHUP, (void (*)(int))handleSignal);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTERM, (void (*)(int))handleSignal);
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, (void (*)(int))handleSignal);
    signal(SIGPIPE, (void (*)(int))handleSignal);
#endif

    return;
}

/*  conqend - machine dependent clean-up */
static void conqend(void)
{
    if (!pktNoNetwork() && !serverDead)
        sendCommand(CPCMD_DISCONNECT, 0); /* tell the server */

    return;

}

/*  conquest-tty - main program */
int main(int argc, char *argv[])
{
    int i;
    char *ch;
    bool wantMetaList = false;  /* wants to see a list from metaserver */
    bool serveropt = false;     /* specified a server with '-s' */

    /* tell the packet routines that we are a client */
    pktSetClientMode(true);
    pktSetClientProtocolVersion(PROTOCOL_VERSION);

    Context.recmode = RECMODE_OFF;
    Context.updsec = 5;		/* dflt - 5/sec */
    Context.msgrand = time(0);

    utStrncpy(cInfo.metaServer, META_DFLT_SERVER, MAXHOSTNAME);
    cInfo.sock = -1;
    cInfo.usock = -1;
    cInfo.doUDP = false;
    cInfo.tryUDP = true;
    cInfo.remoteport = CN_DFLT_PORT;
    cInfo.state = CLIENT_STATE_INIT;

    utSetLogConfig(false, false);	/* use CQ_USERHOMEDIR for logfile, no stderr */
    utSetLogProgramName(argv[0]);

    cInfo.remotehost = strdup("localhost"); /* default to your own server */

    /* check options */
    while ((i = getopt(argc, argv, "mM:s:uv")) != EOF)
        switch (i)
        {
        case 'm':
            wantMetaList = true;
            break;
        case 'M':
            utStrncpy(cInfo.metaServer, optarg, MAXHOSTNAME);
            break;
        case 's':                 /* [host[:port]] */
            free(cInfo.remotehost);
            cInfo.remotehost = strdup(optarg);
            if (!cInfo.remotehost)
            {
                printf("strdup failed\n");
                exit(1);
            }
            if ((ch = strchr(cInfo.remotehost, ':')) != NULL)
            {
                *ch = 0;
                ch++;
                if ((cInfo.remoteport = atoi(ch)) == 0)
                    cInfo.remoteport = CN_DFLT_PORT;

                /* if no host was specified (only the :port), then set to
                   localhost */
                if (strlen(cInfo.remotehost) == 0)
                {
                    free(cInfo.remotehost);
                    cInfo.remotehost = strdup("localhost");
                }
            }
            else
                cInfo.remoteport = CN_DFLT_PORT;

            serveropt = true;

            break;

        case 'u':
            cInfo.tryUDP = false;
            break;

        case 'v':
            cqDebug++;
            break;

        default:
            printUsage();
            exit(1);
        }

    rndini();		/* initialize random numbers */

    if (!pktInit())
    {
        fprintf(stderr, "pktInit failed, exiting\n");
        utLog("pktInit failed, exiting");
    }
    pktSetSocketFds(cInfo.sock, cInfo.usock);

    if (GetConf(0) == -1)
        exit(1);

    Context.updsec = UserConf.UpdatesPerSecond;

    metaServerVec_t metaServerList;
    if (serveropt && wantMetaList)
    {
        printf("-m ignored, since -s was specified\n");
        wantMetaList = false;
    }
    else if (wantMetaList)
    {                           /* get the metalist and display */
        printf("Querying metaserver at %s\n",
               cInfo.metaServer);
        int nums = metaGetServerList(cInfo.metaServer, metaServerList);

        if (nums < 0)
        {
            printf("metaGetServerList() failed\n");
            return 1;
        }

        if (nums == 0)
        {
            printf("metaGetServerList() reported 0 servers online\n");
            return 1;
        }

        printf("Found %d server(s)\n", nums);
    }

    conqinit();			/* machine dependent initialization */
    ibufInit();

    cdinit();			/* set up display environment */

    Context.maxlin = cdlins();
    Context.maxcol = cdcols();
    Context.snum = -1;
    Context.unum = -1;
    Context.lasttang = Context.lasttdist = 0;
    Context.lastInfoTarget.clear();
    Context.lastInfoStr.clear();

    if (Context.maxlin < MSG_MSG || Context.maxcol < 80)
    {
        cdend();
        fprintf(stderr, "conquest-tty: the terminal must be at least "
                "80x%d (it is %dx%d).\n", MSG_MSG, Context.maxcol,
                Context.maxlin);
        exit(1);
    }

    /* the idle function reads packets and updates the display while
       we wait for keys */
    iolbSetIdleFunc(astservice, ITER_SECONDS);

    catchSignals();       /* enable trapping of interesting signals */

    if (wantMetaList)
    {                           /* list the servers */
        i = selectServer(metaServerList);

        if (i < 0)
        {                       /* didn't pick one */
            cdend();
            return 1;
        }

        free(cInfo.remotehost);
        cInfo.remotehost = strdup(metaServerList[i].altaddr.c_str());
        cInfo.remoteport = metaServerList[i].port;
    }

    /* connect to the host */
    if (!connectServer(cInfo.remotehost, cInfo.remoteport))
    {
        cdend();
        return(1);
    }

    /* now we need to negotiate. */
    if (!clientHello(CONQUEST_TTY_NAME))
    {
        cdend();
        utLog("conquest-tty: clientHello() failed");
        printf("conquest-tty: clientHello() failed, check log\n");
        exit(1);
    }

    if ( welcome() )
    {
        cInfo.state = CLIENT_STATE_MENU;
        menu();
    }

    cdend();			/* clean up display environment */
    conqend();			/* machine dependent clean-up */

    exit(0);

}
