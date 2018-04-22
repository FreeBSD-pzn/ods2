/* Cache.h v1.3    Definitions for cache routines */

/*
        This is part of ODS2 written by Paul Nankervis,
        email address:  Paulnank@au1.ibm.com

        ODS2 is distributed freely for all members of the
        VMS community to use. However all derived works
        must maintain comments in their source to acknowledge
        the contibution of the original author.
*/

#ifndef CACHE_LOADED

#define CACHE_LOADED

#ifndef VAXC	/* Stupid VAX C doesn't allow "signed" keyword */
#define signed signed
#else
#define signed
#endif

struct CACHE {
    struct CACHE *nextlru;	/* next object on least recently used list */
    struct CACHE *lastlru;	/* last object on least recently used list */
    struct CACHE *left;		/* left branch of binary tree */
    struct CACHE *right;	/* right branch of binary tree */
    struct CACHE **parent;	/* address of pointer to this object */
    void *(*objmanager) (struct CACHE * cacheobj,int flushonly);
    unsigned hashval;		/* object hash value */
    short refcount;		/* object reference count */
    signed char balance;	/* object tree imbalance factor */
    signed char objtype;	/* object type (for debugging) */
};

void cache_show(void);
int cache_refcount(struct CACHE *cacheobj);
struct CACHE *cache_delete(struct CACHE *cacheobj);
void cache_purge(void);
void cache_flush(void);
void cache_remove(struct CACHE *cacheobj);
void cache_touch(struct CACHE * cacheobj);
void cache_untouch(struct CACHE * cacheobj,int recycle);
void *cache_find(void **root,unsigned hashval,void *keyval,unsigned *retsts,
                 int (*compare_func) (unsigned hashval,void *keyval,void *node),
                 void *(*create_func) (unsigned hashval,void *keyval,unsigned *retsts));
#endif
