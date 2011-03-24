/*
 *      Copyright (C) 2005-2008 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "system.h"
#include "utils/log.h"
#include "threads/SingleLock.h"
#include "utils/TimeUtils.h"
#include "CacheCircular.h"

using namespace XFILE;

CCacheCircular::CCacheCircular(size_t front, size_t back)
 : CCacheStrategy()
 , m_beg(0)
 , m_end(0)
 , m_cur(0)
 , m_buf(NULL)
 , m_size(front + back)
 , m_size_back(back)
#ifdef _WIN32
 , m_handle(INVALID_HANDLE_VALUE)
#endif
{
}

CCacheCircular::~CCacheCircular()
{
  Close();
}

int CCacheCircular::Open()
{
#ifdef _WIN32
  m_handle = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, m_size, NULL);
  if(m_handle == INVALID_HANDLE_VALUE)
    return CACHE_RC_ERROR;
  m_buf = (uint8_t*)MapViewOfFile(m_handle, FILE_MAP_ALL_ACCESS, 0, 0, 0);
#else
  m_buf = new uint8_t[m_size];
#endif
  if(m_buf == 0)
    return CACHE_RC_ERROR;
  m_beg = 0;
  m_end = 0;
  m_cur = 0;
  return CACHE_RC_OK;
}

int CCacheCircular::Close()
{
#ifdef _WIN32
  UnmapViewOfFile(m_buf);
  CloseHandle(m_handle);
  m_handle = INVALID_HANDLE_VALUE;
#else
  delete[] m_buf;
#endif
  m_buf = NULL;
  return CACHE_RC_OK;
}

int CCacheCircular::WriteToCache(const char *buf, size_t len)
{
  CSingleLock lock(m_sync);

  // where are we in the buffer
  size_t pos   = m_end % m_size;
  size_t limit = m_size - m_size_back - (size_t)(m_end - m_cur);
  size_t wrap  = m_size - pos;

  // limit by max forward size
  if(len > limit)
    len = limit;

  // limit to wrap point
  if(len > wrap)
    len = wrap;

  // write the data
  memcpy(m_buf + pos, buf, len);
  m_end += len;

  // drop history that was overwritten
  if(m_end - m_beg > m_size)
    m_beg = m_end - m_size;

  m_written.Set();

  return len;
}

int CCacheCircular::ReadFromCache(char *buf, size_t len)
{
  CSingleLock lock(m_sync);

  size_t pos   = m_cur % m_size;
  size_t avail = std::min(m_size - pos, (size_t)(m_end - m_cur));

  if(avail == 0)
  {
    if(IsEndOfInput())
      return CACHE_RC_EOF;
    else
      return CACHE_RC_WOULD_BLOCK;
  }

  if(len > avail)
    len = avail;

  memcpy(buf, m_buf + pos, len);
  m_cur += len;

  if (len > 0)
    m_space.Set();

  return len;
}

int64_t CCacheCircular::WaitForData(unsigned int minumum, unsigned int millis)
{
  CSingleLock lock(m_sync);
  uint64_t avail = m_end - m_cur;

  if(millis == 0 || IsEndOfInput())
    return avail;

  if(minumum > m_size - m_size_back)
    minumum = m_size - m_size_back;

  unsigned int time = CTimeUtils::GetTimeMS() + millis;
  while (!IsEndOfInput() && avail < minumum && CTimeUtils::GetTimeMS() < time )
  {
    lock.Leave();
    m_written.WaitMSec(50); // may miss the deadline. shouldn't be a problem.
    lock.Enter();
    avail = m_end - m_cur;
  }

  return avail;
}

int64_t CCacheCircular::Seek(int64_t pos, int iWhence)
{
  if (iWhence != SEEK_SET)
    return CACHE_RC_ERROR;

  CSingleLock lock(m_sync);

  // if seek is a bit over what we have, try to wait a few seconds for the data to be available.
  // we try to avoid a (heavy) seek on the source
  if ((uint64_t)pos >= m_end && (uint64_t)pos < m_end + 100000)
  {
    lock.Leave();
    WaitForData((size_t)(pos - m_cur), 5000);
    lock.Enter();
  }

  if((uint64_t)pos >= m_beg && (uint64_t)pos <= m_end)
  {
    m_cur = pos;
    return pos;
  }

  return CACHE_RC_ERROR;
}

void CCacheCircular::Reset(int64_t pos)
{
  CSingleLock lock(m_sync);
  m_end = pos;
  m_beg = pos;
  m_cur = pos;
}
