/*
 *	udpbso.c:	BSSP UDP-based link service output daemon.
 *			Dedicated to UDP datagrams transmission to 
 *			a single remote BSSP engine.
 *
 *	Authors: Sotirios-Angelos Lenas, SPICE
 *		 Scott Burleigh, JPL
 *
 *	Copyright (c) 2013, California Institute of Technology.
 *	Copyright (c) 2013, Space Internetworking Center,
 *	Democritus University of Thrace.
 *	
 *	All rights reserved. U.S. Government and E.U. Sponsorship acknowledged.
 *
 */

#include "udpbsa.h"

#if defined(linux)

#define IPHDR_SIZE	(sizeof(struct iphdr) + sizeof(struct udphdr))

#elif defined(mingw)

#define IPHDR_SIZE	(20 + 8)

#else

#include "netinet/ip_var.h"
#include "netinet/udp_var.h"

#define IPHDR_SIZE	(sizeof(struct udpiphdr))

#endif

static sm_SemId		udpbsoSemaphore(sm_SemId *semid)
{
	static sm_SemId	semaphore = -1;
	
	if (semid)
	{
		semaphore = *semid;
	}

	return semaphore;
}

static void	shutDownBso()	/*	Commands LSO termination.	*/
{
	sm_SemEnd(udpbsoSemaphore(NULL));
}

/*	*	*	Receiver thread functions	*	*	*/

typedef struct
{
	int		linkSocket;
	int		running;
} ReceiverThreadParms;

static void	*handleDatagrams(void *parm)
{
	/*	Main loop for UDP datagram reception and handling.	*/

	ReceiverThreadParms	*rtp = (ReceiverThreadParms *) parm;
	char			*buffer;
	int			blockLength;
	struct sockaddr_in	fromAddr;
	socklen_t		fromSize;

	buffer = MTAKE(UDPBSA_BUFSZ);
	if (buffer == NULL)
	{
		putErrmsg("udpbsi can't get UDP buffer.", NULL);
		shutDownBso();
		return NULL;
	}
	/*	Can now start receiving bundles.  On failure, take
	 *	down the BSO.						*/

	iblock(SIGTERM);

	while (rtp->running)
	{	
		fromSize = sizeof fromAddr;
		blockLength = irecvfrom(rtp->linkSocket, buffer, UDPBSA_BUFSZ,
				0, (struct sockaddr *) &fromAddr, &fromSize);
		switch (blockLength)
		{
		case -1:
			putSysErrmsg("Can't acquire block", NULL);
			shutDownBso();

			/*	Intentional fall-through to next case.	*/

		case 1:				/*	Normal stop.	*/
			rtp->running = 0;
			continue;
		}
		if (bsspHandleInboundBlock(buffer, blockLength) < 0)
		{
			putErrmsg("Can't handle inbound block.", NULL);
			shutDownBso();
			rtp->running = 0;
			continue;
		}

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	writeErrmsgMemos();
	writeMemo("[i] udpbso receiver thread has ended.");

	/*	Free resources.						*/

	MRELEASE(buffer);
	return NULL;
}

/*	*	*	Main thread functions	*	*	*	*/

int	sendBlockByUDP(int linkSocket, char *from, int length,
		struct sockaddr_in *destAddr )
{
	int	bytesWritten;

	while (1)	/*	Continue until not interrupted.		*/
	{
		bytesWritten = isendto(linkSocket, from, length, 0,
				(struct sockaddr *) destAddr,
				sizeof(struct sockaddr));
#ifdef MULTIDUCTS_DEBUG
		if (bytesWritten > 0)
		{
			printf("udpbso sent %d bytes.\n", bytesWritten);
		}
#endif
		if (bytesWritten < 0)
		{
			if (errno == EINTR)	/*	Interrupted.	*/
			{
				continue;	/*	Retry.		*/
			}

			{
				char			memoBuf[1000];
				struct sockaddr_in	*saddr = destAddr;

				isprintf(memoBuf, sizeof(memoBuf),
					"udpbso sendto() error, dest=[%s:%d], \
nbytes=%d, rv=%d, errno=%d", (char *) inet_ntoa(saddr->sin_addr), 
					ntohs(saddr->sin_port), 
					length, bytesWritten, errno);
				writeMemo(memoBuf);
			}
		}
		return bytesWritten;
	}
}

#if defined (ION_LWT)
int	udpbso(saddr a1, saddr a2, saddr a3, saddr a4, saddr a5,
	       saddr a6, saddr a7, saddr a8, saddr a9, saddr a10)
{
	char		*endpointSpec = (char *) a1;
	unsigned int	txbps = (a2 != 0 ?  strtoul((char *) a2, NULL, 0) : 0);
	uvast		remoteEngineId = a3 != 0 ?  strtouvast((char *) a3) : 0;
#else
int	main(int argc, char *argv[])
{
	char		*endpointSpec = argc > 1 ? argv[1] : NULL;
	unsigned int	txbps = (argc > 2 ?  strtoul(argv[2], NULL, 0) : 0);
	uvast		remoteEngineId = argc > 3 ? strtouvast(argv[3]) : 0;
#endif
	Sdr			sdr;
	BsspVspan		*vspan;
	PsmAddress		vspanElt;
	unsigned short		portNbr = 0;
	unsigned int		ipAddress = 0;
	char			ownHostName[MAXHOSTNAMELEN];
	struct sockaddr		ownSockName;
	struct sockaddr_in	*ownInetName;
	struct sockaddr		bindSockName;
	struct sockaddr_in	*bindInetName;
	struct sockaddr		peerSockName;
	struct sockaddr_in	*peerInetName;
	socklen_t		nameLength;
	ReceiverThreadParms	rtp;
	pthread_t		receiverThread;
	int			blockLength;
	char			*block;
	int			bytesSent;
	float			sleepSecPerBit = 0;
	float			sleep_secs;
	unsigned int		usecs;
	int			fd;
	char			quit = '\0';

	if( txbps != 0 && remoteEngineId == 0 )
	{
		remoteEngineId = txbps;
		txbps = 0;
	}

	if (remoteEngineId == 0 || endpointSpec == NULL)
	{
		PUTS("Usage: udpbso {<remote engine's host name> | @}[:\
		<its port number>] <txbps (0=unlimited)> <remote engine ID>");
		return 0;
	}

	/*	Note that bsspadmin must be run before the first
	 *	invocation of bsspbso, to initialize the BSSP database
	 *	(as necessary) and dynamic database.			*/

	if (bsspInit(0) < 0)
	{
		putErrmsg("udpbso can't initialize BSSP.", NULL);
		return 1;
	}

	sdr = getIonsdr();
	CHKZERO(sdr_begin_xn(sdr));	/*	Just to lock memory.	*/
	findBsspSpan(remoteEngineId, &vspan, &vspanElt);
	if (vspanElt == 0)
	{
		sdr_exit_xn(sdr);
		putErrmsg("No such engine in database.", itoa(remoteEngineId));
		return 1;
	}

	if (vspan->bsoBEPid != ERROR && vspan->bsoBEPid != sm_TaskIdSelf())
	{
		sdr_exit_xn(sdr);
		putErrmsg("BE-BSO task is already started for this span.",
				itoa(vspan->bsoBEPid));
		return 1;
	}

	sdr_exit_xn(sdr);

	/*	All command-line arguments are now validated.  First
	 *	get peer's socket address.				*/

	parseSocketSpec(endpointSpec, &portNbr, &ipAddress);
	if (portNbr == 0)
	{
		portNbr = BsspUdpDefaultPortNbr;
	}

	getNameOfHost(ownHostName, sizeof ownHostName);
	if (ipAddress == 0)		/*	Default to local host.	*/
	{
		ipAddress = getInternetAddress(ownHostName);
	}

	portNbr = htons(portNbr);
	ipAddress = htonl(ipAddress);
	memset((char *) &peerSockName, 0, sizeof peerSockName);
	peerInetName = (struct sockaddr_in *) &peerSockName;
	peerInetName->sin_family = AF_INET;
	peerInetName->sin_port = portNbr;
	memcpy((char *) &(peerInetName->sin_addr.s_addr),
			(char *) &ipAddress, 4);

	/*	Now compute own socket address, used when the peer
	 *	responds to the link service output socket rather
	 *	than to the advertised link service input socket.	*/

	ipAddress = htonl(INADDR_ANY);
	memset((char *) &bindSockName, 0, sizeof bindSockName);
	bindInetName = (struct sockaddr_in *) &bindSockName;
	bindInetName->sin_family = AF_INET;
	bindInetName->sin_port = 0;	/*	Let O/S select it.	*/
	memcpy((char *) &(bindInetName->sin_addr.s_addr),
			(char *) &ipAddress, 4);

	/*	Now create the socket that will be used for sending
	 *	datagrams to the peer BSSP engine and receiving
	 *	datagrams from the peer BSSP engine.			*/

	rtp.linkSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (rtp.linkSocket < 0)
	{
		putSysErrmsg("BE-BSO can't open UDP socket", NULL);
		return 1;
	}

	/*	Bind the socket to own socket address so that we can
	 *	send a 1-byte datagram to that address to shut down
	 *	the datagram handling thread.				*/

	nameLength = sizeof(struct sockaddr);
	if (bind(rtp.linkSocket, &bindSockName, nameLength) < 0
	|| getsockname(rtp.linkSocket, &bindSockName, &nameLength) < 0)
	{
		closesocket(rtp.linkSocket);
		putSysErrmsg("BE-BSO can't bind UDP socket", NULL);
		return 1;
	}

	/*	Set up signal handling.  SIGTERM is shutdown signal.	*/

	oK(udpbsoSemaphore(&(vspan->beSemaphore)));
	signal(SIGTERM, shutDownBso);

	/*	Start the echo handler thread.				*/

	rtp.running = 1;
	if (pthread_begin(&receiverThread, NULL, handleDatagrams, &rtp))
	{
		closesocket(rtp.linkSocket);
		putSysErrmsg("udpbsi can't create receiver thread", NULL);
		return 1;
	}

	/*	Can now begin transmitting to remote engine.		*/

	{
		char	memoBuf[1024];

		isprintf(memoBuf, sizeof(memoBuf),
			"[i] udpbso is running, spec=[%s:%d], txbps=%d \
(0=unlimited), rengine=%d.", (char *) inet_ntoa(peerInetName->sin_addr),
			ntohs(portNbr), txbps, (int) remoteEngineId);
		writeMemo(memoBuf);
	}

	if (txbps)
	{
		sleepSecPerBit = 1.0 / txbps;
	}

	while (rtp.running && !(sm_SemEnded(vspan->beSemaphore)))
	{
		blockLength = bsspDequeueBEOutboundBlock(vspan, &block);
		if (blockLength < 0)
		{
			rtp.running = 0;	/*	Terminate LSO.	*/
			continue;
		}

		if (blockLength == 0)		/*	Interrupted.	*/
		{
			continue;
		}

		if (blockLength > UDPBSA_BUFSZ)
		{
			putErrmsg("Block is too big for UDP BSO.",
					itoa(blockLength));
			rtp.running = 0;	/*	Terminate LSO.	*/
		}
		else
		{
////////////////////// 5byte header (HY)
                        char * tmp = (char *) malloc (blockLength + 5);		//2043 + 5

			tmp[0] = 0xAA;				// Sync
                        tmp[1] = (blockLength >> 8) & 0xFF;	// Length
                        tmp[2] = blockLength;			// Length
			tmp[3] = 0x86;				// Command
			tmp[4] = 0x00;				// Pkt no
						
                        memcpy(&tmp[5], block, blockLength);
                        int totalLength = blockLength + 5;

			bytesSent = sendBlockByUDP(rtp.linkSocket, tmp,
					totalLength, peerInetName);		// block -> tmp, blockLength -> totalLength (HY)
//////////////////////
			if (bytesSent < totalLength)	// blockLength -> totalLength (HY)
			{
				rtp.running = 0;/*	Terminate BSO.	*/
			}

			if (txbps)
			{
				sleep_secs = sleepSecPerBit
					* ((IPHDR_SIZE + blockLength) * 8);
				usecs = sleep_secs * 1000000.0;
				if (usecs == 0)
				{
					usecs = 1;
				}

				microsnooze(usecs);
			}
			free(tmp);	// HY
		}

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	/*	Create one-use socket for the closing quit byte.	*/

	portNbr = bindInetName->sin_port;	/*	From binding.	*/
	ipAddress = getInternetAddress(ownHostName);
	ipAddress = htonl(ipAddress);
	memset((char *) &ownSockName, 0, sizeof ownSockName);
	ownInetName = (struct sockaddr_in *) &ownSockName;
	ownInetName->sin_family = AF_INET;
	ownInetName->sin_port = portNbr;
	memcpy((char *) &(ownInetName->sin_addr.s_addr),
			(char *) &ipAddress, 4);

	/*	Wake up the receiver thread by sending it a 1-byte
	 *	datagram.						*/

	fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd >= 0)
	{
		isendto(fd, &quit, 1, 0, &ownSockName, sizeof(struct sockaddr));
		closesocket(fd);
	}

	pthread_join(receiverThread, NULL);
	closesocket(rtp.linkSocket);
	writeErrmsgMemos();
	writeMemo("[i] udpbso has ended.");
	return 0;
}
