/*
   +----------------------------------------------------------------------+
   | PHP Version 4                                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) 1997-2002 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 2.02 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available at through the world-wide-web at                           |
   | http://www.php.net/license/2_02.txt.                                 |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Author: Wez Furlong <wez@thebrainroom.com>                           |
   +----------------------------------------------------------------------+
 */
/* $Id$ */

#include "php.h"
#include "php_mailparse.h"
#include "php_mailparse_mime.h"
#include "php_mailparse_rfc822.h"

#define	MAXLEVELS	20
#define	MAXPARTS	300
#define IS_MIME_1(part)	((part)->mime_version && strcmp("1.0", (part)->mime_version) == 0)
#define CONTENT_TYPE_IS(part, contenttypevalue)	((part)->content_type && strcasecmp((part)->content_type->value, contenttypevalue) == 0)
#define CONTENT_TYPE_ISL(part, contenttypevalue, len)	((part)->content_type && strncasecmp((part)->content_type->value, contenttypevalue, len) == 0)

static void php_mimeheader_free(struct php_mimeheader_with_attributes *attr)
{
	STR_FREE(attr->value);
	zval_dtor(attr->attributes);
	efree(attr->attributes);
	efree(attr);
}

static struct php_mimeheader_with_attributes * php_mimeheader_alloc(char *value)
{
	struct php_mimeheader_with_attributes *attr;
	
	attr = ecalloc(1, sizeof(struct php_mimeheader_with_attributes));

	MAKE_STD_ZVAL(attr->attributes);
	array_init(attr->attributes);

	attr->value = estrdup(value);

	return attr;
}

static struct php_mimeheader_with_attributes *php_mimeheader_alloc_from_tok(php_rfc822_tokenized_t *toks)
{
	struct php_mimeheader_with_attributes *attr;
	int i, first_semi, next_semi;
	
	attr = ecalloc(1, sizeof(struct php_mimeheader_with_attributes));

	MAKE_STD_ZVAL(attr->attributes);
	array_init(attr->attributes);

/*  php_rfc822_print_tokens(toks); */
	
	/* look for optional ; which separates optional attributes from the main value */
	for (first_semi = 2; first_semi < toks->ntokens; first_semi++)
		if (toks->tokens[first_semi].token == ';')
			break;

	attr->value = php_rfc822_recombine_tokens(toks, 2, first_semi - 2, PHP_RFC822_RECOMBINE_STRTOLOWER|PHP_RFC822_RECOMBINE_IGNORE_COMMENTS);

	if (first_semi < toks->ntokens)
		first_semi++;

	while (first_semi < toks->ntokens) {
		/* find the next ; */
		for (next_semi = first_semi; next_semi < toks->ntokens; next_semi++)
			if (toks->tokens[next_semi].token == ';')
				break;
	
		i = first_semi;
		if (i < next_semi)	{
			i++;

			/* ignore comments */
			while (i < next_semi && toks->tokens[i].token == '(')
				i++;

			if (i < next_semi && toks->tokens[i].token == '=') {
				char *name, *value;

				/* Here, next_semi --> "name" and i --> "=", so skip "=" sign */
				i++;

				name = php_rfc822_recombine_tokens(toks, first_semi, 1,
						PHP_RFC822_RECOMBINE_STRTOLOWER|PHP_RFC822_RECOMBINE_IGNORE_COMMENTS);
				value = php_rfc822_recombine_tokens(toks, i, next_semi - i,
						PHP_RFC822_RECOMBINE_STRTOLOWER|PHP_RFC822_RECOMBINE_IGNORE_COMMENTS);
				
				add_assoc_string(attr->attributes, name, value, 0);
				efree(name);
			}
		}
		if (next_semi < toks->ntokens)
			next_semi++;

		first_semi = next_semi;
	}
	return attr;
}

static void php_mimepart_free_child(php_mimepart **part)
{
	TSRMLS_FETCH();
	php_mimepart_free(*part TSRMLS_CC);
}

PHPAPI php_mimepart *php_mimepart_alloc(void)
{
	php_mimepart *part = ecalloc(1, sizeof(php_mimepart));

	part->part_index = 1;

	zend_hash_init(&part->children, 0, NULL, (dtor_func_t)php_mimepart_free_child, 0);
	
	MAKE_STD_ZVAL(part->headerhash);
	array_init(part->headerhash);

	MAKE_STD_ZVAL(part->source.zval);
	
	/* begin in header parsing mode */
	part->parsedata.in_header = 1;
	part->rsrc_id = ZEND_REGISTER_RESOURCE(NULL, part, php_mailparse_le_mime_part());

	return part;
}


PHPAPI void php_mimepart_free(php_mimepart *part TSRMLS_DC)
{
	if (part->rsrc_id) {
		long tmp = part->rsrc_id;
		part->rsrc_id = 0;
		zend_list_delete(tmp);
		if (part->parent != NULL && part->parent->rsrc_id > 0)
			return;
	}
	
	/* free contained parts */

	zend_hash_destroy(&part->children);

	STR_FREE(part->mime_version);
	STR_FREE(part->content_transfer_encoding);
	STR_FREE(part->charset);
	STR_FREE(part->content_base);
	STR_FREE(part->content_location);

	if (part->content_type)
		php_mimeheader_free(part->content_type);
	if (part->content_disposition)
		php_mimeheader_free(part->content_disposition);
	
	smart_str_free(&part->parsedata.workbuf);
	smart_str_free(&part->parsedata.headerbuf);
	
	zval_dtor(part->headerhash);
	zval_dtor(part->source.zval);
	
	efree(part->source.zval);
	efree(part->headerhash);
	efree(part);
}

static void php_mimepart_update_positions(php_mimepart *part, size_t newendpos, size_t newbodyend, size_t deltanlines)
{
	while(part) {
		part->endpos = newendpos;
		part->bodyend = newbodyend;
		part->nlines += deltanlines;
		if (!part->parsedata.in_header)
			part->nbodylines += deltanlines;
		part = part->parent;
	}
}

PHPAPI char *php_mimepart_attribute_get(struct php_mimeheader_with_attributes *attr, char *attrname)
{
	zval **attrval;

	if (SUCCESS == zend_hash_find(Z_ARRVAL_P(attr->attributes), attrname, strlen(attrname)+1, (void**)&attrval))
		return Z_STRVAL_PP(attrval);
	return NULL;
}

#define STR_SET_REPLACE(ptr, newval)	do { STR_FREE(ptr); ptr = estrdup(newval); } while(0)

static int php_mimepart_process_header(php_mimepart *part TSRMLS_DC)
{
	php_rfc822_tokenized_t *toks;
	char *header_key, *header_val, *header_val_stripped;
	zval **zheaderval;

	if (part->parsedata.headerbuf.len == 0)
		return SUCCESS;

	smart_str_0(&part->parsedata.headerbuf);

	/* parse the header line */
	toks = php_mailparse_rfc822_tokenize((const char*)part->parsedata.headerbuf.c, 0 TSRMLS_CC);

	/* valid headers consist of at least three tokens, with the first being a string and the
	 * second token being a ':' */
	if (toks->ntokens < 2 || toks->tokens[0].token != 0 || toks->tokens[1].token != ':') {
		part->parsedata.headerbuf.len = 0;

		php_rfc822_tokenize_free(toks);
		return FAILURE;
	}

	/* get a lower-case version of the first token */
	header_key = php_rfc822_recombine_tokens(toks, 0, 1, PHP_RFC822_RECOMBINE_IGNORE_COMMENTS|PHP_RFC822_RECOMBINE_STRTOLOWER);
	
	header_val = strchr(part->parsedata.headerbuf.c, ':');
	header_val_stripped = php_rfc822_recombine_tokens(toks, 2, toks->ntokens-2, PHP_RFC822_RECOMBINE_IGNORE_COMMENTS|PHP_RFC822_RECOMBINE_STRTOLOWER);
	
	if (header_val) {
		header_val++;
		while (isspace(*header_val))
			header_val++;

		/* add the header to the hash.
		 * join multiple To: or Cc: lines together */
		if ((strcmp(header_key, "to") == 0 || strcmp(header_key, "cc") == 0) &&
			SUCCESS == zend_hash_find(Z_ARRVAL_P(part->headerhash), header_key, strlen(header_key)+1, (void**)&zheaderval)) {
			int newlen;
			char *newstr;

			newlen = strlen(header_val) + Z_STRLEN_PP(zheaderval) + 3;
			newstr = emalloc(newlen);

			strcpy(newstr, Z_STRVAL_PP(zheaderval));
			strcat(newstr, ", ");
			strcat(newstr, header_val);

			add_assoc_string(part->headerhash, header_key, newstr, 0);
		} else {
			add_assoc_string(part->headerhash, header_key, header_val, 1);
		}
			
		/* if it is useful, keep a pointer to it in the mime part */
		if (strcmp(header_key, "mime-version") == 0)
			STR_SET_REPLACE(part->mime_version, header_val_stripped);

		if (strcmp(header_key, "content-location") == 0) {
			STR_FREE(part->content_location);
			part->content_location = php_rfc822_recombine_tokens(toks, 2, toks->ntokens-2, PHP_RFC822_RECOMBINE_IGNORE_COMMENTS);
		}
		if (strcmp(header_key, "content-base") == 0) {
			STR_FREE(part->content_base);
			part->content_base = php_rfc822_recombine_tokens(toks, 2, toks->ntokens-2, PHP_RFC822_RECOMBINE_IGNORE_COMMENTS);
		}
			
		if (strcmp(header_key, "content-transfer-encoding") == 0)
			STR_SET_REPLACE(part->content_transfer_encoding, header_val_stripped);
		if (strcmp(header_key, "content-type") == 0) {
			char *charset;

			if (part->content_type)
				php_mimeheader_free(part->content_type);

			part->content_type = php_mimeheader_alloc_from_tok(toks);
			part->boundary = php_mimepart_attribute_get(part->content_type, "boundary");
			
			charset = php_mimepart_attribute_get(part->content_type, "charset");
			if (charset) {
				STR_SET_REPLACE(part->charset, charset);
			}
		}
		if (strcmp(header_key, "content-disposition") == 0) {
			part->content_disposition = php_mimeheader_alloc_from_tok(toks);
		}
		
	}
	STR_FREE(header_key);
	STR_FREE(header_val_stripped);

	php_rfc822_tokenize_free(toks);

	/* zero the buffer size */
	part->parsedata.headerbuf.len = 0;
	return SUCCESS;
}

static php_mimepart *alloc_new_child_part(php_mimepart *parentpart, size_t startpos, int inherit)
{
	php_mimepart *child = php_mimepart_alloc();
	int ret;

	child->parent = parentpart;
	
	child->source.kind = parentpart->source.kind;
	REPLACE_ZVAL_VALUE(&child->source.zval, parentpart->source.zval, 1);
	
	ret = zend_hash_next_index_insert(&parentpart->children, (void*)&child, sizeof(php_mimepart *), NULL);
	child->startpos = child->endpos = child->bodystart = child->bodyend = startpos;

	if (inherit) {
		if (parentpart->content_transfer_encoding)
			child->content_transfer_encoding = estrdup(parentpart->content_transfer_encoding);
		if (parentpart->charset)
			child->charset = estrdup(parentpart->charset);
	}
	
	return child;
}

static int php_mimepart_process_line(php_mimepart *part TSRMLS_DC)
{
	php_mimepart *workpart;
	size_t origcount, linelen;
	char *c;

	/* sanity check */
	if (zend_hash_num_elements(&part->children) > MAXPARTS) {
		zend_error(E_WARNING, "%s(): MIME message too complex", get_active_function_name(TSRMLS_C));
		return FAILURE;
	}

	c = part->parsedata.workbuf.c;
	smart_str_0(&part->parsedata.workbuf);
	
	/* strip trailing \r\n -- we always have a trailing \n */
	origcount = part->parsedata.workbuf.len;
	linelen = origcount - 1;
	if (linelen && c[linelen-1] == '\r')
		--linelen;

	/* Discover which part we were last working on */
	workpart = part;
	while (zend_hash_num_elements(&workpart->children) > 0) {
		php_mimepart *lastpart, **tmppart;
		HashPosition pos;
		int bound_len;

		zend_hash_internal_pointer_end_ex(&workpart->children, &pos);
		zend_hash_get_current_data_ex(&workpart->children, (void**)&tmppart, &pos);
		lastpart = *tmppart;
		
		if (lastpart->parsedata.completed) {
			php_mimepart_update_positions(workpart, workpart->endpos + linelen, workpart->endpos + linelen, 1);
			return SUCCESS;
		}
		
		if (workpart->boundary == NULL || workpart->parsedata.in_header) {
			workpart = lastpart;
			continue;
		}

		bound_len = strlen(workpart->boundary);

		/* Look for a boundary */
		if (c[0] == '-' && c[1] == '-' && linelen >= 2+bound_len && strncasecmp(workpart->boundary, c+2, bound_len) == 0) {
			php_mimepart *newpart;

			/* is it the final boundary ? */
			if (linelen >= 4 + bound_len && strncmp(c+2+bound_len, "--", 2) == 0) {
				lastpart->parsedata.completed = 1;
				php_mimepart_update_positions(workpart, workpart->endpos + linelen, workpart->endpos + linelen, 1);
				return SUCCESS;
			}

			newpart = alloc_new_child_part(workpart, workpart->endpos + origcount, 1);
			php_mimepart_update_positions(workpart, workpart->endpos + linelen, workpart->endpos + linelen, 1);
			newpart->mime_version = estrdup(workpart->mime_version);
			newpart->parsedata.in_header = 1;
			return SUCCESS;
		}
		workpart = lastpart;
	}

	if (!workpart->parsedata.in_header) {
		size_t update_len = origcount;
		
		/* update the body/part end positions */
		if (workpart->parent && CONTENT_TYPE_ISL(workpart->parent, "multipart/", 10) == 0)
			update_len = linelen;

		if (!workpart->parsedata.completed)
			php_mimepart_update_positions(workpart, workpart->endpos + origcount, workpart->endpos + update_len, 1);
	} else {

		if (linelen > 0) {
		
			php_mimepart_update_positions(workpart, workpart->endpos + origcount, workpart->endpos + linelen, 1);
		
			if (isspace((int)(unsigned char)*c)) {
				smart_str_appendl(&workpart->parsedata.headerbuf, " ", 1);
			} else {
				php_mimepart_process_header(workpart TSRMLS_CC);
			}
			/* save header for possible continuation */
			smart_str_appendl(&workpart->parsedata.headerbuf, c, linelen);
			
		} else {
			/* end of headers */
			php_mimepart_process_header(workpart TSRMLS_CC);
			
			/* start of body */
			workpart->parsedata.in_header = 0;
			workpart->bodystart = workpart->endpos + origcount;
			php_mimepart_update_positions(workpart, workpart->bodystart, workpart->bodystart, 1);
			--workpart->nbodylines;

			/* some broken mailers include the content-type header but not a mime-version header.
			 * Let's relax and pretend they said they were mime 1.0 compatible */
			if (workpart->mime_version == NULL && workpart->content_type != NULL)
				workpart->mime_version = estrdup("1.0");
		
			if (!IS_MIME_1(workpart)) {
				/* if we don't understand the MIME version, discard the content-type and
				 * boundary */
				if (part->content_type)
					php_mimeheader_free(part->content_type);
				if (part->content_disposition)
					php_mimeheader_free(part->content_disposition);
				workpart->boundary = NULL;
				workpart->content_type = php_mimeheader_alloc("text/plain");
			}

			/* if there is no content type, default to text/plain, but use multipart/digest when in
			 * a multipart/rfc822 message */
			if (IS_MIME_1(workpart) && workpart->content_type == NULL) {
				char *def_type = "text/plain";

				if (workpart->parent && CONTENT_TYPE_IS(workpart->parent, "multipart/digest"))
					def_type = "message/rfc822";

				workpart->content_type = php_mimeheader_alloc(def_type);
			}

			/* if no charset had previously been set, either through inheritance or by an
			 * explicit content-type header, default to us-ascii */
			if (workpart->charset == NULL) {
				workpart->charset = estrdup(MAILPARSEG(def_charset));
			}
					
			if (CONTENT_TYPE_IS(workpart, "message/rfc822")) {
				workpart = alloc_new_child_part(workpart, workpart->bodystart, 0);
				workpart->parsedata.in_header = 1;
				return SUCCESS;
				
			}
		
			/* create a section for the preamble that precedes the first boundary */
			if (workpart->boundary) {
				workpart = alloc_new_child_part(workpart, workpart->bodystart, 1);
				workpart->parsedata.in_header = 0;
				workpart->parsedata.is_dummy = 1;
				return SUCCESS;
			}
			
			return SUCCESS;	
		}
		
	}
	
	return SUCCESS;
}

PHPAPI int php_mimepart_parse(php_mimepart *part, const char *buf, size_t bufsize TSRMLS_DC)
{
	size_t len;
		
	while(bufsize > 0) {
		/* look for EOL */
		for (len = 0; len < bufsize; len++)
			if (buf[len] == '\n')
				break;
		if (len < bufsize && buf[len] == '\n') {
			++len;
			smart_str_appendl(&part->parsedata.workbuf, buf, len);
			if (FAILURE == php_mimepart_process_line(part TSRMLS_CC))
				return FAILURE;
			
			part->parsedata.workbuf.len = 0;
		} else {
			smart_str_appendl(&part->parsedata.workbuf, buf, len);
		}

		buf += len;
		bufsize -= len;
	}
	return SUCCESS;
}

static int enum_parts_recurse(php_mimepart_enumerator *top, php_mimepart_enumerator **child,
		php_mimepart *part, mimepart_enumerator_func callback, void *ptr TSRMLS_DC)
{
	php_mimepart_enumerator next;
	php_mimepart **childpart;
	HashPosition pos;

	*child = NULL;
	if (FAILURE == (*callback)(part, top, ptr TSRMLS_CC))
		return FAILURE;
	
	*child = &next;
	next.id = 1;

	if (CONTENT_TYPE_ISL(part, "multipart/", 10))
		next.id = 0;

	zend_hash_internal_pointer_reset_ex(&part->children, &pos);
	while (SUCCESS == zend_hash_get_current_data_ex(&part->children, (void**)&childpart, &pos)) {
		if (next.id)
			if (FAILURE == enum_parts_recurse(top, &next.next, *childpart, callback, ptr TSRMLS_CC))
				return FAILURE;
		next.id++;
		zend_hash_move_forward_ex(&part->children, &pos);
	}
	return SUCCESS;
}

PHPAPI void php_mimepart_enum_parts(php_mimepart *part, mimepart_enumerator_func callback, void *ptr TSRMLS_DC)
{
	php_mimepart_enumerator top;
	top.id = 1;

	enum_parts_recurse(&top, &top.next, part, callback, ptr TSRMLS_CC);
}

PHPAPI void php_mimepart_enum_child_parts(php_mimepart *part, mimepart_child_enumerator_func callback, void *ptr TSRMLS_DC)
{
	HashPosition pos;
	php_mimepart **childpart;
	int index = 0;
	
	zend_hash_internal_pointer_reset_ex(&part->children, &pos);
	while (SUCCESS == zend_hash_get_current_data_ex(&part->children, (void**)&childpart, &pos)) {
		if (FAILURE == (*callback)(part, *childpart, index, ptr TSRMLS_CC))
			return;

		zend_hash_move_forward_ex(&part->children, &pos);
		index++;
	}
}

struct find_part_struct {
	const char *searchfor;
	php_mimepart *foundpart;
};

static int find_part_callback(php_mimepart *part, php_mimepart_enumerator *id, void *ptr TSRMLS_DC)
{
	struct find_part_struct *find = ptr;
	const unsigned char *num = (const unsigned char*)find->searchfor;
	unsigned int n;

	while (id)	{
		if (!isdigit((int)*num))
			return SUCCESS;
		/* convert from decimal to int */
		n = 0;
		while (isdigit((int)*num))
			n = (n * 10) + (*num++ - '0');
		if (*num)	{
			if (*num != '.')
				return SUCCESS;
			num++;
		}
		if (n != (unsigned int)id->id) {
			return SUCCESS;
		}
		id = id->next;
	}
	if (*num == 0)
		find->foundpart = part;
	
	return SUCCESS;
}

PHPAPI php_mimepart *php_mimepart_find_by_name(php_mimepart *parent, const char *name TSRMLS_DC)
{
	struct find_part_struct find = { name, NULL };
	php_mimepart_enum_parts(parent, find_part_callback, &find TSRMLS_CC);
	return find.foundpart;
}

PHPAPI php_mimepart *php_mimepart_find_child_by_position(php_mimepart *parent, int position TSRMLS_DC)
{
	HashPosition pos;
	php_mimepart **childpart;
	
	zend_hash_internal_pointer_reset_ex(&parent->children, &pos);
	while(position-- > 0)
		if (FAILURE == zend_hash_move_forward_ex(&parent->children, &pos))
			return NULL;
	
	if (FAILURE == zend_hash_get_current_data_ex(&parent->children, (void**)&childpart, &pos))
		return NULL;

	return *childpart;

}

static int filter_into_work_buffer(int c, void *dat TSRMLS_DC)
{
	php_mimepart *part = dat;

	smart_str_appendc(&part->parsedata.workbuf, c);

	if (part->parsedata.workbuf.len >= 4096) {
		
		part->extract_func(part, part->extract_context, part->parsedata.workbuf.c, part->parsedata.workbuf.len TSRMLS_CC);
		part->parsedata.workbuf.len = 0;
	}

	return c;
}

PHPAPI void php_mimepart_decoder_prepare(php_mimepart *part, int do_decode, php_mimepart_extract_func_t decoder, void *ptr TSRMLS_DC)
{
	enum mbfl_no_encoding from = mbfl_no_encoding_8bit;
	
	if (do_decode && part->content_transfer_encoding) {
		from = mbfl_name2no_encoding(part->content_transfer_encoding);
		if (from == mbfl_no_encoding_invalid) {
			zend_error(E_WARNING, "%s(): I don't know how to decode %s transfer encoding!",
					get_active_function_name(TSRMLS_C),
					part->content_transfer_encoding);
			from = mbfl_no_encoding_8bit;
		}
	}

	part->extract_func = decoder;
	part->extract_context = ptr;
	part->parsedata.workbuf.len = 0;
	
	if (do_decode) {
		if (from == mbfl_no_encoding_8bit || from == mbfl_no_encoding_7bit) {
			part->extract_filter = NULL;
		} else {
			part->extract_filter = mbfl_convert_filter_new(
					from, mbfl_no_encoding_8bit,
					filter_into_work_buffer,
					NULL,
					part
					TSRMLS_CC
					);
		}
	}
	
}

PHPAPI void php_mimepart_decoder_finish(php_mimepart *part TSRMLS_DC)
{
	if (part->extract_filter) {
		mbfl_convert_filter_flush(part->extract_filter TSRMLS_CC);
		mbfl_convert_filter_delete(part->extract_filter TSRMLS_CC);
	}
	if (part->extract_func && part->parsedata.workbuf.len > 0) {
		part->extract_func(part, part->extract_context, part->parsedata.workbuf.c, part->parsedata.workbuf.len TSRMLS_CC);
		part->parsedata.workbuf.len = 0;
	}
}

PHPAPI int php_mimepart_decoder_feed(php_mimepart *part, const char *buf, size_t bufsize TSRMLS_DC)
{
	if (buf && bufsize) {
		int i;

		if (part->extract_filter) {
			for (i = 0; i < bufsize; i++) {
				if (mbfl_convert_filter_feed(buf[i], part->extract_filter TSRMLS_CC) < 0) {
					zend_error(E_WARNING, "%s() - filter conversion failed. Input message is probably incorrectly encoded\n",
							get_active_function_name(TSRMLS_C));
					return -1;
				}
			}
		} else {
			return part->extract_func(part, part->extract_context, buf, bufsize TSRMLS_CC);
		}
	}
	return 0;
}

PHPAPI void php_mimepart_remove_from_parent(php_mimepart *part TSRMLS_DC)
{
	php_mimepart *parent = part->parent;
	HashPosition pos;
	php_mimepart **childpart;

	if (parent == NULL)
		return;

	part->parent = NULL;
	
	zend_hash_internal_pointer_reset_ex(&parent->children, &pos);
	while(SUCCESS == zend_hash_get_current_data_ex(&parent->children, (void**)&childpart, &pos)) {

		if (SUCCESS == zend_hash_get_current_data_ex(&parent->children, (void**)&childpart, &pos)) {
			if (*childpart == part) {
				ulong h;
				zend_hash_get_current_key_ex(&parent->children, NULL, NULL, &h, 0, &pos);
				zend_hash_index_del(&parent->children, h);
				break;
			}
		}
		zend_hash_move_forward_ex(&parent->children, &pos);
	}
}

PHPAPI void php_mimepart_add_child(php_mimepart *part, php_mimepart *child TSRMLS_DC)
{

}

