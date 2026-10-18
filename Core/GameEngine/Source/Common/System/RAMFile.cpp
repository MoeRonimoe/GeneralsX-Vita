/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

//----------------------------------------------------------------------------
//
//                       Westwood Studios Pacific.
//
//                       Confidential Information
//                Copyright(C) 2001 - All Rights Reserved
//
//----------------------------------------------------------------------------
//
// Project:   WSYS Library
//
// Module:    IO
//
// File name: WSYS_RAMFile.cpp
//
// Created:   11/08/01
//
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
//         Includes
//----------------------------------------------------------------------------

#include "PreRTS.h"

#include <fcntl.h>
// GeneralsX @build BenderAI 12/02/2026 io.h is Windows-specific (file descriptors)
// Linux: Uses POSIX unistd.h for read/write/close/lseek instead
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <sys/stat.h>
#if defined(__vita__)
#include <pthread.h>
#endif

#include "Common/AsciiString.h"
#include "Common/FileSystem.h"
#include "Common/RAMFile.h"
#include "Common/PerfTimer.h"


//----------------------------------------------------------------------------
//         Externals
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Defines
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Types
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Data
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Public Data
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Prototypes
//----------------------------------------------------------------------------



//----------------------------------------------------------------------------
//         Private Functions
//----------------------------------------------------------------------------

//=================================================================
// RAMFile::RAMFile
//=================================================================

RAMFile::RAMFile()
: m_size(0),
	m_data(nullptr),
	m_pos(0)
{

}


//----------------------------------------------------------------------------
//         Public Functions
//----------------------------------------------------------------------------


//=================================================================
// RAMFile::~RAMFile
//=================================================================

RAMFile::~RAMFile()
{
	closeFile();
}

//=================================================================
// RAMFile::open
//=================================================================
/**
	* This function opens a file using the standard C open() or
	* fopen() call. Access flags are mapped to the appropriate flags.
	* Returns true if file was opened successfully.
	*/
//=================================================================

//DECLARE_PERF_TIMER(RAMFile)
Bool RAMFile::open( const Char *filename, Int access, size_t bufferSize )
{
	//USE_PERF_TIMER(RAMFile)

	bufferSize = 0; // RAM File needs no file buffer because it is read in one go.

	File *file = TheFileSystem->openFile( filename, access, bufferSize );

	if ( file == nullptr )
	{
		return FALSE;
	}

	Bool result = open( file );

	file->close();

	return result;
}

//============================================================================
// RAMFile::open
//============================================================================

Bool RAMFile::open( File *file )
{
	//USE_PERF_TIMER(RAMFile)
	if ( file == nullptr )
	{
		return FALSE;
	}

	const Int access = file->getAccess();

	if ( !File::open( file->getName(), access ))
	{
		return FALSE;
	}

	// read whole file in to memory
	m_size = file->size();
	m_data = MSGNEW("RAMFILE") char [ m_size ];	// pool[]ify

	if ( m_data == nullptr )
	{
		return FALSE;
	}

	m_size = file->read( m_data, m_size );

	if ( m_size < 0 )
	{
		delete [] m_data;
		m_data = nullptr;
		return FALSE;
	}

	m_pos = 0;

	return TRUE;
}

//============================================================================
// RAMFile::openFromArchive
//============================================================================
#if defined(__vita__)
// GeneralsX @performance Claude 12/10/2026 The files read from the archives last, kept for a moment.
// WW3D opens a texture three times in a row (for its size, for a small version, for the texture), and
// an open reads the whole file from the archive. On the PS Vita's memory card that is 30 ms for a
// usual texture and a third of a second for one of the menu's, each time; half of all the time spent
// reading files went into reading what had just been read. Other systems keep what they read from a
// disk themselves, which is why this is for this one only.
namespace
{

struct RecentRead
{
	AsciiString archive;
	Int offset;
	Int size;
	Char *data;
	UnsignedInt used;	///< count of the last use, the lowest goes first
};

enum
{
	RECENT_READ_COUNT = 8,
	RECENT_READ_MIN_BYTES = 16 * 1024,			///< less is one request to the card either way
	RECENT_READ_MAX_BYTES = 5 * 1024 * 1024,
	RECENT_READ_BUDGET = 8 * 1024 * 1024
};

RecentRead s_recentReads[RECENT_READ_COUNT];
UnsignedInt s_recentReadUse = 0;
size_t s_recentReadBytes = 0;
pthread_mutex_t s_recentReadLock = PTHREAD_MUTEX_INITIALIZER;	///< files are opened by more than one thread

//-----------------------------------------------------------------
/// Copies a file that was read a moment ago. Returns FALSE if it was not.
Bool copyRecentRead(const AsciiString &archive, Int offset, Int size, Char *destination)
{
	Bool found = FALSE;

	pthread_mutex_lock(&s_recentReadLock);
	for (Int i = 0; i < RECENT_READ_COUNT && !found; ++i)
	{
		RecentRead &read = s_recentReads[i];
		if (read.data != nullptr && read.offset == offset && read.size == size && read.archive == archive)
		{
			memcpy(destination, read.data, size);
			read.used = ++s_recentReadUse;
			found = TRUE;
		}
	}
	pthread_mutex_unlock(&s_recentReadLock);

	return found;
}

//-----------------------------------------------------------------
void keepRecentRead(const AsciiString &archive, Int offset, Int size, const Char *data)
{
	if (size < RECENT_READ_MIN_BYTES || size > RECENT_READ_MAX_BYTES)
		return;

	Char *copy = static_cast<Char *>(malloc(size));
	if (copy == nullptr)
		return;
	memcpy(copy, data, size);

	pthread_mutex_lock(&s_recentReadLock);

	// Room for it: a free place, and the bytes.
	for (;;)
	{
		Int oldest = -1;
		Int unused = -1;
		for (Int i = 0; i < RECENT_READ_COUNT; ++i)
		{
			if (s_recentReads[i].data == nullptr)
				unused = i;
			else if (oldest < 0 || s_recentReads[i].used < s_recentReads[oldest].used)
				oldest = i;
		}

		if (oldest < 0 || (unused >= 0 && s_recentReadBytes + size <= RECENT_READ_BUDGET))
		{
			RecentRead &read = s_recentReads[unused >= 0 ? unused : 0];
			read.archive = archive;
			read.offset = offset;
			read.size = size;
			read.data = copy;
			read.used = ++s_recentReadUse;
			s_recentReadBytes += size;
			break;
		}

		RecentRead &gone = s_recentReads[oldest];
		s_recentReadBytes -= gone.size;
		free(gone.data);
		gone.data = nullptr;
	}

	pthread_mutex_unlock(&s_recentReadLock);
}

} // namespace
#endif // __vita__

Bool RAMFile::openFromArchive(File *archiveFile, const AsciiString& filename, Int offset, Int size)
{
	//USE_PERF_TIMER(RAMFile)
	if (archiveFile == nullptr) {
		return FALSE;
	}

	if (File::open(filename.str(), File::READ | File::BINARY) == FALSE) {
		return FALSE;
	}

	delete[] m_data;
	m_data = MSGNEW("RAMFILE") Char [size];	// pool[]ify
	m_size = size;

#if defined(__vita__)
	if (copyRecentRead(archiveFile->getName(), offset, size, m_data)) {
		m_nameStr = filename;
		return TRUE;
	}
#endif

	if (archiveFile->seek(offset, File::START) != offset) {
		return FALSE;
	}
	if (archiveFile->read(m_data, size) != size) {
		return FALSE;
	}
	m_nameStr = filename;

#if defined(__vita__)
	keepRecentRead(archiveFile->getName(), offset, size, m_data);
#endif

	return TRUE;
}

//=================================================================
// RAMFile::close
//=================================================================
/**
	* Closes the current file if it is open.
  * Must call RAMFile::close() for each successful RAMFile::open() call.
	*/
//=================================================================

void RAMFile::close()
{
	closeFile();
	File::close();
}

//=================================================================

void RAMFile::closeFile()
{
	delete [] m_data;
	m_data = nullptr;
}

//=================================================================
// RAMFile::read
//=================================================================
// if buffer is null, just advance the current position by 'bytes'
Int RAMFile::read( void *buffer, Int bytes )
{
	if( m_data == nullptr )
	{
		return -1;
	}

	Int bytesLeft = m_size - m_pos ;

	if ( bytes > bytesLeft )
	{
		bytes = bytesLeft;
	}

	if (( bytes > 0 ) && ( buffer != nullptr ))
	{
		memcpy ( buffer, &m_data[m_pos], bytes );
	}

	m_pos += bytes;

	return bytes;
}

//=================================================================
// RAMFile::readChar
//=================================================================

Int RAMFile::readChar()
{
	return EOF;
}

//=================================================================
// RAMFile::readWideChar
//=================================================================

Int RAMFile::readWideChar()
{
	return WEOF;
}

//=================================================================
// RAMFile::write
//=================================================================

Int RAMFile::write( const void *buffer, Int bytes )
{
	return -1;
}

//=================================================================
// RAMFile::writeFormat - Ascii
//=================================================================

Int RAMFile::writeFormat( const Char* format, ... )
{
	return -1;
}

//=================================================================
// RAMFile::writeFormat - Wide character
//=================================================================

Int RAMFile::writeFormat( const WideChar* format, ... )
{
	return -1;
}

//=================================================================
// RAMFile::writeChar - Ascii
//=================================================================

Int RAMFile::writeChar( const Char* character )
{
	return EOF;
}

//=================================================================
// RAMFile::writeChar - Wide character
//=================================================================

Int RAMFile::writeChar( const WideChar* character )
{
	return WEOF;
}

//=================================================================
// RAMFile::seek
//=================================================================

Int RAMFile::seek( Int pos, seekMode mode)
{
	Int newPos;

	switch( mode )
	{
		case START:
			newPos = pos;
			break;
		case CURRENT:
			newPos = m_pos + pos;
			break;
		case END:
			DEBUG_ASSERTCRASH(pos <= 0, ("RAMFile::seek - position should be <= 0 for a seek starting from the end."));
			newPos = m_size + pos;
			break;
		default:
			// bad seek mode
			return -1;
	}

	if ( newPos < 0 )
	{
		newPos = 0;
	}
	else if ( newPos > m_size )
	{
		newPos = m_size;
	}

	m_pos = newPos;

	return m_pos;

}

//=================================================================
// RAMFile::flush
//=================================================================

Bool RAMFile::flush()
{
	return false;
}

//=================================================================
// RAMFile::scanInt
//=================================================================
Bool RAMFile::scanInt(Int &newInt)
{
	newInt = 0;
	AsciiString tempstr;

	while ((m_pos < m_size) && ((m_data[m_pos] < '0') || (m_data[m_pos] > '9')) && (m_data[m_pos] != '-')) {
		++m_pos;
	}

	if (m_pos >= m_size) {
		m_pos = m_size;
		return FALSE;
	}

	do {
		tempstr.concat(m_data[m_pos]);
		++m_pos;
	} while ((m_pos < m_size) && ((m_data[m_pos] >= '0') && (m_data[m_pos] <= '9')));

//	if (m_pos < m_size) {
//		--m_pos;
//	}

	newInt = atoi(tempstr.str());
	return TRUE;
}

//=================================================================
// RAMFile::scanInt
//=================================================================
Bool RAMFile::scanReal(Real &newReal)
{
	newReal = 0.0;
	AsciiString tempstr;
	Bool sawDec = FALSE;

	while ((m_pos < m_size) && ((m_data[m_pos] < '0') || (m_data[m_pos] > '9')) && (m_data[m_pos] != '-') && (m_data[m_pos] != '.')) {
		++m_pos;
	}

	if (m_pos >= m_size) {
		m_pos = m_size;
		return FALSE;
	}

	do {
		tempstr.concat(m_data[m_pos]);
		if (m_data[m_pos] == '.') {
			sawDec = TRUE;
		}
		++m_pos;
	} while ((m_pos < m_size) && (((m_data[m_pos] >= '0') && (m_data[m_pos] <= '9')) || ((m_data[m_pos] == '.') && !sawDec)));

//	if (m_pos < m_size) {
//		--m_pos;
//	}

	newReal = atof(tempstr.str());
	return TRUE;
}

//=================================================================
// RAMFile::scanString
//=================================================================
Bool RAMFile::scanString(AsciiString &newString)
{
	newString.clear();

	while ((m_pos < m_size) && isspace(m_data[m_pos])) {
		++m_pos;
	}

	if (m_pos >= m_size) {
		m_pos = m_size;
		return FALSE;
	}

	do {
		newString.concat(m_data[m_pos]);
		++m_pos;
	} while ((m_pos < m_size) && (!isspace(m_data[m_pos])));

	return TRUE;
}

//=================================================================
// RAMFile::nextLine
//=================================================================
void RAMFile::nextLine(Char *buf, Int bufSize)
{
	Int i = 0;
	// seek to the next new-line character
	while ((m_pos < m_size) && (m_data[m_pos] != '\n')) {
		if ((buf != nullptr) && (i < (bufSize-1))) {
			buf[i] = m_data[m_pos];
			++i;
		}
		++m_pos;
	}

	// we got to the new-line character, now go one past it.
	if (m_pos < m_size) {
		if ((buf != nullptr) && (i < bufSize)) {
			buf[i] = m_data[m_pos];
			++i;
		}
		++m_pos;
	}
	if (buf != nullptr) {
		if (i < bufSize) {
			buf[i] = 0;
		} else {
			buf[bufSize] = 0;
		}
	}
	if (m_pos >= m_size) {
		m_pos = m_size;
	}
}

//=================================================================
// RAMFile::nextLine
//=================================================================
Bool RAMFile::copyDataToFile(File *localFile)
{
	if (localFile == nullptr) {
		return FALSE;
	}

	if (localFile->write(m_data, m_size) == m_size) {
		return TRUE;
	}


	return FALSE;
}

//=================================================================
//=================================================================
File* RAMFile::convertToRAMFile()
{
	return this;
}

//=================================================================
// RAMFile::readEntireAndClose
//=================================================================
/**
	Allocate a buffer large enough to hold entire file, read
	the entire file into the buffer, then close the file.
	the buffer is owned by the caller, who is responsible
	for freeing is (via delete[]). This is a Good Thing to
	use because it minimizes memory copies for BIG files.
*/
char* RAMFile::readEntireAndClose()
{

	if (m_data == nullptr)
	{
		DEBUG_CRASH(("m_data is null in RAMFile::readEntireAndClose -- should not happen!"));
		return NEW char[1];	// just to avoid crashing...
	}

	char* tmp = m_data;
	m_data = nullptr;	// will belong to our caller!

	close();

	return tmp;
}
