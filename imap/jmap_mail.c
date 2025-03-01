/* jmap_mail.c -- Routines for handling JMAP mail messages
 *
 * Copyright (c) 1994-2014 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <config.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <ctype.h>
#include <string.h>
#include <syslog.h>
#include <assert.h>
#include <sys/mman.h>

#include <sasl/saslutil.h>

#ifdef HAVE_LIBCHARDET
#include <chardet/chardet.h>
#endif

#include "acl.h"
#include "annotate.h"
#include "append.h"
#include "bsearch.h"
#include "carddav_db.h"
#include "hashset.h"
#include "http_dav.h"
#include "http_jmap.h"
#include "http_proxy.h"
#include "jmap_ical.h"
#include "jmap_mail.h"
#include "jmap_mail_query.h"
#include "json_support.h"
#include "mailbox.h"
#include "mappedfile.h"
#include "mboxevent.h"
#include "mboxlist.h"
#include "mboxname.h"
#include "msgrecord.h"
#include "notify.h"
#include "parseaddr.h"
#include "proxy.h"
#include "search_query.h"
#include "seen.h"
#include "smtpclient.h"
#include "statuscache.h"
#include "stristr.h"
#include "sync_log.h"
#include "times.h"
#include "util.h"
#include "xmalloc.h"
#include "xsha1.h"
#include "xstrnchr.h"


/* generated headers are not necessarily in current directory */
#include "imap/http_err.h"
#include "imap/imap_err.h"

static int jmap_email_query(jmap_req_t *req);
static int jmap_email_querychanges(jmap_req_t *req);
static int jmap_email_get(jmap_req_t *req);
static int jmap_email_set(jmap_req_t *req);
static int jmap_email_changes(jmap_req_t *req);
static int jmap_email_import(jmap_req_t *req);
static int jmap_email_parse(jmap_req_t *req);
static int jmap_email_copy(jmap_req_t *req);
static int jmap_email_matchmime_method(jmap_req_t *req);
static int jmap_searchsnippet_get(jmap_req_t *req);
static int jmap_thread_get(jmap_req_t *req);
static int jmap_thread_changes(jmap_req_t *req);
static int jmap_identity_get(jmap_req_t *req);

/*
 * Possibly to be implemented:
 * - Email/removeAttachments
 * - Email/report
 * - Identity/changes
 * - Identity/set
 */

static jmap_method_t jmap_mail_methods_standard[] = {
    {
        "Email/query",
        JMAP_URN_MAIL,
        &jmap_email_query,
        JMAP_SHARED_CSTATE
    },
    {
        "Email/queryChanges",
        JMAP_URN_MAIL,
        &jmap_email_querychanges,
        JMAP_SHARED_CSTATE
    },
    {
        "Email/get",
        JMAP_URN_MAIL,
        &jmap_email_get,
        JMAP_SHARED_CSTATE
    },
    {
        "Email/set",
        JMAP_URN_MAIL,
        &jmap_email_set,
        /*flags*/0
    },
    {
        "Email/changes",
        JMAP_URN_MAIL,
        &jmap_email_changes,
        JMAP_SHARED_CSTATE
    },
    {
        "Email/import",
        JMAP_URN_MAIL,
        &jmap_email_import,
        /*flags*/0
    },
    {
        "Email/parse",
        JMAP_URN_MAIL,
        &jmap_email_parse,
        JMAP_SHARED_CSTATE
    },
    {
        "Email/copy",
        JMAP_URN_MAIL,
        &jmap_email_copy,
        /*flags*/ 0
    },
    {
        "Email/matchMime",
        JMAP_MAIL_EXTENSION,
        &jmap_email_matchmime_method,
        JMAP_SHARED_CSTATE
    },
    {
        "SearchSnippet/get",
        JMAP_URN_MAIL,
        &jmap_searchsnippet_get,
        JMAP_SHARED_CSTATE
    },
    {
        "Thread/get",
        JMAP_URN_MAIL,
        &jmap_thread_get,
        JMAP_SHARED_CSTATE
    },
    {
        "Thread/changes",
        JMAP_URN_MAIL,
        &jmap_thread_changes,
        JMAP_SHARED_CSTATE
    },
    {
        "Identity/get",
        JMAP_URN_MAIL,
        &jmap_identity_get,
        JMAP_SHARED_CSTATE
    },
    { NULL, NULL, NULL, 0}
};

static jmap_method_t jmap_mail_methods_nonstandard[] = {
    { NULL, NULL, NULL, 0}
};

/* NULL terminated list of supported jmap_email_query sort fields */
struct email_sortfield {
    const char *name;
    const char *capability;
};

static struct email_sortfield email_sortfields[] = {
    {
        "receivedAt",
        NULL
    },
    {
        "sentAt",
        NULL
    },
    {
        "from",
        NULL
    },
    {
        "id",
        NULL
    },
    {
        "emailstate",
        NULL
    },
    {
        "size",
        NULL
    },
    {
        "subject",
        NULL
    },
    {
        "to",
        NULL
    },
    {
        "hasKeyword",
        NULL
    },
    {
        "someInThreadHaveKeyword",
        NULL
    },
    {
        "addedDates",
        JMAP_MAIL_EXTENSION
    },
    {
        "threadSize",
        JMAP_MAIL_EXTENSION
    },
    {
        "spamScore",
        JMAP_MAIL_EXTENSION
    },
    {
        "snoozedUntil",
        JMAP_MAIL_EXTENSION
    },
    {
        NULL,
        NULL
    }
};

#define JMAP_MAIL_MAX_MAILBOXES_PER_EMAIL 20
#define JMAP_MAIL_MAX_KEYWORDS_PER_EMAIL 100 /* defined in mailbox_user_flag */

HIDDEN void jmap_mail_init(jmap_settings_t *settings)
{
    jmap_method_t *mp;
    for (mp = jmap_mail_methods_standard; mp->name; mp++) {
        hash_insert(mp->name, mp, &settings->methods);
    }

    json_object_set_new(settings->server_capabilities,
            JMAP_URN_MAIL, json_object());

    if (config_getswitch(IMAPOPT_JMAP_NONSTANDARD_EXTENSIONS)) {
        json_object_set_new(settings->server_capabilities,
                JMAP_SEARCH_EXTENSION, json_object());
        json_object_set_new(settings->server_capabilities,
                JMAP_MAIL_EXTENSION, json_object());

        for (mp = jmap_mail_methods_nonstandard; mp->name; mp++) {
            hash_insert(mp->name, mp, &settings->methods);
        }
    }

    jmap_emailsubmission_init(settings);
    jmap_mailbox_init(settings);
    jmap_vacation_init(settings);
}

HIDDEN void jmap_mail_capabilities(json_t *account_capabilities, int mayCreateTopLevel)
{
    json_t *sortopts = json_array();
    struct email_sortfield *sp;
    int support_extensions = config_getswitch(IMAPOPT_JMAP_NONSTANDARD_EXTENSIONS);

    for (sp = email_sortfields; sp->name; sp++) {
        if (sp->capability && !support_extensions) continue;
        json_array_append_new(sortopts, json_string(sp->name));
    }

    long max_size_attachments_per_email =
        config_getint(IMAPOPT_JMAP_MAIL_MAX_SIZE_ATTACHMENTS_PER_EMAIL);

    max_size_attachments_per_email *= 1024;
    if (max_size_attachments_per_email <= 0) {
        syslog(LOG_ERR, "jmap: invalid property value: %s",
                imapopts[IMAPOPT_JMAP_MAIL_MAX_SIZE_ATTACHMENTS_PER_EMAIL].optname);
        max_size_attachments_per_email = 0;
    }

    json_t *email_capabilities = json_pack("{s:i s:i s:i s:o, s:b}",
            "maxMailboxesPerEmail", JMAP_MAIL_MAX_MAILBOXES_PER_EMAIL,
            "maxKeywordsPerEmail", JMAP_MAIL_MAX_KEYWORDS_PER_EMAIL,
            "maxSizeAttachmentsPerEmail", max_size_attachments_per_email,
            "emailsListSortOptions", sortopts,
            "mayCreateTopLevelMailbox", mayCreateTopLevel);

    json_object_set_new(account_capabilities, JMAP_URN_MAIL, email_capabilities);

    if (config_getswitch(IMAPOPT_JMAP_NONSTANDARD_EXTENSIONS)) {
        json_object_set_new(account_capabilities, JMAP_SEARCH_EXTENSION, json_object());
        json_object_set_new(account_capabilities, JMAP_MAIL_EXTENSION, json_object());
    }

    jmap_mailbox_capabilities(account_capabilities);
}

#define JMAP_HAS_ATTACHMENT_FLAG "$HasAttachment"

typedef enum MsgType {
        MSG_IS_ROOT = 0,
        MSG_IS_ATTACHED = 1,
} MsgType;

/*
 * Emails
 */

static char *_decode_to_utf8(const char *charset,
                             const char *data, size_t datalen,
                             const char *encoding,
                             int *is_encoding_problem)
{
    charset_t cs = charset_lookupname(charset);
    char *text = NULL;
    int enc = encoding_lookupname(encoding);

    if (cs == CHARSET_UNKNOWN_CHARSET || enc == ENCODING_UNKNOWN) {
        syslog(LOG_INFO, "decode_to_utf8 error (%s, %s)", charset, encoding);
        *is_encoding_problem = 1;
        goto done;
    }
    text = charset_to_utf8(data, datalen, cs, enc);
    if (!text) {
        *is_encoding_problem = 1;
        goto done;
    }

    size_t textlen = strlen(text);
    struct char_counts counts = charset_count_validutf8(text, textlen);
    *is_encoding_problem = counts.invalid || counts.replacement;

    const char *charset_id = charset_name(cs);
    if (!strncasecmp(charset_id, "UTF-32", 6)) {
        /* Special-handle UTF-32. Some clients announce the wrong endianess. */
        if (counts.invalid || counts.replacement) {
            charset_t guess_cs = CHARSET_UNKNOWN_CHARSET;
            if (!strcasecmp(charset_id, "UTF-32") || !strcasecmp(charset_id, "UTF-32BE"))
                guess_cs = charset_lookupname("UTF-32LE");
            else
                guess_cs = charset_lookupname("UTF-32BE");
            char *guess = charset_to_utf8(data, datalen, guess_cs, enc);
            if (guess) {
                struct char_counts guess_counts = charset_count_validutf8(guess, strlen(guess));
                if (guess_counts.valid > counts.valid) {
                    free(text);
                    text = guess;
                    counts = guess_counts;
                }
            }
            charset_free(&guess_cs);
        }
    }

#ifdef HAVE_LIBCHARDET
    if (counts.invalid || counts.replacement) {
        static Detect *d = NULL;
        if (!d) d = detect_init();

        DetectObj *obj = detect_obj_init();
        if (!obj) goto done;
        detect_reset(&d);

        struct buf buf = BUF_INITIALIZER;
        charset_decode(&buf, data, datalen, enc);
        buf_cstring(&buf);
        if (detect_handledata_r(&d, buf_base(&buf), buf_len(&buf), &obj) == CHARDET_SUCCESS) {
            charset_t guess_cs = charset_lookupname(obj->encoding);
            if (guess_cs != CHARSET_UNKNOWN_CHARSET) {
                char *guess = charset_to_utf8(data, datalen, guess_cs, enc);
                if (guess) {
                    struct char_counts guess_counts = charset_count_validutf8(guess, strlen(guess));
                    if (guess_counts.valid > counts.valid) {
                        free(text);
                        text = guess;
                        counts = guess_counts;
                    }
                    else {
                        free(guess);
                    }
                }
                charset_free(&guess_cs);
            }
        }
        detect_obj_free(&obj);
        buf_free(&buf);
    }
#endif

done:
    charset_free(&cs);
    return text;
}

static char *_decode_mimeheader(const char *raw)
{
    if (!raw) return NULL;

    int is_8bit = 0;
    const char *p;
    for (p = raw; *p; p++) {
        if (*p & 0x80) {
            is_8bit = 1;
            break;
        }
    }

    char *val = NULL;
    if (is_8bit) {
        int err = 0;
        val = _decode_to_utf8("utf-8", raw, strlen(raw), NULL, &err);
    }
    if (!val) {
        val = charset_decode_mimeheader(raw, CHARSET_KEEPCASE);
    }
    return val;
}

struct headers {
    json_t *raw; /* JSON array of EmailHeader */
    json_t *all; /* JSON object: lower-case header name => list of values */
    struct buf buf;
};

#define HEADERS_INITIALIZER \
    { json_array(), json_object(), BUF_INITIALIZER }

static void _headers_init(struct headers *headers) {
    headers->raw = json_array();
    headers->all = json_object();
    memset(&headers->buf, 0, sizeof(struct buf));
}

static void _headers_fini(struct headers *headers) {
    json_decref(headers->all);
    json_decref(headers->raw);
    buf_free(&headers->buf);
}

static void _headers_put_new(struct headers *headers, json_t *header, int shift)
{
    const char *name = json_string_value(json_object_get(header, "name"));

    if (headers->raw == NULL)
        headers->raw = json_array();
    if (headers->all == NULL)
        headers->all = json_object();

    /* Append (or shift) the raw header to the in-order header list */
    if (shift)
        json_array_insert(headers->raw, 0, header);
    else
        json_array_append(headers->raw, header);

    /* Append the raw header to the list of all equal-named headers */
    buf_setcstr(&headers->buf, name);
    const char *lcasename = buf_lcase(&headers->buf);
    json_t *all = json_object_get(headers->all, lcasename);
    if (!all) {
        all = json_array();
        json_object_set_new(headers->all, lcasename, all);
    }

    if (shift)
        json_array_insert_new(all, 0, header);
    else
        json_array_append_new(all, header);
}

static void _headers_add_new(struct headers *headers, json_t *header)
{
    if (!header) return;
    _headers_put_new(headers, header, 0);
}

static void _headers_shift_new(struct headers *headers, json_t *header)
{
    if (!header) return;
    _headers_put_new(headers, header, 1);
}

static json_t* _headers_get(struct headers *headers, const char *name)
{
    char *lcasename = lcase(xstrdup(name));
    json_t *jheader = json_object_get(headers->all, lcasename);
    free(lcasename);
    return jheader;
}

static int _headers_have(struct headers *headers, const char *name)
{
    return _headers_get(headers, name) != NULL;
}

static int _headers_from_mime_cb(const char *key, const char *val, void *_rock)
{
    struct headers *headers = _rock;
    _headers_add_new(headers, json_pack("{s:s s:s}", "name", key, "value", val));
    return 0;
}

static void _headers_from_mime(const char *base, size_t len, struct headers *headers)
{
    message_foreach_header(base, len, _headers_from_mime_cb, headers);
}

static json_t *_header_as_raw(const char *raw)
{
    if (!raw) return json_null();
    size_t len = strlen(raw);
    if (len > 1 && raw[len-1] == '\n' && raw[len-2] == '\r') len -= 2;
    return json_stringn(raw, len);
}

static json_t *_header_as_date(const char *raw)
{
    if (!raw) return json_null();

    time_t t;
    if (time_from_rfc5322(raw, &t, DATETIME_FULL) == -1) {
        if (!strchr(raw, '\r')) return json_null();
        char *tmp = charset_unfold(raw, strlen(raw), CHARSET_UNFOLD_SKIPWS);
        int r = time_from_rfc5322(tmp, &t, DATETIME_FULL);
        free(tmp);
        if (r == -1) return json_null();
    }

    char cbuf[RFC3339_DATETIME_MAX+1];
    cbuf[RFC3339_DATETIME_MAX] = '\0';
    time_to_rfc3339(t, cbuf, RFC3339_DATETIME_MAX+1);
    return json_string(cbuf);
}

static json_t *_header_as_text(const char *raw)
{
    if (!raw) return json_null();

    /* TODO this could be optimised to omit unfolding, decoding
     * or normalisation, or all, if ASCII */
    /* Unfold and remove CRLF */
    char *unfolded = charset_unfold(raw, strlen(raw), 0);
    char *p = strchr(unfolded, '\r');
    while (p && *(p + 1) != '\n') {
        p = strchr(p + 1, '\r');
    }
    if (p) *p = '\0';

    /* Trim starting SP */
    const char *trimmed = unfolded;
    while (isspace(*trimmed)) {
        trimmed++;
    }

    /* Decode header */
    char *decoded = _decode_mimeheader(trimmed);

    /* Convert to Unicode NFC */
    char *normalized = charset_utf8_normalize(decoded);

    json_t *result = json_string(normalized);
    free(normalized);
    free(decoded);
    free(unfolded);
    return result;
}

static void _remove_ws(char *s)
{
    char *d = s;
    do {
        while (isspace(*s))
            s++;
    } while ((*d++ = *s++));
}

static json_t *_header_as_messageids(const char *raw)
{
    if (!raw) return json_null();
    json_t *msgids = json_array();
    char *unfolded = charset_unfold(raw, strlen(raw), CHARSET_UNFOLD_SKIPWS);

    const char *p = unfolded;

    while (*p) {
        /* Skip preamble */
        while (isspace(*p) || *p == ',') p++;
        if (!*p) break;

        /* Find end of id */
        const char *q = p;
        if (*p == '<') {
            while (*q && *q != '>') q++;
        }
        else {
            while (*q && !isspace(*q)) q++;
        }

        /* Read id */
        char *val = xstrndup(*p == '<' ? p + 1 : p,
                             *q == '>' ? q - p - 1 : q - p);
        if (*p == '<') {
            _remove_ws(val);
        }
        if (*val) {
            /* calculate the value that would be created if this was
             * fed back into an Email/set and make sure it would
             * validate */
            char *msgid = strconcat("<", val, ">", NULL);
            int r = conversations_check_msgid(msgid, strlen(msgid));
            if (!r) json_array_append_new(msgids, json_string(val));
            free(msgid);
        }
        free(val);

        /* Reset iterator */
        p = *q ? q + 1 : q;
    }


    if (!json_array_size(msgids)) {
        json_decref(msgids);
        msgids = json_null();
    }
    free(unfolded);
    return msgids;
}

static json_t *_emailaddresses_from_addr(struct address *addr)
{
    if (!addr) return json_null();

    json_t *addresses = json_array();
    struct buf buf = BUF_INITIALIZER;

    while (addr) {
        json_t *e = json_pack("{}");

        const char *domain = addr->domain;
        if (!strcmpsafe(domain, "unspecified-domain")) {
            domain = NULL;
        }

        if (!addr->name && addr->mailbox && !domain) {
            /* That's a group */
            json_object_set_new(e, "name", json_string(addr->mailbox));
            json_object_set_new(e, "email", json_null());
            json_array_append_new(addresses, e);
            addr = addr->next;
            continue;
        }

        /* name */
        if (addr->name) {
            char *tmp = _decode_mimeheader(addr->name);
            if (tmp) json_object_set_new(e, "name", json_string(tmp));
            free(tmp);
        } else {
            json_object_set_new(e, "name", json_null());
        }

        /* email */
        if (addr->mailbox) {
            buf_setcstr(&buf, addr->mailbox);
            if (domain) {
                buf_putc(&buf, '@');
                buf_appendcstr(&buf, domain);
            }
            json_object_set_new(e, "email", json_string(buf_cstring(&buf)));
            buf_reset(&buf);
        } else {
            json_object_set_new(e, "email", json_null());
        }
        json_array_append_new(addresses, e);
        addr = addr->next;
    }

    if (!json_array_size(addresses)) {
        json_decref(addresses);
        addresses = json_null();
    }
    buf_free(&buf);
    return addresses;
}


static json_t *_header_as_addresses(const char *raw)
{
    if (!raw) return json_null();

    struct address *addrs = NULL;
    parseaddr_list(raw, &addrs);
    json_t *result = _emailaddresses_from_addr(addrs);
    parseaddr_free(addrs);
    return result;
}

static json_t *_header_as_urls(const char *raw)
{
    if (!raw) return json_null();

    /* A poor man's implementation of RFC 2369, returning anything
     * between < and >. */
    json_t *urls = json_array();
    const char *base = raw;
    const char *top = raw + strlen(raw);
    while (base < top) {
        const char *lo = strchr(base, '<');
        if (!lo) break;
        const char *hi = strchr(lo, '>');
        if (!hi) break;
        char *tmp = charset_unfold(lo + 1, hi - lo - 1, CHARSET_UNFOLD_SKIPWS);
        _remove_ws(tmp);
        if (*tmp) json_array_append_new(urls, json_string(tmp));
        free(tmp);
        base = hi + 1;
    }
    if (!json_array_size(urls)) {
        json_decref(urls);
        urls = json_null();
    }
    return urls;
}

enum _header_form {
    HEADER_FORM_UNKNOWN = 0, /* MUST be zero so we can cast to void* */
    HEADER_FORM_RAW,
    HEADER_FORM_TEXT,
    HEADER_FORM_ADDRESSES,
    HEADER_FORM_MESSAGEIDS,
    HEADER_FORM_DATE,
    HEADER_FORM_URLS
};

struct header_prop {
    char *lcasename;
    char *name;
    const char *prop;
    enum _header_form form;
    int all;
};

static void _header_prop_fini(struct header_prop *prop)
{
    free(prop->lcasename);
    free(prop->name);
}

static void _header_prop_free(struct header_prop *prop)
{
    _header_prop_fini(prop);
    free(prop);
}

static struct header_prop *_header_parseprop(const char *s)
{
    strarray_t *fields = strarray_split(s + 7, ":", 0);
    const char *f0, *f1, *f2;
    int is_valid = 1;
    enum _header_form form = HEADER_FORM_RAW;
    char *lcasename = NULL, *name = NULL;

    /* Initialize allowed header forms by lower-case header name. Any
     * header in this map is allowed to be requested either as Raw
     * or the form of the map value (casted to void* because C...).
     * Any header not found in this map is allowed to be requested
     * in any form. */
    static hash_table allowed_header_forms = HASH_TABLE_INITIALIZER;
    if (allowed_header_forms.size == 0) {
        /* TODO initialize with all headers in RFC5322 and RFC2369 */
        construct_hash_table(&allowed_header_forms, 32, 0);
        hash_insert("bcc", (void*) HEADER_FORM_ADDRESSES, &allowed_header_forms);
        hash_insert("cc", (void*) HEADER_FORM_ADDRESSES, &allowed_header_forms);
        hash_insert("content-type", (void*) HEADER_FORM_RAW, &allowed_header_forms);
        hash_insert("comment", (void*) HEADER_FORM_TEXT, &allowed_header_forms);
        hash_insert("date", (void*) HEADER_FORM_DATE, &allowed_header_forms);
        hash_insert("from", (void*) HEADER_FORM_ADDRESSES, &allowed_header_forms);
        hash_insert("in-reply-to", (void*) HEADER_FORM_MESSAGEIDS, &allowed_header_forms);
        hash_insert("list-archive", (void*) HEADER_FORM_URLS, &allowed_header_forms);
        hash_insert("list-help", (void*) HEADER_FORM_URLS, &allowed_header_forms);
        hash_insert("list-owner", (void*) HEADER_FORM_URLS, &allowed_header_forms);
        hash_insert("list-post", (void*) HEADER_FORM_URLS, &allowed_header_forms);
        hash_insert("list-subscribe", (void*) HEADER_FORM_URLS, &allowed_header_forms);
        hash_insert("list-unsubscribe", (void*) HEADER_FORM_URLS, &allowed_header_forms);
        hash_insert("message-id", (void*) HEADER_FORM_MESSAGEIDS, &allowed_header_forms);
        hash_insert("references", (void*) HEADER_FORM_MESSAGEIDS, &allowed_header_forms);
        hash_insert("reply-to", (void*) HEADER_FORM_ADDRESSES, &allowed_header_forms);
        hash_insert("resent-date", (void*) HEADER_FORM_DATE, &allowed_header_forms);
        hash_insert("resent-from", (void*) HEADER_FORM_ADDRESSES, &allowed_header_forms);
        hash_insert("resent-message-id", (void*) HEADER_FORM_MESSAGEIDS, &allowed_header_forms);
        hash_insert("resent-reply-to", (void*) HEADER_FORM_ADDRESSES, &allowed_header_forms);
        hash_insert("resent-sender", (void*) HEADER_FORM_ADDRESSES, &allowed_header_forms);
        hash_insert("resent-to", (void*) HEADER_FORM_ADDRESSES, &allowed_header_forms);
        hash_insert("resent-cc", (void*) HEADER_FORM_ADDRESSES, &allowed_header_forms);
        hash_insert("resent-bcc", (void*) HEADER_FORM_ADDRESSES, &allowed_header_forms);
        hash_insert("sender", (void*) HEADER_FORM_ADDRESSES, &allowed_header_forms);
        hash_insert("subject", (void*) HEADER_FORM_TEXT, &allowed_header_forms);
        hash_insert("to", (void*) HEADER_FORM_ADDRESSES, &allowed_header_forms);
    }

    /* Parse property string into fields */
    f0 = f1 = f2 = NULL;
    switch (fields->count) {
        case 3:
            f2 = strarray_nth(fields, 2);
            /* fallthrough */
        case 2:
            f1 = strarray_nth(fields, 1);
            /* fallthrough */
        case 1:
            f0 = strarray_nth(fields, 0);
            lcasename = lcase(xstrdup(f0));
            name = xstrdup(f0);
            break;
        default:
            strarray_free(fields);
            return NULL;
    }

    if (f2 && (strcmp(f2, "all") || !strcmp(f1, "all"))) {
        strarray_free(fields);
        free(lcasename);
        free(name);
        return NULL;
    }
    if (f1) {
        if (!strcmp(f1, "asRaw"))
            form = HEADER_FORM_RAW;
        else if (!strcmp(f1, "asText"))
            form = HEADER_FORM_TEXT;
        else if (!strcmp(f1, "asAddresses"))
            form = HEADER_FORM_ADDRESSES;
        else if (!strcmp(f1, "asMessageIds"))
            form = HEADER_FORM_MESSAGEIDS;
        else if (!strcmp(f1, "asDate"))
            form = HEADER_FORM_DATE;
        else if (!strcmp(f1, "asURLs"))
            form = HEADER_FORM_URLS;
        else if (strcmp(f1, "all"))
            is_valid = 0;
    }

    /* Validate requested header form */
    if (is_valid && form != HEADER_FORM_RAW) {
        enum _header_form allowed_form = (enum _header_form) \
                                         hash_lookup(lcasename, &allowed_header_forms);
        if (allowed_form != HEADER_FORM_UNKNOWN && form != allowed_form) {
            is_valid = 0;
        }
    }

    struct header_prop *hprop = NULL;
    if (is_valid) {
        hprop = xzmalloc(sizeof(struct header_prop));
        hprop->lcasename = lcasename;
        hprop->name = name;
        hprop->prop = s;
        hprop->form = form;
        hprop->all = f2 != NULL || (f1 && !strcmp(f1, "all"));
    }
    else {
        free(lcasename);
        free(name);
    }
    strarray_free(fields);
    return hprop;
}

/* Generate a preview of text of at most len bytes, excluding the zero
 * byte.
 *
 * Consecutive whitespaces, including newlines, are collapsed to a single
 * blank. If text is longer than len and len is greater than 4, then return
 * a string  ending in '...' and holding as many complete UTF-8 characters,
 * that the total byte count of non-zero characters is at most len.
 *
 * The input string must be properly encoded UTF-8 */
static char *_email_extract_preview(const char *text, size_t len)
{
    unsigned char *dst, *d, *t;
    size_t n;

    if (!text) {
        return NULL;
    }

    /* Replace all whitespace with single blanks. */
    dst = (unsigned char *) xzmalloc(len+1);
    for (t = (unsigned char *) text, d = dst; *t && d < (dst+len); ++t, ++d) {
        *d = isspace(*t) ? ' ' : *t;
        if (isspace(*t)) {
            while(isspace(*++t))
                ;
            --t;
        }
    }
    n = d - dst;

    /* Anything left to do? */
    if (n < len || len <= 4) {
        return (char*) dst;
    }

    /* Append trailing ellipsis. */
    dst[--n] = '.';
    dst[--n] = '.';
    dst[--n] = '.';
    while (n && (dst[n] & 0xc0) == 0x80) {
        dst[n+2] = 0;
        dst[--n] = '.';
    }
    if (dst[n] >= 0x80) {
        dst[n+2] = 0;
        dst[--n] = '.';
    }
    return (char *) dst;
}

struct _email_mailboxes_rock {
    jmap_req_t *req;
    json_t *mboxs;
};

static int _email_mailboxes_cb(const conv_guidrec_t *rec, void *rock)
{
    struct _email_mailboxes_rock *data = (struct _email_mailboxes_rock*) rock;
    json_t *mboxs = data->mboxs;
    jmap_req_t *req = data->req;
    struct mailbox *mbox = NULL;
    msgrecord_t *mr = NULL;
    uint32_t system_flags, internal_flags;
    int r;

    if (rec->part) return 0;

    static int needrights = ACL_READ|ACL_LOOKUP;
    if (!jmap_hasrights_byname(req, rec->mboxname, needrights))
        return 0;

    r = jmap_openmbox(req, rec->mboxname, &mbox, 0);
    if (r) return r;

    // we only want regular mailboxes!
    if (mbox->mbtype & MBTYPES_NONIMAP) goto done;

    r = msgrecord_find(mbox, rec->uid, &mr);
    if (r) goto done;

    r = msgrecord_get_systemflags(mr, &system_flags);
    if (r) goto done;

    r = msgrecord_get_internalflags(mr, &internal_flags);
    if (r) goto done;

    if (!r) {
        char datestr[RFC3339_DATETIME_MAX];
        time_t t;
        int exists = 1;

        if (system_flags & FLAG_DELETED || internal_flags & FLAG_INTERNAL_EXPUNGED) {
            exists = 0;
            r = msgrecord_get_lastupdated(mr, &t);
        }
        else {
            r = msgrecord_get_savedate(mr, &t);
        }

        if (r) goto done;
        time_to_rfc3339(t, datestr, RFC3339_DATETIME_MAX);

        json_t *mboxdata = json_object_get(mboxs, mbox->uniqueid);
        if (!mboxdata) {
            mboxdata = json_object();
            json_object_set_new(mboxs, mbox->uniqueid, mboxdata);
        }

        if (exists) {
            json_t *prev = json_object_get(mboxdata, "added");
            if (prev) {
                const char *val = json_string_value(prev);
                // we want the FIRST date it was added to the mailbox, so skip if this is newer
                if (strcmp(datestr, val) >= 0) goto done;
            }

            json_object_set_new(mboxdata, "added", json_string(datestr));
        }
        else {
            json_t *prev = json_object_get(mboxdata, "removed");
            if (prev) {
                const char *val = json_string_value(prev);
                // we want the LAST date it was removed from the mailbox, so skip if this is older
                if (strcmp(datestr, val) <= 0) goto done;
            }

            json_object_set_new(mboxdata, "removed", json_string(datestr));
        }
    }


done:
    if (mr) msgrecord_unref(&mr);
    jmap_closembox(req, &mbox);
    return r;
}

static char *_emailbodies_to_plain(struct emailbodies *bodies, const struct buf *msg_buf)
{
    if (bodies->textlist.count == 1) {
        int is_encoding_problem = 0;
        struct body *textbody = ptrarray_nth(&bodies->textlist, 0);
        char *text = _decode_to_utf8(textbody->charset_id,
                                     msg_buf->s + textbody->content_offset,
                                     textbody->content_size,
                                     textbody->encoding,
                                     &is_encoding_problem);
        return text;
    }

    /* Concatenate all plain text bodies and replace any
     * inlined images with placeholders. */
    int i;
    struct buf buf = BUF_INITIALIZER;
    for (i = 0; i < bodies->textlist.count; i++) {
        struct body *part = ptrarray_nth(&bodies->textlist, i);

        if (i) buf_appendcstr(&buf, "\n");

        if (!strcmp(part->type, "TEXT")) {
            int is_encoding_problem = 0;
            char *t = _decode_to_utf8(part->charset_id,
                                      msg_buf->s + part->content_offset,
                                      part->content_size,
                                      part->encoding,
                                      &is_encoding_problem);
            if (t) buf_appendcstr(&buf, t);
            free(t);
        }
        else if (!strcmp(part->type, "IMAGE")) {
            struct param *param;
            const char *fname = NULL;
            for (param = part->disposition_params; param; param = param->next) {
                if (!strncasecmp(param->attribute, "filename", 8)) {
                    fname =param->value;
                    break;
                }
            }
            buf_appendcstr(&buf, "[Inline image");
            if (fname) {
                buf_appendcstr(&buf, ":");
                buf_appendcstr(&buf, fname);
            }
            buf_appendcstr(&buf, "]");
        }
    }
    return buf_release(&buf);
}

/* Replace any <HTML> and </HTML> tags in t with <DIV> and </DIV>,
 * writing results into buf */
static void _html_concat_div(struct buf *buf, const char *t)
{
    const char *top = t + strlen(t);
    const char *p = t, *q = p;

    while (*q) {
        const char *tag = NULL;
        if (q < top - 5 && !strncasecmp(q, "<html", 5) &&
                (*(q+5) == '>' || isspace(*(q+5)))) {
            /* Found a <HTML> tag */
            tag = "<div>";
        }
        else if (q < top - 6 && !strncasecmp(q, "</html", 6) &&
                (*(q+6) == '>' || isspace(*(q+6)))) {
            /* Found a </HTML> tag */
            tag = "</div>";
        }

        /* No special tag? */
        if (!tag) {
            q++;
            continue;
        }

        /* Append whatever we saw since the last HTML tag. */
        buf_appendmap(buf, p, q - p);

        /* Look for the end of the tag and replace it, even if
         * it prematurely ends at the end of the buffer . */
        while (*q && *q != '>') { q++; }
        buf_appendcstr(buf, tag);
        if (*q) q++;

        /* Prepare for next loop */
        p = q;
    }
    buf_appendmap(buf, p, q - p);
}


static char *_emailbodies_to_html(struct emailbodies *bodies, const struct buf *msg_buf)
{
    if (bodies->htmllist.count == 1) {
        const struct body *part = ptrarray_nth(&bodies->htmllist, 0);
        int is_encoding_problem = 0;
        char *html = _decode_to_utf8(part->charset_id,
                                     msg_buf->s + part->content_offset,
                                     part->content_size,
                                     part->encoding,
                                     &is_encoding_problem);
        return html;
    }

    /* Concatenate all TEXT bodies, enclosing PLAIN text
     * in <div> and replacing <html> tags in HTML bodies
     * with <div>. */
    int i;
    struct buf buf = BUF_INITIALIZER;
    for (i = 0; i < bodies->htmllist.count; i++) {
        struct body *part = ptrarray_nth(&bodies->htmllist, i);

        /* XXX htmllist might include inlined images but we
         * currently ignore them. After all, there should
         * already be an <img> tag for their Content-Id
         * header value. If this turns out to be not enough,
         * we can insert the <img> tags here. */
        if (strcasecmp(part->type, "TEXT")) {
            continue;
        }

        if (!i)
            buf_appendcstr(&buf, "<html>"); // XXX use HTML5?

        int is_encoding_problem = 0;
        char *t = _decode_to_utf8(part->charset_id,
                                  msg_buf->s + part->content_offset,
                                  part->content_size,
                                  part->encoding,
                                  &is_encoding_problem);
        if (t && !strcmp(part->subtype, "HTML")) {
            _html_concat_div(&buf, t);
        }
        else if (t) {
            buf_appendcstr(&buf, "<div>");
            buf_appendcstr(&buf, t);
            buf_appendcstr(&buf, "</div>");
        }
        free(t);

        if (i == bodies->htmllist.count - 1)
            buf_appendcstr(&buf, "</html>");
    }
    return buf_release(&buf);
}

static void _html_to_plain_cb(const struct buf *buf, void *rock)
{
    struct buf *dst = (struct buf*) rock;
    const char *p;
    int seenspace = 0;

    /* Just merge multiple space into one. That's similar to
     * charset_extract's MERGE_SPACE but since we don't want
     * it to canonify the text into search form */
    for (p = buf_base(buf); p < buf_base(buf) + buf_len(buf) && *p; p++) {
        if (*p == ' ') {
            if (seenspace) continue;
            seenspace = 1;
        } else {
            seenspace = 0;
        }
        buf_appendmap(dst, p, 1);
    }
}

static char *_html_to_plain(const char *html) {
    struct buf src = BUF_INITIALIZER;
    struct buf dst = BUF_INITIALIZER;
    charset_t utf8 = charset_lookupname("utf8");
    char *text;
    char *tmp, *q;
    const char *p;

    /* Replace <br> and <p> with newlines */
    q = tmp = xstrdup(html);
    p = html;
    while (*p) {
        if (!strncmp(p, "<br>", 4) || !strncmp(p, "</p>", 4)) {
            *q++ = '\n';
            p += 4;
        }
        else if (!strncmp(p, "p>", 3)) {
            p += 3;
        } else {
            *q++ = *p++;
        }
    }
    *q = 0;

    /* Strip html tags */
    buf_init_ro(&src, tmp, q - tmp);
    buf_setcstr(&dst, "");
    charset_extract(&_html_to_plain_cb, &dst,
            &src, utf8, ENCODING_NONE, "HTML", CHARSET_KEEPCASE);
    buf_cstring(&dst);

    /* Trim text */
    buf_trim(&dst);
    text = buf_releasenull(&dst);
    if (!strlen(text)) {
        free(text);
        text = NULL;
    }

    buf_free(&src);
    free(tmp);
    charset_free(&utf8);

    return text;
}

static const char *_guid_from_id(const char *msgid)
{
    return msgid + 1;
}

static conversation_id_t _cid_from_id(const char *thrid)
{
    conversation_id_t cid = 0;
    if (thrid[0] == 'T')
        conversation_id_decode(&cid, thrid+1);
    return cid;
}

/*
 * Lookup all mailboxes where msgid is contained in.
 *
 * The return value is a JSON object keyed by the mailbox unique id,
 * and its mailbox name as value.
 */
static json_t *_email_mailboxes(jmap_req_t *req, const char *msgid)
{
    struct _email_mailboxes_rock data = { req, json_pack("{}") };
    conversations_guid_foreach(req->cstate, _guid_from_id(msgid), _email_mailboxes_cb, &data);
    return data.mboxs;
}



static void _email_read_annot(const jmap_req_t *req, msgrecord_t *mr,
                              const char *annot, struct buf *buf)
{
    if (!strncmp(annot, "/shared/", 8)) {
        msgrecord_annot_lookup(mr, annot+7, /*userid*/"", buf);
    }
    else if (!strncmp(annot, "/private/", 9)) {
        msgrecord_annot_lookup(mr, annot+7, req->userid, buf);
    }
    else {
        msgrecord_annot_lookup(mr, annot, "", buf);
    }
}

static json_t *_email_read_jannot(const jmap_req_t *req, msgrecord_t *mr,
                                  const char *annot, int structured)
{
    struct buf buf = BUF_INITIALIZER;
    json_t *annotvalue = NULL;

    _email_read_annot(req, mr, annot, &buf);

    if (buf_len(&buf)) {
        if (structured) {
            json_error_t jerr;
            annotvalue = json_loads(buf_cstring(&buf), JSON_DECODE_ANY, &jerr);
            /* XXX - log error? */
        }
        else {
            annotvalue = json_string(buf_cstring(&buf));
        }

        if (!annotvalue) {
            syslog(LOG_ERR, "jmap: annotation %s has bogus value", annot);
        }
    }
    buf_free(&buf);
    return annotvalue;
}


struct _email_find_rock {
    jmap_req_t *req;
    char *mboxname;
    uint32_t uid;
};

static int _email_find_cb(const conv_guidrec_t *rec, void *rock)
{
    struct _email_find_rock *d = (struct _email_find_rock*) rock;
    jmap_req_t *req = d->req;

    if (rec->part) return 0;

    /* Make sure we are allowed to read this mailbox */
    if (!jmap_hasrights_byname(req, rec->mboxname, ACL_READ))
        return 0;

    int r = 0;
    struct mailbox *mbox = NULL;
    msgrecord_t *mr = NULL;
    uint32_t system_flags;
    uint32_t internal_flags;

    r = jmap_openmbox(req, rec->mboxname, &mbox, 0);
    if (r) {
        // we want to keep looking and see if we can find a mailbox we can open
        syslog(LOG_ERR, "IOERROR: email_find_cb failed to open %s: %s",
               rec->mboxname, error_message(r));
        goto done;
    }

    r = msgrecord_find(mbox, rec->uid, &mr);
    if (!r) r = msgrecord_get_systemflags(mr, &system_flags);
    if (!r) r = msgrecord_get_internalflags(mr, &internal_flags);
    if (r) {
        // we want to keep looking and see if we can find a message we can read
        syslog(LOG_ERR, "IOERROR: email_find_cb failed to find message %u in mailbox %s: %s",
               rec->uid, rec->mboxname, error_message(r));
        goto done;
    }

    // if it's deleted, skip
    if ((system_flags & FLAG_DELETED) || (internal_flags & FLAG_INTERNAL_EXPUNGED))
        goto done;

    d->mboxname = xstrdup(rec->mboxname);
    d->uid = rec->uid;

done:
    jmap_closembox(req, &mbox);
    msgrecord_unref(&mr);
    return d->mboxname ? IMAP_OK_COMPLETED : 0;
}

static int _email_find_in_account(jmap_req_t *req,
                                  const char *account_id,
                                  const char *email_id,
                                  char **mboxnameptr,
                                  uint32_t *uidptr)
{
    struct _email_find_rock rock = { req, NULL, 0 };
    int r;

    /* must be prefixed with 'M' */
    if (email_id[0] != 'M')
        return IMAP_NOTFOUND;
    /* this is on a 24 character prefix only */
    if (strlen(email_id) != 25)
        return IMAP_NOTFOUND;
    /* Open conversation state, if not already open */
    struct conversations_state *mycstate = NULL;
    if (strcmp(req->accountid, account_id)) {
        r = conversations_open_user(account_id, 1/*shared*/, &mycstate);
        if (r) return r;
    }
    else {
        mycstate = req->cstate;
    }
    r = conversations_guid_foreach(mycstate, _guid_from_id(email_id),
                                   _email_find_cb, &rock);
    if (mycstate != req->cstate) {
        conversations_commit(&mycstate);
    }
    /* Set return values */
    if (r == IMAP_OK_COMPLETED)
        r = 0;
    else if (!rock.mboxname)
        r = IMAP_NOTFOUND;
    *mboxnameptr = rock.mboxname;
    *uidptr = rock.uid;
    return r;
}

HIDDEN int jmap_email_find(jmap_req_t *req,
                           const char *email_id,
                           char **mboxnameptr,
                           uint32_t *uidptr)
{
    return _email_find_in_account(req, req->accountid, email_id, mboxnameptr, uidptr);
}

struct email_getcid_rock {
    jmap_req_t *req;
    int checkacl;
    conversation_id_t cid;
};

static int _email_get_cid_cb(const conv_guidrec_t *rec, void *rock)
{
    struct email_getcid_rock *d = (struct email_getcid_rock *)rock;
    if (rec->part) return 0;
    if (!rec->cid) return 0;
    /* Make sure we are allowed to read this mailbox */
    if (d->checkacl && !jmap_hasrights_byname(d->req, rec->mboxname, ACL_READ))
            return 0;
    d->cid = rec->cid;
    return IMAP_OK_COMPLETED;
}

static int _email_get_cid(jmap_req_t *req, const char *msgid,
                           conversation_id_t *cidp)
{
    int r;

    /* must be prefixed with 'M' */
    if (msgid[0] != 'M')
        return IMAP_NOTFOUND;
    /* this is on a 24 character prefix only */
    if (strlen(msgid) != 25)
        return IMAP_NOTFOUND;

    int checkacl = strcmp(req->userid, req->accountid);
    struct email_getcid_rock rock = { req, checkacl, 0 };
    r = conversations_guid_foreach(req->cstate, _guid_from_id(msgid), _email_get_cid_cb, &rock);
    if (r == IMAP_OK_COMPLETED) {
        *cidp = rock.cid;
        r = 0;
    }
    return r;
}

struct email_expunge_check {
    jmap_req_t *req;
    modseq_t since_modseq;
    int status;
};

static int _email_is_expunged_cb(const conv_guidrec_t *rec, void *rock)
{
    struct email_expunge_check *check = rock;
    msgrecord_t *mr = NULL;
    struct mailbox *mbox = NULL;
    uint32_t flags;
    int r = 0;

    if (rec->part) return 0;

    r = jmap_openmbox(check->req, rec->mboxname, &mbox, 0);
    if (r) return r;

    r = msgrecord_find(mbox, rec->uid, &mr);
    if (!r) {
        uint32_t internal_flags;
        modseq_t createdmodseq;
        r = msgrecord_get_systemflags(mr, &flags);
        if (!r) msgrecord_get_internalflags(mr, &internal_flags);
        if (!r) msgrecord_get_createdmodseq(mr, &createdmodseq);
        if (!r) {
            /* OK, this is a legit record, let's check it out */
            if (createdmodseq <= check->since_modseq)
                check->status |= 1;  /* contains old messages */
            if (!((flags & FLAG_DELETED) || (internal_flags & FLAG_INTERNAL_EXPUNGED)))
                check->status |= 2;  /* contains alive messages */
        }
        msgrecord_unref(&mr);
    }

    jmap_closembox(check->req, &mbox);
    return 0;
}

static void _email_search_perf_attr(const search_attr_t *attr, strarray_t *perf_filters)
{
    const char *cost = NULL;

    switch (search_attr_cost(attr)) {
        case SEARCH_COST_INDEX:
            cost = "index";
            break;
        case SEARCH_COST_CONV:
            cost = "conversations";
            break;
        case SEARCH_COST_ANNOT:
            cost = "annotations";
            break;
        case SEARCH_COST_CACHE:
            cost = "cache";
            break;
        case SEARCH_COST_BODY:
            cost = search_attr_is_fuzzable(attr) ? "xapian" : "body";
            break;
        default:
            ; // ignore
    }

    if (cost) strarray_add(perf_filters, cost);
}

static void _email_search_string(search_expr_t *parent,
                                 const char *s,
                                 const char *name,
                                 strarray_t *perf_filters)
{
    charset_t utf8 = charset_lookupname("utf-8");
    search_expr_t *e;
    const search_attr_t *attr = search_attr_find(name);
    enum search_op op;

    assert(attr);

    op = search_attr_is_fuzzable(attr) ? SEOP_FUZZYMATCH : SEOP_MATCH;

    e = search_expr_new(parent, op);
    e->attr = attr;
    e->value.s = charset_convert(s, utf8, charset_flags);
    if (!e->value.s) {
        e->op = SEOP_FALSE;
        e->attr = NULL;
    }

    _email_search_perf_attr(attr, perf_filters);
    charset_free(&utf8);
}

static void _email_search_type(search_expr_t *parent, const char *s, strarray_t *perf_filters)
{
    strarray_t types = STRARRAY_INITIALIZER;

    /* Handle type wildcards */
    // XXX: due to Xapian's 64 character indexing limitation, we're not prefixing application_
    // to the Microsoft types
    if (!strcasecmp(s, "image")) {
        strarray_append(&types, "image_gif");
        strarray_append(&types, "image_jpeg");
        strarray_append(&types, "image_pjpeg");
        strarray_append(&types, "image_jpg");
        strarray_append(&types, "image_png");
        strarray_append(&types, "image_bmp");
        strarray_append(&types, "image_tiff");
    }
    else if (!strcasecmp(s, "document")) {
        strarray_append(&types, "application_msword");
        strarray_append(&types, "vnd.openxmlformats-officedocument.wordprocessingml.document");
        strarray_append(&types, "vnd.openxmlformats-officedocument.wordprocessingml.template");
        strarray_append(&types, "application_vnd.sun.xml.writer");
        strarray_append(&types, "application_vnd.sun.xml.writer.template");
        strarray_append(&types, "application_vnd.oasis.opendocument.text");
        strarray_append(&types, "application_vnd.oasis.opendocument.text-template");
        strarray_append(&types, "application_x-iwork-pages-sffpages");
        strarray_append(&types, "application_vnd.apple.pages");
    }
    else if (!strcasecmp(s, "spreadsheet")) {
        strarray_append(&types, "application_vnd.ms-excel");
        strarray_append(&types, "vnd.openxmlformats-officedocument.spreadsheetml.sheet");
        strarray_append(&types, "vnd.openxmlformats-officedocument.spreadsheetml.template");
        strarray_append(&types, "application_vnd.sun.xml.calc");
        strarray_append(&types, "application_vnd.sun.xml.calc.template");
        strarray_append(&types, "application_vnd.oasis.opendocument.spreadsheet");
        strarray_append(&types, "application_vnd.oasis.opendocument.spreadsheet-template");
        strarray_append(&types, "application_x-iwork-numbers-sffnumbers");
        strarray_append(&types, "application_vnd.apple.numbers");
    }
    else if (!strcasecmp(s, "presentation")) {
        strarray_append(&types, "application_vnd.ms-powerpoint");
        strarray_append(&types, "vnd.openxmlformats-officedocument.presentationml.presentation");
        strarray_append(&types, "vnd.openxmlformats-officedocument.presentationml.template");
        strarray_append(&types, "vnd.openxmlformats-officedocument.presentationml.slideshow");
        strarray_append(&types, "application_vnd.sun.xml.impress");
        strarray_append(&types, "application_vnd.sun.xml.impress.template");
        strarray_append(&types, "application_vnd.oasis.opendocument.presentation");
        strarray_append(&types, "application_vnd.oasis.opendocument.presentation-template");
        strarray_append(&types, "application_x-iwork-keynote-sffkey");
        strarray_append(&types, "application_vnd.apple.keynote");
    }
    else if (!strcasecmp(s, "email")) {
        strarray_append(&types, "message_rfc822");
    }
    else if (!strcasecmp(s, "pdf")) {
        strarray_append(&types, "application_pdf");
    }
    else {
        /* FUZZY contenttype is indexed as `type_subtype` */
        char *tmp = xstrdup(s);
        char *p = strchr(tmp, '/');
        if (p) *p = '_';
        strarray_append(&types, tmp);
        free(tmp);
    }

    /* Build expression */
    search_expr_t *p = (types.count > 1) ? search_expr_new(parent, SEOP_OR) : parent;
    const search_attr_t *attr = search_attr_find("contenttype");
    do {
        search_expr_t *e = search_expr_new(p, SEOP_FUZZYMATCH);
        e->attr = attr;
        struct buf buf = BUF_INITIALIZER;
        char *orig = strarray_pop(&types);
        const unsigned char *s = (const unsigned char *)orig;
        for ( ; *s ; ++s) {
            if (Uisalnum(*s) || *s == '_')
                buf_putc(&buf, *s);
        }
        e->value.s = buf_release(&buf);
        free(orig);
        buf_free(&buf);
    } while (types.count);
    _email_search_perf_attr(attr, perf_filters);

    strarray_fini(&types);
}

static void _email_search_keyword(search_expr_t *parent, const char *keyword, strarray_t *perf_filters)
{
    search_expr_t *e;
    if (!strcasecmp(keyword, "$Seen")) {
        e = search_expr_new(parent, SEOP_MATCH);
        e->attr = search_attr_find("indexflags");
        e->value.u = MESSAGE_SEEN;
    }
    else if (!strcasecmp(keyword, "$Draft")) {
        e = search_expr_new(parent, SEOP_MATCH);
        e->attr = search_attr_find("systemflags");
        e->value.u = FLAG_DRAFT;
    }
    else if (!strcasecmp(keyword, "$Flagged")) {
        e = search_expr_new(parent, SEOP_MATCH);
        e->attr = search_attr_find("systemflags");
        e->value.u = FLAG_FLAGGED;
    }
    else if (!strcasecmp(keyword, "$Answered")) {
        e = search_expr_new(parent, SEOP_MATCH);
        e->attr = search_attr_find("systemflags");
        e->value.u = FLAG_ANSWERED;
    }
    else {
        e = search_expr_new(parent, SEOP_MATCH);
        e->attr = search_attr_find("keyword");
        e->value.s = xstrdup(keyword);
    }
    _email_search_perf_attr(e->attr, perf_filters);
}

static void _email_search_threadkeyword(search_expr_t *parent, const char *keyword,
                                        int matchall, strarray_t *perf_filters)
{
    const char *flag = jmap_keyword_to_imap(keyword);
    if (!flag) return;

    search_expr_t *e = search_expr_new(parent, SEOP_MATCH);
    e->attr = search_attr_find(matchall ? "allconvflags" : "convflags");
    e->value.s = xstrdup(flag);
    _email_search_perf_attr(e->attr, perf_filters);
}

static void _email_search_contactgroup(search_expr_t *parent,
                                       const char *groupid,
                                       const char *attrname,
                                       hash_table *contactgroups,
                                       strarray_t *perf_filters)
{
    if (!contactgroups || !contactgroups->size) return;

    strarray_t *members = hash_lookup(groupid, contactgroups);
    if (members && strarray_size(members)) {
        search_expr_t *e = search_expr_new(parent, SEOP_OR);
        int j;
        for (j = 0; j < strarray_size(members); j++) {
            _email_search_string(e, strarray_nth(members, j),
                    attrname, perf_filters);
        }
    }
}

/* ====================================================================== */

static void _emailsearch_folders_internalise(struct index_state *state,
                                             const union search_value *v,
                                             void **internalisedp)
{
    if (state && v) {
        *internalisedp = mailbox_get_cstate(state->mailbox);
    }
}

struct jmap_search_folder_match_rock {
    const strarray_t *folders;
    intptr_t is_otherthan;
};

static int _emailsearch_folders_match_cb(const conv_guidrec_t *rec, void *rock)
{
    if ((rec->system_flags & FLAG_DELETED) ||
        (rec->internal_flags & FLAG_INTERNAL_EXPUNGED)) return 0;

    // TODO we could match for mboxid, once the mailbox-id patch lands
    struct jmap_search_folder_match_rock *myrock = rock;
    int pos = strarray_find(myrock->folders, rec->mboxname, 0);
    return ((pos >= 0) == (myrock->is_otherthan == 0)) ? IMAP_OK_COMPLETED : 0;
}

static int _emailsearch_folders_match(message_t *m, const union search_value *v,
                                      void *internalised,
                                      void *data1)
{
    struct conversations_state *cstate = internalised;
    if (!cstate) return 0;
    const struct message_guid *guid = NULL;
    int r = message_get_guid(m, &guid);
    if (r) return 0;
    struct jmap_search_folder_match_rock rock = { v->rock, (intptr_t) data1 };
    r = conversations_guid_foreach(cstate, message_guid_encode(guid),
                                   _emailsearch_folders_match_cb, &rock);
    return r == IMAP_OK_COMPLETED;
}

static void _emailsearch_folders_serialise(struct buf *buf,
                                           const union search_value *v)
{
    char *tmp = strarray_join((strarray_t*)v->rock, " ");
    buf_putc(buf, '(');
    buf_appendcstr(buf, tmp);
    buf_putc(buf, ')');
    free(tmp);
}

static int _emailsearch_folders_unserialise(struct protstream* prot,
                                            union search_value *v)
{
    struct dlist *dl = NULL;

    int c = dlist_parse_asatomlist(&dl, 0, prot);
    if (c == EOF) return EOF;

    strarray_t *folders = strarray_new();
    struct buf tmp = BUF_INITIALIZER;
    struct dlist_print_iter *iter = dlist_print_iter_new(dl, /*printkeys*/ 0);
    while (iter && dlist_print_iter_step(iter, &tmp)) {
        if (buf_len(&tmp)) strarray_append(folders, buf_cstring(&tmp));
        buf_reset(&tmp);
    }
    dlist_print_iter_free(&iter);
    buf_free(&tmp);
    v->rock = folders;
    return c;
}

static void _emailsearch_folders_duplicate(union search_value *new,
                                           const union search_value *old)
{
    new->rock = strarray_dup((strarray_t*)old->rock);
}

static void _emailsearch_folders_free(union search_value *v)
{
    strarray_free(v->rock);
}

static const search_attr_t _emailsearch_folders_attr = {
    "jmap_folders",
    SEA_MUTABLE,
    SEARCH_PART_NONE,
    SEARCH_COST_CONV,
    _emailsearch_folders_internalise,
    /*cmp*/NULL,
    _emailsearch_folders_match,
    _emailsearch_folders_serialise,
    _emailsearch_folders_unserialise,
    /*get_countability*/NULL,
    _emailsearch_folders_duplicate,
    _emailsearch_folders_free,
    (void*)0 /*is_otherthan*/
};

static const search_attr_t _emailsearch_folders_otherthan_attr = {
    "jmap_folders_otherthan",
    SEA_MUTABLE,
    SEARCH_PART_NONE,
    SEARCH_COST_CONV,
    _emailsearch_folders_internalise,
    /*cmp*/NULL,
    _emailsearch_folders_match,
    _emailsearch_folders_serialise,
    _emailsearch_folders_unserialise,
    /*get_countability*/NULL,
    _emailsearch_folders_duplicate,
    _emailsearch_folders_free,
    (void*)1 /*is_otherthan*/
};


/* ====================================================================== */

static search_expr_t *_email_buildsearchexpr(jmap_req_t *req, json_t *filter,
                                             search_expr_t *parent,
                                             hash_table *contactgroups,
                                             strarray_t *perf_filters)
{
    search_expr_t *this, *e;
    json_t *val;
    const char *s;
    size_t i;
    time_t t;

    if (!JNOTNULL(filter)) {
        return search_expr_new(parent, SEOP_TRUE);
    }

    if ((s = json_string_value(json_object_get(filter, "operator")))) {
        enum search_op op = SEOP_UNKNOWN;

        if (!strcmp("AND", s)) {
            op = SEOP_AND;
        } else if (!strcmp("OR", s)) {
            op = SEOP_OR;
        } else if (!strcmp("NOT", s)) {
            op = SEOP_NOT;
        }

        this = search_expr_new(parent, op);
        e = op == SEOP_NOT ? search_expr_new(this, SEOP_OR) : this;

        json_array_foreach(json_object_get(filter, "conditions"), i, val) {
            _email_buildsearchexpr(req, val, e, contactgroups, perf_filters);
        }
    } else {
        this = search_expr_new(parent, SEOP_AND);

        /* zero properties evaluate to true */
        search_expr_new(this, SEOP_TRUE);

        if ((s = json_string_value(json_object_get(filter, "after")))) {
            time_from_iso8601(s, &t);
            e = search_expr_new(this, SEOP_GE);
            e->attr = search_attr_find("internaldate");
            e->value.u = t;
            _email_search_perf_attr(e->attr, perf_filters);
        }
        if ((s = json_string_value(json_object_get(filter, "before")))) {
            time_from_iso8601(s, &t);
            e = search_expr_new(this, SEOP_LE);
            e->attr = search_attr_find("internaldate");
            e->value.u = t;
            _email_search_perf_attr(e->attr, perf_filters);
        }
        if ((s = json_string_value(json_object_get(filter, "body")))) {
            _email_search_string(this, s, "body", perf_filters);
        }
        if ((s = json_string_value(json_object_get(filter, "cc")))) {
            _email_search_string(this, s, "cc", perf_filters);
        }
        if ((s = json_string_value(json_object_get(filter, "bcc")))) {
            _email_search_string(this, s, "bcc", perf_filters);
        }
        if ((s = json_string_value(json_object_get(filter, "from")))) {
            _email_search_string(this, s, "from", perf_filters);
        }
        if ((s = json_string_value(json_object_get(filter, "fromContactGroupId")))) {
            _email_search_contactgroup(this, s, "from", contactgroups, perf_filters);
        }
        if ((s = json_string_value(json_object_get(filter, "toContactGroupId")))) {
            _email_search_contactgroup(this, s, "to", contactgroups, perf_filters);
        }
        if ((s = json_string_value(json_object_get(filter, "ccContactGroupId")))) {
            _email_search_contactgroup(this, s, "cc", contactgroups, perf_filters);
        }
        if ((s = json_string_value(json_object_get(filter, "bccContactGroupId")))) {
            _email_search_contactgroup(this, s, "bcc", contactgroups, perf_filters);
        }
        if (JNOTNULL((val = json_object_get(filter, "hasAttachment")))) {
            e = val == json_false() ? search_expr_new(this, SEOP_NOT) : this;
            e = search_expr_new(e, SEOP_MATCH);
            e->attr = search_attr_find("keyword");
            e->value.s = xstrdup(JMAP_HAS_ATTACHMENT_FLAG);
            _email_search_perf_attr(e->attr, perf_filters);
        }
        if ((s = json_string_value(json_object_get(filter, "attachmentName")))) {
            _email_search_string(this, s, "attachmentname", perf_filters);
        }
        if ((s = json_string_value(json_object_get(filter, "attachmentType")))) {
            _email_search_type(this, s, perf_filters);
        }
        if (JNOTNULL((val = json_object_get(filter, "header")))) {
            const char *k, *v;
            charset_t utf8 = charset_lookupname("utf-8");
            search_expr_t *e;

            if (json_array_size(val) == 2) {
                k = json_string_value(json_array_get(val, 0));
                v = json_string_value(json_array_get(val, 1));
            } else {
                k = json_string_value(json_array_get(val, 0));
                v = ""; /* Empty string matches any value */
            }

            e = search_expr_new(this, SEOP_MATCH);
            e->attr = search_attr_find_field(k);
            e->value.s = charset_convert(v, utf8, charset_flags);
            if (!e->value.s) {
                e->op = SEOP_FALSE;
                e->attr = NULL;
            }
            _email_search_perf_attr(e->attr, perf_filters);
            charset_free(&utf8);
        }
        if ((val = json_object_get(filter, "inMailbox"))) {
            strarray_t *folders = strarray_new();
            const char *mboxid = json_string_value(val);
            const mbentry_t *mbentry = jmap_mbentry_by_uniqueid(req, mboxid);
            if (mbentry && jmap_hasrights(req, mbentry, ACL_LOOKUP)) {
                strarray_append(folders, mbentry->name);
            }
            if (strarray_size(folders)) {
                search_expr_t *e = search_expr_new(this, SEOP_MATCH);
                e->attr = &_emailsearch_folders_attr;
                e->value.rock = folders;
                strarray_add(perf_filters, "mailbox");
            }
        }

        if ((val = json_object_get(filter, "inMailboxOtherThan"))) {
            strarray_t *folders = strarray_new();
            json_t *jmboxid;
            json_array_foreach(val, i, jmboxid) {
                const char *mboxid = json_string_value(jmboxid);
                const mbentry_t *mbentry = jmap_mbentry_by_uniqueid(req, mboxid);
                if (mbentry && jmap_hasrights(req, mbentry, ACL_LOOKUP)) {
                    strarray_append(folders, mbentry->name);
                }
            }
            if (strarray_size(folders)) {
                search_expr_t *e = search_expr_new(this, SEOP_MATCH);
                e->attr = &_emailsearch_folders_otherthan_attr;
                e->value.rock = folders;
                strarray_add(perf_filters, "mailbox");
            }
        }

        if (JNOTNULL((val = json_object_get(filter, "allInThreadHaveKeyword")))) {
            /* This shouldn't happen, validate_sort should have reported
             * allInThreadHaveKeyword as unsupported. Let's ignore this
             * filter and return false positives. */
            _email_search_threadkeyword(this, json_string_value(val), 1, perf_filters);
        }
        if (JNOTNULL((val = json_object_get(filter, "someInThreadHaveKeyword")))) {
            _email_search_threadkeyword(this, json_string_value(val), 0, perf_filters);
        }
        if (JNOTNULL((val = json_object_get(filter, "noneInThreadHaveKeyword")))) {
            e = search_expr_new(this, SEOP_NOT);
            _email_search_threadkeyword(e, json_string_value(val), 0, perf_filters);
        }

        if (JNOTNULL((val = json_object_get(filter, "hasKeyword")))) {
            _email_search_keyword(this, json_string_value(val), perf_filters);
        }
        if (JNOTNULL((val = json_object_get(filter, "notKeyword")))) {
            e = search_expr_new(this, SEOP_NOT);
            _email_search_keyword(e, json_string_value(val), perf_filters);
        }

        if (JNOTNULL((val = json_object_get(filter, "maxSize")))) {
            e = search_expr_new(this, SEOP_LE);
            e->attr = search_attr_find("size");
            e->value.u = json_integer_value(val);
            _email_search_perf_attr(e->attr, perf_filters);
        }
        if (JNOTNULL((val = json_object_get(filter, "minSize")))) {
            e = search_expr_new(this, SEOP_GE);
            e->attr = search_attr_find("size");
            e->value.u = json_integer_value(val);
            _email_search_perf_attr(e->attr, perf_filters);
        }
        if ((s = json_string_value(json_object_get(filter, "sinceEmailState")))) {
            /* non-standard */
            e = search_expr_new(this, SEOP_GT);
            e->attr = search_attr_find("modseq");
            e->value.u = atomodseq_t(s);
            _email_search_perf_attr(e->attr, perf_filters);
        }
        if ((s = json_string_value(json_object_get(filter, "subject")))) {
            _email_search_string(this, s, "subject", perf_filters);
        }
        if ((s = json_string_value(json_object_get(filter, "text")))) {
            _email_search_string(this, s, "text", perf_filters);
        }
        if ((s = json_string_value(json_object_get(filter, "attachmentBody")))) {
            _email_search_string(this, s, "attachmentbody", perf_filters);
        }
        if ((s = json_string_value(json_object_get(filter, "to")))) {
            _email_search_string(this, s, "to", perf_filters);
        }
    }

    return this;
}

static search_expr_t *_email_buildsearch(jmap_req_t *req, json_t *filter,
                                         hash_table *contactgroups,
                                         strarray_t *perf_filters)
{
    search_expr_t *root = _email_buildsearchexpr(req, filter, /*parent*/NULL,
                                                 contactgroups, perf_filters);

    /* The search API internally optimises for IMAP folder queries
     * and we'd like to benefit from this also for JMAP. To do so,
     * we try to convert as many inMailboxId expressions to IMAP
     * mailbox matches as possible. This includes the first
     * inMailboxId that is part of a positive AND and any inMailboxId
     * that is part of a positive OR expression. */
    ptrarray_t todo = PTRARRAY_INITIALIZER;
    int found_and_match = 0;
    struct work {
        search_expr_t *e;
        int in_or;
    };
    struct work *w = xmalloc(sizeof(struct work));
    w->e = root;
    w->in_or = 0;
    ptrarray_push(&todo, w);
    while ((w = ptrarray_pop(&todo))) {
        if (w->e->op == SEOP_MATCH) {
            if (!strcmp(w->e->attr->name, "jmap_folders") &&
                w->e->attr->data1 == 0 &&
                strarray_size((strarray_t*)w->e->value.rock) == 1) {
                /* Its's an inMailboxId expression */
                if (w->in_or || !found_and_match) {
                    char *folder = strarray_pop((strarray_t*)w->e->value.rock);
                    _emailsearch_folders_free(&w->e->value);
                    const search_attr_t *attr = search_attr_find("folder");
                    w->e->value.s = folder;
                    w->e->attr = attr;
                    found_and_match = !w->in_or;
                }
            }
        }
        else if (w->e->op == SEOP_AND || w->e->op == SEOP_OR) {
            search_expr_t *c;
            for (c = w->e->children; c; c = c->next) {
                struct work *ww = xmalloc(sizeof(struct work));
                ww->e = c;
                ww->in_or = w->in_or || w->e->op == SEOP_OR;
                ptrarray_push(&todo, ww);
            }
        }
        free(w);
    }
    ptrarray_fini(&todo);

    return root;
}

static void _email_contactfilter_initreq(jmap_req_t *req, struct email_contactfilter *cfilter)
{
    const char *addressbookid = json_string_value(json_object_get(req->args, "addressbookId"));
    jmap_email_contactfilter_init(req->accountid, addressbookid, cfilter);
}

static void _email_parse_filter_cb(jmap_req_t *req,
                                   struct jmap_parser *parser,
                                   json_t *filter,
                                   json_t *unsupported,
                                   void *rock,
                                   json_t **err)
{
    struct email_contactfilter *cfilter = rock;

    /* Parse filter */
    jmap_email_filtercondition_parse(parser, filter, unsupported,
                                     req->using_capabilities);
    if (json_array_size(parser->invalid)) return;

    /* Gather contactgroups */
    int r = jmap_email_contactfilter_from_filtercondition(parser, filter, cfilter);
    if (r) {
        *err = jmap_server_error(r);
        return;
    }

    const char *field;
    json_t *arg;

    /* Validate permissions */
    json_object_foreach(filter, field, arg) {
        if (!strcmp(field, "inMailbox")) {
            if (json_is_string(arg)) {
                const mbentry_t *mbentry = jmap_mbentry_by_uniqueid(req, json_string_value(arg));
                if (!mbentry || !jmap_hasrights(req, mbentry, ACL_LOOKUP)) {
                    jmap_parser_invalid(parser, field);
                }
            }
        }
        else if (!strcmp(field, "inMailboxOtherThan")) {
            if (json_is_array(arg)) {
                size_t i;
                json_t *val;
                json_array_foreach(arg, i, val) {
                    const char *s = json_string_value(val);
                    int is_valid = 0;
                    if (s) {
                        const mbentry_t *mbentry = jmap_mbentry_by_uniqueid(req, s);
                        is_valid = mbentry && jmap_hasrights(req, mbentry, ACL_LOOKUP);
                    }
                    if (!is_valid) {
                        jmap_parser_push_index(parser, field, i, s);
                        jmap_parser_invalid(parser, NULL);
                        jmap_parser_pop(parser);
                    }
                }
            }
        }
    }
}

static struct sortcrit *_email_buildsort(json_t *sort, int *sort_savedate)
{
    json_t *jcomp;
    size_t i;
    struct sortcrit *sortcrit;

    if (!JNOTNULL(sort) || json_array_size(sort) == 0) {
        sortcrit = xzmalloc(2 * sizeof(struct sortcrit));
        sortcrit[0].flags |= SORT_REVERSE;
        sortcrit[0].key = SORT_ARRIVAL;
        sortcrit[1].flags |= SORT_REVERSE;
        sortcrit[1].key = SORT_SEQUENCE;
        return sortcrit;
    }

    sortcrit = xzmalloc((json_array_size(sort) + 1) * sizeof(struct sortcrit));

    json_array_foreach(sort, i, jcomp) {
        const char *prop = json_string_value(json_object_get(jcomp, "property"));

        if (json_object_get(jcomp, "isAscending") == json_false()) {
            sortcrit[i].flags |= SORT_REVERSE;
        }

        /* Note: add any new sort criteria also to is_supported_email_sort */

        if (!strcmp(prop, "receivedAt")) {
            sortcrit[i].key = SORT_ARRIVAL;
        }
        if (!strcmp(prop, "sentAt")) {
            sortcrit[i].key = SORT_DATE;
        }
        if (!strcmp(prop, "from")) {
            sortcrit[i].key = SORT_DISPLAYFROM;
        }
        if (!strcmp(prop, "id")) {
            sortcrit[i].key = SORT_GUID;
        }
        if (!strcmp(prop, "emailState")) {
            sortcrit[i].key = SORT_MODSEQ;
        }
        if (!strcmp(prop, "size")) {
            sortcrit[i].key = SORT_SIZE;
        }
        if (!strcmp(prop, "subject")) {
            sortcrit[i].key = SORT_SUBJECT;
        }
        if (!strcmp(prop, "to")) {
            sortcrit[i].key = SORT_DISPLAYTO;
        }
        if (!strcmp(prop, "hasKeyword")) {
            const char *name = json_string_value(json_object_get(jcomp, "keyword"));
            const char *flagname = jmap_keyword_to_imap(name);
            if (flagname) {
                sortcrit[i].key = SORT_HASFLAG;
                sortcrit[i].args.flag.name = xstrdup(flagname);
            }
        }
        if (!strcmp(prop, "someInThreadHaveKeyword")) {
            const char *name = json_string_value(json_object_get(jcomp, "keyword"));
            const char *flagname = jmap_keyword_to_imap(name);
            if (flagname) {
                sortcrit[i].key = SORT_HASCONVFLAG;
                sortcrit[i].args.flag.name = xstrdup(flagname);
            }
        }
        // FM specific
        if (!strcmp(prop, "addedDates") || !strcmp(prop, "snoozedUntil")) {
            const char *mboxid =
                json_string_value(json_object_get(jcomp, "mailboxId"));

            if (sort_savedate) *sort_savedate = 1;
            sortcrit[i].key = (*prop == 's') ? SORT_SNOOZEDUNTIL : SORT_SAVEDATE;
            sortcrit[i].args.mailbox.id = xstrdupnull(mboxid);
        }
        if (!strcmp(prop, "threadSize")) {
            sortcrit[i].key = SORT_CONVSIZE;
        }
        if (!strcmp(prop, "spamScore")) {
            sortcrit[i].key = SORT_SPAMSCORE;
        }
    }

    i = json_array_size(sort);
    sortcrit[i].key = SORT_SEQUENCE;

    return sortcrit;
}

static void _email_querychanges_added(struct jmap_querychanges *query,
                                      const char *email_id)
{
    json_t *item = json_pack("{s:s,s:i}", "id", email_id, "index", query->total-1);
    json_array_append_new(query->added, item);
}

static void _email_querychanges_destroyed(struct jmap_querychanges *query,
                                          const char *email_id)
{
    json_array_append_new(query->removed, json_string(email_id));
}

struct emailsearch {
    int is_mutable;
    char *hash;
    strarray_t perf_filters;
    /* Internal state */
    search_query_t *query;
    struct searchargs *args;
    struct index_state *state;
    struct sortcrit *sortcrit;
    struct index_init init;
    ptrarray_t *cached_msgdata;
};

static void _emailsearch_free(struct emailsearch *search)
{
    if (!search) return;

    index_close(&search->state);
    search_query_free(search->query);
    freesearchargs(search->args);
    freesortcrit(search->sortcrit);
    free(search->hash);
    strarray_fini(&search->perf_filters);
    free(search);
}

static char *_emailsearch_hash(struct emailsearch *search)
{
    struct buf buf = BUF_INITIALIZER;
    if (search->args->root) {
        search_expr_t *mysearch = search_expr_duplicate(search->args->root);
        search_expr_normalise(&mysearch);
        char *tmp = search_expr_serialise(mysearch);
        buf_appendcstr(&buf, tmp);
        free(tmp);
        search_expr_free(mysearch);
    }
    else {
        buf_appendcstr(&buf, "noquery");
    }
    if (search->query->sortcrit) {
        char *tmp = sortcrit_as_string(search->query->sortcrit);
        buf_appendcstr(&buf, tmp);
        free(tmp);
    }
    else {
        buf_appendcstr(&buf, "nosort");
    }
    unsigned char raw_sha1[SHA1_DIGEST_LENGTH];
    xsha1((const unsigned char *) buf_base(&buf), buf_len(&buf), raw_sha1);
    size_t hex_size = (SHA1_DIGEST_LENGTH << 1);
    char hex_sha1[hex_size + 1];
    bin_to_lchex(raw_sha1, SHA1_DIGEST_LENGTH, hex_sha1);
    hex_sha1[hex_size] = '\0';
    buf_free(&buf);
    return xstrdup(hex_sha1);
}

#define FNAME_EMAILSEARCH_DB "/jmap_emailsearch.db"
#define EMAILSEARCH_DB "twoskip"

static char *emailsearch_getcachepath(void)
{
    return xstrdupnull(config_getstring(IMAPOPT_JMAP_EMAILSEARCH_DB_PATH));
}

static int _jmap_checkfolder(const char *mboxname, void *rock)
{
    jmap_req_t *req = (jmap_req_t *)rock;

    // we only want to look in folders that the user is allowed to read
    if (jmap_hasrights_byname(req, mboxname, ACL_READ))
        return 1;

    return 0;
}

static struct emailsearch* _emailsearch_new(jmap_req_t *req,
                                            json_t *filter,
                                            json_t *sort,
                                            hash_table *contactgroups,
                                            int want_expunged,
                                            int ignore_timer,
                                            int *sort_savedate)
{
    struct emailsearch* search = xzmalloc(sizeof(struct emailsearch));
    int r = 0;

    /* Build search args */
    search->args = new_searchargs(NULL/*tag*/, GETSEARCH_CHARSET_FIRST,
            &jmap_namespace, req->accountid, req->authstate, 0);
    search->args->root = _email_buildsearch(req, filter, contactgroups, &search->perf_filters);

    /* Build index state */
    search->init.userid = req->accountid;
    search->init.authstate = req->authstate;
    search->init.want_expunged = want_expunged;
    search->init.examine_mode = 1;

    char *inboxname = mboxname_user_mbox(req->accountid, NULL);
    r = index_open(inboxname, &search->init, &search->state);
    free(inboxname);
    if (r) {
        syslog(LOG_ERR, "jmap: _emailsearch_new: %s", error_message(r));
        freesearchargs(search->args);
        free(search);
        return NULL;
    }

    /* Build query */
    search->query = search_query_new(search->state, search->args);
    search->query->sortcrit =
        search->sortcrit = _email_buildsort(sort, sort_savedate);
    search->query->multiple = 1;
    search->query->need_ids = 1;
    search->query->verbose = 0;
    search->query->want_expunged = want_expunged;
    search->query->ignore_timer = ignore_timer;
    search->query->checkfolder = _jmap_checkfolder;
    search->query->checkfolderrock = req;
    search->query->attachments_in_any = jmap_is_using(req, JMAP_SEARCH_EXTENSION);
    search->is_mutable = search_is_mutable(search->sortcrit, search->args);

    /* Make hash */
    search->hash = _emailsearch_hash(search);

    return search;
}

static int _emailsearch_run(struct emailsearch *search, const ptrarray_t **msgdataptr)
{
    int r = search_query_run(search->query);
    if (r) {
        syslog(LOG_ERR, "jmap: _emailsearch_run: %s", error_message(r));
        return r;
    }
    *msgdataptr = &search->query->merged_msgdata;
    return 0;
}

static int _email_parse_comparator(jmap_req_t *req,
                                   struct jmap_comparator *comp,
                                   void *rock __attribute__((unused)),
                                   json_t **err __attribute__((unused)))
{
    /* Reject any collation */
    if (comp->collation) {
        return 0;
    }

    /* Search in list of supported sortFields */
    struct email_sortfield *sp = email_sortfields;
    for (sp = email_sortfields; sp->name; sp++) {
        if (!strcmp(sp->name, comp->property)) {
            return !sp->capability || jmap_is_using(req, sp->capability);
        }
    }

    return 0;
}

static char *_email_make_querystate(modseq_t modseq, uint32_t uid, modseq_t addrbook_modseq)
{
    struct buf buf = BUF_INITIALIZER;
    buf_printf(&buf, MODSEQ_FMT ":%u", modseq, uid);
    if (addrbook_modseq) {
        buf_printf(&buf, ",addrbook:" MODSEQ_FMT, addrbook_modseq);
    }
    return buf_release(&buf);
}

static int _email_read_querystate(const char *s, modseq_t *modseq, uint32_t *uid,
                                  modseq_t *addrbook_modseq)
{
    char sentinel = 0;

    /* Parse mailbox modseq and uid */
    int n = sscanf(s, MODSEQ_FMT ":%u%c", modseq, uid, &sentinel);
    if (n <= 2) return n == 2;
    else if (sentinel != ',') return 0;

    /* Parse addrbook modseq */
    s = strchr(s, ',') + 1;
    if (strncmp(s, "addrbook:", 9)) return 0;
    s += 9;
    n = sscanf(s, MODSEQ_FMT "%c", addrbook_modseq, &sentinel);
    if (n != 1) return 0;

    /* Parsed successfully */
    return 1;
}

struct cached_emailquery {
    char *ids;         /* zero-terminated id strings */
    size_t ids_count;  /* count of ids in ids array */
    size_t id_size;    /* byte-length of an id, excluding 0 byte */
};

#define _CACHED_EMAILQUERY_INITIALIZER { NULL, 0, 0 }

static void _cached_emailquery_fini(struct cached_emailquery *cache_record)
{
    free(cache_record->ids);
}

#define _EMAILSEARCH_CACHE_VERSION 0x2

static int _email_query_writecache(struct db *cache_db,
                                   const char *cache_key,
                                   modseq_t current_modseq,
                                   strarray_t *email_ids)
{
    int r = 0;

    /* Serialise cache record preamble */
    struct buf buf = BUF_INITIALIZER;
    buf_appendbit32(&buf, _EMAILSEARCH_CACHE_VERSION);
    buf_appendbit64(&buf, current_modseq);
    /* Serialise email ids */
    buf_appendbit64(&buf, strarray_size(email_ids));
    if (strarray_size(email_ids)) {
        const char *email_id = strarray_nth(email_ids, 0);
        size_t email_id_len = strlen(email_id);
        buf_appendbit64(&buf, email_id_len);
        int i;
        for (i = 0; i < strarray_size(email_ids); i++) {
            const char *email_id = strarray_nth(email_ids, i);
            if (strlen(email_id) != email_id_len) {
                syslog(LOG_ERR, "jmap: email id %s has length %zd,"
                                "expected %zd - aborting cache",
                                email_id, strlen(email_id), email_id_len);
                r = CYRUSDB_INTERNAL;
                goto done;
            }
            buf_appendcstr(&buf, email_id);
            buf_putc(&buf, '\0');
        }
    }
    /* Store cache record */
    r = cyrusdb_store(cache_db, cache_key, strlen(cache_key),
            buf_base(&buf), buf_len(&buf), NULL);

done:
    buf_free(&buf);
    return r;
}

static int _email_query_readcache(struct db *cache_db,
                                  const char *cache_key,
                                  modseq_t current_modseq,
                                  struct cached_emailquery *cache_record)
{
    /* Load cache record */
    const char *data = NULL;
    size_t datalen = 0;
    int r = cyrusdb_fetch(cache_db, cache_key, strlen(cache_key), &data, &datalen, NULL);
    if (r) {
        if (r != CYRUSDB_NOTFOUND) {
            syslog(LOG_ERR, "jmap: can't fetch cached email search (%s): %s",
                    cache_key, cyrusdb_strerror(r));
        }
        return r;
    }

    /* Read cache record preamble */
    const char *p = data;
    uint32_t version = ntohl(((bit32*)(p))[0]); p += 4;
    if (version != _EMAILSEARCH_CACHE_VERSION) {
        syslog(LOG_ERR, "jmap: unexpected cache version %d (%s)", version, cache_key);
        r = CYRUSDB_EXISTS;
        goto done;
    }
    modseq_t cached_modseq = ntohll(((bit64*)(p))[0]); p += 8;
    if (cached_modseq != current_modseq) {
        r = CYRUSDB_EXISTS;
        goto done;
    }

    /* Read email ids */
    size_t ids_count = ntohll(((bit64*)(p))[0]); p += 8;
    cache_record->ids_count= ids_count;
    if (ids_count) {
        size_t id_size = ntohll(((bit64*)(p))[0]); p += 8;
        cache_record->id_size = id_size;
        size_t ids_size = ids_count * (id_size + 1);
        cache_record->ids = xmalloc(ids_size);
        memcpy(cache_record->ids, p, ids_size);
        p += ids_size;
    }

    /* Check end of record */
    if (p != data + datalen) {
        syslog(LOG_ERR, "jmap: invalid query cache entry %s", cache_key);
        r = CYRUSDB_NOTFOUND;
        goto done;
    }

done:
    if (r) {
        _cached_emailquery_fini(cache_record);
        cyrusdb_delete(cache_db, cache_key, strlen(cache_key), NULL, 0);
        return r == CYRUSDB_EXISTS? CYRUSDB_NOTFOUND : r;
    }
    return 0;
}

static void _email_query(jmap_req_t *req, struct jmap_query *query,
                         int collapse_threads,
                         hash_table *contactgroups,
                         json_t **jemailpartids, json_t **err)
{
    char *cache_fname = NULL;
    char *cache_key = NULL;
    struct db *cache_db = NULL;
    modseq_t current_modseq = jmap_highestmodseq(req, MBTYPE_EMAIL);
    int is_cached = 0;

    struct emailsearch *search = _emailsearch_new(req, query->filter, query->sort,
                                                  contactgroups, 0, 0,
                                                  &query->sort_savedate);
    if (!search) {
        *err = jmap_server_error(IMAP_INTERNAL);
        goto done;
    }

    /* can calculate changes for mutable sort, but not mutable search */
    query->can_calculate_changes = search->is_mutable > 1 ? 0 : 1;

    /* make query state */
    query->query_state = _email_make_querystate(current_modseq, 0,
            contactgroups->size ?  jmap_highestmodseq(req, MBTYPE_ADDRESSBOOK) : 0);

    /* Open cache */
    cache_fname = emailsearch_getcachepath();
    if (cache_fname) {
        int flags = CYRUSDB_CREATE|CYRUSDB_CONVERT;
        int r = cyrusdb_open(EMAILSEARCH_DB, cache_fname, flags, &cache_db);
        if (r) {
            syslog(LOG_WARNING, "jmap: can't open email search cache %s: %s",
                    cache_fname, cyrusdb_strerror(r));
        }
    }

    /* Make cache key */
    cache_key = strconcat(req->accountid,
            "/", collapse_threads ?  "collapsed" : "uncollapsed",
            "/", search->hash, NULL
    );

    /* Lookup cache */
    if (cache_db) {
        struct cached_emailquery cache_record = _CACHED_EMAILQUERY_INITIALIZER;
        int r = _email_query_readcache(cache_db, cache_key, current_modseq, &cache_record);
        if (!r) {
            size_t from = query->position;
            if (query->anchor) {
                size_t i;
                for (i = 0; i < cache_record.ids_count; i++) {
                    const char *email_id = cache_record.ids + i * (cache_record.id_size + 1);
                    if (!strcmp(email_id, query->anchor)) {
                        if (query->anchor_offset < 0) {
                            size_t neg_offset = (size_t) -query->anchor_offset;
                            from = neg_offset < i ? i - neg_offset : 0;
                        }
                        else {
                            from = i + query->anchor_offset;
                        }
                        break;
                    }
                }
                if (i == cache_record.ids_count) {
                    *err = json_pack("{s:s}", "type", "anchorNotFound");
                }
            }
            else if (query->position < 0) {
                ssize_t sposition = (ssize_t) cache_record.ids_count + query->position;
                from = sposition < 0 ? 0 : sposition;
            }
            size_t to = query->limit ? from + query->limit : cache_record.ids_count;
            if (to > cache_record.ids_count) to = cache_record.ids_count;
            size_t i;
            for (i = from; i < to; i++) {
                const char *email_id = cache_record.ids + i * (cache_record.id_size + 1);
                json_array_append_new(query->ids, json_string(email_id));
            }
            query->result_position = from;
            query->total = cache_record.ids_count;
            is_cached = 1;
        }
        _cached_emailquery_fini(&cache_record);
    }

    /* Set performance info */
    if (jmap_is_using(req, JMAP_PERFORMANCE_EXTENSION)) {
        json_object_set_new(req->perf_details, "isCached", json_boolean(is_cached));
        int i;
        json_t *jfilters = json_array();
        for (i = 0; i < strarray_size(&search->perf_filters); i++) {
            const char *cost = strarray_nth(&search->perf_filters, i);
            json_array_append_new(jfilters, json_string(cost));
        }
        json_object_set_new(req->perf_details, "filters", jfilters);
    }
    if (is_cached) goto done;

    /* Run search */
    const ptrarray_t *msgdata = NULL;
    int r = _emailsearch_run(search, &msgdata);
    if (r) {
        *err = jmap_server_error(r);
        goto done;
    }

    // TODO cache emailId -> threadId on the request context
    // TODO support negative positions
    assert(query->position >= 0);

    /* Initialize search result loop */
    size_t anchor_position = (size_t)-1;
    char email_id[JMAP_EMAILID_SIZE];

    struct hashset *seen_emails = hashset_new(12);
    struct hashset *seen_threads = hashset_new(8);
    struct hashset *savedates = NULL;

    /* List of all matching email ids */
    strarray_t email_ids = STRARRAY_INITIALIZER;

    int found_anchor = 0;

    if (query->sort_savedate) {
        /* Build hashset of messages with savedates */
        int j;

        savedates = hashset_new(12);

        for (j = 0; j < msgdata->count; j++) {
            MsgData *md = ptrarray_nth(msgdata, j);

            /* Skip expunged or hidden messages */
            if (md->system_flags & FLAG_DELETED ||
                md->internal_flags & FLAG_INTERNAL_EXPUNGED)
                continue;

            if (md->savedate) hashset_add(savedates, &md->guid.value);
        }
    }

    int i;
    for (i = 0 ; i < msgdata->count; i++) {
        MsgData *md = ptrarray_nth(msgdata, i);

        /* Skip expunged or hidden messages */
        if (md->system_flags & FLAG_DELETED ||
            md->internal_flags & FLAG_INTERNAL_EXPUNGED)
            continue;

        /* Is there another copy of this message with a targeted savedate? */
        if (!md->savedate &&
            savedates && hashset_exists(savedates, &md->guid.value))
            continue;

        /* Have we seen this message already? */
        if (!hashset_add(seen_emails, &md->guid.value))
            continue;
        if (collapse_threads && !hashset_add(seen_threads, &md->cid))
            continue;

        /* This message matches the query. */
        size_t result_count = json_array_size(query->ids);
        query->total++;
        jmap_set_emailid(&md->guid, email_id);

        if (cache_db) strarray_append(&email_ids, email_id);

        /* Apply query window, if any */
        if (query->anchor) {
            if (!strcmp(email_id, query->anchor)) {
                found_anchor = 1;
                /* Recalculate the search result */
                json_t *anchored_ids = json_pack("[]");
                size_t j;
                /* Set countdown to enter the anchor window */
                if (query->anchor_offset > 0) {
                    anchor_position = query->anchor_offset;
                } else {
                    anchor_position = 0;
                }
                /* Readjust the result list */
                if (query->anchor_offset < 0) {
                    size_t neg_offset = (size_t) -query->anchor_offset;
                    size_t from = neg_offset < result_count ? result_count - neg_offset : 0;
                    for (j = from; j < result_count; j++) {
                        json_array_append(anchored_ids, json_array_get(query->ids, j));
                    }
                }
                json_decref(query->ids);
                query->ids = anchored_ids;
                result_count = json_array_size(query->ids);

                /* Adjust the window position for this anchor. */
                query->result_position = query->total - json_array_size(anchored_ids) - 1;
            }
            if (anchor_position != (size_t)-1 && anchor_position) {
                /* Found the anchor but haven't yet entered its window */
                anchor_position--;
                /* But this message still counts to the window position */
                query->result_position++;
                continue;
            }
        }
        else if (query->position > 0 && query->total < ((size_t) query->position) + 1) {
            continue;
        }

        /* Apply limit */
        if (query->limit && result_count && query->limit <= result_count)
            continue;

        /* Add message to result */
        json_array_append_new(query->ids, json_string(email_id));
        if (*jemailpartids == NULL) {
            *jemailpartids = json_object();
        }
        if (md->folder && md->folder->partids.size) {
            const strarray_t *partids = hashu64_lookup(md->uid, &md->folder->partids);
            if (partids && strarray_size(partids)) {
                json_t *jpartids = json_array();
                int k;
                for (k = 0; k < strarray_size(partids); k++) {
                    const char *partid = strarray_nth(partids, k);
                    json_array_append_new(jpartids, json_string(partid));
                }
                json_object_set_new(*jemailpartids, email_id, jpartids);
            }
        }
        if (!json_object_get(*jemailpartids, email_id)) {
            json_object_set_new(*jemailpartids, email_id, json_null());
        }
    }
    hashset_free(&seen_threads);
    hashset_free(&seen_emails);
    if (savedates) hashset_free(&savedates);

    if (!query->anchor) {
        query->result_position = query->position;
    }
    else if (!found_anchor) {
        *err = json_pack("{s:s}", "type", "anchorNotFound");
    }

    /* Cache search result */
    if (cache_db) {
        int r = _email_query_writecache(cache_db, cache_key, current_modseq, &email_ids);
        if (r) {
            syslog(LOG_ERR, "jmap: can't cache email search (%s): %s",
                    cache_key, cyrusdb_strerror(r));
            r = 0;
        }
    }
    strarray_fini(&email_ids);

    if (jemailpartids && !*jemailpartids)
        *jemailpartids = json_null();

done:
    _emailsearch_free(search);
    if (cache_db) {
        int r = cyrusdb_close(cache_db);
        if (r) {
            syslog(LOG_ERR, "jmap: can't close email search cache %s: %s",
                    cache_fname, cyrusdb_strerror(r));
        }
    }
    free(cache_key);
    free(cache_fname);
}

static int _email_queryargs_parse(jmap_req_t *req,
                                  struct jmap_parser *parser __attribute__((unused)),
                                  const char *key,
                                  json_t *arg,
                                  void *rock)
{
    int *collapse_threads = (int *) rock;
    int r = 1;

    if (!strcmp(key, "collapseThreads") && json_is_boolean(arg)) {
        *collapse_threads = json_boolean_value(arg);
    }
    else if (!strcmp(key, "addressbookId") && json_is_string(arg) &&
             jmap_is_using(req, JMAP_MAIL_EXTENSION)) {

        /* Lookup addrbook */
        char *addrbookname = carddav_mboxname(req->accountid, json_string_value(arg));
        mbentry_t *mbentry = NULL;
        int is_valid = 0;
        if (!mboxlist_lookup(addrbookname, &mbentry, NULL)) {
            is_valid = jmap_hasrights(req, mbentry, ACL_LOOKUP) &&
                       mbentry->mbtype == MBTYPE_ADDRESSBOOK;
        }
        mboxlist_entry_free(&mbentry);
        free(addrbookname);
        return is_valid;
    }
    else r = 0;

    return r;
}

static int jmap_email_query(jmap_req_t *req)
{
    struct jmap_parser parser = JMAP_PARSER_INITIALIZER;
    struct jmap_query query;
    int collapse_threads = 0;
    json_t *jemailpartids = NULL;
    struct email_contactfilter contactfilter;
    int r = 0;

    _email_contactfilter_initreq(req, &contactfilter);

    /* Parse request */
    json_t *err = NULL;
    jmap_query_parse(req, &parser,
                     _email_queryargs_parse, &collapse_threads,
                     _email_parse_filter_cb, &contactfilter,
                     _email_parse_comparator, NULL,
                     &query, &err);
    if (err) {
        jmap_error(req, err);
        goto done;
    }
    if (query.position < 0) {
        /* we currently don't support negative positions */
        jmap_parser_invalid(&parser, "position");
    }
    if (json_array_size(parser.invalid)) {
        err = json_pack("{s:s}", "type", "invalidArguments");
        json_object_set(err, "arguments", parser.invalid);
        jmap_error(req, err);
        goto done;
    }
    else if (r) {
        jmap_error(req, jmap_server_error(r));
        goto done;
    }

    /* Run query */
    _email_query(req, &query, collapse_threads, &contactfilter.contactgroups,
                 &jemailpartids, &err);
    if (err) {
        jmap_error(req, err);
        goto done;
    }

    /* Build response */
    json_t *res = jmap_query_reply(&query);
    json_object_set(res, "collapseThreads", json_boolean(collapse_threads));
    if (jmap_is_using(req, JMAP_SEARCH_EXTENSION)) {
        json_object_set(res, "partIds", jemailpartids); // incref
    }
    if (jmap_is_using(req, JMAP_DEBUG_EXTENSION)) {
        /* List language stats */
        const struct search_engine *engine = search_engine();
        if (engine->list_lang_stats) {
            ptrarray_t lstats = PTRARRAY_INITIALIZER;
            int r = engine->list_lang_stats(req->accountid, &lstats);
            if (!r) {
                json_t *jstats = json_object();
                struct search_lang_stats *lstat;
                while ((lstat = ptrarray_pop(&lstats))) {
                    json_t *jstat = json_pack("{s:f}", "weight", lstat->weight);
                    json_object_set_new(jstats, lstat->iso_lang, jstat);
                    free(lstat->iso_lang);
                    free(lstat);
                }
                json_object_set_new(res, "languageStats", jstats);
            }
            ptrarray_fini(&lstats);
        }
    }
    jmap_ok(req, res);

done:
    jmap_email_contactfilter_fini(&contactfilter);
    json_decref(jemailpartids);
    jmap_query_fini(&query);
    jmap_parser_fini(&parser);
    return 0;
}

static void _email_querychanges_collapsed(jmap_req_t *req,
                                          struct jmap_querychanges *query,
                                          struct email_contactfilter *contactfilter,
                                          json_t **err)
{
    modseq_t since_modseq;
    uint32_t since_uid;
    uint32_t num_changes = 0;
    modseq_t addrbook_modseq = 0;

    if (!_email_read_querystate(query->since_querystate,
                                &since_modseq, &since_uid,
                                &addrbook_modseq)) {
        *err = json_pack("{s:s}", "type", "cannotCalculateChanges");
        return;
    }
    if (addrbook_modseq && addrbook_modseq != jmap_highestmodseq(req, MBTYPE_ADDRESSBOOK)) {
        *err = json_pack("{s:s}", "type", "cannotCalculateChanges");
        return;
    }

    struct emailsearch *search = _emailsearch_new(req, query->filter, query->sort,
                                                  &contactfilter->contactgroups,
                                                  /*want_expunged*/1, /*ignore_timer*/0,
                                                  &query->sort_savedate);
    if (!search) {
        *err = jmap_server_error(IMAP_INTERNAL);
        goto done;
    }

    /* Run search */
    const ptrarray_t *msgdata = NULL;
    int r = _emailsearch_run(search, &msgdata);
    if (r) {
        if (r == IMAP_SEARCH_SLOW) {
            *err = json_pack("{s:s, s:s}", "type", "cannotCalculateChanges",
                                           "description", "search too slow");
        }
        else {
            *err = jmap_server_error(r);
        }
        goto done;
    }

    /* Prepare result loop */
    char email_id[JMAP_EMAILID_SIZE];
    int found_up_to = 0;
    size_t mdcount = msgdata->count;

    hash_table touched_ids = HASH_TABLE_INITIALIZER;
    memset(&touched_ids, 0, sizeof(hash_table));
    construct_hash_table(&touched_ids, mdcount + 1, 0);

    hashu64_table touched_cids = HASH_TABLE_INITIALIZER;
    memset(&touched_cids, 0, sizeof(hashu64_table));
    construct_hashu64_table(&touched_cids, mdcount + 1, 0);

    /* touched_ids contains values for each email_id:
     * 1 - email has been modified
     * 2 - email has been seen (aka: non-expunged record shown)
     * 4 - email has been reported removed
     * 8 - email has been reported added
     */

    /* touched_cids contains values for each thread
     * 1 - thread has been modified
     * 2 - thread has been seen (aka: exemplar shown)
     * 4 - thread has had an expunged shown (aka: possible old exemplar passed)
     * 8 - thread is finished (aka: old exemplar definitely passed)
     */

    // phase 1: find messages and threads which have been modified
    size_t i;
    for (i = 0 ; i < mdcount; i++) {
        MsgData *md = ptrarray_nth(msgdata, i);

        // for this phase, we only care that it has a change
        if (md->modseq <= since_modseq) {
            if (search->is_mutable) {
                modseq_t modseq = 0;
                conversation_get_modseq(req->cstate, md->cid, &modseq);
                if (modseq > since_modseq)
                    hashu64_insert(md->cid, (void*)1, &touched_cids);
            }
            continue;
        }

        jmap_set_emailid(&md->guid, email_id);

        hash_insert(email_id, (void*)1, &touched_ids);
        hashu64_insert(md->cid, (void*)1, &touched_cids);
    }

    // phase 2: report messages that need it
    for (i = 0 ; i < mdcount; i++) {
        MsgData *md = ptrarray_nth(msgdata, i);

        jmap_set_emailid(&md->guid, email_id);

        int is_expunged = (md->system_flags & FLAG_DELETED) ||
                (md->internal_flags & FLAG_INTERNAL_EXPUNGED);

        size_t touched_id = (size_t)hash_lookup(email_id, &touched_ids);
        size_t new_touched_id = touched_id;

        size_t touched_cid = (size_t)hashu64_lookup(md->cid, &touched_cids);
        size_t new_touched_cid = touched_cid;

        if (is_expunged) {
            // don't need to tell changes any more
            if (found_up_to) goto doneloop;

            // nothing to do if not changed (could not be old exemplar)
            if (!(touched_id & 1)) goto doneloop;

            // could not possibly be old exemplar
            if (!search->is_mutable && (touched_cid & 8)) goto doneloop;

            // add the destroy notice
            if (!(touched_id & 4)) {
                _email_querychanges_destroyed(query, email_id);
                new_touched_id |= 4;
                new_touched_cid |= 4;
            }

            goto doneloop;
        }

        // this is the exemplar for the cid
        if (!(touched_cid & 2)) {
            query->total++;
            new_touched_cid |= 2;
        }

        if (found_up_to) goto doneloop;

        // if it's a changed cid, see if we should tell
        if ((touched_cid & 1)) {
            // haven't told the exemplar yet?  This is the exemplar!
            if (!(touched_cid & 2)) {
                // not yet told in any way, and this ID hasn't been told at all
                if (touched_cid == 1 && touched_id == 0 && !search->is_mutable) {
                    // this is both old AND new exemplar, horray.  We don't
                    // need to tell anything
                    new_touched_cid |= 8;
                    goto doneloop;
                }

                // have to tell both a remove and an add for the exemplar
                if (!(touched_id & 4)) {
                    _email_querychanges_destroyed(query, email_id);
                    new_touched_id |= 4;
                    num_changes++;
                }
                if (!(touched_id & 8)) {
                    _email_querychanges_added(query, email_id);
                    new_touched_id |= 8;
                    num_changes++;
                }
                new_touched_cid |= 4;
                goto doneloop;
            }
            // otherwise we've already told the exemplar.

            // could not possibly be old exemplar
            if (!search->is_mutable && (touched_cid & 8)) goto doneloop;

            // OK, maybe this alive message WAS the old examplar
            if (!(touched_id & 4)) {
                _email_querychanges_destroyed(query, email_id);
                new_touched_id |= 4;
                new_touched_cid |= 4;
            }

            // and if this message is a stopper (must have been a candidate
            // for old exemplar) then stop
            if (!(touched_id & 1)) {
                new_touched_cid |= 8;
            }
        }

    doneloop:
        if (query->max_changes && (num_changes > query->max_changes)) {
            *err = json_pack("{s:s}", "type", "tooManyChanges");
            break;
        }
        if (new_touched_id != touched_id)
            hash_insert(email_id, (void*)new_touched_id, &touched_ids);
        if (new_touched_cid != touched_cid)
            hashu64_insert(md->cid, (void*)new_touched_cid, &touched_cids);
        // if the search is mutable, later changes could have
        // been earlier once, so no up_to_id is possible
        if (!found_up_to && !search->is_mutable
                         && query->up_to_id
                         && !strcmp(email_id, query->up_to_id)) {
            found_up_to = 1;
        }
    }

    free_hash_table(&touched_ids, NULL);
    free_hashu64_table(&touched_cids, NULL);

    modseq_t modseq = jmap_highestmodseq(req, MBTYPE_EMAIL);
    query->new_querystate = _email_make_querystate(modseq, 0, addrbook_modseq);

done:
    _emailsearch_free(search);
}

static void _email_querychanges_uncollapsed(jmap_req_t *req,
                                            struct jmap_querychanges *query,
                                            struct email_contactfilter *contactfilter,
                                            json_t **err)
{
    modseq_t since_modseq;
    uint32_t since_uid;
    uint32_t num_changes = 0;
    modseq_t addrbook_modseq = 0;

    if (!_email_read_querystate(query->since_querystate,
                                &since_modseq, &since_uid,
                                &addrbook_modseq)) {
        *err = json_pack("{s:s}", "type", "cannotCalculateChanges");
        return;
    }
    if (addrbook_modseq && addrbook_modseq != jmap_highestmodseq(req, MBTYPE_ADDRESSBOOK)) {
        *err = json_pack("{s:s}", "type", "cannotCalculateChanges");
        return;
    }

    struct emailsearch *search = _emailsearch_new(req, query->filter, query->sort,
                                                  &contactfilter->contactgroups,
                                                  /*want_expunged*/1, /*ignore_timer*/0,
                                                  &query->sort_savedate);
    if (!search) {
        *err = jmap_server_error(IMAP_INTERNAL);
        goto done;
    }

    /* Run search */
    const ptrarray_t *msgdata = NULL;
    int r = _emailsearch_run(search, &msgdata);
    if (r) {
        if (r == IMAP_SEARCH_SLOW) {
            *err = json_pack("{s:s, s:s}", "type", "cannotCalculateChanges",
                                           "description", "search too slow");
        }
        else {
            *err = jmap_server_error(r);
        }
        goto done;
    }

    /* Prepare result loop */
    char email_id[JMAP_EMAILID_SIZE];
    int found_up_to = 0;
    size_t mdcount = msgdata->count;

    hash_table touched_ids = HASH_TABLE_INITIALIZER;
    memset(&touched_ids, 0, sizeof(hash_table));
    construct_hash_table(&touched_ids, mdcount + 1, 0);

    /* touched_ids contains values for each email_id:
     * 1 - email has been modified
     * 2 - email has been seen (aka: non-expunged record shown)
     * 4 - email has been reported removed
     * 8 - email has been reported added
     */

    // phase 1: find messages which have been modified
    size_t i;
    for (i = 0 ; i < mdcount; i++) {
        MsgData *md = ptrarray_nth(msgdata, i);

        // for this phase, we only care that it has a change
        if (md->modseq <= since_modseq) continue;

        jmap_set_emailid(&md->guid, email_id);

        hash_insert(email_id, (void*)1, &touched_ids);
    }

    // phase 2: report messages that need it
    for (i = 0 ; i < mdcount; i++) {
        MsgData *md = ptrarray_nth(msgdata, i);

        jmap_set_emailid(&md->guid, email_id);

        int is_expunged = (md->system_flags & FLAG_DELETED) ||
                (md->internal_flags & FLAG_INTERNAL_EXPUNGED);

        size_t touched_id = (size_t)hash_lookup(email_id, &touched_ids);
        size_t new_touched_id = touched_id;

        if (is_expunged) {
            // don't need to tell changes any more
            if (found_up_to) continue;

            // nothing to do if not changed
            if (!(touched_id & 1)) continue;

            // add the destroy notice
            if (!(touched_id & 4)) {
                _email_querychanges_destroyed(query, email_id);
                new_touched_id |= 4;
            }

            goto doneloop;
        }

        // this is an exemplar
        if (!(touched_id & 2)) {
            query->total++;
            new_touched_id |= 2;
        }

        if (found_up_to) goto doneloop;

        // if it's changed, tell about that
        if ((touched_id & 1)) {
            if (!search->is_mutable && touched_id == 1 && md->modseq <= since_modseq) {
                // this is the exemplar, and it's unchanged,
                // and we haven't told a removed yet, so we
                // can just suppress everything
                new_touched_id |= 4 | 8;
                goto doneloop;
            }

            // otherwise we're going to have to tell both, if we haven't already
            if (!(touched_id & 4)) {
                _email_querychanges_destroyed(query, email_id);
                new_touched_id |= 4;
                num_changes++;
            }
            if (!(touched_id & 8)) {
                _email_querychanges_added(query, email_id);
                new_touched_id |= 8;
                num_changes++;
            }
        }

    doneloop:
        if (query->max_changes && (num_changes > query->max_changes)) {
            *err = json_pack("{s:s}", "type", "tooManyChanges");
            break;
        }
        if (new_touched_id != touched_id)
            hash_insert(email_id, (void*)new_touched_id, &touched_ids);
        // if the search is mutable, later changes could have
        // been earlier once, so no up_to_id is possible
        if (!found_up_to && !search->is_mutable
                         && query->up_to_id
                         && !strcmp(email_id, query->up_to_id)) {
            found_up_to = 1;
        }
    }

    free_hash_table(&touched_ids, NULL);

    modseq_t modseq = jmap_highestmodseq(req, MBTYPE_EMAIL);
    query->new_querystate = _email_make_querystate(modseq, 0, addrbook_modseq);

done:
    _emailsearch_free(search);
}

static int jmap_email_querychanges(jmap_req_t *req)
{
    struct jmap_parser parser = JMAP_PARSER_INITIALIZER;
    struct jmap_querychanges query;
    struct email_contactfilter contactfilter;
    int collapse_threads = 0;

    _email_contactfilter_initreq(req, &contactfilter);

    /* Parse arguments */
    json_t *err = NULL;
    jmap_querychanges_parse(req, &parser,
                            _email_queryargs_parse, &collapse_threads,
                            _email_parse_filter_cb, &contactfilter,
                            _email_parse_comparator, NULL,
                            &query, &err);
    if (err) {
        jmap_error(req, err);
        goto done;
    }

    if (json_array_size(parser.invalid)) {
        err = json_pack("{s:s}", "type", "invalidArguments");
        json_object_set(err, "arguments", parser.invalid);
        jmap_error(req, err);
        goto done;
    }

    /* Query changes */
    if (collapse_threads)
        _email_querychanges_collapsed(req, &query, &contactfilter, &err);
    else
        _email_querychanges_uncollapsed(req, &query, &contactfilter, &err);
    if (err) {
        jmap_error(req, err);
        goto done;
    }

    /* Build response */
    json_t *res = jmap_querychanges_reply(&query);
    json_object_set(res, "collapseThreads", json_boolean(collapse_threads));
    jmap_ok(req, res);

done:
    jmap_email_contactfilter_fini(&contactfilter);
    jmap_querychanges_fini(&query);
    jmap_parser_fini(&parser);
    return 0;
}

static void _email_changes(jmap_req_t *req, struct jmap_changes *changes, json_t **err)
{
    /* Run search */
    json_t *filter = json_pack("{s:o}", "sinceEmailState",
                               jmap_fmtstate(changes->since_modseq));
    json_t *sort = json_pack("[{s:s}]", "property", "emailState");

    struct emailsearch *search = _emailsearch_new(req, filter, sort,
                                                  /*contactgroups*/NULL,
                                                  /*want_expunged*/1,
                                                  /*ignore_timer*/1,
                                                  NULL);
    if (!search) {
        *err = jmap_server_error(IMAP_INTERNAL);
        goto done;
    }

    const ptrarray_t *msgdata = NULL;
    int r = _emailsearch_run(search, &msgdata);
    if (r) {
        *err = jmap_server_error(r);
        goto done;
    }

    /* Process results */
    char email_id[JMAP_EMAILID_SIZE];
    size_t changes_count = 0;
    modseq_t highest_modseq = 0;
    int i;
    hash_table seen_ids = HASH_TABLE_INITIALIZER;
    memset(&seen_ids, 0, sizeof(hash_table));
    construct_hash_table(&seen_ids, msgdata->count + 1, 0);

    for (i = 0 ; i < msgdata->count; i++) {
        MsgData *md = ptrarray_nth(msgdata, i);

        jmap_set_emailid(&md->guid, email_id);

        /* Skip already seen messages */
        if (hash_lookup(email_id, &seen_ids)) continue;
        hash_insert(email_id, (void*)1, &seen_ids);

        /* Apply limit, if any */
        if (changes->max_changes && ++changes_count > changes->max_changes) {
            changes->has_more_changes = 1;
            break;
        }

        /* Keep track of the highest modseq */
        if (highest_modseq < md->modseq)
            highest_modseq = md->modseq;

        struct email_expunge_check rock = { req, changes->since_modseq, 0 };
        int r = conversations_guid_foreach(req->cstate, _guid_from_id(email_id),
                                           _email_is_expunged_cb, &rock);
        if (r) {
            *err = jmap_server_error(r);
            goto done;
        }

        /* Check the message status - status is a bitfield with:
         * 1: a message exists which was created on or before since_modseq
         * 2: a message exists which is not deleted
         *
         * from those facts we can determine ephemeral / destroyed / created / updated
         * and we don't need to tell about ephemeral (all created since last time, but none left)
         */
        switch (rock.status) {
        default:
            break; /* all messages were created AND deleted since previous state! */
        case 1:
            /* only expunged messages exist */
            json_array_append_new(changes->destroyed, json_string(email_id));
            break;
        case 2:
            /* alive, and all messages are created since previous modseq */
            json_array_append_new(changes->created, json_string(email_id));
            break;
        case 3:
            /* alive, and old */
            json_array_append_new(changes->updated, json_string(email_id));
            break;
        }
    }
    free_hash_table(&seen_ids, NULL);

    /* Set new state */
    changes->new_modseq = changes->has_more_changes ?
        highest_modseq : jmap_highestmodseq(req, MBTYPE_EMAIL);

done:
    json_decref(filter);
    json_decref(sort);
    _emailsearch_free(search);
}

static int jmap_email_changes(jmap_req_t *req)
{
    struct jmap_parser parser = JMAP_PARSER_INITIALIZER;
    struct jmap_changes changes;

    /* Parse request */
    json_t *err = NULL;
    jmap_changes_parse(req, &parser, NULL, NULL, &changes, &err);
    if (err) {
        jmap_error(req, err);
        goto done;
    }

    /* Search for updates */
    _email_changes(req, &changes, &err);
    if (err) {
        jmap_error(req, err);
        goto done;
    }

    /* Build response */
    jmap_ok(req, jmap_changes_reply(&changes));

done:
    jmap_changes_fini(&changes);
    jmap_parser_fini(&parser);
    return 0;
}

static void _thread_changes(jmap_req_t *req, struct jmap_changes *changes, json_t **err)
{
    conversation_t conv = CONVERSATION_INIT;

    /* Run search */
    json_t *filter = json_pack("{s:o}", "sinceEmailState",
                               jmap_fmtstate(changes->since_modseq));
    json_t *sort = json_pack("[{s:s}]", "property", "emailState");
    struct emailsearch *search = _emailsearch_new(req, filter, sort,
                                                  /*contactgroups*/NULL,
                                                  /*want_expunged*/1,
                                                  /*ignore_timer*/1,
                                                  NULL);
    if (!search) {
        *err = jmap_server_error(IMAP_INTERNAL);
        goto done;
    }

    const ptrarray_t *msgdata = NULL;
    int r = _emailsearch_run(search, &msgdata);
    if (r) {
        *err = jmap_server_error(r);
        goto done;
    }

    /* Process results */
    size_t changes_count = 0;
    modseq_t highest_modseq = 0;
    int i;

    struct hashset *seen_threads = hashset_new(8);

    char thread_id[JMAP_THREADID_SIZE];

    for (i = 0 ; i < msgdata->count; i++) {
        MsgData *md = ptrarray_nth(msgdata, i);

        /* Skip already seen threads */
        if (!hashset_add(seen_threads, &md->cid)) continue;

        /* Apply limit, if any */
        if (changes->max_changes && ++changes_count > changes->max_changes) {
            changes->has_more_changes = 1;
            break;
        }

        /* Keep track of the highest modseq */
        if (highest_modseq < md->modseq)
            highest_modseq = md->modseq;

        /* Determine if the thread got changed or destroyed */
        if (conversation_load_advanced(req->cstate, md->cid, &conv, /*flags*/0))
            continue;

        /* Report thread */
        jmap_set_threadid(md->cid, thread_id);
        if (conv.exists) {
            if (conv.createdmodseq <= changes->since_modseq)
                json_array_append_new(changes->updated, json_string(thread_id));
            else
                json_array_append_new(changes->created, json_string(thread_id));
        }
        else {
            if (conv.createdmodseq <= changes->since_modseq)
                json_array_append_new(changes->destroyed, json_string(thread_id));
        }

        conversation_fini(&conv);
        memset(&conv, 0, sizeof(conversation_t));
    }
    hashset_free(&seen_threads);

    /* Set new state */
    changes->new_modseq = changes->has_more_changes ?
        highest_modseq : jmap_highestmodseq(req, MBTYPE_EMAIL);

done:
    conversation_fini(&conv);
    json_decref(filter);
    json_decref(sort);
    _emailsearch_free(search);
}

static int jmap_thread_changes(jmap_req_t *req)
{
    struct jmap_parser parser = JMAP_PARSER_INITIALIZER;
    struct jmap_changes changes;

    /* Parse request */
    json_t *err = NULL;
    jmap_changes_parse(req, &parser, NULL, NULL, &changes, &err);
    if (err) {
        jmap_error(req, err);
        goto done;
    }

    /* Search for updates */
    _thread_changes(req, &changes, &err);
    if (err) {
        jmap_error(req, err);
        goto done;
    }

    /* Build response */
    jmap_ok(req, jmap_changes_reply(&changes));

done:
    jmap_changes_fini(&changes);
    jmap_parser_fini(&parser);
    return 0;
}

static int _snippet_get_cb(struct mailbox *mbox __attribute__((unused)),
                           uint32_t uid __attribute__((unused)),
                           int part, const char *s, void *rock)
{
    const char *propname = NULL;
    json_t *snippet = rock;

    if (part == SEARCH_PART_SUBJECT) {
        propname = "subject";
    }
    else if (part == SEARCH_PART_BODY || part == SEARCH_PART_ATTACHMENTBODY) {
        propname = "preview";
    }

    if (propname) {
        json_object_set_new(snippet, propname, json_string(s));
    }

    /* Avoid costly attachment body snippets, if possible */
    return part == SEARCH_PART_BODY ? IMAP_OK_COMPLETED : 0;
}

static int _snippet_get(jmap_req_t *req, json_t *filter,
                        json_t *messageids, json_t *jemailpartids,
                        json_t **snippets, json_t **notfound)
{
    struct index_state *state = NULL;
    void *intquery = NULL;
    search_builder_t *bx = NULL;
    search_text_receiver_t *rx = NULL;
    struct mailbox *mbox = NULL;
    struct searchargs *searchargs = NULL;
    struct index_init init;
    json_t *snippet = NULL;
    int r = 0;
    json_t *val;
    size_t i;
    char *mboxname = NULL;
    static search_snippet_markup_t markup = { "<mark>", "</mark>", "..." };
    strarray_t partids = STRARRAY_INITIALIZER;

    *snippets = json_pack("[]");
    *notfound = json_pack("[]");

    /* Build searchargs */
    strarray_t perf_filters = STRARRAY_INITIALIZER;
    searchargs = new_searchargs(NULL/*tag*/, GETSEARCH_CHARSET_FIRST,
                                &jmap_namespace, req->userid, req->authstate, 0);
    searchargs->root = _email_buildsearch(req, filter, /*contactgroups*/NULL, &perf_filters);
    strarray_fini(&perf_filters);

    /* Build the search query */
    memset(&init, 0, sizeof(init));
    init.userid = req->userid;
    init.authstate = req->authstate;
    init.examine_mode = 1;

    char *inboxname = mboxname_user_mbox(req->accountid, NULL);
    r = index_open(inboxname, &init, &state);
    free(inboxname);
    if (r) goto done;

    bx = search_begin_search(state->mailbox, SEARCH_MULTIPLE);
    if (!bx) {
        r = IMAP_INTERNAL;
        goto done;
    }

    search_build_query(bx, searchargs->root);
    if (!bx->get_internalised) {
        r = IMAP_INTERNAL;
        goto done;
    }
    intquery = bx->get_internalised(bx);
    search_end_search(bx);
    if (!intquery) {
        r = IMAP_INTERNAL;
        goto done;
    }

    /* Set up snippet callback context */
    snippet = json_pack("{}");
    rx = search_begin_snippets(intquery, 0, &markup, _snippet_get_cb, snippet);
    if (!rx) {
        r = IMAP_INTERNAL;
        goto done;
    }

    /* Convert the snippets */
    json_array_foreach(messageids, i, val) {
        message_t *msg;
        msgrecord_t *mr = NULL;
        uint32_t uid;

        const char *msgid = json_string_value(val);

        r = jmap_email_find(req, msgid, &mboxname, &uid);
        if (r) {
            if (r == IMAP_NOTFOUND) {
                json_array_append_new(*notfound, json_string(msgid));
            }
            r = 0;
            continue;
        }

        r = jmap_openmbox(req, mboxname, &mbox, 0);
        if (r) goto done;

        r = msgrecord_find(mbox, uid, &mr);
        if (r) goto doneloop;

        r = msgrecord_get_message(mr, &msg);
        if (r) goto doneloop;

        json_t *jpartids = json_object_get(jemailpartids, msgid);
        if (jpartids) {
            json_t *jpartid;
            size_t j;
            json_array_foreach(jpartids, j, jpartid) {
                strarray_append(&partids, json_string_value(jpartid));
            }
        }
        json_object_set_new(snippet, "emailId", json_string(msgid));
        json_object_set_new(snippet, "subject", json_null());
        json_object_set_new(snippet, "preview", json_null());

        r = rx->begin_mailbox(rx, mbox, /*incremental*/0);
        r = index_getsearchtext(msg, jpartids ? &partids : NULL, rx, 1);
        if (!r || r == IMAP_OK_COMPLETED) {
            json_array_append_new(*snippets, json_deep_copy(snippet));
            r = 0;
        }
        int r2 = rx->end_mailbox(rx, mbox);
        if (!r) r = r2;

        json_object_clear(snippet);
        strarray_truncate(&partids, 0);
        msgrecord_unref(&mr);

doneloop:
        if (mr) msgrecord_unref(&mr);
        jmap_closembox(req, &mbox);
        free(mboxname);
        mboxname = NULL;
        if (r) goto done;
    }

    if (!json_array_size(*notfound)) {
        json_decref(*notfound);
        *notfound = json_null();
    }

done:
    if (rx) search_end_snippets(rx);
    if (snippet) json_decref(snippet);
    if (intquery) search_free_internalised(intquery);
    if (mboxname) free(mboxname);
    if (mbox) jmap_closembox(req, &mbox);
    if (searchargs) freesearchargs(searchargs);
    strarray_fini(&partids);
    index_close(&state);

    return r;
}

static int _email_filter_contains_text(json_t *filter)
{
    if (JNOTNULL(filter)) {
        json_t *val;
        size_t i;

        if (JNOTNULL(json_object_get(filter, "text"))) {
            return 1;
        }
        if (JNOTNULL(json_object_get(filter, "subject"))) {
            return 1;
        }
        if (JNOTNULL(json_object_get(filter, "body"))) {
            return 1;
        }
        if (JNOTNULL(json_object_get(filter, "attachmentBody"))) {
            return 1;
        }

        /* We don't generate snippets for headers, but we
         * might find header text in the body or subject again. */
        if (JNOTNULL(json_object_get(filter, "header"))) {
            return 1;
        }
        if (JNOTNULL(json_object_get(filter, "from"))) {
            return 1;
        }
        if (JNOTNULL(json_object_get(filter, "to"))) {
            return 1;
        }
        if (JNOTNULL(json_object_get(filter, "cc"))) {
            return 1;
        }
        if (JNOTNULL(json_object_get(filter, "bcc"))) {
            return 1;
        }

        json_array_foreach(json_object_get(filter, "conditions"), i, val) {
            if (_email_filter_contains_text(val)) {
                return 1;
            }
        }
    }
    return 0;
}

static int jmap_searchsnippet_get(jmap_req_t *req)
{
    int r = 0;
    const char *key;
    json_t *arg, *jfilter = NULL, *jmessageids = NULL, *jemailpartids = NULL;
    json_t *snippets, *notfound;
    struct buf buf = BUF_INITIALIZER;
    struct jmap_parser parser = JMAP_PARSER_INITIALIZER;
    struct email_contactfilter contactfilter;
    json_t *err = NULL;

    _email_contactfilter_initreq(req, &contactfilter);

    /* Parse and validate arguments. */
    json_t *unsupported_filter = json_pack("[]");

    json_object_foreach(req->args, key, arg) {
        if (!strcmp(key, "accountId")) {
            /* already handled in jmap_api() */
        }

        /* filter */
        else if (!strcmp(key, "filter")) {
            jfilter = arg;
            if (JNOTNULL(jfilter)) {
                jmap_parser_push(&parser, "filter");
                jmap_filter_parse(req, &parser, jfilter, unsupported_filter,
                                  _email_parse_filter_cb, &contactfilter, &err);
                jmap_parser_pop(&parser);
                if (err) break;
            }
        }

        /* messageIds */
        else if (!strcmp(key, "emailIds")) {
            jmessageids = arg;
            if (json_array_size(jmessageids)) {
                jmap_parse_strings(jmessageids, &parser, "emailIds");
            }
            else if (!json_is_array(jmessageids)) {
                jmap_parser_invalid(&parser, "emailIds");
            }
        }

        /* partIds */
        else if (jmap_is_using(req, JMAP_SEARCH_EXTENSION) && !strcmp(key, "partIds")) {
            jemailpartids = arg;
            int is_valid = 1;
            if (json_is_object(jemailpartids)) {
                const char *email_id;
                json_t *jpartids;
                json_object_foreach(jemailpartids, email_id, jpartids) {
                    if (json_is_array(jpartids)) {
                        size_t i;
                        json_t *jpartid;
                        json_array_foreach(jpartids, i, jpartid) {
                            if (!json_is_string(jpartid)) {
                                is_valid = 0;
                                break;
                            }
                        }
                    }
                    else if (json_is_null(jpartids)) {
                        /* JSON null means: no parts */
                        continue;
                    }
                    if (!is_valid) break;
                }
            }
            else is_valid = json_is_null(jemailpartids);
            if (!is_valid) {
                jmap_parser_invalid(&parser, "partIds");
            }
        }
        else jmap_parser_invalid(&parser, key);
        if (!json_object_size(jemailpartids)) {
            jemailpartids = NULL;
        }
    }

    /* Bail out for argument errors */
    if (err) {
        jmap_error(req, err);
        json_decref(unsupported_filter);
        goto done;
    }
    else if (json_array_size(parser.invalid)) {
        jmap_error(req, json_pack("{s:s, s:O}", "type", "invalidArguments",
                    "arguments", parser.invalid));
        json_decref(unsupported_filter);
        goto done;
    }
    /* Report unsupported filters */
    if (json_array_size(unsupported_filter)) {
        jmap_error(req, json_pack("{s:s, s:o}", "type", "unsupportedFilter",
                    "filters", unsupported_filter));
        goto done;
    }
    json_decref(unsupported_filter);

    if (json_array_size(jmessageids) && _email_filter_contains_text(jfilter)) {
        /* Render snippets */
        r = _snippet_get(req, jfilter, jmessageids, jemailpartids, &snippets, &notfound);
        if (r) goto done;
    } else {
        /* Trivial, snippets cant' match */
        size_t i;
        json_t *val;

        snippets = json_pack("[]");
        notfound = json_null();

        json_array_foreach(jmessageids, i, val) {
            json_array_append_new(snippets, json_pack("{s:s s:n s:n}",
                        "emailId", json_string_value(val),
                        "subject", "preview"));
        }
    }

    /* Prepare response. */
    json_t *res = json_pack("{s:o s:o}",
                            "list", snippets, "notFound", notfound);
    if (jfilter) json_object_set(res, "filter", jfilter);
    jmap_ok(req, res);

done:
    jmap_email_contactfilter_fini(&contactfilter);
    jmap_parser_fini(&parser);
    buf_free(&buf);
    return r;
}

static int _thread_is_shared_cb(const conv_guidrec_t *rec, void *rock)
{
    if (rec->part) return 0;
    jmap_req_t *req = (jmap_req_t *)rock;
    static int needrights = ACL_READ|ACL_LOOKUP;
    if (jmap_hasrights_byname(req, rec->mboxname, needrights))
        return IMAP_OK_COMPLETED;
    return 0;
}

static int _thread_get(jmap_req_t *req, json_t *ids,
                       json_t *list, json_t *not_found)
{
    conversation_t conv = CONVERSATION_INIT;
    json_t *val;
    size_t i;
    int r = 0;

    json_array_foreach(ids, i, val) {
        conv_thread_t *thread;
        char email_id[JMAP_EMAILID_SIZE];

        const char *threadid = json_string_value(val);

        memset(&conv, 0, sizeof(conversation_t));
        r = conversation_load_advanced(req->cstate, _cid_from_id(threadid),
                                       &conv, CONV_WITHTHREAD);
        if (r || !conv.thread) {
            json_array_append_new(not_found, json_string(threadid));
            continue;
        }

        int is_own_account = !strcmp(req->userid, req->accountid);
        json_t *ids = json_pack("[]");
        for (thread = conv.thread; thread; thread = thread->next) {
            if (!is_own_account) {
                const char *guidrep = message_guid_encode(&thread->guid);
                int r = conversations_guid_foreach(req->cstate, guidrep,
                                                   _thread_is_shared_cb, req);
                if (r != IMAP_OK_COMPLETED) {
                    if (r) {
                        syslog(LOG_ERR, "jmap: _thread_is_shared_cb(%s): %s",
                                guidrep, error_message(r));
                    }
                    continue;
                }
            }
            jmap_set_emailid(&thread->guid, email_id);
            json_array_append_new(ids, json_string(email_id));
        }

        /* if we didn't find any visible IDs, then the thread doesn't really
           exist for this user */
        if (!json_array_size(ids)) {
            json_decref(ids);
            json_array_append_new(not_found, json_string(threadid));
            continue;
        }

        json_t *jthread = json_pack("{s:s s:o}", "id", threadid, "emailIds", ids);
        json_array_append_new(list, jthread);

        conversation_fini(&conv);
    }

    r = 0;

    conversation_fini(&conv);
    return r;
}

static const jmap_property_t thread_props[] = {
    {
        "id",
        NULL,
        JMAP_PROP_IMMUTABLE | JMAP_PROP_ALWAYS_GET
    },
    {
        "emailIds",
        NULL,
        0
    },
    { NULL, NULL, 0 }
};

static int jmap_thread_get(jmap_req_t *req)
{
    struct jmap_parser parser = JMAP_PARSER_INITIALIZER;
    struct jmap_get get;
    json_t *err = NULL;

    /* Parse request */
    jmap_get_parse(req, &parser, thread_props, /*allow_null_ids*/0,
                   NULL, NULL, &get, &err);
    if (err) {
        jmap_error(req, err);
        goto done;
    }

    /* Find threads */
    int r = _thread_get(req, get.ids, get.list, get.not_found);
    if (r) {
        syslog(LOG_ERR, "jmap: Thread/get: %s", error_message(r));
        jmap_error(req, jmap_server_error(r));
        goto done;
    }

    json_t *jstate = jmap_getstate(req, MBTYPE_EMAIL, /*refresh*/0);
    get.state = xstrdup(json_string_value(jstate));
    json_decref(jstate);
    jmap_ok(req, jmap_get_reply(&get));

done:
    jmap_parser_fini(&parser);
    jmap_get_fini(&get);
    return 0;
}

struct email_getcontext {
    struct seen *seendb;           /* Seen database for shared accounts */
    hash_table seenseq_by_mbox_id; /* Cached seen sequences */
};

static void _email_getcontext_fini(struct email_getcontext *ctx)
{
    free_hash_table(&ctx->seenseq_by_mbox_id, (void(*)(void*))seqset_free);
    seen_close(&ctx->seendb);
}

struct email_getargs {
    /* Email/get arguments */
    hash_table *props; /* owned by JMAP get or process stack */
    hash_table *bodyprops;
    ptrarray_t want_headers;     /* array of header_prop */
    ptrarray_t want_bodyheaders; /* array of header_prop */
    short fetch_text_body;
    short fetch_html_body;
    short fetch_all_body;
    size_t max_body_bytes;
    /* Request-scoped context */
    struct email_getcontext ctx;
};

#define _EMAIL_GET_ARGS_INITIALIZER \
    { \
        NULL, \
        NULL, \
        PTRARRAY_INITIALIZER, \
        PTRARRAY_INITIALIZER, \
        0, \
        0, \
        0, \
        0, \
        { \
            NULL, \
            HASH_TABLE_INITIALIZER \
        } \
    };

/* Initialized in email_get_parse. *Not* thread-safe */
static hash_table _email_get_default_bodyprops = HASH_TABLE_INITIALIZER;
static hash_table _email_parse_default_props = HASH_TABLE_INITIALIZER;

static void _email_getargs_fini(struct email_getargs *args)
{
    if (args->bodyprops && args->bodyprops != &_email_get_default_bodyprops) {
        free_hash_table(args->bodyprops, NULL);
        free(args->bodyprops);
    }
    args->bodyprops = NULL;

    struct header_prop *prop;
    while ((prop = ptrarray_pop(&args->want_headers))) {
        _header_prop_fini(prop);
        free(prop);
    }
    ptrarray_fini(&args->want_headers);
    while ((prop = ptrarray_pop(&args->want_bodyheaders))) {
        _header_prop_fini(prop);
        free(prop);
    }
    ptrarray_fini(&args->want_bodyheaders);
    _email_getcontext_fini(&args->ctx);
}

/* A wrapper to aggregate JMAP keywords over a set of message records.
 * Notably the $seen keyword is a pain to map from IMAP to JMAP:
 * (1) it must only be reported if the IMAP \Seen flag is set on
 *     all non-deleted index records.
 * (2) it must be read from seen.db for shared mailboxes
 */
struct email_keywords {
    const char *userid;
    hash_table counts;
    size_t totalmsgs;
    hash_table *seenseq_by_mbox_id;
    struct seen *seendb;
};

#define _EMAIL_KEYWORDS_INITIALIZER { NULL, HASH_TABLE_INITIALIZER, 0, NULL, NULL }

/* Initialize the keyword aggregator for the authenticated userid.
 *
 * The seenseq hash table is used to read cached sequence sets
 * read from seen.db per mailbox. If the hash table does not
 * contain a sequence for the respective mailbox id, it is read
 * from seen.db and stored in the map.
 * Callers must free any entries in seenseq_by_mbox_id. */
static void _email_keywords_init(struct email_keywords *keywords,
                                 const char *userid,
                                 struct seen *seendb,
                                 hash_table *seenseq_by_mbox_id)
{
    construct_hash_table(&keywords->counts, 64, 0);
    keywords->userid = userid;
    keywords->seendb = seendb;
    keywords->seenseq_by_mbox_id = seenseq_by_mbox_id;
}

static void _email_keywords_fini(struct email_keywords *keywords)
{
    free_hash_table(&keywords->counts, NULL);
}

static void _email_keywords_add_keyword(struct email_keywords *keywords,
                                        const char *keyword)
{
    uintptr_t count = (uintptr_t) hash_lookup(keyword, &keywords->counts);
    hash_insert(keyword, (void*) count+1, &keywords->counts);
}

static int _email_keywords_add_msgrecord(struct email_keywords *keywords,
                                         msgrecord_t *mr)
{
    uint32_t uid, system_flags, internal_flags;
    uint32_t user_flags[MAX_USER_FLAGS/32];
    struct mailbox *mbox = NULL;

    int r = msgrecord_get_uid(mr, &uid);
    if (r) goto done;
    r = msgrecord_get_mailbox(mr, &mbox);
    if (r) goto done;
    r = msgrecord_get_systemflags(mr, &system_flags);
    if (r) goto done;
    r = msgrecord_get_internalflags(mr, &internal_flags);
    if (r) goto done;
    if (system_flags & FLAG_DELETED || internal_flags & FLAG_INTERNAL_EXPUNGED) goto done;
    r = msgrecord_get_userflags(mr, user_flags);
    if (r) goto done;

    int read_seendb = !mailbox_internal_seen(mbox, keywords->userid);

    /* Read system flags */
    if ((system_flags & FLAG_DRAFT))
        _email_keywords_add_keyword(keywords, "$draft");
    if ((system_flags & FLAG_FLAGGED))
        _email_keywords_add_keyword(keywords, "$flagged");
    if ((system_flags & FLAG_ANSWERED))
        _email_keywords_add_keyword(keywords, "$answered");
    if (!read_seendb && system_flags & FLAG_SEEN)
        _email_keywords_add_keyword(keywords, "$seen");

    /* Read user flags */
    struct buf buf = BUF_INITIALIZER;
    int i;
    for (i = 0 ; i < MAX_USER_FLAGS ; i++) {
        if (mbox->flagname[i] && (user_flags[i/32] & 1<<(i&31))) {
            buf_setcstr(&buf, mbox->flagname[i]);
            _email_keywords_add_keyword(keywords, buf_lcase(&buf));
        }
    }
    buf_free(&buf);

    if (read_seendb) {
        /* Read $seen keyword from seen.db for shared accounts */
        struct seqset *seenseq = hash_lookup(mbox->uniqueid, keywords->seenseq_by_mbox_id);
        if (!seenseq) {
            struct seendata sd = SEENDATA_INITIALIZER;
            int r = seen_read(keywords->seendb, mbox->uniqueid, &sd);
            if (!r) {
                seenseq = seqset_parse(sd.seenuids, NULL, sd.lastuid);
                hash_insert(mbox->uniqueid, seenseq, keywords->seenseq_by_mbox_id);
                seen_freedata(&sd);
            }
            else {
                syslog(LOG_ERR, "Could not read seen state for %s (%s)",
                        keywords->userid, error_message(r));
            }
        }

        if (seenseq && seqset_ismember(seenseq, uid))
            _email_keywords_add_keyword(keywords, "$seen");
    }

    /* Count message */
    keywords->totalmsgs++;

done:
    return r;
}

static json_t *_email_keywords_to_jmap(struct email_keywords *keywords)
{
    json_t *jkeywords = json_object();
    hash_iter *kwiter = hash_table_iter(&keywords->counts);
    while (hash_iter_next(kwiter)) {
        const char *keyword = hash_iter_key(kwiter);
        uintptr_t count = (uintptr_t) hash_iter_val(kwiter);
        if (strcasecmp(keyword, "$seen") || count == keywords->totalmsgs) {
            json_object_set_new(jkeywords, keyword, json_true());
        }
    }
    hash_iter_free(&kwiter);
    return jkeywords;
}


struct email_get_keywords_rock {
    jmap_req_t *req;
    struct email_keywords keywords;
};

static int _email_get_keywords_cb(const conv_guidrec_t *rec, void *vrock)
{
    struct email_get_keywords_rock *rock = vrock;
    jmap_req_t *req = rock->req;
    struct mailbox *mbox = NULL;
    msgrecord_t *mr = NULL;

    if (rec->part) return 0;

    if (!jmap_hasrights_byname(req, rec->mboxname, ACL_READ|ACL_LOOKUP)) return 0;

    /* Fetch system flags */
    int r = jmap_openmbox(req, rec->mboxname, &mbox, 0);
    if (r) return r;

    r = msgrecord_find(mbox, rec->uid, &mr);
    if (r) goto done;

    r = _email_keywords_add_msgrecord(&rock->keywords, mr);

done:
    msgrecord_unref(&mr);
    jmap_closembox(req, &mbox);
    return r;
}

static int _email_get_keywords(jmap_req_t *req,
                               struct email_getcontext *ctx,
                               const char *msgid,
                               json_t **jkeywords)
{
    /* Initialize seen.db and sequence set cache */
    if (ctx->seendb == NULL && strcmp(req->accountid, req->userid)) {
        int r = seen_open(req->userid, SEEN_CREATE, &ctx->seendb);
        if (r) return r;
    }
    if (ctx->seenseq_by_mbox_id.size == 0) {
        construct_hash_table(&ctx->seenseq_by_mbox_id, 128, 0);
    }
    /* Gather keywords for all message records */
    struct email_get_keywords_rock rock = { req, _EMAIL_KEYWORDS_INITIALIZER };
    _email_keywords_init(&rock.keywords, req->userid, ctx->seendb, &ctx->seenseq_by_mbox_id);
    int r = conversations_guid_foreach(req->cstate, _guid_from_id(msgid),
                                       _email_get_keywords_cb, &rock);
    *jkeywords = _email_keywords_to_jmap(&rock.keywords);
    _email_keywords_fini(&rock.keywords);
    return r;
}

struct email_get_snoozed_rock {
    jmap_req_t *req;
    json_t *snoozed;
};

static int _email_get_snoozed_cb(const conv_guidrec_t *rec, void *vrock)
{
    struct email_get_snoozed_rock *rock = vrock;

    if (rec->part) return 0;

    if (!jmap_hasrights_byname(rock->req, rec->mboxname, ACL_READ|ACL_LOOKUP))
        return 0;

    if (FLAG_INTERNAL_SNOOZED ==
        (rec->internal_flags & (FLAG_INTERNAL_SNOOZED|FLAG_INTERNAL_EXPUNGED))) {
        /* Fetch snoozed annotation */
        rock->snoozed = jmap_fetch_snoozed(rec->mboxname, rec->uid);
    }

    /* Short-circuit the foreach if we find a snoozed message */
    return (rock->snoozed != NULL);
}

static void _email_parse_wantheaders(json_t *jprops,
                                     struct jmap_parser *parser,
                                     const char *prop_name,
                                     ptrarray_t *want_headers)
{
    size_t i;
    json_t *jval;
    json_array_foreach(jprops, i, jval) {
        const char *s = json_string_value(jval);
        if (!s || strncmp(s, "header:", 7))
            continue;
        struct header_prop *hprop;
        if ((hprop = _header_parseprop(s))) {
            ptrarray_append(want_headers, hprop);
        }
        else {
            jmap_parser_push_index(parser, prop_name, i, s);
            jmap_parser_invalid(parser, NULL);
            jmap_parser_pop(parser);
        }
    }
}

static void _email_init_default_props(hash_table *props)
{
    /* Initialize process-owned default property list */
    construct_hash_table(props, 32, 0);
    if (props == &_email_get_default_bodyprops) {
        hash_insert("blobId",      (void*)1, props);
        hash_insert("charset",     (void*)1, props);
        hash_insert("cid",         (void*)1, props);
        hash_insert("disposition", (void*)1, props);
        hash_insert("language",    (void*)1, props);
        hash_insert("location",    (void*)1, props);
        hash_insert("name",        (void*)1, props);
        hash_insert("partId",      (void*)1, props);
        hash_insert("size",        (void*)1, props);
        hash_insert("type",        (void*)1, props);
    }
    else {
        hash_insert("attachments",   (void*)1, props);
        hash_insert("bcc",           (void*)1, props);
        hash_insert("bodyValues",    (void*)1, props);
        hash_insert("cc",            (void*)1, props);
        hash_insert("from",          (void*)1, props);
        hash_insert("hasAttachment", (void*)1, props);
        hash_insert("htmlBody",      (void*)1, props);
        hash_insert("inReplyTo",     (void*)1, props);
        hash_insert("messageId",     (void*)1, props);
        hash_insert("preview",       (void*)1, props);
        hash_insert("references",    (void*)1, props);
        hash_insert("replyTo",       (void*)1, props);
        hash_insert("sender",        (void*)1, props);
        hash_insert("sentAt",        (void*)1, props);
        hash_insert("subject",       (void*)1, props);
        hash_insert("textBody",      (void*)1, props);
        hash_insert("to",            (void*)1, props);
    }
}

static int _email_getargs_parse(jmap_req_t *req __attribute__((unused)),
                                struct jmap_parser *parser,
                                const char *key,
                                json_t *arg,
                                void *rock)
{
    struct email_getargs *args = (struct email_getargs *) rock;
    int r = 1;

    /* bodyProperties */
    if (!strcmp(key, "bodyProperties")) {
        if (jmap_parse_strings(arg, parser, "bodyProperties")) {
            size_t i;
            json_t *val;

            args->bodyprops = xzmalloc(sizeof(hash_table));
            construct_hash_table(args->bodyprops, json_array_size(arg) + 1, 0);
            json_array_foreach(arg, i, val) {
                hash_insert(json_string_value(val), (void*)1, args->bodyprops);
            }
        }
        /* header:Xxx properties */
        _email_parse_wantheaders(arg, parser, "bodyProperties",
                                 &args->want_bodyheaders);
    }

    /* fetchTextBodyValues */
    else if (!strcmp(key, "fetchTextBodyValues") && json_is_boolean(arg)) {
        args->fetch_text_body = json_boolean_value(arg);
    }

    /* fetchHTMLBodyValues */
    else if (!strcmp(key, "fetchHTMLBodyValues") && json_is_boolean(arg)) {
        args->fetch_html_body = json_boolean_value(arg);
    }

    /* fetchAllBodyValues */
    else if (!strcmp(key, "fetchAllBodyValues") && json_is_boolean(arg)) {
        args->fetch_all_body = json_boolean_value(arg);
    }

    /* maxBodyValueBytes */
    else if (!strcmp(key, "maxBodyValueBytes") &&
             json_is_integer(arg) && json_integer_value(arg) > 0) {
        args->max_body_bytes = json_integer_value(arg);
    }

    else r = 0;

    return r;
}

struct cyrusmsg {
    msgrecord_t *mr;                 /* Message record for top-level message */
    const struct body *part0;        /* Root body-part */
    const struct body *rfc822part;   /* RFC822 root part for embedded message */
    const struct buf *mime;          /* Raw MIME buffer */
    json_t *imagesize_by_part;       /* FastMail-specific extension */

    message_t *_m;                   /* Message loaded from message record */
    struct body *_mybody;            /* Bodystructure */
    struct buf _mymime;              /* Raw MIME buffer */
    struct headers *_headers;        /* Parsed part0 headers. Don't free. */
    hash_table *_headers_by_part_id; /* Parsed subpart headers. Don't free. */
    ptrarray_t _headers_mempool;     /* Allocated headers memory */
};

static void _cyrusmsg_free(struct cyrusmsg **msgptr)
{
    if (msgptr == NULL || *msgptr == NULL) return;

    struct cyrusmsg *msg = *msgptr;
    if (msg->_mybody) {
        message_free_body(msg->_mybody);
        free(msg->_mybody);
    }
    buf_free(&msg->_mymime);
    json_decref(msg->imagesize_by_part);
    if (msg->_headers_by_part_id) {
        free_hash_table(msg->_headers_by_part_id, NULL);
        free(msg->_headers_by_part_id);
    }
    struct headers *hdrs;
    while ((hdrs = ptrarray_pop(&msg->_headers_mempool))) {
        _headers_fini(hdrs);
        free(hdrs);
    }
    ptrarray_fini(&msg->_headers_mempool);
    free(*msgptr);
    *msgptr = NULL;
}

static int _cyrusmsg_from_record(msgrecord_t *mr, struct cyrusmsg **msgptr)
{
    struct cyrusmsg *msg = xzmalloc(sizeof(struct cyrusmsg));
    msg->mr = mr;
    *msgptr = msg;
    return 0;
}

static int _cyrusmsg_from_bodypart(msgrecord_t *mr,
                                   struct body *body,
                                   const struct body *part,
                                   struct cyrusmsg **msgptr)
{
    struct cyrusmsg *msg = xzmalloc(sizeof(struct cyrusmsg));
    msg->mr = mr;
    msg->part0 = body;
    msg->rfc822part = part;
    *msgptr = msg;
    return 0;
}

static void _cyrusmsg_init_partids(struct body *body, const char *part_id)
{
    if (!body) return;

    if (!strcmp(body->type, "MULTIPART")) {
        struct buf buf = BUF_INITIALIZER;
        int i;
        for (i = 0; i < body->numparts; i++) {
            struct body *subpart = body->subpart + i;
            if (part_id) buf_printf(&buf, "%s.", part_id);
            buf_printf(&buf, "%d", i + 1);
            subpart->part_id = buf_release(&buf);
            _cyrusmsg_init_partids(subpart, subpart->part_id);
        }
        free(body->part_id);
        body->part_id = NULL;
        buf_free(&buf);
    }
    else {
        struct buf buf = BUF_INITIALIZER;
        if (!body->part_id) {
            if (part_id) buf_printf(&buf, "%s.", part_id);
            buf_printf(&buf, "%d", 1);
            body->part_id = buf_release(&buf);
        }
        buf_free(&buf);

        if (!strcmp(body->type, "MESSAGE") &&
            !strcmp(body->subtype, "RFC822")) {
            _cyrusmsg_init_partids(body->subpart, body->part_id);
        }
    }
}


static int _cyrusmsg_from_buf(const struct buf *buf, struct cyrusmsg **msgptr)
{
    /* No more return from here */
    struct body *mybody = xzmalloc(sizeof(struct body));
    struct protstream *pr = prot_readmap(buf_base(buf), buf_len(buf));

    /* Pre-run compliance check */
    int r = message_copy_strict(pr, /*to*/NULL, buf_len(buf), /*allow_null*/0);
    if (r) goto done;

    /* Parse message */
    r = message_parse_mapped(buf_base(buf), buf_len(buf), mybody);
    if (r || !mybody->subpart) {
        r = IMAP_MESSAGE_BADHEADER;
        goto done;
    }

    /* parse_mapped doesn't set part ids */
    _cyrusmsg_init_partids(mybody->subpart, NULL);

    struct cyrusmsg *msg = xzmalloc(sizeof(struct cyrusmsg));
    msg->_mybody = mybody;
    msg->part0 = mybody;
    msg->rfc822part = mybody;
    msg->mime = buf;
    *msgptr = msg;

done:
    if (pr) prot_free(pr);
    if (r && mybody) {
        message_free_body(mybody);
        free(mybody);
    }
    return r;
}

static int _cyrusmsg_need_part0(struct cyrusmsg *msg)
{
    if (msg->part0)
        return 0;
    if (!msg->mr)
        return IMAP_INTERNAL;

    int r = msgrecord_extract_bodystructure(msg->mr, &msg->_mybody);
    if (r) return r;
    msg->part0 = msg->_mybody;
    return 0;
}

static int _cyrusmsg_need_mime(struct cyrusmsg *msg)
{
    if (msg->mime) return 0;
    if (msg->mr == NULL) {
        return IMAP_INTERNAL;
    }
    int r = msgrecord_get_body(msg->mr, &msg->_mymime);
    msg->mime = &msg->_mymime;
    if (r) return r;
    return 0;
}

static int _cyrusmsg_get_headers(struct cyrusmsg *msg,
                                 const struct body *part,
                                 struct headers **headersptr)
{

    if (part == NULL && msg->_headers) {
        *headersptr = msg->_headers;
        return 0;
    }
    else if (part && part->part_id) {
        if (msg->_headers_by_part_id) {
            *headersptr = hash_lookup(part->part_id, msg->_headers_by_part_id);
            if (*headersptr) return 0;
        }
        else {
            msg->_headers_by_part_id = xzmalloc(sizeof(hash_table));
            construct_hash_table(msg->_headers_by_part_id, 64, 0);
        }
    }

    /* Prefetch body structure */
    int r = _cyrusmsg_need_part0(msg);
    if (r) return r;
    /* Prefetch MIME message */
    r = _cyrusmsg_need_mime(msg);
    if (r) return r;
    const struct body *header_part = part ? part : msg->part0;
    struct headers *headers = xmalloc(sizeof(struct headers));
    _headers_init(headers);
    _headers_from_mime(msg->mime->s + header_part->header_offset,
                       header_part->header_size, headers);
    if (part && part->part_id)
        hash_insert(part->part_id, headers, msg->_headers_by_part_id);
    else if (part == NULL)
        msg->_headers = headers;
    ptrarray_append(&msg->_headers_mempool, headers);
    *headersptr = headers;
    return 0;
}

static json_t * _email_get_header(struct cyrusmsg *msg,
                                  const struct body *part,
                                  const char *lcasename,
                                  enum _header_form want_form,
                                  int want_all)
{
    if (!part) {
        /* Fetch bodypart */
        int r = _cyrusmsg_need_part0(msg);
        if (r) return json_null();
        part = msg->part0;
    }

    /* Try to read the header from the parsed body part */
    if (part && !want_all && want_form != HEADER_FORM_RAW) {
        json_t *jval = NULL;
        if (!strcmp("messageId", lcasename)) {
            jval = want_form == HEADER_FORM_MESSAGEIDS ?
                _header_as_messageids(part->message_id) : json_null();
        }
        else if (!strcmp("inReplyTo", lcasename)) {
            jval = want_form == HEADER_FORM_MESSAGEIDS ?
                _header_as_messageids(part->in_reply_to) : json_null();
        }
        if (!strcmp("subject", lcasename)) {
            jval = want_form == HEADER_FORM_TEXT ?
                _header_as_text(part->subject) : json_null();
        }
        if (!strcmp("from", lcasename)) {
            jval = want_form == HEADER_FORM_ADDRESSES ?
                _emailaddresses_from_addr(part->from) : json_null();
        }
        else if (!strcmp("to", lcasename)) {
            jval = want_form == HEADER_FORM_ADDRESSES ?
                _emailaddresses_from_addr(part->to) : json_null();
        }
        else if (!strcmp("cc", lcasename)) {
            jval = want_form == HEADER_FORM_ADDRESSES ?
                _emailaddresses_from_addr(part->cc) : json_null();
        }
        else if (!strcmp("bcc", lcasename)) {
            jval = want_form == HEADER_FORM_ADDRESSES ?
                _emailaddresses_from_addr(part->bcc) : json_null();
        }
        else if (!strcmp("sentAt", lcasename)) {
            jval = json_null();
            if (want_form == HEADER_FORM_DATE) {
                struct offsettime t;
                if (offsettime_from_rfc5322(part->date, &t, DATETIME_FULL) != -1) {
                    char datestr[30];
                    offsettime_to_iso8601(&t, datestr, 30, 1);
                    jval = json_string(datestr);
                }
            }
        }
        if (jval) return jval;
    }

    /* Determine header form converter */
    json_t* (*conv)(const char *raw);
    switch (want_form) {
        case HEADER_FORM_TEXT:
            conv = _header_as_text;
            break;
        case HEADER_FORM_DATE:
            conv = _header_as_date;
            break;
        case HEADER_FORM_ADDRESSES:
            conv = _header_as_addresses;
            break;
        case HEADER_FORM_MESSAGEIDS:
            conv = _header_as_messageids;
            break;
        case HEADER_FORM_URLS:
            conv = _header_as_urls;
            break;
        default:
            conv = _header_as_raw;
    }

    /* Try to read the value from the index record or header cache */
    if (msg->mr && part == msg->part0 && !want_all && want_form != HEADER_FORM_RAW) {
        if (!msg->_m) {
            int r = msgrecord_get_message(msg->mr, &msg->_m);
            if (r) return json_null();
        }
        struct buf buf = BUF_INITIALIZER;
        int r = message_get_field(msg->_m, lcasename, MESSAGE_RAW|MESSAGE_LAST, &buf);
        if (r) return json_null();
        json_t *jval = NULL;
        if (buf_len(&buf)) jval = conv(buf_cstring(&buf));
        buf_free(&buf);
        if (jval) return jval;
    }

    /* Read the raw MIME headers */
    struct headers *partheaders = NULL;
    int r = _cyrusmsg_get_headers(msg, part, &partheaders);
    if (r) return json_null();

    /* Lookup array of EmailHeader objects by name */
    json_t *jheaders = json_object_get(partheaders->all, lcasename);
    if (!jheaders || !json_array_size(jheaders)) {
        return want_all ? json_array() : json_null();
    }

    /* Convert header values */
    if (want_all) {
        json_t *allvals = json_array();
        size_t i;
        for (i = 0; i < json_array_size(jheaders); i++) {
            json_t *jheader = json_array_get(jheaders, i);
            json_t *jheaderval = json_object_get(jheader, "value");
            json_array_append_new(allvals, conv(json_string_value(jheaderval)));
        }
        return allvals;
    }

    json_t *jheader = json_array_get(jheaders, json_array_size(jheaders) - 1);
    json_t *jheaderval = json_object_get(jheader, "value");
    return conv(json_string_value(jheaderval));
}

static int _email_get_meta(jmap_req_t *req,
                           struct email_getargs *args,
                           struct cyrusmsg *msg,
                           json_t *email)
{
    int r = 0;
    hash_table *props = args->props;
    char email_id[JMAP_EMAILID_SIZE];

    if (msg->rfc822part) {
        if (jmap_wantprop(props, "id")) {
            json_object_set_new(email, "id", json_null());
        }
        if (jmap_wantprop(props, "blobId")) {
            char blob_id[JMAP_BLOBID_SIZE];
            jmap_set_blobid(&msg->rfc822part->content_guid, blob_id);
            json_object_set_new(email, "blobId", json_string(blob_id));
        }
        if (jmap_wantprop(props, "threadId"))
            json_object_set_new(email, "threadId", json_null());
        if (jmap_wantprop(props, "mailboxIds"))
            json_object_set_new(email, "mailboxIds", json_null());
        if (jmap_wantprop(props, "keywords"))
            json_object_set_new(email, "keywords", json_object());
        if (jmap_wantprop(props, "size")) {
            size_t size = msg->rfc822part->header_size + msg->rfc822part->content_size;
            json_object_set_new(email, "size", json_integer(size));
        }
        if (jmap_wantprop(props, "receivedAt"))
            json_object_set_new(email, "receivedAt", json_null());
        return 0;
    }

    /* This is a top-level messages with a regular index record. */

    /* Determine message id */
    struct message_guid guid;
    r = msgrecord_get_guid(msg->mr, &guid);
    if (r) goto done;

    jmap_set_emailid(&guid, email_id);

    /* id */
    if (jmap_wantprop(props, "id")) {
        json_object_set_new(email, "id", json_string(email_id));
    }

    /* blobId */
    if (jmap_wantprop(props, "blobId")) {
        char blob_id[JMAP_BLOBID_SIZE];
        jmap_set_blobid(&guid, blob_id);
        json_object_set_new(email, "blobId", json_string(blob_id));
    }

    /* threadid */
    if (jmap_wantprop(props, "threadId")) {
        bit64 cid;
        r = msgrecord_get_cid(msg->mr, &cid);
        if (r) goto done;
        char thread_id[JMAP_THREADID_SIZE];
        jmap_set_threadid(cid, thread_id);
        json_object_set_new(email, "threadId", json_string(thread_id));
    }

    /* mailboxIds */
    if (jmap_wantprop(props, "mailboxIds") ||
        jmap_wantprop(props, "addedDates") || jmap_wantprop(props, "removedDates")) {
        json_t *mboxids =
            jmap_wantprop(props, "mailboxIds") ? json_object() : NULL;
        json_t *added =
            jmap_wantprop(props, "addedDates") ? json_object() : NULL;
        json_t *removed =
            jmap_wantprop(props, "removedDates") ? json_object() : NULL;
        json_t *mailboxes = _email_mailboxes(req, email_id);

        json_t *val;
        const char *mboxid;
        json_object_foreach(mailboxes, mboxid, val) {
            json_t *exists = json_object_get(val, "added");

            if (exists) {
                if (mboxids) json_object_set_new(mboxids, mboxid, json_true());
                if (added) json_object_set(added, mboxid, exists);
            }
            else if (removed) {
                json_object_set(removed, mboxid, json_object_get(val, "removed"));
            }
        }
        json_decref(mailboxes);
        if (mboxids) json_object_set_new(email, "mailboxIds", mboxids);
        if (removed) json_object_set_new(email, "removedDates", removed);
        if (added) json_object_set_new(email, "addedDates", added);
    }

    /* keywords */
    if (jmap_wantprop(props, "keywords")) {
        json_t *keywords = NULL;
        r = _email_get_keywords(req, &args->ctx, email_id, &keywords);
        if (r) goto done;
        json_object_set_new(email, "keywords", keywords);
    }

    /* size */
    if (jmap_wantprop(props, "size")) {
        uint32_t size;
        r = msgrecord_get_size(msg->mr, &size);
        if (r) goto done;
        json_object_set_new(email, "size", json_integer(size));
    }

    /* receivedAt */
    if (jmap_wantprop(props, "receivedAt")) {
        char datestr[RFC3339_DATETIME_MAX];
        time_t t;
        r = msgrecord_get_internaldate(msg->mr, &t);
        if (r) goto done;
        time_to_rfc3339(t, datestr, RFC3339_DATETIME_MAX);
        json_object_set_new(email, "receivedAt", json_string(datestr));
    }

    /* FastMail-extension properties */
    if (jmap_wantprop(props, "trustedSender")) {
        json_t *trusted_sender = NULL;
        int has_trusted_flag = 0;
        r = msgrecord_hasflag(msg->mr, "$IsTrusted", &has_trusted_flag);
        if (r) goto done;
        if (has_trusted_flag) {
            struct buf buf = BUF_INITIALIZER;
            _email_read_annot(req, msg->mr, "/vendor/messagingengine.com/trusted", &buf);
            if (buf_len(&buf)) {
                trusted_sender = json_string(buf_cstring(&buf));
            }
            buf_free(&buf);
        }
        json_object_set_new(email, "trustedSender", trusted_sender ?
                trusted_sender : json_null());
    }

    if (jmap_wantprop(props, "spamScore")) {
        int r = 0;
        struct buf buf = BUF_INITIALIZER;
        json_t *jval = json_null();
        if (!msg->_m) r = msgrecord_get_message(msg->mr, &msg->_m);
        if (!r) r = message_get_field(msg->_m, "x-spam-score", MESSAGE_RAW, &buf);
        if (!r && buf_len(&buf)) jval = json_real(atof(buf_cstring(&buf)));
        json_object_set_new(email, "spamScore", jval);
        buf_free(&buf);
    }

    if (jmap_wantprop(props, "snoozed")) {
        struct email_get_snoozed_rock rock = { req, NULL };

        /* Look for the first snoozed copy of this email_id */
        conversations_guid_foreach(req->cstate, _guid_from_id(email_id),
                                   _email_get_snoozed_cb, &rock);

        json_object_set_new(email, "snoozed",
                            rock.snoozed ? rock.snoozed : json_null());
    }

done:
    return r;
}

static int _email_get_headers(jmap_req_t *req __attribute__((unused)),
                              struct email_getargs *args,
                              struct cyrusmsg *msg,
                              json_t *email)
{
    int r = 0;
    hash_table *props = args->props;

    if (jmap_wantprop(props, "headers") || args->want_headers.count) {
        /* headers */
        if (jmap_wantprop(props, "headers")) {
            struct headers *headers = NULL;
            r = _cyrusmsg_get_headers(msg, NULL, &headers);
            if (r) return r;
            json_object_set(email, "headers", headers->raw); /* incref! */
        }
        /* headers:Xxx */
        if (ptrarray_size(&args->want_headers)) {
            int i;
            for (i = 0; i < ptrarray_size(&args->want_headers); i++) {
                struct header_prop *want_header = ptrarray_nth(&args->want_headers, i);
                json_t *jheader = _email_get_header(msg, NULL, want_header->lcasename,
                                      want_header->form, want_header->all);
                json_object_set_new(email, want_header->prop, jheader);
            }
        }
    }

    /* references */
    if (jmap_wantprop(props, "references")) {
        json_t *references = _email_get_header(msg, NULL, "references",
                                               HEADER_FORM_MESSAGEIDS,/*all*/0);
        json_object_set_new(email, "references", references);
    }
    /* sender */
    if (jmap_wantprop(props, "sender")) {
        json_t *sender = _email_get_header(msg, NULL, "sender",
                                           HEADER_FORM_ADDRESSES,/*all*/0);
        json_object_set_new(email, "sender", sender);
    }
    /* replyTo */
    if (jmap_wantprop(props, "replyTo")) {
        json_t *replyTo = _email_get_header(msg, NULL, "reply-to",
                                            HEADER_FORM_ADDRESSES, /*all*/0);
        json_object_set_new(email, "replyTo", replyTo);
    }

    /* The following fields are all read from the body-part structure */
    const struct body *part = NULL;
    if (jmap_wantprop(props, "messageId") ||
        jmap_wantprop(props, "inReplyTo") ||
        jmap_wantprop(props, "from") ||
        jmap_wantprop(props, "to") ||
        jmap_wantprop(props, "cc") ||
        jmap_wantprop(props, "bcc") ||
        jmap_wantprop(props, "subject") ||
        jmap_wantprop(props, "sentAt")) {
        if (msg->rfc822part) {
            part = msg->rfc822part->subpart;
        }
        else {
            r = _cyrusmsg_need_part0(msg);
            if (r) return r;
            part = msg->part0;
        }
    }
    /* messageId */
    if (jmap_wantprop(props, "messageId")) {
        json_object_set_new(email, "messageId",
                _header_as_messageids(part->message_id));
    }
    /* inReplyTo */
    if (jmap_wantprop(props, "inReplyTo")) {
        json_object_set_new(email, "inReplyTo",
                _header_as_messageids(part->in_reply_to));
    }
    /* from */
    if (jmap_wantprop(props, "from")) {
        json_object_set_new(email, "from",
                _emailaddresses_from_addr(part->from));
    }
    /* to */
    if (jmap_wantprop(props, "to")) {
        json_object_set_new(email, "to",
                _emailaddresses_from_addr(part->to));
    }
    /* cc */
    if (jmap_wantprop(props, "cc")) {
        json_object_set_new(email, "cc",
                _emailaddresses_from_addr(part->cc));
    }
    /* bcc */
    if (jmap_wantprop(props, "bcc")) {
        json_object_set_new(email, "bcc",
                _emailaddresses_from_addr(part->bcc));
    }
    /* subject */
    if (jmap_wantprop(props, "subject")) {
        json_object_set_new(email, "subject",
                _header_as_text(part->subject));
    }
    /* sentAt */
    if (jmap_wantprop(props, "sentAt")) {
        json_t *jsent_at = json_null();
        struct offsettime t;
        if (offsettime_from_rfc5322(part->date, &t, DATETIME_FULL) != -1) {
            char datestr[30];
            offsettime_to_iso8601(&t, datestr, 30, 1);
            jsent_at = json_string(datestr);
        }
        json_object_set_new(email, "sentAt", jsent_at);
    }

    return r;
}

static json_t *_email_get_bodypart(jmap_req_t *req,
                                   struct email_getargs *args,
                                   struct cyrusmsg *msg,
                                   const struct body *part)
{
    struct buf buf = BUF_INITIALIZER;
    struct param *param;

    hash_table *bodyprops = args->bodyprops;
    ptrarray_t *want_bodyheaders = &args->want_bodyheaders;

    json_t *jbodypart = json_object();

    /* partId */
    if (jmap_wantprop(bodyprops, "partId")) {
        json_t *jpart_id = json_null();
        if (strcasecmp(part->type, "MULTIPART"))
            jpart_id = json_string(part->part_id);
        json_object_set_new(jbodypart, "partId", jpart_id);
    }

    /* blobId */
    if (jmap_wantprop(bodyprops, "blobId")) {
        json_t *jblob_id = json_null();
        if (!message_guid_isnull(&part->content_guid)) {
            char blob_id[JMAP_BLOBID_SIZE];
            jmap_set_blobid(&part->content_guid, blob_id);
            jblob_id = json_string(blob_id);
        }
        json_object_set_new(jbodypart, "blobId", jblob_id);
    }

    /* size */
    if (jmap_wantprop(bodyprops, "size")) {
        size_t size = 0;
        if (part->numparts && strcasecmp(part->type, "MESSAGE")) {
            /* Multipart */
            size = 0;
        }
        else if (part->charset_enc & 0xff) {
            if (part->decoded_content_size == 0) {
                char *tmp = NULL;
                size_t tmp_size;
                int r = _cyrusmsg_need_mime(msg);
                if (!r)  {
                    charset_decode_mimebody(msg->mime->s + part->content_offset,
                            part->content_size, part->charset_enc, &tmp, &tmp_size);
                    size = tmp_size;
                    free(tmp);
                }
            }
            else {
                size = part->decoded_content_size;
            }
        }
        else {
            size = part->content_size;
        }
        json_object_set_new(jbodypart, "size", json_integer(size));
    }

    /* headers */
    if (jmap_wantprop(bodyprops, "headers") || want_bodyheaders->count) {
        /* headers */
        if (jmap_wantprop(bodyprops, "headers")) {
            struct headers *headers = NULL;
            int r = _cyrusmsg_get_headers(msg, part, &headers);
            if (!r) {
                json_object_set(jbodypart, "headers", headers->raw); /* incref! */
            }
            else {
                json_object_set(jbodypart, "headers", json_null());
            }
        }
        /* headers:Xxx */
        if (ptrarray_size(want_bodyheaders)) {
            int i;
            for (i = 0; i < ptrarray_size(want_bodyheaders); i++) {
                struct header_prop *want_header = ptrarray_nth(want_bodyheaders, i);
                json_t *jheader = _email_get_header(msg, part, want_header->lcasename,
                                                    want_header->form, want_header->all);
                json_object_set_new(jbodypart, want_header->prop, jheader);
            }
        }
    }

    /* name */
    if (jmap_wantprop(bodyprops, "name")) {
        const char *fname = NULL;
        char *val = NULL;
        int is_extended = 0;

        /* Lookup name parameter. Disposition header has precedence */
        for (param = part->disposition_params; param; param = param->next) {
            if (!strncasecmp(param->attribute, "filename", 8)) {
                is_extended = param->attribute[8] == '*';
                fname = param->value;
                break;
            }
        }
        /* Lookup Content-Type parameters */
        if (!fname) {
            for (param = part->params; param; param = param->next) {
                if (!strncasecmp(param->attribute, "name", 4)) {
                    is_extended = param->attribute[4] == '*';
                    fname = param->value;
                    break;
                }
            }
        }

        /* Decode header value */
        if (fname && is_extended) {
            val = charset_parse_mimexvalue(fname, NULL);
        }
        if (fname && !val) {
            val = charset_parse_mimeheader(fname, CHARSET_KEEPCASE|CHARSET_MIME_UTF8);
        }
        json_object_set_new(jbodypart, "name", val ?
                json_string(val) : json_null());
        free(val);
    }

    /* type */
    if (jmap_wantprop(bodyprops, "type")) {
        buf_setcstr(&buf, part->type);
        if (part->subtype) {
            buf_appendcstr(&buf, "/");
            buf_appendcstr(&buf, part->subtype);
        }
        json_object_set_new(jbodypart, "type", json_string(buf_lcase(&buf)));
    }

    /* charset */
    if (jmap_wantprop(bodyprops, "charset")) {
        const char *charset_id = NULL;
        if (part->charset_id) {
            charset_id = part->charset_id;
        }
        else if (!strcasecmp(part->type, "TEXT")) {
            charset_id = "us-ascii";
        }
        json_object_set_new(jbodypart, "charset", charset_id ?
                json_string(charset_id) : json_null());
    }

    /* disposition */
    if (jmap_wantprop(bodyprops, "disposition")) {
        json_t *jdisp = json_null();
        if (part->disposition) {
            char *disp = lcase(xstrdup(part->disposition));
            jdisp = json_string(disp);
            free(disp);
        }
        json_object_set_new(jbodypart, "disposition", jdisp);
    }


    /* cid */
    if (jmap_wantprop(bodyprops, "cid")) {
        json_t *jcid = _email_get_header(msg, part, "content-id",
                                         HEADER_FORM_MESSAGEIDS, /*all*/0);
        json_object_set(jbodypart, "cid", json_array_size(jcid) ?
                json_array_get(jcid, 0) : json_null());
        json_decref(jcid);
    }


    /* language */
    if (jmap_wantprop(bodyprops, "language")) {
        json_t *jlanguage = json_null();
        json_t *jrawheader = _email_get_header(msg, part, "content-language",
                                               HEADER_FORM_RAW, /*all*/0);
        if (JNOTNULL(jrawheader)) {
            /* Split by space and comma and aggregate into array */
            const char *s = json_string_value(jrawheader);
            jlanguage = json_array();
            int i;
            char *tmp = charset_unfold(s, strlen(s), 0);
            strarray_t *ls = strarray_split(tmp, "\t ,", STRARRAY_TRIM);
            for (i = 0; i < ls->count; i++) {
                json_array_append_new(jlanguage, json_string(strarray_nth(ls, i)));
            }
            strarray_free(ls);
            free(tmp);
        }
        if (!json_array_size(jlanguage)) {
            json_decref(jlanguage);
            jlanguage = json_null();
        }
        json_object_set_new(jbodypart, "language", jlanguage);
        json_decref(jrawheader);
    }


    /* location */
    if (jmap_wantprop(bodyprops, "location")) {
        json_object_set_new(jbodypart, "location", part->location ?
                json_string(part->location) : json_null());
    }

    /* subParts */
    if (!strcmp(part->type, "MULTIPART")) {
        json_t *subparts = json_array();
        int i;
        for (i = 0; i < part->numparts; i++) {
            struct body *subpart = part->subpart + i;
            json_array_append_new(subparts,
                    _email_get_bodypart(req, args, msg, subpart));

        }
        json_object_set_new(jbodypart, "subParts", subparts);
    }
    else if (jmap_wantprop(bodyprops, "subParts")) {
        json_object_set_new(jbodypart, "subParts", json_array());
    }


    /* FastMail extension properties */
    if (jmap_wantprop(bodyprops, "imageSize")) {
        json_t *imagesize = json_null();
        if (msg->mr && msg->imagesize_by_part == NULL) {
            /* This is the first attempt to read the vendor annotation.
             * Load the annotation value, if any, for top-level messages.
             * Use JSON null for an unsuccessful attempt, so we know not
             * to try again. */
            msg->imagesize_by_part = _email_read_jannot(req, msg->mr,
                    "/vendor/messagingengine.com/imagesize", 1);
            if (!msg->imagesize_by_part)
                msg->imagesize_by_part = json_null();
        }
        imagesize = json_object_get(msg->imagesize_by_part, part->part_id);
        json_object_set(jbodypart, "imageSize", imagesize ? imagesize : json_null());
    }
    if (jmap_wantprop(bodyprops, "isDeleted")) {
        json_object_set_new(jbodypart, "isDeleted",
                json_boolean(!strcmp(part->type, "TEXT") &&
                             !strcmp(part->subtype, "X-ME-REMOVED-FILE")));
    }

    buf_free(&buf);
    return jbodypart;
}

static json_t * _email_get_bodyvalue(struct body *part,
                                     const struct buf *msg_buf,
                                     size_t max_body_bytes,
                                     int is_html)
{
    json_t *jbodyvalue = NULL;
    int is_encoding_problem = 0;
    int is_truncated = 0;
    struct buf buf = BUF_INITIALIZER;

    /* Decode into UTF-8 buffer */
    char *raw = _decode_to_utf8(part->charset_id,
            msg_buf->s + part->content_offset,
            part->content_size, part->encoding,
            &is_encoding_problem);
    if (!raw) goto done;

    /* In-place remove CR characters from buffer */
    size_t i, j, rawlen = strlen(raw);
    for (i = 0, j = 0; j < rawlen; j++) {
        if (raw[j] != '\r') raw[i++] = raw[j];
    }
    raw[i] = '\0';

    /* Initialize return value */
    buf_initm(&buf, raw, rawlen);

    /* Truncate buffer */
    if (buf_len(&buf) && max_body_bytes && max_body_bytes < buf_len(&buf)) {
        /* Cut of excess bytes */
        buf_truncate(&buf, max_body_bytes);
        is_truncated = 1;
        /* Clip to sane UTF-8 */
        /* XXX do not split between combining characters */
        const unsigned char *base = (unsigned char *) buf_base(&buf);
        const unsigned char *top = base + buf_len(&buf);
        const unsigned char *p = top - 1;
        while (p >= base && ((*p & 0xc0) == 0x80))
            p--;
        if (p >= base) {
            ssize_t have_bytes = top - p;
            ssize_t need_bytes = 0;
            unsigned char hi_nibble = *p & 0xf0;
            switch (hi_nibble) {
                case 0xf0:
                    need_bytes = 4;
                    break;
                case 0xe0:
                    need_bytes = 3;
                    break;
                case 0xc0:
                    need_bytes = 2;
                    break;
                default:
                    need_bytes = 1;
            }
            if (have_bytes < need_bytes)
                buf_truncate(&buf, p - base);
        }
        else {
            buf_reset(&buf);
        }
    }

    /* Truncate HTML */
    if (buf_len(&buf) && max_body_bytes && is_html) {
        /* Truncate any trailing '<' start tag character without closing '>' */
        const char *base = buf_base(&buf);
        const char *top  = base + buf_len(&buf);
        const char *p;
        for (p = top - 1; *p != '>' && p >= base; p--) {
            if (*p == '<') {
                buf_truncate(&buf, p - base + 1);
                is_truncated = 1;
                break;
            }
        }
    }

done:
    jbodyvalue = json_pack("{s:s s:b s:b}",
            "value", buf_cstring(&buf),
            "isEncodingProblem", is_encoding_problem,
            "isTruncated", is_truncated);
    buf_free(&buf);
    return jbodyvalue;
}

static int _email_get_bodies(jmap_req_t *req,
                             struct email_getargs *args,
                             struct cyrusmsg *msg,
                             json_t *email)
{
    struct emailbodies bodies = EMAILBODIES_INITIALIZER;
    hash_table *props = args->props;
    int r = 0;

    const struct body *part;
    if (msg->rfc822part) {
        part = msg->rfc822part->subpart;
    }
    else {
        r = _cyrusmsg_need_part0(msg);
        if (r) return r;
        part =  msg->part0;
    }

    /* Dissect message into its parts */
    r = jmap_emailbodies_extract(part, &bodies);
    if (r) goto done;

    /* bodyStructure */
    if (jmap_wantprop(props, "bodyStructure")) {
        json_object_set_new(email, "bodyStructure",
                _email_get_bodypart(req, args, msg, part));
    }

    /* bodyValues */
    if (jmap_wantprop(props, "bodyValues")) {
        json_t *body_values = json_object();
        /* Determine which body value parts to fetch */
        int i;
        ptrarray_t parts = PTRARRAY_INITIALIZER;
        if (args->fetch_text_body || args->fetch_all_body) {
            for (i = 0; i < bodies.textlist.count; i++)
                ptrarray_append(&parts, ptrarray_nth(&bodies.textlist, i));
        }
        if (args->fetch_html_body || args->fetch_all_body) {
            for (i = 0; i < bodies.htmllist.count; i++)
                ptrarray_append(&parts, ptrarray_nth(&bodies.htmllist, i));
        }
        if (parts.count) {
            r = _cyrusmsg_need_mime(msg);
            if (r) goto done;
        }
        /* Fetch body values */
        for (i = 0; i < parts.count; i++) {
            struct body *part = ptrarray_nth(&parts, i);
            if (strcmp("TEXT", part->type)) {
                continue;
            }
            if (part->part_id && json_object_get(body_values, part->part_id)) {
                continue;
            }
            json_object_set_new(body_values, part->part_id,
                    _email_get_bodyvalue(part, msg->mime, args->max_body_bytes,
                                         !strcmp("HTML", part->subtype)));
        }
        ptrarray_fini(&parts);
        json_object_set_new(email, "bodyValues", body_values);
    }

    /* textBody */
    if (jmap_wantprop(props, "textBody")) {
        json_t *text_body = json_array();
        int i;
        for (i = 0; i < bodies.textlist.count; i++) {
            struct body *part = ptrarray_nth(&bodies.textlist, i);
            json_array_append_new(text_body,
                    _email_get_bodypart(req, args, msg, part));
        }
        json_object_set_new(email, "textBody", text_body);
    }

    /* htmlBody */
    if (jmap_wantprop(props, "htmlBody")) {
        json_t *html_body = json_array();
        int i;
        for (i = 0; i < bodies.htmllist.count; i++) {
            struct body *part = ptrarray_nth(&bodies.htmllist, i);
            json_array_append_new(html_body,
                    _email_get_bodypart(req, args, msg, part));
        }
        json_object_set_new(email, "htmlBody", html_body);
    }

    /* attachments */
    if (jmap_wantprop(props, "attachments")) {
        json_t *attachments = json_array();
        int i;
        for (i = 0; i < bodies.attslist.count; i++) {
            struct body *part = ptrarray_nth(&bodies.attslist, i);
            json_array_append_new(attachments,
                    _email_get_bodypart(req, args, msg, part));
        }
        json_object_set_new(email, "attachments", attachments);
    }

    /* calendarEvents -- non-standard */
    if (jmap_wantprop(props, "calendarEvents")) {
        json_t *calendar_events = json_object();
        int i;
        for (i = 0; i < bodies.attslist.count; i++) {
            struct body *part = ptrarray_nth(&bodies.attslist, i);
            /* Process text/calendar attachments and files ending with .ics */
            if (strcmp(part->type, "TEXT") || strcmp(part->subtype, "CALENDAR")) {
                int has_ics_attachment = 0;
                struct param *param = part->disposition_params;
                while (param) {
                    if (!strcasecmp(param->attribute, "FILENAME")) {
                        size_t len = strlen(param->value);
                        if (len > 4 && !strcasecmp(param->value + len-4, ".ICS")) {
                            has_ics_attachment = 1;
                        }
                    }
                    param = param->next;
                }
                if (!has_ics_attachment)
                    continue;
            }
            /* Parse decoded data to iCalendar object */
            r = _cyrusmsg_need_mime(msg);
            if (r) goto done;
            char *decbuf = NULL;
            size_t declen = 0;
            const char *rawical = charset_decode_mimebody(msg->mime->s + part->content_offset,
                    part->content_size, part->charset_enc, &decbuf, &declen);
            if (!rawical) continue;
            struct buf buf = BUF_INITIALIZER;
            buf_setmap(&buf, rawical, declen);
            icalcomponent *ical = ical_string_as_icalcomponent(&buf);
            buf_free(&buf);
            free(decbuf);
            if (!ical) continue;
            /* Parse iCalendar object to JSCalendar */
            json_t *jsevents = jmapical_tojmap_all(ical, NULL);
            if (json_array_size(jsevents)) {
                json_object_set_new(calendar_events, part->part_id, jsevents);
            }
            icalcomponent_free(ical);
        }
        if (!json_object_size(calendar_events)) {
            json_decref(calendar_events);
            calendar_events = json_null();
        }
        json_object_set_new(email, "calendarEvents", calendar_events);
    }

    /* hasAttachment */
    if (jmap_wantprop(props, "hasAttachment")) {
        int has_att = 0;
        if (msg->rfc822part == NULL) {
            msgrecord_hasflag(msg->mr, JMAP_HAS_ATTACHMENT_FLAG, &has_att);
        }
        else {
            has_att = bodies.attslist.count > 0;
        }
        json_object_set_new(email, "hasAttachment", json_boolean(has_att));
    }

    /* preview */
    if (jmap_wantprop(props, "preview")) {
        const char *preview_annot = config_getstring(IMAPOPT_JMAP_PREVIEW_ANNOT);
        if (preview_annot && msg->rfc822part == NULL) {
            json_t *preview = _email_read_jannot(req, msg->mr, preview_annot, /*structured*/0);
            json_object_set_new(email, "preview", preview ? preview : json_string(""));
        }
        else {
            r = _cyrusmsg_need_mime(msg);
            if (r) goto done;
            /* TODO optimise for up to PREVIEW_LEN bytes */
            char *text = _emailbodies_to_plain(&bodies, msg->mime);
            if (!text) {
                char *html = _emailbodies_to_html(&bodies, msg->mime);
                if (html) text = _html_to_plain(html);
                free(html);
            }
            if (text) {
                size_t len = config_getint(IMAPOPT_JMAP_PREVIEW_LENGTH);
                char *preview = _email_extract_preview(text, len);
                json_object_set_new(email, "preview", json_string(preview));
                free(preview);
                free(text);
            }
        }
    }

done:
    jmap_emailbodies_fini(&bodies);
    return r;
}

static int _email_from_msg(jmap_req_t *req,
                           struct email_getargs *args,
                           struct cyrusmsg *msg,
                           json_t **emailptr)
{
    json_t *email = json_object();
    int r = 0;

    r = _email_get_meta(req, args, msg, email);
    if (r) goto done;

    r = _email_get_headers(req, args, msg, email);
    if (r) goto done;

    r = _email_get_bodies(req, args, msg, email);
    if (r) goto done;

    *emailptr = email;
done:

    if (r) json_decref(email);
    return r;
}


static int _email_from_record(jmap_req_t *req,
                              struct email_getargs *args,
                              msgrecord_t *mr,
                              json_t **emailptr)
{
    struct cyrusmsg *msg = NULL;
    int r = _cyrusmsg_from_record(mr, &msg);
    if (!r) r = _email_from_msg(req, args, msg, emailptr);
    _cyrusmsg_free(&msg);
    return r;
}

static int _email_from_body(jmap_req_t *req,
                            struct email_getargs *args,
                            msgrecord_t *mr,
                            struct body *body,
                            const struct body *part,
                            json_t **emailptr)
{
    struct cyrusmsg *msg = NULL;
    int r = _cyrusmsg_from_bodypart(mr, body, part, &msg);
    if (!r) r = _email_from_msg(req, args, msg, emailptr);
    _cyrusmsg_free(&msg);
    return r;
}

static int _email_from_buf(jmap_req_t *req,
                           struct email_getargs *args,
                           const struct buf *buf,
                           const char *encoding,
                           json_t **emailptr)
{
    struct buf mybuf = BUF_INITIALIZER;
    buf_setcstr(&mybuf, "Content-Type: message/rfc822\r\n");
    if (encoding) {
        if (!strcasecmp(encoding, "BASE64")) {
            char *tmp = NULL;
            size_t tmp_size = 0;
            charset_decode_mimebody(buf_base(buf), buf_len(buf),
                    ENCODING_BASE64, &tmp, &tmp_size);
            buf_appendcstr(&mybuf, "Content-Transfer-Encoding: binary\r\n");
            /* Append base64-decoded body */
            buf_appendcstr(&mybuf, "\r\n");
            buf_appendmap(&mybuf, tmp, tmp_size);
            free(tmp);
        }
        else {
            buf_appendcstr(&mybuf, "Content-Transfer-Encoding: ");
            buf_appendcstr(&mybuf, encoding);
            buf_appendcstr(&mybuf, "\r\n");
            /* Append encoded body */
            buf_appendcstr(&mybuf, "\r\n");
            buf_append(&mybuf, buf);
        }
    }
    else {
        /* Append raw body */
        buf_appendcstr(&mybuf, "\r\n");
        buf_append(&mybuf, buf);
    }

    struct cyrusmsg *msg = NULL;
    int r = _cyrusmsg_from_buf(&mybuf, &msg);
    if (!r) r = _email_from_msg(req, args, msg, emailptr);
    buf_free(&mybuf);
    _cyrusmsg_free(&msg);
    return r;
}

HIDDEN int jmap_email_get_with_props(jmap_req_t *req,
                                     hash_table *props,
                                     msgrecord_t *mr,
                                     json_t **msgp)
{
    struct email_getargs args = _EMAIL_GET_ARGS_INITIALIZER;
    args.props = props;
    int r = _email_from_record(req, &args, mr, msgp);
    args.props = NULL;
    _email_getargs_fini(&args);
    return r;
}

static int _isthreadsonly(json_t *jargs)
{
    json_t *arg = json_object_get(jargs, "properties");
    if (!json_is_array(arg)) return 0;
    if (json_array_size(arg) != 1) return 0;
    const char *s = json_string_value(json_array_get(arg, 0));
    if (strcmpsafe(s, "threadId")) return 0;
    return 1;
}

static void jmap_email_get_threadsonly(jmap_req_t *req, struct jmap_get *get)
{
    size_t i;
    json_t *val;
    json_array_foreach(get->ids, i, val) {
        const char *id = json_string_value(val);
        conversation_id_t cid = 0;

        int r = _email_get_cid(req, id, &cid);
        if (!r && cid) {
            char thread_id[JMAP_THREADID_SIZE];
            jmap_set_threadid(cid, thread_id);
            json_t *msg = json_pack("{s:s, s:s}", "id", id, "threadId", thread_id);
            json_array_append_new(get->list, msg);
        }
        else {
            json_array_append_new(get->not_found, json_string(id));
        }
        if (r) {
            syslog(LOG_ERR, "jmap: Email/get(%s): %s", id, error_message(r));
        }
    }
}

struct _warmup_mboxcache_cb_rock {
    jmap_req_t *req;
    ptrarray_t mboxes;
};

static int _warmup_mboxcache_cb(const conv_guidrec_t *rec, void* vrock)
{
    struct _warmup_mboxcache_cb_rock *rock = vrock;
    int i;
    for (i = 0; i < ptrarray_size(&rock->mboxes); i++) {
        struct mailbox *mbox = ptrarray_nth(&rock->mboxes, i);
        if (!strcmp(rec->mboxname, mbox->name)) {
            return 0;
        }
    }
    struct mailbox *mbox = NULL;
    int r = jmap_openmbox(rock->req, rec->mboxname, &mbox, /*rw*/0);
    if (!r) {
        ptrarray_append(&rock->mboxes, mbox);
    }
    return r;
}

static void jmap_email_get_full(jmap_req_t *req, struct jmap_get *get, struct email_getargs *args)
{
    size_t i;
    json_t *val;

    /* Warm up the mailbox cache by opening all mailboxes */
    struct _warmup_mboxcache_cb_rock rock = { req, PTRARRAY_INITIALIZER };
    json_array_foreach(get->ids, i, val) {
        const char *email_id = json_string_value(val);
        if (email_id[0] != 'M' || strlen(email_id) != 25) {
            continue;
        }
        int r = conversations_guid_foreach(req->cstate, _guid_from_id(email_id),
                                           _warmup_mboxcache_cb, &rock);
        if (r) {
            /* Ignore errors, they'll be handled in email_find */
            syslog(LOG_WARNING, "__warmup_mboxcache_cb(%s): %s", email_id, error_message(r));
            continue;
        }
    }

    /* Process emails one after the other */
    json_array_foreach(get->ids, i, val) {
        const char *id = json_string_value(val);
        char *mboxname = NULL;
        msgrecord_t *mr = NULL;
        json_t *msg = NULL;
        struct mailbox *mbox = NULL;

        uint32_t uid;
        int r = jmap_email_find(req, id, &mboxname, &uid);
        if (!r) {
            r = jmap_openmbox(req, mboxname, &mbox, 0);
            if (!r) {
                r = msgrecord_find(mbox, uid, &mr);
                if (!r) {
                    r = _email_from_record(req, args, mr, &msg);
                }
                jmap_closembox(req, &mbox);
            }
        }
        if (!r && msg) {
            json_array_append_new(get->list, msg);
        }
        else {
            json_array_append_new(get->not_found, json_string(id));
        }
        if (r) {
            syslog(LOG_ERR, "jmap: Email/get(%s): %s", id, error_message(r));
        }

        free(mboxname);
        msgrecord_unref(&mr);
    }

    /* Close cached mailboxes */
    struct mailbox *mbox = NULL;
    while ((mbox = ptrarray_pop(&rock.mboxes))) {
        jmap_closembox(req, &mbox);
    }
    ptrarray_fini(&rock.mboxes);
}

static const jmap_property_t email_props[] = {
    {
        "id",
        NULL,
        JMAP_PROP_SERVER_SET | JMAP_PROP_IMMUTABLE | JMAP_PROP_ALWAYS_GET
    },
    {
        "blobId",
        NULL,
        JMAP_PROP_SERVER_SET | JMAP_PROP_IMMUTABLE
    },
    {
        "threadId",
        NULL,
        JMAP_PROP_SERVER_SET | JMAP_PROP_IMMUTABLE
    },
    {
        "mailboxIds",
        NULL,
        0
    },
    {
        "keywords",
        NULL,
        0
    },
    {
        "size",
        NULL,
        JMAP_PROP_SERVER_SET | JMAP_PROP_IMMUTABLE
    },
    {
        "receivedAt",
        NULL,
        JMAP_PROP_IMMUTABLE
    },
    {
        "headers",
        NULL,
        JMAP_PROP_IMMUTABLE | JMAP_PROP_SKIP_GET
    },
    {
        "header:*",
        NULL,
        JMAP_PROP_IMMUTABLE | JMAP_PROP_SKIP_GET
    },
    {
        "messageId",
        NULL,
        JMAP_PROP_IMMUTABLE
    },
    {
        "inReplyTo",
        NULL,
        JMAP_PROP_IMMUTABLE
    },
    {
        "references",
        NULL,
        JMAP_PROP_IMMUTABLE
    },
    {
        "sender",
        NULL,
        JMAP_PROP_IMMUTABLE
    },
    {
        "from",
        NULL,
        JMAP_PROP_IMMUTABLE
    },
    {
        "to",
        NULL,
        JMAP_PROP_IMMUTABLE
    },
    {
        "cc",
        NULL,
        JMAP_PROP_IMMUTABLE
    },
    {
        "bcc",
        NULL,
        JMAP_PROP_IMMUTABLE
    },
    {
        "replyTo",
        NULL,
        JMAP_PROP_IMMUTABLE
    },
    {
        "subject",
        NULL,
        JMAP_PROP_IMMUTABLE
    },
    {
        "sentAt",
        NULL,
        JMAP_PROP_IMMUTABLE
    },
    {
        "bodyStructure",
        NULL,
        JMAP_PROP_IMMUTABLE | JMAP_PROP_SKIP_GET
    },
    {
        "bodyValues",
        NULL,
        JMAP_PROP_IMMUTABLE
    },
    {
        "textBody",
        NULL,
        JMAP_PROP_IMMUTABLE
    },
    {
        "htmlBody",
        NULL,
        JMAP_PROP_IMMUTABLE
    },
    {
        "attachments",
        NULL,
        JMAP_PROP_IMMUTABLE
    },
    {
        "hasAttachment",
        NULL,
        JMAP_PROP_SERVER_SET | JMAP_PROP_IMMUTABLE
    },
    {
        "preview",
        NULL,
        JMAP_PROP_SERVER_SET | JMAP_PROP_IMMUTABLE
    },

    /* FM extensions (do ALL of these get through to Cyrus?) */
    {
        "addedDates",
        JMAP_MAIL_EXTENSION,
        0
    },
    {
        "removedDates",
        JMAP_MAIL_EXTENSION,
        0
    },
    {
        "trustedSender",
        JMAP_MAIL_EXTENSION,
        JMAP_PROP_SERVER_SET | JMAP_PROP_IMMUTABLE
    },
    {
        "spamScore",
        JMAP_MAIL_EXTENSION,
        JMAP_PROP_IMMUTABLE
    },
    {
        "calendarEvents",
        JMAP_MAIL_EXTENSION,
        JMAP_PROP_IMMUTABLE
    },
    {
        "isDeleted",
        JMAP_MAIL_EXTENSION,
        0
    },
    {
        "imageSize",
        JMAP_MAIL_EXTENSION,
        0
    },
    {
        "snoozed",
        JMAP_MAIL_EXTENSION,
        0
    },
    { NULL, NULL, 0 }
};

static int jmap_email_get(jmap_req_t *req)
{
    struct jmap_parser parser = JMAP_PARSER_INITIALIZER;
    struct jmap_get get;
    struct email_getargs args = _EMAIL_GET_ARGS_INITIALIZER;
    json_t *err = NULL;

    /* Parse request */
    jmap_get_parse(req, &parser, email_props, /*allow_null_ids*/0,
                   &_email_getargs_parse, &args, &get, &err);
    if (!err) {
        /* header:Xxx properties */
        json_t *jprops = json_object_get(req->args, "properties");
        if (JNOTNULL(jprops)) {
            _email_parse_wantheaders(jprops, &parser, "properties",
                                     &args.want_headers);
        }

        if (json_array_size(parser.invalid)) {
            err = json_pack("{s:s s:O}", "type", "invalidArguments",
                            "arguments", parser.invalid);
        }
    }
    if (err) {
        jmap_error(req, err);
        goto done;
    }

    /* properties - already parsed in jmap_get_parse */
    args.props = get.props;

    /* Set default body properties, if not set by client */
    if (args.bodyprops == NULL) {
        args.bodyprops = &_email_get_default_bodyprops;

        if (args.bodyprops->size == 0) {
            _email_init_default_props(args.bodyprops);
        }
    }

    if (_isthreadsonly(req->args))
        jmap_email_get_threadsonly(req, &get);
    else
        jmap_email_get_full(req, &get, &args);

    json_t *jstate = jmap_getstate(req, MBTYPE_EMAIL, /*refresh*/0);
    get.state = xstrdup(json_string_value(jstate));
    json_decref(jstate);

    jmap_ok(req, jmap_get_reply(&get));

done:
    _email_getargs_fini(&args);
    jmap_parser_fini(&parser);
    jmap_get_fini(&get);
    return 0;
}

static int jmap_email_parse(jmap_req_t *req)
{
    struct jmap_parser parser = JMAP_PARSER_INITIALIZER;
    struct email_getargs getargs = _EMAIL_GET_ARGS_INITIALIZER;
    const char *key;
    json_t *arg, *jblobIds = NULL;
    hash_table *props = NULL;

    /* Parse request */
    json_object_foreach(req->args, key, arg) {
        if (!strcmp(key, "accountId")) {
            /* already handled in jmap_api() */
        }

        else if (!strcmp(key, "blobIds")) {
            jblobIds = arg;
            jmap_parse_strings(jblobIds, &parser, "blobIds");
        }

        else if (!strcmp(key, "properties")) {
            if (json_is_array(arg)) {
                size_t i;
                json_t *val;
                props = xzmalloc(sizeof(hash_table));
                construct_hash_table(props, json_array_size(arg) + 1, 0);
                json_array_foreach(arg, i, val) {
                    const char *s = json_string_value(val);
                    if (!s) {
                        jmap_parser_push_index(&parser, "properties", i, s);
                        jmap_parser_invalid(&parser, NULL);
                        jmap_parser_pop(&parser);
                        continue;
                    }
                    hash_insert(s, (void*)1, props);
                }
                getargs.props = props;
            }
            else if (JNOTNULL(arg)) {
                jmap_parser_invalid(&parser, "properties");
            }
        }

        else if (!_email_getargs_parse(req, &parser, key, arg, &getargs)) {
            jmap_parser_invalid(&parser, key);
        }
    }

    if (json_array_size(parser.invalid)) {
        json_t *err = json_pack("{s:s s:O}", "type", "invalidArguments",
                                "arguments", parser.invalid);
        jmap_error(req, err);
        goto done;
    }

    /* Set default properties, if not set by client */
    if (getargs.props == NULL) {
        getargs.props = &_email_parse_default_props;

        if (getargs.props->size == 0) {
            _email_init_default_props(getargs.props);
        }
    }

    /* Set default body properties, if not set by client */
    if (getargs.bodyprops == NULL) {
        getargs.bodyprops = &_email_get_default_bodyprops;

        if (getargs.bodyprops->size == 0) {
            _email_init_default_props(getargs.bodyprops);
        }
    }

    /* Process request */
    json_t *parsed = json_object();
    json_t *notParsable = json_array();
    json_t *notFound = json_array();
    json_t *jval;
    size_t i;
    json_array_foreach(jblobIds, i, jval) {
        const char *blobid = json_string_value(jval);
        struct mailbox *mbox = NULL;
        msgrecord_t *mr = NULL;
        struct body *body = NULL;
        const struct body *part = NULL;

        int r = jmap_findblob(req, NULL/*accountid*/, blobid,
                              &mbox, &mr, &body, &part, NULL);
        if (r) {
            json_array_append_new(notFound, json_string(blobid));
            continue;
        }

        json_t *email = NULL;
        if (part && (strcmp(part->type, "MESSAGE") || !strcmpnull(part->encoding, "BASE64"))) {
            struct buf msg_buf = BUF_INITIALIZER;
            r = msgrecord_get_body(mr, &msg_buf);
            if (!r) {
                struct buf buf = BUF_INITIALIZER;
                buf_init_ro(&buf, buf_base(&msg_buf) + part->content_offset,
                        part->content_size);
                r = _email_from_buf(req, &getargs, &buf, part->encoding, &email);
                buf_free(&buf);
            }
            buf_free(&msg_buf);
            if (r) {
                syslog(LOG_ERR, "jmap: Email/parse(%s): %s", blobid, error_message(r));
            }
        }
        else {
            _email_from_body(req, &getargs, mr, body, part, &email);
        }

        if (email) {
            json_object_set_new(parsed, blobid, email);
        }
        else {
            json_array_append_new(notParsable, json_string(blobid));
        }
        msgrecord_unref(&mr);
        jmap_closembox(req, &mbox);
        message_free_body(body);
        free(body);
    }

    /* Build response */
    json_t *res = json_object();
    if (!json_object_size(parsed)) {
        json_decref(parsed);
        parsed = json_null();
    }
    if (!json_array_size(notParsable)) {
        json_decref(notParsable);
        notParsable = json_null();
    }
    if (!json_array_size(notFound)) {
        json_decref(notFound);
        notFound = json_null();
    }
    json_object_set_new(res, "parsed", parsed);
    json_object_set_new(res, "notParsable", notParsable);
    json_object_set_new(res, "notFound", notFound);
    jmap_ok(req, res);

done:
	_email_getargs_fini(&getargs);
    jmap_parser_fini(&parser);
    free_hash_table(props, NULL);
    free(props);
    return 0;
}

static char *_mime_make_boundary()
{
    char *boundary, *p, *q;

    boundary = xstrdup(makeuuid());
    for (p = boundary, q = boundary; *p; p++) {
        if (*p != '-') *q++ = *p;
    }
    *q = 0;

    return boundary;
}

/* A soft limit for MIME header lengths when generating MIME from JMAP.
 * See the header_from_Xxx functions for usage. */
#define MIME_MAX_HEADER_LENGTH 78

static void _mime_write_xparam(struct buf *buf, const char *name, const char *value)
{
    int is_7bit = 1;
    int is_fold = 0;
    const char *p = value;
    for (p = value; *p && (is_7bit || !is_fold); p++) {
        if (*p & 0x80)
            is_7bit = 0;
        if (*p == '\n')
            is_fold = 1;
    }
    char *xvalue = is_7bit ? xstrdup(value) : charset_encode_mimexvalue(value, NULL);

    if (strlen(name) + strlen(xvalue) + 1 < MIME_MAX_HEADER_LENGTH) {
        if (is_7bit)
            buf_printf(buf, ";%s=\"%s\"", name, xvalue);
        else
            buf_printf(buf, ";%s*=%s", name, xvalue);
        goto done;
    }

    /* Break value into continuations */
    int section = 0;
    p = xvalue;
    struct buf line = BUF_INITIALIZER;
    buf_appendcstr(&line, ";\r\n ");
    while (*p) {
        /* Build parameter continuation line. */
        buf_printf(&line, "%s*%d*=", name, section);
        /* Write at least one character of the value */
        if (is_7bit)
            buf_putc(&line, '"');
        int n = buf_len(&line) + 1;
        do {
            buf_putc(&line, *p);
            n++;
            p++;
            if (!is_7bit && *p == '%' && n >= MIME_MAX_HEADER_LENGTH - 2)
                break;
        } while (*p && n < MIME_MAX_HEADER_LENGTH);
        if (is_7bit)
            buf_putc(&line, '"');
        /* Write line */
        buf_append(buf, &line);
        /* Prepare next iteration */
        if (*p) buf_appendcstr(buf, ";\r\n ");
        buf_reset(&line);
        section++;
    }
    buf_free(&line);

done:
    free(xvalue);
}

static int _copy_msgrecords(struct auth_state *authstate,
                            const char *user_id,
                            struct namespace *namespace,
                            struct mailbox *src,
                            struct mailbox *dst,
                            ptrarray_t *msgrecs)
{
    struct appendstate as;
    int r;
    int nolink = !config_getswitch(IMAPOPT_SINGLEINSTANCESTORE);

    r = append_setup_mbox(&as, dst, user_id, authstate,
            ACL_INSERT, NULL, namespace, 0, EVENT_MESSAGE_COPY);
    if (r) goto done;

    r = append_copy(src, &as, msgrecs, nolink,
                    mboxname_same_userid(src->name, dst->name));
    if (r) {
        append_abort(&as);
        goto done;
    }

    r = append_commit(&as);
    if (r) goto done;

    sync_log_mailbox_double(src->name, dst->name);
done:
    return r;
}

static int _copy_msgrecord(struct auth_state *authstate,
                           const char *user_id,
                           struct namespace *namespace,
                           struct mailbox *src,
                           struct mailbox *dst,
                           msgrecord_t *mrw)
{

    if (!strcmp(src->uniqueid, dst->uniqueid))
        return 0;

    ptrarray_t msgrecs = PTRARRAY_INITIALIZER;
    ptrarray_add(&msgrecs, mrw);
    int r = _copy_msgrecords(authstate, user_id, namespace, src, dst, &msgrecs);
    ptrarray_fini(&msgrecs);
    return r;
}

/* A subset of all messages within an IMAP mailbox. */
struct email_mboxrec {
    char *mboxname;     /* IMAP mailbox name */
    char *mbox_id;      /* Cyrus-internal unique mailbox id */
    ptrarray_t uidrecs; /* Array of struct email_uidrec */
};

/* A single mailboxname/UID pair of the JMAP email identified by
 * email_id. Each email has 1 or more uid records, but uid
 * records may represent expunged messages. */
struct email_uidrec {
    struct email_mboxrec *mboxrec; /* owning mailboxrec */
    char *email_id;                /* JMAP email id */
    uint32_t uid;                  /* IMAP uid in mbox */
    int is_new;                    /* Used by Email/set{update} */
    int is_snoozed;                /* Used by Email/set{update} */
};

static void _email_multiexpunge(jmap_req_t *req, struct mailbox *mbox,
                                ptrarray_t *uidrecs, json_t *errors)
{
    int r;
    struct mboxevent *mboxevent = NULL;
    msgrecord_t *mrw = NULL;
    uint32_t system_flags, internal_flags;

    mboxevent = mboxevent_new(EVENT_MESSAGE_EXPUNGE);

    int j;
    int didsome = 0;
    for (j = 0; j < ptrarray_size(uidrecs); j++) {
        struct email_uidrec *uidrec = ptrarray_nth(uidrecs, j);
        // skip known errors
        if (json_object_get(errors, uidrec->email_id)) {
             continue;
        }
        // load the record
        if (mrw) msgrecord_unref(&mrw);
        r = msgrecord_find(mbox, uidrec->uid, &mrw);
        if (!r) r = msgrecord_get_systemflags(mrw, &system_flags);
        if (!r) r = msgrecord_get_internalflags(mrw, &internal_flags);
        // already expunged, skip (aka: will be reported as success)
        if (internal_flags & FLAG_INTERNAL_EXPUNGED) continue;
        // update the flags
        if (!r) r = msgrecord_add_systemflags(mrw, FLAG_DELETED);
        if (!r) r = msgrecord_add_internalflags(mrw, FLAG_INTERNAL_EXPUNGED);
        if (!r) r = msgrecord_rewrite(mrw);
        if (!r) {
            mboxevent_extract_msgrecord(mboxevent, mrw);
            didsome++;
        }
        // if errors, record the issue
        if (r) json_object_set_new(errors, uidrec->email_id, jmap_server_error(r));
    }
    if (mrw) msgrecord_unref(&mrw);

    /* Report mailbox event if anything to say */
    if (didsome) {
        mboxevent_extract_mailbox(mboxevent, mbox);
        mboxevent_set_numunseen(mboxevent, mbox, -1);
        mboxevent_set_access(mboxevent, NULL, NULL, req->userid, mbox->name, 0);
        mboxevent_notify(&mboxevent);
    }
    mboxevent_free(&mboxevent);
}

struct email_append_detail {
    char blob_id[JMAP_BLOBID_SIZE];
    char email_id[JMAP_EMAILID_SIZE];
    char thread_id[JMAP_THREADID_SIZE];
    size_t size;
};

static void _email_append(jmap_req_t *req,
                          json_t *mailboxids,
                          strarray_t *keywords,
                          time_t internaldate,
                          json_t *snoozed,
                          int has_attachment,
                          const char *sourcefile,
                          int(*writecb)(jmap_req_t* req, FILE* fp, void* rock, json_t **err),
                          void *rock,
                          struct email_append_detail *detail,
                          json_t **err)
{
    int fd;
    void *addr;
    FILE *f = NULL;
    char *mboxname = NULL, *last = NULL;
    const char *id;
    struct stagemsg *stage = NULL;
    struct mailbox *mbox = NULL;
    msgrecord_t *mr = NULL;
    quota_t qdiffs[QUOTA_NUMRESOURCES] = QUOTA_DIFFS_INITIALIZER;
    json_t *val, *mailboxes = NULL;
    size_t len;
    int r = 0;
    time_t savedate = 0;

    if (json_object_size(mailboxids) > JMAP_MAIL_MAX_MAILBOXES_PER_EMAIL) {
        *err = json_pack("{s:s}", "type", "tooManyMailboxes");
        goto done;
    }
    else if (strarray_size(keywords) > JMAP_MAIL_MAX_KEYWORDS_PER_EMAIL) {
        *err = json_pack("{s:s}", "type", "tooManyKeywords");
        goto done;
    }

    if (!internaldate) internaldate = time(NULL);

    /* Pick the mailbox to create the message in, prefer \Snoozed then \Drafts */
    mailboxes = json_pack("{}"); /* maps mailbox ids to mboxnames */
    json_object_foreach(mailboxids, id, val) {
        const char *mboxid = id;
        /* Lookup mailbox */
        if (mboxid && mboxid[0] == '#') {
            mboxid = jmap_lookup_id(req, mboxid + 1);
        }
        if (!mboxid) continue;
        const mbentry_t *mbentry = jmap_mbentry_by_uniqueid(req, mboxid);
        if (!mbentry || !jmap_hasrights(req, mbentry, ACL_LOOKUP)) {
            r = IMAP_MAILBOX_NONEXISTENT;
            goto done;
        }

        /* Convert intermediary mailbox to real mailbox */
        if (mbentry->mbtype & MBTYPE_INTERMEDIATE) {
            r = mboxlist_promote_intermediary(mbentry->name);
            if (r) goto done;
        }

        if (json_is_string(val)) {
            /* We flagged this mailboxId as the $snoozed mailbox */
            if (mboxname) free(mboxname);
            mboxname = xstrdup(mbentry->name);
        }
        else if (!mboxname) {
            mbname_t *mbname = mbname_from_intname(mbentry->name);

            /* Is this the draft mailbox? */
            struct buf buf = BUF_INITIALIZER;
            annotatemore_lookup(mbname_intname(mbname), "/specialuse",
                                req->accountid, &buf);
            if (buf.len) {
                strarray_t *uses = strarray_split(buf_cstring(&buf), " ", STRARRAY_TRIM);
                if (strarray_find_case(uses, "\\Drafts", 0)) {
                    if (mboxname) free(mboxname);
                    mboxname = xstrdup(mbentry->name);
                }
                strarray_free(uses);
            }
            buf_free(&buf);
            mbname_free(&mbname);
        }

        /* If we haven't picked a mailbox, remember the last one. */
        if (last) free(last);
        if (!mboxname) last = xstrdup(mbentry->name);

        /* Map mailbox id to mailbox name. */
        json_object_set_new(mailboxes, mboxid, json_string(mbentry->name));
    }

    /* If we haven't picked a mailbox, pick the last one. */
    if (!mboxname) mboxname = last;
    if (!mboxname) {
        char *s = json_dumps(mailboxids, 0);
        syslog(LOG_ERR, "_email_append: invalid mailboxids: %s", s);
        free(s);
        r = IMAP_INTERNAL;
        goto done;
    }

    /* Create the message in the destination mailbox */
    r = jmap_openmbox(req, mboxname, &mbox, 1);
    if (r) goto done;

    if (sourcefile) {
        if (!(f = append_newstage_full(mbox->name, internaldate, 0, &stage, sourcefile))) {
            syslog(LOG_ERR, "append_newstage(%s) failed", mbox->name);
            r = HTTP_SERVER_ERROR;
            goto done;
        }
    }
    else {
        /* Write the message to the filesystem */
        if (!(f = append_newstage(mbox->name, internaldate, 0, &stage))) {
            syslog(LOG_ERR, "append_newstage(%s) failed", mbox->name);
            r = HTTP_SERVER_ERROR;
            goto done;
        }
        r = writecb(req, f, rock, err);
        if (r) goto done;
        if (fflush(f)) {
            r = IMAP_IOERROR;
            goto done;
        }
    }
    fseek(f, 0L, SEEK_END);
    len = ftell(f);

    /* Generate a GUID from the raw file content */
    fd = fileno(f);
    if ((addr = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0))) {
        struct message_guid guid;
        message_guid_generate(&guid, addr, len);
        jmap_set_emailid(&guid, detail->email_id);
        jmap_set_blobid(&guid, detail->blob_id);
        detail->size = len;
        munmap(addr, len);
    } else {
        r = IMAP_IOERROR;
        goto done;
    }
    fclose(f);
    f = NULL;

    /*  Check if a message with this GUID already exists and is
     *  visible for the authenticated user. */
    char *exist_mboxname = NULL;
    uint32_t exist_uid;
    r = jmap_email_find(req, detail->email_id, &exist_mboxname, &exist_uid);
    free(exist_mboxname);
    if (r != IMAP_NOTFOUND) {
        if (!r) r = IMAP_MAILBOX_EXISTS;
        goto done;
    }

    /* Great, that's a new message! */
    struct body *body = NULL;
    struct appendstate as;

    /* Prepare flags */
    strarray_t flags = STRARRAY_INITIALIZER;
    int i;
    for (i = 0; i < strarray_size(keywords); i++) {
        const char *flag = jmap_keyword_to_imap(strarray_nth(keywords, i));
        if (flag) strarray_append(&flags, flag);
    }
    if (has_attachment) {
        strarray_add(&flags, "$hasattachment");
    }

    /* Append the message to the mailbox. */
    qdiffs[QUOTA_MESSAGE] = 1;
    qdiffs[QUOTA_STORAGE] = len;
    r = append_setup_mbox(&as, mbox, req->userid, httpd_authstate,
            0, qdiffs, 0, 0, EVENT_MESSAGE_NEW);
    if (r) goto done;

    struct entryattlist *annots = NULL;
    if (json_is_object(snoozed)) {
        const char *annot = IMAP_ANNOT_NS "snoozed";
        const char *attrib = "value.shared";
        struct buf buf = BUF_INITIALIZER;
        char *json = json_dumps(snoozed, JSON_COMPACT);

        buf_initm(&buf, json, strlen(json));
        setentryatt(&annots, annot, attrib, &buf);
        buf_free(&buf);

        /* Add \snoozed pseudo-flag */
        strarray_add(&flags, "\\snoozed");

        /* Extract until and use it as savedate */
        time_from_iso8601(json_string_value(json_object_get(snoozed, "until")),
                          &savedate);
    }
    r = append_fromstage_full(&as, &body, stage, internaldate, savedate, 0,
                         flags.count ? &flags : NULL, 0, annots);
    freeentryatts(annots);
    if (r) {
        append_abort(&as);
        goto done;
    }
    strarray_fini(&flags);
    message_free_body(body);
    free(body);

    r = append_commit(&as);
    if (r) goto done;

    /* Load message record */
    r = msgrecord_find(mbox, mbox->i.last_uid, &mr);
    if (r) goto done;

    bit64 cid;
    r = msgrecord_get_cid(mr, &cid);
    if (r) goto done;
    jmap_set_threadid(cid, detail->thread_id);

    /* Complete message creation */
    if (stage) {
        append_removestage(stage);
        stage = NULL;
    }
    json_object_del(mailboxes, mbox->uniqueid);

    /* Make sure there is enough quota for all mailboxes */
    qdiffs[QUOTA_STORAGE] = len;
    if (json_object_size(mailboxes)) {
        char foundroot[MAX_MAILBOX_BUFFER];
        json_t *deltas = json_pack("{}");
        const char *mbname;

        /* Count message delta for each quota root */
        json_object_foreach(mailboxes, id, val) {
            mbname = json_string_value(val);
            if (quota_findroot(foundroot, sizeof(foundroot), mbname)) {
                json_t *delta = json_object_get(deltas, mbname);
                delta = json_integer(json_integer_value(delta) + 1);
                json_object_set_new(deltas, mbname, delta);
            }
        }

        /* Check quota for each quota root. */
        json_object_foreach(deltas, mbname, val) {
            struct quota quota;
            quota_t delta = json_integer_value(val);

            quota_init(&quota, mbname);
            r = quota_check(&quota, QUOTA_STORAGE, delta * qdiffs[QUOTA_STORAGE]);
            if (!r) r = quota_check(&quota, QUOTA_MESSAGE, delta);
            quota_free(&quota);
            if (r) break;
        }
        json_decref(deltas);
        if (r) goto done;
    }

    /* Copy the message to all remaining mailboxes */
    json_object_foreach(mailboxes, id, val) {
        const char *dstname = json_string_value(val);
        struct mailbox *dst = NULL;

        if (!strcmp(mboxname, dstname))
            continue;

        r = jmap_openmbox(req, dstname, &dst, 1);
        if (r) goto done;

        r = _copy_msgrecord(httpd_authstate, req->userid, &jmap_namespace,
                            mbox, dst, mr);

        jmap_closembox(req, &dst);
        if (r) goto done;
    }

done:
    if (f) fclose(f);
    append_removestage(stage);
    msgrecord_unref(&mr);
    jmap_closembox(req, &mbox);
    free(mboxname);
    json_decref(mailboxes);
    if (r && *err == NULL) {
        switch (r) {
            case IMAP_PERMISSION_DENIED:
                *err = json_pack("{s:s}", "type", "forbidden");
                break;
            case IMAP_MAILBOX_EXISTS:
                *err = json_pack("{s:s s:s}", "type", "alreadyExists", "existingId", detail->email_id);
                break;
            case IMAP_QUOTA_EXCEEDED:
                *err = json_pack("{s:s}", "type", "overQuota");
                break;
            case IMAP_MESSAGE_CONTAINSNULL:
            case IMAP_MESSAGE_CONTAINSNL:
            case IMAP_MESSAGE_CONTAINS8BIT:
            case IMAP_MESSAGE_BADHEADER:
            case IMAP_MESSAGE_NOBLANKLINE:
                *err = json_pack("{s:s s:s}", "type", "invalidEmail",
                        "description", error_message(r));
                break;
            default:
                *err = jmap_server_error(r);
        }
    }
}

struct emailpart {
    /* Mandatory fields */
    struct headers headers;       /* raw headers */
    /* Optional fields */
    json_t *jpart;                /* original EmailBodyPart JSON object */
    json_t *jbody;                /* EmailBodyValue for text bodies */
    char *blob_id;                /* blobId to dump contents from */
    ptrarray_t subparts;          /* array of emailpart pointers */
    char *type;                   /* Content-Type main type */
    char *subtype;                /* Content-Type subtype */
    char *charset;                /* Content-Type charset parameter */
    char *boundary;               /* Content-Type boundary parameter */
    char *disposition;            /* Content-Disposition without parameters */
    char *filename;               /* Content-Disposition filename parameter */
};

static void _emailpart_fini(struct emailpart *part)
{
    if (!part) return;

    struct emailpart *subpart;
    while ((subpart = ptrarray_pop(&part->subparts))) {
        _emailpart_fini(subpart);
        free(subpart);
    }
    ptrarray_fini(&part->subparts);
    json_decref(part->jpart);
    json_decref(part->jbody);
    _headers_fini(&part->headers);
    free(part->type);
    free(part->subtype);
    free(part->boundary);
    free(part->charset);
    free(part->disposition);
    free(part->filename);
    free(part->blob_id);
}

struct email {
    struct headers headers; /* parsed headers */
    json_t *jemail;               /* original Email JSON object */
    struct emailpart *body;       /* top-level MIME part */
    time_t internaldate;          /* RFC 3501 internaldate aka receivedAt */
    int has_attachment;           /* set the HasAttachment flag */
    json_t *snoozed;              /* set snoozed annotation */
};

static void _email_fini(struct email *email)
{
    if (!email) return;
    _headers_fini(&email->headers);
    json_decref(email->jemail);
    _emailpart_fini(email->body);
    free(email->body);
}

static json_t *_header_make(const char *header_name, const char *prop_name, struct buf *val)
{
    char *tmp = buf_release(val);
    json_t *jheader = json_pack("{s:s s:s}", "name", header_name, "value", tmp);
    free(tmp);
    if (prop_name) json_object_set_new(jheader, "prop", json_string(prop_name));
    return jheader;
}

typedef json_t* (*header_from_t)(json_t *jval,
                                 struct jmap_parser *parser,
                                 const char *prop_name,
                                 const char *header_name);

static json_t *_header_from_raw(json_t *jraw,
                                struct jmap_parser *parser,
                                const char *prop_name,
                                const char *header_name)
{
    /* Verbatim use header value in raw form */
    if (json_is_string(jraw)) {
        json_t *jheader = json_pack("{s:s s:O s:s}",
                "name", header_name, "value", jraw, "prop", prop_name);
        return jheader;
    }
    else {
        jmap_parser_invalid(parser, prop_name);
        return NULL;
    }
}

static json_t *_header_from_text(json_t *jtext,
                                 struct jmap_parser *parser,
                                 const char *prop_name,
                                 const char *header_name)
{
    /* Parse a Text header into raw form */
    if (json_is_string(jtext)) {
        size_t prefix_len = strlen(header_name) + 2;
        const char *s = json_string_value(jtext);
        /* Q-encoding will fold lines for us */
        int force_quote = prefix_len + strlen(s) > MIME_MAX_HEADER_LENGTH;
        char *tmp = charset_encode_mimeheader(s, strlen(s), force_quote);
        struct buf val = BUF_INITIALIZER;
        /* If text got force-quoted the first line of the Q-encoded
         * text might spill over the soft 78-character limit due to
         * the Header name prefix. Looking at how most of the mail
         * clients are doing this, this seems not to be an issue and
         * allows us to not start the header value with a line fold. */
        buf_setcstr(&val, tmp);
        free(tmp);
        return _header_make(header_name, prop_name, &val);
    }
    else {
        jmap_parser_invalid(parser, prop_name);
        return NULL;
    }
}

static json_t *_header_from_jstrings(json_t *jstrings,
                                     struct jmap_parser *parser,
                                     const char *prop_name,
                                     const char *header_name,
                                     char sep)
{
    if (!json_array_size(jstrings)) {
        jmap_parser_invalid(parser, prop_name);
        return NULL;
    }

    size_t sep_len  = sep ? 1 : 0;
    size_t line_len = strlen(header_name) + 2;
    size_t i;
    json_t *jval;
    struct buf val = BUF_INITIALIZER;

    json_array_foreach(jstrings, i, jval) {
        const char *s = json_string_value(jval);
        if (!s) {
            jmap_parser_invalid(parser, prop_name);
            goto fail;
        }
        size_t s_len = strlen(s);
        if (i && sep) {
            buf_putc(&val, sep);
            line_len++;
        }
        if (line_len + s_len + sep_len  + 1 > MIME_MAX_HEADER_LENGTH) {
            buf_appendcstr(&val, "\r\n ");
            line_len = 1;
        }
        else if (i) {
            buf_putc(&val, ' ');
            line_len++;
        }
        buf_appendcstr(&val, s);
        line_len += s_len;
    }

    return _header_make(header_name, prop_name, &val);

fail:
    buf_free(&val);
    return NULL;
}


static json_t *_header_from_addresses(json_t *addrs,
                                       struct jmap_parser *parser,
                                       const char *prop_name,
                                       const char *header_name)
{
    if (!json_array_size(addrs)) {
        jmap_parser_invalid(parser, prop_name);
        return NULL;
    }

    size_t i;
    json_t *addr;
    struct buf emailbuf = BUF_INITIALIZER;
    struct buf buf = BUF_INITIALIZER;
    json_t *jstrings = json_array();
    json_t *ret = NULL;

    json_array_foreach(addrs, i, addr) {
        json_t *jname = json_object_get(addr, "name");
        if (!json_is_string(jname) && JNOTNULL(jname)) {
            jmap_parser_push_index(parser, prop_name, i, NULL);
            jmap_parser_invalid(parser, "name");
            jmap_parser_pop(parser);
        }

        json_t *jemail = json_object_get(addr, "email");
        if (!json_is_string(jemail) && JNOTNULL(jemail)) {
            jmap_parser_push_index(parser, prop_name, i, NULL);
            jmap_parser_invalid(parser, "email");
            jmap_parser_pop(parser);
        }

        if (json_array_size(parser->invalid))
            goto done;
        if (!JNOTNULL(jname) && !JNOTNULL(jemail))
            continue;

        const char *name = json_string_value(jname);
        const char *email = json_string_value (jemail);
        if (!name && !email) continue;

        /* Trim whitespace from email */
        if (email) {
            buf_setcstr(&emailbuf, email);
            buf_trim(&emailbuf);
            email = buf_cstring(&emailbuf);
        }

        if (name && strlen(name) && email) {
            enum name_type { ATOM, QUOTED_STRING, HIGH_BIT } name_type = ATOM;
            const char *p;
            for (p = name; *p; p++) {
                char c = *p;
                /* Check for ATOM characters */
                if (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z'))
                    continue;
                if ('0' <= c && c <= '9')
                    continue;
                if (strchr("!#$%&'*+-/=?^_`{|}~", c))
                    continue;
                if (c < 0) {
                    /* Contains at least one high bit character. */
                    name_type = HIGH_BIT;
                    break;
                }
                else {
                    /* Requires at least a quoted string, but could
                     * still contain a high bit at a later position. */
                    name_type = QUOTED_STRING;
                }
            }
            if (name_type == ATOM) {
                buf_setcstr(&buf, name);
            }
            else if (name_type == QUOTED_STRING) {
                buf_putc(&buf, '"');
                for (p = name; *p; p++) {
                    if (*p == '"' || *p == '\\' || *p == '\r') {
                        buf_putc(&buf, '\\');
                    }
                    buf_putc(&buf, *p);
                }
                buf_putc(&buf, '"');
            }
            else if (name_type == HIGH_BIT) {
                char *xname = charset_encode_mimephrase(name);
                buf_appendcstr(&buf, xname);
                free(xname);
            }
            buf_printf(&buf, " <%s>", email);
        } else if (email) {
            buf_setcstr(&buf, email);
        }
        json_array_append_new(jstrings, json_string(buf_cstring(&buf)));
        buf_reset(&emailbuf);
        buf_reset(&buf);
    }
    ret = _header_from_jstrings(jstrings, parser, prop_name, header_name, ',');

done:
    json_decref(jstrings);
    buf_free(&emailbuf);
    buf_free(&buf);
    return ret;
}

static json_t *_header_from_messageids(json_t *jmessageids,
                                       struct jmap_parser *parser,
                                       const char *prop_name,
                                       const char *header_name)
{
    if (!json_array_size(jmessageids)) {
        jmap_parser_invalid(parser, prop_name);
        return NULL;
    }

    size_t i;
    json_t *jval;
    struct buf val = BUF_INITIALIZER;
    struct buf msgid = BUF_INITIALIZER;
    json_t *jstrings = json_array();
    json_t *ret = NULL;

    json_array_foreach(jmessageids, i, jval) {
        const char *s = json_string_value(jval);
        if (!s) {
            jmap_parser_invalid(parser, prop_name);
            goto done;
        }
        buf_setcstr(&msgid, s);
        buf_trim(&msgid);
        buf_putc(&val, '<');
        buf_appendcstr(&val, buf_cstring(&msgid));
        buf_putc(&val, '>');
        if (conversations_check_msgid(buf_base(&val), buf_len(&val))) {
            jmap_parser_invalid(parser, prop_name);
            goto done;
        }
        json_array_append_new(jstrings, json_string(buf_cstring(&val)));
        buf_reset(&val);
    }
    ret = _header_from_jstrings(jstrings, parser, prop_name, header_name, 0);

done:
    json_decref(jstrings);
    buf_free(&msgid);
    buf_free(&val);
    return ret;
}

static json_t *_header_from_date(json_t *jdate,
                                 struct jmap_parser *parser,
                                 const char *prop_name,
                                 const char *header_name)
{
    const char *s = json_string_value(jdate);
    if (!s) {
        jmap_parser_invalid(parser, prop_name);
        return NULL;
    }

    struct offsettime t;
    int n = offsettime_from_iso8601(s, &t);
    if (n <= 0 || s[n] != '\0') {
        jmap_parser_invalid(parser, prop_name);
        return NULL;
    }
    char fmt[RFC5322_DATETIME_MAX+1];
    memset(fmt, 0, RFC5322_DATETIME_MAX+1);
    offsettime_to_rfc5322(&t, fmt, RFC5322_DATETIME_MAX+1);

    struct buf val = BUF_INITIALIZER;
    buf_setcstr(&val, fmt);
    return _header_make(header_name, prop_name, &val);
}

static json_t *_header_from_urls(json_t *jurls,
                                 struct jmap_parser *parser,
                                 const char *prop_name,
                                const char *header_name)
{
    if (!json_array_size(jurls)) {
        jmap_parser_invalid(parser, prop_name);
        return NULL;
    }

    size_t i;
    json_t *jval;
    struct buf val = BUF_INITIALIZER;
    json_t *jstrings = json_array();
    json_t *ret = NULL;

    json_array_foreach(jurls, i, jval) {
        const char *s = json_string_value(jval);
        if (!s) {
            jmap_parser_invalid(parser, prop_name);
            goto done;
        }

        buf_appendcstr(&val, "<");
        buf_appendcstr(&val, s);
        buf_appendcstr(&val, ">");
        json_array_append_new(jstrings, json_string(buf_cstring(&val)));
        buf_reset(&val);
    }
    ret = _header_from_jstrings(jstrings, parser, prop_name, header_name, ',');

done:
    json_decref(jstrings);
    buf_free(&val);
    return ret;
}

static void _headers_parseprops(json_t *jobject,
                           struct jmap_parser *parser,
                           struct headers *headers)
{
    const char *field;
    json_t *jval;
    json_object_foreach(jobject, field, jval) {
        if (strncmp(field, "header:", 7))
            continue;
        /* Parse header or reject if invalid form */
        struct header_prop *hprop = _header_parseprop(field);
        if (!hprop) {
            jmap_parser_invalid(parser, field);
            continue;
        }
        /* Reject redefinition of header */
        if (json_object_get(headers->all, hprop->lcasename)) {
            _header_prop_free(hprop);
            jmap_parser_invalid(parser, field);
            continue;
        }
        /* Parse header value */
        header_from_t cb = NULL;
        switch (hprop->form) {
            case HEADER_FORM_RAW:
                cb = _header_from_raw;
                break;
            case HEADER_FORM_TEXT:
                cb = _header_from_text;
                break;
            case HEADER_FORM_ADDRESSES:
                cb = _header_from_addresses;
                break;
            case HEADER_FORM_MESSAGEIDS:
                cb = _header_from_messageids;
                break;
            case HEADER_FORM_DATE:
                cb = _header_from_date;
                break;
            case HEADER_FORM_URLS:
                cb = _header_from_urls;
                break;
            default:
                syslog(LOG_ERR, "jmap: unknown header form: %d", hprop->form);
                jmap_parser_invalid(parser, field);
        }
        if (!jval || jval == json_null()) {
            /* ignore null headers */
            _header_prop_free(hprop);
            continue;
        }
        if (hprop->all) {
            size_t i;
            json_t *jall = jval;
            json_array_foreach(jall, i, jval) {
                jmap_parser_push_index(parser, field, i, NULL);
                json_t *jheader = cb(jval, parser, field, hprop->name);
                if (jheader) _headers_add_new(headers, jheader);
                jmap_parser_pop(parser);
            }
        }
        else {
            json_t *jheader = cb(jval, parser, field, hprop->name);
            if (jheader) _headers_add_new(headers, jheader);
        }
        _header_prop_free(hprop);
    }
}

static void _emailpart_parse_headers(json_t *jpart,
                                     struct jmap_parser *parser,
                                     struct emailpart *part)
{
    /* headers */
    if (JNOTNULL(json_object_get(jpart, "headers"))) {
        jmap_parser_invalid(parser, "headers");
    }

    /* header:Xxx */
    const char *lcasename = NULL;
    json_t *jheaders;
    _headers_parseprops(jpart, parser, &part->headers);
    /* Validate Content-Xxx headers */
    json_object_foreach(part->headers.all, lcasename, jheaders) {
        if (strncmp(lcasename, "content-", 8))
            continue;

        json_t *jheader = json_array_get(jheaders, 0);
        const char *name = json_string_value(json_object_get(jheader, "name"));
        const char *val = json_string_value(json_object_get(jheader, "value"));
        const char *prop = json_string_value(json_object_get(jheader, "prop"));

        /* Reject re-definition of Content-Xxx headers */
        if (json_array_size(jheaders) > 1) {
            size_t j;
            json_array_foreach(jheaders, j, jheader) {
                prop = json_string_value(json_object_get(jheader, "prop"));
                jmap_parser_invalid(parser, prop);
            }
            continue;
        }
        if (!strcasecmp(name, "Content-Type")) {
            /* Validate Content-Type */
            struct param *type_params = NULL;
            message_parse_type(val, &part->type, &part->subtype, &type_params);
            if (part->type  && part->subtype) {
                struct param *param = type_params;
                while (param) {
                    if (!strcasecmp(param->attribute, "BOUNDARY")) {
                        part->boundary = xstrdupnull(param->value);
                    }
                    if (!strcasecmp(param->attribute, "CHARSET")) {
                        part->charset = xstrdupnull(param->value);
                    }
                    param = param->next;
                }
                /* Headers for multipart MUST specify a boundary */
                if (!strcasecmp(part->type, "MULTIPART") && !part->boundary)
                    jmap_parser_invalid(parser, prop);
                /* Headers for bodyparts with partId MUST NOT specify a charset */
                if (JNOTNULL(json_object_get(jpart, "partId")) && part->charset)
                    jmap_parser_invalid(parser, prop);
            }
            else {
                jmap_parser_invalid(parser, prop);
            }
            param_free(&type_params);
        }
        else if (!strcasecmp(name, "Content-Disposition")) {
            /* Validate Content-Disposition */
            struct param *disp_params = NULL;
            message_parse_disposition(val, &part->disposition, &disp_params);
            if (!part->disposition) {
                jmap_parser_invalid(parser, prop);
                continue;
            }
            param_free(&disp_params);
        }
        else if (!strcasecmp(name, "Content-Transfer-Encoding")) {
            /* Always reject Content-Transfer-Encoding */
            jmap_parser_invalid(parser, prop);
        }
    }
}

static struct emailpart *_emailpart_parse(json_t *jpart,
                                          struct jmap_parser *parser,
                                          json_t *bodies)
{
    if (!json_is_object(jpart)) {
        jmap_parser_invalid(parser, NULL);
        return NULL;
    }

    struct buf buf = BUF_INITIALIZER;
    struct emailpart *part = xzmalloc(sizeof(struct emailpart));
    part->jpart = json_incref(jpart);

    json_t *jval;

    /* partId */
    json_t *jpartId = json_object_get(jpart, "partId");
    if (JNOTNULL(jpartId) && !json_is_string(jpartId)) {
        jmap_parser_invalid(parser, "partId");
    }

    /* blobId */
    jval = json_object_get(jpart, "blobId");
    if (JNOTNULL(jval) && json_is_string(jval)) {
        part->blob_id = xstrdup(json_string_value(jval));
    }
    else if (JNOTNULL(jval)) {
        jmap_parser_invalid(parser, "blobId");
    }

    /* size */
    jval = json_object_get(jpart, "size");
    if (JNOTNULL(jval) && (!json_is_integer(jval) || JNOTNULL(jpartId))) {
        jmap_parser_invalid(parser, "size");
    }

    /* Parse headers */
    _emailpart_parse_headers(jpart, parser, part);

    /* Parse convenience header properties */
    int seen_header;

    /* cid */
    json_t *jcid = json_object_get(jpart, "cid");
    seen_header = _headers_have(&part->headers, "Content-Id");
    if (json_is_string(jcid) && !seen_header) {
        const char *cid = json_string_value(jcid);
        buf_setcstr(&buf, "<");
        buf_appendcstr(&buf, cid);
        buf_appendcstr(&buf, ">");
        _headers_add_new(&part->headers, _header_make("Content-Id", "cid", &buf));
    }
    else if (JNOTNULL(jcid)) {
        jmap_parser_invalid(parser, "cid");
    }

    /* language */
    json_t *jlanguage = json_object_get(jpart, "language");
    seen_header = _headers_have(&part->headers, "Content-Language");
    if (json_is_array(jlanguage) && !seen_header) {
        _headers_add_new(&part->headers, _header_from_jstrings(jlanguage,
                    parser, "language", "Content-Language", ','));
    }
    else if (JNOTNULL(jlanguage)) {
        jmap_parser_invalid(parser, "language");
    }

    /* location */
    json_t *jlocation = json_object_get(jpart, "location");
    seen_header = _headers_have(&part->headers, "Content-Location");
    if (json_is_string(jlocation) && !seen_header) {
        buf_setcstr(&buf, json_string_value(jlocation));
        _headers_add_new(&part->headers, _header_make("Content-Location", "location", &buf));
    }
    else if (JNOTNULL(jlocation)) {
        jmap_parser_invalid(parser, "location");
    }

    /* Check Content-Type and Content-Disposition header properties */
    int have_type_header = _headers_have(&part->headers, "Content-Type");
    int have_disp_header = _headers_have(&part->headers, "Content-Disposition");
    /* name */
    json_t *jname = json_object_get(jpart, "name");
    if (json_is_string(jname) && !have_type_header && !have_disp_header) {
        part->filename = xstrdup(json_string_value(jname));
    }
    else if (JNOTNULL(jname)) {
        jmap_parser_invalid(parser, "name");
    }
    /* disposition */
    json_t *jdisposition = json_object_get(jpart, "disposition");
    if (json_is_string(jdisposition) && !have_disp_header) {
        /* Build Content-Disposition header */
        part->disposition = xstrdup(json_string_value(jdisposition));
        buf_setcstr(&buf, part->disposition);
        if (part->filename) {
            _mime_write_xparam(&buf, "filename", part->filename);
        }
        _headers_add_new(&part->headers,
                _header_make("Content-Disposition", "disposition", &buf));
    }
    else if (JNOTNULL(jdisposition)) {
        jmap_parser_invalid(parser, "disposition");
    }
    else if (jname) {
        /* Make Content-Disposition header */
        part->disposition = xstrdup("attachment");
        buf_printf(&buf, "attachment");
        _mime_write_xparam(&buf, "filename", part->filename);
        _headers_add_new(&part->headers,
                _header_make("Content-Disposition", "name", &buf));
    }
    /* charset */
    json_t *jcharset = json_object_get(jpart, "charset");
    if (json_is_string(jcharset) && !have_type_header && !JNOTNULL(jpartId)) {
        part->charset = xstrdup(json_string_value(jcharset));
    }
    else if (JNOTNULL(jcharset)) {
        jmap_parser_invalid(parser, "charset");
    }
    /* type */
    json_t *jtype = json_object_get(jpart, "type");
    if (JNOTNULL(jtype) && json_is_string(jtype) && !have_type_header) {
		const char *type = json_string_value(jtype);
        struct param *type_params = NULL;
        /* Validate type value */
        message_parse_type(type, &part->type, &part->subtype, &type_params);
        if (part->type && part->subtype && !type_params) {
            /* Build Content-Type header */
            if (!strcasecmp(part->type, "MULTIPART")) {
                /* Make boundary */
                part->boundary = _mime_make_boundary();
            }
            buf_reset(&buf);
            buf_printf(&buf, "%s/%s", part->type, part->subtype);
            buf_lcase(&buf);
            if (part->charset) {
                buf_appendcstr(&buf, "; charset=");
                buf_appendcstr(&buf, part->charset);
            }
            if (part->filename) {
                int force_quote = strlen(part->filename) > MIME_MAX_HEADER_LENGTH;
                char *tmp = charset_encode_mimeheader(part->filename, 0, force_quote);
                if (force_quote)
                    buf_appendcstr(&buf, ";\r\n ");
                else
                    buf_appendcstr(&buf, "; ");
                buf_appendcstr(&buf, "name=\"");
                buf_appendcstr(&buf, tmp);
                buf_appendcstr(&buf, "\"");
                free(tmp);
            }
            if (part->boundary) {
                buf_appendcstr(&buf, ";\r\n boundary=");
                buf_appendcstr(&buf, part->boundary);
            }
            _headers_add_new(&part->headers,
                    _header_make("Content-Type", "type", &buf));
        }
        else {
            jmap_parser_invalid(parser, "type");
        }
        param_free(&type_params);
    }
    else if (JNOTNULL(jtype)) {
        jmap_parser_invalid(parser, "type");
    }

    /* Validate by type */
    const char *part_id = json_string_value(json_object_get(jpart, "partId"));
    const char *blob_id = json_string_value(json_object_get(jpart, "blobId"));
    json_t *subParts = json_object_get(jpart, "subParts");
    json_t *bodyValue = part_id ? json_object_get(bodies, part_id) : NULL;

    if (part_id && blob_id)
        jmap_parser_invalid(parser, "blobId");
    if (part_id && !bodyValue)
        jmap_parser_invalid(parser, "partId");

    if (subParts || (part->type && !strcasecmp(part->type, "MULTIPART"))) {
        /* Parse sub parts */
        if (json_array_size(subParts)) {
            size_t i;
            json_t *subPart;
            json_array_foreach(subParts, i, subPart) {
                jmap_parser_push_index(parser, "subParts", i, NULL);
                struct emailpart *subpart = _emailpart_parse(subPart, parser, bodies);
                if (subpart) ptrarray_append(&part->subparts, subpart);
                jmap_parser_pop(parser);
            }
        }
        else {
            jmap_parser_invalid(parser, "subParts");
        }
        /* Must not have a body value */
        if (JNOTNULL(bodyValue))
            jmap_parser_invalid(parser, "partId");
        /* Must not have a blobId */
        if (blob_id)
            jmap_parser_invalid(parser, "blobId");
    }
    else if (part_id || (part->type && !strcasecmp(part->type, "TEXT"))) {
        /* Must have a text body as blob or bodyValue */
        if ((bodyValue == NULL) == (blob_id == NULL))
            jmap_parser_invalid(parser, "blobId");
        /* Must not have sub parts */
        if (JNOTNULL(subParts))
            jmap_parser_invalid(parser, "subParts");
    }
    else {
        /* Must have a blob id */
        if (!blob_id)
            jmap_parser_invalid(parser, "blobId");
        /* Must not have a text body */
        if (bodyValue)
            jmap_parser_invalid(parser, "partId");
        /* Must not have sub parts */
        if (JNOTNULL(subParts))
            jmap_parser_invalid(parser, "subParts");
    }

    buf_free(&buf);

    if (json_array_size(parser->invalid)) {
        _emailpart_fini(part);
        free(part);
        return NULL;
    }

    /* Finalize part definition */
    part->jbody = json_incref(bodyValue);

    return part;
}

static struct emailpart *_emailpart_new_multi(const char *subtype,
                                               ptrarray_t *subparts)
{
    struct emailpart *part = xzmalloc(sizeof(struct emailpart));
    int i;

    part->type = xstrdup("multipart");
    part->subtype = xstrdup(subtype);
    part->boundary = _mime_make_boundary();
    struct buf val = BUF_INITIALIZER;
    buf_printf(&val, "%s/%s;boundary=%s",
            part->type, part->subtype, part->boundary);
    _headers_add_new(&part->headers,
            _header_make("Content-Type", NULL, &val));
    for (i = 0; i < subparts->count; i++)
        ptrarray_append(&part->subparts, ptrarray_nth(subparts, i));

    return part;
}

static struct emailpart *_email_buildbody(struct emailpart *text_body,
                                          struct emailpart *html_body,
                                          ptrarray_t *attachments)
{
    struct emailpart *root = NULL;

    /* Split attachments into inlined, emails and other files */
    ptrarray_t attached_emails = PTRARRAY_INITIALIZER;
    ptrarray_t attached_files = PTRARRAY_INITIALIZER;
    ptrarray_t inlined_files = PTRARRAY_INITIALIZER;
    int i;
    for (i = 0; i < attachments->count; i++) {
        struct emailpart *part = ptrarray_nth(attachments, i);
        if (part->type && !strcasecmp(part->type, "MESSAGE")) {
            ptrarray_append(&attached_emails, part);
        }
        else if (part->disposition && !strcasecmp(part->disposition, "INLINE") &&
                 (text_body || html_body)) {
            ptrarray_append(&inlined_files, part);
        }
        else {
            ptrarray_append(&attached_files, part);
        }
    }

    /* Make MIME part for embedded emails. */
    struct emailpart *emails = NULL;
    if (attached_emails.count >= 2)
        emails = _emailpart_new_multi("digest", &attached_emails);
    else if (attached_emails.count == 1)
        emails = ptrarray_nth(&attached_emails, 0);

    /* Make MIME part for text bodies. */
    struct emailpart *text = NULL;
    if (text_body && html_body) {
        ptrarray_t alternatives = PTRARRAY_INITIALIZER;
        ptrarray_append(&alternatives, text_body);
        ptrarray_append(&alternatives, html_body);
        text = _emailpart_new_multi("alternative", &alternatives);
        ptrarray_fini(&alternatives);
    }
    else if (text_body)
        text = text_body;
    else if (html_body)
        text = html_body;

    /* Make MIME part for inlined attachments, if any. */
    if (text && inlined_files.count) {
        struct emailpart *related = _emailpart_new_multi("related", &inlined_files);
        ptrarray_insert(&related->subparts, 0, text);
        text = related;
    }

    /* Choose top-level MIME part. */
    if (attached_files.count) {
        struct emailpart *mixed = _emailpart_new_multi("mixed", &attached_files);
        if (emails) ptrarray_insert(&mixed->subparts, 0, emails);
        if (text) ptrarray_insert(&mixed->subparts, 0, text);
        root = mixed;
    }
    else if (text && emails) {
        ptrarray_t wrapped = PTRARRAY_INITIALIZER;
        ptrarray_append(&wrapped, text);
        ptrarray_append(&wrapped, emails);
        root = _emailpart_new_multi("mixed", &wrapped);
        ptrarray_fini(&wrapped);
    }
    else if (text)
        root = text;
    else if (emails)
        root = emails;
    else
        root = NULL;

    ptrarray_fini(&attached_emails);
    ptrarray_fini(&attached_files);
    ptrarray_fini(&inlined_files);
    return root;
}


static void _email_parse_bodies(json_t *jemail,
                                struct jmap_parser *parser,
                                struct email *email)
{
    /* bodyValues */
    json_t *bodyValues = json_object_get(jemail, "bodyValues");
    if (json_is_object(bodyValues)) {
        const char *part_id;
        json_t *bodyValue;
        jmap_parser_push(parser, "bodyValues");
        json_object_foreach(bodyValues, part_id, bodyValue) {
            jmap_parser_push(parser, part_id);
            if (json_is_object(bodyValue)) {
                json_t *jval = json_object_get(bodyValue, "value");
                if (!json_is_string(jval)) {
                    jmap_parser_invalid(parser, "value");
                }
                jval = json_object_get(bodyValue, "isEncodingProblem");
                if (JNOTNULL(jval) && jval != json_false()) {
                    jmap_parser_invalid(parser, "isEncodingProblem");
                }
                jval = json_object_get(bodyValue, "isTruncated");
                if (JNOTNULL(jval) && jval != json_false()) {
                    jmap_parser_invalid(parser, "isTruncated");
                }
            }
            else {
                jmap_parser_invalid(parser, NULL);
            }
            jmap_parser_pop(parser);
        }
        jmap_parser_pop(parser);
    }
    else if (JNOTNULL(bodyValues)) {
        jmap_parser_invalid(parser, "bodyValues");
    }

    /* bodyStructure */
    json_t *jbody = json_object_get(jemail, "bodyStructure");
    if (json_is_object(jbody)) {
        jmap_parser_push(parser, "bodyStructure");
        email->body = _emailpart_parse(jbody, parser, bodyValues);
        jmap_parser_pop(parser);
        /* Top-level body part MUST NOT redefine headers in Email */
        if (email->body) {
            const char *name;
            json_t *jheader;
            json_object_foreach(email->body->headers.all, name, jheader) {
                if (json_object_get(email->headers.all, name)) {
                    /* Report offending header property */
                    json_t *jprop = json_object_get(jheader, "prop");
                    const char *prop = json_string_value(jprop);
                    if (prop) prop = "bodyStructure";
                    jmap_parser_invalid(parser, prop);
                }
            }
        }
    }
    else if (JNOTNULL(jbody)) {
        jmap_parser_invalid(parser, "bodyStructure");
    }

    json_t *jtextBody = json_object_get(jemail, "textBody");
    json_t *jhtmlBody = json_object_get(jemail, "htmlBody");
    json_t *jattachments = json_object_get(jemail, "attachments");

    struct emailpart *text_body = NULL;
    struct emailpart *html_body = NULL;
    ptrarray_t attachments = PTRARRAY_INITIALIZER; /* array of struct emailpart* */

    if (JNOTNULL(jbody)) {
        /* bodyStructure and fooBody are mutually exclusive */
        if (JNOTNULL(jtextBody)) {
            jmap_parser_invalid(parser, "textBody");
        }
        if (JNOTNULL(jhtmlBody)) {
            jmap_parser_invalid(parser, "htmlBody");
        }
        if (JNOTNULL(jattachments)) {
            jmap_parser_invalid(parser, "attachments");
        }
    }
    else {
        /* textBody */
        if (json_array_size(jtextBody) == 1) {
            json_t *jpart = json_array_get(jtextBody, 0);
            jmap_parser_push_index(parser, "textBody", 0, NULL);
            text_body = _emailpart_parse(jpart, parser, bodyValues);
            if (text_body) {
                if (!text_body->type) {
                    /* Set default type */
                    text_body->type = xstrdup("text");
                    text_body->subtype = xstrdup("plain");
                    struct buf val = BUF_INITIALIZER;
                    buf_setcstr(&val, "text/plain");
                    _headers_add_new(&text_body->headers,
                            _header_make("Content-Type", NULL, &val));
                }
                else if (strcasecmp(text_body->type, "text") ||
                         strcasecmp(text_body->subtype, "plain")) {
                    jmap_parser_invalid(parser, "type");
                }
            }
            jmap_parser_pop(parser);
        }
        else if (JNOTNULL(jtextBody)) {
            jmap_parser_invalid(parser, "textBody");
        }
        /* htmlBody */
        if (json_array_size(jhtmlBody) == 1) {
            json_t *jpart = json_array_get(jhtmlBody, 0);
            jmap_parser_push_index(parser, "htmlBody", 0, NULL);
            html_body = _emailpart_parse(jpart, parser, bodyValues);
            jmap_parser_pop(parser);
            if (html_body) {
                if (!html_body->type) {
                    /* Set default type */
                    html_body->type = xstrdup("text");
                    html_body->subtype = xstrdup("html");
                    struct buf val = BUF_INITIALIZER;
                    buf_setcstr(&val, "text/html");
                    _headers_add_new(&html_body->headers,
                            _header_make("Content-Type", NULL, &val));
                }
                else if (strcasecmp(html_body->type, "text") ||
                         strcasecmp(html_body->subtype, "html")) {
                    jmap_parser_invalid(parser, "htmlBody");
                }
            }
        }
        else if (JNOTNULL(jhtmlBody)) {
            jmap_parser_invalid(parser, "htmlBody");
        }
        /* attachments */
        if (json_is_array(jattachments)) {
            size_t i;
            json_t *jpart;
            struct emailpart *attpart;
            int have_inlined = 0;
            json_array_foreach(jattachments, i, jpart) {
                jmap_parser_push_index(parser, "attachments", i, NULL);
                attpart = _emailpart_parse(jpart, parser, bodyValues);
                if (attpart) {
                    if (!have_inlined && attpart->disposition) {
                        have_inlined = !strcasecmp(attpart->disposition, "INLINE");
                    }
                    ptrarray_append(&attachments, attpart);
                }
                jmap_parser_pop(parser);
            }
            if (have_inlined && !html_body) {
                /* Reject inlined attachments without a HTML body. The client
                 * is free to produce whatever it wants by setting bodyStructure,
                 * but for the convenience properties we require sane inputs. */
                jmap_parser_invalid(parser, "htmlBody");
            }
        }
        else if (JNOTNULL(jattachments)) {
            jmap_parser_invalid(parser, "attachments");
        }
    }

    /* calendarEvents is read-only */
    if (JNOTNULL(json_object_get(jemail, "calendarEvents"))) {
        jmap_parser_invalid(parser, "calendarEvents");
    }

    if (!email->body) {
        /* Build email body from convenience body properties */
        email->body = _email_buildbody(text_body, html_body, &attachments);
    }

    ptrarray_fini(&attachments);

    /* Look through all parts if any part is an attachment.
     * If so, set the hasAttachment flag. */
    if (email->body) {
        ptrarray_t work = PTRARRAY_INITIALIZER;
        ptrarray_append(&work, email->body);

        while (!email->has_attachment && work.count) {
            struct emailpart *part = ptrarray_pop(&work);
            if (part->disposition && strcasecmp(part->disposition, "INLINE")) {
                email->has_attachment = 1;
            }
            else if (part->filename) {
                email->has_attachment = 1;
            }
            else if (part->type && strcasecmp(part->type, "TEXT") &&
                     strcasecmp(part->type, "MULTIPART") &&
                     (!part->disposition || strcasecmp(part->disposition, "INLINE"))) {
                email->has_attachment = 1;
            }
            else if (part->blob_id && (!part->type || strcasecmp(part->type, "TEXT"))) {
                email->has_attachment = 1;
            }
            else {
                int i;
                for (i = 0; i < part->subparts.count; i++) {
                    struct emailpart *subpart = ptrarray_nth(&part->subparts, i);
                    ptrarray_append(&work, subpart);
                }
            }
        }
        ptrarray_fini(&work);
    }
}

static void _email_snoozed_parse(json_t *snoozed,
                                 struct jmap_parser *parser)
{
    const char *field;
    json_t *jval;

    jmap_parser_push(parser, "snoozed");
    json_object_foreach(snoozed, field, jval) {
        if (!strcmp(field, "until")) {
            if (!json_is_utcdate(jval)) {
                jmap_parser_invalid(parser, "until");
            }
        }
        else if (!strcmp(field, "setKeywords")) {
            const char *keyword;
            json_t *jbool;

            jmap_parser_push(parser, "setKeywords");
            json_object_foreach(jval, keyword, jbool) {
                if (!json_is_boolean(jbool) ||
                    !jmap_email_keyword_is_valid(keyword)) {
                    jmap_parser_invalid(parser, keyword);
                }
            }
            jmap_parser_pop(parser);
        }
        else if (strcmp(field, "moveToMailboxId")) {
            jmap_parser_invalid(parser, field);
        }
    }
    jmap_parser_pop(parser);
}

/* Parse a JMAP Email into its internal representation for creation. */
static void _parse_email(json_t *jemail,
                         struct jmap_parser *parser,
                         struct email *email)
{
    email->jemail = json_incref(jemail);

    /* mailboxIds */
    json_t *jmailboxIds = json_object_get(jemail, "mailboxIds");
    if (json_object_size(jmailboxIds)) {
        const char *mailboxid;
        json_t *jval;
        jmap_parser_push(parser, "mailboxIds");
        json_object_foreach(jmailboxIds, mailboxid, jval) {
            if (*mailboxid == '\0' || jval != json_true()) {
                jmap_parser_invalid(parser, NULL);
                break;
            }
        }
        jmap_parser_pop(parser);
    }
    else {
        jmap_parser_invalid(parser, "mailboxIds");
    }

    /* keywords */
    json_t *jkeywords = json_object_get(jemail, "keywords");
    if (json_is_object(jkeywords)) {
        const char *keyword;
        json_t *jval;
        jmap_parser_push(parser, "keywords");
        json_object_foreach(jkeywords, keyword, jval) {
            if (jval != json_true() || !jmap_email_keyword_is_valid(keyword)) {
                jmap_parser_invalid(parser, keyword);
            }
        }
        jmap_parser_pop(parser);
    }
    else if (JNOTNULL(jkeywords)) {
        jmap_parser_invalid(parser, "keywords");
    }

    /* headers */
    if (JNOTNULL(json_object_get(jemail, "headers"))) {
        jmap_parser_invalid(parser, "headers");
    }
    /* header:Xxx */
    _headers_parseprops(jemail, parser, &email->headers);
    size_t i;
    json_t *jheader;
    json_array_foreach(email->headers.raw, i, jheader) {
        const char *name = json_string_value(json_object_get(jheader, "name"));
        const char *val = json_string_value(json_object_get(jheader, "value"));
        /* Reject Content-Xxx headers in Email/headers */
        if (!strncasecmp("Content-", name, 8)) {
            char *tmp = strconcat("header:", name, NULL);
            jmap_parser_invalid(parser, tmp);
            free(tmp);
        }
        else if (!strcasecmp("Message-ID", name) || !strcasecmp("In-Reply-To", name)) {
            /* conversations.db will barf if these are invalid raw headers,
             * so make sure we reject invalid values here. */
            if (conversations_check_msgid(val, strlen(val))) {
                char *tmp = strconcat("header:", name, NULL);
                jmap_parser_invalid(parser, tmp);
                free(tmp);
            }
        }
    }

    /* Parse convenience header properties - in order as serialised */
    struct buf buf = BUF_INITIALIZER;
    json_t *prop;
    int seen_header;

    /* messageId */
    prop = json_object_get(jemail, "messageId");
    seen_header = _headers_have(&email->headers, "Message-Id");
    if (json_array_size(prop) == 1 && !seen_header) {
        _headers_add_new(&email->headers, _header_from_messageids(prop,
                    parser, "messageId", "Message-Id"));
    }
    else if (JNOTNULL(prop)) {
        jmap_parser_invalid(parser, "messageId");
    }
    /* inReplyTo */
    prop = json_object_get(jemail, "inReplyTo");
    seen_header = _headers_have(&email->headers, "In-Reply-To");
    if (json_is_array(prop) && !seen_header) {
        _headers_add_new(&email->headers, _header_from_messageids(prop,
                    parser, "inReplyTo", "In-Reply-To"));
    }
    else if (JNOTNULL(prop)) {
        jmap_parser_invalid(parser, "inReplyTo");
    }
    /* references */
    prop = json_object_get(jemail, "references");
    seen_header = _headers_have(&email->headers, "References");
    if (json_is_array(prop) && !seen_header) {
        _headers_add_new(&email->headers, _header_from_messageids(prop,
                    parser, "references", "References"));
    }
    else if (JNOTNULL(prop)) {
        jmap_parser_invalid(parser, "references");
    }
    /* sentAt */
    prop = json_object_get(jemail, "sentAt");
    seen_header = _headers_have(&email->headers, "Date");
    if (json_is_string(prop) && !seen_header) {
        _headers_add_new(&email->headers, _header_from_date(prop,
                    parser, "sentAt", "Date"));
    }
    else if (JNOTNULL(prop)) {
        jmap_parser_invalid(parser, "sentAt");
    }
    /* receivedAt */
    prop = json_object_get(jemail, "receivedAt");
    if (json_is_utcdate(prop)) {
        time_from_iso8601(json_string_value(prop), &email->internaldate);
    }
    else if (JNOTNULL(prop)) {
        jmap_parser_invalid(parser, "receivedAt");
    }
    /* from */
    prop = json_object_get(jemail, "from");
    seen_header = _headers_have(&email->headers, "From");
    if (json_is_array(prop) && !seen_header) {
        if ((jheader = _header_from_addresses(prop, parser, "from", "From"))) {
            _headers_add_new(&email->headers, jheader);
        }
    }
    else if (JNOTNULL(prop)) {
        jmap_parser_invalid(parser, "from");
    }
    /* replyTo */
    prop = json_object_get(jemail, "replyTo");
    seen_header = _headers_have(&email->headers, "Reply-To");
    if (json_is_array(prop) && !seen_header) {
        if ((jheader = _header_from_addresses(prop, parser, "replyTo", "Reply-To"))) {
            _headers_add_new(&email->headers, jheader);
        }
    }
    else if (JNOTNULL(prop)) {
        jmap_parser_invalid(parser, "replyTo");
    }
    /* sender */
    prop = json_object_get(jemail, "sender");
    seen_header = _headers_have(&email->headers, "Sender");
    if (json_is_array(prop) && !seen_header) {
        if ((jheader = _header_from_addresses(prop, parser, "sender", "Sender"))) {
            _headers_add_new(&email->headers, jheader);
        }
    }
    else if (JNOTNULL(prop)) {
        jmap_parser_invalid(parser, "sender");
    }
    /* to */
    prop = json_object_get(jemail, "to");
    seen_header = _headers_have(&email->headers, "To");
    if (json_is_array(prop) && !seen_header) {
        if ((jheader = _header_from_addresses(prop, parser, "to", "To"))) {
            _headers_add_new(&email->headers, jheader);
        }
    }
    else if (JNOTNULL(prop)) {
        jmap_parser_invalid(parser, "to");
    }
    /* cc */
    prop = json_object_get(jemail, "cc");
    seen_header = _headers_have(&email->headers, "Cc");
    if (json_is_array(prop) && !seen_header) {
        if ((jheader = _header_from_addresses(prop, parser, "cc", "Cc"))) {
            _headers_add_new(&email->headers, jheader);
        }
    }
    else if (JNOTNULL(prop)) {
        jmap_parser_invalid(parser, "cc");
    }
    /* bcc */
    prop = json_object_get(jemail, "bcc");
    seen_header = _headers_have(&email->headers, "Bcc");
    if (json_is_array(prop) && !seen_header) {
        if ((jheader = _header_from_addresses(prop, parser, "bcc", "Bcc"))) {
            _headers_add_new(&email->headers, jheader);
        }
    }
    else if (JNOTNULL(prop)) {
        jmap_parser_invalid(parser, "bcc");
    }
    /* subject */
    prop = json_object_get(jemail, "subject");
    seen_header = _headers_have(&email->headers, "Subject");
    if (json_is_string(prop) && !seen_header) {
        if ((jheader = _header_from_text(prop, parser, "subject", "Subject"))) {
            _headers_add_new(&email->headers, jheader);
        }
    }
    else if (JNOTNULL(prop)) {
        jmap_parser_invalid(parser, "subject");
    }
    buf_free(&buf);

    /* Parse bodies */
    _email_parse_bodies(jemail, parser, email);

    /* Is snoozed being set? */
    json_t *snoozed = json_object_get(jemail, "snoozed");
    if (json_is_object(snoozed)) {
        _email_snoozed_parse(snoozed, parser);
    }
    else if (JNOTNULL(snoozed)) {
        jmap_parser_invalid(parser, "snoozed");
    }
    email->snoozed = snoozed;
}

static void _emailpart_blob_to_mime(jmap_req_t *req,
                                    FILE *fp,
                                    struct emailpart *emailpart,
                                    json_t *missing_blobs)
{
    struct buf blob_buf = BUF_INITIALIZER;
    msgrecord_t *mr = NULL;
    struct mailbox *mbox = NULL;
    struct body *body = NULL;
    const struct body *part = NULL;

    /* Find body part containing blob */
    int r = jmap_findblob(req, NULL/*accountid*/, emailpart->blob_id,
                          &mbox, &mr, &body, &part, &blob_buf);
    if (r) goto done;

    uint32_t size;
    r = msgrecord_get_size(mr, &size);
    if (r) goto done;

    /* Fetch blob contents and headers */
    const char *content = blob_buf.s;
    size_t content_size = blob_buf.len;
    const char *src_encoding = NULL;
    if (part) {
        content += part->content_offset;
        content_size = part->content_size;
        src_encoding = part->encoding;
    }

    /* Determine target encoding */
    const char *encoding = src_encoding;
    char *encbuf = NULL;

    if (!strcasecmpsafe(emailpart->type, "MESSAGE")) {
        if (!strcasecmpsafe(src_encoding, "BASE64")) {
            /* This is a MESSAGE and hence it is only allowed
             * to be in 7bit, 8bit or binary encoding. Base64
             * is not allowed, so let's decode the blob and
             * assume it to be in binary encoding. */
            encoding = "BINARY";
            content = charset_decode_mimebody(content, content_size,
                    ENCODING_BASE64, &encbuf, &content_size);
        }
    }
    else if (strcasecmpsafe(src_encoding, "QUOTED-PRINTABLE") &&
             strcasecmpsafe(src_encoding, "BASE64")) {
        /* Encode text to quoted-printable, if it isn't an attachment */
        if (!strcasecmpsafe(emailpart->type, "TEXT") &&
            (!strcasecmpsafe(emailpart->subtype, "PLAIN") ||
             !strcasecmpsafe(emailpart->subtype, "HTML")) &&
            (!emailpart->disposition || !strcasecmp(emailpart->disposition, "INLINE"))) {
            encoding = "QUOTED-PRINTABLE";
            size_t lenqp = 0;
            encbuf = charset_qpencode_mimebody(content, content_size, 0, &lenqp);
            content = encbuf;
            content_size = lenqp;
        }
        /* Encode all other types to base64 */
        else {
            encoding = "BASE64";
            size_t len64 = 0;
            /* Pre-flight encoder to determine length */
            charset_encode_mimebody(NULL, content_size, NULL, &len64, NULL, 1 /* wrap */);
            if (len64) {
                /* Now encode the body */
                encbuf = xmalloc(len64);
                charset_encode_mimebody(content, content_size, encbuf, &len64, NULL, 1 /* wrap */);
            }
            content = encbuf;
            content_size = len64;
        }
    }

    /* Write headers defined by client. */
    size_t i;
    json_t *jheader;
    json_array_foreach(emailpart->headers.raw, i, jheader) {
        json_t *jval = json_object_get(jheader, "name");
        const char *name = json_string_value(jval);
        jval = json_object_get(jheader, "value");
        const char *value = json_string_value(jval);
        fprintf(fp, "%s: %s\r\n", name, value);
    }

    /* Write encoding header, if required */
    if (encoding) {
        fputs("Content-Transfer-Encoding: ", fp);
        fputs(encoding, fp);
        fputs("\r\n", fp);
    }
    /* Write body */
    fputs("\r\n", fp);
    if (content_size) fwrite(content, 1, content_size, fp);
    free(encbuf);

done:
    if (r) json_array_append_new(missing_blobs, json_string(emailpart->blob_id));
    if (body) {
        message_free_body(body);
        free(body);
    }
    msgrecord_unref(&mr);
    jmap_closembox(req, &mbox);
    buf_free(&blob_buf);
}

static void _emailpart_text_to_mime(FILE *fp, struct emailpart *part)
{
    json_t *jval = json_object_get(part->jbody, "value");
    const char *text = json_string_value(jval);
    size_t len = strlen(text);

    /* Check and sanitise text */
    int has_long_lines = 0;
    int is_7bit = 1;
    const char *p = text;
    const char *base = text;
    const char *top = text + len;
    const char *last_lf = p;
    struct buf txtbuf = BUF_INITIALIZER;
    for (p = base; p < top; p++) {
        /* Keep track of line-length and high-bit bytes */
        if (p - last_lf > 998)
            has_long_lines = 1;
        if (*p == '\n')
            last_lf = p;
        if (*p & 0x80)
            is_7bit = 0;
        /* Omit CR */
        if (*p == '\r')
            continue;
        /* Expand LF to CRLF */
        if (*p == '\n')
            buf_putc(&txtbuf, '\r');
        buf_putc(&txtbuf, *p);
    }
    const char *charset = NULL;
    if (!is_7bit) charset = "utf-8";

    /* Write headers */
    size_t i;
    json_t *jheader;
    json_array_foreach(part->headers.raw, i, jheader) {
        json_t *jval = json_object_get(jheader, "name");
        const char *name = json_string_value(jval);
        jval = json_object_get(jheader, "value");
        const char *value = json_string_value(jval);
        if (!strcasecmp(name, "Content-Type") && charset) {
            /* Clients are forbidden to set charset on TEXT bodies,
             * so make sure we properly set the parameter value. */
            fprintf(fp, "%s: %s;charset=%s\r\n", name, value, charset);
        }
        else {
            fprintf(fp, "%s: %s\r\n", name, value);
        }
    }
    /* Write body */
    if (!is_7bit || has_long_lines) {
        /* Write quoted printable */
        size_t qp_len = 0;
        char *qp_text = charset_qpencode_mimebody(txtbuf.s, txtbuf.len, 1, &qp_len);
        fputs("Content-Transfer-Encoding: quoted-printable\r\n", fp);
        fputs("\r\n", fp);
        fwrite(qp_text, 1, qp_len, fp);
        free(qp_text);
    }
    else {
        /*  Write plain */
        fputs("\r\n", fp);
        fwrite(buf_cstring(&txtbuf), 1, buf_len(&txtbuf), fp);
    }

    buf_free(&txtbuf);
}

static void _emailpart_to_mime(jmap_req_t *req, FILE *fp,
                               struct emailpart *part,
                               json_t *missing_blobs)
{
    if (part->subparts.count) {
        /* Write raw headers */
        size_t i;
        json_t *jheader;
        json_array_foreach(part->headers.raw, i, jheader) {
            json_t *jval = json_object_get(jheader, "name");
            const char *name = json_string_value(jval);
            jval = json_object_get(jheader, "value");
            const char *value = json_string_value(jval);
            fprintf(fp, "%s: %s\r\n", name, value);
        }
        /* Write default Content-Type, if not set */
        if (!_headers_have(&part->headers, "Content-Type")) {
            part->boundary = _mime_make_boundary();
            fputs("Content-Type: multipart/mixed;boundary=", fp);
            fputs(part->boundary, fp);
            fputs("\r\n", fp);
        }
        /* Write sub parts */
        int j;
        int is_digest = part->type && !strcasecmp(part->type, "MULTIPART") &&
                        part->subtype && !strcasecmp(part->subtype, "DIGEST");
        for (j = 0; j < part->subparts.count; j++) {
            struct emailpart *subpart = ptrarray_nth(&part->subparts, j);
            if (is_digest && !subpart->type && subpart->blob_id) {
                /* multipart/digest changes the default content type of this
                 * part from text/plain to message/rfc822, so make sure that
                 * emailpart_blob_to_mime will properly deal with it */
                subpart->type = xstrdup("MESSAGE");
                subpart->subtype = xstrdup("RFC822");
            }
            fprintf(fp, "\r\n--%s\r\n", part->boundary);
            _emailpart_to_mime(req, fp, subpart, missing_blobs);
        }
        fprintf(fp, "\r\n--%s--\r\n", part->boundary);
    }
    else if (part->jbody) {
        _emailpart_text_to_mime(fp, part);
    }
    else if (part->blob_id) {
        _emailpart_blob_to_mime(req, fp, part, missing_blobs);
    }
}

static int _email_have_toplevel_header(struct email *email, const char *lcasename)
{
    json_t *header = json_object_get(email->headers.all, lcasename);
    if (!header && email->body) {
        header = json_object_get(email->body->headers.all, lcasename);
    }
    return JNOTNULL(header);
}

static int _email_to_mime(jmap_req_t *req, FILE *fp, void *rock, json_t **err)
{
    struct email *email = rock;
    json_t *header;
    size_t i;

    /* Set mandatory and quasi-mandatory headers */
    if (!_email_have_toplevel_header(email, "mime-version")) {
        header = json_pack("{s:s s:s}", "name", "Mime-Version", "value", "1.0");
        _headers_shift_new(&email->headers, header);
    }
    if (!_email_have_toplevel_header(email, "user-agent")) {
        char *tmp = strconcat("Cyrus-JMAP/", CYRUS_VERSION, NULL);
        header = json_pack("{s:s s:s}", "name", "User-Agent", "value", tmp);
        _headers_shift_new(&email->headers, header);
        free(tmp);
    }
    if (!_email_have_toplevel_header(email, "message-id")) {
        struct buf buf = BUF_INITIALIZER;
        buf_printf(&buf, "<%s@%s>", makeuuid(), config_servername);
        header = json_pack("{s:s s:s}", "name", "Message-Id", "value", buf_cstring(&buf));
        _headers_shift_new(&email->headers, header);
        buf_free(&buf);
    }
    if (!_email_have_toplevel_header(email, "date")) {
        char fmt[RFC5322_DATETIME_MAX+1];
        memset(fmt, 0, RFC5322_DATETIME_MAX+1);
        time_to_rfc5322(time(NULL), fmt, RFC5322_DATETIME_MAX+1);
        header = json_pack("{s:s s:s}", "name", "Date", "value", fmt);
        _headers_shift_new(&email->headers, header);
    }
    if (!_email_have_toplevel_header(email, "from")) {
        header = json_pack("{s:s s:s}", "name", "From", "value", req->userid);
        _headers_shift_new(&email->headers, header);
    }

    /* Write headers */
    json_array_foreach(email->headers.raw, i, header) {
        json_t *jval;
        jval = json_object_get(header, "name");
        const char *name = json_string_value(jval);
        jval = json_object_get(header, "value");
        const char *value = json_string_value(jval);
        fprintf(fp, "%s: %s\r\n", name, value);
    }

    json_t *missing_blobs = json_array();
    if (email->body) _emailpart_to_mime(req, fp, email->body, missing_blobs);
    if (json_array_size(missing_blobs)) {
        *err = json_pack("{s:s s:o}", "type", "blobNotFound",
                "notFound", missing_blobs);
    }
    else {
        json_decref(missing_blobs);
    }

    return 0;
}

static void _append_validate_mboxids(jmap_req_t *req,
                                     json_t *jmailboxids,
                                     struct jmap_parser *parser,
                                     int *have_snoozed_mboxid)
{
    char *snoozed_mboxname = NULL, *snoozed_uniqueid = NULL;
    const char *mbox_id;
    json_t *jval;
    void *tmp;

    jmap_mailbox_find_role(req, "snoozed", &snoozed_mboxname, &snoozed_uniqueid);

    jmap_parser_push(parser, "mailboxIds");
    json_object_foreach_safe(jmailboxids, tmp, mbox_id, jval) {
        int need_rights = ACL_LOOKUP|ACL_INSERT;
        int is_valid = 1;
        if (*mbox_id == '$') {
            /* Lookup mailbox by role */
            const char *role = mbox_id + 1;
            char *uniqueid = NULL;
            char *mboxname = NULL;
            if (snoozed_uniqueid && !strcmp(role, "snoozed") &&
                jmap_hasrights_byname(req, snoozed_mboxname, need_rights)) {
                /* Flag this mailboxId as being $snoozed */
                json_object_del(jmailboxids, mbox_id);
                json_object_set_new(jmailboxids,
                                    snoozed_uniqueid, json_string("$snoozed"));
                *have_snoozed_mboxid = 1;
            }
            else if (!jmap_mailbox_find_role(req, role, &mboxname, &uniqueid) &&
                jmap_hasrights_byname(req, mboxname, need_rights)) {
                json_object_del(jmailboxids, mbox_id);
                json_object_set_new(jmailboxids, uniqueid, json_true());
            }
            else {
                jmap_parser_invalid(parser, NULL);
                is_valid = 0;
            }
            free(uniqueid);
            free(mboxname);
        }
        else {
            /* Lookup mailbox by id */
            const mbentry_t *mbentry = NULL;
            if (*mbox_id == '#') {
                mbox_id = jmap_lookup_id(req, mbox_id + 1);
            }
            if (mbox_id) {
                mbentry = jmap_mbentry_by_uniqueid(req, mbox_id);
            }
            if (!mbentry || !jmap_hasrights(req, mbentry, need_rights)) {
                jmap_parser_invalid(parser, NULL);
                is_valid = 0;
            }
            else if (!strcmpnull(snoozed_uniqueid, mbentry->uniqueid)) {
                /* Flag this mailboxId as being $snoozed */
                json_object_set_new(jmailboxids,
                                    mbox_id, json_string("$snoozed"));
                *have_snoozed_mboxid = 1;
            }
        }
        if (!is_valid) break;
    }
    jmap_parser_pop(parser);

    free(snoozed_mboxname);
    free(snoozed_uniqueid);
}

static void _email_create(jmap_req_t *req,
                          json_t *jemail,
                          json_t **new_email,
                          json_t **set_err)
{
    strarray_t keywords = STRARRAY_INITIALIZER;
    int r = 0, have_snoozed_mboxid = 0;
    *set_err = NULL;
    struct email_append_detail detail;
    memset(&detail, 0, sizeof(struct email_append_detail));

    /* Parse Email object into internal representation */
    struct jmap_parser parser = JMAP_PARSER_INITIALIZER;
    struct email email = { HEADERS_INITIALIZER, NULL, NULL, time(NULL), 0, NULL };
    _parse_email(jemail, &parser, &email);

    /* Validate mailboxIds */
    json_t *jmailboxids = json_copy(json_object_get(jemail, "mailboxIds"));
    _append_validate_mboxids(req, jmailboxids, &parser, &have_snoozed_mboxid);

    /* Validate snoozed + mailboxIds */
    if (json_is_object(email.snoozed) && !have_snoozed_mboxid) {
        jmap_parser_invalid(&parser, "snoozed");
    }
    if (json_array_size(parser.invalid)) {
        *set_err = json_pack("{s:s s:O}", "type", "invalidProperties",
                "properties", parser.invalid);
        goto done;
    }

    /* Gather keywords */
    json_t *jkeywords = json_object_get(jemail, "keywords");
    if (json_object_size(jkeywords)) {
        json_t *jval;
        const char *keyword;
        json_object_foreach(jkeywords, keyword, jval) {
            if (!strcasecmp(keyword, JMAP_HAS_ATTACHMENT_FLAG)) {
                continue;
            }
            strarray_append(&keywords, keyword);
        }
    }
    if (keywords.count > MAX_USER_FLAGS) {
        *set_err = json_pack("{s:s}",  "type", "tooManyKeywords");
        goto done;
    }

    /* Append MIME-encoded Email to mailboxes and write keywords */

    _email_append(req, jmailboxids, &keywords, email.internaldate, email.snoozed,
                  config_getswitch(IMAPOPT_JMAP_SET_HAS_ATTACHMENT) ?
                  email.has_attachment : 0, NULL, _email_to_mime, &email,
                  &detail, set_err);
    if (*set_err) goto done;

    /* Return newly created Email object */
    *new_email = json_pack("{s:s, s:s, s:s, s:i}",
         "id", detail.email_id,
         "blobId", detail.blob_id,
         "threadId", detail.thread_id,
         "size", detail.size);
    *set_err = NULL;

done:
    if (r && *set_err == NULL) {
        syslog(LOG_ERR, "jmap: email_create: %s", error_message(r));
        if (r == IMAP_QUOTA_EXCEEDED)
            *set_err = json_pack("{s:s}", "type", "overQuota");
        else
            *set_err = jmap_server_error(r);
    }
    json_decref(jmailboxids);
    strarray_fini(&keywords);
    jmap_parser_fini(&parser);
    _email_fini(&email);
}

static int _email_uidrec_compareuid_cb(const void **pa, const void **pb)
{
    const struct email_uidrec *a = *pa;
    const struct email_uidrec *b = *pb;
    if (a->uid < b->uid)
        return -1;
    else if (a->uid > b->uid)
        return 1;
    else
        return 0;
}

static void _email_mboxrec_free(struct email_mboxrec *mboxrec)
{
    struct email_uidrec *uidrec;
    while ((uidrec = ptrarray_pop(&mboxrec->uidrecs))) {
        free(uidrec->email_id);
        free(uidrec);
    }
    ptrarray_fini(&mboxrec->uidrecs);
    free(mboxrec->mboxname);
    free(mboxrec->mbox_id);
    free(mboxrec);
}

static void _email_mboxrecs_free(ptrarray_t **mboxrecsptr)
{
    if (mboxrecsptr == NULL || *mboxrecsptr == NULL) return;

    ptrarray_t *mboxrecs = *mboxrecsptr;
    int i;
    for (i = 0; i < ptrarray_size(mboxrecs); i++) {
        _email_mboxrec_free(ptrarray_nth(mboxrecs, i));
    }
    ptrarray_free(mboxrecs);
    *mboxrecsptr = NULL;
}

struct email_mboxrecs_make_rock {
    jmap_req_t *req;
    const char *email_id;
    ptrarray_t *mboxrecs;
};

static int _email_mboxrecs_read_cb(const conv_guidrec_t *rec, void *_rock)
{
    struct email_mboxrecs_make_rock *rock = _rock;
    ptrarray_t *mboxrecs = rock->mboxrecs;

    /* don't process emails that have this email attached! */
    if (rec->part) return 0;

    if (!jmap_hasrights_byname(rock->req, rec->mboxname, ACL_READ|ACL_LOOKUP)) return 0;

    /* Check if there's already a mboxrec for this mailbox. */
    int i;
    struct email_mboxrec *mboxrec = NULL;
    for (i = 0; i < ptrarray_size(mboxrecs); i++) {
        struct email_mboxrec *p = ptrarray_nth(mboxrecs, i);
        if (!strcmp(rec->mboxname, p->mboxname)) {
            mboxrec = p;
            break;
        }
    }
    if (mboxrec == NULL) {
        mbentry_t *mbentry = NULL;
        int r = mboxlist_lookup(rec->mboxname, &mbentry, NULL);
        if (r) return r;

        // we only want regular mailboxes!
        if (mbentry->mbtype & MBTYPES_NONIMAP) {
            mboxlist_entry_free(&mbentry);
            return 0;
        }

        mboxrec = xzmalloc(sizeof(struct email_mboxrec));
        mboxrec->mboxname = xstrdup(rec->mboxname);
        mboxrec->mbox_id = xstrdup(mbentry->uniqueid);
        ptrarray_append(mboxrecs, mboxrec);
        mboxlist_entry_free(&mbentry);
    }

    struct email_uidrec *uidrec = xzmalloc(sizeof(struct email_uidrec));
    uidrec->mboxrec = mboxrec;
    uidrec->email_id = xstrdup(rock->email_id);
    uidrec->uid = rec->uid;
    uidrec->is_snoozed =
        ((rec->internal_flags & (FLAG_INTERNAL_SNOOZED | FLAG_INTERNAL_EXPUNGED))
         == FLAG_INTERNAL_SNOOZED);
    ptrarray_append(&mboxrec->uidrecs, uidrec);

    return 0;
}

static void _email_mboxrecs_read(jmap_req_t *req,
                                 struct conversations_state *cstate,
                                 strarray_t *email_ids,
                                 json_t *set_errors,
                                 ptrarray_t **mboxrecsptr)
{
    ptrarray_t *mboxrecs = ptrarray_new();

    int i;
    for (i = 0; i < strarray_size(email_ids); i++) {
        const char *email_id = strarray_nth(email_ids, i);
        if (email_id[0] != 'M' || strlen(email_id) != 25) {
            // not a valid emailId
            continue;
        }

        struct email_mboxrecs_make_rock rock = { req, email_id, mboxrecs };
        int r = conversations_guid_foreach(cstate, _guid_from_id(email_id),
                                           _email_mboxrecs_read_cb, &rock);
        if (r) {
            json_t *err = (r == IMAP_NOTFOUND || r == IMAP_PERMISSION_DENIED) ?
                json_pack("{s:s}", "notFound") : jmap_server_error(r);
            json_object_set_new(set_errors, email_id, err);
            _email_mboxrecs_free(&mboxrecs);
        }
    }

    // sort the UID lists
    for (i = 0; i < ptrarray_size(mboxrecs); i++) {
        struct email_mboxrec *p = ptrarray_nth(mboxrecs, i);
        ptrarray_sort(&p->uidrecs, _email_uidrec_compareuid_cb);
    }

    *mboxrecsptr = mboxrecs;
}

/* Parsed JMAP Email/set#{update} argument. */
struct email_update {
    const char *email_id;     /* Id of updated JMAP email */
    json_t *keywords;         /* JMAP Email/set keywords argument */
    int patch_keywords;       /* True if keywords is a patch object */
    json_t *full_keywords;    /* Patched JMAP keywords across index records */
    json_t *mailboxids;       /* JMAP Email/set mailboxIds argument */
    int patch_mailboxids;     /* True if mailboxids is a patch object */
    json_t *snoozed;          /* JMAP Email/set snoozed argument */
    int patch_snoozed;        /* True if snoozed is a patch object */
    struct email_uidrec *snoozed_uidrec; /* Currently snoozed email */
    char *snooze_in_mboxid;   /* Snooze the email in this mailboxid */
};

static void _email_update_free(struct email_update *update)
{
    json_decref(update->keywords);
    json_decref(update->full_keywords);
    json_decref(update->mailboxids);
    json_decref(update->snoozed);
    free(update->snooze_in_mboxid);
    free(update);
}

struct modified_flags {
    int added_flags;
    bit32 added_system_flags;
    bit32 added_user_flags[MAX_USER_FLAGS/32];
    int removed_flags;
    bit32 removed_system_flags;
    bit32 removed_user_flags[MAX_USER_FLAGS/32];
};

/* Overwrite or patch the JMAP keywords on message record mrw.
 * If add_seen_uids or del_seen_uids is not NULL, then
 * the record UID is added to respective sequence set,
 * if the flag must be set or deleted. */
static int _email_setflags(json_t *keywords, int patch_keywords,
                           msgrecord_t *mrw,
                           struct seqset *add_seen_uids,
                           struct seqset *del_seen_uids,
                           struct modified_flags *modflags)
{
    uint32_t internal_flags = 0;
    struct mailbox *mbox = NULL;
    uint32_t uid = 0;

    int r = msgrecord_get_mailbox(mrw, &mbox);
    if (r) return r;
    r = msgrecord_get_uid(mrw, &uid);
    if (r) goto done;
    r = msgrecord_get_internalflags(mrw, &internal_flags);
    if (r) goto done;
    if (internal_flags & FLAG_INTERNAL_EXPUNGED) goto done;

    uint32_t old_system_flags = 0;
    r = msgrecord_get_systemflags(mrw, &old_system_flags);
    if (r) goto done;

    uint32_t old_user_flags[MAX_USER_FLAGS/32];
    r = msgrecord_get_userflags(mrw, old_user_flags);
    if (r) goto done;

    /* Determine if to patch or reset flags */
    uint32_t new_system_flags = 0;
    uint32_t new_user_flags[MAX_USER_FLAGS/32];
    if (patch_keywords) {
        new_system_flags = old_system_flags;
        memcpy(new_user_flags, old_user_flags, sizeof(old_user_flags));
    }
    else {
        new_system_flags = (old_system_flags & ~FLAGS_SYSTEM) |
                           (old_system_flags & FLAG_DELETED);
        memset(new_user_flags, 0, sizeof(new_user_flags));
    }

    /* Update flags */
    json_t *jval;
    const char *keyword;
    json_object_foreach(keywords, keyword, jval) {
        if (!strcasecmp(keyword, "$Flagged")) {
            if (jval == json_true())
                new_system_flags |= FLAG_FLAGGED;
            else
                new_system_flags &= ~FLAG_FLAGGED;
        }
        else if (!strcasecmp(keyword, "$Answered")) {
            if (jval == json_true())
                new_system_flags |= FLAG_ANSWERED;
            else
                new_system_flags &= ~FLAG_ANSWERED;
        }
        else if (!strcasecmp(keyword, "$Seen")) {
            if (jval == json_true()) {
                if (add_seen_uids)
                    seqset_add(add_seen_uids, uid, 1);
                else
                    new_system_flags |= FLAG_SEEN;
            }
            else {
                if (del_seen_uids)
                    seqset_add(del_seen_uids, uid, 1);
                else
                    new_system_flags &= ~FLAG_SEEN;
            }
        }
        else if (!strcasecmp(keyword, "$Draft")) {
            if (jval == json_true())
                new_system_flags |= FLAG_DRAFT;
            else
                new_system_flags &= ~FLAG_DRAFT;
        }
        else {
            int userflag;
            r = mailbox_user_flag(mbox, keyword, &userflag, 1);
            if (r) goto done;
            if (jval == json_true())
                new_user_flags[userflag/32] |= 1<<(userflag&31);
            else
                new_user_flags[userflag/32] &= ~(1<<(userflag&31));
        }
    }
    if (!patch_keywords && del_seen_uids) {
        if (json_object_get(keywords, "$seen") == NULL) {
            seqset_add(del_seen_uids, uid, 1);
        }
    }

    /* Write flags to record */
    r = msgrecord_set_systemflags(mrw, new_system_flags);
    if (r) goto done;
    r = msgrecord_set_userflags(mrw, new_user_flags);
    if (r) goto done;
    r = msgrecord_rewrite(mrw);

    /* Determine flag delta */
    memset(modflags, 0, sizeof(struct modified_flags));
    modflags->added_system_flags = ~old_system_flags & new_system_flags & FLAGS_SYSTEM;
    if (modflags->added_system_flags) {
        modflags->added_flags = 1;
    }
    modflags->removed_system_flags = old_system_flags & ~new_system_flags & FLAGS_SYSTEM;
    if (modflags->removed_system_flags) {
        modflags->removed_flags = 1;
    }

    size_t i;
    for (i = 0; i < MAX_USER_FLAGS/32; i++) {
        modflags->added_user_flags[i] = ~old_user_flags[i] & new_user_flags[i];
        if (modflags->added_user_flags[i]) {
            modflags->added_flags = 1;
        }
        modflags->removed_user_flags[i] = old_user_flags[i] & ~new_user_flags[i];
        if (modflags->removed_user_flags[i]) {
            modflags->removed_flags = 1;
        }
    }

done:
    return r;
}

struct email_bulkupdate {
    jmap_req_t *req;                /* JMAP Email/set request context */
    hash_table updates_by_email_id; /* Map to ptrarray of email_update */
    hash_table uidrecs_by_email_id; /* Map to ptrarray of email_uidrec, excluding expunged */
    hash_table plans_by_mbox_id;    /* Map to email_updateplan */
    json_t *set_errors;             /* JMAP SetError by email id */
    struct seen *seendb;            /* Seen database for shared mailboxes, or NULL */
    ptrarray_t *cur_mboxrecs;       /* List of current mbox and UI recs, including expunged */
    ptrarray_t *new_mboxrecs;       /* New mbox and UID records allocated by planner */
};

#define _EMAIL_BULKUPDATE_INITIALIZER {\
    NULL, \
    HASH_TABLE_INITIALIZER, \
    HASH_TABLE_INITIALIZER, \
    HASH_TABLE_INITIALIZER, \
    json_object(), \
    NULL, \
    NULL, \
    ptrarray_new() \
}

static void _email_update_parse(json_t *jemail,
                                struct jmap_parser *parser,
                                struct email_update *update)
{
    struct buf buf = BUF_INITIALIZER;

    /* Are keywords overwritten or patched? */
    json_t *keywords = json_object_get(jemail, "keywords");
    if (keywords == NULL) {
        /* Collect keywords as patch */
        const char *field = NULL;
        json_t *jval;
        keywords = json_object();
        json_object_foreach(jemail, field, jval) {
            if (strncmp(field, "keywords/", 9))  {
                continue;
            }
            const char *keyword = field + 9;
            if (!jmap_email_keyword_is_valid(keyword) || (jval != json_true() && jval != json_null())) {
                jmap_parser_push(parser, "keywords");
                jmap_parser_invalid(parser, keyword);
                jmap_parser_pop(parser);
                continue;
            }
            else if (!strcasecmp(keyword, JMAP_HAS_ATTACHMENT_FLAG)) {
                continue;
            }
            /* At least one keyword gets patched */
            update->patch_keywords = 1;
            /* Normalize keywords to lowercase */
            buf_setcstr(&buf, keyword);
            buf_lcase(&buf);
            json_object_set(keywords, buf_cstring(&buf), jval);
        }
        if (!json_object_size(keywords)) {
            json_decref(keywords);
            keywords = NULL;
        }
    }
    else if (json_is_object(keywords)) {
        /* Overwrite keywords */
        json_t *normalized_keywords = json_object();
        const char *keyword;
        json_t *jval;
        json_object_foreach(keywords, keyword, jval) {
            if (!jmap_email_keyword_is_valid(keyword) || jval != json_true()) {
                jmap_parser_push(parser, "keywords");
                jmap_parser_invalid(parser, keyword);
                jmap_parser_pop(parser);
                continue;
            }
            else if (!strcasecmp(keyword, JMAP_HAS_ATTACHMENT_FLAG)) {
                continue;
            }
            buf_setcstr(&buf, keyword);
            buf_lcase(&buf);
            json_object_set(normalized_keywords, buf_cstring(&buf), jval);
        }
        keywords = normalized_keywords;
    }
    else if (JNOTNULL(keywords)) {
        jmap_parser_invalid(parser, "keywords");
    }
    update->keywords = keywords;

    /* Are mailboxes being overwritten or patched? */
    json_t *mailboxids = json_copy(json_object_get(jemail, "mailboxIds"));
    if (mailboxids == NULL) {
        /* Collect mailboxids as patch */
        const char *field = NULL;
        json_t *jval;
        mailboxids = json_object();
        /* Check if mailboxIds are patched */
        json_object_foreach(jemail, field, jval) {
            if (strncmp(field, "mailboxIds/", 11)) {
                continue;
            }
            update->patch_mailboxids = 1;
            if (jval == json_true() || jval == json_null()) {
                json_object_set(mailboxids, field + 11, jval);
            }
            else {
                jmap_parser_invalid(parser, field);
            }
        }
        if (json_object_size(mailboxids) == 0) {
            json_decref(mailboxids);
            mailboxids = NULL;
        }
    }
    update->mailboxids = mailboxids;

    /* Is snoozed being overwritten or patched? */
    json_t *snoozed = json_copy(json_object_get(jemail, "snoozed"));
    if (snoozed == NULL) {
        /* Collect fields as patch */
        const char *field = NULL;
        json_t *jval;

        snoozed = json_object();
        json_object_foreach(jemail, field, jval) {
            int invalid = 0;

            if (strncmp(field, "snoozed/", 8)) {
                continue;
            }

            const char *subfield = field +8;
            if (!strcmp(subfield, "until")) {
                if (!json_is_utcdate(jval)) invalid = 1;
            }
            else if (!strncmp(subfield, "setKeywords/", 12)) {
                const char *keyword = subfield + 12;
                if (!(json_is_boolean(jval) || json_is_null(jval)) ||
                    !jmap_email_keyword_is_valid(keyword)) invalid = 1;
            }
            else if (!strcmp(subfield, "moveToMailboxId")) {
                if (!json_is_string(jval)) invalid = 1;
            }
            else invalid = 1;

            if (invalid) {
                jmap_parser_invalid(parser, field);
            }
            else {
                /* At least one field gets patched */
                update->patch_snoozed = 1;
                json_object_set(snoozed, subfield, jval);
            }
        }
        if (json_object_size(snoozed) == 0) {
            json_decref(snoozed);
            snoozed = NULL;
        }
    }
    else if (json_is_object(snoozed)) {
        _email_snoozed_parse(snoozed, parser);
    }
    else if (JNOTNULL(snoozed)) {
        jmap_parser_invalid(parser, "snoozed");
    }
    update->snoozed = snoozed;

    buf_free(&buf);
}

/* A plan to create, update or destroy messages per mailbox */
struct email_updateplan {
    char *mboxname;       /* Mailbox IMAP name */
    char *mbox_id;        /* Mailbox unique id */
    struct mailbox *mbox; /* Write-locked mailbox */
    ptrarray_t copy;      /* Array of array of email_uidrec, grouped by mailbox */
    ptrarray_t setflags;  /* Array of email_uidrec */
    ptrarray_t delete;    /* Array of email_uidrec */
    ptrarray_t snooze;    /* Array of email_uidrec */
    int needrights;       /* Required ACL bits set */
    int use_seendb;       /* Set if this mailbox requires seen.db */
    struct email_mboxrec *mboxrec; /* Mailbox record */
    struct seendata old_seendata;   /* Lock-read seen data from database */
    struct seqset *old_seenseq;     /* Parsed seen sequence before update */
};

void _email_updateplan_free_p(void* p)
{
    struct email_updateplan *plan = p;
    seqset_free(plan->old_seenseq);
    seen_freedata(&plan->old_seendata);
    free(plan->mboxname);
    free(plan->mbox_id);
    ptrarray_t *tmp;
    while ((tmp = ptrarray_pop(&plan->copy))) {
        ptrarray_free((ptrarray_t*)tmp);
    }
    ptrarray_fini(&plan->copy);
    ptrarray_fini(&plan->setflags);
    ptrarray_fini(&plan->delete);
    ptrarray_fini(&plan->snooze);
    free(plan);
}

void _ptrarray_free_p(void *p)
{
    ptrarray_free((ptrarray_t*)p);
}


void _email_bulkupdate_close(struct email_bulkupdate *bulk)
{
    hash_iter *iter = hash_table_iter(&bulk->plans_by_mbox_id);
    while (hash_iter_next(iter)) {
        struct email_updateplan *plan = hash_iter_val(iter);
        jmap_closembox(bulk->req, &plan->mbox);
    }
    seen_close(&bulk->seendb); /* force-close on error */
    hash_iter_free(&iter);
    free_hash_table(&bulk->uidrecs_by_email_id, _ptrarray_free_p);
    free_hash_table(&bulk->updates_by_email_id, NULL);
    free_hash_table(&bulk->plans_by_mbox_id, _email_updateplan_free_p);
    _email_mboxrecs_free(&bulk->cur_mboxrecs);
    _email_mboxrecs_free(&bulk->new_mboxrecs);
    json_decref(bulk->set_errors);
}

static struct email_updateplan *_email_bulkupdate_addplan(struct email_bulkupdate *bulk,
                                                          struct mailbox *mbox,
                                                          struct email_mboxrec *mboxrec)
{
    struct email_updateplan *plan = xzmalloc(sizeof(struct email_updateplan));
    plan->mbox = mbox;
    plan->mbox_id = xstrdup(mbox->uniqueid);
    plan->mboxname = xstrdup(mbox->name);
    plan->mboxrec = mboxrec;
    plan->use_seendb = !mailbox_internal_seen(plan->mbox, bulk->req->userid);
    hash_insert(plan->mbox_id, plan, &bulk->plans_by_mbox_id);
    return plan;
}

static void _email_updateplan_error(struct email_updateplan *plan, int errcode, json_t *set_errors)
{
    json_t *err;
    switch (errcode) {
        case IMAP_PERMISSION_DENIED:
            err = json_pack("{s:s}", "type", "forbidden");
            break;
        case IMAP_QUOTA_EXCEEDED:
            err = json_pack("{s:s}", "type", "overQuota");
            break;
        default:
            err = jmap_server_error(errcode);
    }
    int i;
    for (i = 0; i < ptrarray_size(&plan->copy); i++) {
        struct email_uidrec *uidrec = ptrarray_nth(&plan->copy, i);
        if (json_object_get(set_errors, uidrec->email_id)) {
            continue;
        }
        json_object_set(set_errors, uidrec->email_id, err);
    }
    for (i = 0; i < ptrarray_size(&plan->setflags); i++) {
        struct email_uidrec *uidrec = ptrarray_nth(&plan->setflags, i);
        if (json_object_get(set_errors, uidrec->email_id)) {
            continue;
        }
        json_object_set(set_errors, uidrec->email_id, err);
    }
    for (i = 0; i < ptrarray_size(&plan->delete); i++) {
        struct email_uidrec *uidrec = ptrarray_nth(&plan->delete, i);
        if (json_object_get(set_errors, uidrec->email_id)) {
            continue;
        }
        json_object_set(set_errors, uidrec->email_id, err);
    }
    for (i = 0; i < ptrarray_size(&plan->snooze); i++) {
        struct email_uidrec *uidrec = ptrarray_nth(&plan->snooze, i);
        if (json_object_get(set_errors, uidrec->email_id)) {
            continue;
        }
        json_object_set(set_errors, uidrec->email_id, err);
    }
    json_decref(err);
}

static void _email_bulkupdate_plan_mailboxids(struct email_bulkupdate *bulk, ptrarray_t *updates)
{
    hash_table copyupdates_by_mbox_id = HASH_TABLE_INITIALIZER;
    construct_hash_table(&copyupdates_by_mbox_id, ptrarray_size(updates)+1, 0);

    int i;
    for (i = 0; i < ptrarray_size(updates); i++) {
        struct email_update *update = ptrarray_nth(updates, i);
        const char *email_id = update->email_id;

        if (json_object_get(bulk->set_errors, email_id)) {
            continue;
        }
        ptrarray_t *current_uidrecs = hash_lookup(email_id, &bulk->uidrecs_by_email_id);

        if (update->snooze_in_mboxid) {
            /* Update/delete existing snoozeDetails */

            if (update->snoozed) {
                /* Make a new copy of current snoozed message */
                ptrarray_t *copyupdates =
                    hash_lookup(update->snooze_in_mboxid,
                                &copyupdates_by_mbox_id);
                if (copyupdates == NULL) {
                    copyupdates = ptrarray_new();
                    hash_insert(update->snooze_in_mboxid,
                                copyupdates, &copyupdates_by_mbox_id);
                }
                ptrarray_append(copyupdates, update);
            }

            if (update->snoozed_uidrec &&
                !strcmp(update->snooze_in_mboxid,
                        update->snoozed_uidrec->mboxrec->mbox_id)) {
                /* Delete current snoozed message from mailbox */
                struct email_updateplan *plan =
                    hash_lookup(update->snooze_in_mboxid, &bulk->plans_by_mbox_id);
                ptrarray_append(&plan->delete, update->snoozed_uidrec);
                plan->needrights |= ACL_EXPUNGE|ACL_DELETEMSG;
            }
        }

        if (!update->mailboxids) {
            continue;
        }

        if (update->patch_mailboxids) {
            const char *mbox_id = NULL;
            json_t *jval = NULL;
            json_object_foreach(update->mailboxids, mbox_id, jval) {
                int j;

                /* Lookup the uid record of this email in this mailbox, can be NULL. */
                struct email_uidrec *uidrec = NULL;
                struct email_updateplan *plan = hash_lookup(mbox_id, &bulk->plans_by_mbox_id);
                for (j = 0; j < ptrarray_size(current_uidrecs); j++) {
                    struct email_uidrec *tmp = ptrarray_nth(current_uidrecs, j);
                    if (!strcmp(mbox_id, tmp->mboxrec->mbox_id)) {
                        uidrec = tmp;
                        break;
                    }
                }
                /* Patch the mailbox */
                if (jval == json_true()) {
                    if (uidrec) {
                        /* This email is patched to stay in it's mailbox. Whatever. */
                    }
                    else {
                        /* This is a new mailbox for this email. Copy it over. */
                        ptrarray_t *copyupdates = hash_lookup(mbox_id, &copyupdates_by_mbox_id);
                        if (copyupdates == NULL) {
                            copyupdates = ptrarray_new();
                            hash_insert(mbox_id, copyupdates, &copyupdates_by_mbox_id);
                        }
                        /* XXX  Use ptrarray_add() here to avoid duplicating
                           an update already done above via snooze */
                        ptrarray_add(copyupdates, update);
                    }
                }
                else {
                    if (uidrec) {
                        /* Delete the email from this mailbox. */
                        ptrarray_append(&plan->delete, uidrec);
                        plan->needrights |= ACL_EXPUNGE|ACL_DELETEMSG;
                    }
                }
            }
        }
        else {
            json_t *mailboxids = json_deep_copy(update->mailboxids);
            int j;

            /* For all current uid records of this email, determine if to
             * keep, create or delete them in their respective mailbox. */

            for (j = 0; j < ptrarray_size(current_uidrecs); j++) {
                struct email_uidrec *uidrec = ptrarray_nth(current_uidrecs, j);
                struct email_mboxrec *mboxrec = uidrec->mboxrec;
                struct email_updateplan *plan = hash_lookup(mboxrec->mbox_id, &bulk->plans_by_mbox_id);
                json_t *keep = json_object_get(mailboxids, mboxrec->mbox_id);

                if (keep) {
                    /* Keep message in mailbox */
                    json_object_del(mailboxids, mboxrec->mbox_id);
                }
                else {
                    /* Delete message from mailbox */
                    ptrarray_append(&plan->delete, uidrec);
                    plan->needrights |= ACL_EXPUNGE|ACL_DELETEMSG;
                }
            }

            /* Copy message to any new mailboxes which weren't seen in uidrecs */
            const char *mbox_id;
            json_t *jval;
            json_object_foreach(mailboxids, mbox_id, jval) {
                ptrarray_t *copyupdates = hash_lookup(mbox_id, &copyupdates_by_mbox_id);
                if (copyupdates == NULL) {
                    copyupdates = ptrarray_new();
                    hash_insert(mbox_id, copyupdates, &copyupdates_by_mbox_id);
                }
                ptrarray_append(copyupdates, update);
            }
            json_decref(mailboxids);
        }
    }

    /* Cluster copy operations by mailbox */

    hash_iter *iter = hash_table_iter(&copyupdates_by_mbox_id);
    while (hash_iter_next(iter)) {
        const char *dst_mbox_id = hash_iter_key(iter);
        ptrarray_t *updates = hash_iter_val(iter);

        /* Determine the number of messages per source mailbox which
         * could be copied into the destination mailbox. */

        hash_table src_mbox_id_counts = HASH_TABLE_INITIALIZER;
        construct_hash_table(&src_mbox_id_counts, ptrarray_size(updates)*8+1, 0);
        int i;
        for (i = 0; i < ptrarray_size(updates); i++) {
            struct email_update *update = ptrarray_nth(updates, i);
            ptrarray_t *current_uidrecs = hash_lookup(update->email_id, &bulk->uidrecs_by_email_id);
            int j;
            for (j = 0; j < ptrarray_size(current_uidrecs); j++) {
                struct email_uidrec *uidrec = ptrarray_nth(current_uidrecs, j);
                const char *src_mbox_id = uidrec->mboxrec->mbox_id;
                uintptr_t count = (uintptr_t) hash_lookup(src_mbox_id, &src_mbox_id_counts);
                hash_insert(src_mbox_id, (void*) count++, &src_mbox_id_counts);
            }
        }

        /* For each copy update, pick the uid record from the source mailbox
         * that minimizes append_copy calls between mailboxes. */

        for (i = 0; i < ptrarray_size(updates); i++) {
            struct email_update *update = ptrarray_nth(updates, i);
            ptrarray_t *current_uidrecs = hash_lookup(update->email_id, &bulk->uidrecs_by_email_id);

            int j;
            struct email_uidrec *pick_uidrec = ptrarray_nth(current_uidrecs, 0);
            uintptr_t best_count = (uintptr_t) hash_lookup(pick_uidrec->mboxrec->mbox_id, &src_mbox_id_counts);
            for (j = 1; j < ptrarray_size(current_uidrecs); j++) {
                struct email_uidrec *tmp = ptrarray_nth(current_uidrecs, j);
                uintptr_t count = (uintptr_t) hash_lookup(tmp->mboxrec->mbox_id, &src_mbox_id_counts);
                if (count > best_count) {
                    pick_uidrec = tmp;
                    best_count = count;
                }
            }

            /* Add the picked uid record to its slot in the copy plan */
            struct email_updateplan *plan = hash_lookup(dst_mbox_id, &bulk->plans_by_mbox_id);
            ptrarray_t *pick_uidrecs = NULL;
            for (j = 0; j < ptrarray_size(&plan->copy); j++) {
                ptrarray_t *copy_uidrecs = ptrarray_nth(&plan->copy, j);
                struct email_uidrec *tmp = ptrarray_nth(copy_uidrecs, 0);
                if (!strcmp(tmp->mboxrec->mbox_id, pick_uidrec->mboxrec->mbox_id)) {
                    /* We found an existing slot in the copy plan */
                    pick_uidrecs = copy_uidrecs;
                    break;
                }
            }
            if (pick_uidrecs == NULL) {
                pick_uidrecs = ptrarray_new();
                ptrarray_append(&plan->copy, pick_uidrecs);
            }
            ptrarray_append(pick_uidrecs, pick_uidrec);
            plan->needrights |= ACL_INSERT;
        }
        free_hash_table(&src_mbox_id_counts, NULL);
    }
    hash_iter_free(&iter);

    free_hash_table(&copyupdates_by_mbox_id, _ptrarray_free_p);
}

static void _email_bulkupdate_checklimits(struct email_bulkupdate *bulk)
{
    /* Validate mailbox counts per email */
    hash_table mbox_ids_by_email_id = HASH_TABLE_INITIALIZER;
    construct_hash_table(&mbox_ids_by_email_id, hash_numrecords(&bulk->uidrecs_by_email_id)+1, 0);

    /* Collect current mailboxes per email */
    hash_iter *iter = hash_table_iter(&bulk->uidrecs_by_email_id);
    while (hash_iter_next(iter)) {
        ptrarray_t *uidrecs = hash_iter_val(iter);
        int i;
        for (i = 0; i < ptrarray_size(uidrecs); i++) {
            struct email_uidrec *uidrec = ptrarray_nth(uidrecs, i);
            strarray_t *mbox_ids = hash_lookup(uidrec->email_id, &mbox_ids_by_email_id);
            if (!mbox_ids) {
                mbox_ids = strarray_new();
                hash_insert(uidrec->email_id, mbox_ids, &mbox_ids_by_email_id);
            }
            strarray_add(mbox_ids, uidrec->mboxrec->mbox_id);
        }
    }
    hash_iter_free(&iter);
    /* Apply plans to mailbox counts */
    iter = hash_table_iter(&bulk->plans_by_mbox_id);
    while (hash_iter_next(iter)) {
        struct email_updateplan *plan = hash_iter_val(iter);
        int i;
        for (i = 0; i < ptrarray_size(&plan->copy); i++) {
            ptrarray_t *uidrecs = ptrarray_nth(&plan->copy, i);
            int j;
            for (j = 0; j < ptrarray_size(uidrecs); j++) {
                struct email_uidrec *uidrec = ptrarray_nth(uidrecs, j);
                strarray_t *mbox_ids = hash_lookup(uidrec->email_id, &mbox_ids_by_email_id);
                if (!mbox_ids) {
                    mbox_ids = strarray_new();
                    hash_insert(uidrec->email_id, mbox_ids, &mbox_ids_by_email_id);
                }
                strarray_add(mbox_ids, plan->mbox_id);
            }
        }
        for (i = 0; i < ptrarray_size(&plan->delete); i++) {
            struct email_uidrec *uidrec = ptrarray_nth(&plan->delete, i);
            strarray_t *mbox_ids = hash_lookup(uidrec->email_id, &mbox_ids_by_email_id);
            if (!mbox_ids) {
                mbox_ids = strarray_new();
                hash_insert(uidrec->email_id, mbox_ids, &mbox_ids_by_email_id);
            }
            strarray_remove_all(mbox_ids, plan->mbox_id);
        }
    }
    hash_iter_free(&iter);
    /* Validate mailbox counts */
    iter = hash_table_iter(&mbox_ids_by_email_id);
    while (hash_iter_next(iter)) {
        const char *email_id = hash_iter_key(iter);
        strarray_t *mbox_ids = hash_iter_val(iter);
        if (!mbox_ids) continue;
        if (strarray_size(mbox_ids) > JMAP_MAIL_MAX_MAILBOXES_PER_EMAIL) {
            if (json_object_get(bulk->set_errors, email_id) == NULL) {
                json_object_set_new(bulk->set_errors, email_id,
                        json_pack("{s:s}", "type", "tooManyMailboxes"));
            }
        }
        strarray_free(mbox_ids);
    }
    hash_iter_free(&iter);
    free_hash_table(&mbox_ids_by_email_id, NULL);

    /* Validate keyword counts. This assumes keyword patches already
     * have been replaced with the complete set of patched keywords. */
    iter = hash_table_iter(&bulk->updates_by_email_id);
    while (hash_iter_next(iter)) {
        struct email_update *update = hash_iter_val(iter);
        if (json_object_get(bulk->set_errors, update->email_id)) {
            continue;
        }
        if (json_object_size(update->keywords) > JMAP_MAIL_MAX_KEYWORDS_PER_EMAIL) {
            json_object_set_new(bulk->set_errors, update->email_id,
                    json_pack("{s:s}", "type", "tooManyKeywords"));
        }
    }
    hash_iter_free(&iter);
}

static json_t *_email_bulkupdate_aggregate_keywords(struct email_bulkupdate *bulk,
                                                    const char *email_id,
                                                    hash_table *seenseq_by_mbox_id)
{
    ptrarray_t *current_uidrecs = hash_lookup(email_id, &bulk->uidrecs_by_email_id);
    struct email_keywords keywords = _EMAIL_KEYWORDS_INITIALIZER;
    _email_keywords_init(&keywords, bulk->req->userid, bulk->seendb, seenseq_by_mbox_id);

    int i;
    for (i = 0; i < ptrarray_size(current_uidrecs); i++) {
        struct email_uidrec *uidrec = ptrarray_nth(current_uidrecs, i);
        struct email_updateplan *plan = hash_lookup(uidrec->mboxrec->mbox_id,
                                                    &bulk->plans_by_mbox_id);
        msgrecord_t *mr = NULL;
        int r = msgrecord_find(plan->mbox, uidrec->uid, &mr);
        if (!r) _email_keywords_add_msgrecord(&keywords, mr);
        if (r) {
            if (!json_object_get(bulk->set_errors, uidrec->email_id)) {
                json_object_set_new(bulk->set_errors, uidrec->email_id,
                        jmap_server_error(r));
            }
        }
        msgrecord_unref(&mr);
    }
    json_t *aggregated_keywords = _email_keywords_to_jmap(&keywords);
    _email_keywords_fini(&keywords);
    return aggregated_keywords;
}

static int _flag_update_changes_seen(json_t *new, json_t *old)
{
    int is_seen = json_object_get(new, "$seen") ? 1 : 0;
    int was_seen = (old && json_object_get(old, "$seen")) ? 1 : 0;
    return is_seen != was_seen;
}

static int _flag_update_changes_not_seen(json_t *new, json_t *old)
{
    const char *name;
    json_t *val;

    json_object_foreach(new, name, val) {
        if (!strcmp(name, "$seen")) continue;
        int was_seen = (old && json_object_get(old, name)) ? 1 : 0;
        if (!was_seen) return 1;
    }

    if (old) {
        json_object_foreach(old, name, val) {
            if (!strcmp(name, "$seen")) continue;
            int is_seen = json_object_get(new, name) ? 1 : 0;
            if (!is_seen) return 1;
        }
    }


    return 0;
}

static void _email_bulkupdate_plan_keywords(struct email_bulkupdate *bulk, ptrarray_t *updates)
{
    int i;

    /* Open seen.db, if required */
    if (strcmp(bulk->req->accountid, bulk->req->userid)) {
        int r = seen_open(bulk->req->userid, SEEN_CREATE, &bulk->seendb);
        if (r) {
            /* There's something terribly wrong. Abort all updates. */
            syslog(LOG_ERR, "_email_bulkupdate_plan_keywords: can't open seen.db: %s",
                            error_message(r));
            for (i = 0; i < ptrarray_size(updates); i++) {
                struct email_update *update = ptrarray_nth(updates, i);
                if (json_object_get(bulk->set_errors, update->email_id) == NULL) {
                    json_object_set_new(bulk->set_errors, update->email_id,
                            jmap_server_error(r));
                }
            }
            return;
        }
    }

    /* Add uid records to each mailboxes setflags plan */
    for (i = 0; i < ptrarray_size(updates); i++) {
        struct email_update *update = ptrarray_nth(updates, i);
        const char *email_id = update->email_id;

        if (!update->keywords || json_object_get(bulk->set_errors, email_id)) {
            continue;
        }
        ptrarray_t *current_uidrecs = hash_lookup(email_id, &bulk->uidrecs_by_email_id);

        int j;
        for (j = 0; j < ptrarray_size(current_uidrecs); j++) {
            struct email_uidrec *uidrec = ptrarray_nth(current_uidrecs, j);
            struct email_mboxrec *mboxrec = uidrec->mboxrec;
            struct email_updateplan *plan = hash_lookup(mboxrec->mbox_id, &bulk->plans_by_mbox_id);

            if (!jmap_hasrights_byname(bulk->req, plan->mboxname, ACL_READ|ACL_LOOKUP)) {
                continue;
            }
            if (!update->mailboxids) {
                /* Add keyword update to all current uid records */
                ptrarray_append(&plan->setflags, uidrec);
            }
            else {
                /* Add keyword update to all current records that won't be deleted */
                json_t *jval = json_object_get(update->mailboxids, mboxrec->mbox_id);
                if (jval == json_true() || (jval == NULL && update->patch_mailboxids)) {
                    ptrarray_append(&plan->setflags, uidrec);
                }
            }
        }
    }

    hash_table seenseq_by_mbox_id = HASH_TABLE_INITIALIZER;
    construct_hash_table(&seenseq_by_mbox_id, hash_numrecords(&bulk->plans_by_mbox_id)+1, 0);

    /* Plan keyword updates per mailbox */
    hash_iter *iter = hash_table_iter(&bulk->plans_by_mbox_id);
    while (hash_iter_next(iter)) {
        struct email_updateplan *plan = hash_iter_val(iter);
        if (plan->use_seendb) {
            /* Read seen sequence set */
            int r = seen_lockread(bulk->seendb, plan->mbox->uniqueid, &plan->old_seendata);
            if (!r) {
                plan->old_seenseq = seqset_parse(plan->old_seendata.seenuids, NULL, 0);
                if (!hash_lookup(plan->mbox_id, &seenseq_by_mbox_id))
                    hash_insert(plan->mbox_id, seqset_dup(plan->old_seenseq), &seenseq_by_mbox_id);
            }
            else {
                int j;
                for (j = 0; j < ptrarray_size(&plan->setflags); j++) {
                    struct email_uidrec *uidrec = ptrarray_nth(&plan->setflags, j);
                    if (json_object_get(bulk->set_errors, uidrec->email_id) == NULL) {
                        json_object_set_new(bulk->set_errors, uidrec->email_id,
                                jmap_server_error(r));
                    }
                }
            }
        }

        /* Determine the ACL and keywords for all new uid records */
        for (i = 0; i < ptrarray_size(&plan->copy); i++) {
            ptrarray_t *copy_uidrecs = ptrarray_nth(&plan->copy, i);
            int j;
            for (j = 0; j < ptrarray_size(copy_uidrecs); j++) {
                struct email_uidrec *uidrec = ptrarray_nth(copy_uidrecs, j);
                if (json_object_get(bulk->set_errors, uidrec->email_id)) {
                    continue;
                }
                struct email_update *update = hash_lookup(uidrec->email_id,
                                                          &bulk->updates_by_email_id);

                if (!update->full_keywords) {
                    /* Determine the full set of keywords to write on this record */
                    if (!update->keywords) {
                        /* Write the combined keywords of all records of this email */
                        update->full_keywords = _email_bulkupdate_aggregate_keywords(bulk,
                                                     uidrec->email_id, &seenseq_by_mbox_id);
                    }
                    else if (update->patch_keywords) {
                        /* Write the patched, combined keywords */
                        json_t *aggregated_keywords = _email_bulkupdate_aggregate_keywords(bulk,
                                uidrec->email_id, &seenseq_by_mbox_id);
                        update->full_keywords = jmap_patchobject_apply(aggregated_keywords,
                                update->keywords, NULL);
                        json_decref(aggregated_keywords);
                    }
                    else {
                        /* Write the keywords defined in the update */
                        update->full_keywords = json_incref(update->keywords);
                    }
                }

                /* Determine required ACL rights */
                if (_flag_update_changes_seen(update->full_keywords, NULL))
                    plan->needrights |= ACL_SETSEEN;
                if (_flag_update_changes_not_seen(update->full_keywords, NULL))
                    plan->needrights |= ACL_WRITE;
                /* XXX - what about annotations? */
            }
        }

        /* Determine ACL for all existing uid records with updated keywords */
        for (i = 0; i < ptrarray_size(&plan->setflags); i++) {
            struct email_uidrec *uidrec = ptrarray_nth(&plan->setflags, i);
            if (json_object_get(bulk->set_errors, uidrec->email_id)) {
                continue;
            }
            struct email_update *update = hash_lookup(uidrec->email_id,
                                                      &bulk->updates_by_email_id);

            /* Convert flags to JMAP keywords */
            struct email_keywords keywords = _EMAIL_KEYWORDS_INITIALIZER;
            _email_keywords_init(&keywords, bulk->req->userid, bulk->seendb, &seenseq_by_mbox_id);
            msgrecord_t *mr = NULL;
            int r = msgrecord_find(plan->mbox, uidrec->uid, &mr);
            if (!r) _email_keywords_add_msgrecord(&keywords, mr);
            if (r) {
                json_object_set_new(bulk->set_errors, uidrec->email_id,
                        jmap_server_error(r));
            }
            msgrecord_unref(&mr);
            json_t *current_keywords = _email_keywords_to_jmap(&keywords);
            _email_keywords_fini(&keywords);
            json_t *new_keywords;
            if (update->patch_keywords) {
                new_keywords = jmap_patchobject_apply(current_keywords, update->keywords, NULL);
            }
            else {
                new_keywords = json_incref(update->keywords);
            }

            /* Determine required ACL rights */
            if (_flag_update_changes_seen(new_keywords, current_keywords))
                plan->needrights |= ACL_SETSEEN;
            if (_flag_update_changes_not_seen(new_keywords, current_keywords))
                plan->needrights |= ACL_WRITE;
            /* XXX - what about annotations? */

            json_decref(new_keywords);
            json_decref(current_keywords);
        }
    }
    hash_iter_free(&iter);

    free_hash_table(&seenseq_by_mbox_id, (void(*)(void*))seqset_free);
}

static void _email_bulkupdate_plan_snooze(struct email_bulkupdate *bulk,
                                          ptrarray_t *updates)
{
    char *snoozed_mboxid = NULL, *inboxid = NULL;

    jmap_mailbox_find_role(bulk->req, "snoozed", NULL, &snoozed_mboxid);
    jmap_mailbox_find_role(bulk->req, "inbox", NULL, &inboxid);

    int i;
    for (i = 0; i < ptrarray_size(updates); i++) {
        struct email_update *update = ptrarray_nth(updates, i);
        const char *email_id = update->email_id;
        int invalid = 0, find_mbox = 0;

        if (json_object_get(bulk->set_errors, email_id)) {
            continue;
        }
        ptrarray_t *current_uidrecs =
            hash_lookup(email_id, &bulk->uidrecs_by_email_id);

        /* Lookup the currently snoozed copy of this email_id */
        int j;
        for (j = 0; j < ptrarray_size(current_uidrecs); j++) {
            struct email_uidrec *uidrec = ptrarray_nth(current_uidrecs, j);
            if (uidrec->is_snoozed) {
                update->snoozed_uidrec = uidrec;
                break;
            }
        }

        if (update->snoozed_uidrec) {
            const char *current_mboxid =
                update->snoozed_uidrec->mboxrec->mbox_id;
            struct email_updateplan *plan =
                hash_lookup(current_mboxid, &bulk->plans_by_mbox_id);

            if (update->snoozed) {
                /* Updating/removing snoozeDetails */
                json_t *jval = NULL;

                if (update->patch_snoozed) {
                    const char *current_mboxname =
                        update->snoozed_uidrec->mboxrec->mboxname;
                    json_t *orig =
                        jmap_fetch_snoozed(current_mboxname,
                                           update->snoozed_uidrec->uid);
                    json_t *patch = update->snoozed;
                  
                    update->snoozed = jmap_patchobject_apply(orig, patch, NULL);
                    json_decref(orig);
                    json_decref(patch);
                }

                if (update->mailboxids) {
                    jval = json_object_get(update->mailboxids, current_mboxid);
                }
                if (!update->mailboxids || jval == json_true() ||
                     (jval == NULL && update->patch_mailboxids)) {
                    /* This message is NOT being deleted -
                       Update/remove snoozed */
                    update->snooze_in_mboxid = xstrdup(current_mboxid);
                    update->snoozed_uidrec->is_snoozed = 0;
                    ptrarray_append(&plan->snooze, update->snoozed_uidrec);
                }
                else {
                    /* Determine which mailbox to use for the snoozed email */
                    find_mbox = 1;
                }
            }
            else if (update->mailboxids) {
                /* No change to snoozeDetails -
                   Check if this message is being deleted */
                json_t *jval =
                    json_object_get(update->mailboxids, current_mboxid);

                if (jval == json_null() ||
                    (jval == NULL && !update->patch_mailboxids)) {
                    /* This message is being deleted - Remove snoozed */
                    update->snooze_in_mboxid = xstrdup(current_mboxid);
                    update->snoozed_uidrec->is_snoozed = 0;
                    ptrarray_append(&plan->snooze, update->snoozed_uidrec);
                }
            }
        }
        else if (json_is_object(update->snoozed)) {
            /* Setting snoozeDetails */
            if (!update->mailboxids) {
                /* invalidArguments */
                invalid = 1;
            }
            else {
                /* Determine which mailbox to use for the snoozed email */
                find_mbox = 1;
            }
        }

        if (find_mbox) {
            /* Determine which mailbox to use for the snoozed email */
            const char *movetoid =
                json_string_value(json_object_get(update->snoozed,
                                                  "moveToMailboxId"));

            if (json_is_true(json_object_get(update->mailboxids,
                                             snoozed_mboxid))) {
                /* Being added to \snoozed mailbox */
                update->snooze_in_mboxid = xstrdup(snoozed_mboxid);
            }
            else if (movetoid &&
                     json_is_true(json_object_get(update->mailboxids,
                                                  movetoid))) {
                /* Being added to moveToMailboxId */
                update->snooze_in_mboxid = xstrdup(movetoid);
            }
            else if (json_is_true(json_object_get(update->mailboxids,
                                                  inboxid))) {
                /* Being added to Inbox */
                update->snooze_in_mboxid = xstrdup(inboxid);
            }
            else {
                const char *mbox_id = NULL;
                json_t *jval = NULL;

                json_object_foreach(update->mailboxids, mbox_id, jval) {
                    if (json_is_true(jval)) {
                        /* Use the first mailbox being added to */
                        update->snooze_in_mboxid = xstrdup(mbox_id);
                        break;
                    }
                }

                if (!update->snooze_in_mboxid) {
                    /* invalidArguments */
                    invalid = 1;
                }
            }
        }

        if (invalid) {
            json_object_set_new(bulk->set_errors, email_id,
                                json_pack("{s:s s:[s,s]}",
                                          "type", "invalidArguments",
                                          "arguments", "mailboxIds", "snoozed"));
        }
    }

    free(snoozed_mboxid);
    free(inboxid);
}


static void _email_bulkupdate_plan(struct email_bulkupdate *bulk, ptrarray_t *updates)
{
    int i;

    /* Pre-process snooze updates */
    _email_bulkupdate_plan_snooze(bulk, updates);

    /* Plan mailbox copies, moves and deletes */
    _email_bulkupdate_plan_mailboxids(bulk, updates);

    /* Pre-process keyword updates */
    _email_bulkupdate_plan_keywords(bulk, updates);

    /* Check mailbox count and keyword limits per email */
    _email_bulkupdate_checklimits(bulk);

    /* Validate plans */
    strarray_t erroneous_plans = STRARRAY_INITIALIZER;

    /* Check permissions */
    hash_iter *iter = hash_table_iter(&bulk->plans_by_mbox_id);
    while (hash_iter_next(iter)) {
        struct email_updateplan *plan = hash_iter_val(iter);
        if (!jmap_hasrights_byname(bulk->req, plan->mboxname, plan->needrights)) {
            _email_updateplan_error(plan, IMAP_PERMISSION_DENIED, bulk->set_errors);
            strarray_append(&erroneous_plans, plan->mbox_id);
        }
    }
    hash_iter_reset(iter);
    if (!config_getswitch(IMAPOPT_SINGLEINSTANCESTORE)) {
        /* Check quota */
        while (hash_iter_next(iter)) {
            struct email_updateplan *plan = hash_iter_val(iter);
            quota_t qdiffs[QUOTA_NUMRESOURCES] = QUOTA_DIFFS_INITIALIZER;
            int i;
            for (i = 0; i < ptrarray_size(&plan->copy); i++) {
                qdiffs[QUOTA_MESSAGE] += ptrarray_size(ptrarray_nth(&plan->copy, i));
            }
            qdiffs[QUOTA_MESSAGE] -= ptrarray_size(&plan->delete);
            int r = mailbox_quota_check(plan->mbox, qdiffs);
            if (r) {
                _email_updateplan_error(plan, r, bulk->set_errors);
                strarray_append(&erroneous_plans, plan->mbox_id);
            }
        }
    }
    hash_iter_free(&iter);

    /* Remove erroneous plans */
    for (i = 0; i < strarray_size(&erroneous_plans); i++) {
        const char *mbox_id = strarray_nth(&erroneous_plans, i);
        struct email_updateplan *plan = hash_del(mbox_id, &bulk->plans_by_mbox_id);
        if (!plan) continue;
        jmap_closembox(bulk->req, &plan->mbox);
        _email_updateplan_free_p(plan);
    }
    strarray_fini(&erroneous_plans);

    /* Sort UID records arrays */
    iter = hash_table_iter(&bulk->plans_by_mbox_id);
    while (hash_iter_next(iter)) {
        struct email_updateplan *plan = hash_iter_val(iter);
        for (i = 0; i < ptrarray_size(&plan->copy); i++) {
            ptrarray_t *uidrecs = ptrarray_nth(&plan->copy, i);
            ptrarray_sort(uidrecs, _email_uidrec_compareuid_cb);
        }
        ptrarray_sort(&plan->setflags, _email_uidrec_compareuid_cb);
        ptrarray_sort(&plan->delete, _email_uidrec_compareuid_cb);
        ptrarray_sort(&plan->snooze, _email_uidrec_compareuid_cb);
    }
    hash_iter_free(&iter);
}

static void _email_bulkupdate_open(jmap_req_t *req, struct email_bulkupdate *bulk, ptrarray_t *updates)
{
    int i;
    bulk->req = req;

    /* Map mailbox creation ids and role to mailbox identifiers */
    for(i = 0; i < ptrarray_size(updates); i++) {
        struct email_update *update = ptrarray_nth(updates, i);
        if (!update->mailboxids) continue;

        void *tmp;
        const char *mbox_id;
        json_t *jval;
        json_object_foreach_safe(update->mailboxids, tmp, mbox_id, jval) {
            int is_valid = 1;
            if (*mbox_id == '$') {
                const char *role = mbox_id + 1;
                char *mboxname = NULL;
                char *uniqueid = NULL;
                if (!jmap_mailbox_find_role(bulk->req, role, &mboxname, &uniqueid)) {
                    json_object_del(update->mailboxids, mbox_id);
                    json_object_set(update->mailboxids, uniqueid, jval);
                }
                else is_valid = 0;
                free(uniqueid);
                free(mboxname);
            }
            else if (*mbox_id == '#') {
                const char *resolved_mbox_id = jmap_lookup_id(req, mbox_id + 1);
                if (resolved_mbox_id) {
                    json_object_del(update->mailboxids, mbox_id);
                    json_object_set(update->mailboxids, resolved_mbox_id, jval);
                }
                else is_valid = 0;
            }
            if (!is_valid) {
                if (json_object_get(bulk->set_errors, update->email_id) == NULL) {
                    json_object_set_new(bulk->set_errors, update->email_id,
                            json_pack("{s:s s:[s]}", "type", "invalidProperties",
                                "properties", "mailboxIds"));
                }
            }
        }
    }

    /* Map updates to their email id */
    construct_hash_table(&bulk->updates_by_email_id, ptrarray_size(updates)+1, 0);
    for (i = 0; i < ptrarray_size(updates); i++) {
        struct email_update *update = ptrarray_nth(updates, i);
        hash_insert(update->email_id, update, &bulk->updates_by_email_id);
    }

    /* Determine uid records per mailbox */
    strarray_t email_ids = STRARRAY_INITIALIZER;
    for (i = 0; i < ptrarray_size(updates); i++) {
        struct email_update *update = ptrarray_nth(updates, i);
        if (json_object_get(bulk->set_errors, update->email_id)) {
            continue;
        }
        strarray_append(&email_ids, update->email_id);
    }
    _email_mboxrecs_read(req, req->cstate, &email_ids, bulk->set_errors, &bulk->cur_mboxrecs);

    /* Open current mailboxes */
    size_t mboxhash_size = ptrarray_size(updates) * JMAP_MAIL_MAX_MAILBOXES_PER_EMAIL + 1;
    construct_hash_table(&bulk->plans_by_mbox_id, mboxhash_size, 0);
    construct_hash_table(&bulk->uidrecs_by_email_id, strarray_size(&email_ids)+1, 0);
    for (i = 0; i < ptrarray_size(bulk->cur_mboxrecs); i++) {
        struct email_mboxrec *mboxrec = ptrarray_nth(bulk->cur_mboxrecs, i);
        struct email_updateplan *plan = hash_lookup(mboxrec->mbox_id, &bulk->plans_by_mbox_id);
        if (!plan) {
            struct mailbox *mbox = NULL;
            const mbentry_t *mbentry = jmap_mbentry_by_uniqueid(req, mboxrec->mbox_id);
            int r = 0;
            if (mbentry && mbentry->mbtype & MBTYPE_INTERMEDIATE) {
                r = mboxlist_promote_intermediary(mbentry->name);
            }
            else if (!mbentry) {
                r = IMAP_MAILBOX_NONEXISTENT;
            }
            if (!r) r = jmap_openmbox(req, mboxrec->mboxname, &mbox, /*rw*/1);
            if (r) {
                int j;
                for (j = 0; j < ptrarray_size(&mboxrec->uidrecs); j++) {
                    struct email_uidrec *uidrec = ptrarray_nth(&mboxrec->uidrecs, j);
                    if (json_object_get(bulk->set_errors, uidrec->email_id) == NULL) {
                        json_object_set_new(bulk->set_errors, uidrec->email_id,
                                json_pack("{s:s s:[s]}", "type", "invalidProperties",
                                    "properties", "mailboxIds"));
                    }
                }
                continue;
            }
            plan = _email_bulkupdate_addplan(bulk, mbox, mboxrec);
        }
        /* Map email ids to their list of non-deleted uid records. */
        int j;
        for (j = 0; j < ptrarray_size(&mboxrec->uidrecs); j++) {
            struct email_uidrec *uidrec = ptrarray_nth(&mboxrec->uidrecs, j);
            if (json_object_get(bulk->set_errors, uidrec->email_id)) {
                continue;
            }
            /* Check if the uid record is expunged */
            msgrecord_t *mr = NULL;
            uint32_t system_flags = 0, internal_flags = 0;
            int r = msgrecord_find(plan->mbox, uidrec->uid, &mr);
            if (!r) r = msgrecord_get_systemflags(mr, &system_flags);
            if (!r) r = msgrecord_get_internalflags(mr, &internal_flags);
            if ((system_flags & FLAG_DELETED) || (internal_flags & FLAG_INTERNAL_EXPUNGED)) {
                r = IMAP_NOTFOUND;
            }
            msgrecord_unref(&mr);
            if (r) continue;

            ptrarray_t *current_uidrecs = hash_lookup(uidrec->email_id, &bulk->uidrecs_by_email_id);
            if (current_uidrecs == NULL) {
                current_uidrecs = ptrarray_new();
                hash_insert(uidrec->email_id, current_uidrecs, &bulk->uidrecs_by_email_id);
            }
            ptrarray_append(current_uidrecs, uidrec);
        }
    }
    /* An email with no current uidrecs is expunged */
    for (i = 0; i < strarray_size(&email_ids); i++) {
        const char *email_id = strarray_nth(&email_ids, i);
        if (json_object_get(bulk->set_errors, email_id)) {
            continue;
        }
        if (!hash_lookup(email_id, &bulk->uidrecs_by_email_id)) {
            json_object_set_new(bulk->set_errors, email_id,
                    json_pack("{s:s}", "type", "notFound"));
        }
    }
    strarray_fini(&email_ids);

    /* Open new mailboxes that haven't been opened already */
    for (i = 0; i < ptrarray_size(updates); i++) {
        struct email_update *update = ptrarray_nth(updates, i);
        if (!update->mailboxids) {
            continue;
        }
        json_t *jval;
        const char *mbox_id;
        void *tmp;
        json_object_foreach_safe(update->mailboxids, tmp, mbox_id, jval) {
            struct mailbox *mbox = NULL;
            const mbentry_t *mbentry = jmap_mbentry_by_uniqueid(req, mbox_id);
            if (mbentry) {
                int r = 0;
                if (mbentry->mbtype & MBTYPE_INTERMEDIATE) {
                    r = mboxlist_promote_intermediary(mbentry->name);
                }
                if (!r) jmap_openmbox(req, mbentry->name, &mbox, /*rw*/1);
            }
            if (mbox) {
                if (!hash_lookup(mbox->uniqueid, &bulk->plans_by_mbox_id)) {
                    struct email_mboxrec *mboxrec = xzmalloc(sizeof(struct email_mboxrec));
                    mboxrec->mboxname = xstrdup(mbox->name);
                    mboxrec->mbox_id = xstrdup(mbox->uniqueid);
                    ptrarray_append(bulk->new_mboxrecs, mboxrec);
                    _email_bulkupdate_addplan(bulk, mbox, mboxrec);
                }
                else jmap_closembox(req, &mbox); // already reference counted
            }
            else {
                json_object_set_new(bulk->set_errors, update->email_id,
                        json_pack("{s:s s:[s]}", "type", "invalidProperties",
                            "properties", "mailboxIds"));
            }
        }
    }


    /* Map updates to update plan */
    _email_bulkupdate_plan(bulk, updates);
}

static void _email_bulkupdate_dump(struct email_bulkupdate *bulk, json_t *jdump)
{
    int i;
    struct buf buf = BUF_INITIALIZER;

    json_t *jcur_mboxrecs = json_object();
    for (i = 0; i < ptrarray_size(bulk->cur_mboxrecs); i++) {
        struct email_mboxrec *mboxrec = ptrarray_nth(bulk->cur_mboxrecs, i);
        json_t *jrecs = json_array();
        int j;
        for (j = 0; j < ptrarray_size(&mboxrec->uidrecs); j++) {
            struct email_uidrec *uidrec = ptrarray_nth(&mboxrec->uidrecs, j);
            json_array_append_new(jrecs, json_pack("{s:s s:i}",
                        "emailId", uidrec->email_id, "uid", uidrec->uid));
        }
        json_object_set_new(jcur_mboxrecs, mboxrec->mboxname, jrecs);
    }
    json_object_set_new(jdump, "curMboxrecs", jcur_mboxrecs);

    json_t *jemails = json_object();
    hash_iter *iter = hash_table_iter(&bulk->uidrecs_by_email_id);
    while (hash_iter_next(iter)) {
        const char *email_id = hash_iter_key(iter);
        ptrarray_t *uidrecs = hash_iter_val(iter);
        json_t *juidrecs = json_array();
        int j;
        for (j = 0; j < ptrarray_size(uidrecs); j++) {
            struct email_uidrec *uidrec = ptrarray_nth(uidrecs, j);
            buf_printf(&buf, "%s:%d", uidrec->mboxrec->mboxname, uidrec->uid);
            json_array_append_new(juidrecs, json_string(buf_cstring(&buf)));
            buf_reset(&buf);
        }
        json_object_set_new(jemails, email_id, juidrecs);
    }
    hash_iter_free(&iter);
    json_object_set_new(jdump, "emails", jemails);

    json_t *jmailboxes = json_object();
    json_t *jplans = json_object();
    iter = hash_table_iter(&bulk->plans_by_mbox_id);
    while (hash_iter_next(iter)) {
        struct email_updateplan *plan = hash_iter_val(iter);
        json_object_set_new(jmailboxes, plan->mboxname, json_string(plan->mbox_id));

        json_t *jplan = json_object();

        json_t *jcopy = json_array();
        int j;
        for (j = 0; j < ptrarray_size(&plan->copy); j++) {
            ptrarray_t *uidrecs = ptrarray_nth(&plan->copy, j);
            int k;
            for (k = 0; k < ptrarray_size(uidrecs); k++) {
                struct email_uidrec *uidrec = ptrarray_nth(uidrecs, k);
                buf_printf(&buf, "%s:%d", uidrec->mboxrec->mboxname, uidrec->uid);
                json_array_append_new(jcopy, json_string(buf_cstring(&buf)));
                buf_reset(&buf);
            }
        }
        json_object_set_new(jplan, "copy", jcopy);

        json_t *jsetflags = json_array();
        for (j = 0; j < ptrarray_size(&plan->setflags); j++) {
            struct email_uidrec *uidrec = ptrarray_nth(&plan->setflags, j);
            json_array_append_new(jsetflags, json_integer(uidrec->uid));
        }
        json_object_set_new(jplan, "setflags", jsetflags);

        json_t *jdelete = json_array();
        for (j = 0; j < ptrarray_size(&plan->delete); j++) {
            struct email_uidrec *uidrec = ptrarray_nth(&plan->delete, j);
            json_array_append_new(jdelete, json_integer(uidrec->uid));
        }
        json_object_set_new(jplan, "delete", jdelete);

        json_t *jsnooze = json_array();
        for (j = 0; j < ptrarray_size(&plan->snooze); j++) {
            struct email_uidrec *uidrec = ptrarray_nth(&plan->snooze, j);
            json_array_append_new(jsnooze, json_integer(uidrec->uid));
        }
        json_object_set_new(jplan, "snooze", jsnooze);

        json_object_set_new(jplans, plan->mboxname, jplan);
    }
    hash_iter_free(&iter);
    json_object_set_new(jdump, "plans", jplans);
    json_object_set_new(jdump, "mailboxes", jmailboxes);

    buf_free(&buf);
}

static void _email_bulkupdate_exec_copy(struct email_bulkupdate *bulk)
{
    hash_iter *iter = hash_table_iter(&bulk->plans_by_mbox_id);
    while (hash_iter_next(iter)) {
        struct email_updateplan *plan = hash_iter_val(iter);
        struct mailbox *dst_mbox = plan->mbox;
        int j;
        for (j = 0; j < ptrarray_size(&plan->copy); j++) {
            ptrarray_t *src_uidrecs = ptrarray_nth(&plan->copy, j);
            ptrarray_t src_msgrecs = PTRARRAY_INITIALIZER;

            if (!ptrarray_size(src_uidrecs)) continue;

            /* Lookup the source mailbox plan of the first entry. */
            struct email_uidrec *tmp = ptrarray_nth(src_uidrecs, 0);
            const char *src_mbox_id = tmp->mboxrec->mbox_id;
            struct email_updateplan *src_plan = hash_lookup(src_mbox_id, &bulk->plans_by_mbox_id);
            struct mailbox *src_mbox = src_plan->mbox;
            uint32_t last_uid_before_copy = dst_mbox->i.last_uid;

            /* Bulk copy messages per mailbox to destination */
            int k;
            for (k = 0; k < ptrarray_size(src_uidrecs); k++) {
                struct email_uidrec *src_uidrec = ptrarray_nth(src_uidrecs, k);
                if (json_object_get(bulk->set_errors, src_uidrec->email_id)) {
                    continue;
                }
                msgrecord_t *mrw = msgrecord_from_uid(src_mbox, src_uidrec->uid);
                ptrarray_append(&src_msgrecs, mrw);
            }
            int r = _copy_msgrecords(httpd_authstate, bulk->req->userid, &jmap_namespace,
                                     src_mbox, dst_mbox, &src_msgrecs);
            if (r) {
                for (k = 0; k < ptrarray_size(src_uidrecs); k++) {
                    struct email_uidrec *src_uidrec = ptrarray_nth(src_uidrecs, k);
                    if (json_object_get(bulk->set_errors, src_uidrec->email_id)) {
                        continue;
                    }
                    json_object_set_new(bulk->set_errors, src_uidrec->email_id, jmap_server_error(r));
                }
            }
            for (k = 0; k < ptrarray_size(&src_msgrecs); k++) {
                msgrecord_t *mrw = ptrarray_nth(&src_msgrecs, k);
                msgrecord_unref(&mrw);
            }
            ptrarray_fini(&src_msgrecs);

            for (k = 0; k < ptrarray_size(src_uidrecs); k++) {
                struct email_uidrec *src_uidrec = ptrarray_nth(src_uidrecs, k);
                if (json_object_get(bulk->set_errors, src_uidrec->email_id)) {
                    continue;
                }

                /* Create new uid record */
                struct email_uidrec *new_uidrec = xzmalloc(sizeof(struct email_uidrec));
                new_uidrec->email_id = xstrdup(src_uidrec->email_id);
                new_uidrec->uid = last_uid_before_copy + k + 1;
                new_uidrec->mboxrec = plan->mboxrec;
                new_uidrec->is_new = 1;
                ptrarray_append(&plan->mboxrec->uidrecs, new_uidrec);

                /* Add new record to setflags plan if keywords are updated */
                /* XXX append_copy should take new flags as parameter */
                struct email_update *update = hash_lookup(src_uidrec->email_id,
                                                          &bulk->updates_by_email_id);
                if (update->keywords || update->full_keywords) {
                    ptrarray_append(&plan->setflags, new_uidrec);
                }

                /* Add new record to snooze plan if copied to snooze folder */
                if (update->snoozed) {
                    if (json_is_object(update->snoozed) &&
                        !strcmpnull(plan->mbox_id, update->snooze_in_mboxid)) {
                        /* Only flag as snoozed (add \snoozed and annotation) if:
                           - SnoozeDetails != json_null() AND
                           - this mailbox is the mailbox in which it is snoozed
                        */
                        new_uidrec->is_snoozed = 1;
                    }
                    ptrarray_append(&plan->snooze, new_uidrec);
                }
            }

        }
    }
    hash_iter_free(&iter);
}

static void _email_bulkupdate_exec_setflags(struct email_bulkupdate *bulk)
{
    hash_iter *iter = hash_table_iter(&bulk->plans_by_mbox_id);
    while (hash_iter_next(iter)) {
        struct email_updateplan *plan = hash_iter_val(iter);
        int j;
        struct seqset *add_seenseq = NULL;
        struct seqset *del_seenseq = NULL;
        uint32_t last_uid = plan->mbox->i.last_uid;

        struct mboxevent *flagsset = mboxevent_new(EVENT_FLAGS_SET);
        int notify_flagsset = 0;
        struct mboxevent *flagsclear = mboxevent_new(EVENT_FLAGS_CLEAR);
        int notify_flagsclear = 0;

        if (plan->use_seendb) {
            add_seenseq = seqset_init(0, SEQ_SPARSE);
            del_seenseq = seqset_init(0, SEQ_SPARSE);
        }

        /* Re-sort uid records before processing. */
        ptrarray_sort(&plan->setflags, _email_uidrec_compareuid_cb);

        /* Process uid records */
        for (j = 0; j < ptrarray_size(&plan->setflags); j++) {
            struct email_uidrec *uidrec = ptrarray_nth(&plan->setflags, j);
            if (json_object_get(bulk->set_errors, uidrec->email_id)) {
                continue;
            }
            const char *email_id = uidrec->email_id;
            struct email_update *update = hash_lookup(email_id, &bulk->updates_by_email_id);

            /* Determine if to write the aggregated or updated JMAP keywords */
            json_t *keywords = uidrec->is_new ? update->full_keywords : update->keywords;
            int patch_keywords = uidrec->is_new ? 0 : update->patch_keywords;

            /* Write keywords */
            struct modified_flags modflags;
            memset(&modflags, 0, sizeof(struct modified_flags));
            msgrecord_t *mrw = msgrecord_from_uid(plan->mbox, uidrec->uid);
            int r = _email_setflags(keywords, patch_keywords, mrw,
                                    add_seenseq, del_seenseq, &modflags);
            if (!r) {
                if (modflags.added_flags) {
                    mboxevent_add_flags(flagsset, plan->mbox->flagname,
                                        modflags.added_system_flags,
                                        modflags.added_user_flags);
                    mboxevent_extract_msgrecord(flagsset, mrw);
                    notify_flagsset = 1;
                }
                if (modflags.removed_flags) {
                    mboxevent_add_flags(flagsclear, plan->mbox->flagname,
                                        modflags.removed_system_flags,
                                        modflags.removed_user_flags);
                    mboxevent_extract_msgrecord(flagsclear, mrw);
                    notify_flagsclear = 1;
                }
                msgrecord_unref(&mrw);
                if (last_uid < uidrec->uid) {
                    last_uid = uidrec->uid;
                }
            }
            else {
                json_object_set_new(bulk->set_errors, email_id, jmap_server_error(r));
            }
        }
        /* Write seen db for shared mailboxes */
        if (plan->use_seendb) {
            if (add_seenseq || del_seenseq) {
                struct seqset *new_seenseq = seqset_init(0, SEQ_SPARSE);
                if (del_seenseq->len) {
                    uint32_t uid;
                    while ((uid = seqset_getnext(plan->old_seenseq)))
                        if (!seqset_ismember(del_seenseq, uid))
                            seqset_add(new_seenseq, uid, 1);
                }
                else {
                    seqset_join(new_seenseq, plan->old_seenseq);
                }
                if (add_seenseq->len)
                    seqset_join(new_seenseq, add_seenseq);
                struct seendata sd = SEENDATA_INITIALIZER;
                sd.seenuids = seqset_cstring(new_seenseq);
                if (!sd.seenuids) sd.seenuids = xstrdup("");
                sd.lastread = time(NULL);
                sd.lastchange = plan->mbox->i.last_appenddate;
                sd.lastuid = last_uid;
                int r = seen_write(bulk->seendb, plan->mbox->uniqueid, &sd);
                if (r) {
                    for (j = 0; j < ptrarray_size(&plan->setflags); j++) {
                        struct email_uidrec *uidrec = ptrarray_nth(&plan->setflags, j);
                        if (json_object_get(bulk->set_errors, uidrec->email_id) == NULL) {
                            json_object_set_new(bulk->set_errors, uidrec->email_id,
                                                jmap_server_error(r));
                        }
                    }
                }
                seqset_free(add_seenseq);
                seqset_free(del_seenseq);
                seqset_free(new_seenseq);
                seen_freedata(&sd);
            }
        }
        if (notify_flagsset) {
            mboxevent_extract_mailbox(flagsset, plan->mbox);
            mboxevent_set_numunseen(flagsset, plan->mbox, -1);
            mboxevent_set_access(flagsset, NULL, NULL, bulk->req->userid,
                                 plan->mbox->name, 0);
            mboxevent_notify(&flagsset);
        }
        if (notify_flagsclear) {
            mboxevent_extract_mailbox(flagsclear, plan->mbox);
            mboxevent_set_numunseen(flagsclear, plan->mbox, -1);
            mboxevent_set_access(flagsclear, NULL, NULL, bulk->req->userid,
                                 plan->mbox->name, 0);
            mboxevent_notify(&flagsclear);
        }
        mboxevent_free(&flagsset);
        mboxevent_free(&flagsclear);
    }
    hash_iter_free(&iter);
}

static void _email_bulkupdate_exec_delete(struct email_bulkupdate *bulk)
{
    hash_iter *iter = hash_table_iter(&bulk->plans_by_mbox_id);
    while (hash_iter_next(iter)) {
        struct email_updateplan *plan = hash_iter_val(iter);
        _email_multiexpunge(bulk->req, plan->mbox, &plan->delete, bulk->set_errors);
    }
    hash_iter_free(&iter);
}

static void _email_bulkupdate_exec_snooze(struct email_bulkupdate *bulk)
{
    hash_iter *iter = hash_table_iter(&bulk->plans_by_mbox_id);
    while (hash_iter_next(iter)) {
        struct email_updateplan *plan = hash_iter_val(iter);
        int j;

        /* Process uid records */
        for (j = 0; j < ptrarray_size(&plan->snooze); j++) {
            struct email_uidrec *uidrec = ptrarray_nth(&plan->snooze, j);
            if (json_object_get(bulk->set_errors, uidrec->email_id)) {
                continue;
            }
            const char *email_id = uidrec->email_id;
            struct email_update *update =
                hash_lookup(email_id, &bulk->updates_by_email_id);

            /* Write annotation */
            msgrecord_t *mrw = msgrecord_from_uid(plan->mbox, uidrec->uid);
            uint32_t internalflags = 0;
            int r = IMAP_MAILBOX_NONEXISTENT;
            time_t savedate = 0;

            if (mrw) r = msgrecord_get_internalflags(mrw, &internalflags);
            if (!r) r = msgrecord_get_savedate(mrw, &savedate);

            if (!r) {
                const char *annot = IMAP_ANNOT_NS "snoozed";
                struct buf val = BUF_INITIALIZER;

                if (uidrec->is_snoozed) {
                    /* Set/update annotation */
                    char *json = json_dumps(update->snoozed, JSON_COMPACT);

                    buf_initm(&val, json, strlen(json));

                    /* Extract until and use it as savedate */
                    time_from_iso8601(json_string_value(json_object_get(update->snoozed, "until")),
                                      &savedate);

                    internalflags |= FLAG_INTERNAL_SNOOZED;
                }
                else {
                    /* Delete annotation */
                    internalflags &= ~FLAG_INTERNAL_SNOOZED;
                }

                r = msgrecord_annot_write(mrw, annot, "", &val);
                if (!r) r = msgrecord_set_internalflags(mrw, internalflags);
                if (!r) r = msgrecord_set_savedate(mrw, savedate);
                if (!r) r = msgrecord_rewrite(mrw);
                msgrecord_unref(&mrw);
                buf_free(&val);
            }

            if (r) {
                json_object_set_new(bulk->set_errors, email_id, jmap_server_error(r));
            }
        }
    }
    hash_iter_free(&iter);
}


static void _email_bulkupdate_exec(struct email_bulkupdate *bulk,
                                   json_t *updated,
                                   json_t *not_updated,
                                   json_t *debug)
{
    /*  Execute plans */
    _email_bulkupdate_exec_copy(bulk);
    _email_bulkupdate_exec_setflags(bulk);
    _email_bulkupdate_exec_snooze(bulk);
    _email_bulkupdate_exec_delete(bulk);

    /* Report results */
    hash_iter *iter = hash_table_iter(&bulk->updates_by_email_id);
    while (hash_iter_next(iter)) {
        const char *email_id = hash_iter_key(iter);
        json_t *err = json_object_get(bulk->set_errors, email_id);
        if (err) {
            json_object_set(not_updated, email_id, err);
        }
        else {
            json_object_set_new(updated, email_id, json_pack("{s:s}", "id", email_id));
        }
    }
    hash_iter_free(&iter);

    if (debug) _email_bulkupdate_dump(bulk, debug);
}


/* Execute the sequence of JMAP Email/set/{update} arguments in bulk. */
static void _email_update_bulk(jmap_req_t *req,
                               json_t *update,
                               json_t *updated,
                               json_t *not_updated,
                               json_t *debug)
{
    struct email_bulkupdate bulkupdate = _EMAIL_BULKUPDATE_INITIALIZER;
    ptrarray_t updates = PTRARRAY_INITIALIZER;
    int i;

    /* Bulk updates are implemented to minimize the number of Cyrus mailbox
     * open and close calls. This is achieved by the following steps:
     *
     * (1) Parse all update arguments.
     *
     * (2) For each updated email, determine its mailboxes and UID records.
     *     Keep note of all mailboxes where the email is currently contained
     *     in, deleted from or copied to. Open all these mailboxes and create
     *     an empty update plan for each mailbox.
     *
     * (3) For each email, determine the required update operations per
     *     mailbox:
     *
     *     (3.1) If the email keywords are updated, add the email's
     *           UID record to the `setflags` field of the according
     *           mailbox update plan.
     *
     *     (3.2) If the email mailboxes are updated, determine which
     *           mailboxes to delete the UID record from, and where
     *           to copy an existing UID record to. For each mailbox,
     *           determine all the UID records that are copied into it.
     *           Cluster all these UID records such that the number
     *           of mailboxes to copy from is minimized.
     *
     * (4) Check required permissions and quota for each mailbox. For
     *     any failed mailbox check, mark all updates of emails that
     *     affect this mailbox as failed.
     *
     * (5) Check email-scoped limits such as mailbox count and keywords per
     *     email. Mark all erroneous emails as not updated.
     *
     * (6) Execute the update plans. Apply copy first, followed by setflags,
     *     followed by deletes.
     *
     * (7) Report all updated emails and close all mailboxes.
     *
     */

    /* Parse updates and add valid updates to todo list. */
    const char *email_id;
    json_t *jval;
    bulkupdate.req = req;
    json_object_foreach(update, email_id, jval) {
        struct jmap_parser parser = JMAP_PARSER_INITIALIZER;
        struct email_update *update = xzmalloc(sizeof(struct email_update));
        update->email_id = email_id;
        _email_update_parse(jval, &parser, update);
        if (json_array_size(parser.invalid)) {
            json_object_set_new(not_updated, email_id,
                    json_pack("{s:s s:O}", "type", "invalidProperties",
                        "properties", parser.invalid));
            _email_update_free(update);
        }
        else {
            ptrarray_append(&updates, update);
        }
        jmap_parser_fini(&parser);
    }
    if (ptrarray_size(&updates)) {
        /* Build and execute bulk update */
        _email_bulkupdate_open(req, &bulkupdate, &updates);
        _email_bulkupdate_exec(&bulkupdate, updated, not_updated, debug);
        _email_bulkupdate_close(&bulkupdate);
    }
    else {
        /* just clean up the memory we allocated above */
        _email_mboxrecs_free(&bulkupdate.new_mboxrecs);
        json_decref(bulkupdate.set_errors);
    }

    for (i = 0; i < ptrarray_size(&updates); i++) {
        _email_update_free(ptrarray_nth(&updates, i));
    }
    ptrarray_fini(&updates);
}

static void _email_destroy_bulk(jmap_req_t *req,
                                json_t *destroy,
                                json_t *destroyed,
                                json_t *not_destroyed)
{
    ptrarray_t *mboxrecs = NULL;
    strarray_t email_ids = STRARRAY_INITIALIZER;
    size_t iz;
    json_t *jval;
    int i;

    /* Map email ids to mailbox name and UID */
    json_array_foreach(destroy, iz, jval) {
        strarray_append(&email_ids, json_string_value(jval));
    }
    _email_mboxrecs_read(req, req->cstate, &email_ids, not_destroyed, &mboxrecs);

    /* Check mailbox ACL for shared accounts. */
    if (strcmp(req->accountid, req->userid)) {
        for (i = 0; i < ptrarray_size(mboxrecs); i++) {
            struct email_mboxrec *mboxrec = ptrarray_nth(mboxrecs, i);
            if (!jmap_hasrights_byname(req, mboxrec->mboxname, ACL_DELETEMSG)) {
                /* Mark all messages of this mailbox as failed */
                int j;
                for (j = 0; j < ptrarray_size(&mboxrec->uidrecs); j++) {
                    struct email_uidrec *uidrec = ptrarray_nth(&mboxrec->uidrecs, j);
                    if (!json_object_get(not_destroyed, uidrec->email_id)) {
                        json_object_set_new(not_destroyed, uidrec->email_id,
                                            json_pack("{s:s}", "type", "forbidden"));
                    }
                }
                /* Remove this mailbox from the todo list */
                ptrarray_remove(mboxrecs, i--);
                _email_mboxrec_free(mboxrec);
            }
        }
    }

    /* Expunge messages in bulk per mailbox */
    for (i = 0; i < ptrarray_size(mboxrecs); i++) {
        struct email_mboxrec *mboxrec = ptrarray_nth(mboxrecs, i);
        struct mailbox *mbox = NULL;
        int j;
        int r = jmap_openmbox(req, mboxrec->mboxname, &mbox, 1);
        if (!r) {
            /* Expunge messages one by one, marking any failed message */
            _email_multiexpunge(req, mbox, &mboxrec->uidrecs, not_destroyed);
        }
        else {
            /* Mark all messages of this mailbox as failed */
            for (j = 0; j < ptrarray_size(&mboxrec->uidrecs); j++) {
                struct email_uidrec *uidrec = ptrarray_nth(&mboxrec->uidrecs, j);
                if (!json_object_get(not_destroyed, uidrec->email_id)) {
                    json_object_set_new(not_destroyed, uidrec->email_id,
                            jmap_server_error(r));
                }
            }
        }
        jmap_closembox(req, &mbox);
    }

    /* Report successful destroys */
    json_array_foreach(destroy, iz, jval) {
        const char *email_id = json_string_value(jval);
        if (!json_object_get(not_destroyed, email_id))
            json_array_append(destroyed, jval);
    }

    _email_mboxrecs_free(&mboxrecs);
    strarray_fini(&email_ids);
}

static int jmap_email_set(jmap_req_t *req)
{
    struct jmap_parser parser = JMAP_PARSER_INITIALIZER;
    struct jmap_set set;

    json_t *err = NULL;
    jmap_set_parse(req, &parser, email_props, NULL, NULL, &set, &err);
    if (err) {
        jmap_error(req, err);
        goto done;
    }

    if (set.if_in_state) {
        /* TODO rewrite state function to use char* not json_t* */
        json_t *jstate = json_string(set.if_in_state);
        if (jmap_cmpstate(req, jstate, MBTYPE_EMAIL)) {
            jmap_error(req, json_pack("{s:s}", "type", "stateMismatch"));
            goto done;
        }
        json_decref(jstate);
        set.old_state = xstrdup(set.if_in_state);
    }
    else {
        json_t *jstate = jmap_getstate(req, MBTYPE_EMAIL, /*refresh*/0);
        set.old_state = xstrdup(json_string_value(jstate));
        json_decref(jstate);
    }

    json_t *email;
    const char *creation_id;
    json_object_foreach(set.create, creation_id, email) {
        json_t *set_err = NULL;
        json_t *new_email = NULL;
        /* Create message */
        _email_create(req, email, &new_email, &set_err);
        if (set_err) {
            json_object_set_new(set.not_created, creation_id, set_err);
            continue;
        }
        /* Report message as created */
        json_object_set_new(set.created, creation_id, new_email);
        const char *msg_id = json_string_value(json_object_get(new_email, "id"));
        jmap_add_id(req, creation_id, msg_id);
    }

    json_t *debug_bulkupdate = NULL;
    if (jmap_is_using(req, JMAP_DEBUG_EXTENSION)) {
        debug_bulkupdate = json_object();
    }
    _email_update_bulk(req, set.update, set.updated, set.not_updated, debug_bulkupdate);

    _email_destroy_bulk(req, set.destroy, set.destroyed, set.not_destroyed);

    // TODO refactor jmap_getstate to return a string, once
    // all code has been migrated to the new JMAP parser.
    json_t *jstate = jmap_getstate(req, MBTYPE_EMAIL, /*refresh*/1);
    set.new_state = xstrdup(json_string_value(jstate));
    json_decref(jstate);

    json_t *reply = jmap_set_reply(&set);
    if (jmap_is_using(req, JMAP_DEBUG_EXTENSION)) {
        json_object_set_new(reply, "debugBulkUpdate", debug_bulkupdate); // takes ownership
    }
    jmap_ok(req, reply);

done:
    jmap_parser_fini(&parser);
    jmap_set_fini(&set);
    return 0;
}

struct _email_import_rock {
    struct buf buf;
};

static int _email_import_cb(jmap_req_t *req __attribute__((unused)),
                            FILE *out,
                            void *rock,
                            json_t **err __attribute__((unused)))
{
    struct _email_import_rock *data = (struct _email_import_rock*) rock;
    const char *base = data->buf.s;
    size_t len = data->buf.len;
    struct protstream *stream = prot_readmap(base, len);
    int r = message_copy_strict(stream, out, len, 0);
    prot_free(stream);
    return r;
}

struct msgimport_checkacl_rock {
    jmap_req_t *req;
    json_t *mailboxes;
};

static int msgimport_checkacl_cb(const mbentry_t *mbentry, void *xrock)
{
    struct msgimport_checkacl_rock *rock = xrock;
    jmap_req_t *req = rock->req;

    if (!json_object_get(rock->mailboxes, mbentry->uniqueid))
        return 0;

    int needrights = ACL_INSERT|ACL_ANNOTATEMSG;
    if (!jmap_hasrights(req, mbentry, needrights))
        return IMAP_PERMISSION_DENIED;

    return 0;
}

static void _email_import(jmap_req_t *req,
                          json_t *jemail_import,
                          json_t **new_email,
                          json_t **err)
{
    const char *blob_id = json_string_value(json_object_get(jemail_import, "blobId"));
    json_t *jmailbox_ids = json_object_get(jemail_import, "mailboxIds");
    char *mboxname = NULL;
    struct _email_import_rock content = { BUF_INITIALIZER };
    int has_attachment = 0;

    /* Force write locks on mailboxes. */
    req->force_openmbox_rw = 1;

    /* Gather keywords */
    strarray_t keywords = STRARRAY_INITIALIZER;
    const json_t *val;
    const char *keyword;
    json_object_foreach(json_object_get(jemail_import, "keywords"), keyword, val) {
        if (!strcasecmp(keyword, JMAP_HAS_ATTACHMENT_FLAG)) {
            continue;
        }
        strarray_append(&keywords, keyword);
    }

    /* check for internaldate */
    time_t internaldate = 0;
    const char *received_at = json_string_value(json_object_get(jemail_import, "receivedAt"));
    if (received_at) {
        time_from_iso8601(received_at, &internaldate);
    }

    /* check for snoozed */
    json_t *snoozed = json_object_get(jemail_import, "snoozed");

    /* Check mailboxes for ACL */
    if (strcmp(req->userid, req->accountid)) {
        struct msgimport_checkacl_rock rock = { req, jmailbox_ids };
        int r = mboxlist_usermboxtree(req->accountid, req->authstate, msgimport_checkacl_cb, &rock, MBOXTREE_INTERMEDIATES);
        if (r) {
            *err = json_pack("{s:s s:[s]}", "type", "invalidProperties",
                    "properties", "mailboxIds");
            goto done;
        }
    }

    /* Start import */
    struct email_append_detail detail;
    memset(&detail, 0, sizeof(struct email_append_detail));

    /* Lookup blob */
    struct mailbox *mbox = NULL;
    msgrecord_t *mr = NULL;

    /* see if we can get a direct email! */
    const char *sourcefile = NULL;
    int r = jmap_findblob_exact(req, NULL/*accountid*/, blob_id,
                                &mbox, &mr);
    if (!r) r = msgrecord_get_fname(mr, &sourcefile);
    if (!r) r = msgrecord_get_body(mr, &content.buf);
    if (!r) goto gotrecord;

    /* better clean up before we go the slow path */
    buf_reset(&content.buf);
    sourcefile = NULL;
    msgrecord_unref(&mr);
    jmap_closembox(req, &mbox);

    struct body *body = NULL;
    const struct body *part = NULL;
    struct buf msg_buf = BUF_INITIALIZER;
    r = jmap_findblob(req, NULL/*accountid*/, blob_id,
                      &mbox, &mr, &body, &part, &msg_buf);
    if (r) {
        if (r == IMAP_NOTFOUND || r == IMAP_PERMISSION_DENIED)
            *err = json_pack("{s:s s:[s]}", "type", "invalidProperties",
                    "properties", "blobId");
        else
            *err = jmap_server_error(r);
        goto done;
    }

    /* Decode blob */
    const char *blob_base = buf_base(&msg_buf);
    size_t blob_len = buf_len(&msg_buf);
    if (part) {
        blob_base += part->content_offset;
        blob_len = part->content_size;

        int enc = encoding_lookupname(part->encoding);
        if (enc != ENCODING_NONE) {
            char *tmp;
            size_t dec_len;
            const char *dec = charset_decode_mimebody(blob_base, blob_len, enc, &tmp, &dec_len);
            buf_setmap(&content.buf, dec, dec_len);
            free(tmp);
        }
        else {
            buf_setmap(&content.buf, blob_base, blob_len);
        }
    }
    else {
        buf_setmap(&content.buf, blob_base, blob_len);
    }

    message_free_body(body);
    free(body);

gotrecord:

    /* Determine $hasAttachment flag */
    if (config_getswitch(IMAPOPT_JMAP_SET_HAS_ATTACHMENT)) {
        /* Parse email */
        json_t *email = NULL;
        struct email_getargs getargs = _EMAIL_GET_ARGS_INITIALIZER;
        getargs.props = xzmalloc(sizeof(hash_table));
        construct_hash_table(getargs.props, 1, 0);

        hash_insert("hasAttachment", (void*)1, getargs.props);
        _email_from_buf(req, &getargs, &content.buf, NULL, &email);
        has_attachment = json_boolean_value(json_object_get(email, "hasAttachment"));

        free_hash_table(getargs.props, NULL);
        free(getargs.props);
        getargs.props = NULL;
        _email_getargs_fini(&getargs);
        json_decref(email);
    }

    /* Write the message to the file system */
    _email_append(req, jmailbox_ids, &keywords, internaldate, snoozed,
                  has_attachment, sourcefile, _email_import_cb, &content, &detail, err);

    msgrecord_unref(&mr);
    jmap_closembox(req, &mbox);

    if (*err) goto done;

    *new_email = json_pack("{s:s, s:s, s:s, s:i}",
         "id", detail.email_id,
         "blobId", detail.blob_id,
         "threadId", detail.thread_id,
         "size", detail.size);

done:
    strarray_fini(&keywords);
    buf_free(&content.buf);
    free(mboxname);
}

static int jmap_email_import(jmap_req_t *req)
{
    json_t *created = json_pack("{}");
    json_t *not_created = json_pack("{}");

    struct jmap_parser parser = JMAP_PARSER_INITIALIZER;
    const char *key;
    json_t *arg, *emails = NULL;
    const char *id;
    json_t *jemail_import;
    int have_snoozed_mboxid = 0;

    /* Parse request */
    json_object_foreach(req->args, key, arg) {
        if (!strcmp(key, "accountId")) {
            /* already handled in jmap_api() */
        }

        else if (!strcmp(key, "emails")) {
            if (json_is_object(arg)) {
                emails = arg;
                jmap_parser_push(&parser, "emails");
                json_object_foreach(emails, id, jemail_import) {
                    if (!json_is_object(jemail_import)) {
                        jmap_parser_invalid(&parser, id);
                    }
                }
                jmap_parser_pop(&parser);
            }
        }

        else {
            jmap_parser_invalid(&parser, key);
        }
    }

    /* emails is a required argument */
    if (!emails) jmap_parser_invalid(&parser, "emails");

    if (json_array_size(parser.invalid)) {
        json_t *err = json_pack("{s:s s:O}", "type", "invalidArguments",
                                "arguments", parser.invalid);
        jmap_error(req, err);
        goto done;
    }

    json_object_foreach(emails, id, jemail_import) {
        /* Parse import */
        struct jmap_parser parser = JMAP_PARSER_INITIALIZER;
        json_t *jval;
        const char *s;

        /* blobId */
        s = json_string_value(json_object_get(jemail_import, "blobId"));
        if (!s) {
            jmap_parser_invalid(&parser, "blobId");
        }

        /* keywords */
        json_t *keywords = json_object_get(jemail_import, "keywords");
        if (json_is_object(keywords)) {
            jmap_parser_push(&parser, "keywords");
            json_object_foreach(keywords, s, jval) {
                if (jval != json_true() || !jmap_email_keyword_is_valid(s)) {
                    jmap_parser_invalid(&parser, s);
                }
            }
            jmap_parser_pop(&parser);
        }
        else if (JNOTNULL(keywords)) {
            jmap_parser_invalid(&parser, "keywords");
        }

        /* receivedAt */
        json_t *jrecv = json_object_get(jemail_import, "receivedAt");
        if (JNOTNULL(jrecv)) {
            if (!json_is_utcdate(jrecv)) {
                jmap_parser_invalid(&parser, "receivedAt");
            }
        }

        /* Validate mailboxIds */
        json_t *jmailboxids = json_copy(json_object_get(jemail_import, "mailboxIds"));
        if (json_object_size(jmailboxids)) {
            _append_validate_mboxids(req, jmailboxids, &parser, &have_snoozed_mboxid);
        }
        else {
            jmap_parser_invalid(&parser, "mailboxIds");
        }

        /* Validate snoozed + mailboxIds */
        json_t *snoozed = json_object_get(jemail_import, "snoozed");
        if (JNOTNULL(snoozed) &&
            !(json_is_utcdate(json_object_get(snoozed, "until")) &&
              have_snoozed_mboxid)) {
            jmap_parser_invalid(&parser, "snoozed");
        }

        json_t *invalid = json_incref(parser.invalid);
        jmap_parser_fini(&parser);
        if (json_array_size(invalid)) {
            json_t *err = json_pack("{s:s}", "type", "invalidProperties");
            json_object_set_new(err, "properties", invalid);
            json_object_set_new(not_created, id, err);
            json_decref(jmailboxids);
            continue;
        }
        json_decref(invalid);

        /* Process import */
        json_t *orig_mailboxids = json_incref(json_object_get(jemail_import, "mailboxIds"));
        json_object_set_new(jemail_import, "mailboxIds", jmailboxids);
        json_t *new_email = NULL;
        json_t *err = NULL;
        _email_import(req, jemail_import, &new_email, &err);
        if (err) {
            json_object_set_new(not_created, id, err);
        }
        else {
            /* Successful import */
            json_object_set_new(created, id, new_email);
            const char *newid = json_string_value(json_object_get(new_email, "id"));
            jmap_add_id(req, id, newid);
        }
        json_object_set_new(jemail_import, "mailboxIds", orig_mailboxids);
    }

    /* Reply */
    jmap_ok(req, json_pack("{s:s s:O s:O}",
                "accountId", req->accountid,
                "created", created,
                "notCreated", not_created));

done:
    json_decref(created);
    json_decref(not_created);
    jmap_parser_fini(&parser);
    return 0;
}

struct _email_copy_checkmbox_rock {
    jmap_req_t *req;           /* JMAP request context */
    json_t *dst_mboxids;       /* mailboxIds argument */
    strarray_t *dst_mboxnames; /* array of destination mailbox names */
};

static int _email_copy_checkmbox_cb(const mbentry_t *mbentry, void *_rock)
{
    struct _email_copy_checkmbox_rock *rock = _rock;

    /* Ignore anything but regular and intermediate mailboxes */
    if (!mbentry || (mbentry->mbtype & ~MBTYPE_INTERMEDIATE)) {
        return 0;
    }
    if (!json_object_get(rock->dst_mboxids, mbentry->uniqueid)) {
        return 0;
    }

    /* Check read-write ACL rights */
    int needrights = ACL_LOOKUP|ACL_READ|ACL_WRITE|ACL_INSERT|
                     ACL_SETSEEN|ACL_ANNOTATEMSG;
    if (!jmap_hasrights(rock->req, mbentry, needrights))
        return IMAP_PERMISSION_DENIED;

    /* Mark this mailbox as found */
    strarray_append(rock->dst_mboxnames, mbentry->name);
    size_t want_count = json_object_size(rock->dst_mboxids);
    size_t have_count = strarray_size(rock->dst_mboxnames);
    return want_count == have_count ? IMAP_OK_COMPLETED : 0;
}

struct _email_copy_writeprops_rock {
    /* Input values */
    jmap_req_t *req;
    const char *received_at;
    json_t *keywords;
    struct seen *seendb;
    /* Return values */
    conversation_id_t cid; /* Thread id of copied message */
    uint32_t size;         /* Byte size of copied message */
};

static int _email_copy_writeprops_cb(const conv_guidrec_t* rec, void* _rock)
{
    struct _email_copy_writeprops_rock *rock = _rock;
    struct mailbox *mbox = NULL;
    msgrecord_t *mr = NULL;
    jmap_req_t *req = rock->req;
    struct mboxevent *flagsset = mboxevent_new(EVENT_FLAGS_SET);
    struct mboxevent *flagsclear = mboxevent_new(EVENT_FLAGS_CLEAR);
    int notify_flagsset = 0;
    int notify_flagsclear = 0;

    if (rec->part) {
        return 0;
    }

    /* Overwrite message record */
    int r = jmap_openmbox(rock->req, rec->mboxname, &mbox, /*rw*/1);
    if (!r) r = msgrecord_find(mbox, rec->uid, &mr);
    if (!r && rock->received_at) {
        time_t internal_date;
        time_from_iso8601(rock->received_at, &internal_date);
        r = msgrecord_set_internaldate(mr, internal_date);
    }
    if (!r) {
        /* Write the keywords. There's lots of ceremony around seen.db */
        struct seqset *seenseq = NULL;
        struct seqset *addseen = NULL;
        struct seqset *delseen = NULL;

        /* Read the current seen sequence from seen.db */
        int need_seendb = !mailbox_internal_seen(mbox, req->userid);
        if (need_seendb) {
            delseen = seqset_init(0, SEQ_SPARSE);
            addseen = seqset_init(0, SEQ_SPARSE);
            struct seendata sd = SEENDATA_INITIALIZER;
            int r = seen_lockread(rock->seendb, mbox->uniqueid, &sd);
            if (!r) {
                seenseq = seqset_parse(sd.seenuids, NULL, sd.lastuid);
                seen_freedata(&sd);
            }
        }

        /* Write the flags on the record */
        struct modified_flags modflags;
        memset(&modflags, 0, sizeof(struct modified_flags));
        if (!r) r = _email_setflags(rock->keywords, 0, mr, addseen, delseen, &modflags);
        if (!r) {
            if (modflags.added_flags) {
                mboxevent_extract_msgrecord(flagsset, mr);
                mboxevent_add_flags(flagsset, mbox->flagname,
                                    modflags.added_system_flags,
                                    modflags.added_user_flags);
                notify_flagsset = 1;
            }
            if (modflags.removed_flags) {
                mboxevent_extract_msgrecord(flagsclear, mr);
                mboxevent_add_flags(flagsclear, mbox->flagname,
                                    modflags.removed_system_flags,
                                    modflags.removed_user_flags);
                notify_flagsclear = 1;
            }
        }

        /* Write back changes to seen.db */
        if (!r && need_seendb && (addseen->len || delseen->len)) {
            if (delseen->len) {
                struct seqset *newseen = seqset_init(0, SEQ_SPARSE);
                uint32_t uid;
                while ((uid = seqset_getnext(seenseq))) {
                    if (!seqset_ismember(delseen, uid)) {
                        seqset_add(newseen, uid, 1);
                    }
                }
                seqset_free(seenseq);
                seenseq = newseen;
            }
            else if (addseen->len) {
                seqset_add(seenseq, rec->uid, 1);
            }

            struct seendata sd = SEENDATA_INITIALIZER;
            sd.seenuids = seqset_cstring(seenseq);
            if (!sd.seenuids) sd.seenuids = xstrdup("");
            sd.lastread = time(NULL);
            sd.lastchange = mbox->i.last_appenddate;
            sd.lastuid = mbox->i.last_uid;
            r = seen_write(rock->seendb, mbox->uniqueid, &sd);
            seen_freedata(&sd);
        }

        seqset_free(delseen);
        seqset_free(addseen);
        seqset_free(seenseq);
    }
    if (!r) r = msgrecord_rewrite(mr);
    if (r) goto done;

    /* Write mboxevents */
    if (notify_flagsset) {
        mboxevent_extract_mailbox(flagsset, mbox);
        mboxevent_set_numunseen(flagsset, mbox, -1);
        mboxevent_set_access(flagsset, NULL, NULL, req->userid, mbox->name, 0);
        mboxevent_notify(&flagsset);
    }
    if (notify_flagsclear) {
        mboxevent_extract_mailbox(flagsclear, mbox);
        mboxevent_set_numunseen(flagsclear, mbox, -1);
        mboxevent_set_access(flagsclear, NULL, NULL, req->userid, mbox->name, 0);
        mboxevent_notify(&flagsclear);
    }

    /* Read output values */
    if (!rock->cid) rock->cid = rec->cid;
    if (!rock->size) r = msgrecord_get_size(mr, &rock->size);

done:
    mboxevent_free(&flagsset);
    mboxevent_free(&flagsclear);
    if (mr) msgrecord_unref(&mr);
    jmap_closembox(rock->req, &mbox);
    return r;
}

struct _email_exists_rock {
    jmap_req_t *req;
    int exists;
};

static int _email_exists_cb(const conv_guidrec_t *rec, void *rock)
{
    struct _email_exists_rock *data = (struct _email_exists_rock*) rock;
    jmap_req_t *req = data->req;
    struct mailbox *mbox = NULL;
    msgrecord_t *mr = NULL;
    uint32_t internal_flags;
    int r = 0;

    r = jmap_openmbox(req, rec->mboxname, &mbox, 0);
    if (r) return r;

    r = msgrecord_find(mbox, rec->uid, &mr);
    if (r) goto done;

    r = msgrecord_get_internalflags(mr, &internal_flags);
    if (r) goto done;

    if (!(internal_flags & FLAG_INTERNAL_EXPUNGED)) {
        data->exists = 1;
        r = CYRUSDB_DONE;
    }

done:
    if (mr) msgrecord_unref(&mr);
    jmap_closembox(req, &mbox);
    return r;
}

struct emailcopy_pickrecord_rock {
    jmap_req_t *req;
    struct mailbox *mbox;
    msgrecord_t *mr;
    struct email_keywords keywords;
    int gather_keywords;
};

static int _email_copy_pickrecord_cb(const conv_guidrec_t *rec, void *vrock)
{
    struct emailcopy_pickrecord_rock *rock = vrock;
    jmap_req_t *req = rock->req;

    /* Make sure we are allowed to read this mailbox */
    if (!jmap_hasrights_byname(req, rec->mboxname, ACL_READ)) return 0;

    struct mailbox *mbox = NULL;
    msgrecord_t *mr = NULL;

    /* Check if this record is expunged */
    int r = jmap_openmbox(req, rec->mboxname, &mbox, 0);
    if (r) goto done;

    r = msgrecord_find(mbox, rec->uid, &mr);
    if (r) goto done;
    uint32_t system_flags;
    uint32_t internal_flags;
    r = msgrecord_get_systemflags(mr, &system_flags);
    if (!r) msgrecord_get_internalflags(mr, &internal_flags);
    if (system_flags & FLAG_DELETED || internal_flags & FLAG_INTERNAL_EXPUNGED) {
        msgrecord_unref(&mr);
        jmap_closembox(req, &mbox);
        goto done;
    }

    /* Aggregate message record flags into JMAP keywords */
    if (rock->gather_keywords) {
        _email_keywords_add_msgrecord(&rock->keywords, mr);
    }

    /* Keep this message record as source to copy from? */
    if (!rock->mbox) {
        rock->mbox = mbox;
        rock->mr = mr;
    }
    else {
        msgrecord_unref(&mr);
        jmap_closembox(req, &mbox);
    }

done:
    return r;
}

static void _email_copy(jmap_req_t *req, json_t *copy_email,
                        const char *from_account_id,
                        struct seen *seendb,
                        json_t **new_email, json_t **err)
{
    strarray_t dst_mboxnames = STRARRAY_INITIALIZER;
    struct mailbox *src_mbox = NULL;
    msgrecord_t *src_mr = NULL;
    char *src_mboxname = NULL;
    int r = 0;
    char *blob_id = NULL;
    json_t *new_keywords = NULL;
    json_t *jmailboxids = json_copy(json_object_get(copy_email, "mailboxIds"));

    /* Support mailboxids by role */
    const char *mbox_id;
    json_t *jval;
    void *tmp;
    json_object_foreach_safe(jmailboxids, tmp, mbox_id, jval) {
        if (*mbox_id != '$') continue;
        const char *role = mbox_id + 1;
        char *uniqueid = NULL;
        if (jmap_mailbox_find_role(req, role, NULL, &uniqueid) == 0) {
            json_object_del(jmailboxids, mbox_id);
            json_object_set_new(jmailboxids, uniqueid, jval);
        }
        else {
            *err = json_pack("{s:s s:[s]}", "type", "invalidProperties",
                    "properties", "mailboxIds");
        }
        free(uniqueid);
        if (*err) goto done;
    }

    const char *email_id = json_string_value(json_object_get(copy_email, "id"));
    uint32_t src_size = 0;

    /* Lookup source message record and gather JMAP keywords */
    new_keywords = json_deep_copy(json_object_get(copy_email, "keywords"));
    if (json_is_null(new_keywords)) {
        new_keywords = NULL;
    }
    hash_table seenseq_by_mbox_id = HASH_TABLE_INITIALIZER;
    construct_hash_table(&seenseq_by_mbox_id, 32, 0);
    struct emailcopy_pickrecord_rock pickrecord_rock = {
        req, NULL, NULL, _EMAIL_KEYWORDS_INITIALIZER, 0
    };
    if (!new_keywords) {
        _email_keywords_init(&pickrecord_rock.keywords, req->userid, seendb, &seenseq_by_mbox_id);
        pickrecord_rock.gather_keywords = 1;
    }
    if (strcmp(from_account_id, req->accountid)) {
        struct conversations_state *mycstate = NULL;
        r = conversations_open_user(from_account_id, 0/*shared*/, &mycstate);
        if (!r) r = conversations_guid_foreach(mycstate, _guid_from_id(email_id),
                _email_copy_pickrecord_cb, &pickrecord_rock);
        if (!r) r = conversations_commit(&mycstate);
    }
    else {
        r = conversations_guid_foreach(req->cstate, _guid_from_id(email_id),
                _email_copy_pickrecord_cb, &pickrecord_rock);
    }
    if (!r && pickrecord_rock.mbox) {
        src_mbox = pickrecord_rock.mbox;
        src_mr = pickrecord_rock.mr;
        if (!new_keywords) {
            new_keywords = _email_keywords_to_jmap(&pickrecord_rock.keywords);
        }
        r = msgrecord_get_size(src_mr, &src_size);
    }
    else if (!r) {
        r = IMAP_NOTFOUND;
    }
    free_hash_table(&seenseq_by_mbox_id, (void (*)(void *)) seqset_free);
    _email_keywords_fini(&pickrecord_rock.keywords);
    if (r) {
        if (r == IMAP_NOTFOUND || r == IMAP_PERMISSION_DENIED) {
            *err = json_pack("{s:s s:[s]}", "type", "invalidProperties",
                    "properties", "id");
            r = 0;
        }
        goto done;
    }

    /* Override hasAttachment flag */
    int has_attachment = 0;
    r = msgrecord_hasflag(src_mr, JMAP_HAS_ATTACHMENT_FLAG, &has_attachment);
    if (r) goto done;
    if (has_attachment)
        json_object_set_new(new_keywords, JMAP_HAS_ATTACHMENT_FLAG, json_true());
    else
        json_object_del(new_keywords, JMAP_HAS_ATTACHMENT_FLAG);


    struct message_guid guid;
    r = msgrecord_get_guid(src_mr, &guid);
    if (r) goto done;
    blob_id = xstrdup(message_guid_encode(&guid));

    /* Check if email already exists in to_account */
    struct _email_exists_rock data = { req, 0 };
    conversations_guid_foreach(req->cstate, blob_id, _email_exists_cb, &data);
    if (data.exists) {
        *err = json_pack("{s:s s:s}", "type", "alreadyExists", "existingId", email_id);
        goto done;
    }

    /* Lookup mailbox names and make sure they are all writeable */
    struct _email_copy_checkmbox_rock checkmbox_rock = {
        req, jmailboxids, &dst_mboxnames
    };
    r = mboxlist_usermboxtree(req->accountid, httpd_authstate,
                              _email_copy_checkmbox_cb, &checkmbox_rock,
                              MBOXTREE_INTERMEDIATES);
    if (r != IMAP_OK_COMPLETED) {
        if (r == 0 || r == IMAP_PERMISSION_DENIED) {
            *err = json_pack("{s:s s:[s]}", "type", "invalidProperties",
                    "properties", "mailboxIds");
            r = 0;
        }
        goto done;
    }

    /* Copy message record to mailboxes */
    char *dst_mboxname;
    while ((dst_mboxname = strarray_pop(&dst_mboxnames))) {
        mbentry_t *mbentry = NULL;
        r = jmap_mboxlist_lookup(dst_mboxname, &mbentry, NULL);
        if (!r && (mbentry->mbtype & MBTYPE_INTERMEDIATE)) {
            r = mboxlist_promote_intermediary(dst_mboxname);
        }
        if (!r) {
            struct mailbox *dst_mbox = NULL;
            r = jmap_openmbox(req, dst_mboxname, &dst_mbox, /*rw*/1);
            if (!r) {
                r = _copy_msgrecord(httpd_authstate, req->accountid,
                        &jmap_namespace, src_mbox, dst_mbox, src_mr);
            }
            jmap_closembox(req, &dst_mbox);
            free(dst_mboxname);
        }
        mboxlist_entry_free(&mbentry);
        if (r) goto done;
    }

    /* Rewrite new message record properties and lookup thread id */
    const char *receivedAt = json_string_value(json_object_get(copy_email, "receivedAt"));
    struct _email_copy_writeprops_rock writeprops_rock = {
        req, receivedAt, new_keywords, seendb, /*cid*/0, /*size*/0
    };
    r = conversations_guid_foreach(req->cstate, email_id + 1,
                                   _email_copy_writeprops_cb, &writeprops_rock);

    if (!r) {
        char thread_id[JMAP_THREADID_SIZE];
        jmap_set_threadid(writeprops_rock.cid, thread_id);
        *new_email = json_pack("{s:s s:s s:s s:i}",
                "id", email_id,
                "blobId", blob_id,
                "threadId", thread_id,
                "size", src_size);
    }

done:
    json_decref(jmailboxids);
    if (r && *err == NULL) {
        *err = jmap_server_error(r);
    }
    free(src_mboxname);
    free(blob_id);
    strarray_fini(&dst_mboxnames);
    if (src_mr) msgrecord_unref(&src_mr);
    jmap_closembox(req, &src_mbox);
    json_decref(new_keywords);
}

static void _email_copy_validate_props(json_t *jemail, json_t **err)
{
    struct jmap_parser myparser = JMAP_PARSER_INITIALIZER;

    /* Validate properties */
    json_t *prop, *id = NULL, *mailboxids = NULL;;
    const char *pname;
    json_object_foreach(jemail, pname, prop) {
        if (!strcmp(pname, "id")) {
            if (!json_is_string(prop)) {
                jmap_parser_invalid(&myparser, "id");
            }
            id = prop;
        }
        else if (!strcmp(pname, "mailboxIds")) {
            jmap_parser_push(&myparser, "mailboxIds");
            const char *mbox_id;
            json_t *jbool;
            json_object_foreach(prop, mbox_id, jbool) {
                if (!strlen(mbox_id) || jbool != json_true()) {
                    jmap_parser_invalid(&myparser, NULL);
                    break;
                }
            }
            jmap_parser_pop(&myparser);
            mailboxids = prop;
        }
        else if (!strcmp(pname, "keywords")) {
            if (json_is_object(prop)) {
                jmap_parser_push(&myparser, "keywords");
                const char *keyword;
                json_t *jbool;
                json_object_foreach(prop, keyword, jbool) {
                    if (!jmap_email_keyword_is_valid(keyword) ||
                        jbool != json_true()) {
                        jmap_parser_invalid(&myparser, keyword);
                    }
                }
                jmap_parser_pop(&myparser);
            }
            else {
                jmap_parser_invalid(&myparser, "keywords");
            }
        }
        else if (!strcmp(pname, "receivedAt")) {
            if (!json_is_utcdate(prop)) {
                jmap_parser_invalid(&myparser, "receivedAt");
            }
        }
        else {
            jmap_parser_invalid(&myparser, pname);
        }
    }
    /* Check mandatory properties */
    if (!id) {
        jmap_parser_invalid(&myparser, "id");
    }
    if (!mailboxids) {
        jmap_parser_invalid(&myparser, "mailboxIds");
    }
    /* Reject invalid properties... */
    if (json_array_size(myparser.invalid)) {
        *err = json_pack("{s:s s:O}",
                         "type", "invalidProperties",
                         "properties", myparser.invalid);
    }

    jmap_parser_fini(&myparser);
}

static int jmap_email_copy(jmap_req_t *req)
{
    struct jmap_parser parser = JMAP_PARSER_INITIALIZER;
    struct jmap_copy copy;
    json_t *err = NULL;
    struct seen *seendb = NULL;
    json_t *destroy_emails = json_array();

    /* Parse request */
    jmap_copy_parse(req, &parser, NULL, NULL, &copy, &err);
    if (err) {
        jmap_error(req, err);
        goto done;
    }

    int r = seen_open(req->userid, SEEN_CREATE, &seendb);
    if (r) {
        syslog(LOG_ERR, "jmap_email_copy: can't open seen.db: %s",
                        error_message(r));
        jmap_error(req, jmap_server_error(r));
        goto done;
    }

    /* Process request */
    const char *creation_id;
    json_t *copy_email;
    json_object_foreach(copy.create, creation_id, copy_email) {
        json_t *set_err = NULL;
        json_t *new_email = NULL;

        /* Validate create */
        _email_copy_validate_props(copy_email, &set_err);
        if (set_err) {
            json_object_set_new(copy.not_created, creation_id, set_err);
            continue;
        }

        /* Copy message */
        _email_copy(req, copy_email, copy.from_account_id,
                    seendb, &new_email, &set_err);
        if (set_err) {
            json_object_set_new(copy.not_created, creation_id, set_err);
            continue;
        }

        /* Note the source ID for deletion */
        json_array_append(destroy_emails, json_object_get(copy_email, "id"));

        /* Report the message as created */
        json_object_set_new(copy.created, creation_id, new_email);
        const char *msg_id = json_string_value(json_object_get(new_email, "id"));
        jmap_add_id(req, creation_id, msg_id);
    }

    /* Build response */
    jmap_ok(req, jmap_copy_reply(&copy));

    /* Destroy originals, if requested */
    if (copy.on_success_destroy_original && json_array_size(destroy_emails)) {
        json_t *subargs = json_object();
        json_object_set(subargs, "destroy", destroy_emails);
        json_object_set_new(subargs, "accountId", json_string(copy.from_account_id));
        jmap_add_subreq(req, "Email/set", subargs, NULL);
    }

done:
    json_decref(destroy_emails);
    jmap_parser_fini(&parser);
    jmap_copy_fini(&copy);
    seen_close(&seendb);
    return 0;
}

/* Identity/get method */
static const jmap_property_t identity_props[] = {
    {
        "id",
        NULL,
        JMAP_PROP_SERVER_SET | JMAP_PROP_IMMUTABLE | JMAP_PROP_ALWAYS_GET
    },
    {
        "name",
        NULL,
        0
    },
    {
        "email",
        NULL,
        JMAP_PROP_IMMUTABLE
    },
    {
        "replyTo",
        NULL,
        0
    },
    {
        "bcc",
        NULL,
        0
    },
    {
        "textSignature",
        NULL,
        0
    },
    {
        "htmlSignature",
        NULL,
        0
    },
    {
        "mayDelete",
        NULL,
        JMAP_PROP_SERVER_SET
    },

    /* FM extensions (do ALL of these get through to Cyrus?) */
    {
        "displayName",
        JMAP_MAIL_EXTENSION,
        0
    },
    {
        "addBccOnSMTP",
        JMAP_MAIL_EXTENSION,
        0
    },
    {
        "saveSentToMailboxId",
        JMAP_MAIL_EXTENSION,
        0
    },
    {
        "saveOnSMTP",
        JMAP_MAIL_EXTENSION,
        0
    },
    {
        "useForAutoReply",
        JMAP_MAIL_EXTENSION,
        0
    },
    {
        "isAutoConfigured",
        JMAP_MAIL_EXTENSION,
        0
    },
    {
        "enableExternalSMTP",
        JMAP_MAIL_EXTENSION,
        0
    },
    {
        "smtpServer",
        JMAP_MAIL_EXTENSION,
        0
    },
    {
        "smtpPort",
        JMAP_MAIL_EXTENSION,
        0
    },
    {
        "smtpSSL",
        JMAP_MAIL_EXTENSION,
        0
    },
    {
        "smtpUser",
        JMAP_MAIL_EXTENSION,
        0
    },
    {
        "smtpPassword",
        JMAP_MAIL_EXTENSION,
        0
    },
    {
        "smtpRemoteService",
        JMAP_MAIL_EXTENSION,
        0
    },
    {
        "popLinkId",
        JMAP_MAIL_EXTENSION,
        0
    },

    { NULL, NULL, 0 }
};

static int jmap_identity_get(jmap_req_t *req)
{
    struct jmap_parser parser = JMAP_PARSER_INITIALIZER;
    struct jmap_get get;
    json_t *err = NULL;

    /* Parse request */
    jmap_get_parse(req, &parser, identity_props, /*allow_null_ids*/1,
                   NULL, NULL, &get, &err);
    if (err) {
        jmap_error(req, err);
        goto done;
    }

    /* Build response */
    json_t *me = json_pack("{s:s}", "id", req->userid);
    if (jmap_wantprop(get.props, "name")) {
        json_object_set_new(me, "name", json_string(""));
    }
    if (jmap_wantprop(get.props, "email")) {
        json_object_set_new(me, "email",
                json_string(strchr(req->userid, '@') ? req->userid : ""));
    }

    if (jmap_wantprop(get.props, "mayDelete")) {
        json_object_set_new(me, "mayDelete", json_false());
    }
    if (json_array_size(get.ids)) {
        size_t i;
        json_t *val;
        json_array_foreach(get.ids, i, val) {
            if (strcmp(json_string_value(val), req->userid)) {
                json_array_append(get.not_found, val);
            }
            else {
                json_array_append(get.list, me);
            }
        }
    } else if (!JNOTNULL(get.ids)) {
        json_array_append(get.list, me);
    }
    json_decref(me);

    /* Reply */
    get.state = xstrdup("0");
    jmap_ok(req, jmap_get_reply(&get));

done:
    jmap_parser_fini(&parser);
    jmap_get_fini(&get);
    return 0;
}

static int jmap_email_matchmime_method(jmap_req_t *req)
{
    json_t *jfilter = json_object_get(req->args, "filter");
    json_t *jmime = json_object_get(req->args, "mime");
    json_t *err = NULL;

    struct buf mime = BUF_INITIALIZER;
    buf_setcstr(&mime, json_string_value(jmime));
    int matches = jmap_email_matchmime(&mime, jfilter, req->accountid, time(NULL), &err);
    buf_free(&mime);
    if (!err) {
        json_t *res = json_pack("{s:O s:b}", "filter", jfilter, "matches", matches);
        jmap_ok(req, res);
    }
    else {
        jmap_error(req, err);
    }

    return 0;
}
