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

#include "c_defs.h"
#include "global.h"
#include "conqdef.h"
#include "cb.h"
#include "context.h"
#include "conf.h"
#include "color.h"
#include "ui.h"
#include "cd2lb.h"
#include "cumisc.h"
#include "cprintf.h"

#include "conqnet.h"
#include "packet.h"
#include "userauth.h"
#include "client.h"
#include "clientlb.h"
#include "conqutil.h"
#include "serverflags.h"
#include "iolb.h"

/* ChangePassword() - change a users password - called from UserOptsMenu() */
void ChangePassword(int unum, int isoper)
{
    static const char *header = "Change Password";
    char pw[MAX_USERNAME], pwr[MAX_USERNAME], epw[MAX_USERNAME];
    char salt[3];
    int lin = 0, col = 0;

    if (isoper == false)
        cdclear();
    else
    {
        cdclrl(MSG_LIN1, 2);
    }

    if (isoper == false)
    {
        lin = 1;
        col = ((Context.maxcol / 2) - (strlen(header) / 2));

        cprintf(lin, col, ALIGN_NONE, "#%d#%s", NoColor, header);
    } /* ! isoper */
    /* get and recheck new passwd */
    pw[0] = 0;
    cdclrl( MSG_LIN1, 2  );
    cdputs("Use any printable characters.", MSG_LIN2, 1);

    cdgetx( "New Password: ", MSG_LIN1, 1,
            TERMS, pw, MAX_USERNAME - 1, false );

    if (isoper == false)
    {
        pwr[0] = 0;
        cdclrl( MSG_LIN1, 2  );
        cdputs("Use any printable characters.", MSG_LIN2, 1);
        cdgetx( "Retype Password: ", MSG_LIN1, 1,
                TERMS, pwr, MAX_USERNAME - 1, false );

        if (strcmp(pw, pwr) != 0)
	{			/* pw's don't match, start over */
            cdbeep();
            cdclrl( MSG_LIN2, 1  );
            uiPutColor(RedLevelColor);
            cdputs("Passwords don't match.", MSG_LIN2, 1);
            uiPutColor(NoColor);
            cdrefresh();
            sleep(2);
            return;
	}
    } /* ! isoper */
    /* ok, we have a new password -
       make it so */

    if (isoper)
    {				/* do it locally */
        salt[0] = (cbUsers[unum].username[0] != 0) ? cbUsers[unum].username[0] : 'J';
        salt[1] = (cbUsers[unum].username[1] != 0) ? cbUsers[unum].username[1] : 'T';
        salt[2] = 0;

        utStrncpy(epw, (char *)crypt(pw, salt), MAX_USERNAME);
        utStrncpy(cbUsers[unum].pw, epw, MAX_USERNAME);
    }
    else				/* send a packet */
        sendAuth(cInfo.sock, CPAUTH_CHGPWD, "", pw);

    cdclrl(MSG_LIN1, 2);

    return;
}

static void _printStatus(int lin)
{
    static char buf1[BUFFER_SIZE_1024];
    static char buf2[BUFFER_SIZE_1024];
    static char buf3[BUFFER_SIZE_1024];
    static char buf4[BUFFER_SIZE_1024];
    static char buf5[BUFFER_SIZE_1024];
    static char buf6[BUFFER_SIZE_1024];
    static bool inited = false;
    time_t servtm;

    if (!inited)
    {
        inited = true;
        sprintf(buf1, "#%d#Server: #%d#  %%s", MagentaColor, NoColor);
        sprintf(buf2, "#%d#Version: #%d# %%s", MagentaColor, NoColor);
        sprintf(buf3, "#%d#Time: #%d#    %%s", MagentaColor, NoColor);
        sprintf(buf4,
                "#%d#Status: #%d#  Users #%d#%%d, #%d#Ships #%d#%%d/%%d #%d#"
                "(#%d#%%d #%d#active, #%d#%%d #%d#vacant, "
                "#%d#%%d #%d#robot)",
                MagentaColor, NoColor, CyanColor, NoColor, CyanColor, NoColor,
                CyanColor, NoColor, CyanColor, NoColor,
                CyanColor, NoColor);
        sprintf(buf5, "#%d#Flags: #%d#   %%s", MagentaColor, NoColor);
        sprintf(buf6, "#%d#MOTD: #%d#    %%s", MagentaColor, NoColor);
    }

    lin++;

    cprintf(lin++, 0, ALIGN_NONE, buf1, sHello.servername);

    cprintf(lin++, 0, ALIGN_NONE, buf2, sHello.serverver);

    servtm = sStat.servertime; /* fix alignment */
    cprintf(lin++, 0, ALIGN_NONE, buf3, ctime(&servtm));

    cprintf(lin++, 0, ALIGN_NONE, buf4,
            sStat.numusers, sStat.numtotal, cbLimits.maxShips(),
            sStat.numactive, sStat.numvacant, sStat.numrobot);

    std::string flags;
    for (const std::string& f : clntServerFlagsStr(sStat.serverFlags))
    {
        if (!flags.empty())
            flags += " ";
        flags += f;
    }
    cprintf(lin++, 0, ALIGN_NONE, buf5, flags.c_str());

    cprintf(lin++, 0, ALIGN_NONE, buf6, sHello.motd);

    return;
}

/* Logon() - log into the server (curses client).  Returns true on
 * success, with the username in username (MAX_USERNAME bytes). */
bool Logon(char *username)
{
    int lin, slin;
    int ch, statline;
    char nm[MAX_USERNAME], pw[MAX_USERNAME], pwr[MAX_USERNAME];
    int rv;
    extern char *ConquestVersion;
    extern char *ConquestDate;

    while (true)
    {
        cdclear();

        /* Display the logo. */
        lin = mcuConqLogo();

        if ( sStat.serverFlags & SERVER_F_CLOSED )
            cprintf( lin, 0, ALIGN_CENTER, "#%d#%s", RedLevelColor,
                     "The game is closed.");
        else
            cprintf( lin, 1, ALIGN_CENTER,
                     "#%d#Welcome to #%d#Conquest#%d# %s (%s)",
                     YellowLevelColor,
                     RedLevelColor,
                     YellowLevelColor,
                     ConquestVersion, ConquestDate);

        lin++;
        statline = lin;

        lin += 7;

        nm[0] = 0;

        cdrefresh();

        while (true)			/* login loop */
	{
            _printStatus(statline);
            slin = lin;
            cdclrl( slin, Context.maxlin - slin - 1 );
            slin += 3;
            cdclrl( MSG_LIN1, 2 );
            cdputs("You can use A-Z, a-z, and 0-9.", MSG_LIN1, 1);
            cprintf( slin - 2, 1, ALIGN_LEFT,
                     "#%d#Please login. Press [ENTER] to exit.",
                     SpecialColor);

            cprintf( slin - 1, 1, ALIGN_LEFT,
                     "#%d#(New Users: Just pick a username)",
                     SpecialColor);

            ch = cdgetx( "Username: ", slin, 1, TERMS, nm, MAX_USERNAME - 1,
                         true );

            if (ch == TERM_EXTRA )
                break;		/* redraw stats and things */

            if (ch == TERM_ABORT || nm[0] == 0)
                return false;   /* if empty user, or abort char, leave */

            if (!uaValidateUsername(nm))
	    {			/* invalid username */
                cdbeep();
                cdclrl( MSG_LIN2, 1 );
                uiPutColor(RedLevelColor);
                cdputs("Invalid character in username.", MSG_LIN2, 1);
                uiPutColor(NoColor);
                nm[0] = 0;
                continue;
	    }

            if ((rv = sendAuth(cInfo.sock, CPAUTH_CHECKUSER, nm, "")) < 0)
                return false;

            if (rv == PERR_NOUSER)
	    {			/* nope... */
                slin++;
                if (mcuAskYN("User doesn't exist. Is this a new user? ", slin, 1))
		{			/* yep */
                    pw[0] = 0;
                    cdclrl( MSG_LIN1, 2  );
                    cdputs("Use any printable characters.", MSG_LIN1, 1);
                    cdgetx( "Password: ", slin, 1,
                            TERMS, pw, MAX_USERNAME - 1, false );

                    slin++;
                    pwr[0] = 0;
                    cdclrl( MSG_LIN1, 2  );
                    cdputs("Use any printable characters.", MSG_LIN1, 1);
                    cdgetx( "Retype Password: ", slin, 1,
                            TERMS, pwr, MAX_USERNAME - 1, false );

                    if (strcmp(pw, pwr) != 0)
		    {			/* pw's don't match, start over */
                        cdbeep();
                        cdclrl( MSG_LIN2, 1  );
                        uiPutColor(RedLevelColor);
                        cdputs("Passwords don't match.", MSG_LIN2, 1);
                        uiPutColor(NoColor);
                        cdrefresh();
                        utSleep(2.0);
                        continue;
		    }
                    /* if we're here, we have a username
                       and password (new user) - time
                       to rock. */

                    if ((rv = sendAuth(cInfo.sock, CPAUTH_LOGIN,
                                       nm, pw)) < 0)
                        return false;

                    if (rv != PERR_OK)
		    {
                        utLog("conquest: CPAUTH_LOGIN returned %d\n",
                              rv);
                        continue;
		    }

                    utLog("clntauth: INFO: new user '%s' logged in", nm);
                    utStrncpy(username, nm, MAX_USERNAME);
                    return true;
		}
                else
                    continue;		/* restart */
	    }
            else
	    {			/* exists, verify that the pw is valid */
                slin++;
                pw[0] = 0;
                cdclrl( MSG_LIN1, 2  );
                cdputs("Use any printable characters.", MSG_LIN1, 1);
                cdgetx( "Password: ", slin, 1,
                        TERMS, pw, MAX_USERNAME - 1, false );

                if ((rv = sendAuth(cInfo.sock, CPAUTH_LOGIN,
                                   nm, pw)) < 0)
                    return false;

                if (rv != PERR_OK)
		{			/* invalid pw */
                    cdbeep();
                    cdclrl( MSG_LIN2, 1  );
                    uiPutColor(RedLevelColor);
                    cdputs("Invalid Password.", MSG_LIN2, 1);
                    uiPutColor(NoColor);
                    cdrefresh();
                    utLog("INFO: Invalid password for user '%s'", nm);
                    utSleep(2.0);
                    continue;
		}

                /* good pw - go for the gusto */
                utLog("clntauth:INFO: user '%s' logged in", nm);
                utStrncpy(username, nm, MAX_USERNAME);
                return true;
	    }
	} /* end while */
    }

    return false;               /* NOTREACHED */
}
