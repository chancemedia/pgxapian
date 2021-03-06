/** @file closefrom.cc
 * @brief Implementation of closefrom() function.
 */
/* Copyright (C) 2010 Olly Betts
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <config.h>

#ifndef HAVE_CLOSEFROM

#include "closefrom.h"

#include "safeerrno.h"
#include "safefcntl.h"
#include "safeunistd.h"

#if defined __linux__ || defined __APPLE__
# include "safedirent.h"
# include <cstdlib>

using namespace std;
#endif

void
Xapian::Internal::closefrom(int fd)
{
#ifdef F_CLOSEM
    // Apparently supported by at least NetBSD, AIX, IRIX.
    if (fcntl(fd, F_CLOSEM, 0) >= 0)
	return;
#elif defined __linux__ || defined __APPLE__
    // The loop might close the fd associated with dir if we don't take
    // special care to avoid that by either skipping this fd in the closing
    // loop (if dirfd() is available) or making sure we have a free fd below
    // the first we close in the loop.
#if !defined HAVE_DIRFD && !defined dirfd
    // Make sure that the lowest fd we have been asked to close is closed, and
    // then raise this lower bound - this should ensure that opendir() gets
    // an fd below the new lower bound.
    while (close(fd) < 0 && errno == EINTR) { }
    ++fd;
#endif
#if 0
    // Some platforms (e.g. AIX) have /proc/<pid>/fd but not /proc/self - if
    // any such platforms don't have either closefrom() or F_CLOSEM then this
    // code can be used.
    string path = "/proc/";
    path += str(getpid());
    path += "/fd";
    DIR * dir = opendir(path.c_str());
#elif defined __linux__
    DIR * dir = opendir("/proc/self/fd");
#elif defined __APPLE__ // Mac OS X
    DIR * dir = opendir("/dev/fd");
#endif
    if (dir) {
	while (true) {
	    errno = 0;
	    struct dirent *entry = readdir(dir);
	    if (entry == NULL) {
		closedir(dir);
		// Fallback if readdir() or closedir() fails.
		if (errno) break;
		return;
	    }
	    char ch;
	    ch = entry->d_name[0];
	    if (ch < '0' || ch > '9')
		continue;
	    int n = atoi(entry->d_name);
	    if (n >= fd) {
#if defined HAVE_DIRFD || defined dirfd
		if (n == dirfd(dir)) continue;
#endif
		// Retry on EINTR.
		while (close(n) < 0 && errno == EINTR) { }
	    }
	}
    }
#endif
    int maxfd = -1;
#ifdef F_MAXFD
    maxfd = fcntl(0, F_MAXFD);
#endif
    if (maxfd < 0)
	maxfd = static_cast<int>(sysconf(_SC_OPEN_MAX));
    while (fd < maxfd) {
	// Retry on EINTR; just ignore other errors (we'll get EBADF if fd
	// isn't open so that's OK).
	while (close(fd) < 0 && errno == EINTR) { }
	++fd;
    }
}

#endif
