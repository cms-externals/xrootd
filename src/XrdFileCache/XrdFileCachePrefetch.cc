//----------------------------------------------------------------------------------
// Copyright (c) 2014 by Board of Trustees of the Leland Stanford, Jr., University
// Author: Alja Mrak-Tadel, Matevz Tadel, Brian Bockelman
//----------------------------------------------------------------------------------
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// XRootD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
//----------------------------------------------------------------------------------

#include <stdio.h>
#include <sstream>
#include <fcntl.h>

#include "XrdCl/XrdClLog.hh"
#include "XrdCl/XrdClConstants.hh"
#include "XrdCl/XrdClFile.hh"
#include "XrdSys/XrdSysPthread.hh"
#include "XrdSys/XrdSysTimer.hh"
#include "XrdOss/XrdOss.hh"
#include "XrdOuc/XrdOucEnv.hh"
#include "Xrd/XrdScheduler.hh"

#include "XrdSfs/XrdSfsInterface.hh"
#include "XrdPosix/XrdPosixFile.hh"

#include "XrdFileCachePrefetch.hh"
#include "XrdFileCacheFactory.hh"
#include "XrdFileCache.hh"

using namespace XrdFileCache;

namespace XrdPosixGlobals
{
   extern XrdScheduler *schedP;
}

namespace
{
   const int PREFETCH_MAX_ATTEMPTS = 10;

   class DiskSyncer : public XrdJob
   {
   private:
      Prefetch *m_prefetch;

   public:
      DiskSyncer(Prefetch *pref, const char *desc="") :
         XrdJob(desc),
         m_prefetch(pref)
      {}

      void DoIt()
      {
         m_prefetch->Sync();
      }
   };
}


Prefetch::RAM::RAM():m_numBlocks(0),m_buffer(0), m_blockStates(0), m_writeMutex(0)
{
   m_numBlocks = Factory::GetInstance().RefConfiguration().m_NRamBuffersRead + Factory::GetInstance().RefConfiguration().m_NRamBuffersPrefetch;
   m_buffer = (char*)malloc(m_numBlocks * Factory::GetInstance().RefConfiguration().m_bufferSize);
   m_blockStates = new RAMBlock[m_numBlocks];
}

Prefetch::RAM::~RAM()
{
   free(m_buffer);
   delete [] m_blockStates;
}

Prefetch::Prefetch(XrdOucCacheIO &inputIO, std::string& disk_file_path, long long iOffset, long long iFileSize) :
   m_output(NULL),
   m_infoFile(NULL),
   m_cfi(Factory::GetInstance().RefConfiguration().m_bufferSize),
   m_input(inputIO),
   m_temp_filename(disk_file_path),
   m_offset(iOffset),
   m_fileSize(iFileSize),
   m_started(false),
   m_failed(false),
   m_stopping(false),
   m_stopped(false),
   m_stateCond(0),    // We will explicitly lock the condition before use.
   m_queueCond(0),
   m_syncer(new DiskSyncer(this, "XrdFileCache::DiskSyncer")),
   m_non_flushed_cnt(0),
   m_in_sync(false)
{
   assert(m_fileSize > 0);
   clLog()->Debug(XrdCl::AppMsg, "Prefetch::Prefetch() %p %s", (void*)&m_input, lPath());
}
//______________________________________________________________________________

bool Prefetch::InitiateClose()
{
   // Retruns true if delay is needed

   clLog()->Debug(XrdCl::AppMsg, "Prefetch::Initiate close start", lPath());
   if (m_cfi.IsComplete()) return false;

   XrdSysCondVarHelper monitor(m_stateCond);
   m_stopping = true;
   if (m_started == false)
   {
      m_stopped = true;
      return false;
   }

   return true;
}

//______________________________________________________________________________
Prefetch::~Prefetch()
{
   clLog()->Debug(XrdCl::AppMsg, "Prefetch::~Prefetch() %p %s", (void*)this, lPath());

   m_queueCond.Lock();
   m_queueCond.Signal();
   m_queueCond.UnLock();

   Cache::RemoveWriteQEntriesFor(this);
   clLog()->Info(XrdCl::AppMsg, "Prefetch::~Prefetch() check write queues ...%s", lPath());
   while (true)
   {
      m_stateCond.Lock();
      bool isStopped = m_stopped;
      m_stateCond.UnLock();

      if (isStopped)
      {
         clLog()->Debug(XrdCl::AppMsg, "Prefetch::~Prefetch sleep, waiting queues to empty begin %s", lPath());

         bool writewait = false;
         m_ram.m_writeMutex.Lock();
         for (int i = 0; i < m_ram.m_numBlocks;++i )
         {
            if (m_ram.m_blockStates[i].refCount)
            {
               writewait = true;
               break;
            }
         }
         m_ram.m_writeMutex.UnLock();

         // disk sync
         {
            XrdSysMutexHelper _lck(&m_syncStatusMutex);

            if (m_in_sync) writewait = true;
         }

         if (!writewait)
            break;
      }

      XrdSysTimer::Wait(100);
   }
   clLog()->Debug(XrdCl::AppMsg, "Prefetch::~Prefetch finished with writing %s",lPath() );

   bool do_sync = false;
   {
      XrdSysMutexHelper _lck(&m_syncStatusMutex);
      if (m_non_flushed_cnt > 0)
      {
         do_sync   = true;
         m_in_sync = true;

         clLog()->Info(XrdCl::AppMsg, "Prefetch::~Prefetch sync unflushed %d\n", m_non_flushed_cnt);
      }
   }
   if (do_sync)
   {
      Sync();
   }

   if (m_output)
   {
      clLog()->Info(XrdCl::AppMsg, "Prefetch::~Prefetch close data file %p",(void*)this , lPath());

      m_output->Close();
      delete m_output;
      m_output = NULL;
   }
   else
   {
      clLog()->Info(XrdCl::AppMsg, "Prefetch::~Prefetch close data file -- not opened %p",(void*)this , lPath());
   }
   if (m_infoFile)
   {
      clLog()->Info(XrdCl::AppMsg, "Prefetch::~Prefetch close info file");

      // write statistics in *cinfo file
      AppendIOStatToFileInfo();

      m_infoFile->Close();
      delete m_infoFile;
      m_infoFile = NULL;
   }
   else
   {
      clLog()->Info(XrdCl::AppMsg, "Prefetch::~Prefetch close info file -- not opened %p",(void*)this , lPath());
   }

   delete m_syncer;
}

//______________________________________________________________________________
const char* Prefetch::lPath() const
{
  return  m_temp_filename.c_str();
}

//______________________________________________________________________________

bool Prefetch::Open()
{
   // clLog()->Debug(XrdCl::AppMsg, "Prefetch::Open() open file for disk cache %s", lPath());
   XrdOss  &output_fs =  *Factory::GetInstance().GetOss();
   // Create the data file itself.
   XrdOucEnv myEnv;
   output_fs.Create(Factory::GetInstance().RefConfiguration().m_username.c_str(), m_temp_filename.c_str(), 0644, myEnv, XRDOSS_mkpath);
   m_output = output_fs.newFile(Factory::GetInstance().RefConfiguration().m_username.c_str());
   if (m_output)
   {
      int res = m_output->Open(m_temp_filename.c_str(), O_RDWR, 0644, myEnv);
      if ( res < 0)
      {
         clLog()->Error(XrdCl::AppMsg, "Prefetch::Open() can't open local file %s", m_temp_filename.c_str());
         delete m_output;
         m_output = NULL;
         return false;
      }
   }
   else
   {
      clLog()->Error(XrdCl::AppMsg, "Prefetch::Open() can't get data holder ");
      return false;
   }

   // Create the info file
   std::string ifn = m_temp_filename + Info::m_infoExtension;
   output_fs.Create(Factory::GetInstance().RefConfiguration().m_username.c_str(), ifn.c_str(), 0644, myEnv, XRDOSS_mkpath);
   m_infoFile = output_fs.newFile(Factory::GetInstance().RefConfiguration().m_username.c_str());
   if (m_infoFile)
   {

      int res = m_infoFile->Open(ifn.c_str(), O_RDWR, 0644, myEnv);
      if ( res < 0 )
      {
         clLog()->Error(XrdCl::AppMsg, "Prefetch::Open() can't get info-FD %s  %s", ifn.c_str(), lPath());
         delete m_output;
         m_output = NULL;
         delete m_infoFile;
         m_infoFile = NULL;

         return false;
      }
   }
   if (!m_infoFile)
   {
      return false;
   }
   if ( m_cfi.Read(m_infoFile) <= 0)
   {
      assert(m_fileSize > 0);
      int ss = (m_fileSize -1)/m_cfi.GetBufferSize() + 1;
      //      clLog()->Info(XrdCl::AppMsg, "Creating new file info with size %lld. Reserve space for %d blocks %s", m_fileSize,  ss, lPath());
      m_cfi.ResizeBits(ss);
      m_cfi.WriteHeader(m_infoFile);
   }
   else
   {
      clLog()->Debug(XrdCl::AppMsg, "Info file already exists %s", lPath());
      // m_cfi.Print();
   }

   return true;
}


//_________________________________________________________________________________________________
void
Prefetch::Run()
{
   {
      XrdSysCondVarHelper monitor(m_stateCond);

      if (m_started)
      {
         clLog()->Error(XrdCl::AppMsg, "Prefetch::Run() Already started for %s", lPath());
         m_stopped = true;
         return;
      }

      if (m_stopped)
      {
         return;
      }

      if ( !Open())
      {
         m_failed = true;
      }
      m_started = true;
      // Broadcast to possible io-read waiting objects
      m_stateCond.Broadcast();

      if (m_failed)
      {
         m_stopped = true;
         return;
      }
   }
   assert(m_infoFile);
   clLog()->Debug(XrdCl::AppMsg, "Prefetch::Run() Starting loop over tasks for %s", lPath());

   Task* task;
   int numReadBlocks = 0;
   while ((task = GetNextTask()) != 0)
   {
      DoTask(task);

      if (task->condVar)
      {
         clLog()->Debug(XrdCl::AppMsg, "Prefetch::Run() task %p condvar %p",  task, task->condVar);
         XrdSysCondVarHelper tmph(task->condVar);
         task->condVar->Signal();
      }

      clLog()->Debug(XrdCl::AppMsg, "Prefetch::Run() delete task %p condvar %p",  task, task->condVar);
      delete task;

      numReadBlocks++;
   }  // loop tasks


   clLog()->Debug(XrdCl::AppMsg, "Prefetch::Run() exits, download %s  !", m_cfi.IsComplete() ? " completed " : "unfinished %s", lPath());


   m_cfi.CheckComplete();

   m_stateCond.Lock();
   m_stopped = true;
   m_stateCond.UnLock();
} // end Run()


//______________________________________________________________________________
Prefetch::Task*
Prefetch::CreateTaskForFirstUndownloadedBlock()
{
   // first check if there are enough write and ram resources
   if (Cache::HaveFreeWritingSlots() == false) return 0;

   int nRP = 0;
   for (int i =0 ; i < m_ram.m_numBlocks; ++i) {
      if (m_ram.m_blockStates[i].fromRead == false && m_ram.m_blockStates[i].refCount > 0) nRP++;
   }
   if ( nRP >= Factory::GetInstance().RefConfiguration().m_NRamBuffersPrefetch ) {
      clLog()->Dump(XrdCl::AppMsg, "Prefetch::CreateTaskForFirstUndownloadedBlock no resources %d %d, %s ",  nRP, Factory::GetInstance().RefConfiguration().m_NRamBuffersPrefetch, lPath());
      return 0;
   }

   Task *task = new Task;
   Task &t = * task;
   t.ramBlockIdx = -1;
   int fileBlockIdx = -1;
   for (int f = 0; f < m_cfi.GetSizeInBits(); ++f)
   {
      m_downloadStatusMutex.Lock();
      bool isdn = m_cfi.TestBit(f);
      m_downloadStatusMutex.UnLock();

      if (!isdn)
      {
         fileBlockIdx = f + m_offset/m_cfi.GetBufferSize();
         // get ram for the file block
         m_ram.m_writeMutex.Lock();
         for (int r =0 ; r < m_ram.m_numBlocks; ++r)
         {

            if (m_ram.m_blockStates[r].fileBlockIdx == fileBlockIdx)
            {
               // skip this task, the file block f is already downloaded
               break;
            }

            if (m_ram.m_blockStates[r].refCount == 0 )
            {
               t.ramBlockIdx = r;

               assert(m_ram.m_blockStates[r].fileBlockIdx == -1);
               m_ram.m_blockStates[r].refCount = 1;
               m_ram.m_blockStates[r].fileBlockIdx = fileBlockIdx;
               m_ram.m_blockStates[r].status = kReadWait;
               break;
            }
         }
         m_ram.m_writeMutex.UnLock();

         break;
      }
   }

   if (t.ramBlockIdx >= 0) {
      clLog()->Dump(XrdCl::AppMsg, "Prefetch::CreateTaskForFirstUndownloadedBlock success block %d %s ",  fileBlockIdx, lPath());
      return task;
   }
   else if (fileBlockIdx == -1) {
      m_cfi.CheckComplete();
   }

   delete task;
   return 0;
}

//_____________________________________________________________________________
Prefetch::Task*
Prefetch::GetNextTask()
{
   while (true)
   {
      m_stateCond.Lock();
      bool doExit = m_stopping;
      m_stateCond.UnLock();
      if (doExit) return 0;

      m_queueCond.Lock();

      if ( ! m_tasks_queue.empty())
      {
         // Exiting with queueMutex held !!!
         break;
      }

      // returns true on ETIMEDOUT
      if ( ! m_queueCond.WaitMS(100))
      {
         // Can be empty as result of a signal from destructor
         if( ! m_tasks_queue.empty())
            // Exiting with queueMutex held !!!
            break;
      }

      m_queueCond.UnLock();

      m_stateCond.Lock();
      doExit = m_stopping;
      m_stateCond.UnLock();
      if (doExit) return 0;

      Task* t = CreateTaskForFirstUndownloadedBlock();
      if (t)
         return t;
      else if (m_cfi.IsComplete()) 
         return 0; 
   }

   Task *task = m_tasks_queue.front();
   m_tasks_queue.pop_front();

   m_queueCond.UnLock();

   assert(task->ramBlockIdx >=0);
   clLog()->Info(XrdCl::AppMsg, "Prefetch::GetNextTask [%d] from queue %s", m_ram.m_blockStates[task->ramBlockIdx].fileBlockIdx, lPath());

   return task;
}

//______________________________________________________________________________
void
Prefetch::DoTask(Task* task)
{
   // read block from client  into buffer
   int fileBlockIdx = m_ram.m_blockStates[task->ramBlockIdx].fileBlockIdx;
   long long offset  = fileBlockIdx * m_cfi.GetBufferSize();

   long long  rw_size = m_cfi.GetBufferSize();
   // fix size if this is the last file block
   if ( offset + rw_size - m_offset > m_fileSize ) {
      rw_size = m_fileSize + m_offset - offset;
      assert (rw_size < m_cfi.GetBufferSize());
   }
   int   missing = rw_size;
   int   cnt  = 0;
   char* buff = m_ram.m_buffer;
   buff += task->ramBlockIdx * m_cfi.GetBufferSize();
   while (missing)
   {
      clLog()->Dump(XrdCl::AppMsg, "Prefetch::DoTask() for block f = %d r = %dsingal = %p  %s", fileBlockIdx, task->ramBlockIdx, task->condVar,  lPath());
      int retval = m_input.Read(buff, offset, missing);
      if (retval < 0)
      {
         clLog()->Warning(XrdCl::AppMsg, "Prefetch::DoTask() failed for negative ret %d block %d %s", retval, fileBlockIdx , lPath());
         break;
      }

      missing -= retval;
      offset  += retval;
      buff    += retval;
      ++cnt;
      if (cnt > PREFETCH_MAX_ATTEMPTS)
      {
         break;
      }
   }

   m_ram.m_writeMutex.Lock();
   if (missing) {
       m_ram.m_blockStates[task->ramBlockIdx].status = kReadFailed;
       m_ram.m_blockStates[task->ramBlockIdx].readErrno = errno;
   }
   else {
       m_ram.m_blockStates[task->ramBlockIdx].status = kReadSuccess;
       m_ram.m_blockStates[task->ramBlockIdx].readErrno = 0;
   }
   m_ram.m_writeMutex.Broadcast();
   m_ram.m_writeMutex.UnLock();

   if (missing == 0)
   {
      // queue for ram to disk write
      XrdSysCondVarHelper monitor(m_stateCond);
      if (!m_stopping) { Cache::AddWriteTask(this, task->ramBlockIdx, rw_size, task->condVar ? true : false );
      }
      else {
         m_ram.m_blockStates[task->ramBlockIdx].refCount--;
      }

   }
   else
   {
      DecRamBlockRefCount(task->ramBlockIdx);
      clLog()->Dump(XrdCl::AppMsg, "Prefetch::DoTask() incomplete read missing %d for block %d %s", missing, fileBlockIdx, lPath());
   }
}

//_________________________________________________________________________________________________
void
Prefetch::WriteBlockToDisk(int ramIdx, size_t size)
{
   // called from XrdFileCache::Cache when process queue

   int fileIdx = m_ram.m_blockStates[ramIdx].fileBlockIdx;
   char* buff = m_ram.m_buffer;
   buff += ramIdx*m_cfi.GetBufferSize();
   assert(ramIdx >=0 && ramIdx < m_ram.m_numBlocks);
   int retval = 0;

   // write block buffer into disk file
   long long offset = fileIdx * m_cfi.GetBufferSize() - m_offset;
   int buffer_remaining = size;
   int buffer_offset = 0;
   int cnt = 0;
   while ((buffer_remaining > 0) && // There is more to be written
          (((retval = m_output->Write(buff, offset + buffer_offset, buffer_remaining)) != -1)
           || (errno == EINTR))) // Write occurs without an error
   {
      buffer_remaining -= retval;
      buff += retval;
      cnt++;

      if (buffer_remaining)
      {
         clLog()->Warning(XrdCl::AppMsg, "Prefetch::WriteToDisk() reattempt[%d] writing missing %d for block %d %s",
                          cnt, buffer_remaining, fileIdx, lPath());
      }
      if (cnt > PREFETCH_MAX_ATTEMPTS)
      {
         clLog()->Error(XrdCl::AppMsg, "Prefetch::WriteToDisk() write failes too manny attempts %s", lPath());
         return;
      }
   }

   // set bit fetched
   clLog()->Dump(XrdCl::AppMsg, "Prefetch::WriteToDisk() success set bit for block [%d] size [%d] %s", fileIdx, size, lPath());
   int pfIdx =  fileIdx - m_offset/m_cfi.GetBufferSize();
   m_downloadStatusMutex.Lock();
   m_cfi.SetBitFetched(pfIdx);
   m_downloadStatusMutex.UnLock();


   // set bit synced
   bool schedule_sync = false;
   {
      XrdSysMutexHelper _lck(&m_syncStatusMutex);

      if (m_in_sync)
      {
         m_writes_during_sync.push_back(pfIdx);
      }
      else
      {
         m_cfi.SetBitWriteCalled(pfIdx);
         ++m_non_flushed_cnt;
      }

      if (m_non_flushed_cnt >= 100)
      {
         schedule_sync     = true;
         m_in_sync         = true;
         m_non_flushed_cnt = 0;
      }
   }

   if (schedule_sync)
   {
      XrdPosixGlobals::schedP->Schedule(m_syncer);
   }
}

//______________________________________________________________________________
void Prefetch::Sync()
{ 
   clLog()->Dump(XrdCl::AppMsg, "Prefetch::Sync %s", lPath());

   m_output->Fsync();

   m_cfi.WriteHeader(m_infoFile);

   int written_while_in_sync;
   {
      XrdSysMutexHelper _lck(&m_syncStatusMutex);

      for (std::vector<int>::iterator i = m_writes_during_sync.begin(); i != m_writes_during_sync.end(); ++i)
      {
         m_cfi.SetBitWriteCalled(*i);
      }
      written_while_in_sync = m_non_flushed_cnt = (int) m_writes_during_sync.size();
      m_writes_during_sync.clear();

      m_in_sync = false;
   }

   clLog()->Dump(XrdCl::AppMsg, "Prefetch::Sync %d blocks written during sync.", written_while_in_sync);

   m_infoFile->Fsync();
}

//______________________________________________________________________________
void Prefetch::DecRamBlockRefCount(int ramIdx)
{
   clLog()->Dump(XrdCl::AppMsg, "Prefetch::DecRamBlockRefCount  %d %d %s",  m_ram.m_blockStates[ramIdx].fileBlockIdx, ramIdx,lPath() );

   // mark ram block available
   m_ram.m_writeMutex.Lock();
   assert(m_ram.m_blockStates[ramIdx].refCount);
   assert(ramIdx >= 0 && ramIdx < m_ram.m_numBlocks);

   m_ram.m_blockStates[ramIdx].refCount --;
   if (m_ram.m_blockStates[ramIdx].refCount == 0) {
       m_ram.m_blockStates[ramIdx].fileBlockIdx = -1;
   }
   m_ram.m_writeMutex.UnLock();
}

//______________________________________________________________________________
bool Prefetch::ReadFromTask(int iFileBlockIdx, char* iBuff, long long iOff, size_t iSize)
{
   // offs == offset inside the block, size  read size in block
   clLog()->Dump(XrdCl::AppMsg, "Prefetch::ReadFromTask fileIdx= %d begin", iFileBlockIdx);

   m_stateCond.Lock();
   bool doExit = m_stopping;
   m_stateCond.UnLock();
   if (doExit) return false;

   if (Cache::HaveFreeWritingSlots())
   {
      int ramIdx = -1;
      m_ram.m_writeMutex.Lock();

      int nRR = 0;
      for (int i =0 ; i < m_ram.m_numBlocks; ++i) {
         if (m_ram.m_blockStates[i].fromRead && m_ram.m_blockStates[i].refCount > 0) nRR++;
      }

      if (nRR < Factory::GetInstance().RefConfiguration().m_NRamBuffersRead) {
         for (int i =0 ; i < m_ram.m_numBlocks; ++i)
         {
            if (m_ram.m_blockStates[i].refCount == 0)
            {
               assert(m_ram.m_blockStates[i].fileBlockIdx == -1);
               ramIdx = i;
               m_ram.m_blockStates[i].refCount = 1;
               m_ram.m_blockStates[i].fileBlockIdx = iFileBlockIdx;
               m_ram.m_blockStates[i].fromRead = true;
               m_ram.m_blockStates[i].status = kReadWait;
               break;
            }
         }
      }
      m_ram.m_writeMutex.UnLock();

      if (ramIdx >= 0)
      {
         clLog()->Dump(XrdCl::AppMsg, "Prefetch::ReadFromTask, going to add task fileIdx=%d ", iFileBlockIdx);
         XrdSysCondVar newTaskCond(0);
         {
            XrdSysCondVarHelper xx(newTaskCond);

            Task* task = new Task(ramIdx, &newTaskCond);

            m_queueCond.Lock();
            m_tasks_queue.push_front(task);
            m_queueCond.Signal();
            m_queueCond.UnLock();

            clLog()->Dump(XrdCl::AppMsg, "Prefetch::ReadFromTask wait task %p confvar %p",  task, task->condVar);

            newTaskCond.Wait();
         }
         if (m_ram.m_blockStates[ramIdx].status == kReadSuccess)
         {
            clLog()->Dump(XrdCl::AppMsg, "Prefetch::ReadFromTask memcpy from RAM to IO::buffer fileIdx=%d ", iFileBlockIdx);
            long long inBlockOff = iOff - iFileBlockIdx * m_cfi.GetBufferSize();
            char* srcBuff = m_ram.m_buffer + ramIdx*m_cfi.GetBufferSize();
            memcpy(iBuff, srcBuff + inBlockOff, iSize);
         }
         else
         {
            clLog()->Error(XrdCl::AppMsg, "Prefetch::ReadFromTask client fileIdx=%d failed", iFileBlockIdx);
         }

         return m_ram.m_blockStates[ramIdx].status == kReadSuccess;
      }
      else {
         clLog()->Debug(XrdCl::AppMsg, "Prefetch::ReadFromTask can't get free ram, not enough resources");
         return false;
      }
   }
   else {
      clLog()->Debug(XrdCl::AppMsg, "Prefetch::ReadFromTask write queue full, not enough resources");
      return false;
   }
}

//______________________________________________________________________________

ssize_t Prefetch::ReadInBlocks(char *buff, off_t off, size_t size)
{
   long long off0 = off;
   int idx_first = off0 / m_cfi.GetBufferSize();
   int idx_last  = (off0 + size -1)/ m_cfi.GetBufferSize();

   size_t bytes_read = 0;
   for (int blockIdx = idx_first; blockIdx <= idx_last; ++blockIdx)
   {

      int readBlockSize = size;
      if (idx_first != idx_last)
      {
         if (blockIdx == idx_first)
         {
            readBlockSize = (blockIdx + 1) * m_cfi.GetBufferSize() - off0;
            clLog()->Dump(XrdCl::AppMsg, "Read partially till the end of the block %s", lPath());
         }
         else if (blockIdx == idx_last)
         {
            readBlockSize = (off0+size) - blockIdx*m_cfi.GetBufferSize();
            clLog()->Dump(XrdCl::AppMsg, "Read partially from beginning of block %s", lPath());
         }
          else
         {
            readBlockSize = m_cfi.GetBufferSize();
         }
      }

      if (readBlockSize > m_cfi.GetBufferSize()) {
         clLog()->Error(XrdCl::AppMsg, "block size invalid");
      }

      int retvalBlock = -1;
      // now do per block read at Read(buff, off, readBlockSize)

      m_downloadStatusMutex.Lock();
      bool dsl = m_cfi.TestBit(blockIdx - m_offset/m_cfi.GetBufferSize());
      m_downloadStatusMutex.UnLock();

      if (dsl)
      {
         retvalBlock = m_output->Read(buff, off - m_offset, readBlockSize);
         m_stats.m_BytesDisk += retvalBlock;
         clLog()->Dump(XrdCl::AppMsg, "Prefetch::ReadInBlocks [%d] disk = %d",blockIdx, retvalBlock);
      }
      else
      {
         int RamIdx = -1;
         m_ram.m_writeMutex.Lock();
         for (int ri = 0; ri < m_ram.m_numBlocks; ++ri )
         {
            if (m_ram.m_blockStates[ri].fileBlockIdx == blockIdx)
            {
               RamIdx = ri;
               m_ram.m_blockStates[ri].refCount++;
               clLog()->Dump(XrdCl::AppMsg, "Prefetch::ReadInBlocks  ram = %d file block = %d wait", RamIdx, blockIdx);
               while (m_ram.m_blockStates[ri].status == kReadWait)
               {
                   m_ram.m_writeMutex.Wait();
               }
               break;
            }
         }

         m_ram.m_writeMutex.UnLock();

         if (RamIdx >= 0 ) {
             if ( m_ram.m_blockStates[RamIdx].status == kReadSuccess) {
                 clLog()->Dump(XrdCl::AppMsg, "Prefetch::ReadInBlocks  ram = %d file block = %d", RamIdx, blockIdx);
                 int in_block_off = off - m_ram.m_blockStates[RamIdx].fileBlockIdx *m_cfi.GetBufferSize();
                 char *rbuff = m_ram.m_buffer + RamIdx*m_cfi.GetBufferSize() + in_block_off;
                 memcpy(buff, rbuff, readBlockSize);
                 DecRamBlockRefCount(RamIdx);
                 retvalBlock = readBlockSize;
             }
             else  {
                 errno = m_ram.m_blockStates[RamIdx].readErrno;
                 DecRamBlockRefCount(RamIdx);
                 return -1;
             }
         }
         else
         {
            if (ReadFromTask(blockIdx, buff, off, readBlockSize))
            {
               retvalBlock = readBlockSize; // presume since ReadFromTask did not fail, could pass a refrence to ReadFromTask
               m_stats.m_BytesRam += retvalBlock;
               clLog()->Dump(XrdCl::AppMsg, "Prefetch::ReadInBlocks [%d]  fromTask = %d", blockIdx, blockIdx);
            }
            else
            {
               retvalBlock = m_input.Read(buff, off, readBlockSize);
               clLog()->Dump(XrdCl::AppMsg, "Prefetch::ReadInBlocks [%d]  client = %d", blockIdx, retvalBlock);
               m_stats.m_BytesMissed += retvalBlock;
            }
         }
      }

      if (retvalBlock > 0 )
      {
         bytes_read += retvalBlock;
         buff       += retvalBlock;
         off        += retvalBlock;
         if (readBlockSize != retvalBlock)
         {
            clLog()->Warning(XrdCl::AppMsg, "Prefetch::ReadInBlocks incomplete , missing = %d", readBlockSize-retvalBlock);
            return bytes_read;
         }
      }
      else
      {
         return bytes_read;
      }
   }
   return bytes_read;
}


//______________________________________________________________________________

int Prefetch::ReadV (const XrdOucIOVec *readV, int n)
{
   {
      XrdSysCondVarHelper monitor(m_stateCond);

      // AMT check if this can be done once during initalization
      if (m_failed) return m_input.ReadV(readV, n);

      if ( ! m_started)
      {
         m_stateCond.Wait();
         if (m_failed) return 0;
      }
   }

   // check if read sizes are big enough to cache

   XrdCl::XRootDStatus Status;
   XrdCl::ChunkList chunkVec;
   XrdCl::VectorReadInfo *vrInfo = 0;

   std::vector<int> cachedReads;

   int nbytes = 0;
   for (int i=0; i<n; i++)
   {
      nbytes += readV[i].size;

      XrdSfsXferSize size = readV[i].size;
      XrdSfsFileOffset off = readV[i].offset;
      bool cached = true;
      const int idx_first = off / m_cfi.GetBufferSize();
      const int idx_last = (off + size - 1) / m_cfi.GetBufferSize();
      for (int blockIdx = idx_first; blockIdx <= idx_last; ++blockIdx)
      {
         bool onDisk = false;
         bool inRam = false;
         m_downloadStatusMutex.Lock();
         onDisk = m_cfi.TestBit(blockIdx);
         m_downloadStatusMutex.UnLock();
         if (!onDisk) {
            m_ram.m_writeMutex.Lock();
            for (int ri = 0; ri < m_ram.m_numBlocks; ++ri )
            {
               if (m_ram.m_blockStates[ri].fileBlockIdx == blockIdx)
               {
                  inRam = true;
                  break;
               }
            }
            m_ram.m_writeMutex.UnLock();
         }

         if ((inRam || onDisk) == false) {
            cached = false;
            break;
         }
      }

      if (cached) {
         clLog()->Debug(XrdCl::AppMsg, "Prefetch::ReadV %d from cache ", i);
         if (Read(readV[i].data, readV[i].offset, readV[i].size) < 0)
             return -1;
      }
      else
      {
         clLog()->Debug(XrdCl::AppMsg, "Prefetch::ReadV %d add back to client vector read ", i);
         chunkVec.push_back(XrdCl::ChunkInfo((uint64_t)readV[i].offset,
                                             (uint32_t)readV[i].size,
                                             (void *)readV[i].data
                                             ));
      }

   }
   if (!chunkVec.empty()) {
      XrdCl::File& clFile = ((XrdPosixFile&)m_input).clFile;
      Status = clFile.VectorRead(chunkVec, (void *)0, vrInfo);
      delete vrInfo;

      if (!Status.IsOK())
      {
         XrdPosixMap::Result(Status);
         return -1;
      }
   }
   return nbytes;
}
//______________________________________________________________________________
ssize_t
Prefetch::Read(char *buff, off_t off, size_t size)
{
   {
      XrdSysCondVarHelper monitor(m_stateCond);

      // AMT check if this can be done once during initalization
      if (m_failed) return m_input.Read(buff, off, size);

      if ( ! m_started)
      {
         m_stateCond.Wait();
         if (m_failed) return 0;
      }
   }

   clLog()->Dump(XrdCl::AppMsg, "Prefetch::Read()  off = %lld size = %lld. %s", off, size, lPath());

   bool fileComplete;
   m_downloadStatusMutex.Lock();
   fileComplete = m_cfi.IsComplete();
   m_downloadStatusMutex.UnLock();

   if (fileComplete)
   {
      int res = m_output->Read(buff, off - m_offset, size);
      m_stats.m_BytesDisk += res;
      return res;
   }
   else
   {
      return ReadInBlocks(buff, off, size);
   }
}


//______________________________________________________________________________
void Prefetch::AppendIOStatToFileInfo()
{
   // lock in case several IOs want to write in *cinfo file
   m_downloadStatusMutex.Lock();
   if (m_infoFile)
   {
      Info::AStat as;
      as.DetachTime  = time(0);
      as.BytesDisk   = m_stats.m_BytesDisk;
      as.BytesRam    = m_stats.m_BytesRam;
      as.BytesMissed = m_stats.m_BytesMissed;
      m_cfi.AppendIOStat(as, (XrdOssDF*)m_infoFile);
   }
   else
   {
      clLog()->Warning(XrdCl::AppMsg, "Prefetch::AppendIOStatToFileInfo() info file not opened %s", lPath());
   }
   m_downloadStatusMutex.UnLock();
}



