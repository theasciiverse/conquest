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

#ifndef _CUMISC_H
#define _CUMISC_H

void mcuHistList();
void mcuPutThing( int what, int lin, int col );
int  mcuReadMsg( int msgnum, int dsplin );
void mcuPlanetList( int team, int snum );
void mcuPlayList( bool godlike, bool doall, int snum );
int  mcuReviewMsgs( int snum, int slm );
void mcuTeamList( int team );
void mcuUserList( bool godlike, int snum );
void mcuUserStats( bool godlike, int snum );
bool  mcuConfirm(void);
int  mcuAskYN(const char *question, int lin, int col);
char mcuGetCX( const char *pmt, int lin, int offset, const char *terms,
               char *buf, int len );
int  mcuMore( const char *pmt );
void mcuPutPrompt( const char *pmt, int line );
int  mcuConqLogo(void);
bool mcuKPAngle(int ch, real *angle);
bool mcuGetTarget( const char *pmt, int lin, int col, real *dir,
                   real cdefault );

#endif /* _CUMISC_H */
