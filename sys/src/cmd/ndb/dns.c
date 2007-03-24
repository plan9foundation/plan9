#include <u.h>
#include <libc.h>
#include <auth.h>
#include <fcall.h>
#include <bio.h>
#include <ctype.h>
#include <ip.h>
#include <pool.h>
#include "dns.h"

enum
{
	Maxrequest=		1024,
	Maxreply=		512,
	Maxrrr=			16,
	Maxfdata=		8192,

	Defmaxage=		60*60,		/* 1 hour */

	Qdir=			0,
	Qdns=			1,
};

typedef struct Mfile	Mfile;
typedef struct Job	Job;
typedef struct Network	Network;

int vers;		/* incremented each clone/attach */

struct Mfile
{
	Mfile		*next;		/* next free mfile */
	int		ref;

	char		*user;
	Qid		qid;
	int		fid;

	int		type;		/* reply type */
	char		reply[Maxreply];
	ushort		rr[Maxrrr];	/* offset of rr's */
	ushort		nrr;		/* number of rr's */
};

/*
 *  active local requests
 */
struct Job
{
	Job	*next;
	int	flushed;
	Fcall	request;
	Fcall	reply;
};
Lock	joblock;
Job	*joblist;

struct {
	Lock;
	Mfile	*inuse;		/* active mfile's */
} mfalloc;

Cfg	cfg;
int	debug;
uchar	ipaddr[IPaddrlen];	/* my ip address */
int	maxage = Defmaxage;
int	mfd[2];
int	needrefresh;
ulong	now;
int	sendnotifies;
int	testing;
char	*trace;
int	traceactivity;
char	*zonerefreshprogram;

char	*logfile = "dns";	/* or "dns.test" */
char	*dbfile;
char	mntpt[Maxpath];

int	fillreply(Mfile*, int);
void	freejob(Job*);
void	io(void);
void	mountinit(char*, char*);
Job*	newjob(void);
void	rattach(Job*, Mfile*);
void	rauth(Job*);
void	rclunk(Job*, Mfile*);
void	rcreate(Job*, Mfile*);
void	rflush(Job*);
void	ropen(Job*, Mfile*);
void	rread(Job*, Mfile*);
void	rremove(Job*, Mfile*);
void	rstat(Job*, Mfile*);
void	rversion(Job*);
char*	rwalk(Job*, Mfile*);
void	rwrite(Job*, Mfile*, Request*);
void	rwstat(Job*, Mfile*);
void	sendmsg(Job*, char*);
void	setext(char*, int, char*);

void
usage(void)
{
	fprint(2, "usage: %s [-norRst] [-a maxage] [-f ndb-file] [-N target] "
		"[-x netmtpt] [-z refreshprog]\n", argv0);
	exits("usage");
}

void
main(int argc, char *argv[])
{
	char	servefile[Maxpath], ext[Maxpath];

	setnetmtpt(mntpt, sizeof mntpt, nil);
	ext[0] = 0;
	ARGBEGIN{
	case 'a':
		maxage = atol(EARGF(usage()));
		if (maxage <= 0)
			maxage = Defmaxage;
		break;
	case 'd':
		debug = 1;
		traceactivity = 1;
		break;
	case 'f':
		dbfile = EARGF(usage());
		break;
	case 'n':
		sendnotifies = 1;
		break;
	case 'N':
		target = atol(EARGF(usage()));
		if (target < 100)
			target = 100;
		break;
	case 'o':
		cfg.straddle = 1;	/* straddle inside & outside networks */
		break;
	case 'r':
		cfg.resolver = 1;
		break;
	case 'R':
		norecursion = 1;
		break;
	case 's':
		cfg.serve = 1;		/* serve network */
		cfg.cachedb = 1;
		break;
	case 't':
		testing = 1;
		break;
	case 'x':
		setnetmtpt(mntpt, sizeof mntpt, EARGF(usage()));
		setext(ext, sizeof ext, mntpt);
		break;
	case 'z':
		zonerefreshprogram = EARGF(usage());
		break;
	}ARGEND
	USED(argc);
	USED(argv);

	if(testing)
		mainmem->flags |= POOL_NOREUSE | POOL_ANTAGONISM;
	rfork(RFREND|RFNOTEG);

	cfg.inside = (*mntpt == '\0' || strcmp(mntpt, "/net") == 0);

	/* start syslog before we fork */
	fmtinstall('F', fcallfmt);
	dninit();
	if(myipaddr(ipaddr, mntpt) < 0)
		sysfatal("can't read my ip address");
	dnslog("starting %s%sdns %s%son %I's %s",
		(cfg.straddle? "straddling ": ""),
		(cfg.cachedb? "caching ": ""),
		(cfg.serve?   "udp server ": ""),
		(cfg.resolver? "resolver ": ""), ipaddr, mntpt);

	opendatabase();

	snprint(servefile, sizeof servefile, "#s/dns%s", ext);
	unmount(servefile, mntpt);
	remove(servefile);
	mountinit(servefile, mntpt);

	now = time(nil);
	srand(now*getpid());
	db2cache(1);

	if (cfg.straddle && !seerootns())
		dnslog("straddle server misconfigured; can't see root name servers");
	if(cfg.serve)
		dnudpserver(mntpt);
	if(sendnotifies)
		notifyproc();

	io();
	dnslog("io returned, exiting");
	exits(0);
}

/*
 *  if a mount point is specified, set the cs extension to be the mount point
 *  with '_'s replacing '/'s
 */
void
setext(char *ext, int n, char *p)
{
	int i, c;

	n--;
	for(i = 0; i < n; i++){
		c = p[i];
		if(c == 0)
			break;
		if(c == '/')
			c = '_';
		ext[i] = c;
	}
	ext[i] = 0;
}

void
mountinit(char *service, char *mntpt)
{
	int f;
	int p[2];
	char buf[32];

	if(pipe(p) < 0)
		abort(); /* "pipe failed" */;
	switch(rfork(RFFDG|RFPROC|RFNAMEG)){
	case 0:
		close(p[1]);
		procsetname("main");
		break;
	case -1:
		abort(); /* "fork failed\n" */;
	default:
		close(p[0]);

		/*
		 *  make a /srv/dns
		 */
		f = create(service, 1, 0666);
		if(f < 0)
			abort(); /* service */;
		snprint(buf, sizeof buf, "%d", p[1]);
		if(write(f, buf, strlen(buf)) != strlen(buf))
			abort(); /* "write %s", service */;
		close(f);

		/*
		 *  put ourselves into the file system
		 */
		if(mount(p[1], -1, mntpt, MAFTER, "") < 0)
			fprint(2, "dns mount failed: %r\n");
		_exits(0);
	}
	mfd[0] = mfd[1] = p[0];
}

Mfile*
newfid(int fid, int needunused)
{
	Mfile *mf;

	lock(&mfalloc);
	for(mf = mfalloc.inuse; mf != nil; mf = mf->next)
		if(mf->fid == fid){
			unlock(&mfalloc);
			if(needunused)
				return nil;
			return mf;
		}
	mf = emalloc(sizeof(*mf));
	if(mf == nil)
		sysfatal("out of memory");
	mf->fid = fid;
	mf->next = mfalloc.inuse;
	mfalloc.inuse = mf;
	unlock(&mfalloc);
	return mf;
}

void
freefid(Mfile *mf)
{
	Mfile **l;

	lock(&mfalloc);
	for(l = &mfalloc.inuse; *l != nil; l = &(*l)->next)
		if(*l == mf){
			*l = mf->next;
			if(mf->user)
				free(mf->user);
			memset(mf, 0, sizeof *mf);	/* cause trouble */
			free(mf);
			unlock(&mfalloc);
			return;
		}
	sysfatal("freeing unused fid");
}

Mfile*
copyfid(Mfile *mf, int fid)
{
	Mfile *nmf;

	nmf = newfid(fid, 1);
	if(nmf == nil)
		return nil;
	nmf->fid = fid;
	nmf->user = estrdup(mf->user);
	nmf->qid.type = mf->qid.type;
	nmf->qid.path = mf->qid.path;
	nmf->qid.vers = vers++;
	return nmf;
}

Job*
newjob(void)
{
	Job *job;

	job = emalloc(sizeof *job);
	lock(&joblock);
	job->next = joblist;
	joblist = job;
	job->request.tag = -1;
	unlock(&joblock);
	return job;
}

void
freejob(Job *job)
{
	Job **l;

	lock(&joblock);
	for(l = &joblist; *l; l = &(*l)->next)
		if(*l == job){
			*l = job->next;
			memset(job, 0, sizeof *job);	/* cause trouble */
			free(job);
			break;
		}
	unlock(&joblock);
}

void
flushjob(int tag)
{
	Job *job;

	lock(&joblock);
	for(job = joblist; job; job = job->next)
		if(job->request.tag == tag && job->request.type != Tflush){
			job->flushed = 1;
			break;
		}
	unlock(&joblock);
}

void
io(void)
{
	long n;
	Mfile *mf;
	uchar mdata[IOHDRSZ + Maxfdata];
	Request req;
	Job *job;

	memset(&req, 0, sizeof req);
	/*
	 *  a slave process is sometimes forked to wait for replies from other
	 *  servers.  The master process returns immediately via a longjmp
	 *  through 'mret'.
	 */
	if(setjmp(req.mret))
		putactivity(0);
	procsetname("main 9p reading loop");
	req.isslave = 0;
	for(;;){
		n = read9pmsg(mfd[0], mdata, sizeof mdata);
		if(n<=0){
			dnslog("error reading mntpt: %r");
			exits(0);
		}

		job = newjob();
		if(convM2S(mdata, n, &job->request) != n){
			freejob(job);
			continue;
		}
		mf = newfid(job->request.fid, 0);
		if(debug)
			dnslog("%F", &job->request);

		getactivity(&req, 0);
		req.aborttime = now + Maxreqtm;

		switch(job->request.type){
		default:
			warning("unknown request type %d", job->request.type);
			break;
		case Tversion:
			rversion(job);
			break;
		case Tauth:
			rauth(job);
			break;
		case Tflush:
			rflush(job);
			break;
		case Tattach:
			rattach(job, mf);
			break;
		case Twalk:
			rwalk(job, mf);
			break;
		case Topen:
			ropen(job, mf);
			break;
		case Tcreate:
			rcreate(job, mf);
			break;
		case Tread:
			rread(job, mf);
			break;
		case Twrite:
			rwrite(job, mf, &req);
			break;
		case Tclunk:
			rclunk(job, mf);
			break;
		case Tremove:
			rremove(job, mf);
			break;
		case Tstat:
			rstat(job, mf);
			break;
		case Twstat:
			rwstat(job, mf);
			break;
		}

		freejob(job);

		/*
		 *  slave processes die after replying
		 */
		if(req.isslave){
			putactivity(0);
			_exits(0);
		}

		putactivity(0);
	}
}

void
rversion(Job *job)
{
	if(job->request.msize > IOHDRSZ + Maxfdata)
		job->reply.msize = IOHDRSZ + Maxfdata;
	else
		job->reply.msize = job->request.msize;
	if(strncmp(job->request.version, "9P2000", 6) != 0)
		sendmsg(job, "unknown 9P version");
	else{
		job->reply.version = "9P2000";
		sendmsg(job, 0);
	}
}

void
rauth(Job *job)
{
	sendmsg(job, "dns: authentication not required");
}

/*
 *  don't flush till all the slaves are done
 */
void
rflush(Job *job)
{
	flushjob(job->request.oldtag);
	sendmsg(job, 0);
}

void
rattach(Job *job, Mfile *mf)
{
	if(mf->user != nil)
		free(mf->user);
	mf->user = estrdup(job->request.uname);
	mf->qid.vers = vers++;
	mf->qid.type = QTDIR;
	mf->qid.path = 0LL;
	job->reply.qid = mf->qid;
	sendmsg(job, 0);
}

char*
rwalk(Job *job, Mfile *mf)
{
	int i, nelems;
	char *err;
	char **elems;
	Mfile *nmf;
	Qid qid;

	err = 0;
	nmf = nil;
	elems  = job->request.wname;
	nelems = job->request.nwname;
	job->reply.nwqid = 0;

	if(job->request.newfid != job->request.fid){
		/* clone fid */
		nmf = copyfid(mf, job->request.newfid);
		if(nmf == nil){
			err = "clone bad newfid";
			goto send;
		}
		mf = nmf;
	}
	/* else nmf will be nil */

	qid = mf->qid;
	if(nelems > 0)
		/* walk fid */
		for(i=0; i<nelems && i<MAXWELEM; i++){
			if((qid.type & QTDIR) == 0){
				err = "not a directory";
				break;
			}
			if (strcmp(elems[i], "..") == 0 ||
			    strcmp(elems[i], ".") == 0){
				qid.type = QTDIR;
				qid.path = Qdir;
Found:
				job->reply.wqid[i] = qid;
				job->reply.nwqid++;
				continue;
			}
			if(strcmp(elems[i], "dns") == 0){
				qid.type = QTFILE;
				qid.path = Qdns;
				goto Found;
			}
			err = "file does not exist";
			break;
		}

send:
	if(nmf != nil && (err!=nil || job->reply.nwqid<nelems))
		freefid(nmf);
	if(err == nil)
		mf->qid = qid;
	sendmsg(job, err);
	return err;
}

void
ropen(Job *job, Mfile *mf)
{
	int mode;
	char *err;

	err = 0;
	mode = job->request.mode;
	if(mf->qid.type & QTDIR)
		if(mode)
			err = "permission denied";
	job->reply.qid = mf->qid;
	job->reply.iounit = 0;
	sendmsg(job, err);
}

void
rcreate(Job *job, Mfile *mf)
{
	USED(mf);
	sendmsg(job, "creation permission denied");
}

void
rread(Job *job, Mfile *mf)
{
	int i, n;
	long clock;
	ulong cnt;
	vlong off;
	char *err;
	uchar buf[Maxfdata];
	Dir dir;

	n = 0;
	err = nil;
	off = job->request.offset;
	cnt = job->request.count;
	*buf = '\0';
	job->reply.data = (char*)buf;
	if(mf->qid.type & QTDIR){
		clock = time(nil);
		if(off == 0){
			dir.name = "dns";
			dir.qid.type = QTFILE;
			dir.qid.vers = vers;
			dir.qid.path = Qdns;
			dir.mode = 0666;
			dir.length = 0;
			dir.uid = dir.gid = dir.muid = mf->user;
			dir.atime = dir.mtime = clock;		/* wrong */
			n = convD2M(&dir, buf, sizeof buf);
		}
	} else if (off < 0)
		err = "negative read offset";
	else {
		/* first offset will always be zero */
		for(i = 1; i <= mf->nrr; i++)
			if(mf->rr[i] > off)
				break;
		if(i <= mf->nrr) {
			if(off + cnt > mf->rr[i])
				n = mf->rr[i] - off;
			else
				n = cnt;
			assert(n >= 0);
			job->reply.data = mf->reply + off;
		}
	}
	job->reply.count = n;
	sendmsg(job, err);
}

void
rwrite(Job *job, Mfile *mf, Request *req)
{
	int rooted, status, wantsav;
	long n;
	ulong cnt;
	char *err, *p, *atype;
	RR *rp, *tp, *neg;

	err = nil;
	cnt = job->request.count;
	if(mf->qid.type & QTDIR){
		err = "can't write directory";
		goto send;
	}
	if (job->request.offset != 0) {
		err = "writing at non-zero offset";
		goto send;
	}
	if(cnt >= Maxrequest){
		err = "request too long";
		goto send;
	}
	job->request.data[cnt] = 0;
	if(cnt > 0 && job->request.data[cnt-1] == '\n')
		job->request.data[cnt-1] = 0;

	/*
	 *  special commands
	 */
	if(strcmp(job->request.data, "debug")==0){
		debug ^= 1;
		goto send;
	} else if(strcmp(job->request.data, "dump")==0){
		dndump("/lib/ndb/dnsdump");
		goto send;
	} else if(strcmp(job->request.data, "refresh")==0){
		needrefresh = 1;
		goto send;
	} else if(strcmp(job->request.data, "poolcheck")==0){
		poolcheck(mainmem);
		goto send;
	}

	/*
	 *  kill previous reply
	 */
	mf->nrr = 0;
	mf->rr[0] = 0;

	/*
	 *  break up request (into a name and a type)
	 */
	atype = strchr(job->request.data, ' ');
	if(atype == 0){
		err = "illegal request";
		goto send;
	} else
		*atype++ = 0;

	/*
	 *  tracing request
	 */
	if(strcmp(atype, "trace") == 0){
		if(trace)
			free(trace);
		if(*job->request.data)
			trace = estrdup(job->request.data);
		else
			trace = 0;
		goto send;
	}

	/* normal request: domain [type] */
	mf->type = rrtype(atype);
	if(mf->type < 0){
		err = "unknown type";
		goto send;
	}

	p = atype - 2;
	if(p >= job->request.data && *p == '.'){
		rooted = 1;
		*p = 0;
	} else
		rooted = 0;

	p = job->request.data;
	if(*p == '!'){
		wantsav = 1;
		p++;
	} else
		wantsav = 0;

	dncheck(0, 1);
	status = 0;
	rp = dnresolve(p, Cin, mf->type, req, 0, 0, Recurse, rooted, &status);

	dncheck(0, 1);
	neg = rrremneg(&rp);
	if(neg){
		status = neg->negrcode;
		rrfreelist(neg);
	}

	if(rp == nil)
		switch(status){
		case Rname:
			err = "name does not exist";
			break;
		case Rserver:
			err = "dns failure";
			break;
		default:
			err = "resource does not exist";
			break;
		}
	else {
		lock(&joblock);
		if(!job->flushed){
			/* format data to be read later */
			n = 0;
			mf->nrr = 0;
			for(tp = rp; mf->nrr < Maxrrr-1 && n < Maxreply && tp &&
			    tsame(mf->type, tp->type); tp = tp->next){
				mf->rr[mf->nrr++] = n;
				if(wantsav)
					n += snprint(mf->reply+n, Maxreply-n,
						"%Q", tp);
				else
					n += snprint(mf->reply+n, Maxreply-n,
						"%R", tp);
			}
			mf->rr[mf->nrr] = n;
		}
		unlock(&joblock);
		rrfreelist(rp);
	}
send:
	dncheck(0, 1);
	job->reply.count = cnt;
	sendmsg(job, err);
}

void
rclunk(Job *job, Mfile *mf)
{
	freefid(mf);
	sendmsg(job, 0);
}

void
rremove(Job *job, Mfile *mf)
{
	USED(mf);
	sendmsg(job, "remove permission denied");
}

void
rstat(Job *job, Mfile *mf)
{
	Dir dir;
	uchar buf[IOHDRSZ+Maxfdata];

	if(mf->qid.type & QTDIR){
		dir.name = ".";
		dir.mode = DMDIR|0555;
	} else {
		dir.name = "dns";
		dir.mode = 0666;
	}
	dir.qid = mf->qid;
	dir.length = 0;
	dir.uid = dir.gid = dir.muid = mf->user;
	dir.atime = dir.mtime = time(nil);
	job->reply.nstat = convD2M(&dir, buf, sizeof buf);
	job->reply.stat = buf;
	sendmsg(job, 0);
}

void
rwstat(Job *job, Mfile *mf)
{
	USED(mf);
	sendmsg(job, "wstat permission denied");
}

void
sendmsg(Job *job, char *err)
{
	int n;
	uchar mdata[IOHDRSZ + Maxfdata];
	char ename[ERRMAX];

	if(err){
		job->reply.type = Rerror;
		snprint(ename, sizeof ename, "dns: %s", err);
		job->reply.ename = ename;
	}else
		job->reply.type = job->request.type+1;
	job->reply.tag = job->request.tag;
	n = convS2M(&job->reply, mdata, sizeof mdata);
	if(n == 0){
		warning("sendmsg convS2M of %F returns 0", &job->reply);
		abort();
	}
	lock(&joblock);
	if(job->flushed == 0)
		if(write(mfd[1], mdata, n)!=n)
			sysfatal("mount write");
	unlock(&joblock);
	if(debug)
		dnslog("%F %d", &job->reply, n);
}

/*
 *  the following varies between dnsdebug and dns
 */
void
logreply(int id, uchar *addr, DNSmsg *mp)
{
	RR *rp;

	dnslog("%d: rcvd %I flags:%s%s%s%s%s", id, addr,
		mp->flags & Fauth? " auth": "",
		mp->flags & Ftrunc? " trunc": "",
		mp->flags & Frecurse? " rd": "",
		mp->flags & Fcanrec? " ra": "",
		(mp->flags & (Fauth|Rmask)) == (Fauth|Rname)? " nx": "");
	for(rp = mp->qd; rp != nil; rp = rp->next)
		dnslog("%d: rcvd %I qd %s", id, addr, rp->owner->name);
	for(rp = mp->an; rp != nil; rp = rp->next)
		dnslog("%d: rcvd %I an %R", id, addr, rp);
	for(rp = mp->ns; rp != nil; rp = rp->next)
		dnslog("%d: rcvd %I ns %R", id, addr, rp);
	for(rp = mp->ar; rp != nil; rp = rp->next)
		dnslog("%d: rcvd %I ar %R", id, addr, rp);
}

void
logsend(int id, int subid, uchar *addr, char *sname, char *rname, int type)
{
	char buf[12];

	dnslog("[%d] %d.%d: sending to %I/%s %s %s",
		getpid(), id, subid, addr, sname, rname,
		rrname(type, buf, sizeof buf));
}

RR*
getdnsservers(int class)
{
	return dnsservers(class);
}
