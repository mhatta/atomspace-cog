/*
 * FILE:
 * opencog/persist/cog-simple/CogSimpleStorage.cc
 *
 * FUNCTION:
 * Simple CogServer-backed persistent storage.
 *
 * HISTORY:
 * Copyright (c) 2020 Linas Vepstas <linasvepstas@gmail.com>
 *
 * LICENSE:
 * SPDX-License-Identifier: AGPL-3.0-or-later
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License v3 as
 * published by the Free Software Foundation and including the exceptions
 * at http://opencog.org/wiki/Licenses
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program; if not, write to:
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <errno.h>
#include <unistd.h>

static int unistd_close(int fd) { return close(fd); }

#include "CogSimpleStorage.h"

using namespace opencog;

/// This is a cheap, simple, super-low-brow atomspace server
/// built on the cogserver. Its not special. It's simple.
/// It is meant to be replaced by something better.

/* ================================================================ */
// Constructors

void CogSimpleStorage::init(const char * uri)
{
#define URIX_LEN (sizeof("cog://") - 1)  // Should be 6
	if (strncmp(uri, "cog://", URIX_LEN))
		throw IOException(TRACE_INFO, "Unknown URI '%s'\n", uri);
	_uri = uri;
}

CogSimpleStorage::CogSimpleStorage(std::string uri) :
	StorageNode(COG_SIMPLE_STORAGE_NODE, std::move(uri)),
	_sockfd(-1)
{
	init(_name.c_str());
}

CogSimpleStorage::~CogSimpleStorage()
{
	close();
}

void CogSimpleStorage::open(void)
{
	if (connected()) return;

	std::lock_guard<std::mutex> lck(_mtx);

	// We expect the URI to be for the form
	//    cog://ipv4-addr/atomspace-name
	//    cog://ipv4-addr:port/atomspace-name

	const char* uri = _uri.c_str();

	std::string host(uri + URIX_LEN);
	size_t slash = host.find_first_of(":/");
	if (std::string::npos != slash)
		host = host.substr(0, slash);

#define DEFAULT_COGSERVER_PORT "17001"
	std::string port = DEFAULT_COGSERVER_PORT;
	size_t colon = _uri.find(':', URIX_LEN + host.length());
	if (std::string::npos != colon)
	{
		port = _uri.substr(colon+1);
		slash = port.find('/');
		if (std::string::npos != slash)
			port = port.substr(0, slash);
	}

	struct addrinfo hints;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC; // IPv4 or IPv6
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	struct addrinfo *servinfo;
	int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &servinfo);
	if (rc)
		throw IOException(TRACE_INFO, "Unknown host %s: %s",
			host.c_str(), strerror(rc));

	_sockfd = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);

	if (0 > _sockfd)
	{
		int norr = errno;
		free(servinfo);
		throw IOException(TRACE_INFO, "Unable to create socket to host %s: %s",
			host.c_str(), strerror(norr));
	}

	rc = connect(_sockfd, servinfo->ai_addr, servinfo->ai_addrlen);
	if (0 > rc)
	{
		int norr = errno;
		free(servinfo);
		throw IOException(TRACE_INFO, "Unable to connect to host %s: %s",
			host.c_str(), strerror(norr));
	}
	free(servinfo);

	// We are going to be sending oceans of tiny packets,
	// and we want the fastest-possible responses.
	int flags = 1;
	rc = setsockopt(_sockfd, IPPROTO_TCP, TCP_NODELAY, &flags, sizeof(flags));
	if (0 > rc)
		fprintf(stderr, "Error setting sockopt: %s", strerror(errno));
#ifndef __APPLE__
	flags = 1;
	rc = setsockopt(_sockfd, IPPROTO_TCP, TCP_QUICKACK, &flags, sizeof(flags));
	if (0 > rc)
		fprintf(stderr, "Error setting sockopt: %s", strerror(errno));
#endif
	// Get the s-expression shell.
	std::string eval = "sexpr\n";

#if USE_GUILE_INSTEAD
	// Instead of using the s-expression shell, one can use the guile
	// shell. Everything should work just as well; two unit tests are
	// slower.

	// Get to the scheme prompt, but make it be silent.
	std::string eval = "scm hush\n";
#endif
	rc = send(_sockfd, eval.c_str(), eval.size(), 0);
	if (0 > rc)
		throw IOException(TRACE_INFO, "Unable to talk to cogserver at host %s: %s",
			host.c_str(), strerror(errno));

	// Throw away the cogserver prompt.
	do_recv();

#if USE_GUILE_INSTEAD
	// See above.
	do_send("(use-modules (opencog exec))\n");
	do_recv();
	do_send("(cog-set-server-mode! #t)\n");
	do_recv();
#endif
}

bool CogSimpleStorage::connected(void)
{
	return 0 < _sockfd;
}

void CogSimpleStorage::close(void)
{
	if (connected())
		unistd_close(_sockfd);
	_sockfd = -1;
}

/* ================================================================== */

void CogSimpleStorage::do_send(const std::string& str)
{
	if (not connected())
		throw IOException(TRACE_INFO, "Not connected to cogserver!");

	int rc = send(_sockfd, str.c_str(), str.size(), 0);
	if (0 > rc)
		throw IOException(TRACE_INFO, "Unable to talk to cogserver: %s",
			strerror(errno));
}

// If the argument `garbage` is set to true, then assume that
// the first read contains the CogServer prompt, which is maybe
// colorized, and is, in any case, not newine teminated.
std::string CogSimpleStorage::do_recv(bool garbage)
{
	if (not connected())
		throw IOException(TRACE_INFO, "Not connected to cogserver!");

	// The read strategy is as as folows:
	// messages are always terminated by a newline, with one exception:
	// Upon the initial connection to the CogServer, the server will
	// send it's default prompt. That prompt is not newline-terminated.
	// However, in that case, `garbage==true` and so we terminate the
	// read.
	std::string rb;
	bool first_time = true;
	while (true)
	{
		// Receive 4K bytes of message.
		char buf[4097];
		int len = recv(_sockfd, buf, 4096, 0);

		if (0 > len)
			throw IOException(TRACE_INFO, "Unable to talk to cogserver: %s",
				strerror(errno));

		if (0 == len)
		{
			unistd_close(_sockfd);
			_sockfd = 0;
			throw IOException(TRACE_INFO, "Cogserver unexpectedly closed connection");
		}
		buf[len] = 0;

		// Ignore solitary synchronous idle chars.
		// The CogServer sends these when it is congested
		// and is looking for half-open sockets.
		if (1 == len and 0x16 == buf[0])
			continue;

		// Normal short reads are either newline-terminated,
		// or are reads of the cogserver prompt, which are
		// blank-space terminated.
		if (first_time and len < 4096 and
		   (('\n' == buf[len-1]) or garbage))
		{
			return buf;
		}

		first_time = false;
		rb += buf;

		// newline-terminated strings mean we are done.
		if ('\n' == buf[len-1])
			return rb;
	}
	return rb;
}

/* ================================================================== */
/// Drain the pending store queue. This is a fencing operation; the
/// goal is to make sure that all writes that occurred before the
/// barrier really are performed before before all the writes after
/// the barrier.
///
void CogSimpleStorage::barrier(AtomSpace* as)
{
}

/* ================================================================ */

void CogSimpleStorage::clear_stats(void)
{
}

void CogSimpleStorage::print_stats(void)
{
	printf("Connected to %s\n", _uri.c_str());
	printf("no stats yet\n");
}

DEFINE_NODE_FACTORY(CogSimpleStorageNode, COG_SIMPLE_STORAGE_NODE)

/* ============================= END OF FILE ================= */
