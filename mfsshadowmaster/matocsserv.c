/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA.

   This file is part of MooseFS.

   MooseFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   MooseFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with MooseFS.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <stdio.h>
#include <time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>

#include "MFSCommunication.h"

#include "datapack.h"
#include "matocsserv.h"
#include "cfg.h"
#include "main.h"
#include "sockets.h"
#include "chunks.h"
#include "random.h"

#define MaxPacketSize 500000000
/*print log control */
//#define LOG_COUNT 1000

static uint32_t log_got_chunk_checksum=0;
static uint32_t log_got_createchunk_status=0;
static uint32_t log_got_deletechunk_status=0;
static uint32_t	log_got_replicatechunk_status=0;
static uint32_t	log_got_setchunkversion_status=0;
static uint32_t	log_got_duplicatechunk_status=0;
static uint32_t	log_got_truncatechunk_status=0;
static uint32_t	log_got_duptruncchunk_status=0;
static uint32_t	log_got_chunkop_status=0;

static uint64_t maxtotalspace;
static serventry *matocsservhead=NULL;
static int lsock;
static int32_t lsockpdescpos;
static int first_add_listen_sock;

// from config
static char *ListenHost;
static char *ListenPort;
static uint32_t LOG_COUNT;

/* replications DB */

#define REPHASHSIZE 256
#define REPHASHFN(chid,ver) (((chid)^(ver)^((chid)>>8))%(REPHASHSIZE))

typedef struct _repsrc {
	void *src;
	struct _repsrc *next;
} repsrc;

typedef struct _repdst {
	uint64_t chunkid;
	uint32_t version;
	void *dst;
	repsrc *srchead;
	struct _repdst *next;
} repdst;

static repdst* rephash[REPHASHSIZE];
static repsrc *repsrcfreehead=NULL;
static repdst *repdstfreehead=NULL;

repsrc* matocsserv_repsrc_malloc() {
	repsrc *r;
	if (repsrcfreehead) {
		r = repsrcfreehead;
		repsrcfreehead = r->next;
	} else {
		r = (repsrc*)malloc(sizeof(repsrc));
	}
	return r;
}

void matocsserv_repsrc_free(repsrc *r) {
	r->next = repsrcfreehead;
	repsrcfreehead = r;
}

repdst* matocsserv_repdst_malloc() {
	repdst *r;
	if (repdstfreehead) {
		r = repdstfreehead;
		repdstfreehead = r->next;
	} else {
		r = (repdst*)malloc(sizeof(repdst));
	}
	return r;
}

void matocsserv_repdst_free(repdst *r) {
	r->next = repdstfreehead;
	repdstfreehead = r;
}

void matocsserv_replication_init(void) {
	uint32_t hash;
	for (hash=0 ; hash<REPHASHSIZE ; hash++) {
		rephash[hash]=NULL;
	}
	repsrcfreehead=NULL;
	repdstfreehead=NULL;
}

int matocsserv_replication_find(uint64_t chunkid,uint32_t version,void *dst) {
	uint32_t hash = REPHASHFN(chunkid,version);
	repdst *r;
	for (r=rephash[hash] ; r ; r=r->next) {
		if (r->chunkid==chunkid && r->version==version && r->dst==dst) {
			return 1;
		}
	}
	return 0;
}

void matocsserv_replication_begin(uint64_t chunkid,uint32_t version,void *dst,uint8_t srccnt,void **src) {
	uint32_t hash = REPHASHFN(chunkid,version);
	uint8_t i;
	repdst *r;
	repsrc *rs;

	if (srccnt>0) {
		r = matocsserv_repdst_malloc();
		r->chunkid = chunkid;
		r->version = version;
		r->dst = dst;
		r->srchead = NULL;
		r->next = rephash[hash];
		rephash[hash] = r;
		for (i=0 ; i<srccnt ; i++) {
			rs = matocsserv_repsrc_malloc();
			rs->src = src[i];
			rs->next = r->srchead;
			r->srchead = rs;
			((serventry *)(src[i]))->rrepcounter++;
		}
		((serventry *)(dst))->wrepcounter++;
	}
}

void matocsserv_replication_end(uint64_t chunkid,uint32_t version,void *dst) {
	uint32_t hash = REPHASHFN(chunkid,version);
	repdst *r,**rp;
	repsrc *rs,*rsdel;

	rp = &(rephash[hash]);
	while ((r=*rp)!=NULL) {
		if (r->chunkid==chunkid && r->version==version && r->dst==dst) {
			rs = r->srchead;
			while (rs) {
				rsdel = rs;
				rs = rs->next;
				((serventry *)(rsdel->src))->rrepcounter--;
				matocsserv_repsrc_free(rsdel);
			}
			((serventry *)(dst))->wrepcounter--;
			*rp = r->next;
			matocsserv_repdst_free(r);
		} else {
			rp = &(r->next);
		}
	}
}

void matocsserv_replication_disconnected(void *srv) {
	uint32_t hash;
	repdst *r,**rp;
	repsrc *rs,*rsdel,**rsp;

	for (hash=0 ; hash<REPHASHSIZE ; hash++) {
		rp = &(rephash[hash]);
		while ((r=*rp)!=NULL) {
			if (r->dst==srv) {
				rs = r->srchead;
				while (rs) {
					rsdel = rs;
					rs = rs->next;
					((serventry *)(rsdel->src))->rrepcounter--;
					matocsserv_repsrc_free(rsdel);
				}
				((serventry *)(srv))->wrepcounter--;
				*rp = r->next;
				matocsserv_repdst_free(r);
			} else {
				rsp = &(r->srchead);
				while ((rs=*rsp)!=NULL) {
					if (rs->src==srv) {
						((serventry *)(srv))->rrepcounter--;
						*rsp = rs->next;
						matocsserv_repsrc_free(rs);
					} else {
						rsp = &(rs->next);
					}
				}
				rp = &(r->next);
			}
		}
	}
}

/* replication DB END */




int matocsserv_space_compare(const void *a,const void *b) {
	const struct servsort {
		double space;
		void *ptr;
	} *aa=a,*bb=b;
	if (aa->space > bb->space) {
		return 1;
	}
	if (aa->space < bb->space) {
		return -1;
	}
	return 0;
}

void matocsserv_usagedifference(double *minusage,double *maxusage,uint16_t *usablescount,uint16_t *totalscount) {
	serventry *eptr;
	uint32_t j,k;
	double minspace=1.0,maxspace=0.0;
	double space;
	j = 0;
	k = 0;
	for (eptr = matocsservhead ; eptr && j<65535 && k<65535; eptr=eptr->next) {
		if (eptr->mode!=KILL && eptr->listen_sock==0) {
			if (eptr->totalspace>0 && eptr->usedspace<=eptr->totalspace) {
				space = (double)(eptr->usedspace) / (double)(eptr->totalspace);
				if (j==0) {
					minspace = maxspace = space;
				} else if (space<minspace) {
					minspace = space;
				} else if (space>maxspace) {
					maxspace = space;
				}
				j++;
			}
			k++;
		}
	}
	*usablescount = j;
	*totalscount = k;
	if (j==0) {
		*minusage = 1.0;
		*maxusage = 0.0;
	} else {
		*minusage = minspace;
		*maxusage = maxspace;
	}
}

uint16_t matocsserv_getservers_ordered(void* ptrs[65535],double maxusagediff,uint32_t *pmin,uint32_t *pmax) {
	static struct servsort {
		double space;
		void *ptr;
	} servsorttab[65535],servtab[65536];
	serventry *eptr;
	uint32_t i,j,k,min,mid,max;
	double minspace=1.0,maxspace=0.0;
	uint64_t tspace,uspace;
	double space;

//	syslog(LOG_NOTICE,"getservers start");
	j = 0;
	tspace = 0;
	uspace = 0;
	for (eptr = matocsservhead ; eptr && j<65535; eptr=eptr->next) {
		if (eptr->mode!=KILL && eptr->totalspace>0 && eptr->usedspace<=eptr->totalspace && eptr->listen_sock==0) {
			uspace += eptr->usedspace;
			tspace += eptr->totalspace;
			space = (double)(eptr->usedspace) / (double)(eptr->totalspace);
			if (j==0) {
				minspace = maxspace = space;
			} else if (space<minspace) {
				minspace = space;
			} else if (space>maxspace) {
				maxspace = space;
			}
			servtab[j].ptr = eptr;
			servtab[j].space = space;
//			syslog(LOG_NOTICE,"ptr: %p, space:%lf",eptr,space);
			j++;
		}
	}
	if (j==0) {
//		syslog(LOG_NOTICE,"getservers - noservers");
		return 0;
	}

	space = (double)(uspace)/(double)(tspace);
//	syslog(LOG_NOTICE,"getservers - minspace: %lf , maxspace: %lf , diff: %lf , maxusagediff: %lf",minspace,maxspace,maxspace-minspace,maxusagediff);
//	if (maxspace-minspace<=maxusagediff*2) {
//		maxusagediff = (maxspace-minspace)/2.0;
//	}
	min = 0;
	max = j;
	mid = 0;
	for (i=0 ; i<j ; i++) {
		if (servtab[i].space<space-maxusagediff) {
			ptrs[min++]=servtab[i].ptr;
		} else if (servtab[i].space>space+maxusagediff) {
			ptrs[--max]=servtab[i].ptr;
		} else {
			servsorttab[mid++]=servtab[i];
		}
//		if (servtab[i].space-minspace<maxusagediff) {
//			ptrs[min++]=servtab[i].ptr;
//		} else if (maxspace-servtab[i].space<maxusagediff) {
//			ptrs[--max]=servtab[i].ptr;
//		} else {
//			servsorttab[mid++]=servtab[i];
//		}
	}

	// random <0-min)
	for (i=0 ; i<min ; i++) {
		// k = random <i,j)
		k = i+(rndu32()%(min-i));
		// swap(i,k)
		if (i!=k) {
			void* p = ptrs[i];
			ptrs[i] = ptrs[k];
			ptrs[k] = p;
		}
	}

	// random <max-j)
	for (i=max ; i<j ; i++) {
		// k = random <i,j)
		k = i+(rndu32()%(j-i));
		// swap(i,k)
		if (i!=k) {
			void* p = ptrs[i];
			ptrs[i] = ptrs[k];
			ptrs[k] = p;
		}
	}

	// sort <min-max)
	if (mid>0) {
		qsort(servsorttab,mid,sizeof(struct servsort),matocsserv_space_compare);
	}
	for (i=0 ; i<mid ; i++) {
		ptrs[min+i]=servsorttab[i].ptr;
	}
	if (pmin!=NULL) {
		*pmin=min;
	}
	if (pmax!=NULL) {
		*pmax=j-max;
	}
//		syslog(LOG_NOTICE,"getservers <0-%"PRIu32") random ; <%"PRIu32"-%"PRIu32") sort ; <%"PRIu32"-END) random",min,min,max,max);
//		for (i=0 ; i<j ; i++) {
//			syslog(LOG_NOTICE,"getservers - s%"PRIu32" : %p",i,ptrs[i]);
//		}
	return j;
}


int matocsserv_carry_compare(const void *a,const void *b) {
	const struct rservsort {
		double w;
		double carry;
		serventry *ptr;
	} *aa=a,*bb=b;
	if (aa->carry > bb->carry) {
		return -1;
	}
	if (aa->carry < bb->carry) {
		return 1;
	}
	return 0;
}

uint16_t matocsserv_getservers_wrandom(void* ptrs[65536],uint16_t demand) {
	static struct rservsort {
		double w;
		double carry;
		serventry *ptr;
	} servtab[65536];
	serventry *eptr;
	double carry;
	uint32_t i;
	uint32_t allcnt;
	uint32_t availcnt;
	if (maxtotalspace==0) {
		return 0;
	}
	allcnt=0;
	availcnt=0;
	for (eptr = matocsservhead ; eptr && allcnt<65536 ; eptr=eptr->next) {
		if (eptr->mode!=KILL && eptr->totalspace>0 && eptr->usedspace<=eptr->totalspace && (eptr->totalspace - eptr->usedspace)>(1<<30) && eptr->listen_sock==0) {
			servtab[allcnt].w = (double)eptr->totalspace/(double)maxtotalspace;
			servtab[allcnt].carry = eptr->carry;
			servtab[allcnt].ptr = eptr;
			allcnt++;
			if (eptr->carry>=1.0) {
				availcnt++;
			}
		}
	}
	if (demand>allcnt) {
		demand=allcnt;
	}
	while (availcnt<demand) {
		availcnt=0;
		for (i=0 ; i<allcnt ; i++) {
			carry = servtab[i].carry + servtab[i].w;
			servtab[i].carry = carry;
			servtab[i].ptr->carry = carry;
			if (carry>=1.0) {
				availcnt++;
			}
		}
	}
	qsort(servtab,allcnt,sizeof(struct rservsort),matocsserv_carry_compare);
	for (i=0 ; i<demand ; i++) {
		ptrs[i] = servtab[i].ptr;
		servtab[i].ptr->carry-=1.0;
	}
	return demand;
}

/*
uint16_t matocsserv_getservers_wrandom(void* ptrs[65535],uint16_t demand,uint32_t cuip) {
	static struct rservsort {
		uint32_t p;
		uint32_t srt;
//		double rndcarry;
		serventry *ptr;
	} servtab[65536],x;
	serventry *eptr;
	double maxrndcarry;
	int32_t local;
	uint32_t psum;
	uint32_t r,j,i,k;
	j = 0;
	psum = 0;
	maxrndcarry = 0.0;
	local = -1;
	for (eptr = matocsservhead ; eptr && j<65535; eptr=eptr->next) {
		if (eptr->mode!=KILL && eptr->totalspace>0 && eptr->usedspace<=eptr->totalspace && (eptr->totalspace - eptr->usedspace)>(1<<30)) {
			if (eptr->rndcarry>maxrndcarry) {
				maxrndcarry = eptr->rndcarry;
			}
			if (eptr->servip==cuip) {
				local=j;
			}
			servtab[j].p = eptr->totalspace>>30;
			if (eptr->rndcarry>=1.0) {
				servtab[j].srt = rndu32()|0x80000000;
			} else {
				servtab[j].srt = rndu32()&0x7FFFFFFF;
			}
//			servtab[j].rndcarry = eptr->rndcarry;
			servtab[j].ptr=eptr;
			psum += servtab[j].p;
			j++;
		}
	}
	if (j==0) {
		return 0;
	}
	if (demand>j) {
		demand=j;
	}
	if (local>=0 && servtab[local].ptr->rndcarry>=1.0) {	// localhost can be used
		// place localhost in the first place
		if (local!=0) {
			x = servtab[0];
			servtab[0] = servtab[local];
			servtab[local] = x;
		}
		// sort the rest
		if (j>1) {
			qsort(servtab+1,j-1,sizeof(struct rservsort),matocsserv_rndcarry_compare);
		}
	} else if (local>=0 && maxrndcarry<10.0) { // localhost can be forced
		// bias rndcarry
		for (i=0 ; i<j ; i++) {
			if ((servtab[i].ptr->rndcarry += (double)(servtab[i].p)/(double)(servtab[local].p))>=1.0) {
				servtab[i].srt |= 0x80000000;
			} else {
				servtab[i].srt &= 0x7FFFFFFF;
			}
		}
		// place localhost in the first place
		if (local!=0) {
			x = servtab[0];
			servtab[0] = servtab[local];
			servtab[local] = x;
		}
		// sort the rest
		if (j>1) {
			qsort(servtab+1,j-1,sizeof(struct rservsort),matocsserv_rndcarry_compare);
		}
	} else { // localhost can't be used
		qsort(servtab,j,sizeof(struct rservsort),matocsserv_rndcarry_compare);
	}
//	k = j-1;
//	while (i<k) {
//		while (i<k && servtab[i].ptr->rndcarry>=1.0) i++;
//		while (i<k && servtab[k].ptr->rndcarry<1.0) k--;
//		if (i<k) {
//			x = servtab[i];
//			servtab[i] = servtab[k];
//			servtab[k] = x;
//		}
//	}
	for (k=0 ; k<demand ; k++) {
		if (servtab[k].ptr->rndcarry>=1.0) {
			servtab[k].ptr->rndcarry-=1.0;
			// found server with carry (previously choosen at least once) - so use it
		} else {
			do {
				// r = random <0,psum)
				r = rndu32()%psum;
				// choose randomly one of 'j' servers with propability servtab[i].p/psum (for i from 0 to j-1)
				for (i=0 ; i<j && r>=servtab[i].p ; i++) {
					r-=servtab[i].p;
				}
				if (i<k) {	// server was choosen before
					servtab[i].ptr->rndcarry+=1.0;
					r = 1;
				} else {
					r = 0;
					if (i>k) {
						x = servtab[i];
						servtab[i] = servtab[k];
						servtab[k] = x;
					}
				}
			} while (r);
		}
		ptrs[k] = servtab[k].ptr;
	}
	return demand;
}
*/

uint16_t matocsserv_getservers_lessrepl(void* ptrs[65535],uint16_t replimit) {
	serventry *eptr;
	uint32_t j,k,r;
	void *x;
	j=0;
	for (eptr = matocsservhead ; eptr && j<65535; eptr=eptr->next) {
		if (eptr->mode!=KILL && eptr->totalspace>0 && eptr->usedspace<=eptr->totalspace && (eptr->totalspace - eptr->usedspace)>(eptr->totalspace/100) && eptr->wrepcounter<replimit && eptr->listen_sock==0) {
			ptrs[j] = (void*)eptr;
			j++;
		}
	}
	if (j==0) {
		return 0;
	}
	for (k=0 ; k<j-1 ; k++) {
		r = k + rndu32()%(j-k);
		if (r!=k) {
			x = ptrs[k];
			ptrs[k] = ptrs[r];
			ptrs[r] = x;
		}
	}
	return j;
}

void matocsserv_getspace(uint64_t *totalspace,uint64_t *availspace) {
	serventry *eptr;
	uint64_t tspace,uspace;
	tspace = 0;
	uspace = 0;
	for (eptr = matocsservhead ; eptr ; eptr=eptr->next) {
		if (eptr->mode!=KILL && eptr->totalspace>0 && eptr->listen_sock==0) {
			tspace += eptr->totalspace;
			uspace += eptr->usedspace;
		}
	}
	*totalspace = tspace;
	*availspace = tspace-uspace;
}

/*
int matocsserv_balanced(double balancelimit) {
	serventry *eptr;
	uint64_t tspace,uspace;
	double min,max,x;
	tspace = 0;
	uspace = 0;
	min = 1.0;
	max = 0.0;
	for (eptr = matocsservhead ; eptr ; eptr=eptr->next) {
		if (eptr->mode!=KILL && eptr->totalspace>0) {
			tspace += eptr->totalspace;
			uspace += eptr->usedspace;
			x = (double)(eptr->usedspace)/(double)(eptr->totalspace);
			if (x>max) {
				max=x;
			}
			if (x<min) {
				min=x;
			}
		}
	}
	x = (double)uspace/(double)tspace;
	if (max<min) {
		return 1;
	}
	if (min>=x-limit && max<=x+limit) {
		return 1;
	}
	return 0;
}
*/
uint32_t matocsserv_cservlist_size(void) {
	serventry *eptr;
	uint32_t i;
	i=0;
	for (eptr = matocsservhead ; eptr ; eptr=eptr->next) {
		if (eptr->mode!=KILL && eptr->listen_sock==0) {
			i++;
		}
	}
	return i*(4+4+2+8+8+4+8+8+4+4);
}

void matocsserv_cservlist_data(uint8_t *ptr) {
	serventry *eptr;
	for (eptr = matocsservhead ; eptr ; eptr=eptr->next) {
		if (eptr->mode!=KILL && eptr->listen_sock==0) {
			put32bit(&ptr,eptr->version);
			put32bit(&ptr,eptr->servip);
			put16bit(&ptr,eptr->servport);
			put64bit(&ptr,eptr->usedspace);
			put64bit(&ptr,eptr->totalspace);
			put32bit(&ptr,eptr->chunkscount);
			put64bit(&ptr,eptr->todelusedspace);
			put64bit(&ptr,eptr->todeltotalspace);
			put32bit(&ptr,eptr->todelchunkscount);
			put32bit(&ptr,eptr->errorcounter);
		}
	}
}

void log_print_control(void) {
	log_got_chunk_checksum=0;
	log_got_createchunk_status=0;
	log_got_deletechunk_status=0;
	log_got_replicatechunk_status=0;
	log_got_setchunkversion_status=0;
	log_got_duplicatechunk_status=0;
	log_got_truncatechunk_status=0;
	log_got_duptruncchunk_status=0;
	log_got_chunkop_status=0;
}

char* matocsserv_getstrip(void *e) {
	serventry *eptr = (serventry *)e;
	static char *empty="???";
	if (eptr->mode!=KILL && eptr->servstrip) {
		return eptr->servstrip;
	}
	return empty;
}

int matocsserv_getlocation(void *e,uint32_t *servip,uint16_t *servport) {
	serventry *eptr = (serventry *)e;
	if (eptr->mode!=KILL) {
		*servip = eptr->servip;
		*servport = eptr->servport;
		return 0;
	}
	return -1;
}


uint16_t matocsserv_replication_write_counter(void *e) {
	serventry *eptr = (serventry *)e;
	return eptr->wrepcounter;
}

uint16_t matocsserv_replication_read_counter(void *e) {
	serventry *eptr = (serventry *)e;
	return eptr->rrepcounter;
}

char* matocsserv_makestrip(uint32_t ip) {
	uint8_t *ptr,pt[4];
	uint32_t l,i;
	char *optr;
	ptr = pt;
	put32bit(&ptr,ip);
	l=0;
	for (i=0 ; i<4 ; i++) {
		if (pt[i]>=100) {
			l+=3;
		} else if (pt[i]>=10) {
			l+=2;
		} else {
			l+=1;
		}
	}
	l+=4;
	optr = malloc(l);
	snprintf(optr,l,"%"PRIu8".%"PRIu8".%"PRIu8".%"PRIu8,pt[0],pt[1],pt[2],pt[3]);
	optr[l-1]=0;
	return optr;
}

uint8_t* matocsserv_createpacket(serventry *eptr,uint32_t type,uint32_t size) {
	packetstruct *outpacket;
	uint8_t *ptr;
	uint32_t psize;

	outpacket=(packetstruct*)malloc(sizeof(packetstruct));
	if (outpacket==NULL) {
		return NULL;
	}
	psize = size+8;
	outpacket->packet=malloc(psize);
	outpacket->bytesleft = psize;
	if (outpacket->packet==NULL) {
		free(outpacket);
		return NULL;
	}
	ptr = outpacket->packet;
	put32bit(&ptr,type);
	put32bit(&ptr,size);
	outpacket->startptr = (uint8_t*)(outpacket->packet);
	outpacket->next = NULL;
	*(eptr->outputtail) = outpacket;
	eptr->outputtail = &(outpacket->next);
	return ptr;
}
/* for future use */
int matocsserv_send_chunk_checksum(void *e,uint64_t chunkid,uint32_t version) {
	serventry *eptr = (serventry *)e;
	uint8_t *data;

	if (eptr->mode!=KILL) {
		data = matocsserv_createpacket(eptr,ANTOCS_CHUNK_CHECKSUM,8+4);
		if (data==NULL) {
			return -1;
		}
		put64bit(&data,chunkid);
		put32bit(&data,version);
	}
	return 0;
}
/* for future use */
void matocsserv_got_chunk_checksum(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint64_t chunkid;
	uint32_t version,checksum;
	uint8_t status;
	if (length!=8+4+1 && length!=8+4+4) {
		MFSLOG(LOG_NOTICE,"CSTOAN_CHUNK_CHECKSUM - wrong size (%"PRIu32"/13|16)",length);
		eptr->mode=KILL;
		return ;
	}
	chunkid = get64bit(&data);
	version = get32bit(&data);
	if (length==8+4+1) {
		status = get8bit(&data);
//		chunk_got_checksum_status(eptr,chunkid,version,status);
                log_got_chunk_checksum %= LOG_COUNT;
                if (log_got_chunk_checksum++ == 0) {
			MFSLOG(LOG_NOTICE,"(%s:%"PRIu16") chunk: %016"PRIX64" calculate checksum status: %"PRIu8,eptr->servstrip,eptr->servport,chunkid,status);
		}
	} else {
		checksum = get32bit(&data);
//		chunk_got_checksum(eptr,chunkid,version,checksum);
		log_got_chunk_checksum %= LOG_COUNT;
                if (log_got_chunk_checksum++ == 0) {
			MFSLOG(LOG_NOTICE,"(%s:%"PRIu16") chunk: %016"PRIX64" calculate checksum: %08"PRIX32,eptr->servstrip,eptr->servport,chunkid,checksum);
		}
	}
}

int matocsserv_send_createchunk(void *e,uint64_t chunkid,uint32_t version) {
	serventry *eptr = (serventry *)e;
	uint8_t *data;

	if (eptr->mode!=KILL) {
		data = matocsserv_createpacket(eptr,MATOCS_CREATE,8+4);
		if (data==NULL) {
			return -1;
		}
		put64bit(&data,chunkid);
		put32bit(&data,version);
	}
	return 0;
}

void matocsserv_got_createchunk_status(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint64_t chunkid;
	uint8_t status;
	if (length!=8+1) {
		MFSLOG(LOG_NOTICE,"CSTOMA_CREATE - wrong size (%"PRIu32"/9)",length);
		eptr->mode=KILL;
		return;
	}
	chunkid = get64bit(&data);
	status = get8bit(&data);
	chunk_got_create_status(eptr,chunkid,status);
	if (status!=0) {
		log_got_createchunk_status %= LOG_COUNT;
                if (log_got_createchunk_status++ == 0) {
			MFSLOG(LOG_NOTICE,"(%s:%"PRIu16") chunk: %016"PRIX64" creation status: %"PRIu8,eptr->servstrip,eptr->servport,chunkid,status);
		}
	}
}

int matocsserv_send_deletechunk(void *e,uint64_t chunkid,uint32_t version) {
	serventry *eptr = (serventry *)e;
	uint8_t *data;

	if (eptr->mode!=KILL) {
		data = matocsserv_createpacket(eptr,MATOCS_DELETE,8+4);
		if (data==NULL) {
			return -1;
		}
		put64bit(&data,chunkid);
		put32bit(&data,version);
	}
	return 0;
}

void matocsserv_got_deletechunk_status(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint64_t chunkid;
	uint8_t status;
	if (length!=8+1) {
		MFSLOG(LOG_NOTICE,"CSTOMA_DELETE - wrong size (%"PRIu32"/9)",length);
		eptr->mode=KILL;
		return;
	}
	chunkid = get64bit(&data);
	status = get8bit(&data);
	chunk_got_delete_status(eptr,chunkid,status);
	if (status!=0) {
		log_got_deletechunk_status %= LOG_COUNT;
                if (log_got_deletechunk_status++ == 0) {
			MFSLOG(LOG_NOTICE,"(%s:%"PRIu16") chunk: %016"PRIX64" deletion status: %"PRIu8,eptr->servstrip,eptr->servport,chunkid,status);
		}
	}
}

int matocsserv_send_replicatechunk(void *e,uint64_t chunkid,uint32_t version,void *src) {
	serventry *eptr = (serventry *)e;
	serventry *srceptr = (serventry *)src;
	uint8_t *data;

	if (matocsserv_replication_find(chunkid,version,eptr)) {
		return -1;
	}
	if (eptr->mode!=KILL && srceptr->mode!=KILL) {
		data = matocsserv_createpacket(eptr,MATOCS_REPLICATE,8+4+4+2);
		if (data==NULL) {
			return -1;
		}
		put64bit(&data,chunkid);
		put32bit(&data,version);
		put32bit(&data,srceptr->servip);
		put16bit(&data,srceptr->servport);
		matocsserv_replication_begin(chunkid,version,eptr,1,&src);
		eptr->carry = 0;
	}
	return 0;
}

int matocsserv_send_replicatechunk_xor(void *e,uint64_t chunkid,uint32_t version,uint8_t cnt,void **src,uint64_t *srcchunkid,uint32_t *srcversion) {
	serventry *eptr = (serventry *)e;
	serventry *srceptr;
	uint8_t i;
	uint8_t *data;

	if (matocsserv_replication_find(chunkid,version,eptr)) {
		return -1;
	}
	if (eptr->mode!=KILL) {
		for (i=0 ; i<cnt ; i++) {
			srceptr = (serventry *)(src[i]);
			if (srceptr->mode==KILL) {
				return 0;
			}
		}
		data = matocsserv_createpacket(eptr,MATOCS_REPLICATE,8+4+cnt*(8+4+4+2));
		if (data==NULL) {
			return -1;
		}
		put64bit(&data,chunkid);
		put32bit(&data,version);
		for (i=0 ; i<cnt ; i++) {
			srceptr = (serventry *)(src[i]);
			put64bit(&data,srcchunkid[i]);
			put32bit(&data,srcversion[i]);
			put32bit(&data,srceptr->servip);
			put16bit(&data,srceptr->servport);
		}
		matocsserv_replication_begin(chunkid,version,eptr,cnt,src);
		eptr->carry = 0;
	}
	return 0;
}

/*
int matocsserv_send_replicatechunk(void *e,uint64_t chunkid,uint32_t version,uint32_t ip,uint16_t port) {
	serventry *eptr = (serventry *)e;
	uint8_t *data;

	if (eptr->mode!=KILL) {
		data = matocsserv_createpacket(eptr,MATOCS_REPLICATE,8+4+4+2);
		if (data==NULL) {
			return -1;
		}
		put64bit(&data,chunkid);
		put32bit(&data,version);
		put32bit(&data,ip);
		put16bit(&data,port);
		eptr->repcounter++;
	}
	return 0;
}

int matocsserv_send_replicatechunk_xor(void *e,uint64_t chunkid,uint32_t version,uint8_t cnt,uint8_t *fromdata) {
	serventry *eptr = (serventry *)e;
	uint8_t *data;

	if (eptr->mode!=KILL) {
		data = matocsserv_createpacket(eptr,MATOCS_REPLICATE,8+4+cnt*(8+4+4+2));
		if (data==NULL) {
			return -1;
		}
		put64bit(&data,chunkid);
		put32bit(&data,version);
		memcpy(data,fromdata,cnt*(8+4+4+2));
		eptr->repcounter++;
	}
	return 0;
}
*/

void matocsserv_got_replicatechunk_status(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint64_t chunkid;
	uint32_t version;
	uint8_t status;
	if (length!=8+4+1) {
		MFSLOG(LOG_NOTICE,"CSTOMA_REPLICATE - wrong size (%"PRIu32"/13)",length);
		eptr->mode=KILL;
		return;
	}
//	if (eptr->repcounter>0) {
//		eptr->repcounter--;
//	}
	chunkid = get64bit(&data);
	version = get32bit(&data);
	matocsserv_replication_end(chunkid,version,eptr);
	status = get8bit(&data);
	chunk_got_replicate_status(eptr,chunkid,version,status);
	if (status!=0) {
		log_got_replicatechunk_status %= LOG_COUNT;
                if (log_got_replicatechunk_status++ == 0) {
			MFSLOG(LOG_NOTICE,"(%s:%"PRIu16") chunk: %016"PRIX64" replication status: %"PRIu8,eptr->servstrip,eptr->servport,chunkid,status);
		}
	}
}

int matocsserv_send_setchunkversion(void *e,uint64_t chunkid,uint32_t version,uint32_t oldversion) {
	serventry *eptr = (serventry *)e;
	uint8_t *data;

	if (eptr->mode!=KILL) {
		data = matocsserv_createpacket(eptr,MATOCS_SET_VERSION,8+4+4);
		if (data==NULL) {
			return -1;
		}
		put64bit(&data,chunkid);
		put32bit(&data,version);
		put32bit(&data,oldversion);
	}
	return 0;
}

void matocsserv_got_setchunkversion_status(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint64_t chunkid;
	uint8_t status;
	if (length!=8+1) {
		MFSLOG(LOG_NOTICE,"CSTOMA_SET_VERSION - wrong size (%"PRIu32"/9)",length);
		eptr->mode=KILL;
		return;
	}
	chunkid = get64bit(&data);
	status = get8bit(&data);
	chunk_got_setversion_status(eptr,chunkid,status);
	if (status!=0) {
		log_got_setchunkversion_status %= LOG_COUNT;
                if (log_got_setchunkversion_status++ == 1) {
			MFSLOG(LOG_NOTICE,"(%s:%"PRIu16") chunk: %016"PRIX64" set version status: %"PRIu8,eptr->servstrip,eptr->servport,chunkid,status);
		}
	}
}


int matocsserv_send_duplicatechunk(void *e,uint64_t chunkid,uint32_t version,uint64_t oldchunkid,uint32_t oldversion) {
	serventry *eptr = (serventry *)e;
	uint8_t *data;

	if (eptr->mode!=KILL) {
		data = matocsserv_createpacket(eptr,MATOCS_DUPLICATE,8+4+8+4);
		if (data==NULL) {
			return -1;
		}
		put64bit(&data,chunkid);
		put32bit(&data,version);
		put64bit(&data,oldchunkid);
		put32bit(&data,oldversion);
	}
	return 0;
}

void matocsserv_got_duplicatechunk_status(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint64_t chunkid;
	uint8_t status;
	if (length!=8+1) {
		MFSLOG(LOG_NOTICE,"CSTOMA_DUPLICATE - wrong size (%"PRIu32"/9)",length);
		eptr->mode=KILL;
		return;
	}
	chunkid = get64bit(&data);
	status = get8bit(&data);
	chunk_got_duplicate_status(eptr,chunkid,status);
	if (status!=0) {
		log_got_duplicatechunk_status %= LOG_COUNT;
                if (log_got_duplicatechunk_status++ == 0) {
			MFSLOG(LOG_NOTICE,"(%s:%"PRIu16") chunk: %016"PRIX64" duplication status: %"PRIu8,eptr->servstrip,eptr->servport,chunkid,status);
		}
	}
}

int matocsserv_send_truncatechunk(void *e,uint64_t chunkid,uint32_t length,uint32_t version,uint32_t oldversion) {
	serventry *eptr = (serventry *)e;
	uint8_t *data;

	if (eptr->mode!=KILL) {
		data = matocsserv_createpacket(eptr,MATOCS_TRUNCATE,8+4+4+4);
		if (data==NULL) {
			return -1;
		}
		put64bit(&data,chunkid);
		put32bit(&data,length);
		put32bit(&data,version);
		put32bit(&data,oldversion);
	}
	return 0;
}

void matocsserv_got_truncatechunk_status(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint64_t chunkid;
	uint8_t status;
	if (length!=8+1) {
		MFSLOG(LOG_NOTICE,"CSTOMA_TRUNCATE - wrong size (%"PRIu32"/9)",length);
		eptr->mode=KILL;
		return;
	}
	chunkid = get64bit(&data);
	status = get8bit(&data);
	chunk_got_truncate_status(eptr,chunkid,status);
//	matocsserv_notify(&(eptr->duplication),eptr,chunkid,status);
	if (status!=0) {
		log_got_truncatechunk_status %= LOG_COUNT;
                if (log_got_truncatechunk_status++ == 1) {	
			MFSLOG(LOG_NOTICE,"(%s:%"PRIu16") chunk: %016"PRIX64" truncate status: %"PRIu8,eptr->servstrip,eptr->servport,chunkid,status);
		}
	}
}

int matocsserv_send_duptruncchunk(void *e,uint64_t chunkid,uint32_t version,uint64_t oldchunkid,uint32_t oldversion,uint32_t length) {
	serventry *eptr = (serventry *)e;
	uint8_t *data;

	if (eptr->mode!=KILL) {
		data = matocsserv_createpacket(eptr,MATOCS_DUPTRUNC,8+4+8+4+4);
		if (data==NULL) {
			return -1;
		}
		put64bit(&data,chunkid);
		put32bit(&data,version);
		put64bit(&data,oldchunkid);
		put32bit(&data,oldversion);
		put32bit(&data,length);
	}
	return 0;
}

void matocsserv_got_duptruncchunk_status(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint64_t chunkid;
	uint8_t status;
	if (length!=8+1) {
		MFSLOG(LOG_NOTICE,"CSTOMA_DUPTRUNC - wrong size (%"PRIu32"/9)",length);
		eptr->mode=KILL;
		return;
	}
	chunkid = get64bit(&data);
	status = get8bit(&data);
	chunk_got_duptrunc_status(eptr,chunkid,status);
//	matocsserv_notify(&(eptr->duplication),eptr,chunkid,status);
	if (status!=0) {
		log_got_duptruncchunk_status %= LOG_COUNT;
                if (log_got_duptruncchunk_status++ == 1) {
			MFSLOG(LOG_NOTICE,"(%s:%"PRIu16") chunk: %016"PRIX64" duplication with truncate status: %"PRIu8,eptr->servstrip,eptr->servport,chunkid,status);
		}
	}
}

int matocsserv_send_chunkop(void *e,uint64_t chunkid,uint32_t version,uint32_t newversion,uint64_t copychunkid,uint32_t copyversion,uint32_t leng) {
	serventry *eptr = (serventry *)e;
	uint8_t *data;

	if (eptr->mode!=KILL) {
		data = matocsserv_createpacket(eptr,MATOCS_CHUNKOP,8+4+4+8+4+4);
		if (data==NULL) {
			return -1;
		}
		put64bit(&data,chunkid);
		put32bit(&data,version);
		put32bit(&data,newversion);
		put64bit(&data,copychunkid);
		put32bit(&data,copyversion);
		put32bit(&data,leng);
	}
	return 0;
}

void matocsserv_got_chunkop_status(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint64_t chunkid,copychunkid;
	uint32_t version,newversion,copyversion,leng;
	uint8_t status;
	if (length!=8+4+4+8+4+4+1) {
		MFSLOG(LOG_NOTICE,"CSTOMA_CHUNKOP - wrong size (%"PRIu32"/33)",length);
		eptr->mode=KILL;
		return;
	}
	chunkid = get64bit(&data);
	version = get32bit(&data);
	newversion = get32bit(&data);
	copychunkid = get64bit(&data);
	copyversion = get32bit(&data);
	leng = get32bit(&data);
	status = get8bit(&data);
	if (newversion!=version) {
		chunk_got_chunkop_status(eptr,chunkid,status);
	}
	if (copychunkid>0) {
		chunk_got_chunkop_status(eptr,copychunkid,status);
	}
	if (status!=0) {
		log_got_chunkop_status %= LOG_COUNT;
                if (log_got_chunkop_status++ == 0) {
			MFSLOG(LOG_NOTICE,"(%s:%"PRIu16") chunkop(%016"PRIX64",%08"PRIX32",%08"PRIX32",%016"PRIX64",%08"PRIX32",%"PRIu32") status: %"PRIu8,eptr->servstrip,eptr->servport,chunkid,version,newversion,copychunkid,copyversion,leng,status);
		}
	}
}

void matocsserv_register(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint64_t chunkid;
	uint32_t chunkversion;
	serventry *eaptr;
	uint32_t i,chunkcount;
	uint8_t rversion;
	double us,ts;

	if (eptr->servip>0 || eptr->servport>0) {
		MFSLOG(LOG_WARNING,"got register message from registered chunk-server !!!");
		eptr->mode=KILL;
		return;
	}
	if ((length&1)==0) {
		if (length<22 || ((length-22)%12)!=0) {
			MFSLOG(LOG_NOTICE,"CSTOMA_REGISTER (old ver.) - wrong size (%"PRIu32"/22+N*12)",length);
			eptr->mode=KILL;
			return;
		}
		eptr->servip = get32bit(&data);
		eptr->servport = get16bit(&data);
		eptr->usedspace = get64bit(&data);
		eptr->totalspace = get64bit(&data);
		length-=22;
	} else {
		rversion = get8bit(&data);
		MFSLOG(LOG_NOTICE,"register packet version: %u",rversion);
		if (rversion==1) {
			if (length<39 || ((length-39)%12)!=0) {
				MFSLOG(LOG_NOTICE,"CSTOMA_REGISTER (ver 1) - wrong size (%"PRIu32"/39+N*12)",length);
				eptr->mode=KILL;
				return;
			}
			eptr->servip = get32bit(&data);
			eptr->servport = get16bit(&data);
			eptr->usedspace = get64bit(&data);
			eptr->totalspace = get64bit(&data);
			eptr->todelusedspace = get64bit(&data);
			eptr->todeltotalspace = get64bit(&data);
			length-=39;
		} else if (rversion==2) {
			if (length<47 || ((length-47)%12)!=0) {
				MFSLOG(LOG_NOTICE,"CSTOMA_REGISTER (ver 2) - wrong size (%"PRIu32"/47+N*12)",length);
				eptr->mode=KILL;
				return;
			}
			eptr->servip = get32bit(&data);
			eptr->servport = get16bit(&data);
			eptr->usedspace = get64bit(&data);
			eptr->totalspace = get64bit(&data);
			eptr->chunkscount = get32bit(&data);
			eptr->todelusedspace = get64bit(&data);
			eptr->todeltotalspace = get64bit(&data);
			eptr->todelchunkscount = get32bit(&data);
			length-=47;
		} else if (rversion==3) {
			if (length<49 || ((length-49)%12)!=0) {
				MFSLOG(LOG_NOTICE,"CSTOMA_REGISTER (ver 3) - wrong size (%"PRIu32"/49+N*12)",length);
				eptr->mode=KILL;
				return;
			}
			eptr->servip = get32bit(&data);
			eptr->servport = get16bit(&data);
			eptr->timeout = get16bit(&data);
			eptr->usedspace = get64bit(&data);
			eptr->totalspace = get64bit(&data);
			eptr->chunkscount = get32bit(&data);
			eptr->todelusedspace = get64bit(&data);
			eptr->todeltotalspace = get64bit(&data);
			eptr->todelchunkscount = get32bit(&data);
			length-=49;
		} else if (rversion==4) {
			if (length<53 || ((length-53)%12)!=0) {
				MFSLOG(LOG_NOTICE,"CSTOMA_REGISTER (ver 4) - wrong size (%"PRIu32"/53+N*12)",length);
				eptr->mode=KILL;
				return;
			}
			eptr->version = get32bit(&data);
			eptr->servip = get32bit(&data);
			eptr->servport = get16bit(&data);
			eptr->timeout = get16bit(&data);
			eptr->usedspace = get64bit(&data);
			eptr->totalspace = get64bit(&data);
			eptr->chunkscount = get32bit(&data);
			eptr->todelusedspace = get64bit(&data);
			eptr->todeltotalspace = get64bit(&data);
			eptr->todelchunkscount = get32bit(&data);
			length-=53;
		} else {
			MFSLOG(LOG_NOTICE,"CSTOMA_REGISTER - wrong version (%"PRIu8"/1..4)",rversion);
			eptr->mode=KILL;
			return;
		}
	}
	if (eptr->servip==0) {
		tcpgetpeer(eptr->sock,&(eptr->servip),NULL);
	}
	if (eptr->servstrip) {
		free(eptr->servstrip);
	}
	eptr->servstrip = matocsserv_makestrip(eptr->servip);
	if (((eptr->servip)&0xFF000000) == 0x7F000000) {
		MFSLOG(LOG_NOTICE,"chunkserver connected using localhost (IP: %s) - you cannot use localhost for communication between chunkserver and master", eptr->servstrip);
		eptr->mode=KILL;
		return;
	}
	if (eptr->totalspace>maxtotalspace) {
		maxtotalspace=eptr->totalspace;
	}
	us = (double)(eptr->usedspace)/(double)(1024*1024*1024);
	ts = (double)(eptr->totalspace)/(double)(1024*1024*1024);
	MFSLOG(LOG_NOTICE,"chunkserver register - ip: %s, port: %"PRIu16", usedspace: %"PRIu64" (%.2lf GiB), totalspace: %"PRIu64" (%.2lf GiB)",eptr->servstrip,eptr->servport,eptr->usedspace,us,eptr->totalspace,ts);
	for (eaptr=matocsservhead ; eaptr ; eaptr=eaptr->next) {
		if (eptr!=eaptr && eaptr->mode!=KILL && eaptr->servip==eptr->servip && eaptr->servport==eptr->servport && eptr->listen_sock==0) {
			MFSLOG(LOG_WARNING,"chunk-server already connected !!!");
			eptr->mode=KILL;
			return;
		}
	}
//	eptr->creation = NULL;
//	eptr->setversion = NULL;
//	eptr->duplication = NULL;
	chunkcount = length/(8+4);
	for (i=0 ; i<chunkcount ; i++) {
		chunkid = get64bit(&data);
		chunkversion = get32bit(&data);
		chunk_server_has_chunk(eptr,chunkid,chunkversion);
	}
}

void matocsserv_space(serventry *eptr,const uint8_t *data,uint32_t length) {
	if (length!=16 && length!=32 && length!=40) {
		MFSLOG(LOG_NOTICE,"CSTOMA_SPACE - wrong size (%"PRIu32"/16|32|40)",length);
		eptr->mode=KILL;
		return;
	}
	eptr->usedspace = get64bit(&data);
	eptr->totalspace = get64bit(&data);
	if (eptr->totalspace>maxtotalspace) {
		maxtotalspace=eptr->totalspace;
	}
	if (length==40) {
		eptr->chunkscount = get32bit(&data);
	}
	if (length>=32) {
		eptr->todelusedspace = get64bit(&data);
		eptr->todeltotalspace = get64bit(&data);
		if (length==40) {
			eptr->todelchunkscount = get32bit(&data);
		}
	}
}

void matocsserv_chunk_damaged(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint64_t chunkid;
	uint32_t i;

	if (length%8!=0) {
		MFSLOG(LOG_NOTICE,"CSTOMA_CHUNK_DAMAGED - wrong size (%"PRIu32"/N*8)",length);
		eptr->mode=KILL;
		return;
	}
	for (i=0 ; i<length/8 ; i++) {
		chunkid = get64bit(&data);
//		syslog(LOG_NOTICE,"(%s:%"PRIu16") chunk: %016"PRIX64" is damaged",eptr->servstrip,eptr->servport,chunkid);
		chunk_damaged(eptr,chunkid);
	}
}

void matocsserv_chunks_lost(serventry *eptr,const uint8_t *data,uint32_t length) {
	uint64_t chunkid;
	uint32_t i;

	if (length%8!=0) {
		MFSLOG(LOG_NOTICE,"CSTOMA_CHUNK_LOST - wrong size (%"PRIu32"/N*8)",length);
		eptr->mode=KILL;
		return;
	}
	for (i=0 ; i<length/8 ; i++) {
		chunkid = get64bit(&data);
//		syslog(LOG_NOTICE,"(%s:%"PRIu16") chunk lost: %016"PRIX64,eptr->servstrip,eptr->servport,chunkid);
		chunk_lost(eptr,chunkid);
	}
}

void matocsserv_error_occurred(serventry *eptr,const uint8_t *data,uint32_t length) {
	(void)data;
	if (length!=0) {
		MFSLOG(LOG_NOTICE,"CSTOMA_ERROR_OCCURRED - wrong size (%"PRIu32"/0)",length);
		eptr->mode=KILL;
		return;
	}
	eptr->errorcounter++;
}

/*
void matocsserv_broadcast_logstring(uint64_t version,uint8_t *logstr,uint32_t logstrsize) {
	serventry *eptr;
	uint8_t *data;

	for (eptr = matocsservhead ; eptr ; eptr=eptr->next) {
		data = matocsserv_createpacket(eptr,MATOCS_STRUCTURE_LOG,9+logstrsize);
		if (data!=NULL) {
			// put32bit(&data,version);
			put8bit(&data,0xFF);
			put64bit(&data,version);
			memcpy(data,logstr,logstrsize);
		}
	}
}

void matocsserv_broadcast_logrotate() {
	serventry *eptr;

	for (eptr = matocsservhead ; eptr ; eptr=eptr->next) {
		matocsserv_createpacket(eptr,MATOCS_STRUCTURE_LOG_ROTATE,0);
	}
}
*/
void matocsserv_gotpacket(serventry *eptr,uint32_t type,const uint8_t *data,uint32_t length) {
	switch (type) {
		case ANTOAN_NOP:
			break;
		case CSTOMA_REGISTER:
			matocsserv_register(eptr,data,length);
			break;
		case CSTOMA_SPACE:
			matocsserv_space(eptr,data,length);
			break;
		case CSTOMA_CHUNK_DAMAGED:
			matocsserv_chunk_damaged(eptr,data,length);
			break;
		case CSTOMA_CHUNK_LOST:
			matocsserv_chunks_lost(eptr,data,length);
			break;
		case CSTOMA_ERROR_OCCURRED:
			matocsserv_error_occurred(eptr,data,length);
			break;
		case CSTOAN_CHUNK_CHECKSUM:
			matocsserv_got_chunk_checksum(eptr,data,length);
			break;
		case CSTOMA_CREATE:
			matocsserv_got_createchunk_status(eptr,data,length);
			break;
		case CSTOMA_DELETE:
			matocsserv_got_deletechunk_status(eptr,data,length);
			break;
		case CSTOMA_REPLICATE:
			matocsserv_got_replicatechunk_status(eptr,data,length);
			break;
		case CSTOMA_DUPLICATE:
			matocsserv_got_duplicatechunk_status(eptr,data,length);
			break;
		case CSTOMA_SET_VERSION:
			matocsserv_got_setchunkversion_status(eptr,data,length);
			break;
		case CSTOMA_TRUNCATE:
			matocsserv_got_truncatechunk_status(eptr,data,length);
			break;
		case CSTOMA_DUPTRUNC:
			matocsserv_got_duptruncchunk_status(eptr,data,length);
			break;
		default:
			MFSLOG(LOG_NOTICE,"matocs: got unknown message (type:%"PRIu32")",type);
			eptr->mode=KILL;
	}
}

void matocsserv_term(void) {
	serventry *eptr,*eaptr;
	packetstruct *pptr,*paptr;
	MFSLOG(LOG_INFO,"matocs: closing %s:%s",ListenHost,ListenPort);
	tcpclose(lsock);

	eptr = matocsservhead;
	while (eptr) {
		if (eptr->inputpacket.packet) {
			free(eptr->inputpacket.packet);
		}
		pptr = eptr->outputhead;
		while (pptr) {
			if (pptr->packet) {
				free(pptr->packet);
			}
			paptr = pptr;
			pptr = pptr->next;
			free(paptr);
		}
		eaptr = eptr;
		eptr = eptr->next;
		free(eaptr);
	}
	matocsservhead=NULL;
}

void matocsserv_read(serventry *eptr) {
	int32_t i;
	uint32_t type,size;
	const uint8_t *ptr;
	for (;;) {
		i=read(eptr->sock,eptr->inputpacket.startptr,eptr->inputpacket.bytesleft);
		if (i==0) {
			MFSLOG(LOG_INFO,"connection with CS(%s) lost",eptr->servstrip);
			eptr->mode = KILL;
			return;
		}
		if (i<0) {
			if (errno!=EAGAIN) {
				MFSLOG(LOG_INFO,"read from CS(%s) error: %m",eptr->servstrip);
				eptr->mode = KILL;
			}
			return;
		}
		eptr->inputpacket.startptr+=i;
		eptr->inputpacket.bytesleft-=i;

		if (eptr->inputpacket.bytesleft>0) {
			return;
		}

		if (eptr->mode==HEADER) {
			ptr = eptr->hdrbuff+4;
			size = get32bit(&ptr);

			if (size>0) {
				if (size>MaxPacketSize) {
					MFSLOG(LOG_WARNING,"CS(%s) packet too long (%"PRIu32"/%u)",eptr->servstrip,size,MaxPacketSize);
					eptr->mode = KILL;
					return;
				}
				eptr->inputpacket.packet = malloc(size);
				if (eptr->inputpacket.packet==NULL) {
					MFSLOG(LOG_WARNING,"CS(%s) packet: out of memory",eptr->servstrip);
					eptr->mode = KILL;
					return;
				}
				eptr->inputpacket.bytesleft = size;
				eptr->inputpacket.startptr = eptr->inputpacket.packet;
				eptr->mode = DATA;
				continue;
			}
			eptr->mode = DATA;
		}

		if (eptr->mode==DATA) {
			ptr = eptr->hdrbuff;
			type = get32bit(&ptr);
			size = get32bit(&ptr);

			eptr->mode=HEADER;
			eptr->inputpacket.bytesleft = 8;
			eptr->inputpacket.startptr = eptr->hdrbuff;

			matocsserv_gotpacket(eptr,type,eptr->inputpacket.packet,size);

			if (eptr->inputpacket.packet) {
				free(eptr->inputpacket.packet);
			}
			eptr->inputpacket.packet=NULL;
		}
	}
}

void matocsserv_write(serventry *eptr) {
	packetstruct *pack;
	int32_t i;
	for (;;) {
		pack = eptr->outputhead;
		if (pack==NULL) {
			return;
		}
		i=write(eptr->sock,pack->startptr,pack->bytesleft);
		if (i<0) {
			if (errno!=EAGAIN) {
				MFSLOG(LOG_INFO,"write to CS(%s) error: %m",eptr->servstrip);
				eptr->mode = KILL;
			}
			return;
		}
		pack->startptr+=i;
		pack->bytesleft-=i;
		if (pack->bytesleft>0) {
			return;
		}
		free(pack->packet);
		eptr->outputhead = pack->next;
		if (eptr->outputhead==NULL) {
			eptr->outputtail = &(eptr->outputhead);
		}
		free(pack);
	}
}

void matocsserv_desc(int epoll_fd) {
        uint32_t now=get_current_time();
        serventry *eptr,**kptr,**wptr;
        packetstruct *pptr,*paptr;
	struct epoll_event ev;
	int ret;

	if(first_add_listen_sock==0) {
                eptr = malloc(sizeof(serventry));
               	eptr->next = matocsservhead;
                matocsservhead = eptr;
                eptr->sock = lsock;
                eptr->mode = HEADER;
                eptr->lastread = eptr->lastwrite = get_current_time();
                eptr->inputpacket.next = NULL;
                eptr->inputpacket.bytesleft = 8;
                eptr->inputpacket.startptr = eptr->hdrbuff;
                eptr->inputpacket.packet = NULL;
                eptr->outputhead = NULL;
                eptr->outputtail = &(eptr->outputhead);

                eptr->version=0;
                eptr->timeout=60;

                eptr->listen_sock = 1;
                eptr->connection = 1;
       

                ev.data.ptr = eptr;
                ev.events = EPOLLIN;
                ret = epoll_ctl(epoll_fd,EPOLL_CTL_ADD,lsock,&ev);
		if(ret!=0) {
                        MFSLOG(LOG_NOTICE,"epoll_ctl error");
                }

                first_add_listen_sock = 1;
//		syslog(LOG_NOTICE,"chunkserver:first add listen socket,last read is %d,last write is %d",eptr->lastread,eptr->lastwrite);
	}

        kptr = &matocsservhead;
        wptr = &matocsservhead;
	while((eptr=*kptr)) {
		if (eptr->listen_sock == 0 && eptr->mode != KILL) {
			ev.data.ptr = eptr;
			ev.events = EPOLLIN;
			if (eptr->outputhead!=NULL && eptr->mode != KILL) {
				ev.events = EPOLLIN|EPOLLOUT;
			}
			ret = epoll_ctl(epoll_fd,EPOLL_CTL_MOD,eptr->sock,&ev);
			if(ret!=0) {
                	        MFSLOG(LOG_NOTICE,"epoll_ctl error");
                	}
		}

		if (eptr->listen_sock == 1) {
                        eptr->lastread = eptr->lastwrite = get_current_time();
                }                       
		if ((uint32_t)(eptr->lastread+eptr->timeout)<(uint32_t)now) {
                        eptr->mode = KILL;
                }
                if ((uint32_t)(eptr->lastwrite+(eptr->timeout/2))<(uint32_t)now && eptr->outputhead==NULL) {
                        matocsserv_createpacket(eptr,ANTOAN_NOP,0);
                }                
		if (eptr->mode == KILL) {
                        ev.data.ptr = eptr;

                        MFSLOG(LOG_NOTICE,"chunkserver disconnected - ip: %s, port: %"PRIu16" ",eptr->servstrip,eptr->servport);
                        matocsserv_replication_disconnected(eptr);
                        chunk_server_disconnected(eptr);
			    epoll_ctl(epoll_fd,EPOLL_CTL_DEL,eptr->sock,&ev);						
                        tcpclose(eptr->sock);
                        if (eptr->inputpacket.packet) {
                                free(eptr->inputpacket.packet);
                        }
                        pptr = eptr->outputhead;
                        while (pptr) {
                                if (pptr->packet) {
                                        free(pptr->packet);
                                }
                                paptr = pptr;
                                pptr = pptr->next;
                                free(paptr);
                        }
                        if (eptr->servstrip) {
                                free(eptr->servstrip);
                        }
			if(eptr == matocsservhead) {
                                matocsservhead = eptr->next;
                                wptr = &matocsservhead;
                        }
                        else {
                                (*wptr)->next = eptr->next;
                        }
                        *kptr = eptr->next;
                        free(eptr);
		} else {
                        wptr = &eptr;
                        kptr = &(eptr->next);
		}
	}
}

void matocsserv_serve(int epoll_fd,int count,struct epoll_event *pdesc) {
	uint32_t now=get_current_time();
	uint32_t peerip;
	serventry *eptr,*weptr;
	int ns;

	weptr = (serventry *)pdesc[count].data.ptr;
	if ((weptr->listen_sock == 1) && lsockpdescpos>=0 && (pdesc[count].events & EPOLLIN)) {
		ns=tcpaccept(lsock);
		if (ns<0) {
			MFSLOG(LOG_INFO,"Master<->CS socket: accept error: %m");
		} else {
			struct epoll_event ev;
			tcpnonblock(ns);
			tcpnodelay(ns);
			eptr = malloc(sizeof(serventry));
			eptr->next = matocsservhead;
			matocsservhead = eptr;
			eptr->sock = ns;
			eptr->mode = HEADER;
			eptr->lastread = eptr->lastwrite = get_current_time();
			eptr->inputpacket.next = NULL;
			eptr->inputpacket.bytesleft = 8;
			eptr->inputpacket.startptr = eptr->hdrbuff;
			eptr->inputpacket.packet = NULL;
			eptr->outputhead = NULL;
			eptr->outputtail = &(eptr->outputhead);

			tcpgetpeer(eptr->sock,&peerip,NULL);
			eptr->servstrip = matocsserv_makestrip(peerip);
			eptr->version=0;
			eptr->servip=0;
			eptr->servport=0;
			eptr->timeout=60;
			eptr->usedspace=0;
			eptr->totalspace=0;
			eptr->chunkscount=0;
			eptr->todelusedspace=0;
			eptr->todeltotalspace=0;
			eptr->todelchunkscount=0;
			eptr->errorcounter=0;
			eptr->rrepcounter=0;
			eptr->wrepcounter=0;

			eptr->carry=(double)(rndu32())/(double)(0xFFFFFFFFU);
				
			eptr->listen_sock = 0;
                        eptr->connection = 1;
		
			ev.data.ptr = eptr;
                	ev.events = EPOLLIN;
                    	epoll_ctl(epoll_fd,EPOLL_CTL_ADD,ns,&ev);
		}
	}
	if (weptr->listen_sock == 0) {
		if (pdesc[count].events & (EPOLLERR|EPOLLHUP)) {
			weptr->mode = KILL;
		}
		if ((pdesc[count].events & EPOLLIN) && weptr->mode!=KILL) {
			matocsserv_read(weptr);
			weptr->lastread = now;
			
		}
		if ((pdesc[count].events & EPOLLOUT) && weptr->mode!=KILL && weptr->outputhead!=NULL) {
			matocsserv_write(weptr);
			weptr->lastwrite = now;			
		}
	}	
}

int matocsserv_init() {
	ListenHost = cfg_getstr("MATOCS_LISTEN_HOST","*");
	ListenPort = cfg_getstr("MATOCS_LISTEN_PORT","9420");
	LOG_COUNT = cfg_getuint32("LOG_PRINT_FREQUENCY",1000);

	first_add_listen_sock = 0;
	lsock = tcpsocket();
	if (lsock<0) {
		MFSLOG(LOG_ERR,"matocs: socket error: %m");
		fprintf(msgfd,"master <-> chunkservers module: can't create socket\n");
		return -1;
	}
	tcpnonblock(lsock);
	tcpnodelay(lsock);
	tcpreuseaddr(lsock);
	if (tcpsetacceptfilter(lsock)<0) {
		MFSLOG(LOG_NOTICE,"matocs: can't set accept filter: %m");
	}
	if (tcpstrlisten(lsock,ListenHost,ListenPort,100)<0) {
		MFSLOG(LOG_ERR,"matocs: listen error: %m");
		fprintf(msgfd,"master <-> chunkservers module: can't listen on socket\n");
		return -1;
	}
	MFSLOG(LOG_NOTICE,"matocs: listen on %s:%s",ListenHost,ListenPort);
	fprintf(msgfd,"master <-> chunkservers module: listen on %s:%s\n",ListenHost,ListenPort);

	matocsserv_replication_init();
	matocsservhead = NULL;
	main_destructregister(matocsserv_term);
	main_epollregister(matocsserv_desc,matocsserv_serve);
	main_timeregister(TIMEMODE_RUNONCE,60,0,log_print_control);
	return 0;
}
