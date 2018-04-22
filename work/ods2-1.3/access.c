/* Access.c v1.3 */

/*
        This is part of ODS2 written by Paul Nankervis,
        email address:  Paulnank@au1.ibm.com

        ODS2 is distributed freely for all members of the
        VMS community to use. However all derived works
        must maintain comments in their source to acknowledge
        the contibution of the original author.
*/

/*
        This module implements 'accessing' files on an ODS2
        disk volume. It uses its own low level interface to support
        'higher level' APIs. For example it is called by the
        'RMS' routines.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include "ssdef.h"
#include "access.h"
#include "phyio.h"


#define DEBUGx


/* checksum() to produce header checksum values... */

unsigned short checksum(vmsword *block)
{
    register int count = 255;
    register unsigned result = 0;
    register unsigned short *ptr = block;
    do {
	register unsigned data = *ptr++;
        result += VMSWORD(data);
    } while (--count > 0);
    return result;
}


/* rvn_to_dev() find device from relative volume number */

struct VCBDEV *rvn_to_dev(struct VCB *vcb,unsigned rvn)
{
    if (rvn < 2) {
        if (vcb->vcbdev[0].dev != NULL) return vcb->vcbdev;
    } else {
        if (rvn <= vcb->devices) {
            if (vcb->vcbdev[rvn - 1].dev != NULL)
                return &vcb->vcbdev[rvn - 1];
        }
    }
    return NULL;                /* RVN illegal or device not mounted */
}

/* fid_copy() copy fid from file header with default rvn! */

void fid_copy(struct fiddef *dst,struct fiddef *src,unsigned rvn)
{
    dst->fid$w_num = VMSWORD(src->fid$w_num);
    dst->fid$w_seq = VMSWORD(src->fid$w_seq);
    if (src->fid$b_rvn == 0) {
        dst->fid$b_rvn = rvn;
    } else {
        dst->fid$b_rvn = src->fid$b_rvn;
    }
    dst->fid$b_nmx = src->fid$b_nmx;
}

/* deaccesshead() release header from INDEXF... */

unsigned deaccesshead(struct VIOC *vioc,struct HEAD *head,unsigned idxblk)
{
    if (head && idxblk) {
	unsigned short check = checksum((vmsword *) head);
	head->fh2$w_checksum = VMSWORD(check);
    }
    return deaccesschunk(vioc,idxblk,1,1);
}


/* accesshead() find file or extension header from INDEXF... */

unsigned accesshead(struct VCB *vcb,struct fiddef *fid,unsigned seg_num,
                    struct VIOC **vioc,struct HEAD **headbuff,
                    unsigned *retidxblk,unsigned wrtflg)
{
    register unsigned sts;
    register struct VCBDEV *vcbdev;
    register unsigned idxblk;
    vcbdev = rvn_to_dev(vcb,fid->fid$b_rvn);
    if (vcbdev == NULL) return SS$_DEVNOTMOUNT;
    if (wrtflg && ((vcb->status & VCB_WRITE) == 0)) return SS$_WRITLCK;
    idxblk = fid->fid$w_num + (fid->fid$b_nmx << 16) - 1 +
        VMSWORD(vcbdev->home.hm2$w_ibmapvbn) + VMSWORD(vcbdev->home.hm2$w_ibmapsize);
    if (vcbdev->idxfcb->head != NULL) {
        if (idxblk >= VMSSWAP(vcbdev->idxfcb->head->fh2$w_recattr.fat$l_efblk)) {
            printf("Not in index file\n");
            return SS$_NOSUCHFILE;
        }
    }
    sts = accesschunk(vcbdev->idxfcb,idxblk,vioc,(char **) headbuff,
                      NULL,wrtflg ? 1 : 0);
    if (sts & 1) {
        register struct HEAD *head = *headbuff;
        if (retidxblk) {
            if (wrtflg) {
                *retidxblk = idxblk;
            } else {
                *retidxblk = 0;
            }
        }
        if (VMSWORD(head->fh2$w_fid.fid$w_num) != fid->fid$w_num ||
            head->fh2$w_fid.fid$b_nmx != fid->fid$b_nmx ||
            VMSWORD(head->fh2$w_fid.fid$w_seq) != fid->fid$w_seq ||
            (head->fh2$w_fid.fid$b_rvn != fid->fid$b_rvn &&
             head->fh2$w_fid.fid$b_rvn != 0)) {
            /* lib$signal(SS$_NOSUCHFILE); */
            sts = SS$_NOSUCHFILE;
        } else {
            if (head->fh2$b_idoffset < 38 ||
                head->fh2$b_idoffset > head->fh2$b_mpoffset ||
                head->fh2$b_mpoffset > head->fh2$b_acoffset ||
                head->fh2$b_acoffset > head->fh2$b_rsoffset ||
                head->fh2$b_map_inuse > head->fh2$b_acoffset - head->fh2$b_mpoffset ||
                checksum((vmsword *) head) != VMSWORD(head->fh2$w_checksum)) {
                sts = SS$_DATACHECK;
            } else {
                if (VMSWORD(head->fh2$w_seg_num) != seg_num) sts = SS$_FILESEQCHK;
            }
        }
        if ((sts & 1) == 0) deaccesschunk(*vioc,0,0,0);
    }
    return sts;
}





struct WCBKEY {
    unsigned vbn;
    struct FCB *fcb;
    struct WCB *prevwcb;
};                              /* WCBKEY passes info to compare/create routines... */

/* wcb_compare() compare two windows routine - return -1 for less, 0 for match... */
/*    as a by product keep highest previous entry so that if a new window
      is required we don't have to go right back to the initial file header */

int wcb_compare(unsigned hashval,void *keyval,void *thiswcb)
{
    register struct WCBKEY *wcbkey = (struct WCBKEY *) keyval;
    register struct WCB *wcb = (struct WCB *) thiswcb;
    if (wcbkey->vbn < wcb->loblk) {
        return -1;              /* Search key is less than this window maps... */
    } else {
        if (wcbkey->vbn <= wcb->hiblk) {
            return 0;           /* Search key must be in this window... */
        } else {
            if (wcbkey->prevwcb == NULL) {
                wcbkey->prevwcb = wcb;
            } else {
                if (wcb->loblk != 0 && wcb->hiblk > wcbkey->prevwcb->hiblk) wcbkey->prevwcb = wcb;
            }
            return 1;           /* Search key is higher than this window... */
        }
    }
}

/* premap_indexf() called to physically read the header for indexf.sys
   so that indexf.sys can be mapped and read into virtual cache.. */

struct HEAD *premap_indexf(struct FCB *fcb,unsigned *retsts)
{
    struct HEAD *head;
    struct VCBDEV *vcbdev = rvn_to_dev(fcb->vcb,fcb->rvn);
    if (vcbdev == NULL) {
        *retsts = SS$_DEVNOTMOUNT;
        return NULL;
    }
    head = (struct HEAD *) malloc(sizeof(struct HEAD));
    if (head == NULL) {
        *retsts = SS$_INSFMEM;
    } else {
        *retsts = phyio_read(vcbdev->dev->handle,VMSLONG(vcbdev->home.hm2$l_ibmaplbn) +
                             VMSWORD(vcbdev->home.hm2$w_ibmapsize),sizeof(struct HEAD),
                             (char *) head);
        if (!(*retsts & 1)) {
            free(head);
            head = NULL;
        } else {
            if (VMSWORD(head->fh2$w_fid.fid$w_num) != 1 ||
                head->fh2$w_fid.fid$b_nmx != 0 ||
                VMSWORD(head->fh2$w_fid.fid$w_seq) != 1 ||
                VMSWORD(head->fh2$w_checksum) != checksum((unsigned short *) head)) {
                *retsts = SS$_DATACHECK;
                free(head);
                head = NULL;
            }
        }
    }
    return head;
}

/* wcb_create() creates a window control block by reading appropriate
   file headers... */

void *wcb_create(unsigned hashval,void *keyval,unsigned *retsts)
{
    register struct WCB *wcb = (struct WCB *) malloc(sizeof(struct WCB));
    if (wcb == NULL) {
        *retsts = SS$_INSFMEM;
    } else {
        unsigned curvbn;
        unsigned extents = 0;
        struct HEAD *head;
        struct VIOC *vioc = NULL;
        register struct WCBKEY *wcbkey = (struct WCBKEY *) keyval;
        wcb->cache.objmanager = NULL;
        wcb->cache.objtype = 3;
        if (wcbkey->prevwcb == NULL) {
            curvbn = wcb->loblk = 1;
            wcb->hd_seg_num = 0;
            head = wcbkey->fcb->head;
            if (head == NULL) {
                head = premap_indexf(wcbkey->fcb,retsts);
                if (head == NULL) return NULL;
                head->fh2$w_ext_fid.fid$w_num = 0;
                head->fh2$w_ext_fid.fid$b_nmx = 0;
            }
            fid_copy(&wcb->hd_fid,&head->fh2$w_fid,wcbkey->fcb->rvn);
        } else {
            wcb->loblk = wcbkey->prevwcb->hiblk + 1;
            curvbn = wcbkey->prevwcb->hd_basevbn;
            wcb->hd_seg_num = wcbkey->prevwcb->hd_seg_num;
            memcpy(&wcb->hd_fid,&wcbkey->prevwcb->hd_fid,sizeof(struct fiddef));
        }
        do {
            register unsigned short *mp;
            register unsigned short *me;
            wcb->hd_basevbn = curvbn;
            if (wcb->hd_seg_num != 0) {
                *retsts = accesshead(wcbkey->fcb->vcb,&wcb->hd_fid,wcb->hd_seg_num,&vioc,&head,NULL,0);
                if ((*retsts & 1) == 0) {
                    free(wcb);
                    return NULL;
                }
            }
            mp = (unsigned short *) head + head->fh2$b_mpoffset;
            me = mp + head->fh2$b_map_inuse;
            while (mp < me) {
                register unsigned phylen,phyblk;
                switch (VMSWORD(*mp) >> 14) {
                    case 0:
                        phylen = 0;
                        mp++;
                        break;
                    case 1:
                        phylen = (VMSWORD(*mp) & 0377) + 1;
                        phyblk = ((VMSWORD(*mp) & 037400) << 8) | VMSWORD(mp[1]);
                        mp += 2;
                        break;
                    case 2:
                        phylen = (VMSWORD(*mp) & 037777) + 1;
                        phyblk = (VMSWORD(mp[2]) << 16) | VMSWORD(mp[1]);
                        mp += 3;
                        break;
                    case 3:
                        phylen = ((VMSWORD(*mp) & 037777) << 16) + VMSWORD(mp[1]) + 1;
                        phyblk = (VMSWORD(mp[3]) << 16) | VMSWORD(mp[2]);
                        mp += 4;
                }
                curvbn += phylen;
                if (phylen != 0 && curvbn > wcb->loblk) {
                    wcb->phylen[extents] = phylen;
                    wcb->phyblk[extents] = phyblk;
                    wcb->rvn[extents] = wcb->hd_fid.fid$b_rvn;
                    if (++extents >= EXTMAX) {
                        if (curvbn > wcbkey->vbn) {
                            break;
                        } else {
                            extents = 0;
                            wcb->loblk = curvbn;
                        }
                    }
                }
            }
            if (extents >= EXTMAX || (VMSWORD(head->fh2$w_ext_fid.fid$w_num) == 0
                                      && head->fh2$w_ext_fid.fid$b_nmx == 0)) {
                break;
            } else {
                register unsigned rvn;
                wcb->hd_seg_num++;
                rvn = wcb->hd_fid.fid$b_rvn;
                fid_copy(&wcb->hd_fid,&head->fh2$w_ext_fid,rvn);
                if (vioc != NULL) deaccesshead(vioc,NULL,0);
            }
        } while (1);
        if (vioc != NULL) {
            deaccesshead(vioc,NULL,0);
        } else {
            if (wcbkey->fcb->head == NULL) free(head);
        }
        wcb->hiblk = curvbn - 1;
        wcb->extcount = extents;
        *retsts = SS$_NORMAL;
        if (curvbn <= wcbkey->vbn) {
            free(wcb);
            *retsts = SS$_DATACHECK;
            wcb = NULL;
        }
    }
    return wcb;
}


/* getwindow() find a window to map VBN to LBN ... */

unsigned getwindow(struct FCB * fcb,unsigned vbn,struct VCBDEV **devptr,
                   unsigned *phyblk,unsigned *phylen,struct fiddef *hdrfid,
                   unsigned *hdrseq)
{
    unsigned sts;
    struct WCB *wcb;
    struct WCBKEY wcbkey;
#ifdef DEBUG
    printf("Accessing window for vbn %d, file (%x)\n",vbn,fcb->cache.hashval);
#endif
    wcbkey.vbn = vbn;
    wcbkey.fcb = fcb;
    wcbkey.prevwcb = NULL;
    wcb = cache_find((void *) &fcb->wcb,0,&wcbkey,&sts,wcb_compare,wcb_create);
    if (wcb == NULL) return sts;
    {
        register unsigned extent = 0;
        register unsigned togo = vbn - wcb->loblk;
        while (togo >= wcb->phylen[extent]) {
            togo -= wcb->phylen[extent];
            if (++extent > wcb->extcount) return SS$_BUGCHECK;
        }
        *devptr = rvn_to_dev(fcb->vcb,wcb->rvn[extent]);
        *phyblk = wcb->phyblk[extent] + togo;
        *phylen = wcb->phylen[extent] - togo;
        if (hdrfid != NULL) memcpy(hdrfid,&wcb->hd_fid,sizeof(struct fiddef));
        if (hdrseq != NULL) *hdrseq = wcb->hd_seg_num;
#ifdef DEBUG
        printf("Mapping vbn %d to %d (%d -> %d)[%d] file (%x)\n",
               vbn,*phyblk,wcb->loblk,wcb->hiblk,wcb->hd_basevbn,fcb->cache.hashval);
#endif
        cache_untouch(&wcb->cache,1);
    }
    if (*devptr == NULL) return SS$_DEVNOTMOUNT;
    return SS$_NORMAL;
}


/* Object manager for VIOC objects:- if the object has been
   modified then we need to flush it to disk before we let
   the cache routines do anything to it... */

void *vioc_manager(struct CACHE * cacheobj,int flushonly)
{
    register struct VIOC *vioc = (struct VIOC *) cacheobj;
    if (vioc->modmask != 0) {
        register struct FCB *fcb = vioc->fcb;
        register int length = VIOC_CHUNKSIZE;
        register unsigned curvbn = vioc->cache.hashval + 1;
        register char *address = (char *) vioc->data;
        register unsigned modmask = vioc->modmask;
        printf("\nvioc_manager writing vbn %d\n",curvbn);
        do {
            register unsigned sts;
            int wrtlen = 0;
            unsigned phyblk,phylen;
            struct VCBDEV *vcbdev;
            while (length > 0 && (1 & modmask) == 0) {
                length--;
                curvbn++;
                address += 512;
                modmask = modmask >> 1;
            }
            while (wrtlen < length && (1 & modmask) != 0) {
                wrtlen++;
                modmask = modmask >> 1;
            }
            length -= wrtlen;
            while (wrtlen > 0) {
                if (fcb->highwater != 0 && curvbn >= fcb->highwater) {
                    length = 0;
                    break;
                }
                sts = getwindow(fcb,curvbn,&vcbdev,&phyblk,&phylen,NULL,NULL);
                if (!(sts & 1)) return NULL;
                if (phylen > wrtlen) phylen = wrtlen;
                if (fcb->highwater != 0 && curvbn + phylen > fcb->highwater) {
                    phylen = fcb->highwater - curvbn;
                }
                sts = phyio_write(vcbdev->dev->handle,phyblk,phylen * 512,address);
                if (!(sts & 1)) return NULL;
                wrtlen -= phylen;
                curvbn += phylen;
                address += phylen * 512;
            }
        } while (length > 0 && modmask != 0);
        vioc->modmask = 0;
        vioc->cache.objmanager = NULL;
    }
    return cacheobj;
}


/* deaccesschunk() to deaccess a VIOC (chunk of a file) */

unsigned deaccesschunk(struct VIOC *vioc,unsigned wrtvbn,
                       int wrtblks,int reuse)
{
#ifdef DEBUG
    printf("Deaccess chunk %8x\n",vioc->cache.hashval);
#endif
    if (wrtvbn) {
        register unsigned modmask;
        if (wrtvbn <= vioc->cache.hashval ||
            wrtvbn + wrtblks > vioc->cache.hashval + VIOC_CHUNKSIZE + 1) {
	     return SS$_BADPARAM;
	}
        modmask = 1 << (wrtvbn - vioc->cache.hashval - 1);
        while (--wrtblks > 0) modmask |= modmask << 1;
        if ((vioc->wrtmask | modmask) != vioc->wrtmask) return SS$_WRITLCK;
        vioc->modmask |= modmask;
        if (vioc->cache.refcount == 1) vioc->wrtmask = 0;
        vioc->cache.objmanager = vioc_manager;
    }
    cache_untouch(&vioc->cache,reuse);
    return SS$_NORMAL;
}


void *vioc_create(unsigned hashval,void *keyval,unsigned *retsts)
{
    register struct VIOC *vioc = (struct VIOC *) malloc(sizeof(struct VIOC));
    if (vioc == NULL) {
        *retsts = SS$_INSFMEM;
    } else {
        register int length;
        register unsigned curvbn = hashval + 1;
        register char *address;
        register struct FCB *fcb = (struct FCB *) keyval;
        vioc->cache.objmanager = NULL;
        vioc->cache.objtype = 7;
        vioc->fcb = fcb;
        vioc->wrtmask = 0;
        vioc->modmask = 0;
        length = fcb->hiblock - curvbn + 1;
        if (length > VIOC_CHUNKSIZE) length = VIOC_CHUNKSIZE;
        address = (char *) vioc->data;
        do {
            if (fcb->highwater != 0 && curvbn >= fcb->highwater) {
                memset(address,0,length * 512);
                break;
            } else {
                register unsigned sts;
                unsigned phyblk,phylen;
                struct VCBDEV *vcbdev;
                sts = getwindow(fcb,curvbn,&vcbdev,&phyblk,&phylen,NULL,NULL);
                if (sts & 1) {
                    if (phylen > length) phylen = length;
                    if (fcb->highwater != 0 && curvbn + phylen > fcb->highwater) {
                        phylen = fcb->highwater - curvbn;
                    }
                    sts = phyio_read(vcbdev->dev->handle,phyblk,phylen * 512,address);
                }
                if ((sts & 1) == 0) {
                    free(vioc);
                    *retsts = sts;
                    return NULL;
                }
                length -= phylen;
                curvbn += phylen;
                address += phylen * 512;
            }
        } while (length > 0);
        *retsts = SS$_NORMAL;
    }
    return vioc;
}



/* accesschunk() return pointer to a 'chunk' of a file ... */

unsigned accesschunk(struct FCB *fcb,unsigned vbn,struct VIOC **retvioc,
                     char **retbuff,unsigned *retblocks,unsigned wrtblks)
{
    unsigned sts;
    register int blocks;
    register struct VIOC *vioc;
#ifdef DEBUG
    printf("Access chunk %8x %d (%x)\n",base,vbn,fcb->cache.hashval);
#endif
    if (vbn < 1 || vbn > fcb->hiblock) return SS$_ENDOFFILE;
    blocks = (vbn - 1) / VIOC_CHUNKSIZE * VIOC_CHUNKSIZE;
    if (wrtblks) {
        if ((fcb->status & FCB_WRITE) == 0) return SS$_WRITLCK;
        if (vbn + wrtblks > blocks + VIOC_CHUNKSIZE + 1) {
	     return SS$_BADPARAM;
        }
    }
    vioc = cache_find((void *) &fcb->vioc,blocks,fcb,&sts,NULL,vioc_create);
    if (vioc == NULL) return sts;
    /*
        Return result to caller...
    */
    *retvioc = vioc;
    blocks = vbn - blocks - 1;
    *retbuff = vioc->data[blocks];
    if (wrtblks || retblocks != NULL) {
        register unsigned modmask = 1 << blocks;
        blocks = VIOC_CHUNKSIZE - blocks;
        if (vbn + blocks > fcb->hiblock) blocks = fcb->hiblock - vbn + 1;
        if (wrtblks && blocks > wrtblks) blocks = wrtblks;
        if (retblocks != NULL) *retblocks = blocks;
        if (wrtblks && blocks) {
            while (--blocks > 0) modmask |= modmask << 1;
            vioc->wrtmask |= modmask;
            vioc->cache.objmanager = vioc_manager;
        }
    }
    return SS$_NORMAL;
}


unsigned deallocfile(struct FCB *fcb);

/* deaccessfile() finish accessing a file.... */

unsigned deaccessfile(struct FCB *fcb)
{
#ifdef DEBUG
    printf("Deaccessing file (%x) reference %d\n",fcb->cache.hashval,fcb->cache.refcount);
#endif
    if (fcb->cache.refcount == 1) {
        register unsigned refcount;
        refcount = cache_refcount((struct CACHE *) fcb->wcb) +
            cache_refcount((struct CACHE *) fcb->vioc);
        if (refcount != 0) {
            printf("File reference counts non-zero %d  (%d)\n",refcount,
		fcb->cache.hashval);
#ifdef DEBUG
            printf("File reference counts non-zero %d %d\n",
                   cache_refcount((struct CACHE *) fcb->wcb),cache_refcount((struct CACHE *) fcb->vioc));
#endif
            return SS$_BUGCHECK;
        }
        if (fcb->status & FCB_WRITE) {
            if (VMSLONG(fcb->head->fh2$l_filechar) & FH2$M_MARKDEL) {
                return deallocfile(fcb);
            }
        }
    }
    cache_untouch(&fcb->cache,1);
    return SS$_NORMAL;
}


/* Object manager for FCB objects:- we point to one of our
   sub-objects (vioc or wcb) in preference to letting the
   cache routines get us!  But we when run out of excuses
   it is time to clean up the file header...  :-(   */

void *fcb_manager(struct CACHE *cacheobj,int flushonly)
{
    register struct FCB *fcb = (struct FCB *) cacheobj;
    if (fcb->vioc != NULL) return &fcb->vioc->cache;
    if (fcb->wcb != NULL) return &fcb->wcb->cache;
    if (fcb->cache.refcount != 0 || flushonly) return NULL;
    if (fcb->headvioc != NULL) {
        deaccesshead(fcb->headvioc,fcb->head,fcb->headvbn);
        fcb->headvioc = NULL;
    }
    return cacheobj;
}

void *fcb_create(unsigned filenum,void *keyval,unsigned *retsts)
{
    register struct FCB *fcb = (struct FCB *) malloc(sizeof(struct FCB));
    if (fcb == NULL) {
        *retsts = SS$_INSFMEM;
    } else {
        fcb->cache.objmanager = fcb_manager;
        fcb->cache.objtype = 2;
        fcb->vcb = NULL;
        fcb->headvioc = NULL;
        fcb->head = NULL;
        fcb->wcb = NULL;
        fcb->vioc = NULL;
        fcb->headvbn = 0;
        fcb->hiblock = 100000;
        fcb->highwater = 0;
        fcb->status = 0;
        fcb->rvn = 0;
    }
    return fcb;
}


/* accessfile() open up file for access... */

unsigned accessfile(struct VCB * vcb,struct fiddef * fid,struct FCB **fcbadd,
                    unsigned wrtflg)
{
    unsigned sts;
    register struct FCB *fcb;
    register unsigned filenum = (fid->fid$b_nmx << 16) + fid->fid$w_num;
#ifdef DEBUG
    printf("Accessing file (%d,%d,%d)\n",(fid->fid$b_nmx << 16) +
           fid->fid$w_num,fid->fid$w_seq,fid->fid$b_rvn);
#endif
    if (filenum < 1) return SS$_BADPARAM;
    if (wrtflg && ((vcb->status & VCB_WRITE) == 0)) return SS$_WRITLCK;
    if (fid->fid$b_rvn > 1) filenum |= fid->fid$b_rvn << 24;
    fcb = cache_find((void *) &vcb->fcb,filenum,NULL,&sts,NULL,fcb_create);
    if (fcb == NULL) return sts;
    /* If not found make one... */
    *fcbadd = fcb;
    if (fcb->vcb == NULL) {
        fcb->rvn = fid->fid$b_rvn;
        if (fcb->rvn == 0 && vcb->devices > 1) fcb->rvn = 1;
        fcb->vcb = vcb;
    }
    if (wrtflg) {
        if (fcb->headvioc != NULL && (fcb->status & FCB_WRITE) == 0) {
            deaccesshead(fcb->headvioc,NULL,0);
            fcb->headvioc = NULL;
        }
        fcb->status |= FCB_WRITE;
    }
    if (fcb->headvioc == NULL) {
        register unsigned sts;
        sts = accesshead(vcb,fid,0,&fcb->headvioc,&fcb->head,&fcb->headvbn,wrtflg);
        if (sts & 1) {
            fcb->hiblock = VMSSWAP(fcb->head->fh2$w_recattr.fat$l_hiblk);
            if (fcb->head->fh2$b_idoffset > 39) {
                fcb->highwater = VMSLONG(fcb->head->fh2$l_highwater);
            } else {
                fcb->highwater = 0;
            }
        } else {
            printf("Accessfile status %d\n",sts);
            fcb->cache.objmanager = NULL;
            cache_untouch(&fcb->cache,0);
            cache_delete(&fcb->cache);
            return sts;
        }
    }
    return SS$_NORMAL;
}




/* dismount() finish processing on a volume */

unsigned dismount(struct VCB * vcb)
{
    register unsigned sts,device;
    struct VCBDEV *vcbdev;
    int expectfiles = vcb->devices;
    int openfiles = cache_refcount(&vcb->fcb->cache);
    if (vcb->status & VCB_WRITE) expectfiles *= 2;
#ifdef DEBUG
    printf("Dismounting disk %d\n",openfiles);
#endif
    sts = SS$_NORMAL;
    if (openfiles != expectfiles) {
        sts = SS$_DEVNOTDISM;
    } else {
        vcbdev = vcb->vcbdev;
        for (device = 0; device < vcb->devices; device++) {
            if (vcbdev->dev != NULL) {
                if (vcb->status & VCB_WRITE && vcbdev->mapfcb != NULL) {
                    sts = deaccessfile(vcbdev->mapfcb);
                    if (!(sts & 1)) break;
                    vcbdev->idxfcb->status &= ~FCB_WRITE;
                    vcbdev->mapfcb = NULL;
                }
                cache_remove(&vcb->fcb->cache);
                sts = deaccesshead(vcbdev->idxfcb->headvioc,vcbdev->idxfcb->head,vcbdev->idxfcb->headvbn);
                if (!(sts & 1)) break;
                vcbdev->idxfcb->headvioc = NULL;
                cache_untouch(&vcbdev->idxfcb->cache,0);
                vcbdev->dev->vcb = NULL;
            }
            vcbdev++;
        }
        if (sts & 1) {
            cache_remove(&vcb->fcb->cache);
            while (vcb->dircache) cache_delete((struct CACHE *) vcb->dircache);
#ifdef DEBUG
            printf("Post close\n");
            cachedump();
#endif
            free(vcb);
        }
    }
    return sts;
}

#define HOME_LIMIT 100

/* mount() make disk volume available for processing... */

unsigned mount(unsigned flags,unsigned devices,char *devnam[],char *label[],struct VCB **retvcb)
{
    register unsigned device,sts;
    struct VCB *vcb;
    struct VCBDEV *vcbdev;
    if (sizeof(struct HOME) != 512 || sizeof(struct HEAD) != 512) return SS$_NOTINSTALL;
    vcb = (struct VCB *) malloc(sizeof(struct VCB) + (devices - 1) * sizeof(struct VCBDEV));
    if (vcb == NULL) return SS$_INSFMEM;
    vcb->status = 0;
    if (flags & 1) vcb->status |= VCB_WRITE;
    vcb->fcb = NULL;
    vcb->dircache = NULL;
    vcbdev = vcb->vcbdev;
    for (device = 0; device < devices; device++) {
        sts = SS$_NOSUCHVOL;
        vcbdev->dev = NULL;
        if (strlen(devnam[device])) {
            int hba;
            sts = device_lookup(strlen(devnam[device]),devnam[device],1,&vcbdev->dev);
            if (!(sts & 1)) break;
            for (hba = 1; hba <= HOME_LIMIT; hba++) {
                sts = phyio_read(vcbdev->dev->handle,hba,sizeof(struct HOME),(char *) &vcbdev->home);
                if (!(sts & 1)) break;
                if (hba == VMSLONG(vcbdev->home.hm2$l_homelbn) &&
                    memcmp(vcbdev->home.hm2$t_format,"DECFILE11B  ",12) == 0) break;
                sts = SS$_DATACHECK;
            }
            if (sts & 1) {
                if (VMSWORD(vcbdev->home.hm2$w_checksum2) != checksum((unsigned short *) &vcbdev->home)) {
                    sts = SS$_DATACHECK;
                } else {
                    if (VMSWORD(vcbdev->home.hm2$w_rvn) != device + 1)
                        if (VMSWORD(vcbdev->home.hm2$w_rvn) > 1 || device != 0)
                            sts = SS$_UNSUPVOLSET;
                    if (vcbdev->dev->vcb != NULL) {
                        sts = SS$_DEVMOUNT;
                    }
                }
            }
            if (!(sts & 1)) break;
        }
        vcbdev++;
    }
    if (sts & 1) {
        vcb->devices = devices;
        vcbdev = vcb->vcbdev;
        for (device = 0; device < devices; device++) {
            vcbdev->idxfcb = NULL;
            vcbdev->mapfcb = NULL;
            vcbdev->clustersize = 0;
            vcbdev->max_cluster = 0;
            vcbdev->free_clusters = 0;
            if (strlen(devnam[device])) {
                struct fiddef idxfid = {1,1,0,0};
                idxfid.fid$b_rvn = device + 1;
                sts = accessfile(vcb,&idxfid,&vcbdev->idxfcb,flags & 1);
                if (!(sts & 1)) {
                    vcbdev->dev = NULL;
                } else {
                    vcbdev->dev->vcb = vcb;
                    if (flags & 1) {
                        struct fiddef mapfid = {2,2,0,0};
                        mapfid.fid$b_rvn = device + 1;
                        sts = accessfile(vcb,&mapfid,&vcbdev->mapfcb,1);
                        if (sts & 1) {
                            struct VIOC *vioc;
                            struct SCB *scb;
                            sts = accesschunk(vcbdev->mapfcb,1,&vioc,(char **) &scb,NULL,0);
                            if (sts & 1) {
                                if (scb->scb$w_cluster == vcbdev->home.hm2$w_cluster) {
                                    vcbdev->clustersize = vcbdev->home.hm2$w_cluster;
                                    vcbdev->max_cluster = (scb->scb$l_volsize + scb->scb$w_cluster - 1) / scb->scb$w_cluster;
                                    deaccesschunk(vioc,0,0,0);
				    sts = update_freecount(vcbdev,&vcbdev->free_clusters);
printf("Freespace is %d\n",vcbdev->free_clusters);
                                }
                            }
                        }
                    }
                }
            }
            vcbdev++;
        }
    } else {
        free(vcb);
        vcb = NULL;
    }
    if (retvcb != NULL) *retvcb = vcb;
    return sts;
}
