/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * Portions Copyright 2007-2009 Apple Inc.
 */

/*
 *	autod_readdir.c
 */

#pragma ident	"@(#)autod_readdir.c	1.23	05/06/08 SMI"

#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/param.h>
#include <errno.h>
#include <pwd.h>
#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>
#include "automount.h"
#include "automountd.h"

static void build_dir_entry_list(struct rddir_cache *rdcp,
				struct dir_entry *list);
static int rddir_cache_enter(char *map, uint_t bucket_size,
				struct rddir_cache **rdcpp);
int rddir_cache_lookup(char *map, struct rddir_cache **rdcpp);
static int rddir_cache_delete(struct rddir_cache *rdcp);
static int create_dirents(struct rddir_cache *rdcp, off_t offset,
				uint32_t rda_count,
				off_t *rddir_offset,
				boolean_t *rddir_eof,
				byte_buffer *rddir_entries,
				mach_msg_type_number_t *rddir_entriesCnt);
struct dir_entry *rddir_entry_lookup(char *name, struct dir_entry *list);
static void free_offset_tbl(struct off_tbl *head);
static void free_dir_list(struct dir_entry *head);

#define	OFFSET_BUCKET_SIZE	100

pthread_rwlock_t rddir_cache_lock;		/* readdir cache lock */
struct rddir_cache *rddir_head;		/* readdir cache head */

int
do_readdir(autofs_pathname rda_map, off_t rda_offset,
    uint32_t rda_count, off_t *rddir_offset,
    boolean_t *rddir_eof, byte_buffer *rddir_entries,
    mach_msg_type_number_t *rddir_entriesCnt)
{
	struct dir_entry *list = NULL, *l;
	struct rddir_cache *rdcp = NULL;
	int error;
	int cache_time = RDDIR_CACHE_TIME;

	if (automountd_nobrowse) {
		/*
		 * Browsability was disabled return an empty list.
		 */
		rddir_entriesCnt = 0;
		*rddir_eof = TRUE;
		*rddir_entries = NULL;

		return (0);
	}

	pthread_rwlock_rdlock(&rddir_cache_lock);
	error = rddir_cache_lookup(rda_map, &rdcp);
	if (error) {
		pthread_rwlock_unlock(&rddir_cache_lock);
		pthread_rwlock_wrlock(&rddir_cache_lock);
		error = rddir_cache_lookup(rda_map, &rdcp);
		if (error) {
			if (trace > 2)
				trace_prt(1,
				"map %s not found, adding...\n", rda_map);
			/*
			 * entry doesn't exist, add it.
			 */
			error = rddir_cache_enter(rda_map,
					OFFSET_BUCKET_SIZE, &rdcp);
		}
	}
	pthread_rwlock_unlock(&rddir_cache_lock);

	if (error)
		return (error);

	assert(rdcp != NULL);
	assert(rdcp->in_use);

	if (!rdcp->full) {
		pthread_rwlock_wrlock(&rdcp->rwlock);
		if (!rdcp->full) {
			/*
			 * cache entry hasn't been filled up, do it now.
			 */
			char *stack[STACKSIZ];
			char **stkptr;

			/*
			 * Initialize the stack of open files
			 * for this thread
			 */
			stack_op(INIT, NULL, stack, &stkptr);
			(void) getmapkeys(rda_map, &list, &error,
			    &cache_time, stack, &stkptr);
			if (!error)
				build_dir_entry_list(rdcp, list);
			else if (list) {
				free_dir_list(list);
				list = NULL;
			}
		}
	} else
		pthread_rwlock_rdlock(&rdcp->rwlock);

	if (!error) {
		error = create_dirents(rdcp, rda_offset, rda_count,
		    rddir_offset, rddir_eof, rddir_entries,
		    rddir_entriesCnt);
		if (error) {
			if (rdcp->offtp) {
				free_offset_tbl(rdcp->offtp);
				rdcp->offtp = NULL;
			}
			if (rdcp->entp) {
				free_dir_list(rdcp->entp);
				rdcp->entp = NULL;
			}
			rdcp->full = 0;
			list = NULL;
		}
	}

	if (trace > 2) {
		/*
		 * print this list only once
		 */
		for (l = list; l != NULL; l = l->next)
			trace_prt(0, "%s\n", l->name);
		trace_prt(0, "\n");
	}

	if (!error) {
		if (cache_time) {
			/*
			 * keep list of entries for up to
			 * 'cache_time' seconds
			 */
			rdcp->ttl = time((time_t *)NULL) + cache_time;
		} else {
			/*
			 * the underlying name service indicated not
			 * to cache contents.
			 */
			if (rdcp->offtp) {
				free_offset_tbl(rdcp->offtp);
				rdcp->offtp = NULL;
			}
			if (rdcp->entp) {
				free_dir_list(rdcp->entp);
				rdcp->entp = NULL;
			}
			rdcp->full = 0;
		}
	} else {
		/*
		 * return an empty list
		 */
		*rddir_entriesCnt = 0;
		*rddir_eof = TRUE;
		*rddir_entries = NULL;
	}
	pthread_rwlock_unlock(&rdcp->rwlock);

	pthread_mutex_lock(&rdcp->lock);
	rdcp->in_use--;
	pthread_mutex_unlock(&rdcp->lock);

	assert(rdcp->in_use >= 0);

	return (error);
}

#define	roundtoint(x)	(((x) + sizeof (int) - 1) & ~(sizeof (int) - 1))

static int
create_dirents(struct rddir_cache *rdcp, off_t offset, uint32_t rda_count,
    off_t *rddir_offset, boolean_t *rddir_eof, byte_buffer *rddir_entries,
    mach_msg_type_number_t *rddir_entriesCnt)
{
	uint_t total_bytes_wanted;
	size_t bufsize;
	size_t this_reclen;
	uint_t outcount = 0;
	int namelen;
	struct dir_entry *list = NULL, *l;
	kern_return_t ret;
	vm_address_t buffer_vm_address;
	struct dirent_nonext *dp;
	uint8_t *outbuf;
	struct off_tbl *offtp, *next = NULL;
	int this_bucket = 0;
	int error = 0;
	int x = 0, y = 0;

#if 0
	assert(RW_LOCK_HELD(&rdcp->rwlock));
#endif
	for (offtp = rdcp->offtp; offtp != NULL; offtp = next) {
		x++;
		next = offtp->next;
		this_bucket = (next == NULL);
		if (!this_bucket)
			this_bucket = (offset < next->offset);
		if (this_bucket) {
			/*
			 * has to be in this bucket
			 */
			assert(offset >= offtp->offset);
			list = offtp->first;
			break;
		}
		/*
		 * loop to look in next bucket
		 */
	}

	for (l = list; l != NULL && l->offset < offset; l = l->next)
		y++;

	if (l == NULL) {
		/*
		 * reached end of directory
		 */
		error = 0;
		goto empty;
	}

	if (trace > 2)
		trace_prt(1, "%s: offset searches (%d, %d)\n", rdcp->map, x, y);

	total_bytes_wanted = rda_count;
	bufsize = total_bytes_wanted + sizeof (struct dirent_nonext);
	ret = vm_allocate(current_task(), &buffer_vm_address,
	    bufsize, VM_FLAGS_ANYWHERE);
	if (ret != KERN_SUCCESS) {
		syslog(LOG_ERR, "memory allocation error: %s",
		    mach_error_string(ret));
		error = ENOMEM;
		goto empty;
	}
	outbuf = (uint8_t *)buffer_vm_address;
	memset(outbuf, 0, bufsize);
	/* LINTED pointer alignment */
	dp = (struct dirent_nonext *)outbuf;

	for (;;) {
		namelen = (int)strlen(l->name);
		this_reclen = DIRENT_RECLEN(namelen);
		if (outcount + this_reclen > total_bytes_wanted) {
			break;
		}

		/*
		 * XXX - 64-bit inumbers....
		 */
		dp->d_ino = (__uint32_t)l->nodeid;
		dp->d_reclen = this_reclen;
#if 0
		dp->d_type = DT_DIR;
#else
		dp->d_type = DT_UNKNOWN;
#endif
		dp->d_namlen = namelen;
		(void) strcpy(dp->d_name, l->name);
		outcount += dp->d_reclen;
		dp = (struct dirent_nonext *)((char *)dp + dp->d_reclen);
		assert(outcount <= total_bytes_wanted);
		if (!l->next)
			break;
		l = l->next;
	}

	/*
	 * "l" is the last element; make offset one plus that entry's
	 * offset.
	 */
	*rddir_offset = l->offset + 1;

	if (outcount > 0) {
		/*
		 * have some entries
		 */
		*rddir_entriesCnt = outcount;
		*rddir_eof = (l == NULL);
		*rddir_entries = outbuf;
		error = 0;
	} else {
		/*
		 * total_bytes_wanted is not large enough for one
		 * directory entry
		 */
		*rddir_entriesCnt = 0;
		*rddir_eof = FALSE;
		*rddir_entries = NULL;
		vm_deallocate(current_task(), buffer_vm_address, bufsize);
		syslog(LOG_ERR,
			"byte count in readdir too small for one directory entry");
		error = EIO;
	}
	return (error);

empty:	*rddir_entriesCnt = 0;
	*rddir_eof = TRUE;
	*rddir_entries = NULL;
	return (error);
}


/*
 * add new entry to cache for 'map'
 */
static int
rddir_cache_enter(char *map, uint_t bucket_size, struct rddir_cache **rdcpp)
{
	struct rddir_cache *p;
#if 0
	assert(RW_LOCK_HELD(&rddir_cache_lock));
#endif

	/*
	 * Add to front of the list at this time
	 */
	p = (struct rddir_cache *)malloc(sizeof (*p));
	if (p == NULL) {
		syslog(LOG_ERR,
			"rddir_cache_enter: memory allocation failed\n");
		return (ENOMEM);
	}
	memset((char *)p, 0, sizeof (*p));

	p->map = malloc(strlen(map) + 1);
	if (p->map == NULL) {
		syslog(LOG_ERR,
			"rddir_cache_enter: memory allocation failed\n");
		free(p);
		return (ENOMEM);
	}
	strcpy(p->map, map);

	p->bucket_size = bucket_size;
	/*
	 * no need to grab mutex lock since I haven't yet made the
	 * node visible to the list
	 */
	p->in_use = 1;
	(void) pthread_rwlock_init(&p->rwlock, NULL);
	(void) pthread_mutex_init(&p->lock, NULL);

	if (rddir_head == NULL)
		rddir_head = p;
	else {
		p->next = rddir_head;
		rddir_head = p;
	}
	*rdcpp = p;

	return (0);
}

/*
 * find 'map' in readdir cache
 */
int
rddir_cache_lookup(char *map, struct rddir_cache **rdcpp)
{
	struct rddir_cache *p;

#if 0
	assert(RW_LOCK_HELD(&rddir_cache_lock));
#endif
	for (p = rddir_head; p != NULL; p = p->next) {
		if (strcmp(p->map, map) == 0) {
			/*
			 * found matching entry
			 */
			*rdcpp = p;
			pthread_mutex_lock(&p->lock);
			p->in_use++;
			pthread_mutex_unlock(&p->lock);
			return (0);
		}
	}
	/*
	 * didn't find entry
	 */
	return (ENOENT);
}

/*
 * free the offset table
 */
static void
free_offset_tbl(struct off_tbl *head)
{
	struct off_tbl *p, *next = NULL;

	for (p = head; p != NULL; p = next) {
		next = p->next;
		free(p);
	}
}

/*
 * free the directory entries
 */
static void
free_dir_list(struct dir_entry *head)
{
	struct dir_entry *p, *next = NULL;

	for (p = head; p != NULL; p = next) {
		next = p->next;
		assert(p->name);
		free(p->name);
		free(p);
	}
}

static void
rddir_cache_entry_free(struct rddir_cache *p)
{
#if 0
	assert(RW_LOCK_HELD(&rddir_cache_lock));
#endif
	assert(!p->in_use);
	if (p->map)
		free(p->map);
	if (p->offtp)
		free_offset_tbl(p->offtp);
	if (p->entp)
		free_dir_list(p->entp);
	free(p);
}

/*
 * Remove entry from the rddircache
 * the caller must own the rddir_cache_lock.
 */
static int
rddir_cache_delete(struct rddir_cache *rdcp)
{
	struct rddir_cache *p, *prev;

#if 0
	assert(RW_LOCK_HELD(&rddir_cache_lock));
#endif
	/*
	 * Search cache for entry
	 */
	prev = NULL;
	for (p = rddir_head; p != NULL; p = p->next) {
		if (p == rdcp) {
			/*
			 * entry found, remove from list if not in use
			 */
			if (p->in_use)
				return (EBUSY);
			if (prev)
				prev->next = p->next;
			else
				rddir_head = p->next;
			rddir_cache_entry_free(p);
			return (0);
		}
		prev = p;
	}
	syslog(LOG_ERR, "Couldn't find entry %x in cache\n", p);
	return (ENOENT);
}

/*
 * Return entry that matches name, NULL otherwise.
 * Assumes the readers lock for this list has been grabed.
 */
struct dir_entry *
rddir_entry_lookup(char *name, struct dir_entry *list)
{
	return (btree_lookup(list, name));
}

static void
build_dir_entry_list(struct rddir_cache *rdcp, struct dir_entry *list)
{
	struct dir_entry *p;
	off_t offset = AUTOFS_DAEMONCOOKIE, offset_list = AUTOFS_DAEMONCOOKIE;
	struct off_tbl *offtp, *last = NULL;
	ino_t inonum = 4;

#if 0
	assert(RW_LOCK_HELD(&rdcp->rwlock));
#endif
	assert(rdcp->entp == NULL);
	rdcp->entp = list;
	for (p = list; p != NULL; p = p->next) {
		p->nodeid = inonum;
		p->offset = offset;
		if (offset >= offset_list) {
			/*
			 * add node to index table
			 */
			offtp = (struct off_tbl *)
				malloc(sizeof (struct off_tbl));
			if (offtp != NULL) {
				offtp->offset = offset;
				offtp->first = p;
				offtp->next = NULL;
				offset_list += rdcp->bucket_size;
			} else {
				syslog(LOG_ERR,
"WARNING: build_dir_entry_list: could not add offset to index table\n");
				continue;
			}
			/*
			 * add to cache
			 */
			if (rdcp->offtp == NULL)
				rdcp->offtp = offtp;
			else
				last->next = offtp;
			last = offtp;
		}
		offset++;
		inonum += 2;		/* use even numbers in daemon */
	}
	rdcp->full = 1;
}

pthread_mutex_t cleanup_lock;
pthread_cond_t cleanup_start_cv;
pthread_cond_t cleanup_done_cv;

/*
 * cache cleanup thread starting point
 */
void *
cache_cleanup(__unused void *unused)
{
	struct timespec abstime;
	struct rddir_cache *p, *next = NULL;
	int error;

	pthread_mutex_init(&cleanup_lock, NULL);
	pthread_cond_init(&cleanup_start_cv, NULL);
	pthread_cond_init(&cleanup_done_cv, NULL);

	pthread_mutex_lock(&cleanup_lock);
	for (;;) {
		/*
		 * delay RDDIR_CACHE_TIME seconds, or until some other thread
		 * requests that I cleanup the caches
		 */
		abstime.tv_sec = time(NULL) + RDDIR_CACHE_TIME/2;
		abstime.tv_nsec = 0;
		if ((error = pthread_cond_timedwait(
		    &cleanup_start_cv, &cleanup_lock, &abstime)) != 0) {
			if (error != ETIMEDOUT) {
				if (trace > 1)
					trace_prt(1,
					"cleanup thread wakeup (%d)\n", error);
				continue;
			}
		}
		pthread_mutex_unlock(&cleanup_lock);

		/*
		 * Perform the cache cleanup
		 */
		pthread_rwlock_wrlock(&rddir_cache_lock);
		for (p = rddir_head; p != NULL; p = next) {
			next = p->next;
			if (p->in_use > 0) {
				/*
				 * cache entry busy, skip it
				 */
				if (trace > 1) {
					trace_prt(1,
					"%s cache in use\n", p->map);
				}
				continue;
			}
			/*
			 * Cache entry is not in use, and nobody can grab a
			 * new reference since I'm holding the rddir_cache_lock
			 */

			/*
			 * error will be zero if some thread signaled us asking
			 * that the caches be freed. In such case, free caches
			 * even if they're still valid and nobody is referencing
			 * them at this time. Otherwise, free caches only
			 * if their time to live (ttl) has expired.
			 */
			if (error == ETIMEDOUT && (p->ttl > time((time_t *)NULL))) {
				/*
				 * Scheduled cache cleanup, if cache is still
				 * valid don't free.
				 */
				if (trace > 1) {
					trace_prt(1,
					"%s cache still valid\n", p->map);
				}
				continue;
			}
			if (trace > 1)
				trace_prt(1, "%s freeing cache\n", p->map);
			assert(!p->in_use);
			error = rddir_cache_delete(p);
			assert(!error);
		}
		pthread_rwlock_unlock(&rddir_cache_lock);

		/*
		 * Clean up the fstab cache.
		 */
		clean_fstab_cache(error == ETIMEDOUT);

		/*
		 * wakeup the thread/threads waiting for the
		 * cleanup to finish
		 */
		pthread_mutex_lock(&cleanup_lock);
		pthread_cond_broadcast(&cleanup_done_cv);
	}
	/* NOTREACHED */
	return NULL;
}
