/* Cache.c v1.3   Caching control routines */

/*
        This is part of ODS2 written by Paul Nankervis,
        email address:  Paulnank@au1.ibm.com

        ODS2 is distributed freely for all members of the
        VMS community to use. However all derived works
        must maintain comments in their source to acknowledge
        the contibution of the original author.
*/

/*
    The theory is that all cachable objects share a common cache pool.
    Any object with a reference count of zero is a candidate for
    destruction. All cacheable objects have a 'struct CACHE' as the
    first item of the object so that cache pointers and object pointers
    are 'interchangeable'. All cache objects are also part of a binary
    tree so that they can be located. There are many instances of these
    binary trees: for files on a volume, file windows, file chunks
    (segments) etc. Also each object can have an object manager: a
    routine to handle special object deletion requirements (for example
    to remove all file chunks before removing a file), or to flush
    objects (write modified chunks to disk).

    The main routines is cache_find() which is used to search for and
    place objects in a binary tree which is located by a tree pointer.
    cache_touch() and cache_untouch() are used to bump and decrement
    reference counts. Any object with a reference count of zero is a
    candidate for destruction - but if it requires special action
    before it is destroyed, such as deleting a subtree, flushing data
    to disk, etc, then it should have an 'object manager' function
    assigned which will be called at deletion time to take care of these
    needs.

    This version of the cache routines attempts to maintain binary tree
    balance by dynamically changing tree shape during search functions.
    All objects remain in the binary tree until destruction so that they
    can be re-used at any time. Objects with a zero reference count are
    special in that they are kept in a 'least recently used' linked list.
    When the reference count is decemented a flag is used to indicate
    whether the object is likely to be referenced again so that the object
    can be put in the 'correct' end of this list.

    Note: These routines are 'general' in that do not know anything
    about ODS2 objects or structures....
*/


#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include "ssdef.h"
#include "cache.h"


#define DEBUG                   /* Debug mode? */
#define IMBALANCE 5             /* Tree imbalance limit */
#define CACHELIM  256           /* Free object limit */
#define CACHEGOAL 128           /* Free object goal */

int cachefinds = 0;
int cachecreated = 0;
int cachepurges = 0;
int cachepeak = 0;
int cachecount = 0;
int cachefreecount = 0;
int cachedeletes = 0;

int cachedeleteing = 0;         /* Cache deletion in progress... */

struct CACHE lrulist = {&lrulist,&lrulist,NULL,NULL,NULL,NULL,0,0,1,0};


/* cache_show() - to print cache statistics */

void cache_show(void)
{
    printf("CACHE_SHOW Find %d Create %d Purge %d Peak %d Count %d Free %d\n",
           cachefinds,cachecreated,cachepurges,cachepeak,cachecount,cachefreecount);
    if (cachecreated - cachedeletes != cachecount) printf(" - Deleted %d\n",cachedeletes);
}


/* cache_refcount() - compute reference count for cache subtree... */

int cache_refcount(struct CACHE *cacheobj)
{
    register int refcount = 0;
    if (cacheobj != NULL) {
        refcount = cacheobj->refcount;
        if (cacheobj->left != NULL) refcount += cache_refcount(cacheobj->left);
        if (cacheobj->right != NULL) refcount += cache_refcount(cacheobj->right);
    }
    return refcount;
}


/* cache_delete() - blow away item from cache - allow item to select a proxy (!)
   and adjust 'the tree' containing the item. */

struct CACHE *cache_delete(struct CACHE *cacheobj)
{
    while (cacheobj->objmanager != NULL) {
        register struct CACHE *proxyobj;
        cachedeleteing = 1;
        proxyobj = (*cacheobj->objmanager) (cacheobj,0);
        cachedeleteing = 0;
        if (proxyobj == NULL) return NULL;
        if (proxyobj == cacheobj) break;
        cacheobj = proxyobj;
    }
#ifdef DEBUG
    if (cachedeleteing != 0) printf("CACHE deletion while delete in progress\n");
#endif
    if (cacheobj->refcount != 0) {
#ifdef DEBUG
        printf("CACHE attempt to delete referenced object %d:%d\n",
                cacheobj->objtype,cacheobj->hashval);
#endif
        return NULL;
    }
    cacheobj->lastlru->nextlru = cacheobj->nextlru;
    cacheobj->nextlru->lastlru = cacheobj->lastlru;
    if (cacheobj->left == NULL) {
        if (cacheobj->right == NULL) {
            *(cacheobj->parent) = NULL;
        } else {
            cacheobj->right->parent = cacheobj->parent;
            *(cacheobj->parent) = cacheobj->right;
        }
    } else {
        if (cacheobj->right == NULL) {
            cacheobj->left->parent = cacheobj->parent;
            *(cacheobj->parent) = cacheobj->left;
        } else {
            register struct CACHE *path = cacheobj->right;
            if (path->left != NULL) {
                do {
                    path = path->left;
                } while (path->left != NULL);
                *(path->parent) = path->right;
                if (path->right != NULL) path->right->parent = path->parent;
                path->right = cacheobj->right;
                path->right->parent = &path->right;
            }
            path->left = cacheobj->left;
            path->left->parent = &path->left;
            path->parent = cacheobj->parent;
            *(cacheobj->parent) = path;
            path->balance = 0;
        }
    }
    cachecount--;
    cachefreecount--;
    cachedeletes++;
#ifdef DEBUG
    cacheobj->nextlru = NULL;
    cacheobj->lastlru = NULL;
    cacheobj->left = NULL;
    cacheobj->right = NULL;
    cacheobj->parent = NULL;
    cacheobj->objmanager = NULL;
    cacheobj->hashval = 0;
    cacheobj->balance = 0;
    cacheobj->refcount = 0;
#endif
    free(cacheobj);
    return cacheobj;
}


/* cache_purge() - trim size of free list */

void cache_purge(void)
{
    if (cachedeleteing == 0) {
        register struct CACHE *cacheobj = lrulist.lastlru;
        cachepurges++;
        while (cachefreecount > CACHEGOAL && cacheobj != &lrulist) {
            register struct CACHE *lastobj = cacheobj->lastlru;
#ifdef DEBUG
            if (cacheobj->lastlru->nextlru != cacheobj ||
                cacheobj->nextlru->lastlru != cacheobj ||
                *(cacheobj->parent) != cacheobj) {
                printf("CACHE pointers in bad shape!\n");
            }
#endif
            if (cacheobj->refcount == 0) {
                if (cache_delete(cacheobj) != lastobj) cacheobj = lastobj;
            } else {
                cacheobj = lastobj;
            }
        }
    }
}



/* cache_flush() - flush modified entries in cache */

void cache_flush(void)
{
    register struct CACHE *cacheobj = lrulist.lastlru;
    while (cacheobj != &lrulist) {
        if (cacheobj->objmanager != NULL) {
            (*cacheobj->objmanager) (cacheobj,1);
        }
        cacheobj = cacheobj->lastlru;
    }
}


/* cache_remove() - delete all possible objects from cache subtree */

void cache_remove(struct CACHE *cacheobj)
{
    if (cacheobj != NULL) {
        if (cacheobj->left != NULL) cache_remove(cacheobj->left);
        if (cacheobj->right != NULL) cache_remove(cacheobj->right);
        if (cacheobj->refcount == 0) {
            struct CACHE *delobj;
            do {
                delobj = cache_delete(cacheobj);
            } while (delobj != NULL && delobj != cacheobj);
        }
    }
}

/* cache_touch() - to increase the access count on an object... */

void cache_touch(struct CACHE *cacheobj)
{
    if (cacheobj->refcount++ == 0) {
#ifdef DEBUG
        if (cacheobj->nextlru == NULL || cacheobj->lastlru == NULL) {
            printf("CACHE LRU pointers corrupt!\n");
        }
#endif
        cacheobj->nextlru->lastlru = cacheobj->lastlru;
        cacheobj->lastlru->nextlru = cacheobj->nextlru;
        cacheobj->nextlru = NULL;
        cacheobj->lastlru = NULL;
        cachefreecount--;
    }
}

/* cache_untouch() - to deaccess an object... */

void cache_untouch(struct CACHE *cacheobj,int recycle)
{
    if (cacheobj->refcount > 0) {
        if (--cacheobj->refcount == 0) {
            if (++cachefreecount >= CACHELIM) cache_purge();
#ifdef DEBUG
            if (cacheobj->nextlru != NULL || cacheobj->lastlru != NULL) {
                printf("CACHE LRU pointers corrupt\n");
            }
#endif
            if (recycle) {
                cacheobj->nextlru = lrulist.nextlru;
                cacheobj->lastlru = &lrulist;
                cacheobj->nextlru->lastlru = cacheobj;
                lrulist.nextlru = cacheobj;
            } else {
                cacheobj->lastlru = lrulist.lastlru;
                cacheobj->nextlru = &lrulist;
                cacheobj->lastlru->nextlru = cacheobj;
                lrulist.lastlru = cacheobj;
            }
        }
#ifdef DEBUG
    } else {
        printf("CACHE untouch limit exceeded\n");
#endif
    }
}

/* cache_find() - to find or create cache entries...

        The grand plan here was to use a hash code as a quick key
        and call a compare function for duplicates. So far no data
        type actually works like this - they either have a unique binary
        key, or all records rely on the compare function - sigh!
        Never mind, the potential is there!  :-)

        This version will call a creation function to allocate and
        initialize an object if it is not found.
*/

void *cache_find(void **root,unsigned hashval,void *keyval,unsigned *retsts,
                 int (*compare_func) (unsigned hashval,void *keyval,void *node),
                 void *(*create_func) (unsigned hashval,void *keyval,unsigned *retsts))
{
    register struct CACHE *cacheobj,**parent = (struct CACHE **) root;
    cachefinds++;
    while ((cacheobj = *parent) != NULL) {
        register int cmp = hashval - cacheobj->hashval;
#ifdef DEBUG
        if (cacheobj->parent != parent) {
            printf("CACHE Parent pointer is corrupt\n");
        }
#endif
        if (cmp == 0 && compare_func != NULL) cmp = (*compare_func) (hashval,keyval,cacheobj);
        if (cmp == 0) {
            cache_touch(cacheobj);
            if (retsts != NULL) *retsts = SS$_NORMAL;
            return cacheobj;
        }
        if (cmp < 0) {
#ifdef IMBALANCE
            register struct CACHE *left_path = cacheobj->left;
            if (left_path != NULL && cacheobj->balance-- < -IMBALANCE) {
                cacheobj->left = left_path->right;
                if (cacheobj->left != NULL) cacheobj->left->parent = &cacheobj->left;
                left_path->right = cacheobj;
                cacheobj->parent = &left_path->right;
                *parent = left_path;
                left_path->parent = parent;
                cacheobj->balance = 0;
            } else {
                parent = &cacheobj->left;
            }
        } else {
            register struct CACHE *right_path = cacheobj->right;
            if (right_path != NULL && cacheobj->balance++ > IMBALANCE) {
                cacheobj->right = right_path->left;
                if (cacheobj->right != NULL) cacheobj->right->parent = &cacheobj->right;
                right_path->left = cacheobj;
                cacheobj->parent = &right_path->left;
                *parent = right_path;
                right_path->parent = parent;
                cacheobj->balance = 0;
            } else {
                parent = &cacheobj->right;
            }
#else
            parent = &cacheobj->left;
        } else {
            parent = &cacheobj->right;
#endif
        }
    }
    if (create_func == NULL) {
        if (retsts != NULL) *retsts = SS$_ITEMNOTFOUND;
    } else {
        cacheobj = (*create_func) (hashval,keyval,retsts);
        if (cacheobj != NULL) {
            cacheobj->nextlru = NULL;
            cacheobj->lastlru = NULL;
            cacheobj->left = NULL;
            cacheobj->right = NULL;
            cacheobj->parent = parent;
            cacheobj->hashval = hashval;
            cacheobj->balance = 0;
            cacheobj->refcount = 1;
            *parent = cacheobj;
            cachecreated++;
            if (cachecount++ >= cachepeak) cachepeak = cachecount;
        }
    }
    return cacheobj;
}
