/*
	stcpcli.c:	BP TCP-based convergence-layer input
			daemon, designed to serve as an input
			duct.

	Author: Scott Burleigh, JPL

	Copyright (c) 2004, California Institute of Technology.
	ALL RIGHTS RESERVED.  U.S. Government Sponsorship
	acknowledged.
	
									*/
#include "stcpcla.h"

static ReqAttendant	*_attendant(ReqAttendant *newAttendant)
{
	static ReqAttendant	*attendant = NULL;

	if (newAttendant)
	{
		attendant = newAttendant;
	}

	return attendant;
}

static void	handleStopThread()
{
	isignal(SIGINT, handleStopThread);
}

static void	handleStopStcpcli()
{
	isignal(SIGTERM, handleStopStcpcli);
	ionKillMainThread("stcpcli");
}

/*	*	*	Receiver thread functions	*	*	*/

typedef struct
{
	VInduct		*vduct;
	LystElt		elt;
	pthread_mutex_t	*mutex;
	int		bundleSocket;
	pthread_t	thread;
	int		*running;
} ReceiverThreadParms;

static void	terminateReceiverThread(ReceiverThreadParms *parms)
{
	writeErrmsgMemos();
	writeMemo("[i] stcpcli receiver thread stopping.");
	pthread_mutex_lock(parms->mutex);
	if (parms->bundleSocket != -1)
	{
		closesocket(parms->bundleSocket);
		parms->bundleSocket = -1;
	}

	lyst_delete(parms->elt);
	pthread_mutex_unlock(parms->mutex);
	MRELEASE(parms);
}

static void	*receiveBundles(void *parm)
{
	/*	Main loop for bundle reception thread on one
	 *	connection, terminating when connection is lost.	*/

	ReceiverThreadParms	*parms = (ReceiverThreadParms *) parm;
	char			*procName = "stcpcli";
	AcqWorkArea		*work;
	char			*buffer;
	int			threadRunning = 1;

	work = bpGetAcqArea(parms->vduct);
	if (work == NULL)
	{
		putErrmsg("stcpcli can't get acquisition work area.", NULL);
		ionKillMainThread(procName);
		terminateReceiverThread(parms);
		return NULL;
	}

	buffer = MTAKE(STCPCLA_BUFSZ);
	if (buffer == NULL)
	{
		putErrmsg("stcpcli can't get TCP buffer.", NULL);
		ionKillMainThread(procName);
		terminateReceiverThread(parms);
		return NULL;
	}

	/*	Now start receiving bundles.				*/

	while (threadRunning && *(parms->running))
	{
		if (bpBeginAcq(work, 0, NULL) < 0)
		{
			putErrmsg("Can't begin acquisition of bundle.", NULL);
			ionKillMainThread(procName);
			threadRunning = 0;
			continue;
		}

		switch (receiveBundleByStcp(&(parms->bundleSocket), work,
				buffer, _attendant(NULL)))
		{
		case -1:
			putErrmsg("Can't acquire bundle.", NULL);

			/*	Intentional fall-through to next case.	*/

		case 0:				/*	Normal stop.	*/
			threadRunning = 0;
			continue;

		default:
			break;			/*	Out of switch.	*/
		}

		if (bpEndAcq(work) < 0)
		{
			putErrmsg("Can't end acquisition of bundle.", NULL);
			ionKillMainThread(procName);
			threadRunning = 0;
			continue;
		}

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	/*	End of receiver thread; release resources.		*/

	bpReleaseAcqArea(work);
	MRELEASE(buffer);
	terminateReceiverThread(parms);
	return NULL;
}

/*	*	*	Access thread functions	*	*	*	*/

typedef struct
{
	VInduct			*vduct;
	struct sockaddr_storage		socketName;
	struct sockaddr_in	*inetName;
    struct sockaddr_in6 *inet6Name;
    int         domain;
	int			ductSocket;
	int			running;
} AccessThreadParms;

static void	*spawnReceivers(void *parm)
{
	/*	Main loop for acceptance of connections and
	 *	creation of receivers to service those connections.	*/

	AccessThreadParms	*atp = (AccessThreadParms *) parm;
	char			*procName = "stcpcli";
	pthread_mutex_t		mutex;
	Lyst			threads;
	int			newSocket;
	struct sockaddr_storage		cloSocketName;
	socklen_t		nameLength;
	ReceiverThreadParms	*parms;
	LystElt			elt;
	pthread_t		thread;

	snooze(1);	/*	Let main thread become interruptable.	*/
	pthread_mutex_init(&mutex, NULL);
	threads = lyst_create_using(getIonMemoryMgr());
	if (threads == NULL)
	{
		putErrmsg("stcpcli can't create threads list.", NULL);
		ionKillMainThread(procName);
		pthread_mutex_destroy(&mutex);
		return NULL;
	}

	/*	Can now begin accepting connections from remote
	 *	contacts.  On failure, take down the whole CLI.		*/

	while (atp->running)
	{
		nameLength = sizeof(struct sockaddr_storage);
		newSocket = accept(atp->ductSocket, (struct sockaddr *) &cloSocketName,
				&nameLength);
		if (newSocket < 0)
		{
			putSysErrmsg("stcpcli accept() failed", NULL);
			ionKillMainThread(procName);
			atp->running = 0;
			continue;
		}

		if (atp->running == 0)
		{
			closesocket(newSocket);
			break;	/*	Main thread has shut down.	*/
		}

		parms = (ReceiverThreadParms *)
				MTAKE(sizeof(ReceiverThreadParms));
		if (parms == NULL)
		{
			putErrmsg("stcpcli can't allocate for thread.", NULL);
			closesocket(newSocket);
			ionKillMainThread(procName);
			atp->running = 0;
			continue;
		}

		parms->vduct = atp->vduct;
		pthread_mutex_lock(&mutex);
		parms->elt = lyst_insert_last(threads, parms);
		pthread_mutex_unlock(&mutex);
		if (parms->elt == NULL)
		{
			putErrmsg("stcpcli can't allocate for thread.", NULL);
			MRELEASE(parms);
			closesocket(newSocket);
			ionKillMainThread(procName);
			atp->running = 0;
			continue;
		}

		parms->mutex = &mutex;
		parms->bundleSocket = newSocket;
		parms->running = &(atp->running);
		if (pthread_begin(&(parms->thread), NULL, receiveBundles,
					parms))
		{
			putSysErrmsg("stcpcli can't create new thread", NULL);
			MRELEASE(parms);
			closesocket(newSocket);
			ionKillMainThread(procName);
			atp->running = 0;
			continue;
		}

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	closesocket(atp->ductSocket);
	writeErrmsgMemos();

	/*	Shut down all current CLI threads cleanly.		*/

	while (1)
	{
		pthread_mutex_lock(&mutex);
		elt = lyst_first(threads);
		if (elt == NULL)	/*	All threads shut down.	*/
		{
			pthread_mutex_unlock(&mutex);
			break;
		}

		/*	Trigger termination of thread.			*/

		parms = (ReceiverThreadParms *) lyst_data(elt);
		thread = parms->thread;
#ifdef mingw
		shutdown(parms->bundleSocket, SD_BOTH);
#else
		pthread_kill(thread, SIGINT);
#endif
		pthread_mutex_unlock(&mutex);
		pthread_join(thread, NULL);
	}

	lyst_destroy(threads);
	writeErrmsgMemos();
	writeMemo("[i] stcpcli access thread has ended.");
	pthread_mutex_destroy(&mutex);
	return NULL;
}

/*	*	*	Main thread functions	*	*	*	*/

#if defined (ION_LWT)
int	stcpcli(int a1, int a2, int a3, int a4, int a5,
		int a6, int a7, int a8, int a9, int a10)
{
	char	*ductName = (char *) a1;
#else
int	main(int argc, char *argv[])
{
	char	*ductName = (argc > 1 ? argv[1] : NULL);
#endif
	VInduct			*vduct;
	PsmAddress		vductElt;
	Sdr			sdr;
	Induct			duct;
	ClProtocol		protocol;
	char			*hostName;
	unsigned short		portNbr;
	unsigned int		hostNbr;
    unsigned char       hostAddr[sizeof(struct in6_addr)];
	AccessThreadParms	atp;
	socklen_t		nameLength;
	ReqAttendant		attendant;
	pthread_t		accessThread;
	int			fd;

	if (ductName == NULL)
	{
		PUTS("Usage: stcpcli <local host name>[:<port number>]");
		return 0;
	}

	if (bpAttach() < 0)
	{
		putErrmsg("stcpcli can't attach to BP.", NULL);
		return -1;
	}

	findInduct("stcp", ductName, &vduct, &vductElt);
	if (vductElt == 0)
	{
		putErrmsg("No such stcp duct.", ductName);
		return -1;
	}

	if (vduct->cliPid != ERROR && vduct->cliPid != sm_TaskIdSelf())
	{
		putErrmsg("CLI task is already started for this duct.",
				itoa(vduct->cliPid));
		return -1;
	}

	/*	All command-line arguments are now validated.		*/

	sdr = getIonsdr();
	CHKERR(sdr_begin_xn(sdr));
	sdr_read(sdr, (char *) &duct, sdr_list_data(sdr, vduct->inductElt),
			sizeof(Induct));
	sdr_read(sdr, (char *) &protocol, duct.protocol, sizeof(ClProtocol));
	sdr_exit_xn(sdr);
	hostName = ductName;
	if ((atp.domain = parseSocketSpec(ductName, &portNbr, hostAddr)) < 0)
	{
		putErrmsg("Can't get IP/port for host.", hostName);
		return -1;
	}

	if (portNbr == 0)
	{
		portNbr = BpStcpDefaultPortNbr;
	}

	portNbr = htons(portNbr);
	atp.vduct = vduct;
	memset((char *) &(atp.socketName), 0, sizeof(struct sockaddr));
    if (atp.domain == AF_INET)
    {
        atp.inetName = (struct sockaddr_in *) &(atp.socketName);
        atp.inetName->sin_family = AF_INET;
        atp.inetName->sin_port = portNbr;
        memcpy((char *) &(atp.inetName->sin_addr.s_addr), (char *) hostAddr, 4);
        atp.ductSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    }
    else if (atp.domain == AF_INET6)
    {
        atp.inet6Name = (struct sockaddr_in6 *) &(atp.socketName);
        atp.inet6Name->sin_6family = AF_INET6;
        atp.inet6Name->sin6_port = portNbr;
        memcpy((char *) &(atp.inet6Name->sin6_addr.s6_addr), (char *) hostAddr, 16);
        atp.ductSocket = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    }
	if (atp.ductSocket < 0)
	{
		putSysErrmsg("Can't open TCP socket", NULL);
		return 1;
	}

	nameLength = sizeof(struct sockaddr_storage);
	if (reUseAddress(atp.ductSocket)
	|| bind(atp.ductSocket, (struct sockaddr *) &(atp.socketName), nameLength) < 0
	|| listen(atp.ductSocket, 5) < 0
	|| getsockname(atp.ductSocket, (struct sockaddr *) &(atp.socketName), &nameLength) < 0)
	{
		closesocket(atp.ductSocket);
		putSysErrmsg("Can't initialize socket", NULL);
		return 1;
	}

	/*	Set up blocking acquisition of data via TCP.		*/

	if (ionStartAttendant(&attendant) < 0)
	{
		closesocket(atp.ductSocket);
		putErrmsg("Can't initialize blocking TCP reception.", NULL);
		return 1;
	}

	oK(_attendant(&attendant));

	/*	Set up signal handling: SIGTERM is shutdown signal.	*/

	ionNoteMainThread("stcpcli");
#ifndef mingw
	isignal(SIGINT, handleStopThread);
#endif
	isignal(SIGTERM, handleStopStcpcli);

	/*	Start the access thread.				*/

	atp.running = 1;
	if (pthread_begin(&accessThread, NULL, spawnReceivers, &atp))
	{
		closesocket(atp.ductSocket);
		putSysErrmsg("stcpcli can't create access thread", NULL);
		return 1;
	}

	/*	Now sleep until interrupted by SIGTERM, at which point
	 *	it's time to stop the induct.				*/
	{
		char    txt[500];

		if (domain == AF_INET)
		{
			isprintf(txt, sizeof(txt),
				"[i] stcpcli is running, spec=[%s:%d].", 
				inet_ntoa(atp.inetName->sin_addr), ntohs(portNbr));
			writeMemo(txt);
		}
		else if (domain == AF_INET6)
		{
			char hostStr[INET6_ADDRSTRLEN];
			inet_ntop(atp.domain, hostAddr, hostStr, INET6_ADDRSTRLEN);

			isprintf(txt, sizeof(txt),
				"[i] stcpcli is running, spec=[%s:%d].", 
				hostStr, ntohs(portNbr));
			writeMemo(txt);
		}
	}

	ionPauseMainThread(-1);

	/*	Time to shut down.					*/

	atp.running = 0;
	ionPauseAttendant(&attendant);

	/*	Wake up the access thread by connecting to it.		*/

	fd = socket(atp.domain, SOCK_STREAM, IPPROTO_TCP);
	if (fd >= 0)
	{
		oK(connect(fd, (struct sockaddr *) &(atp.socketName), sizeof(struct sockaddr_storage)));

		/*	Immediately discard the connected socket.	*/

		closesocket(fd);
	}

	pthread_join(accessThread, NULL);
	ionStopAttendant(&attendant);
	writeErrmsgMemos();
	writeMemo("[i] stcpcli duct has ended.");
	ionDetach();
	return 0;
}
