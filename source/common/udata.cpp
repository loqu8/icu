/*
******************************************************************************
*
*   Copyright (C) 1999-2015, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
******************************************************************************
*   file name:  udata.cpp
*   encoding:   US-ASCII
*   tab size:   8 (not used)
*   indentation:4
*
*   created on: 1999oct25
*   created by: Markus W. Scherer
*/

#include "unicode/utypes.h"  /* U_PLATFORM etc. */

#ifdef __GNUC__
/* if gcc
#define ATTRIBUTE_WEAK __attribute__ ((weak))
might have to #include some other header
*/
#endif

#include "unicode/putil.h"
#include "unicode/udata.h"
#include "unicode/uversion.h"
#include "charstr.h"
#include "cmemory.h"
#include "cstring.h"
#include "mutex.h"
#include "putilimp.h"
#include "uassert.h"
#include "ucln_cmn.h"
#include "ucmndata.h"
#include "udatamem.h"
#include "uhash.h"
#include "umapfile.h"
#include "umutex.h"

#include "dictionarydata.h"
#include "unicode/ucharstriebuilder.h"
#include "unicode/bytestriebuilder.h"
#include "unicode/ucharstrie.h"
#include "unicode/bytestrie.h"
#include "unicode/utf16.h"
#include "unicode/rbbi.h"

/***********************************************************************
*
*   Notes on the organization of the ICU data implementation
*
*      All of the public API is defined in udata.h
*
*      The implementation is split into several files...
*
*         - udata.c  (this file) contains higher level code that knows about
*                     the search paths for locating data, caching opened data, etc.
*
*         - umapfile.c  contains the low level platform-specific code for actually loading
*                     (memory mapping, file reading, whatever) data into memory.
*
*         - ucmndata.c  deals with the tables of contents of ICU data items within
*                     an ICU common format data file.  The implementation includes
*                     an abstract interface and support for multiple TOC formats.
*                     All knowledge of any specific TOC format is encapsulated here.
*
*         - udatamem.c has code for managing UDataMemory structs.  These are little
*                     descriptor objects for blocks of memory holding ICU data of
*                     various types.
*/

/* configuration ---------------------------------------------------------- */

/* If you are excruciatingly bored turn this on .. */
/* #define UDATA_DEBUG 1 */

#if defined(UDATA_DEBUG)
#   include <stdio.h>
#endif

U_NAMESPACE_USE

/*
 *  Forward declarations
 */
static UDataMemory *udata_findCachedData(const char *path);

/***********************************************************************
*
*    static (Global) data
*
************************************************************************/

/*
 * Pointers to the common ICU data.
 *
 * We store multiple pointers to ICU data packages and iterate through them
 * when looking for a data item.
 *
 * It is possible to combine this with dependency inversion:
 * One or more data package libraries may export
 * functions that each return a pointer to their piece of the ICU data,
 * and this file would import them as weak functions, without a
 * strong linker dependency from the common library on the data library.
 *
 * Then we can have applications depend on only that part of ICU's data
 * that they really need, reducing the size of binaries that take advantage
 * of this.
 */
static UDataMemory *gCommonICUDataArray[10] = { NULL };   // Access protected by icu global mutex.

static u_atomic_int32_t gHaveTriedToLoadCommonData = ATOMIC_INT32_T_INITIALIZER(0);  //  See extendICUData().

static UHashtable  *gCommonDataCache = NULL;  /* Global hash table of opened ICU data files.  */
static icu::UInitOnce gCommonDataCacheInitOnce = U_INITONCE_INITIALIZER;

static UDataFileAccess  gDataFileAccess = UDATA_DEFAULT_ACCESS;  // Access not synchronized.
                                                                 // Modifying is documented as thread-unsafe.

static UBool U_CALLCONV
udata_cleanup(void)
{
    int32_t i;

    if (gCommonDataCache) {             /* Delete the cache of user data mappings.  */
        uhash_close(gCommonDataCache);  /*   Table owns the contents, and will delete them. */
        gCommonDataCache = NULL;        /*   Cleanup is not thread safe.                */
    }
    gCommonDataCacheInitOnce.reset();

    for (i = 0; i < UPRV_LENGTHOF(gCommonICUDataArray) && gCommonICUDataArray[i] != NULL; ++i) {
        udata_close(gCommonICUDataArray[i]);
        gCommonICUDataArray[i] = NULL;
    }
    gHaveTriedToLoadCommonData = 0;

    return TRUE;                   /* Everything was cleaned up */
}

static UBool U_CALLCONV
findCommonICUDataByName(const char *inBasename)
{
    UBool found = FALSE;
    int32_t i;

    UDataMemory  *pData = udata_findCachedData(inBasename);
    if (pData == NULL)
        return FALSE;

    {
        Mutex lock;
        for (i = 0; i < UPRV_LENGTHOF(gCommonICUDataArray); ++i) {
            if ((gCommonICUDataArray[i] != NULL) && (gCommonICUDataArray[i]->pHeader == pData->pHeader)) {
                /* The data pointer is already in the array. */
                found = TRUE;
                break;
            }
        }
    }
    return found;
}


/*
 * setCommonICUData.   Set a UDataMemory to be the global ICU Data
 */
static UBool
swapCommonICUData(UDataMemory *pData,     /*  The new common data.  Belongs to caller, we copy it. */
                  UDataMemory *pOldData,  /*  The original data to swap out.     */
                  UBool       warn,       /*  If true, set USING_DEFAULT warning if ICUData was    */
                                          /*    changed by another thread before we got to it.     */
                  UErrorCode *pErr)
{
    UDataMemory  *newCommonData = UDataMemory_createNewInstance(pErr);
    int32_t i;
    UBool didUpdate = FALSE;
    if (U_FAILURE(*pErr)) {
        return FALSE;
    }

    /*  For the assignment, other threads must cleanly see either the old            */
    /*    or the new, not some partially initialized new.  The old can not be        */
    /*    deleted - someone may still have a pointer to it lying around in           */
    /*    their locals.                                                              */
    UDatamemory_assign(newCommonData, pData);
    umtx_lock(NULL);
    for (i = 0; i < UPRV_LENGTHOF(gCommonICUDataArray); ++i) {
        if ((gCommonICUDataArray[i] == NULL) ||
            ((pOldData != NULL) && (gCommonICUDataArray[i]->pHeader == pOldData->pHeader))) {
            gCommonICUDataArray[i] = newCommonData;
            didUpdate = TRUE;
            break;
        } else if (gCommonICUDataArray[i]->pHeader == pData->pHeader) {
            /* The same data pointer is already in the array. */
            break;
        }
    }
    umtx_unlock(NULL);

    if (i == UPRV_LENGTHOF(gCommonICUDataArray) && warn) {
        *pErr = U_USING_DEFAULT_WARNING;
    }
    if (didUpdate) {
        ucln_common_registerCleanup(UCLN_COMMON_UDATA, udata_cleanup);
    } else {
        uprv_free(newCommonData);
    }
    return didUpdate;
}

static UBool
setCommonICUData(UDataMemory *pData,     /*  The new common data.  Belongs to caller, we copy it. */
                 UBool       warn,       /*  If true, set USING_DEFAULT warning if ICUData was    */
                                         /*    changed by another thread before we got to it.     */
                 UErrorCode *pErr)
{
    return swapCommonICUData(pData, NULL, warn, pErr);
}

static UBool
setCommonICUDataPointer(const void *pData, UBool /*warn*/, UErrorCode *pErrorCode) {
    UDataMemory tData;
    UDataMemory_init(&tData);
    UDataMemory_setData(&tData, pData);
    udata_checkCommonData(&tData, pErrorCode);
    return setCommonICUData(&tData, FALSE, pErrorCode);
}

static const char *
findBasename(const char *path) {
    const char *basename=uprv_strrchr(path, U_FILE_SEP_CHAR);
    if(basename==NULL) {
        return path;
    } else {
        return basename+1;
    }
}

#ifdef UDATA_DEBUG
static const char *
packageNameFromPath(const char *path)
{
    if((path == NULL) || (*path == 0)) {
        return U_ICUDATA_NAME;
    }

    path = findBasename(path);

    if((path == NULL) || (*path == 0)) {
        return U_ICUDATA_NAME;
    }

    return path;
}
#endif

/*----------------------------------------------------------------------*
 *                                                                      *
 *   Cache for common data                                              *
 *      Functions for looking up or adding entries to a cache of        *
 *      data that has been previously opened.  Avoids a potentially     *
 *      expensive operation of re-opening the data for subsequent       *
 *      uses.                                                           *
 *                                                                      *
 *      Data remains cached for the duration of the process.            *
 *                                                                      *
 *----------------------------------------------------------------------*/

typedef struct DataCacheElement {
    char          *name;
    UDataMemory   *item;
} DataCacheElement;



/*
 * Deleter function for DataCacheElements.
 *         udata cleanup function closes the hash table; hash table in turn calls back to
 *         here for each entry.
 */
static void U_CALLCONV DataCacheElement_deleter(void *pDCEl) {
    DataCacheElement *p = (DataCacheElement *)pDCEl;
    udata_close(p->item);              /* unmaps storage */
    uprv_free(p->name);                /* delete the hash key string. */
    uprv_free(pDCEl);                  /* delete 'this'          */
}

static void udata_initHashTable() {
    UErrorCode err = U_ZERO_ERROR;
    U_ASSERT(gCommonDataCache == NULL);
    gCommonDataCache = uhash_open(uhash_hashChars, uhash_compareChars, NULL, &err);
    if (U_FAILURE(err)) {
        // TODO: handle errors better.
        gCommonDataCache = NULL;
    }
    if (gCommonDataCache != NULL) {
        uhash_setValueDeleter(gCommonDataCache, DataCacheElement_deleter);
        ucln_common_registerCleanup(UCLN_COMMON_UDATA, udata_cleanup);
    }
}

 /*   udata_getCacheHashTable()
  *     Get the hash table used to store the data cache entries.
  *     Lazy create it if it doesn't yet exist.
  */
static UHashtable *udata_getHashTable() {
    umtx_initOnce(gCommonDataCacheInitOnce, &udata_initHashTable);
    return gCommonDataCache;
}



static UDataMemory *udata_findCachedData(const char *path)
{
    UHashtable        *htable;
    UDataMemory       *retVal = NULL;
    DataCacheElement  *el;
    const char        *baseName;

    baseName = findBasename(path);   /* Cache remembers only the base name, not the full path. */
    htable = udata_getHashTable();
    umtx_lock(NULL);
    el = (DataCacheElement *)uhash_get(htable, baseName);
    umtx_unlock(NULL);
    if (el != NULL) {
        retVal = el->item;
    }
#ifdef UDATA_DEBUG
    fprintf(stderr, "Cache: [%s] -> %p\n", baseName, retVal);
#endif
    return retVal;
}


static UDataMemory *udata_cacheDataItem(const char *path, UDataMemory *item, UErrorCode *pErr) {
    DataCacheElement *newElement;
    const char       *baseName;
    int32_t           nameLen;
    UHashtable       *htable;
    DataCacheElement *oldValue = NULL;
    UErrorCode        subErr = U_ZERO_ERROR;

    if (U_FAILURE(*pErr)) {
        return NULL;
    }

    /* Create a new DataCacheElement - the thingy we store in the hash table -
     * and copy the supplied path and UDataMemoryItems into it.
     */
    newElement = (DataCacheElement *)uprv_malloc(sizeof(DataCacheElement));
    if (newElement == NULL) {
        *pErr = U_MEMORY_ALLOCATION_ERROR;
        return NULL;
    }
    newElement->item = UDataMemory_createNewInstance(pErr);
    if (U_FAILURE(*pErr)) {
        uprv_free(newElement);
        return NULL;
    }
    UDatamemory_assign(newElement->item, item);

    baseName = findBasename(path);
    nameLen = (int32_t)uprv_strlen(baseName);
    newElement->name = (char *)uprv_malloc(nameLen+1);
    if (newElement->name == NULL) {
        *pErr = U_MEMORY_ALLOCATION_ERROR;
        uprv_free(newElement->item);
        uprv_free(newElement);
        return NULL;
    }
    uprv_strcpy(newElement->name, baseName);

    /* Stick the new DataCacheElement into the hash table.
    */
    htable = udata_getHashTable();
    umtx_lock(NULL);
    oldValue = (DataCacheElement *)uhash_get(htable, path);
    if (oldValue != NULL) {
        subErr = U_USING_DEFAULT_WARNING;
    }
    else {
        uhash_put(
            htable,
            newElement->name,               /* Key   */
            newElement,                     /* Value */
            &subErr);
    }
    umtx_unlock(NULL);

#ifdef UDATA_DEBUG
    fprintf(stderr, "Cache: [%s] <<< %p : %s. vFunc=%p\n", newElement->name, 
    newElement->item, u_errorName(subErr), newElement->item->vFuncs);
#endif

    if (subErr == U_USING_DEFAULT_WARNING || U_FAILURE(subErr)) {
        *pErr = subErr; /* copy sub err unto fillin ONLY if something happens. */
        uprv_free(newElement->name);
        uprv_free(newElement->item);
        uprv_free(newElement);
        return oldValue ? oldValue->item : NULL;
    }

    return newElement->item;
}

/*----------------------------------------------------------------------*==============
 *                                                                      *
 *  Path management.  Could be shared with other tools/etc if need be   *
 * later on.                                                            *
 *                                                                      *
 *----------------------------------------------------------------------*/

#define U_DATA_PATHITER_BUFSIZ  128        /* Size of local buffer for paths         */
                                           /*   Overflow causes malloc of larger buf */

U_NAMESPACE_BEGIN

class UDataPathIterator
{
public:
    UDataPathIterator(const char *path, const char *pkg,
                      const char *item, const char *suffix, UBool doCheckLastFour,
                      UErrorCode *pErrorCode);
    const char *next(UErrorCode *pErrorCode);

private:
    const char *path;                              /* working path (u_icudata_Dir) */
    const char *nextPath;                          /* path following this one */
    const char *basename;                          /* item's basename (icudt22e_mt.res)*/
    const char *suffix;                            /* item suffix (can be null) */

    uint32_t    basenameLen;                       /* length of basename */

    CharString  itemPath;                          /* path passed in with item name */
    CharString  pathBuffer;                        /* output path for this it'ion */
    CharString  packageStub;                       /* example:  "/icudt28b". Will ignore that leaf in set paths. */

    UBool       checkLastFour;                     /* if TRUE then allow paths such as '/foo/myapp.dat'
                                                    * to match, checks last 4 chars of suffix with
                                                    * last 4 of path, then previous chars. */
};

/**
 * @param iter  The iterator to be initialized. Its current state does not matter. 
 * @param path  The full pathname to be iterated over.  If NULL, defaults to U_ICUDATA_NAME 
 * @param pkg   Package which is being searched for, ex "icudt28l".  Will ignore leave directories such as /icudt28l 
 * @param item  Item to be searched for.  Can include full path, such as /a/b/foo.dat 
 * @param suffix  Optional item suffix, if not-null (ex. ".dat") then 'path' can contain 'item' explicitly.
 *               Ex:   'stuff.dat' would be found in '/a/foo:/tmp/stuff.dat:/bar/baz' as item #2.   
 *                     '/blarg/stuff.dat' would also be found.
 */
UDataPathIterator::UDataPathIterator(const char *inPath, const char *pkg,
                                     const char *item, const char *inSuffix, UBool doCheckLastFour,
                                     UErrorCode *pErrorCode)
{
#ifdef UDATA_DEBUG
        fprintf(stderr, "SUFFIX1=%s PATH=%s\n", inSuffix, inPath);
#endif
    /** Path **/
    if(inPath == NULL) {
        path = u_getDataDirectory();
    } else {
        path = inPath;
    }

    /** Package **/
    if(pkg != NULL) {
      packageStub.append(U_FILE_SEP_CHAR, *pErrorCode).append(pkg, *pErrorCode);
#ifdef UDATA_DEBUG
      fprintf(stderr, "STUB=%s [%d]\n", packageStub.data(), packageStub.length());
#endif
    }

    /** Item **/
    basename = findBasename(item);
    basenameLen = (int32_t)uprv_strlen(basename);

    /** Item path **/
    if(basename == item) {
        nextPath = path;
    } else {
        itemPath.append(item, (int32_t)(basename-item), *pErrorCode);
        nextPath = itemPath.data();
    }
#ifdef UDATA_DEBUG
    fprintf(stderr, "SUFFIX=%s [%p]\n", inSuffix, inSuffix);
#endif

    /** Suffix  **/
    if(inSuffix != NULL) {
        suffix = inSuffix;
    } else {
        suffix = "";
    }

    checkLastFour = doCheckLastFour;

    /* pathBuffer will hold the output path strings returned by this iterator */

#ifdef UDATA_DEBUG
    fprintf(stderr, "%p: init %s -> [path=%s], [base=%s], [suff=%s], [itempath=%s], [nextpath=%s], [checklast4=%s]\n",
            iter,
            item,
            path,
            basename,
            suffix,
            itemPath.data(),
            nextPath,
            checkLastFour?"TRUE":"false");
#endif
}

/**
 * Get the next path on the list.
 *
 * @param iter The Iter to be used 
 * @param len  If set, pointer to the length of the returned path, for convenience. 
 * @return Pointer to the next path segment, or NULL if there are no more.
 */
const char *UDataPathIterator::next(UErrorCode *pErrorCode)
{
    if(U_FAILURE(*pErrorCode)) {
        return NULL;
    }

    const char *currentPath = NULL;
    int32_t     pathLen = 0;
    const char *pathBasename;

    do
    {
        if( nextPath == NULL ) {
            break;
        }
        currentPath = nextPath;

        if(nextPath == itemPath.data()) { /* we were processing item's path. */
            nextPath = path; /* start with regular path next tm. */
            pathLen = (int32_t)uprv_strlen(currentPath);
        } else {
            /* fix up next for next time */
            nextPath = uprv_strchr(currentPath, U_PATH_SEP_CHAR);
            if(nextPath == NULL) {
                /* segment: entire path */
                pathLen = (int32_t)uprv_strlen(currentPath); 
            } else {
                /* segment: until next segment */
                pathLen = (int32_t)(nextPath - currentPath);
                /* skip divider */
                nextPath ++;
            }
        }

        if(pathLen == 0) {
            continue;
        }

#ifdef UDATA_DEBUG
        fprintf(stderr, "rest of path (IDD) = %s\n", currentPath);
        fprintf(stderr, "                     ");
        { 
            uint32_t qqq;
            for(qqq=0;qqq<pathLen;qqq++)
            {
                fprintf(stderr, " ");
            }

            fprintf(stderr, "^\n");
        }
#endif
        pathBuffer.clear().append(currentPath, pathLen, *pErrorCode);

        /* check for .dat files */
        pathBasename = findBasename(pathBuffer.data());

        if(checkLastFour == TRUE && 
           (pathLen>=4) &&
           uprv_strncmp(pathBuffer.data() +(pathLen-4), suffix, 4)==0 && /* suffix matches */
           uprv_strncmp(findBasename(pathBuffer.data()), basename, basenameLen)==0  && /* base matches */
           uprv_strlen(pathBasename)==(basenameLen+4)) { /* base+suffix = full len */

#ifdef UDATA_DEBUG
            fprintf(stderr, "Have %s file on the path: %s\n", suffix, pathBuffer.data());
#endif
            /* do nothing */
        }
        else 
        {       /* regular dir path */
            if(pathBuffer[pathLen-1] != U_FILE_SEP_CHAR) {
                if((pathLen>=4) &&
                   uprv_strncmp(pathBuffer.data()+(pathLen-4), ".dat", 4) == 0)
                {
#ifdef UDATA_DEBUG
                    fprintf(stderr, "skipping non-directory .dat file %s\n", pathBuffer.data());
#endif
                    continue;
                }

                /* Check if it is a directory with the same name as our package */
                if(!packageStub.isEmpty() &&
                   (pathLen > packageStub.length()) &&
                   !uprv_strcmp(pathBuffer.data() + pathLen - packageStub.length(), packageStub.data())) {
#ifdef UDATA_DEBUG
                  fprintf(stderr, "Found stub %s (will add package %s of len %d)\n", packageStub.data(), basename, basenameLen);
#endif
                  pathBuffer.truncate(pathLen - packageStub.length());
                }
                pathBuffer.append(U_FILE_SEP_CHAR, *pErrorCode);
            }

            /* + basename */
            pathBuffer.append(packageStub.data()+1, packageStub.length()-1, *pErrorCode);

            if(*suffix)  /* tack on suffix */
            {
                pathBuffer.append(suffix, *pErrorCode);
            }
        }

#ifdef UDATA_DEBUG
        fprintf(stderr, " -->  %s\n", pathBuffer.data());
#endif

        return pathBuffer.data();

    } while(path);

    /* fell way off the end */
    return NULL;
}

U_NAMESPACE_END

/* ==================================================================================*/


/*----------------------------------------------------------------------*
 *                                                                      *
 *  Add a static reference to the common data  library                  *
 *   Unless overridden by an explicit udata_setCommonData, this will be *
 *      our common data.                                                *
 *                                                                      *
 *----------------------------------------------------------------------*/
extern "C" const DataHeader U_DATA_API U_ICUDATA_ENTRY_POINT;

/*
 * This would be a good place for weak-linkage declarations of
 * partial-data-library access functions where each returns a pointer
 * to its data package, if it is linked in.
 */
/*
extern const void *uprv_getICUData_collation(void) ATTRIBUTE_WEAK;
extern const void *uprv_getICUData_conversion(void) ATTRIBUTE_WEAK;
*/

/*----------------------------------------------------------------------*
 *                                                                      *
 *   openCommonData   Attempt to open a common format (.dat) file       *
 *                    Map it into memory (if it's not there already)    *
 *                    and return a UDataMemory object for it.           *
 *                                                                      *
 *                    If the requested data is already open and cached  *
 *                       just return the cached UDataMem object.        *
 *                                                                      *
 *----------------------------------------------------------------------*/
static UDataMemory *
openCommonData(const char *path,          /*  Path from OpenChoice?          */
               int32_t commonDataIndex,   /*  ICU Data (index >= 0) if path == NULL */
               UErrorCode *pErrorCode)
{
    UDataMemory tData;
    const char *pathBuffer;
    const char *inBasename;

    if (U_FAILURE(*pErrorCode)) {
        return NULL;
    }

    UDataMemory_init(&tData);

    /* ??????? TODO revisit this */ 
    if (commonDataIndex >= 0) {
        /* "mini-cache" for common ICU data */
        if(commonDataIndex >= UPRV_LENGTHOF(gCommonICUDataArray)) {
            return NULL;
        }
        {
            Mutex lock;
            if(gCommonICUDataArray[commonDataIndex] != NULL) {
                return gCommonICUDataArray[commonDataIndex];
            }
            int32_t i;
            for(i = 0; i < commonDataIndex; ++i) {
                if(gCommonICUDataArray[i]->pHeader == &U_ICUDATA_ENTRY_POINT) {
                    /* The linked-in data is already in the list. */
                    return NULL;
                }
            }
        }

        /* Add the linked-in data to the list. */
        /*
         * This is where we would check and call weakly linked partial-data-library
         * access functions.
         */
        /*
        if (uprv_getICUData_collation) {
            setCommonICUDataPointer(uprv_getICUData_collation(), FALSE, pErrorCode);
        }
        if (uprv_getICUData_conversion) {
            setCommonICUDataPointer(uprv_getICUData_conversion(), FALSE, pErrorCode);
        }
        */
        setCommonICUDataPointer(&U_ICUDATA_ENTRY_POINT, FALSE, pErrorCode);
        {
            Mutex lock;
            return gCommonICUDataArray[commonDataIndex];
        }
    }


    /* request is NOT for ICU Data.  */

    /* Find the base name portion of the supplied path.   */
    /*   inBasename will be left pointing somewhere within the original path string.      */
    inBasename = findBasename(path);
#ifdef UDATA_DEBUG
    fprintf(stderr, "inBasename = %s\n", inBasename);
#endif

    if(*inBasename==0) {
        /* no basename.     This will happen if the original path was a directory name,   */
        /*    like  "a/b/c/".   (Fallback to separate files will still work.)             */
#ifdef UDATA_DEBUG
        fprintf(stderr, "ocd: no basename in %s, bailing.\n", path);
#endif
        *pErrorCode=U_FILE_ACCESS_ERROR;
        return NULL;
    }

   /* Is the requested common data file already open and cached?                     */
   /*   Note that the cache is keyed by the base name only.  The rest of the path,   */
   /*     if any, is not considered.                                                 */
   {
        UDataMemory  *dataToReturn = udata_findCachedData(inBasename);
        if (dataToReturn != NULL) {
            return dataToReturn;
        }
    }

    /* Requested item is not in the cache.
     * Hunt it down, trying all the path locations
     */

    UDataPathIterator iter(u_getDataDirectory(), inBasename, path, ".dat", TRUE, pErrorCode);

    while((UDataMemory_isLoaded(&tData)==FALSE) && (pathBuffer = iter.next(pErrorCode)) != NULL)
    {
#ifdef UDATA_DEBUG
        fprintf(stderr, "ocd: trying path %s - ", pathBuffer);
#endif
        uprv_mapFile(&tData, pathBuffer);
#ifdef UDATA_DEBUG
        fprintf(stderr, "%s\n", UDataMemory_isLoaded(&tData)?"LOADED":"not loaded");
#endif
    }

#if defined(OS390_STUBDATA) && defined(OS390BATCH)
    if (!UDataMemory_isLoaded(&tData)) {
        char ourPathBuffer[1024];
        /* One more chance, for extendCommonData() */
        uprv_strncpy(ourPathBuffer, path, 1019);
        ourPathBuffer[1019]=0;
        uprv_strcat(ourPathBuffer, ".dat");
        uprv_mapFile(&tData, ourPathBuffer);
    }
#endif

    if (!UDataMemory_isLoaded(&tData)) {
        /* no common data */
        *pErrorCode=U_FILE_ACCESS_ERROR;
        return NULL;
    }

    /* we have mapped a file, check its header */
    udata_checkCommonData(&tData, pErrorCode);


    /* Cache the UDataMemory struct for this .dat file,
     *   so we won't need to hunt it down and map it again next time
     *   something is needed from it.                */
    return udata_cacheDataItem(inBasename, &tData, pErrorCode);
}


/*----------------------------------------------------------------------*
 *                                                                      *
 *   extendICUData   If the full set of ICU data was not loaded at      *
 *                   program startup, load it now.  This function will  *
 *                   be called when the lookup of an ICU data item in   *
 *                   the common ICU data fails.                         *
 *                                                                      *
 *                   return true if new data is loaded, false otherwise.*
 *                                                                      *
 *----------------------------------------------------------------------*/
static UBool extendICUData(UErrorCode *pErr)
{
    UDataMemory   *pData;
    UDataMemory   copyPData;
    UBool         didUpdate = FALSE;

    /*
     * There is a chance for a race condition here.
     * Normally, ICU data is loaded from a DLL or via mmap() and
     * setCommonICUData() will detect if the same address is set twice.
     * If ICU is built with data loading via fread() then the address will
     * be different each time the common data is loaded and we may add
     * multiple copies of the data.
     * In this case, use a mutex to prevent the race.
     * Use a specific mutex to avoid nested locks of the global mutex.
     */
#if MAP_IMPLEMENTATION==MAP_STDIO
    static UMutex extendICUDataMutex = U_MUTEX_INITIALIZER;
    umtx_lock(&extendICUDataMutex);
#endif
    if(!umtx_loadAcquire(gHaveTriedToLoadCommonData)) {
        /* See if we can explicitly open a .dat file for the ICUData. */
        pData = openCommonData(
                   U_ICUDATA_NAME,            /*  "icudt20l" , for example.          */
                   -1,                        /*  Pretend we're not opening ICUData  */
                   pErr);

        /* How about if there is no pData, eh... */

       UDataMemory_init(&copyPData);
       if(pData != NULL) {
          UDatamemory_assign(&copyPData, pData);
          copyPData.map = 0;              /* The mapping for this data is owned by the hash table */
          copyPData.mapAddr = 0;          /*   which will unmap it when ICU is shut down.         */
                                          /* CommonICUData is also unmapped when ICU is shut down.*/
                                          /* To avoid unmapping the data twice, zero out the map  */
                                          /*   fields in the UDataMemory that we're assigning     */
                                          /*   to CommonICUData.                                  */

          didUpdate = /* no longer using this result */
              setCommonICUData(&copyPData,/*  The new common data.                                */
                       FALSE,             /*  No warnings if write didn't happen                  */
                       pErr);             /*  setCommonICUData honors errors; NOP if error set    */
        }

        umtx_storeRelease(gHaveTriedToLoadCommonData, 1);
    }

    didUpdate = findCommonICUDataByName(U_ICUDATA_NAME);  /* Return 'true' when a racing writes out the extended                        */
                                                          /* data after another thread has failed to see it (in openCommonData), so     */
                                                          /* extended data can be examined.                                             */
                                                          /* Also handles a race through here before gHaveTriedToLoadCommonData is set. */

#if MAP_IMPLEMENTATION==MAP_STDIO
    umtx_unlock(&extendICUDataMutex);
#endif
    return didUpdate;               /* Return true if ICUData pointer was updated.   */
                                    /*   (Could potentialy have been done by another thread racing */
                                    /*   us through here, but that's fine, we still return true    */
                                    /*   so that current thread will also examine extended data.   */
}

/*----------------------------------------------------------------------*
 *                                                                      *
 *   udata_setCommonData                                                *
 *                                                                      *
 *----------------------------------------------------------------------*/
U_CAPI void U_EXPORT2
udata_swapCommonData(const void *data, const void *oldData, UErrorCode *pErrorCode) {
    UDataMemory dataMemory;
    UDataMemory oldDataMemory;

    if(pErrorCode==NULL || U_FAILURE(*pErrorCode)) {
        return;
    }

    if(data==NULL) {
        *pErrorCode=U_ILLEGAL_ARGUMENT_ERROR;
        return;
    }

    /* set the data pointer and test for validity */
    UDataMemory_init(&dataMemory);
    UDataMemory_setData(&dataMemory, data);
    udata_checkCommonData(&dataMemory, pErrorCode);
    if (U_FAILURE(*pErrorCode)) {return;}

    if (oldData != NULL)
    {
        UDataMemory_init(&oldDataMemory);
        UDataMemory_setData(&oldDataMemory, oldData);
        udata_checkCommonData(&oldDataMemory, pErrorCode);
        if (U_FAILURE(*pErrorCode)) {return;}

        swapCommonICUData(&dataMemory, &oldDataMemory, TRUE, pErrorCode);
        return;
    }

    /* we have good data */
    /* Set it up as the ICU Common Data.  */
    setCommonICUData(&dataMemory, TRUE, pErrorCode);
}

U_CAPI void U_EXPORT2
udata_setCommonData(const void *data, UErrorCode *pErrorCode) {
    udata_swapCommonData(data, NULL, pErrorCode);
}

/*---------------------------------------------------------------------------
 *
 *  udata_setAppData
 *
 *---------------------------------------------------------------------------- */
U_CAPI void U_EXPORT2
udata_setAppData(const char *path, const void *data, UErrorCode *err)
{
    UDataMemory     udm;

    if(err==NULL || U_FAILURE(*err)) {
        return;
    }
    if(data==NULL) {
        *err=U_ILLEGAL_ARGUMENT_ERROR;
        return;
    }

    UDataMemory_init(&udm);
    UDataMemory_setData(&udm, data);
    udata_checkCommonData(&udm, err);
    udata_cacheDataItem(path, &udm, err);
}

/*----------------------------------------------------------------------------*
 *                                                                            *
 *  checkDataItem     Given a freshly located/loaded data item, either        *
 *                    an entry in a common file or a separately loaded file,  *
 *                    sanity check its header, and see if the data is         *
 *                    acceptable to the app.                                  *
 *                    If the data is good, create and return a UDataMemory    *
 *                    object that can be returned to the application.         *
 *                    Return NULL on any sort of failure.                     *
 *                                                                            *
 *----------------------------------------------------------------------------*/
static UDataMemory *
checkDataItem
(
 const DataHeader         *pHeader,         /* The data item to be checked.                */
 UDataMemoryIsAcceptable  *isAcceptable,    /* App's call-back function                    */
 void                     *context,         /*   pass-thru param for above.                */
 const char               *type,            /*   pass-thru param for above.                */
 const char               *name,            /*   pass-thru param for above.                */
 UErrorCode               *nonFatalErr,     /* Error code if this data was not acceptable  */
                                            /*   but openChoice should continue with       */
                                            /*   trying to get data from fallback path.    */
 UErrorCode               *fatalErr         /* Bad error, caller should return immediately */
 )
{
    UDataMemory  *rDataMem = NULL;          /* the new UDataMemory, to be returned.        */

    if (U_FAILURE(*fatalErr)) {
        return NULL;
    }

    if(pHeader->dataHeader.magic1==0xda &&
        pHeader->dataHeader.magic2==0x27 &&
        (isAcceptable==NULL || isAcceptable(context, type, name, &pHeader->info))
    ) {
        rDataMem=UDataMemory_createNewInstance(fatalErr);
        if (U_FAILURE(*fatalErr)) {
            return NULL;
        }
        rDataMem->pHeader = pHeader;
    } else {
        /* the data is not acceptable, look further */
        /* If we eventually find something good, this errorcode will be */
        /*    cleared out.                                              */
        *nonFatalErr=U_INVALID_FORMAT_ERROR;
    }
    return rDataMem;
}

/**
 * @return 0 if not loaded, 1 if loaded or err 
 */
static UDataMemory *doLoadFromIndividualFiles(const char *pkgName, 
        const char *dataPath, const char *tocEntryPathSuffix,
            /* following arguments are the same as doOpenChoice itself */
            const char *path, const char *type, const char *name,
             UDataMemoryIsAcceptable *isAcceptable, void *context,
             UErrorCode *subErrorCode,
             UErrorCode *pErrorCode)
{
    const char         *pathBuffer;
    UDataMemory         dataMemory;
    UDataMemory *pEntryData;

    /* look in ind. files: package\nam.typ  ========================= */
    /* init path iterator for individual files */
    UDataPathIterator iter(dataPath, pkgName, path, tocEntryPathSuffix, FALSE, pErrorCode);

    while((pathBuffer = iter.next(pErrorCode)))
    {
#ifdef UDATA_DEBUG
        fprintf(stderr, "UDATA: trying individual file %s\n", pathBuffer);
#endif
        if(uprv_mapFile(&dataMemory, pathBuffer))
        {
            pEntryData = checkDataItem(dataMemory.pHeader, isAcceptable, context, type, name, subErrorCode, pErrorCode);
            if (pEntryData != NULL) {
                /* Data is good.
                *  Hand off ownership of the backing memory to the user's UDataMemory.
                *  and return it.   */
                pEntryData->mapAddr = dataMemory.mapAddr;
                pEntryData->map     = dataMemory.map;

#ifdef UDATA_DEBUG
                fprintf(stderr, "** Mapped file: %s\n", pathBuffer);
#endif
                return pEntryData;
            }

            /* the data is not acceptable, or some error occured.  Either way, unmap the memory */
            udata_close(&dataMemory);

            /* If we had a nasty error, bail out completely.  */
            if (U_FAILURE(*pErrorCode)) {
                return NULL;
            }

            /* Otherwise remember that we found data but didn't like it for some reason  */
            *subErrorCode=U_INVALID_FORMAT_ERROR;
        }
#ifdef UDATA_DEBUG
        fprintf(stderr, "%s\n", UDataMemory_isLoaded(&dataMemory)?"LOADED":"not loaded");
#endif
    }
    return NULL;
}

/**
 * @return 0 if not loaded, 1 if loaded or err 
 */
static UDataMemory *doLoadFromCommonData(UBool isICUData, const char * /*pkgName*/, 
        const char * /*dataPath*/, const char * /*tocEntryPathSuffix*/, const char *tocEntryName,
            /* following arguments are the same as doOpenChoice itself */
            const char *path, const char *type, const char *name,
             UDataMemoryIsAcceptable *isAcceptable, void *context,
             UErrorCode *subErrorCode,
             UErrorCode *pErrorCode)
{
    UDataMemory        *pEntryData;
    const DataHeader   *pHeader;
    UDataMemory        *pCommonData;
    int32_t            commonDataIndex;
    UBool              checkedExtendedICUData = FALSE;
    /* try to get common data.  The loop is for platforms such as the 390 that do
     *  not initially load the full set of ICU data.  If the lookup of an ICU data item
     *  fails, the full (but slower to load) set is loaded, the and the loop repeats,
     *  trying the lookup again.  Once the full set of ICU data is loaded, the loop wont
     *  repeat because the full set will be checked the first time through.
     *
     *  The loop also handles the fallback to a .dat file if the application linked
     *   to the stub data library rather than a real library.
     */
    for (commonDataIndex = isICUData ? 0 : -1;;) {
        pCommonData=openCommonData(path, commonDataIndex, subErrorCode); /** search for pkg **/

        if(U_SUCCESS(*subErrorCode) && pCommonData!=NULL) {
            int32_t length;

            /* look up the data piece in the common data */
            pHeader=pCommonData->vFuncs->Lookup(pCommonData, tocEntryName, &length, subErrorCode);
#ifdef UDATA_DEBUG
            fprintf(stderr, "%s: pHeader=%p - %s\n", tocEntryName, pHeader, u_errorName(*subErrorCode));
#endif

            if(pHeader!=NULL) {
                pEntryData = checkDataItem(pHeader, isAcceptable, context, type, name, subErrorCode, pErrorCode);
#ifdef UDATA_DEBUG
                fprintf(stderr, "pEntryData=%p\n", pEntryData);
#endif
                if (U_FAILURE(*pErrorCode)) {
                    return NULL;
                }
                if (pEntryData != NULL) {
                    pEntryData->length = length;
                    return pEntryData;
                }
            }
        }
        /* Data wasn't found.  If we were looking for an ICUData item and there is
         * more data available, load it and try again,
         * otherwise break out of this loop. */
        if (!isICUData) {
            return NULL;
        } else if (pCommonData != NULL) {
            ++commonDataIndex;  /* try the next data package */
        } else if ((!checkedExtendedICUData) && extendICUData(subErrorCode)) {
            checkedExtendedICUData = TRUE;
            /* try this data package slot again: it changed from NULL to non-NULL */
        } else {
            return NULL;
        }
    }
}

/*
 * Identify the Time Zone resources that are subject to special override data loading.
 */
static UBool isTimeZoneFile(const char *name, const char *type) {
    return ((uprv_strcmp(type, "res") == 0) &&
            (uprv_strcmp(name, "zoneinfo64") == 0 ||
             uprv_strcmp(name, "timezoneTypes") == 0 ||
             uprv_strcmp(name, "windowsZones") == 0 ||
             uprv_strcmp(name, "metaZones") == 0));
}

/*
 *  A note on the ownership of Mapped Memory
 *
 *  For common format files, ownership resides with the UDataMemory object
 *    that lives in the cache of opened common data.  These UDataMemorys are private
 *    to the udata implementation, and are never seen directly by users.
 *
 *    The UDataMemory objects returned to users will have the address of some desired
 *    data within the mapped region, but they wont have the mapping info itself, and thus
 *    won't cause anything to be removed from memory when they are closed.
 *
 *  For individual data files, the UDataMemory returned to the user holds the
 *  information necessary to unmap the data on close.  If the user independently
 *  opens the same data file twice, two completely independent mappings will be made.
 *  (There is no cache of opened data items from individual files, only a cache of
 *   opened Common Data files, that is, files containing a collection of data items.)
 *
 *  For common data passed in from the user via udata_setAppData() or
 *  udata_setCommonData(), ownership remains with the user.
 *
 *  UDataMemory objects themselves, as opposed to the memory they describe,
 *  can be anywhere - heap, stack/local or global.
 *  They have a flag to indicate when they're heap allocated and thus
 *  must be deleted when closed.
 */


/*----------------------------------------------------------------------------*
 *                                                                            *
 * main data loading functions                                                *
 *                                                                            *
 *----------------------------------------------------------------------------*/
static UDataMemory *
doOpenChoice(const char *path, const char *type, const char *name,
             UDataMemoryIsAcceptable *isAcceptable, void *context,
             UErrorCode *pErrorCode)
{
    UDataMemory         *retVal = NULL;

    const char         *dataPath;

    int32_t             tocEntrySuffixIndex;
    const char         *tocEntryPathSuffix;
    UErrorCode          subErrorCode=U_ZERO_ERROR;
    const char         *treeChar;

    UBool               isICUData = FALSE;


    /* Is this path ICU data? */
    if(path == NULL ||
       !strcmp(path, U_ICUDATA_ALIAS) ||  /* "ICUDATA" */
       !uprv_strncmp(path, U_ICUDATA_NAME U_TREE_SEPARATOR_STRING, /* "icudt26e-" */
                     uprv_strlen(U_ICUDATA_NAME U_TREE_SEPARATOR_STRING)) ||  
       !uprv_strncmp(path, U_ICUDATA_ALIAS U_TREE_SEPARATOR_STRING, /* "ICUDATA-" */
                     uprv_strlen(U_ICUDATA_ALIAS U_TREE_SEPARATOR_STRING))) {
      isICUData = TRUE;
    }

#if (U_FILE_SEP_CHAR != U_FILE_ALT_SEP_CHAR)  /* Windows:  try "foo\bar" and "foo/bar" */
    /* remap from alternate path char to the main one */
    CharString altSepPath;
    if(path) {
        if(uprv_strchr(path,U_FILE_ALT_SEP_CHAR) != NULL) {
            altSepPath.append(path, *pErrorCode);
            char *p;
            while((p=uprv_strchr(altSepPath.data(), U_FILE_ALT_SEP_CHAR))) {
                *p = U_FILE_SEP_CHAR;
            }
#if defined (UDATA_DEBUG)
            fprintf(stderr, "Changed path from [%s] to [%s]\n", path, altSepPath.s);
#endif
            path = altSepPath.data();
        }
    }
#endif

    CharString tocEntryName; /* entry name in tree format. ex:  'icudt28b/coll/ar.res' */
    CharString tocEntryPath; /* entry name in path format. ex:  'icudt28b\\coll\\ar.res' */

    CharString pkgName;
    CharString treeName;

    /* ======= Set up strings */
    if(path==NULL) {
        pkgName.append(U_ICUDATA_NAME, *pErrorCode);
    } else {
        const char *pkg;
        const char *first;
        pkg = uprv_strrchr(path, U_FILE_SEP_CHAR);
        first = uprv_strchr(path, U_FILE_SEP_CHAR);
        if(uprv_pathIsAbsolute(path) || (pkg != first)) { /* more than one slash in the path- not a tree name */
            /* see if this is an /absolute/path/to/package  path */
            if(pkg) {
                pkgName.append(pkg+1, *pErrorCode);
            } else {
                pkgName.append(path, *pErrorCode);
            }
        } else {
            treeChar = uprv_strchr(path, U_TREE_SEPARATOR);
            if(treeChar) { 
                treeName.append(treeChar+1, *pErrorCode); /* following '-' */
                if(isICUData) {
                    pkgName.append(U_ICUDATA_NAME, *pErrorCode);
                } else {
                    pkgName.append(path, (int32_t)(treeChar-path), *pErrorCode);
                    if (first == NULL) {
                        /*
                        This user data has no path, but there is a tree name.
                        Look up the correct path from the data cache later.
                        */
                        path = pkgName.data();
                    }
                }
            } else {
                if(isICUData) {
                    pkgName.append(U_ICUDATA_NAME, *pErrorCode);
                } else {
                    pkgName.append(path, *pErrorCode);
                }
            }
        }
    }

#ifdef UDATA_DEBUG
    fprintf(stderr, " P=%s T=%s\n", pkgName.data(), treeName.data());
#endif

    /* setting up the entry name and file name 
     * Make up a full name by appending the type to the supplied
     *  name, assuming that a type was supplied.
     */

    /* prepend the package */
    tocEntryName.append(pkgName, *pErrorCode);
    tocEntryPath.append(pkgName, *pErrorCode);
    tocEntrySuffixIndex = tocEntryName.length();

    if(!treeName.isEmpty()) {
        tocEntryName.append(U_TREE_ENTRY_SEP_CHAR, *pErrorCode).append(treeName, *pErrorCode);
        tocEntryPath.append(U_FILE_SEP_CHAR, *pErrorCode).append(treeName, *pErrorCode);
    }

    tocEntryName.append(U_TREE_ENTRY_SEP_CHAR, *pErrorCode).append(name, *pErrorCode);
    tocEntryPath.append(U_FILE_SEP_CHAR, *pErrorCode).append(name, *pErrorCode);
    if(type!=NULL && *type!=0) {
        tocEntryName.append(".", *pErrorCode).append(type, *pErrorCode);
        tocEntryPath.append(".", *pErrorCode).append(type, *pErrorCode);
    }
    tocEntryPathSuffix = tocEntryPath.data()+tocEntrySuffixIndex; /* suffix starts here */

#ifdef UDATA_DEBUG
    fprintf(stderr, " tocEntryName = %s\n", tocEntryName.data());
    fprintf(stderr, " tocEntryPath = %s\n", tocEntryName.data());
#endif

    if(path == NULL) {
        path = COMMON_DATA_NAME; /* "icudt26e" */
    }

    /************************ Begin loop looking for ind. files ***************/
#ifdef UDATA_DEBUG
    fprintf(stderr, "IND: inBasename = %s, pkg=%s\n", "(n/a)", packageNameFromPath(path));
#endif

    /* End of dealing with a null basename */
    dataPath = u_getDataDirectory();

    /****    Time zone individual files override  */
    if (isTimeZoneFile(name, type) && isICUData) {
        const char *tzFilesDir = u_getTimeZoneFilesDirectory(pErrorCode);
        if (tzFilesDir[0] != 0) {
#ifdef UDATA_DEBUG
            fprintf(stderr, "Trying Time Zone Files directory = %s\n", tzFilesDir);
#endif
            retVal = doLoadFromIndividualFiles(/* pkgName.data() */ "", tzFilesDir, tocEntryPathSuffix,
                            /* path */ "", type, name, isAcceptable, context, &subErrorCode, pErrorCode);
            if((retVal != NULL) || U_FAILURE(*pErrorCode)) {
                return retVal;
            }
        }
    }

    /****    COMMON PACKAGE  - only if packages are first. */
    if(gDataFileAccess == UDATA_PACKAGES_FIRST) {
#ifdef UDATA_DEBUG
        fprintf(stderr, "Trying packages (UDATA_PACKAGES_FIRST)\n");
#endif
        /* #2 */
        retVal = doLoadFromCommonData(isICUData, 
                            pkgName.data(), dataPath, tocEntryPathSuffix, tocEntryName.data(),
                            path, type, name, isAcceptable, context, &subErrorCode, pErrorCode);
        if((retVal != NULL) || U_FAILURE(*pErrorCode)) {
            return retVal;
        }
    }

    /****    INDIVIDUAL FILES  */
    if((gDataFileAccess==UDATA_PACKAGES_FIRST) ||
       (gDataFileAccess==UDATA_FILES_FIRST)) {
#ifdef UDATA_DEBUG
        fprintf(stderr, "Trying individual files\n");
#endif
        /* Check to make sure that there is a dataPath to iterate over */
        if ((dataPath && *dataPath) || !isICUData) {
            retVal = doLoadFromIndividualFiles(pkgName.data(), dataPath, tocEntryPathSuffix,
                            path, type, name, isAcceptable, context, &subErrorCode, pErrorCode);
            if((retVal != NULL) || U_FAILURE(*pErrorCode)) {
                return retVal;
            }
        }
    }

    /****    COMMON PACKAGE  */
    if((gDataFileAccess==UDATA_ONLY_PACKAGES) || 
       (gDataFileAccess==UDATA_FILES_FIRST)) {
#ifdef UDATA_DEBUG
        fprintf(stderr, "Trying packages (UDATA_ONLY_PACKAGES || UDATA_FILES_FIRST)\n");
#endif
        retVal = doLoadFromCommonData(isICUData,
                            pkgName.data(), dataPath, tocEntryPathSuffix, tocEntryName.data(),
                            path, type, name, isAcceptable, context, &subErrorCode, pErrorCode);
        if((retVal != NULL) || U_FAILURE(*pErrorCode)) {
            return retVal;
        }
    }
    
    /* Load from DLL.  If we haven't attempted package load, we also haven't had any chance to
        try a DLL (static or setCommonData/etc)  load.
         If we ever have a "UDATA_ONLY_FILES", add it to the or list here.  */  
    if(gDataFileAccess==UDATA_NO_FILES) {
#ifdef UDATA_DEBUG
        fprintf(stderr, "Trying common data (UDATA_NO_FILES)\n");
#endif
        retVal = doLoadFromCommonData(isICUData,
                            pkgName.data(), "", tocEntryPathSuffix, tocEntryName.data(),
                            path, type, name, isAcceptable, context, &subErrorCode, pErrorCode);
        if((retVal != NULL) || U_FAILURE(*pErrorCode)) {
            return retVal;
        }
    }

    /* data not found */
    if(U_SUCCESS(*pErrorCode)) {
        if(U_SUCCESS(subErrorCode)) {
            /* file not found */
            *pErrorCode=U_FILE_ACCESS_ERROR;
        } else {
            /* entry point not found or rejected */
            *pErrorCode=subErrorCode;
        }
    }
    return retVal;
}



/* API ---------------------------------------------------------------------- */

U_CAPI UDataMemory * U_EXPORT2
udata_open(const char *path, const char *type, const char *name,
           UErrorCode *pErrorCode) {
#ifdef UDATA_DEBUG
  fprintf(stderr, "udata_open(): Opening: %s : %s . %s\n", (path?path:"NULL"), name, type);
    fflush(stderr);
#endif

    if(pErrorCode==NULL || U_FAILURE(*pErrorCode)) {
        return NULL;
    } else if(name==NULL || *name==0) {
        *pErrorCode=U_ILLEGAL_ARGUMENT_ERROR;
        return NULL;
    } else {
        return doOpenChoice(path, type, name, NULL, NULL, pErrorCode);
    }
}



U_CAPI UDataMemory * U_EXPORT2
udata_openChoice(const char *path, const char *type, const char *name,
                 UDataMemoryIsAcceptable *isAcceptable, void *context,
                 UErrorCode *pErrorCode) {
#ifdef UDATA_DEBUG
  fprintf(stderr, "udata_openChoice(): Opening: %s : %s . %s\n", (path?path:"NULL"), name, type);
#endif

    if(pErrorCode==NULL || U_FAILURE(*pErrorCode)) {
        return NULL;
    } else if(name==NULL || *name==0 || isAcceptable==NULL) {
        *pErrorCode=U_ILLEGAL_ARGUMENT_ERROR;
        return NULL;
    } else {
        return doOpenChoice(path, type, name, isAcceptable, context, pErrorCode);
    }
}



U_CAPI void U_EXPORT2
udata_getInfo(UDataMemory *pData, UDataInfo *pInfo) {
    if(pInfo!=NULL) {
        if(pData!=NULL && pData->pHeader!=NULL) {
            const UDataInfo *info=&pData->pHeader->info;
            uint16_t dataInfoSize=udata_getInfoSize(info);
            if(pInfo->size>dataInfoSize) {
                pInfo->size=dataInfoSize;
            }
            uprv_memcpy((uint16_t *)pInfo+1, (const uint16_t *)info+1, pInfo->size-2);
            if(info->isBigEndian!=U_IS_BIG_ENDIAN) {
                /* opposite endianness */
                uint16_t x=info->reservedWord;
                pInfo->reservedWord=(uint16_t)((x<<8)|(x>>8));
            }
        } else {
            pInfo->size=0;
        }
    }
}


U_CAPI void U_EXPORT2 udata_setFileAccess(UDataFileAccess access, UErrorCode * /*status*/)
{
    // Note: this function is documented as not thread safe.
    gDataFileAccess = access;
}

/*----------------------------------------------------------------------------*
 *                                                                            *
 * udictdata - move somewhere else                                            *
 *                                                                            *
 *----------------------------------------------------------------------------*/
/* ICU package data file format (.dat files) ------------------------------- ***

Description of the data format after the usual ICU data file header
(UDataInfo etc.).

Format version 1

A .dat package file contains a simple Table of Contents of item names,
followed by the items themselves:

1. ToC table

uint32_t count; - number of items
UDataOffsetTOCEntry entry[count]; - pair of uint32_t values per item:
    uint32_t nameOffset; - offset of the item name
    uint32_t dataOffset; - offset of the item data
both are byte offsets from the beginning of the data

2. item name strings

All item names are stored as char * strings in one block between the ToC table
and the data items.

3. data items

The data items are stored following the item names block.
Each data item is 16-aligned.
The data items are stored in the sorted order of their names.

Therefore, the top of the name strings block is the offset of the first item,
the length of the last item is the difference between its offset and
the .dat file length, and the length of all previous items is the difference
between its offset and the next one.

----------------------------------------------------------------------------- */

struct UNewDataMemory {
    char *file;
    uint16_t headerSize;
    uint8_t magic1, magic2;
};

/* UDataInfo cf. udata.h */
static const UDataInfo dataInfo={
    sizeof(UDataInfo),
    0,

    U_IS_BIG_ENDIAN,
    U_CHARSET_FAMILY,
    sizeof(UChar),
    0,

    {0x43, 0x6d, 0x6e, 0x44},     /* dataFormat="CmnD" */
    {1, 0, 0, 0},                 /* formatVersion */
    {3, 0, 0, 0}                  /* dataVersion */
};

static const UDataInfo dictInfo={
    sizeof(UDataInfo),
    0,

    U_IS_BIG_ENDIAN,
    U_CHARSET_FAMILY,
    sizeof(UChar),
    0,

    {0x44, 0x69, 0x63, 0x74},     /* dataFormat="Dict" */
    {1, 0, 0, 0},                 /* formatVersion */
    {3, 0, 0, 0}                  /* dataVersion */
};

class DictData {
private:
    BytesTrieBuilder *bt;
    UCharsTrieBuilder *ut;
    UChar32 transformConstant;
    int32_t transformType;
public:
    DictData(UBool isBytesTrie, UErrorCode &status);    
    ~DictData();
private:
    char transform(UChar32 c, UErrorCode &status);
    void transform(const UnicodeString &word, CharString &buf, UErrorCode &errorCode);
public:
    void setTransform(const char *t);
    void addWord(const UnicodeString &word, int32_t value, UErrorCode &status); 
    StringPiece serializeBytes(UErrorCode &status);
    void serializeUChars(UnicodeString &s, UErrorCode &status); 
    int32_t getTransform();
    char *generateDat(const char *basename, int32_t &outDatFileSize); 
};

// constructs a new data dictionary. if there is an error, 
// it will be returned in status
// isBytesTrie != 0 will produce a BytesTrieBuilder,
// isBytesTrie == 0 will produce a UCharsTrieBuilder
DictData::DictData(UBool isBytesTrie, UErrorCode &status) : bt(NULL), ut(NULL), 
    transformConstant(0), transformType(DictionaryData::TRANSFORM_NONE) {
    if (isBytesTrie) {
        bt = new BytesTrieBuilder(status);
    } else {
        ut = new UCharsTrieBuilder(status);
    }
}

DictData::~DictData() {
    delete bt;
    delete ut;
}

char DictData::transform(UChar32 c, UErrorCode &status) {
    if (transformType == DictionaryData::TRANSFORM_TYPE_OFFSET) {
        if (c == 0x200D) { return (char)0xFF; }
        else if (c == 0x200C) { return (char)0xFE; }
        int32_t delta = c - transformConstant;
        if (delta < 0 || 0xFD < delta) {
#ifdef UDATA_DEBUG
            fprintf(stderr, "Codepoint U+%04lx out of range for --transform offset-%04lx!\n",
                    (long)c, (long)transformConstant);
#endif
            exit(U_ILLEGAL_ARGUMENT_ERROR); // TODO: should return and print the line number
        }
        return (char)delta;
    } else { // no such transform type 
        status = U_INTERNAL_PROGRAM_ERROR;
        return (char)c; // it should be noted this transform type will not generally work
    }
}

void DictData::transform(const UnicodeString &word, CharString &buf, UErrorCode &errorCode) {
    UChar32 c = 0;
    int32_t len = word.length();
    for (int32_t i = 0; i < len; i += U16_LENGTH(c)) {
        c = word.char32At(i);
        buf.append(transform(c, errorCode), errorCode);
    }
}

// sets the desired transformation data.
// should be populated from a command line argument
// so far the only acceptable format is offset-<hex constant>
// eventually others (mask-<hex constant>?) may be enabled
// more complex functions may be more difficult
void DictData::setTransform(const char *t) {
    if (strncmp(t, "offset-", 7) == 0) {
        char *end;
        unsigned long base = uprv_strtoul(t + 7, &end, 16);
        if (end == (t + 7) || *end != 0 || base > 0x10FF80) {
#ifdef UDATA_DEBUG
            fprintf(stderr, "Syntax for offset value in --transform offset-%s invalid!\n", t + 7);
#endif
//                usageAndDie(U_ILLEGAL_ARGUMENT_ERROR);
        }
        transformType = DictionaryData::TRANSFORM_TYPE_OFFSET;
        transformConstant = (UChar32)base;
    }
    else {
#ifdef UDATA_DEBUG
      fprintf(stderr, "Invalid transform specified: %s\n", t);
#endif
//            usageAndDie(U_ILLEGAL_ARGUMENT_ERROR);
    }
}

// add a word to the trie
void DictData::addWord(const UnicodeString &word, int32_t value, UErrorCode &status) {
    if (bt) {
        CharString buf;
        transform(word, buf, status);
        bt->add(buf.toStringPiece(), value, status);
    }
    if (ut) { ut->add(word, value, status); }
}

// if we are a bytestrie, give back the StringPiece representing the serialized version of us
StringPiece DictData::serializeBytes(UErrorCode &status) {
    return bt->buildStringPiece(USTRINGTRIE_BUILD_SMALL, status);
}

// if we are a ucharstrie, produce the UnicodeString representing the serialized version of us
void DictData::serializeUChars(UnicodeString &s, UErrorCode &status) {
    ut->buildUnicodeString(USTRINGTRIE_BUILD_SMALL, s, status);
}

int32_t DictData::getTransform() {
    return (int32_t)(transformType | transformConstant); 
}

char *DictData::generateDat(const char *basename, int32_t &outDatFileSize)
{
    UErrorCode status = U_ZERO_ERROR;

    // Memory to hold our dict "file"
    UNewDataMemory pDict;

    // Header
    uint16_t headerSize = (uint16_t)(dictInfo.size + 4);

    /* write the size of the header, take padding into account */
    pDict.headerSize=(uint16_t)((headerSize+15)&~0xf);
    pDict.magic1=0xda;
    pDict.magic2=0x27;

    UBool hasValues = true;

    int32_t outDictSize;
    const void *outDict;
    UnicodeString usp;  
    serializeUChars(usp, status);
    outDictSize = usp.length() * U_SIZEOF_UCHAR;    
    outDict = usp.getBuffer();
    
    // Indexes
    int32_t indexes[DictionaryData::IX_COUNT] = {
        DictionaryData::IX_COUNT * sizeof(int32_t), 0, 0, 0, 0, 0, 0, 0
    };
    int32_t size = outDictSize + indexes[DictionaryData::IX_STRING_TRIE_OFFSET];
    indexes[DictionaryData::IX_RESERVED1_OFFSET] = size;
    indexes[DictionaryData::IX_RESERVED2_OFFSET] = size;
    indexes[DictionaryData::IX_TOTAL_SIZE] = size;
    indexes[DictionaryData::IX_TRIE_TYPE] = bt ? DictionaryData::TRIE_TYPE_BYTES : DictionaryData::TRIE_TYPE_UCHARS;
    if (hasValues) {
        indexes[DictionaryData::IX_TRIE_TYPE] |= DictionaryData::TRIE_HAS_VALUES;
    }
    indexes[DictionaryData::IX_TRANSFORM] = getTransform();

    // Put it together  
    uint32_t fileSize = pDict.headerSize + sizeof(indexes) + outDictSize;
    pDict.file = (char *)uprv_malloc(fileSize);

    memcpy(pDict.file, &pDict.headerSize, 2);
    uint32_t offset = 2;
    memcpy(pDict.file + offset, &pDict.magic1, 1);
    offset += 1;
    memcpy(pDict.file + offset, &pDict.magic2, 1);
    offset += 1;
    memcpy(pDict.file + offset, &dictInfo, dictInfo.size);
    offset += dictInfo.size;

    /* write padding bytes to align the data section to 16 bytes */
    uint8_t bytes[16];
    headerSize&=0xf;
    if(headerSize!=0) {
        headerSize=(uint16_t)(16-headerSize);
        uprv_memset(bytes, 0, headerSize);
        memcpy(pDict.file + offset, bytes, headerSize);
        offset += headerSize;
    }

    memcpy(pDict.file + offset, indexes, sizeof(indexes));
    offset += sizeof(indexes);
    memcpy(pDict.file + offset, outDict, outDictSize);
    offset += outDictSize;
    int32_t outDictFileSize = offset;

    // Memory to hold our dat "file"
    UNewDataMemory pData;

    // Header
    headerSize = (uint16_t)(dataInfo.size + 4);

    /* write the size of the header, take padding into account */
    pData.headerSize=(uint16_t)((headerSize+15)&~0xf);
    pData.magic1=0xda;
    pData.magic2=0x27;

    uint32_t fileCount = 1;
  uint32_t basenamelen = strlen(basename);
    uint32_t basenameTotal = basenamelen + 1;
    uint32_t basenameOffset = 4 + 8*fileCount;
    uint32_t fileOffset = (basenameOffset + (basenameTotal+15))&~0xf;   // rounds to nearest 16
    int32_t outDatSize = pData.headerSize + fileOffset + outDictFileSize;
    outDatFileSize = (outDatSize+15) & ~0xf;

    pData.file = (char *)uprv_malloc(outDatFileSize);

    memcpy(pData.file, &pData.headerSize, 2);
    offset = 2;
    memcpy(pData.file + offset, &pData.magic1, 1);
    offset += 1;
    memcpy(pData.file + offset, &pData.magic2, 1);
    offset += 1;
    memcpy(pData.file + offset, &dataInfo, dataInfo.size);
    offset += dataInfo.size;

    /* write padding bytes to align the data section to 16 bytes */
    headerSize&=0xf;
    if(headerSize!=0) {
        headerSize=(uint16_t)(16-headerSize);
        uprv_memset(bytes, 0, headerSize);
        memcpy(pData.file + offset, bytes, headerSize);
        offset += headerSize;
    }
    memcpy(pData.file + offset, &fileCount, 4);
    offset += 4;
    memcpy(pData.file + offset, &basenameOffset, 4);
    offset += 4;
    memcpy(pData.file + offset, &fileOffset, 4);
    offset += 4;
    memcpy(pData.file + offset, basename, basenamelen + 1);
    offset += basenamelen + 1;
    
    headerSize = basenameOffset + basenameTotal;
    uint16_t padSize = fileOffset - headerSize;

    if (padSize != 0)
    {
        uprv_memset(bytes, 0xaa, padSize);
        memcpy(pData.file + offset, bytes, padSize);
        offset += padSize;
    }

    memcpy(pData.file + offset, pDict.file, fileSize);
    offset += fileSize;
    free(pDict.file);

    padSize = outDatFileSize - outDatSize;
    if (padSize != 0)
    {
        uprv_memset(bytes, 0xaa, padSize);
        memcpy(pData.file + offset, bytes, padSize);
        offset += padSize;
    }

    return pData.file;
}

static DictData *cjdict;
static char *cjdictDat;

U_CAPI void U_EXPORT2
cjdict_reset(UErrorCode *err)
{
  if (cjdict != NULL)
  {
    delete cjdict;
    cjdict = NULL;
  }
  
  cjdict_swap(err);
}

U_CAPI void U_EXPORT2
cjdict_addWord(const char *word, int32_t value, UErrorCode *err)
{
  if (cjdict == NULL)
  {
    cjdict = new DictData(FALSE, *err);
    if (*err != U_ZERO_ERROR) return;
  }

  UnicodeString uword(word);
  cjdict->addWord(uword, value, *err);  
}

U_CAPI void U_EXPORT2
cjdict_swap(UErrorCode *err)
{
  if (cjdict == NULL)
  {
    cjdict = new DictData(FALSE, *err);
    if (*err != U_ZERO_ERROR) return;
  }

  int32_t fileSize;
  char *newDat = cjdict->generateDat("icudt57l/brkitr/cjdict.dict", fileSize);
  udata_swapCommonData(newDat, cjdictDat, err);
  if (*err != U_ZERO_ERROR) return;

  rbbi_resetFactories();
  uprv_free(cjdictDat);
  cjdictDat = newDat;
}
