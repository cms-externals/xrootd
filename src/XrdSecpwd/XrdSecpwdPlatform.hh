// $Id$
#ifndef __SECPWD_PLATFORM_
#define __SECPWD_PLATFORM_
/******************************************************************************/
/*                                                                            */
/*                 X r d S e c p w d P l a t f o r m. h h                     */
/*                                                                            */
/* (c) 2005 by the Board of Trustees of the Leland Stanford, Jr., University  */
/*                            All Rights Reserved                             */
/*   Produced by Andrew Hanushevsky for Stanford University under contract    */
/*              DE-AC03-76-SFO0515 with the Department of Energy              */
/******************************************************************************/

//
// crypt
//
#if defined(__solaris__)
#include <crypt.h>
#endif
#if defined(__osf__) || defined(__sgi) || defined(__APPLE__)
extern "C" char *crypt(const char *, const char *);
#endif

//
// shadow passwords
//
#include <grp.h>

#ifdef HAVE_SHADOWPW
#include <shadow.h>
#endif

#endif
