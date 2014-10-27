/***********************************************************/
/*                T X D e b u g . c c                      */
/*                        2003                             */
/*             Produced by Alvise Dorigo                   */
/*         & Fabrizio Furano for INFN padova               */
/***********************************************************/
//
//   $Id$

const char *XrdClientDebugCVSID = "$Id$";
//
// Author: Alvise Dorigo, Fabrizio Furano

#include "XrdClient/XrdClientDebug.hh"

#include "XrdSys/XrdSysPthread.hh"
std::atomic<XrdClientDebug*> XrdClientDebug::fgInstance{nullptr};

//_____________________________________________________________________________
XrdClientDebug* XrdClientDebug::Instance() {
   // Create unique instance
   XrdClientDebug* value = fgInstance.load();

   if (!value) {
      value = new XrdClientDebug;
      XrdClientDebug* expected=nullptr;
      if(!fgInstance.compare_exchange_strong(expected,value)) {
        //another thread beat us to the change
        delete value;

        value = expected;
        if (!value) {
          abort();
        }
      }
   }
   return value;
}

//_____________________________________________________________________________
XrdClientDebug::XrdClientDebug() {
   // Constructor

   fOucLog = new XrdSysLogger();
   fOucErr = new XrdSysError(fOucLog, "Xrd");

   fDbgLevel = EnvGetLong(NAME_DEBUG);
}

//_____________________________________________________________________________
XrdClientDebug::~XrdClientDebug() {
   // Destructor
   delete fOucErr;
   delete fOucLog;

   fOucErr = 0;
   fOucLog = 0;

   if(this == fgInstance) {
     fgInstance = 0;
   }
}
