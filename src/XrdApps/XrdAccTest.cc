/******************************************************************************/
/*                                                                            */
/*                         X r d A c c T e s t . c c                          */
/*                                                                            */
/* (c) 2010 by the Board of Trustees of the Leland Stanford, Jr., University  */
/*                            All Rights Reserved                             */
/*   Produced by Andrew Hanushevsky for Stanford University under contract    */
/*              DE-AC02-76-SFO0515 with the Department of Energy              */
/*                                                                            */
/* This file is part of the XRootD software suite.                            */
/*                                                                            */
/* XRootD is free software: you can redistribute it and/or modify it under    */
/* the terms of the GNU Lesser General Public License as published by the     */
/* Free Software Foundation, either version 3 of the License, or (at your     */
/* option) any later version.                                                 */
/*                                                                            */
/* XRootD is distributed in the hope that it will be useful, but WITHOUT      */
/* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or      */
/* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public       */
/* License for more details.                                                  */
/*                                                                            */
/* You should have received a copy of the GNU Lesser General Public License   */
/* along with XRootD in a file called COPYING.LESSER (LGPL license) and file  */
/* COPYING (GPL license).  If not, see <http://www.gnu.org/licenses/>.        */
/*                                                                            */
/* The copyright holder's institutional names and contributor's names may not */
/* be used to endorse or promote products derived from this software without  */
/* specific prior written permission of the institution or contributor.       */
/******************************************************************************/

/* Syntax: testaccess [=c cfn] [d] [-t] [user host op path]

   Where:  -c     sets the config file name (default is ./acc.cf)
         v -d     turns on debugging.
           -t     turns on tracing.
           user   is the requesting username
           host   is the user's host name
           op     is the requested operation and is one of:
                  cr - create    mv - rename    st - status
                  lk - lock      rd - read      wr - write
                  ls - readdir   rm - remove    ?  - display privs
*/
  
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <strings.h>
#include <stdio.h>
#include <grp.h>
#include <arpa/inet.h>
#include <sys/param.h>
#include <sys/socket.h>

#include "XrdVersion.hh"

#include "XrdAcc/XrdAccAuthorize.hh"
#include "XrdAcc/XrdAccConfig.hh"
#include "XrdAcc/XrdAccGroups.hh"
#include "XrdAcc/XrdAccPrivs.hh"
#include "XrdSys/XrdSysError.hh"
#include "XrdSys/XrdSysHeaders.hh"
#include "XrdSys/XrdSysLogger.hh"
#include "XrdOuc/XrdOucStream.hh"

/******************************************************************************/
/*                        G l o b a l   O b j e c t s                         */
/******************************************************************************/
  
extern int optind;

extern char *optarg;

char *PrivsConvert(XrdAccPrivCaps &ctab, char *buff, int blen);

XrdAccAuthorize *Authorize;

int  extra;

XrdSysLogger myLogger;

XrdSysError  eroute(&myLogger, "acc_");

/******************************************************************************/
/*                       O p e r a t i o n   T a b l e                        */
/******************************************************************************/
typedef struct {const char *opname; Access_Operation oper;} optab_t;
optab_t optab[] =
             {{"?",      AOP_Any},
              {"cm",     AOP_Chmod},
              {"co",     AOP_Chown},
              {"cr",     AOP_Create},
              {"rm",     AOP_Delete},
              {"lk",     AOP_Lock},
              {"mk",     AOP_Mkdir},
              {"mv",     AOP_Rename},
              {"rd",     AOP_Read},
              {"ls",     AOP_Readdir},
              {"st",     AOP_Stat},
              {"wr",     AOP_Update}
             };

int opcnt = sizeof(optab)/sizeof(optab[0]);
  
/******************************************************************************/
/*                                  m a i n                                   */
/******************************************************************************/
  
int main(int argc, char **argv)
{
static XrdVERSIONINFODEF(myVer, XrdAccTest, XrdVNUMBER, XrdVERSION);
extern XrdAccAuthorize *XrdAccDefaultAuthorizeObject(XrdSysLogger   *lp,
                                                     const char     *cfn,
                                                     const char     *parm,
                                                     XrdVersionInfo &myVer);
void Usage(const char *);
char *p2l(XrdAccPrivs priv, char *buff, int blen);
int rc = 0, argnum;
char c, *argval[32];
int DoIt(int argnum, int argc, char **argv);
XrdOucStream Command;
const int maxargs = sizeof(argval)/sizeof(argval[0]);
char *ConfigFN = (char *)"./acc.cf";

// Get all of the options.
//
   while ((c=getopt(argc,argv,"c:d")) != (char)EOF)
     { switch(c)
       {
       case 'c': ConfigFN = optarg;                  break;
       default:  Usage("Invalid option.");
       }
     }

// Obtain the authorization object
//
if (!(Authorize = XrdAccDefaultAuthorizeObject(&myLogger, ConfigFN, 0, myVer)))
   {cerr << "testaccess: Initialization failed." <<endl;
    exit(2);
   }

// If command line options specified, process this
//
   if (optind < argc) {rc = DoIt(optind, argc, argv); exit(rc);}

// Start accepting command from standard in until eof
//
   Command.Attach(0);
   cout << "Waiting for arguments..." <<endl;
   while(Command.GetLine())
       while((argval[1] = Command.GetToken()))
            {for (argnum=2;
                  argnum < maxargs && (argval[argnum]=Command.GetToken());
                  argnum++) {}
             rc |= DoIt(1, argnum, argval);
            }

// All done
//
   exit(rc);
}

int DoIt(int argpnt, int argc, char **argv)
{
char *user, *host, *path, *result, buff[16];
Access_Operation cmd2op(char *opname);
void Usage(const char *);
Access_Operation optype;
XrdAccPrivCaps pargs;
XrdAccPrivs auth;
XrdSecEntity Entity("");

// Make sure user specified
//
   if (argpnt >= argc) Usage("user not specified.");
   user = argv[argpnt++];

// Make sure host specified
//
   if (argpnt >= argc) Usage("host not specified.");
   host = argv[argpnt++];

// Make sure op   specified
//
   if (argpnt >= argc) Usage("operation not specified.");
   optype = cmd2op(argv[argpnt++]);

// Make sure path specified
//
   if (argpnt >= argc) Usage("path not specified.");

// Fill out entity
//
   strcpy(Entity.prot, "krb4");
   Entity.name = user;
   Entity.host = host;

// Process each path, as needed
//                                                            x
   while(argpnt < argc)
        {path = argv[argpnt++];
         auth = Authorize->Access((const XrdSecEntity *)&Entity,
                                  (const char *)path,
                                                optype);
         if (optype != AOP_Any) result=(auth?(char *)"allowed":(char *)"denied");
            else {pargs.pprivs = auth; pargs.nprivs = XrdAccPriv_None;
                  result = PrivsConvert(pargs, buff, sizeof(buff));
                 }
         cout <<result <<": " <<path <<endl;
        }

return 0;
}

/******************************************************************************/
/*                                c m d 2 o p                                 */
/******************************************************************************/
  
Access_Operation cmd2op(char *opname)
{
   int i;
   for (i = 0; i < opcnt; i++) 
       if (!strcmp(opname, optab[i].opname)) return optab[i].oper;
   cerr << "testaccess: Invalid operation - " <<opname <<endl;
   exit(1);
   return AOP_Any;
}

/******************************************************************************/
/*                          P r i v s C o n v e r t                           */
/******************************************************************************/
  
char *PrivsConvert(XrdAccPrivCaps &ctab, char *buff, int blen)
{
     int i=0, j, k=2, bmax = blen-1;
     XrdAccPrivs privs;
     static struct {XrdAccPrivs pcode; char plet;} p2l[] =
                   {{XrdAccPriv_Delete,  'd'},
                    {XrdAccPriv_Insert,  'i'},
                    {XrdAccPriv_Lock,    'k'},
                    {XrdAccPriv_Lookup,  'l'},
                    {XrdAccPriv_Rename,  'n'},
                    {XrdAccPriv_Read,    'r'},
                    {XrdAccPriv_Write,   'w'}
                   };
     static int p2lnum = sizeof(p2l)/sizeof(p2l[0]);

     privs = ctab.pprivs;
     while(k--)
       {for (j = 0; j < p2lnum && i < bmax; j++)
            if (privs & p2l[j].pcode) buff[i++] = p2l[j].plet;
        if (i < bmax && ctab.nprivs != XrdAccPriv_None) buff[i++] = '-';
           else break;
        privs = ctab.nprivs;
       }
     buff[i] = '\0';
     return buff;
}

/******************************************************************************/
/*                                 U s a g e                                  */
/******************************************************************************/
  
void Usage(const char *msg)
     {if (msg) cerr <<"xrdacctest: " <<msg <<endl;
      cerr << "Usage: xrdacctest [-c cfn] [<user> <host> {d|i|k|l|n|r|w} "
                     "<path> [<path> [...]]]" <<endl;
      exit(1);
     }
