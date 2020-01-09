/*
 * ldns-zone-digest
 *
 * ldns-zone-digest is a proof-of-concept implementation of
 * draft-wessels-dns-zone-digest, utilizing the ldns library.  That
 * Internet Draft describes how to compute, sign, and validate a
 * message digest covering a DNS zone File.
 *
 * Copyright (c) 2018, Verisign, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of the copyright holder nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <err.h>
#include <ldns/ldns.h>
#include <assert.h>
#include <getopt.h>
#include <openssl/evp.h>
#include <sys/time.h>
#include <sys/resource.h>

int quiet = 0;

static ldns_rr_type ZONEMD_RR_TYPE = 63;
static int ldns_knows_about_zonemd = 0;
const char *RRNAME = "ZONEMD";
static ldns_rdf *origin = 0;
ldns_rr *the_soa = 0;

#define MAX_ZONEMD_COUNT 10

#if !ZONEMD_INCREMENTAL
ldns_rr_list *the_rrlist = 0;
#endif

#if ZONEMD_INCREMENTAL
typedef struct _zonemd_tree {
	unsigned int depth;
	unsigned int branch;    // only for debugging?
	ldns_rr_list *rrlist;
	struct _zonemd_tree *parent;
	struct _zonemd_tree **kids;
	unsigned char digest[EVP_MAX_MD_SIZE];
	bool dirty;
} zonemd_tree;

zonemd_tree *theTree = 0;
unsigned int zonemd_tree_max_depth = 0;
unsigned int zonemd_tree_max_width = 13;
#if ZONEMD_SAVE_LEAF_COUNTS
FILE *save_leaf_counts = 0;
#endif
#endif

#if DEBUG
#define fdebugf(...) fprintf(__VA_ARGS__)
#else
#define fdebugf(...) (void)0
#endif

unsigned int
uimin(unsigned int a, unsigned int b)
{
	if (a < b)
		return a;
	return b;
}

/*
 * zonemd_print_digest()
 *
 * Prints a digest value in hex representation for debugging.
 */
void
zonemd_print_digest(FILE *fp, const char *preamble, const unsigned char *buf, unsigned int len, const char *postamble)
{
	unsigned int i;
	if (preamble)
		fputs(preamble, fp);
	for (i = 0; i < len; i++) {
		fprintf(fp, "%02x", buf[i]);
	}
	if (postamble)
		fputs(postamble, fp);
}

#if ZONEMD_INCREMENTAL

/*
 * zonemd_tree_branch_by_name()
 *
 * Return branch index for a given name and depth
 */
unsigned int
zonemd_tree_branch_by_name(unsigned int depth, const char *name)
{
	unsigned int len;
	unsigned int pos;
	unsigned int branch;
	len = strlen(name);
	if (len == 0)
		return 0;
	pos = depth % len;
	branch = *(name+pos) % zonemd_tree_max_width;
	fdebugf(stderr, "%s(%d): zonemd_tree_branch_by_name '%s' depth %u pos %u branch %u\n", __FILE__, __LINE__, name, depth, pos, branch);
	return branch;
}

/*
 * zonemd_tree_get_leaf_by_name()
 *
 * Return the leaf node corresponding to the given name
 */
zonemd_tree *
zonemd_tree_get_leaf_by_name(zonemd_tree *node, const char *name)
{
	node->dirty = true;
	if (zonemd_tree_max_depth > node->depth) {
		unsigned int branch = zonemd_tree_branch_by_name(node->depth, name);
		if (node->kids == 0) {
			node->kids = calloc(zonemd_tree_max_width, sizeof(*node->kids));
			assert(node->kids);
		}
		if (node->kids[branch] == 0) {
			node->kids[branch] = calloc(1, sizeof(**node->kids));
			assert(node->kids[branch]);
			node->kids[branch]->depth = node->depth+1;
			node->kids[branch]->branch = branch;
			node->kids[branch]->parent = node;
		}
		return zonemd_tree_get_leaf_by_name(node->kids[branch], name);
	}
	fdebugf(stderr, "%s(%d): zonemd_tree_get_leaf depth %u branch %u\n", __FILE__, __LINE__, node->depth, node->branch);
	return node;
}

/*
 * zonemd_tree_get_leaf_by_owner()
 *
 * Wrapper around zonemd_tree_get_leaf_by_name() that takes an ldns_rdf *owner argument
 */
zonemd_tree *
zonemd_tree_get_leaf_by_owner(zonemd_tree *node, const ldns_rdf *owner)
{
	zonemd_tree *leaf;
	char *name = ldns_rdf2str(owner);
	assert(name);
	leaf = zonemd_tree_get_leaf_by_name(node, name);
	assert(leaf->kids == 0);	/* leaf nodes don't have kids */
	free(name);
	return leaf;
}

/*
 * zonemd_tree_add_rr()
 *
 * Add an RR to the tree.
 */
bool
zonemd_tree_add_rr(zonemd_tree *root, ldns_rr *rr)
{
	zonemd_tree *node = zonemd_tree_get_leaf_by_owner(root, ldns_rr_owner(rr));
	if (node->rrlist == 0) {
		node->rrlist = ldns_rr_list_new();
		assert(node->rrlist);
	}
	fdebugf(stderr, "%s(%d): zonemd_tree_add_rr depth %u branch %u\n", __FILE__, __LINE__, node->depth, node->branch);
	return ldns_rr_list_push_rr(node->rrlist, rr);
}

/*
 * zonemd_tree_full_rrlist()
 *
 * Walk all branches of the tree and buld a full rrlist.  The rrlist is
 * allocated by the caller.
 */
void
zonemd_tree_full_rrlist(zonemd_tree *node, ldns_rr_list *rrlist)
{
	if (node == 0)
		return;
	if (zonemd_tree_max_depth > node->depth && node->kids) {
		unsigned int branch;
		for (branch = 0; branch < zonemd_tree_max_width; branch++)
			zonemd_tree_full_rrlist(node->kids[branch], rrlist);
		return;
	}
	ldns_rr_list_push_rr_list(rrlist, node->rrlist);
#if ZONEMD_SAVE_LEAF_COUNTS
	if (save_leaf_counts) {
		fprintf(save_leaf_counts, "%zd\n", ldns_rr_list_rr_count(node->rrlist));
	}
#endif
}

#endif

/*
 * zonemd_rr_create()
 *
 * This function creates and returns an empty ZONEMD record.
 */
ldns_rr *
zonemd_rr_create(ldns_rdf * owner, uint32_t ttl)
{
	ldns_rr *rr = ldns_rr_new();
	assert(rr);
	ldns_rr_set_owner(rr, ldns_rdf_clone(owner));
	ldns_rr_set_ttl(rr, ttl);
	ldns_rr_set_type(rr, ZONEMD_RR_TYPE);
	return rr;
}

/*
 * zonemd_rr_pack()
 *
 * This function packs ZONEMD rdata into an existing RR.
 */
void
zonemd_rr_pack(ldns_rr *rr, uint32_t serial, uint8_t digest_type, void *digest, size_t digest_sz)
{
	while (ldns_rr_rd_count(rr) > 0) {
		ldns_rdf *rdf = ldns_rr_pop_rdf (rr);
		ldns_rdf_deep_free(rdf);
	}
	if (ldns_knows_about_zonemd) {
		char *tbuf = 0;
		if (digest == 0)
			digest = tbuf = calloc(1, digest_sz);
		ldns_rdf *rdf_serial = ldns_native2rdf_int32(LDNS_RDF_TYPE_INT32, serial);
		ldns_rdf *rdf_digtype = ldns_native2rdf_int8(LDNS_RDF_TYPE_INT8, digest_type);
		ldns_rdf *rdf_parameter = ldns_native2rdf_int8(LDNS_RDF_TYPE_INT8, 0);	/* parameter */
		ldns_rdf *rdf_digest = ldns_rdf_new_frm_data(LDNS_RDF_TYPE_HEX, digest_sz, digest);
		assert(rdf_serial);
		assert(rdf_digtype);
		assert(rdf_parameter);
		assert(rdf_digest);
		ldns_rr_push_rdf(rr, rdf_serial);
		ldns_rr_push_rdf(rr, rdf_digtype);
		ldns_rr_push_rdf(rr, rdf_parameter);
		ldns_rr_push_rdf(rr, rdf_digest);
		if (tbuf)
			free(tbuf);
	} else {
		char *buf;
		buf = calloc(1, 4 + 1 + 1 + digest_sz);
		ldns_write_uint32(&buf[0], serial);
		memcpy(&buf[4], &digest_type, 1);
		if (digest && digest_sz)
			memcpy(&buf[6], digest, digest_sz);
		ldns_rdf *rdf = ldns_rdf_new_frm_data(LDNS_RDF_TYPE_UNKNOWN, 4 + 1 + 1 + digest_sz, buf);
		assert(rdf);
		ldns_rr_push_rdf(rr, rdf);
		free(buf);
	}
}

/*
 * zonemd_rr_find()
 *
 * This function searches through the zone data and returns a list of ZONEMD records found.
 */
ldns_rr_list *
zonemd_rr_find(void)
{
	ldns_rr_list *ret = 0;
	ldns_rr_list *rrlist;
	unsigned int i;
	ret = ldns_rr_list_new();
	assert(ret);
#if !ZONEMD_INCREMENTAL
	rrlist = the_rrlist;
#else
	zonemd_tree *node = zonemd_tree_get_leaf_by_owner(theTree, origin);
	rrlist = node->rrlist;
#endif
	for (i = 0; i < ldns_rr_list_rr_count(rrlist); i++) {
		ldns_rr *rr = 0;
		rr = ldns_rr_list_rr(rrlist, i);
		if (ldns_rr_get_type(rr) != ZONEMD_RR_TYPE)
			continue;
		if (ldns_dname_compare(ldns_rr_owner(rr), origin) != 0)
			continue;
		ldns_rr_list_push_rr(ret, rr);
	}
	return ret;
}

/*
 * zonemd_rr_unpack()
 *
 * This function unpacks ZONEMD fields into the ret_ paramaters.
 */
void
zonemd_rr_unpack(ldns_rr *rr, uint32_t *ret_serial, uint8_t *ret_digest_type, void *ret_digest, unsigned int *ret_digest_sz)
{
	ldns_rdf *rdf = 0;
	if (ldns_knows_about_zonemd) {
		rdf = ldns_rr_rdf(rr, 0);
		assert(rdf);
		if (LDNS_RDF_TYPE_INT32 != ldns_rdf_get_type(rdf))
			errx(1, "%s(%d): %s RDF #1 expected type %u, but got type %u", __FILE__, __LINE__, RRNAME, LDNS_RDF_TYPE_INT32, ldns_rdf_get_type(rdf));
		if (ret_serial)
			*ret_serial = ldns_rdf2native_int32(rdf);
		rdf = ldns_rr_rdf(rr, 1);
		assert(rdf);
		if (LDNS_RDF_TYPE_INT8 != ldns_rdf_get_type(rdf))
			errx(1, "%s(%d): %s RDF #2 expected type %u, but got type %u", __FILE__, __LINE__, RRNAME, LDNS_RDF_TYPE_INT8, ldns_rdf_get_type(rdf));
		if (ret_digest_type)
			*ret_digest_type = ldns_rdf2native_int8(rdf);
		rdf = ldns_rr_rdf(rr, 3);
		assert(rdf);
		if (LDNS_RDF_TYPE_HEX != ldns_rdf_get_type(rdf))
			errx(1, "%s(%d): %s RDF #4 expected type %u, but got type %u", __FILE__, __LINE__, RRNAME, LDNS_RDF_TYPE_HEX, ldns_rdf_get_type(rdf));
		if (ret_digest) {
			assert(ret_digest_sz);
			*ret_digest_sz = uimin(*ret_digest_sz, ldns_rdf_size(rdf));
			memcpy(ret_digest, ldns_rdf_data(rdf), *ret_digest_sz);
		}
	} else {
		unsigned char *buf;
		size_t rdlen;
		rdf = ldns_rr_rdf(rr, 0);
		assert(rdf);
		rdlen = ldns_rdf_size(rdf);
		if (rdlen < 6)
			errx(1, "%s(%d): %s RR rdlen (%d) too short", __FILE__, __LINE__, RRNAME, (int) rdlen);
		buf = ldns_rdf_data(rdf);
		assert(buf);
		if (ret_serial)
			*ret_serial = ldns_read_uint32(&buf[0]);
		rdlen -= 4;
		if (ret_digest_type)
			memcpy(ret_digest_type, &buf[4], 1);
		rdlen -= 1;
		rdlen -= 1;	/* skip over parameter field */
		if (ret_digest) {
			assert(ret_digest_sz);
			*ret_digest_sz = uimin(*ret_digest_sz, rdlen);
			memcpy(ret_digest, &buf[6], *ret_digest_sz);
		}
	}
}

/*
 * zonemd_rr_update_digest()
 *
 * Updates the digest part of a placeholder ZONEMD record.  If the new_digest_buf pointer is NULL, the
 * digest value is set to all zeroes.
 */
void
zonemd_rr_update_digest(ldns_rr * rr, uint8_t new_digest_type, unsigned char *new_digest_buf, unsigned int new_digest_len)
{
	uint32_t old_serial = 0;
	uint8_t old_digest_type;
        unsigned int old_digest_sz = EVP_MAX_MD_SIZE;
	unsigned char old_digest_buf[EVP_MAX_MD_SIZE];
	zonemd_rr_unpack(rr, &old_serial, &old_digest_type, old_digest_buf, &old_digest_sz);
	if (old_digest_type != new_digest_type)
		errx(1, "%s(%d): zonemd_rr_update_digest mismatched digest type.  Found %u but wanted %u.", __FILE__, __LINE__, old_digest_type, new_digest_type);
	zonemd_rr_pack(rr, old_serial, old_digest_type, new_digest_buf, new_digest_len);
}

/*
 * my_typecovered()
 *
 * Convenience function to return the typecovered attribute of an RRSIG.
 */
ldns_rr_type
my_typecovered(ldns_rr *rrsig)
{
	ldns_rdf *rdf = ldns_rr_rrsig_typecovered(rrsig);
	assert(rdf);
	return ldns_rdf2native_int16(rdf);
}

/*
 * zonemd_add_rr()
 *
 * Add an RR to the zone data.
 */
void
zonemd_add_rr(ldns_rr *rr)
{
#if !ZONEMD_INCREMENTAL
		ldns_rr_list_push_rr(the_rrlist, rr);
#else
		zonemd_tree_add_rr(theTree, rr);
#endif
}

/*
 * zonemd_remove_rr()
 *
 * Remove RRs of type 'type' from the zone apex.  If 'type' is RRISG then
 * signatures of type 'covered' are removed.
 */
void
zonemd_remove_rr(ldns_rr_type type, ldns_rr_type covered)
{
	unsigned int i;
	ldns_rr_list **oldp = 0;
	ldns_rr_list *new = 0;
	ldns_rr_list *tbd = 0;

#if !ZONEMD_INCREMENTAL
	oldp = &the_rrlist;
#else
	zonemd_tree *node = zonemd_tree_get_leaf_by_owner(theTree, origin);
	oldp = &node->rrlist;
#endif

	new = ldns_rr_list_new();
	tbd = ldns_rr_list_new();
	assert(new);
	assert(tbd);

	for (i = 0; i < ldns_rr_list_rr_count(*oldp); i++) {
		ldns_rr *rr = ldns_rr_list_rr(*oldp, i);
		if (ldns_dname_compare(ldns_rr_owner(rr), origin) != 0) {
			ldns_rr_list_push_rr(new, rr);
		} else if (ldns_rr_get_type(rr) != type) {
			ldns_rr_list_push_rr(new, rr);
		} else if (type == LDNS_RR_TYPE_RRSIG && my_typecovered(rr) != covered) {
			ldns_rr_list_push_rr(new, rr);
		} else {
			ldns_rr_list_push_rr(tbd, rr);
		}
	}
	ldns_rr_list_free(*oldp);
	*oldp = new;
	ldns_rr_list_deep_free(tbd);
}

/*
 * honemd_digester()
 *
 * wrapper around EVP_get_digestbyname() and so we can reference by number
 */
const EVP_MD *
zonemd_digester(uint8_t type)
{
	const char *name = 0;
	const EVP_MD *md = 0;
	OpenSSL_add_all_digests();
	if (type == 1) {
		name = "sha384";
	} else {
		/* warnx("%s(%d): Unsupported digest type %u", __FILE__, __LINE__, type); */
		return 0;
	}
	md = EVP_get_digestbyname(name);
	if (md == 0)
		errx(1, "%s(%d): Unknown message digest '%s'", __FILE__, __LINE__, name);
	return md;
}

/*
 *
 * zonemd_rrlist_digest()
 *
 * Loops over an rrlist and calls the digest update function on each RR.
 */
void
zonemd_rrlist_digest(ldns_rr_list *rrlist, EVP_MD_CTX *ctx)
{
	unsigned int i;
	ldns_status status;
	ldns_rr *prev = 0;
	/*
	 * thankfully ldns_rr_list_sort() already sorts by RRtype for same owner name
	 */
	ldns_rr_list_sort(rrlist);
	for (i = 0; i < ldns_rr_list_rr_count(rrlist); i++) {
		uint8_t *wire_buf;
		size_t sz;
		ldns_rr *rr = ldns_rr_list_rr(rrlist, i);
		ldns_rr *rr_copy = 0;
		if (prev && ldns_rr_compare(rr, prev) == 0) {
			char *s = ldns_rr2str(rr);
			assert(s);
			warnx("%s(%d): Ignoring duplicate RR: %s\n", __FILE__, __LINE__, s);
			free(s);
			continue;
		}
		prev = rr;
		/*
		 * Don't include RRSIG over ZONEMD in the digest
		 */
		if (ldns_rr_get_type(rr) == LDNS_RR_TYPE_RRSIG)
			if (my_typecovered(rr) == ZONEMD_RR_TYPE)
				continue;
		/*
		 * For ZONEMD RRs at apex, create a copy with digest zeroized
		 */
		if (ldns_rr_get_type(rr) == LDNS_RR_TYPE_ZONEMD && ldns_dname_compare(ldns_rr_owner(rr), origin) == 0) {
			uint8_t type = 0;
			unsigned char digest[EVP_MAX_MD_SIZE];
			unsigned int digest_len = EVP_MAX_MD_SIZE;
			uint32_t serial = 0;
			const EVP_MD *md = 0;
			rr_copy = ldns_rr_clone(rr);
			zonemd_rr_unpack(rr_copy, &serial, &type, digest, &digest_len);
			md = zonemd_digester(type);
			if (md != 0) {
				assert(EVP_MD_size(md) <= (int) sizeof(digest));
				digest_len = EVP_MD_size(md);
			}
			zonemd_rr_update_digest(rr_copy, type, 0, digest_len);	/* zero digest part */
			rr = rr_copy;
		}
#if DEBUG
		char *s = ldns_rr2str(rr);
		fdebugf(stderr, "%s(%d): zonemd_rrlist_digest RR#%u: %s\n", __FILE__, __LINE__, i, s);
		free(s);
#endif
		status = ldns_rr2wire(&wire_buf, rr, LDNS_SECTION_ANSWER, &sz);
		if (status != LDNS_STATUS_OK)
			errx(1, "%s(%d): ldns_rr2wire() failed", __FILE__, __LINE__);
		if (!EVP_DigestUpdate(ctx, wire_buf, sz))
			errx(1, "%s(%d): Digest update failed", __FILE__, __LINE__);
		free(wire_buf);
		if (rr_copy != 0)
			ldns_rr_free(rr_copy);
	}
}

/*
 * zonemd_calc_digest()
 *
 * Calculate a digest over the zone.
 */
void
zonemd_calc_digest(void *arg, const EVP_MD *md, unsigned char *buf, uint8_t digest_type)
{
	EVP_MD_CTX *ctx;
#if !ZONEMD_INCREMENTAL
	(void)(arg); /* skip warning: unused parameter 'arg' */
	if (!quiet)
		fprintf(stderr, "Calculating Digest for type %u\n", digest_type);
#else
	zonemd_tree *node = arg;
	fdebugf(stderr, "%s(%d): zonemd_calc_digest depth %u branch %u\n", __FILE__, __LINE__, node->depth, node->branch);
	if (!node->dirty)
		return;
#endif
	ctx = EVP_MD_CTX_create();
	assert(ctx);
	if (!EVP_DigestInit(ctx, md))
		errx(1, "%s(%d): Digest init failed", __FILE__, __LINE__);
#if !ZONEMD_INCREMENTAL
	zonemd_rrlist_digest(the_rrlist, ctx);
#else
	if (zonemd_tree_max_depth > node->depth) {
		unsigned int branch;
		assert(node->kids);
		for (branch = 0; branch < zonemd_tree_max_width; branch++) {
			if (node->kids[branch] == 0)
				continue;
			zonemd_calc_digest(node->kids[branch], md, (unsigned char *) node->digest, digest_type);
			if (!EVP_DigestUpdate(ctx, node->digest, EVP_MD_size(md)))
				errx(1, "%s(%d): Digest update failed", __FILE__, __LINE__);
		}
	} else {
		assert(node->rrlist);
		ldns_rr_list_sort(node->rrlist);
		zonemd_rrlist_digest(node->rrlist, ctx);
	}
#endif
	if (!EVP_DigestFinal_ex(ctx, buf, 0))
		errx(1, "%s(%d): Digest final failed", __FILE__, __LINE__);
	EVP_MD_CTX_destroy(ctx);
#if ZONEMD_INCREMENTAL
	node->dirty = false;
#endif
}

/*
 * zonemd_resign()
 *
 * Calculate an RRSIG for the ZONEMD RRset ('rrset' parameter).  Requires access to the private
 * zone signing key.
 */
void
zonemd_resign(ldns_rr_list * rrset, const char *zsk_fname)
{
	FILE *fp = 0;
	ldns_key *zsk = 0;
	ldns_key_list *keys = 0;
	ldns_status status;
	ldns_rr_list *rrsig = 0;
	unsigned int i;

	fp = fopen(zsk_fname, "r");
	if (fp == 0)
		err(1, "%s(%d): %s", __FILE__, __LINE__, zsk_fname);
	status = ldns_key_new_frm_fp(&zsk, fp);
	if (status != LDNS_STATUS_OK)
		errx(1, "%s(%d): ldns_key_new_frm_fp: %s", __FILE__, __LINE__, ldns_get_errorstr_by_id(status));
	ldns_key_set_pubkey_owner(zsk, origin);
	keys = ldns_key_list_new();
	assert(keys);
	ldns_key_list_push_key(keys, zsk);

	rrsig = ldns_sign_public(rrset, keys);
	if (rrsig == 0)
		errx(1, "%s(%d): ldns_sign_public() failed", __FILE__, __LINE__);

	zonemd_remove_rr(LDNS_RR_TYPE_RRSIG, ZONEMD_RR_TYPE);
	for (i = 0; i < ldns_rr_list_rr_count(rrsig); i++)
		zonemd_add_rr(ldns_rr_list_rr(rrsig, i));
	ldns_key_list_free(keys);
	ldns_rr_list_free(rrsig);
}

/*
 * zonemd_write_zone()
 *
 * Prints all zone records to 'fp'
 */
void
zonemd_write_zone(FILE * fp)
{
	ldns_rr_list *rrlist = 0;
	unsigned int i;

#if !ZONEMD_INCREMENTAL
	rrlist = the_rrlist;
#else
	rrlist = ldns_rr_list_new();
	zonemd_tree_full_rrlist(theTree, rrlist);
#endif

	assert(rrlist);
	ldns_rr_list_sort(rrlist);
	for (i = 0; i < ldns_rr_list_rr_count(rrlist); i++) {
		ldns_rr *rr = ldns_rr_list_rr(rrlist, i);
		if (rr)
			ldns_rr_print(fp, rr);
	}
#if ZONEMD_INCREMENTAL
	ldns_rr_list_free(rrlist);
#endif
}

void
usage(const char *p)
{
	fprintf(stderr, "usage: %s [options] origin [zonefile]\n", p);
	fprintf(stderr, "\t-c\t\tcalculate the zone digest\n");
	fprintf(stderr, "\t-o file\t\twrite zone to output file\n");
	fprintf(stderr, "\t-u file\t\tfile containing RR updates\n");
	fprintf(stderr, "\t-p type\t\tinsert placeholder record of type\n");
	fprintf(stderr, "\t-v\t\tverify the zone digest\n");
	fprintf(stderr, "\t-z\t\tZSK file name\n");
#if ZONEMD_INCREMENTAL
	fprintf(stderr, "\t-D\t\tDepth of hash tree\n");
	fprintf(stderr, "\t-W\t\tWidth of hash tree\n");
#endif
	fprintf(stderr, "\t-q\t\tquiet mode, show errors only\n");
	exit(2);
}

void
my_getrusage(struct timeval *ret)
{
	struct rusage ru;
	memset(&ru, 0, sizeof(ru));
	getrusage(RUSAGE_SELF, &ru);
	timeradd(&ru.ru_utime, &ru.ru_stime, ret);
}

double
elapsed_msec(struct timeval *a, struct timeval *b)
{
	double dt = 1000.0 * b->tv_sec - 1000.0 *  a->tv_sec;
	dt += (double) b->tv_usec / 1000.0 - (double) a->tv_usec / 1000.0;
	return dt;
}

/*
 * zonemd_add_placeholders()
 *
 * Creates a placeholder ZONEMD record and adds it to 'zone'.  If 'zone' already
 * has a ZONEMD record, it is removed and discarded.
 */
void
zonemd_add_placeholders(uint8_t digest_types[], unsigned int digest_types_cnt)
{
	ldns_rdf *soa_serial_rdf = 0;
	uint32_t soa_serial;
	unsigned int i;

	if (!quiet)
		fprintf(stderr, "Remove existing ZONEMD RRset\n");
	zonemd_remove_rr(ZONEMD_RR_TYPE, 0);

	soa_serial_rdf = ldns_rr_rdf(the_soa, 2);
	soa_serial = ldns_rdf2native_int32(soa_serial_rdf);

	for (i = 0; i < digest_types_cnt; i++) {
		const EVP_MD *md = 0;
		unsigned int digest_len = 0;
		unsigned char *digest_buf = 0;
		ldns_rr *zonemd = 0;
		unsigned int j;
		bool is_dupe = 0;

		for (j = 0; j < i; j++)
			if (digest_types[i] == digest_types[j])
				is_dupe |= 1;

		if (is_dupe) {
			fprintf(stderr, "Ignoring duplicate digest type %u\n", digest_types[i]);
			continue;
		}

		md = zonemd_digester(digest_types[i]);
		assert(md);
		digest_len = EVP_MD_size(md);
		assert(digest_len);
		digest_buf = calloc(1, digest_len);
		assert(digest_buf);
		zonemd = zonemd_rr_create(ldns_rr_owner(the_soa), ldns_rr_ttl(the_soa));
		zonemd_rr_pack(zonemd, soa_serial, digest_types[i], digest_buf, digest_len);
		free(digest_buf);
		if (!quiet)
			fprintf(stderr, "Add placeholder ZONEMD with digest type %u\n", digest_types[i]);
		zonemd_add_rr(zonemd);
	}
}

/*
 * zonemd_read_zone()
 *
 * Read a zone file from disk, with a little extra processing.
 *
 */
void
zonemd_read_zone(const char *origin_str, FILE * fp, uint32_t ttl, ldns_rr_class class)
{
	ldns_zone *zone;
	ldns_status status;
	ldns_rr_list *oldlist;
	ldns_rr_list *tbflist;
	unsigned int i;
	unsigned int count = 0;

	if (!quiet)
		fprintf(stderr, "Loading Zone...");
	origin = ldns_rdf_new_frm_str(LDNS_RDF_TYPE_DNAME, origin_str);
	assert(origin);
	status = ldns_zone_new_frm_fp(&zone, fp, origin, ttl, class);
	if (status != LDNS_STATUS_OK)
		errx(1, "%s(%d): ldns_zone_new_frm_fp: %s", __FILE__, __LINE__, ldns_get_errorstr_by_id(status));
	if (!ldns_zone_soa(zone))
		errx(1, "%s(%d): No SOA record in zone", __FILE__, __LINE__);
	the_soa = ldns_rr_clone(ldns_zone_soa(zone));
	zonemd_add_rr(the_soa);
	count++;
	/*
	 * Remove any out-of-zone data
	 */
	oldlist = ldns_zone_rrs(zone);
	tbflist = ldns_rr_list_new();
	for (i = 0; i < ldns_rr_list_rr_count(oldlist); i++) {
		ldns_rr *rr = ldns_rr_list_rr(oldlist, i);
		if (ldns_dname_compare(ldns_rr_owner(rr), origin) == 0) {
			/* same owner */
			(void) 0;
		} else if (ldns_dname_is_subdomain(ldns_rr_owner(rr), origin)) {
			/* subdomain */
			(void) 0;
		} else {
			/* out-of-zone */
			char *s = ldns_rdf2str(ldns_rr_owner(rr));
			assert(s);
			warnx("%s(%d): Ignoring out-of-zone data for '%s'", __FILE__, __LINE__, s);
			free(s);
			ldns_rr_list_push_rr(tbflist, rr);
			continue;
		}
		zonemd_add_rr(rr);
		count++;
	}

	if (!quiet)
		fprintf(stderr, "%u records\n", count);
	ldns_rr_list_deep_free(tbflist);
	ldns_rr_list_free(oldlist);
	ldns_zone_set_rrs(zone, 0);
	ldns_zone_deep_free(zone);
}

/*
 * zonemd_zone_update()
 *
 * Process incremental updates to zone data.  Input file has lines that start with 'add' or
 * 'del' followed by an RR in presentation format:
 *
 * del example. IN A 1.2.3.4
 * add example. IN A 2.3.4.5
 *
 */
void
zonemd_zone_update(const char *update_file)
{
	ldns_status status;
	char file_buf[4096];
	unsigned int n_add = 0;
	unsigned int n_del = 0;
	unsigned int line = 0;
	FILE *fp;

	fp = fopen(update_file, "r");
	if (!fp)
		err(1, "%s(%d): %s", __FILE__, __LINE__, update_file);

	if (!quiet)
		fprintf(stderr, "Updating Zone...");
	while (fgets(file_buf, sizeof(file_buf), fp)) {
		line++;
		char *cmd = 0;
		char *rr_str = 0;
		ldns_rr *rr = 0;
		cmd = strtok(file_buf, " \t");
		if (cmd == 0) {
			warnx("%s(%d): zonemd_zone_update: %s line %u unparseable input", __FILE__, __LINE__, update_file, line);
			continue;
		}
		rr_str = strtok(0, "\r\n");
		if (rr_str == 0) {
			warnx("%s(%d): zonemd_zone_update: %s line %u unparseable input", __FILE__, __LINE__, update_file, line);
			continue;
		}
		status = ldns_rr_new_frm_str(&rr, rr_str, 0, origin, 0);
		if (status != LDNS_STATUS_OK)
			errx(1, "%s(%d): ldns_rr_new_frm_str: %s", __FILE__, __LINE__, ldns_get_errorstr_by_id(status));
		if (0 == strcmp(cmd, "add")) {
			zonemd_add_rr(rr);
			n_add++;
		} else if (0 == strcmp(cmd, "del")) {
			n_del++;
		} else {
			warnx("%s(%d): zonemd_zone_update: %s line %u expected 'add' or 'del'", __FILE__, __LINE__, update_file, line);
			continue;
		}
	}
	fclose(fp);
	if (!quiet)
		fprintf(stderr, "%u additions, %u deletions\n", n_add, n_del);
}

void
do_calculate(const char *zsk_fname)
{
	ldns_rr_list *zonemd_rr_list = zonemd_rr_find();
	unsigned int i;
	if (!zonemd_rr_list)
		errx(1, "%s(%d): No %s record found at zone apex.  Use -p to add one.", __FILE__, __LINE__, RRNAME);
	for (i = 0; i < ldns_rr_list_rr_count(zonemd_rr_list); i++) {
		uint8_t found_digest_type = 0;
		unsigned char *md_buf = 0;
		unsigned int md_len = 0;
		const EVP_MD *md = 0;
		ldns_rr *zonemd_rr = ldns_rr_list_rr(zonemd_rr_list, i);
		zonemd_rr_unpack(zonemd_rr, 0, &found_digest_type, 0, 0);
		md = zonemd_digester(found_digest_type);
		assert(md);
		md_len = EVP_MD_size(md);
#if !ZONEMD_INCREMENTAL
		md_buf = calloc(1, md_len);
		assert(md_buf);
		zonemd_calc_digest(0, md, md_buf, found_digest_type);
#else
		md_buf = theTree->digest;
		zonemd_calc_digest(theTree, md, md_buf, found_digest_type);
#endif
		zonemd_rr_update_digest(zonemd_rr, found_digest_type, md_buf, md_len);
#if !ZONEMD_INCREMENTAL
		free(md_buf);
#endif
	}
	if (zsk_fname)
		zonemd_resign(zonemd_rr_list, zsk_fname);
	ldns_rr_list_free(zonemd_rr_list);
}

int
do_verify(void)
{
	int rc = 0;
	ldns_rr_list *zonemd_rr_list = zonemd_rr_find();
	unsigned int i;
	if (!zonemd_rr_list)
		errx(1, "%s(%d): No %s record found at zone apex, cannot verify.", __FILE__, __LINE__, RRNAME);
	for (i = 0; i < ldns_rr_list_rr_count(zonemd_rr_list); i++) {
		uint8_t found_digest_type;
		unsigned char found_digest_buf[EVP_MAX_MD_SIZE];
		unsigned int found_digest_len = EVP_MAX_MD_SIZE;
		uint32_t found_serial = 0;
		ldns_rdf *soa_serial_rdf = 0;
		uint32_t soa_serial = 0;
		const EVP_MD *md = 0;
		unsigned char *md_buf = 0;
		unsigned int md_len = 0;
		ldns_rr *zonemd_rr = ldns_rr_list_rr(zonemd_rr_list, i);
		zonemd_rr_unpack(zonemd_rr, &found_serial, &found_digest_type, found_digest_buf, &found_digest_len);
		soa_serial_rdf = ldns_rr_rdf(the_soa, 2);
		soa_serial = ldns_rdf2native_int32(soa_serial_rdf);
		if (found_serial != soa_serial) {
			fprintf(stderr, "%s(%d): SOA serial (%u) does not match ZONEMD serial (%u)\n", __FILE__, __LINE__, soa_serial, found_serial);
			rc |= 1;
		}
		md = zonemd_digester(found_digest_type);
		if (md == 0) {
			fprintf(stderr, "Unable to verify unsupported digest type %u\n", found_digest_type);
			continue;
		}
		assert(EVP_MD_size(md) <= (int) sizeof(found_digest_buf));
		md_len = EVP_MD_size(md);
#if ZONEMD_INCREMENTAL
		md_buf = theTree->digest;
		zonemd_calc_digest(theTree, md, md_buf, found_digest_type);
#else
		md_buf = calloc(1, md_len);
		assert(md_buf);
		zonemd_calc_digest(0, md, md_buf, found_digest_type);
#endif
		if (memcmp(found_digest_buf, md_buf, md_len) != 0) {
			fprintf(stderr, "Found and calculated digests for type %u do NOT match.\n", found_digest_type);
			zonemd_print_digest(stderr, "Found     : ", found_digest_buf, md_len, "\n");
			zonemd_print_digest(stderr, "Calculated: ", md_buf, md_len, "\n");
			rc |= 1;
		} else {
			if (!quiet)
				fprintf(stderr, "Found and calculated digests for type %u do MATCH.\n", found_digest_type);
		}
#if !ZONEMD_INCREMENTAL
		free(md_buf);
#endif
	}
	ldns_rr_list_free(zonemd_rr_list);
	return rc;
}

void
probe_ldns(const char *origin_str)
{
	ldns_rr *rr = 0;
	ldns_rdf *origin = ldns_rdf_new_frm_str(LDNS_RDF_TYPE_DNAME, origin_str);
	ldns_status status = ldns_rr_new_frm_str (&rr, "test 300 IN ZONEMD 123456789 2 0 deadbeef", 0, origin, 0);
	fdebugf(stderr, "%s(%d): probe_ldns: %s\n", __FILE__, __LINE__, ldns_get_errorstr_by_id(status));
	if (LDNS_STATUS_OK == status) {
		ldns_knows_about_zonemd = 1;
		ZONEMD_RR_TYPE = ldns_rr_get_type(rr);
	}
	ldns_rdf_deep_free(origin);
	if (rr)
		ldns_rr_free(rr);
}

int
main(int argc, char *argv[])
{
	int ch;
	FILE *input = stdin;
	char *progname = 0;
	char *output_file = 0;
	char *update_file = 0;
	char *origin_str = 0;
	char *zsk_fname = 0;
	uint8_t algorithms[MAX_ZONEMD_COUNT];
	unsigned int algorithms_cnt = 0;
	int calculate = 0;
	int verify = 0;
	int print_timings = 0;
	int rc = 0;
	struct timeval t0, t1, t2, t3, t4;

	progname = strrchr(argv[0], '/');
	if (0 == progname)
		progname = argv[0];
	memset(algorithms, 0, sizeof(algorithms));

	while ((ch = getopt(argc, argv, "co:p:tu:vz:W:D:q")) != -1) {
		switch (ch) {
		case 'c':
			calculate = 1;
			break;
		case 'o':
			output_file = strdup(optarg);
			break;
		case 'p':
			if (algorithms_cnt < MAX_ZONEMD_COUNT)
				algorithms[algorithms_cnt++] = (uint8_t) strtoul(optarg, 0, 10);
			break;
		case 't':
			print_timings = 1;
			break;
		case 'u':
			update_file = strdup(optarg);
			break;
		case 'v':
			verify = 1;
			break;
		case 'z':
			zsk_fname = strdup(optarg);
			break;
#if ZONEMD_INCREMENTAL
		case 'D':
			zonemd_tree_max_depth = strtoul(optarg, 0, 10);
			break;
		case 'W':
			zonemd_tree_max_width = strtoul(optarg, 0, 10);
			break;
#endif
		case 'q':
			quiet = 1;
			break;
		default:
			usage(progname);
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1 || argc > 2)
		usage(progname);
	origin_str = strdup(argv[0]);
	if (argc == 2) {
		input = fopen(argv[1], "r");
		if (0 == input)
			err(1, "%s(%d): %s", __FILE__, __LINE__, argv[1]);
	}

	probe_ldns(origin_str);

	my_getrusage(&t0);



#if !ZONEMD_INCREMENTAL
	the_rrlist = ldns_rr_list_new();
#else
	theTree = calloc(1, sizeof(*theTree));
	assert(theTree);
#if ZONEMD_SAVE_LEAF_COUNTS
	save_leaf_counts = fopen("leaf-counts.dat", "w");
#endif
#endif
	zonemd_read_zone(origin_str, input, 0, LDNS_RR_CLASS_IN);

	if (algorithms_cnt)
		zonemd_add_placeholders(algorithms, algorithms_cnt);
	my_getrusage(&t1);
	if (calculate)
		do_calculate(zsk_fname);
	my_getrusage(&t2);
	if (verify)
		rc |= do_verify();
	my_getrusage(&t3);
	if (update_file) {
		zonemd_zone_update(update_file);
		if (calculate)
			do_calculate(zsk_fname);
	}
	my_getrusage(&t4);
	if (output_file && (algorithms_cnt || calculate)) {
		FILE *fp = fopen(output_file, "w");
		if (!fp)
			err(1, "%s(%d): %s", __FILE__, __LINE__, output_file);
		zonemd_write_zone(fp);
		fclose(fp);
	}

	if (zsk_fname)
		free(zsk_fname);
	if (origin_str)
		free(origin_str);
	if (output_file)
		free(output_file);
	if (update_file)
		free(update_file);

	if (print_timings)
		printf("TIMINGS: load %7.2lf calculate %7.2lf verify %7.2lf update %7.2lf\n",
			elapsed_msec(&t0, &t1),
			elapsed_msec(&t1, &t2),
			elapsed_msec(&t2, &t3),
			elapsed_msec(&t3, &t4));

	return rc;
}
