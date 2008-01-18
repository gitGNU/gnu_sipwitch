// Copyright (C) 2006-2007 David Sugar, Tycho Softworks.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "server.h"

NAMESPACE_SIPWITCH
using namespace UCOMMON_NAMESPACE;

static volatile unsigned allocated_segments = 0;
static volatile unsigned active_segments = 0;
static volatile unsigned allocated_calls = 0;
static volatile unsigned active_calls = 0;
static unsigned mapped_calls = 0;
static LinkedObject *freesegs = NULL;
static LinkedObject *freecalls = NULL;
static LinkedObject *freemaps = NULL;
static LinkedObject **hash = NULL;
static unsigned keysize = 177;
static condlock_t locking;

stack::background *stack::background::thread = NULL;

static bool tobool(const char *s)
{
	assert(s != NULL);

	switch(*s)
	{
	case 'n':
	case 'N':
	case 'f':
	case 'F':
	case '0':
		return false;
	}
	return true;
}

stack stack::sip;

stack::segment::segment(call *cr, int cid, int did) : OrderedObject()
{
	assert(cr != NULL);
	assert(cid > 0);

	time_t now;

	++cr->count;
	time(&now);
	enlist(&(cr->segments));
	sid.enlist(&hash[cid % keysize]);
	sid.sequence = (uint32_t)now;
	sid.sequence &= 0xffffffffl;
	sid.expires = 0l;
	sid.cid = cid;
	sid.did = did;
	sid.parent = cr;
	sid.state = session::OPEN;
	sid.sdp[0] = 0;
	sid.reg = NULL;
}

void *stack::segment::operator new(size_t size)
{
	assert(size == sizeof(stack::segment));

	return allocate(size, &freesegs, &allocated_segments);
}

void stack::segment::operator delete(void *obj)
{
	assert(obj != NULL);

	((LinkedObject*)(obj))->enlist(&freesegs);
}

stack::call::call() : TimerQueue::event(Timer::reset), segments()
{
}

void *stack::call::operator new(size_t size)
{
	assert(size == sizeof(stack::call));

	++active_calls;
	return allocate(size, &freecalls, &allocated_calls);
}

void stack::call::operator delete(void *obj)
{
	assert(obj != NULL);

	((LinkedObject*)(obj))->enlist(&freecalls);
	--active_calls;
}

void stack::call::disconnect(void)
{
	debug(4, "disconnecting call %08x:%u\n", source->sequence, source->cid);

	linked_pointer<segment> sp = segments.begin();
	while(sp) {
		if(sp->sid.reg) {
			sp->sid.reg->decUse();
			sp->sid.reg = NULL;
		}
		if(sp->sid.cid > 0 && sp->sid.state != session::CLOSED) {
			sp->sid.state = session::CLOSED;
			eXosip_lock();
			eXosip_call_terminate(sp->sid.cid, sp->sid.did);
			eXosip_unlock();
		}
		sp.next();
	}

	if(state != INITIAL) {
		state = FINAL;
		set((timeout_t)7000);
		update();
	}
}

void stack::call::closing(session *s)
{
	assert(s != NULL);

	if(invited) {
		switch(s->state)
		{
		case session::RING:
			--ringing;
			break;
		case session::FWD:
			--forwarding;
			break;
		case session::BUSY:
			--ringbusy;
			break;
		case session::REORDER:
			--unreachable;
			break;
		default:
			break;
		}
		--invited;
	}

	if(invited) {
		update();
		return;
	}

	disconnect();
}

void stack::call::update(void)
{
	// TODO: switch by call state to see if we send reply code!
	if(state == INITIAL) {
		// if(forwarding && invited - unreachable - ringbusy == forwarding)
		// else if(ringing && invited - unreachable - ringbusy == ringing + forwarding)...
		// else if(ringbusy && invited - unreachable == ringbusy) ...
		// else if(invited == unreachable) ...
	}
}

void stack::call::expired(void)
{
	mutex::protect(this);
	switch(state) {
	case HOLDING:	// hold-recall timer expired...

	case RINGING:	// maybe ring-no-answer with forward? invite expired?

	case BUSY:		// invite expired
	case ACTIVE:	// active call session expired without re-invite

	case FINAL:		// session expects to be cleared....
	case REORDER:	// only different in logging

					// TODO: initial may have special case if pending!!!
	case INITIAL:	// if session never used, garbage collect at expire...
		debug(4, "expiring call %08x:%u\n", source->sequence, source->cid);
		mutex::release(this);
		stack::destroy(this);
		return;
	}
	mutex::release(this);
}

stack::background::background(timeout_t iv) : DetachedThread(), Conditional(), expires(Timer::inf)
{
	cancelled = false;
	signalled = false;
	interval = iv;
}

void stack::background::create(timeout_t iv)
{
	thread = new background(iv);
	thread->start();
}

void stack::background::cancel(void)
{
	thread->Conditional::lock();
	thread->cancelled = true;
	thread->Conditional::signal();
	thread->Conditional::unlock();
}

void stack::background::signal(void)
{
	thread->Conditional::lock();
	thread->signalled = true;
	thread->Conditional::signal();
	thread->Conditional::unlock();
}

void stack::background::run(void)
{
	process::errlog(DEBUG1, "starting background thread");
	timeout_t timeout;

	for(;;) {
		Conditional::lock();
		if(cancelled) {
			Conditional::unlock();
			process::errlog(DEBUG1, "stopping background thread");
			thread = NULL;
			DetachedThread::exit();
		}
		timeout = expires.get();
		if(!signalled && timeout) {
			if((long)timeout > 0)
				debug(9, "scheduled %ld\n", timeout);
			if(timeout > interval) 
				timeout = interval;
			Conditional::wait(interval);
		}
		if(signalled || !expires.get()) {
			signalled = false;
			Conditional::unlock();
			debug(4, "background timer expired\n");
			locking.access();
			expires = stack::sip.expire();
			locking.release();
		}
		else {
			signalled = false;
			Conditional::unlock();
		}
		messages::automatic();
		eXosip_lock();		
		eXosip_automatic_action();
		eXosip_unlock();
	}
}

stack::stack() :
service::callback(1), mapped_reuse<MappedCall>(), TimerQueue()
{
	stacksize = 0;
	threading = 2;
	priority = 1;
	timing = 500;
	iface = NULL;
	protocol = IPPROTO_UDP;
	port = 5060;
	family = AF_INET;
	tlsmode = 0;
	send101 = 1;
	incoming = false;
	outgoing = false;
	agent = "sipwitch";
	restricted = trusted = published = NULL;
	localnames = "localhost, localhost.localdomain";
}

void stack::modify(void)
{
}

void stack::update(void)
{
	background::signal();
}

void stack::close(session *s)
{
	assert(s != NULL);

	call *cr;

	if(!s)
		return;

	cr = s->parent;
	mutex::protect(cr);
	if(s->state != session::CLOSED) {
		s->state = session::CLOSED;
		if(s == cr->source || s == cr->source)
			cr->disconnect();
		else
			cr->closing(s);
	}
	mutex::release(cr);
}
	
void stack::clear(session *s)
{
	assert(s != NULL);

	call *cr;

	if(!s)
		return;

	cr = s->parent;
	mutex::protect(cr);
	if(--cr->count == 0) {
		debug(4, "clearing call %08x:%u\n", 
			cr->source->sequence, cr->source->cid);
		mutex::release(cr);
		destroy(cr);
		return;
	}

	if(s->cid > 0) {
		mutex::release(cr);
		locking.exclusive();
		debug(4, "clearing call %08x:%u session %08x:%u\n", 
			cr->source->sequence, cr->source->cid, s->sequence, s->cid); 
		if(s->state != session::CLOSED) {
			s->state = session::CLOSED;
			eXosip_call_terminate(s->cid, s->did);
		}
		s->delist(&hash[s->cid % keysize]);
		s->cid = 0;
		s->did = -1;
		if(s->reg) {
			s->reg->decUse();
			s->reg = NULL;
		}
		locking.share();
	}
	else
		mutex::release(cr);
}

void stack::destroy(session *s)
{
	assert(s != NULL);

	if(!s || !s->parent)
		return;

	destroy(s->parent);
}

void stack::destroy(call *cr)
{
	assert(cr != NULL);

	linked_pointer<segment> sp;

	// we assume access lock was already held when we call this...

	locking.exclusive();
	sp = cr->segments.begin();
	while(sp) {
		--active_segments;
		segment *next = sp.getNext();
		if(sp->sid.reg) {
			sp->sid.reg->decUse();
			sp->sid.reg = NULL;
		}
		if(sp->sid.cid > 0) {
			if(sp->sid.state != session::CLOSED)
				eXosip_call_terminate(sp->sid.cid, sp->sid.did);
			sp->sid.delist(&hash[sp->sid.cid % keysize]);
		}
		delete *sp;
		sp = next;
	}
	if(cr->map)
		cr->map->enlist(&freemaps);
	cr->delist();
	delete cr;
	locking.share();
}

void stack::getInterface(struct sockaddr *iface, struct sockaddr *dest)
{
	assert(iface != NULL && dest != NULL);

	Socket::getinterface(iface, dest);
	switch(iface->sa_family) {
	case AF_INET:
		((struct sockaddr_in*)(iface))->sin_port = htons(sip.port);
		break;
#ifdef	AF_INET6
	case AF_INET6:
		((struct sockaddr_in6*)(iface))->sin6_port = htons(sip.port);
		break;
#endif
	}
}
	
stack::session *stack::create(int cid, int did)
{
	assert(cid > 0);

	call *cr;
	segment *sp;

	locking.modify();
	cr = new call;

	cr->arm(7000);	// Normally we get close in 6 seconds, this assures...
	cr->count = 0;
	cr->invited = cr->ringing = cr->ringbusy = cr->unreachable = cr->forwarding = 0;
	cr->expires = 0l;
	cr->target = NULL;
	cr->state = call::INITIAL;
	cr->enlist(&stack::sip);
	cr->starting = 0l;
	sp = new segment(cr, cid, did);	// after count set to 0!
	cr->source = &(sp->sid);

	locking.share();
	cr->update();
	return cr->source;
}

void stack::logCall(const char *reason, session *session, const char *joined)
{
	assert(reason != NULL && *reason != 0);
	assert(session != NULL);

	time_t now;
	struct tm *dt;
	call *cr;

	if(!session)
		return;

	cr = session->parent;
	session = cr->source;

	if(!cr || !cr->starting)
		return;

	if(!joined && cr->target)
		joined = cr->target->sysident;

	if(!joined)
		joined = "n/a";

	time(&now);
	dt = localtime(&cr->starting);

	process::printlog(NULL, "call %08x:%u %s %04d-%02d-%02d %02d:%02d:%02d %ld %s %s %s %s\n",
		session->sequence, session->cid, reason,
		dt->tm_year + 1900, dt->tm_mon + 1, dt->tm_mday,
		dt->tm_hour, dt->tm_min, dt->tm_sec, now - cr->starting,
		session->sysident, cr->dialed, joined, session->display);		
	
	cr->starting = 0l;
}

void stack::setBusy(int tid, session *session)
{
	assert(session != NULL);

	osip_message_t *reply = NULL;
	call *cr;
	if(!session)
		return;

	cr = session->parent;
	cr->state = call::BUSY;
	cr->disarm();
	cr->update();

	eXosip_lock();
	eXosip_call_build_answer(tid, SIP_BUSY_HERE, &reply);
	eXosip_call_send_answer(tid, SIP_BUSY_HERE, reply);
	eXosip_unlock();		

	logCall("busy", session);
}

stack::session *stack::access(int cid)
{
	assert(cid > 0);

	linked_pointer<session> sp;

	locking.access();
	sp = hash[cid % keysize];
	while(sp) {
		if(sp->cid == cid)
			break;
		sp.next();
	}
	if(!sp) {
		locking.release();
		return NULL;
	}
	return *sp;
}

void stack::release(session *s)
{
	if(s)
		locking.release();
}
	
void stack::start(service *cfg) 
{
	assert(cfg != NULL);
	 
	thread *thr; 
	unsigned thidx = 0;
	process::errlog(DEBUG1, "sip stack starting; creating %d threads at priority %d", threading, priority); 
	eXosip_init();

	MappedReuse::create("sipwitch.callmap", mapped_calls);
	if(!sip)
		process::errlog(FAILURE, "calls could not be mapped");

#ifdef	AF_INET6
	if(protocol == AF_INET6)
		eXosip_enable_ipv6(1);
#endif

	if(eXosip_listen_addr(protocol, iface, port, family, tlsmode)) {
#ifdef	AF_INET6
		if(!iface && protocol == AF_INET6)
			iface = "::*";
#endif
		if(!iface)
			iface = "*";
		process::errlog(FAILURE, "sip cannot bind interface %s, port %d", iface, port);
	}

	osip_trace_initialize_syslog(TRACE_LEVEL0, "sipwitch");
	eXosip_set_user_agent(agent);

#ifdef	EXOSIP2_OPTION_SEND_101
	eXosip_set_option(EXOSIP_OPT_DONT_SEND_101, &send101); 
#endif

	while(thidx++ < threading) {
		thr = new thread();
		thr->start(priority);
	}

	thread::wait(threading);
	background::create(timing);
}

void stack::stop(service *cfg)
{
	assert(cfg != NULL);

	process::errlog(DEBUG1, "sip stack stopping");
	background::cancel();
	thread::shutdown();
	Thread::yield();
	MappedMemory::release();
	MappedMemory::remove("sipwitch.callmap");

}

bool stack::check(void)
{
	process::errlog(INFO, "checking sip stack...");
	eXosip_lock();
	eXosip_unlock();
	return true;
}

void stack::snapshot(FILE *fp) 
{ 
	assert(fp != NULL);

	linked_pointer<call> cp;
	fprintf(fp, "SIP:\n"); 
	locking.access();
	fprintf(fp, "  mapped calls: %d\n", mapped_calls);
	fprintf(fp, "  active calls: %d\n", active_calls);
	fprintf(fp, "  active sessions: %d\n", active_segments);
	fprintf(fp, "  allocated calls: %d\n", allocated_calls);
	fprintf(fp, "  allocated sessions: %d\n", allocated_segments);
	cp = begin();
	while(cp) {
		cp.next();
	}
	locking.release();
} 

bool stack::reload(service *cfg)
{
	assert(cfg != NULL);

	const char *key = NULL, *value;
	linked_pointer<service::keynode> sp = cfg->getList("stack");
	int val;
	const char *localhosts = "localhost, localhost.localdomain";

	while(sp) {
		key = sp->getId();
		value = sp->getPointer();
		if(key && value) {
			if(!stricmp(key, "threading") && !isConfigured())
				threading = atoi(value);
			else if(!stricmp(key, "priority") && !isConfigured())
				priority = atoi(value);
			else if(!stricmp(key, "timing"))
				timing = atoi(value);
			else if(!stricmp(key, "incoming"))
				incoming = tobool(value);
			else if(!stricmp(key, "outgoing"))
				outgoing = tobool(value);
			else if(!stricmp(key, "keysize") && !isConfigured())
				keysize = atoi(value);
			else if(!stricmp(key, "interface") && !isConfigured()) {
#ifdef	AF_INET6
				if(strchr(value, ':') != NULL)
					family = AF_INET6;
#endif
				if(!strcmp(value, ":::") || !strcmp(value, "::*") || !stricmp(value, "*") || !*value)
					value = NULL;
				if(value)
					value = strdup(value);
				iface = value;
			}
			else if(!stricmp(key, "send101") && !isConfigured() && tobool(value))
				send101 = 0;
			else if(!stricmp(key, "keepalive") && !isConfigured()) {
				val = atoi(value);
				eXosip_set_option(EXOSIP_OPT_UDP_KEEP_ALIVE, &val);
			} 
			else if(!stricmp(key, "learn") && !isConfigured()) {
				val = tobool(value);
				eXosip_set_option(EXOSIP_OPT_UDP_LEARN_PORT, &val);
			}
			else if(!stricmp(key, "restricted"))
				restricted = cfg->dup(value);
			else if(!stricmp(key, "localnames"))
				localhosts = cfg->dup(value);
			else if(!stricmp(key, "trusted"))
				trusted = cfg->dup(value);
			else if(!stricmp(key, "published") || !stricmp(key, "public"))
				published = cfg->dup(value);
			else if(!stricmp(key, "agent") && !isConfigured())
				agent = strdup(value);
			else if(!stricmp(key, "port") && !isConfigured())
				port = atoi(value);
			else if(!stricmp(key, "mapped") && !isConfigured())
				mapped_calls = atoi(value);
			else if(!stricmp(key, "transport") && !isConfigured()) {
				if(!stricmp(value, "tcp") || !stricmp(value, "tls"))
					protocol = IPPROTO_TCP;
				else if(!stricmp(value, "tls"))
					tlsmode = 1;
			}
		}
		sp.next();
	}
	localnames = localhosts;

	if(!mapped_calls) 
		mapped_calls = registry::getEntries();
	if(!hash) {
		hash = new LinkedObject*[keysize];
		memset(hash, 0, sizeof(LinkedObject *) * keysize);
	}
	return true;
}

char *stack::sipIdentity(struct sockaddr_internet *addr, char *buf, const char *user, size_t size)
{
	assert(addr != NULL);
	assert(buf != NULL);
	assert(user == NULL || *user != 0);
	assert(size > 0);

	*buf = 0;
	size_t len;

	if(!size)
		size = MAX_URI_SIZE;

	if(!addr)
		return NULL;

	if(user) {
		string::add(buf, size, user);
		string::add(buf, size, "@");
	}

	len = strlen(buf);
	Socket::getaddress((struct sockaddr *)addr, buf + len, size - len);
	return buf;
}

char *stack::sipAddress(struct sockaddr_internet *addr, char *buf, const char *user, size_t size)
{
	assert(addr != NULL);
	assert(buf != NULL);
	assert(user == NULL || *user != 0);
	assert(size > 0);

	char pbuf[10];
	unsigned port;
	bool ipv6 = false;

	*buf = 0;
	size_t len;

	if(!size)
		size = MAX_URI_SIZE;

	if(!addr)
		return NULL;

	switch(addr->sa_family) {
	case AF_INET:
		port = ntohs(addr->ipv4.sin_port);
		break;
#ifdef	AF_INET6
	case AF_INET6:
		ipv6 = true;
		port = ntohs(addr->ipv6.sin6_port);
		break;
#endif
	default:
		return NULL;
	}
	if(!port)
		port = sip.port;

	if(sip.tlsmode)
		string::set(buf, size, "sips:");
	else 
		string::set(buf, size, "sip:");

	if(user) {
		string::add(buf, size, user);
		if(ipv6)
			string::add(buf, size, "@[");
		else			
			string::add(buf, size, "@");
	}
	else if(ipv6)
		string::add(buf, size, "[");

	len = strlen(buf);
	Socket::getaddress((struct sockaddr *)addr, buf + len, size - len);
	if(ipv6)
		snprintf(pbuf, sizeof(pbuf), "]:%u", port);
	else
		snprintf(pbuf, sizeof(pbuf), ":%u", port);
	string::add(buf, size, pbuf);
	return buf;
}
	
Socket::address *stack::getAddress(const char *addr, Socket::address *ap)
{
	assert(addr != NULL && *addr != 0);

	char buffer[MAX_URI_SIZE];
	int family = sip.family;
	const char *svc = "sip";
	char *ep;
	int proto = SOCK_DGRAM;
	if(sip.protocol == IPPROTO_TCP)
		proto = SOCK_STREAM;

	if(!strnicmp(addr, "sip:", 4))
		addr += 4;
	else if(!strnicmp(addr, "sips:", 5))
		addr += 5;

	if(strchr(addr, '@'))
		addr = strchr(addr, '@') + 1;	

#ifdef	AF_INET6
	if(*addr == '[') {
		string::set(buffer, sizeof(buffer), ++addr);
		family = AF_INET6;
		ep = strchr(buffer, ']');
		if(ep)
			*(ep++) = 0;
		if(*ep == ':')
			svc = ++ep;
		goto set;
	} 
#endif
	string::set(buffer, sizeof(buffer), addr);
	ep = strchr(buffer, ':');
	if(ep) {
		*(ep++) = 0;
		svc = ep;
	}

set:
	if(ap)
		ap->add(buffer, svc, family);
	else
		ap = new Socket::address(family, buffer, svc);

	if(ap && !ap->getList()) {
		delete ap;
		addr = NULL;
	}
	return ap;
}

END_NAMESPACE
