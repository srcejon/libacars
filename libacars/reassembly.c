/*
 *  This file is a part of libacars
 *
 *  Copyright (c) 2018-2019 Tomasz Lemiech <szpajder@gmail.com>
 */

#include <sys/time.h>			// struct timeval
#include <string.h>			// strdup
#include <libacars/macros.h>		// la_assert
#include <libacars/hash.h>		// la_hash
#include <libacars/list.h>		// la_list
#include <libacars/vstring.h>		// la_vstring
#include <libacars/util.h>		// LA_XCALLOC, LA_XFREE
#include <libacars/reassembly.h>

typedef struct la_reasm_table_s {
	void const *key;		// a pointer identifying the protocol
					// owning this reasm_table (la_type_descriptor
					// can be used for this purpose). Due to small
					// number of protocols, hash would be an overkill
					// here.
	la_hash *fragment_table;	// keyed with packet identifiers, values are
					// la_reasm_table_entries
	la_reasm_table_funcs funcs;	// protocol-specific callbacks
	int cleanup_interval;		// expire old entries every cleanup_interval
					// number of added fragments
	int frag_cnt;			// counts added fragments (up to cleanup_interval)
} la_reasm_table;

struct la_reasm_ctx_s {
	la_list *rtables;		// list of reasm_tables, one per protocol
};

// the header of the fragment list
typedef struct {
	int last_seq_num;		// sequence number of last seen fragment
	int seq_num_wrap_count;		// number of sequence number wraparounds
	struct timeval first_frag_rx_time;	// time of arrival of the first fragment
	struct timeval reasm_timeout;	// reassembly timeout to be applied to this message
	la_list *fragment_list;		// payloads of all fragments gathered so far
} la_reasm_table_entry;

la_reasm_ctx *la_reasm_ctx_init() {
	LA_NEW(la_reasm_ctx, rctx);
	return rctx;
}

static void la_reasm_table_entry_destroy(void *rt_ptr) {
	if(rt_ptr == NULL) {
		return;
	}
	la_debug_print("table_entry_destroy\n");
	LA_CAST_PTR(rt_entry, la_reasm_table_entry *, rt_ptr);
	la_list_free(rt_entry->fragment_list);
	LA_XFREE(rt_entry);
}

static void la_reasm_table_destroy(void *table) {
	if(table == NULL) {
		return;
	}
	la_debug_print("table_destroy\n");
	LA_CAST_PTR(rtable, la_reasm_table *, table);
	la_hash_destroy(rtable->fragment_table);
	LA_XFREE(rtable);
}

void la_reasm_ctx_destroy(void *ctx) {
	if(ctx == NULL) {
		return;
	}
	la_debug_print("ctx_destroy\n");
	LA_CAST_PTR(rctx, la_reasm_ctx *, ctx);
	la_list_free_full(rctx->rtables, la_reasm_table_destroy);
	LA_XFREE(rctx);
}

la_reasm_table *la_reasm_table_lookup(la_reasm_ctx *rctx, void const *table_id) {
	la_assert(rctx != NULL);
	la_assert(table_id != NULL);

	la_list *l = rctx->rtables;
	while(l != NULL) {
		LA_CAST_PTR(rt, la_reasm_table *, l->data);
		if(rt->key == table_id) {
			return rt;
		}
	}
	return NULL;
}

#define LA_REASM_DEFAULT_CLEANUP_INTERVAL 100

la_reasm_table *la_reasm_table_init(la_reasm_ctx *rctx, void const *table_id,
la_reasm_table_funcs funcs, int const cleanup_interval) {
	la_assert(rctx != NULL);
	la_assert(table_id != NULL);
	la_assert(funcs.get_key);
	la_assert(funcs.get_tmp_key);
	la_assert(funcs.hash_key);
	la_assert(funcs.compare_keys);
	la_assert(funcs.destroy_key);

	la_reasm_table *rtable = la_reasm_table_lookup(rctx, table_id);
	if(rtable != NULL) {
		goto end;
	}
	rtable = LA_XCALLOC(1, sizeof(la_reasm_table));
	rtable->key = table_id;
	rtable->fragment_table = la_hash_new(funcs.hash_key, funcs.compare_keys,
		funcs.destroy_key, la_reasm_table_entry_destroy);
	rtable->funcs = funcs;

// Replace insane value with reasonable default
	rtable->cleanup_interval = cleanup_interval > 0 ?
		cleanup_interval : LA_REASM_DEFAULT_CLEANUP_INTERVAL;
	rctx->rtables = la_list_append(rctx->rtables, rtable);
end:
	return rtable;
}

// Checks if time difference between rx_first and rx_last is greater than timeout.
static bool la_reasm_timed_out(struct timeval const rx_last, struct timeval const rx_first,
struct timeval const timeout) {
	if(timeout.tv_sec == 0 && timeout.tv_usec == 0) {
		return false;
	}
	struct timeval to = {
		.tv_sec = rx_first.tv_sec + timeout.tv_sec,
		.tv_usec = rx_first.tv_usec + timeout.tv_usec
	};
	if(to.tv_usec > 1e9) {
		to.tv_sec++;
		to.tv_usec -= 1e9;
	}
	la_debug_print("rx_first: %lu.%lu to: %lu.%lu rx_last: %lu.%lu\n",
		rx_first.tv_sec, rx_first.tv_usec, to.tv_sec, to.tv_usec, rx_last.tv_sec, rx_last.tv_usec);
	return (rx_last.tv_sec > to.tv_sec ||
		(rx_last.tv_sec == to.tv_sec && rx_last.tv_usec > to.tv_usec));
}

// Callback for la_hash_foreach_remove used during reassembly table cleanups.
static bool is_rt_entry_expired(void const *keyptr, void const *valptr, void const *ctx) {
	LA_UNUSED(keyptr);
	la_assert(valptr != NULL);
	la_assert(ctx != NULL);

	LA_CAST_PTR(rt_entry, la_reasm_table_entry *, valptr);
	LA_CAST_PTR(now, struct timeval *, ctx);
	return la_reasm_timed_out(*now, rt_entry->first_frag_rx_time, rt_entry->reasm_timeout);
}

// Removes expired entries from the given reassembly table.
static void la_reasm_table_cleanup(la_reasm_table *rtable, struct timeval now) {
	la_assert(rtable != NULL);
	la_assert(rtable->fragment_table != NULL);
	la_debug_print("current time: %lu.%lu\n", now.tv_sec, now.tv_usec);
	int deleted_count = la_hash_foreach_remove(rtable->fragment_table,
		is_rt_entry_expired, &now);
	la_debug_print("Expired %d entries\n", deleted_count);
}

#define SEQ_UNINITIALIZED -2

// Core reassembly logic.
// Adds the given fragment to the reassembly table fragment list, provided that
// sequence number validation and expiration timer checks succeeded.
la_reasm_status la_reasm_fragment_add(la_reasm_table *rtable, la_reasm_fragment_info const *finfo) {
	la_assert(rtable != NULL);
	la_assert(finfo != NULL);
	if(finfo->msg_info == NULL) {
		return LA_REASM_ARGS_INVALID;
	}
// Don't allow zero timeout. This would prevent stale rt_entries from being expired,
// causing a massive memory leak.
	if(finfo->reasm_timeout.tv_sec == 0 && finfo->reasm_timeout.tv_usec == 0) {
		return LA_REASM_ARGS_INVALID;
	}
	la_reasm_status ret = LA_REASM_UNKNOWN;
	void *lookup_key = rtable->funcs.get_tmp_key(finfo->msg_info);
	la_assert(lookup_key != NULL);

	la_reasm_table_entry *rt_entry = la_hash_lookup(rtable->fragment_table, lookup_key);
	if(rt_entry == NULL) {
// Don't add if we know that this is not the first fragment of the message.
		if(finfo->seq_num_first != SEQ_FIRST_NONE && finfo->seq_num_first != finfo->seq_num) {
			la_debug_print("No rt_entry found and seq_num %d != seq_num_first %d,"
				" not creating rt_entry\n", finfo->seq_num, finfo->seq_num_first);
			ret = LA_REASM_FIRST_FRAG_MISSING;
			goto end;
		}
		if(finfo->last_fragment) {
// This is the first received fragment of this message and it's the final
// fragment.  Either this message is not fragmented or all fragments except the
// last one have been lost.  In either case there is no point in adding it to
// the fragment table.
			la_debug_print("No rt_entry found and last_fragment=true, not creating rt_entry\n");
			ret = LA_REASM_SKIPPED;
			goto end;
		}
		rt_entry = LA_XCALLOC(1, sizeof(la_reasm_table_entry));
		rt_entry->last_seq_num = SEQ_UNINITIALIZED;
		rt_entry->first_frag_rx_time = finfo->rx_time;
		rt_entry->reasm_timeout = finfo->reasm_timeout;
		la_debug_print("Adding new rt_table entry (rx_time: %lu.%lu timeout: %lu.%lu)\n",
			rt_entry->first_frag_rx_time.tv_sec, rt_entry->first_frag_rx_time.tv_usec,
			rt_entry->reasm_timeout.tv_sec, rt_entry->reasm_timeout.tv_usec);
		void *msg_key = rtable->funcs.get_key(finfo->msg_info);
		la_assert(msg_key != NULL);
		la_hash_insert(rtable->fragment_table, msg_key, rt_entry);
	} else {
		la_debug_print("rt_entry found, last_seq_num: %d\n", rt_entry->last_seq_num);
	}

// Check reassembly timeout
	if(la_reasm_timed_out(finfo->rx_time, rt_entry->first_frag_rx_time, rt_entry->reasm_timeout) == true) {
		la_debug_print("reasm timeout\n");
		la_hash_remove(rtable->fragment_table, lookup_key);
		ret = LA_REASM_TIMED_OUT;
		goto end;
	}

// Check if the sequence number has wrapped (if we're supposed to handle wraparounds)
	if(finfo->seq_num_wrap != SEQ_WRAP_NONE && finfo->seq_num == 0 &&
	finfo->seq_num_wrap == rt_entry->last_seq_num + 1) {
		la_debug_print("seq_num wrap at %d: %d -> %d\n", finfo->seq_num_wrap,
			rt_entry->last_seq_num, finfo->seq_num);
// Current seq_num is 0, so set last_seq_num to -1 to force the next condition to be true.
		rt_entry->last_seq_num = -1;
	}

// Check if the sequence number is correct.
	if(rt_entry->last_seq_num + 1 == finfo->seq_num || rt_entry->last_seq_num == SEQ_UNINITIALIZED) {
		la_debug_print("Good seq_num %d (last=%d), adding fragment to the list\n",
			finfo->seq_num, rt_entry->last_seq_num);
		rt_entry->fragment_list = la_list_append(rt_entry->fragment_list, strdup(finfo->msg_data));
		rt_entry->last_seq_num = finfo->seq_num;

// Update fragment counter; expire old entries if necessary.
// Expiration is performed in relation to rx_time of the fragment currently
// being processed. This allows processing historical data with timestamps in
// the past.
		if(rtable->cleanup_interval > 0 && ++rtable->frag_cnt > rtable->cleanup_interval) {
			la_reasm_table_cleanup(rtable, finfo->rx_time);
			rtable->frag_cnt = 0;
		}

// If we've come to this point successfully and finfo->last_fragment is set,
// then this message is now ready for reassembly. Otherwise it's not.
		ret = finfo->last_fragment ? LA_REASM_COMPLETE : LA_REASM_IN_PROGRESS;
	} else if(rt_entry->last_seq_num == finfo->seq_num) {
// Duplicate or retransmission
		la_debug_print("Skipping duplicate fragment (seq_num: %d)\n", finfo->seq_num);
		ret = LA_REASM_DUPLICATE;
		goto end;
	} else {
// Non-contiguous sequence number - probably some fragments have been lost.
		la_debug_print("seq_num %d out of sequence (last: %d)\n",
			finfo->seq_num, rt_entry->last_seq_num);
		la_hash_remove(rtable->fragment_table, lookup_key);
		ret = LA_REASM_FRAG_OUT_OF_SEQUENCE;
		goto end;
	}
end:
	la_debug_print("Result: %d\n", ret);
	LA_XFREE(lookup_key);
	return ret;
}

// Returns the reassembled payload and removes the packet data from reassembly table
char *la_reasm_payload_get(la_reasm_table *rtable, void const *msg_info) {
	la_assert(rtable != NULL);
	la_assert(msg_info != NULL);

	void *tmp_key = rtable->funcs.get_tmp_key(msg_info);
	la_assert(tmp_key);

	char *ret = NULL;
	la_reasm_table_entry *rt_entry = la_hash_lookup(rtable->fragment_table, tmp_key);
	if(rt_entry == NULL) {
		goto end;
	}
	la_debug_print("Found rt_entry for message, last_seq_num: %d\n", rt_entry->last_seq_num);
	la_vstring *vstr = la_vstring_new();
	la_list *l = rt_entry->fragment_list;
	while(l != NULL) {
		la_vstring_append_sprintf(vstr, "%s", (char *)l->data);
		l = la_list_next(l);
	}
	ret = vstr->str;
	la_vstring_destroy(vstr, false);
	la_hash_remove(rtable->fragment_table, tmp_key);
end:
	LA_XFREE(tmp_key);
	return ret;
}

