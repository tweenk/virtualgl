// Copyright (C)2004 Landmark Graphics Corporation
// Copyright (C)2005, 2006 Sun Microsystems, Inc.
// Copyright (C)2009-2011, 2014, 2018-2019 D. R. Commander
//
// This library is free software and may be redistributed and/or modified under
// the terms of the wxWindows Library License, Version 3.1 or (at your option)
// any later version.  The full license is in the LICENSE.txt file included
// with this distribution.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// wxWindows Library License for more details.

#include "VGLTransReceiver.h"
#include "vglutil.h"

using namespace vglutil;
using namespace vglcommon;
using namespace vglclient;


extern Display *maindpy;


static unsigned short DisplayNumber(Display *dpy)
{
	int dpynum = 0;  char *ptr = NULL;

	if((ptr = strchr(DisplayString(dpy), ':')) != NULL)
	{
		if(strlen(ptr) > 1) dpynum = atoi(ptr + 1);
		if(dpynum < 0 || dpynum > 65535) dpynum = 0;
	}
	return (unsigned short)dpynum;
}


#define ENDIANIZE(h) \
{ \
	if(!LittleEndian()) \
	{ \
		h.size = BYTESWAP(h.size); \
		h.winid = BYTESWAP(h.winid); \
		h.framew = BYTESWAP16(h.framew); \
		h.frameh = BYTESWAP16(h.frameh); \
		h.width = BYTESWAP16(h.width); \
		h.height = BYTESWAP16(h.height); \
		h.x = BYTESWAP16(h.x); \
		h.y = BYTESWAP16(h.y); \
		h.dpynum = BYTESWAP16(h.dpynum); \
	} \
}

#define ENDIANIZE_V1(h) \
{ \
	if(!LittleEndian()) \
	{ \
		h.size = BYTESWAP(h.size); \
		h.winid = BYTESWAP(h.winid); \
		h.framew = BYTESWAP16(h.framew); \
		h.frameh = BYTESWAP16(h.frameh); \
		h.width = BYTESWAP16(h.width); \
		h.height = BYTESWAP16(h.height); \
		h.x = BYTESWAP16(h.x); \
		h.y = BYTESWAP16(h.y); \
	} \
}

#define CONVERT_HEADER(h1, h) \
{ \
	h.size = h1.size; \
	h.winid = h1.winid; \
	h.framew = h1.framew; \
	h.frameh = h1.frameh; \
	h.width = h1.width; \
	h.height = h1.height; \
	h.x = h1.x; \
	h.y = h1.y; \
	h.qual = h1.qual; \
	h.subsamp = h1.subsamp; \
	h.flags = h1.flags; \
	h.dpynum = (unsigned short)h1.dpynum; \
}


VGLTransReceiver::VGLTransReceiver(bool doSSL_, bool ipv6_, int drawMethod_) :
	drawMethod(drawMethod_), listenSocket(NULL), thread(NULL), deadYet(false),
	doSSL(doSSL_), ipv6(ipv6_)
{
	char *env = NULL;

	if((env = getenv("VGL_VERBOSE")) != NULL && strlen(env) > 0
		&& !strncmp(env, "1", 1)) fbx_printwarnings(vglout.getFile());
	NEWCHECK(thread = new Thread(this));
}


VGLTransReceiver::~VGLTransReceiver(void)
{
	deadYet = true;
	listenMutex.lock();
	if(listenSocket) listenSocket->close();
	listenMutex.unlock();
	if(thread) { thread->stop();  thread = NULL; }
}


void VGLTransReceiver::listen(unsigned short port_)
{
	try
	{
		NEWCHECK(listenSocket = new Socket(doSSL, ipv6));
		port = listenSocket->listen(port_);
	}
	catch(...)
	{
		if(listenSocket) { delete listenSocket;  listenSocket = NULL; }
		throw;
	}
	thread->start();
}


void VGLTransReceiver::run(void)
{
	Socket *socket = NULL;  Listener *listener = NULL;

	while(!deadYet)
	{
		try
		{
			listener = NULL;  socket = NULL;
			socket = listenSocket->accept();  if(deadYet) break;
			vglout.println("++ %sConnection from %s.", doSSL ? "SSL " : "",
				socket->remoteName());
			NEWCHECK(listener = new Listener(socket, drawMethod));
			continue;
		}
		catch(Error &e)
		{
			if(!deadYet)
			{
				vglout.println("%s-- %s", e.getMethod(), e.getMessage());
				if(listener) delete listener;
				if(socket) delete socket;
				continue;
			}
		}
	}
	vglout.println("Listener exiting ...");
	listenMutex.lock();
	if(listenSocket) { delete listenSocket;  listenSocket = NULL; }
	listenMutex.unlock();
}


void VGLTransReceiver::Listener::run(void)
{
	ClientWin *w = NULL;
	Frame *f = NULL;
	rrframeheader h;  rrframeheader_v1 h1;  bool haveHeader = false;
	rrversion v;

	try
	{
		recv((char *)&h1, sizeof_rrframeheader_v1);
		ENDIANIZE(h1);
		if(h1.framew != 0 && h1.frameh != 0 && h1.width != 0 && h1.height != 0
			&& h1.winid != 0 && h1.size != 0 && h1.flags != RR_EOF)
		{
			v.major = 1;  v.minor = 0;  haveHeader = true;
		}
		else
		{
			memcpy(v.id, "VGL", 3);
			v.major = RR_MAJOR_VERSION;  v.minor = RR_MINOR_VERSION;
			send((char *)&v, sizeof_rrversion);
			recv((char *)&v, sizeof_rrversion);
			if(strncmp(v.id, "VGL", 3) || v.major < 1)
				THROW("Error reading server version");
		}

		char *env = NULL;
		if((env = getenv("VGL_VERBOSE")) != NULL && strlen(env) > 0
			&& !strncmp(env, "1", 1))
			vglout.println("Server version: %d.%d", v.major, v.minor);
		vglout.flush();

		while(1)
		{
			do
			{
				if(v.major == 1 && v.minor == 0)
				{
					if(!haveHeader)
					{
						recv((char *)&h1, sizeof_rrframeheader_v1);
						ENDIANIZE_V1(h1);
					}
					haveHeader = false;
					CONVERT_HEADER(h1, h);
				}
				else
				{
					recv((char *)&h, sizeof_rrframeheader);
					ENDIANIZE(h);
				}
				bool stereo = (h.flags == RR_LEFT || h.flags == RR_RIGHT);
				unsigned short dpynum =
					(v.major < 2 || (v.major == 2 && v.minor < 1)) ?
					h.dpynum : DisplayNumber(maindpy);
				ERRIFNOT(w = addWindow(dpynum, h.winid, stereo));

				if(!stereo || h.flags == RR_LEFT || !f)
				{
					try
					{
						f = w->getFrame(h.compress == RRCOMP_YUV);
					}
					catch(...) { if(w) deleteWindow(w);  throw; }
				}
				#ifdef USEXV
				if(h.compress == RRCOMP_YUV)
				{
					((XVFrame *)f)->init(h);
					if(h.size != ((XVFrame *)f)->hdr.size && h.flags != RR_EOF)
						THROW("YUV image size mismatch");
				}
				else
				#endif
				((CompressedFrame *)f)->init(h, h.flags);
				if(h.flags != RR_EOF)
					recv((char *)(h.flags == RR_RIGHT ? f->rbits : f->bits), h.size);

				if(!stereo || h.flags != RR_LEFT)
				{
					try
					{
						w->drawFrame(f);
					}
					catch(...) { if(w) deleteWindow(w);  throw; }
				}

			} while(!(f && f->hdr.flags == RR_EOF));

			if(v.major == 1 && v.minor == 0)
			{
				char cts = 1;
				send(&cts, 1);
			}
		}
	}
	catch(Error &e)
	{
		vglout.println("%s-- %s", e.getMethod(), e.getMessage());
	}
	if(thread) { thread->detach();  delete thread; }
	delete this;
}


void VGLTransReceiver::Listener::deleteWindow(ClientWin *w)
{
	int i, j;
	CriticalSection::SafeLock l(winMutex);

	if(nwin > 0)
	{
		for(i = 0; i < nwin; i++)
		{
			if(windows[i] == w)
			{
				delete w;  windows[i] = NULL;
				if(i < nwin - 1)
					for(j = i; j < nwin - 1; j++) windows[j] = windows[j + 1];
				windows[nwin - 1] = NULL;  nwin--;  break;
			}
		}
	}
}


// Register a new window with this server
ClientWin *VGLTransReceiver::Listener::addWindow(int dpynum, Window win,
	bool stereo)
{
	CriticalSection::SafeLock l(winMutex);
	int winid = nwin;

	if(nwin > 0)
	{
		for(winid = 0; winid < nwin; winid++)
			if(windows[winid] && windows[winid]->match(dpynum, win))
				return windows[winid];
	}
	if(nwin >= MAXWIN) THROW("No free window IDs");
	if(dpynum < 0 || dpynum > 65535 || win == None) THROW("Invalid argument");
	NEWCHECK(windows[winid] = new ClientWin(dpynum, win, drawMethod, stereo));

	if(!windows[winid]) THROW("Could not create window instance");
	nwin++;
	return windows[winid];
}


void VGLTransReceiver::Listener::send(char *buf, int len)
{
	try
	{
		if(socket) socket->send(buf, len);
	}
	catch(...)
	{
		vglout.println("Error sending data to server.  Server may have disconnected.");
		vglout.println("   (this is normal if the application exited.)");
		throw;
	}
}


void VGLTransReceiver::Listener::recv(char *buf, int len)
{
	try
	{
		if(socket) socket->recv(buf, len);
	}
	catch(...)
	{
		vglout.println("Error receiving data from server.  Server may have disconnected.");
		vglout.println("   (this is normal if the application exited.)");
		throw;
	}
}
