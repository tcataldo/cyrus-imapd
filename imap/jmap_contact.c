/* jmap_contact.c -- Routines for handling JMAP contact messages
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
#include <errno.h>

#include "annotate.h"
#include "carddav_db.h"
#include "global.h"
#include "hash.h"
#include "http_carddav.h"
#include "http_dav.h"
#include "http_jmap.h"
#include "json_support.h"
#include "mailbox.h"
#include "mboxname.h"
#include "stristr.h"
#include "times.h"
#include "util.h"
#include "vcard_support.h"
#include "xmalloc.h"

/* generated headers are not necessarily in current directory */
#include "imap/http_err.h"
#include "imap/imap_err.h"

static int jmap_contactgroup_get(struct jmap_req *req);
static int jmap_contactgroup_changes(struct jmap_req *req);
static int jmap_contactgroup_set(struct jmap_req *req);
static int jmap_contact_get(struct jmap_req *req);
static int jmap_contact_changes(struct jmap_req *req);
static int jmap_contact_query(struct jmap_req *req);
static int jmap_contact_set(struct jmap_req *req);
static int jmap_contact_copy(struct jmap_req *req);

static int _contact_set_create(jmap_req_t *req, unsigned kind,
                               json_t *jcard, struct carddav_data *cdata,
                               struct mailbox **mailbox, char **uidptr,
                               json_t *invalid);
static int required_set_rights(json_t *props);
static int _json_to_card(struct jmap_req *req,
                         struct carddav_data *cdata,
                         struct vparse_card *card,
                         json_t *arg, strarray_t *flags,
                         struct entryattlist **annotsp,
                         json_t *invalid);

static json_t *jmap_contact_from_vcard(struct vparse_card *card,
                                       struct mailbox *mailbox,
                                       struct index_record *record);

static int jmap_contact_getblob(jmap_req_t *req, const char *blobid,
                                const char *accept);

#define JMAPCACHE_CONTACTVERSION 1

jmap_method_t jmap_contact_methods_standard[] = {
    { NULL, NULL, NULL, 0}
};

jmap_method_t jmap_contact_methods_nonstandard[] = {
    {
        "ContactGroup/get",
        JMAP_CONTACTS_EXTENSION,
        &jmap_contactgroup_get,
        JMAP_SHARED_CSTATE
    },
    {
        "ContactGroup/changes",
        JMAP_CONTACTS_EXTENSION,
        &jmap_contactgroup_changes,
        JMAP_SHARED_CSTATE
    },
    {
        "ContactGroup/set",
        JMAP_CONTACTS_EXTENSION,
        &jmap_contactgroup_set,
        /*flags*/0
    },
    {
        "Contact/get",
        JMAP_CONTACTS_EXTENSION,
        &jmap_contact_get,
        JMAP_SHARED_CSTATE
    },
    {
        "Contact/changes",
        JMAP_CONTACTS_EXTENSION,
        &jmap_contact_changes,
        JMAP_SHARED_CSTATE
    },
    {
        "Contact/query",
        JMAP_CONTACTS_EXTENSION,
        &jmap_contact_query,
        JMAP_SHARED_CSTATE
    },
    {
        "Contact/set",
        JMAP_CONTACTS_EXTENSION,
        &jmap_contact_set,
        /*flags*/0
    },
    {
        "Contact/copy",
        JMAP_CONTACTS_EXTENSION,
        &jmap_contact_copy,
        /*flags*/0
    },
    { NULL, NULL, NULL, 0}
};

static char *_prodid = NULL;

HIDDEN void jmap_contact_init(jmap_settings_t *settings)
{
    jmap_method_t *mp;
    for (mp = jmap_contact_methods_standard; mp->name; mp++) {
        hash_insert(mp->name, mp, &settings->methods);
    }

    if (config_getswitch(IMAPOPT_JMAP_NONSTANDARD_EXTENSIONS)) {
        json_object_set_new(settings->server_capabilities,
                JMAP_CONTACTS_EXTENSION, json_object());

        for (mp = jmap_contact_methods_nonstandard; mp->name; mp++) {
            hash_insert(mp->name, mp, &settings->methods);
        }
    }

    ptrarray_append(&settings->getblob_handlers, jmap_contact_getblob);

    /* Initialize PRODID value
     *
     * XXX - OS X 10.11.6 Contacts is not unfolding PRODID lines, so make
     * sure that PRODID never exceeds the 75 octet limit without CRLF */
    struct buf prodidbuf = BUF_INITIALIZER;
    size_t max_len = 68; /* 75 - strlen("PRODID:") */
    buf_printf(&prodidbuf, "-//CyrusIMAP.org//Cyrus %s//EN", CYRUS_VERSION);
    if (buf_len(&prodidbuf) > max_len) {
        buf_truncate(&prodidbuf, max_len - 6);
        buf_appendcstr(&prodidbuf, "..//EN");
    }
    _prodid = buf_release(&prodidbuf);
}

HIDDEN void jmap_contact_capabilities(json_t *account_capabilities)
{
    if (config_getswitch(IMAPOPT_JMAP_NONSTANDARD_EXTENSIONS)) {
        json_object_set_new(account_capabilities, JMAP_CONTACTS_EXTENSION, json_object());
    }
}

struct changes_rock {
    jmap_req_t *req;
    struct jmap_changes *changes;
    size_t seen_records;
    modseq_t highestmodseq;
};

static void strip_spurious_deletes(struct changes_rock *urock)
{
    /* if something is mentioned in both DELETEs and UPDATEs, it's probably
     * a move.  O(N*M) algorithm, but there are rarely many, and the alternative
     * of a hash will cost more */
    unsigned i, j;

    for (i = 0; i < json_array_size(urock->changes->destroyed); i++) {
        const char *del =
            json_string_value(json_array_get(urock->changes->destroyed, i));

        for (j = 0; j < json_array_size(urock->changes->updated); j++) {
            const char *up =
                json_string_value(json_array_get(urock->changes->updated, j));
            if (!strcmpsafe(del, up)) {
                json_array_remove(urock->changes->destroyed, i--);
                break;
            }
        }
    }
}

static int _match_text(const char *haystack, const char *needle) {
    /* XXX This is just a very crude text matcher. */
    return stristr(haystack, needle) != NULL;
}

/* Return true if text matches the value of arg's property named name. If 
 * name is NULL, match text to any JSON string property of arg or those of
 * its enclosed JSON objects and arrays. */
static int jmap_match_jsonprop(json_t *arg, const char *name, const char *text)
{
    if (name) {
        json_t *val = json_object_get(arg, name);
        if (json_typeof(val) != JSON_STRING) {
            return 0;
        }
        return _match_text(json_string_value(val), text);
    } else {
        const char *key;
        json_t *val;
        int m = 0;
        size_t i;
        json_t *entry;

        json_object_foreach(arg, key, val) {
            switch json_typeof(val) {
                case JSON_STRING:
                    m = _match_text(json_string_value(val), text);
                    break;
                case JSON_OBJECT:
                    m = jmap_match_jsonprop(val, NULL, text);
                    break;
                case JSON_ARRAY:
                    json_array_foreach(val, i, entry) {
                        switch json_typeof(entry) {
                            case JSON_STRING:
                                m = _match_text(json_string_value(entry), text);
                                break;
                            case JSON_OBJECT:
                                m = jmap_match_jsonprop(entry, NULL, text);
                                break;
                            default:
                                /* do nothing */
                                ;
                        }
                        if (m) break;
                    }
                default:
                    /* do nothing */
                    ;
            }
            if (m) return m;
        }
    }
    return 0;
}

/* FIXME DUPLICATE END */

/*****************************************************************************
 * JMAP Contacts API
 ****************************************************************************/

struct cards_rock {
    struct carddav_db *db;
    struct jmap_req *req;
    struct jmap_get *get;
    struct mailbox *mailbox;
    hashu64_table jmapcache;
    int rows;
};

static json_t *jmap_group_from_vcard(struct vparse_card *vcard)
{
    struct vparse_entry *ventry = NULL;
    json_t *obj = json_pack("{}");

    json_t *contactids = json_pack("[]");
    json_t *otherids = json_pack("{}");

    for (ventry = vcard->properties; ventry; ventry = ventry->next) {
        const char *name = ventry->name;
        const char *propval = ventry->v.value;

        if (!name) continue;
        if (!propval) continue;

        if (!strcasecmp(name, "fn")) {
            json_object_set_new(obj, "name", json_string(propval));
        }

        else if (!strcasecmp(name, "x-addressbookserver-member")) {
            if (strncmp(propval, "urn:uuid:", 9)) continue;
            json_array_append_new(contactids, json_string(propval+9));
        }

        else if (!strcasecmp(name, "x-fm-otheraccount-member")) {
            if (strncmp(propval, "urn:uuid:", 9)) continue;
            struct vparse_param *param = vparse_get_param(ventry, "userid");
            if (!param) continue;
            json_t *object = json_object_get(otherids, param->value);
            if (!object) {
                object = json_array();
                json_object_set_new(otherids, param->value, object);
            }
            json_array_append_new(object, json_string(propval+9));
        }
    }

    json_object_set_new(obj, "contactIds", contactids);
    json_object_set_new(obj, "otherAccountContactIds", otherids);

    return obj;
}

static int getgroups_cb(void *rock, struct carddav_data *cdata)
{
    struct cards_rock *crock = (struct cards_rock *) rock;
    struct index_record record;
    jmap_req_t *req = crock->req;
    json_t *obj = NULL;
    char *xhref;
    int r;

    if (!jmap_hasrights_byname(req, cdata->dav.mailbox, DACL_READ))
        return 0;

    if (cdata->jmapversion == JMAPCACHE_CONTACTVERSION) {
        json_error_t jerr;
        obj = json_loads(cdata->jmapdata, 0, &jerr);
        if (obj) goto gotvalue;
    }

    if (!crock->mailbox || strcmp(crock->mailbox->name, cdata->dav.mailbox)) {
        mailbox_close(&crock->mailbox);
        r = mailbox_open_irl(cdata->dav.mailbox, &crock->mailbox);
        if (r) return r;
    }

    r = mailbox_find_index_record(crock->mailbox, cdata->dav.imap_uid, &record);
    if (r) return r;

    /* Load message containing the resource and parse vcard data */
    struct vparse_card *vcard = record_to_vcard(crock->mailbox, &record);
    if (!vcard || !vcard->objects) {
        syslog(LOG_ERR, "record_to_vcard failed for record %u:%s",
                cdata->dav.imap_uid, crock->mailbox->name);
        vparse_free_card(vcard);
        return IMAP_INTERNAL;
    }

    obj = jmap_group_from_vcard(vcard->objects);

    vparse_free_card(vcard);

    hashu64_insert(cdata->dav.rowid, json_dumps(obj, 0), &crock->jmapcache);

gotvalue:

    json_object_set_new(obj, "id", json_string(cdata->vcard_uid));
    json_object_set_new(obj, "uid", json_string(cdata->vcard_uid));

    json_object_set_new(obj, "addressbookId",
                        json_string(strrchr(cdata->dav.mailbox, '.')+1));

    xhref = jmap_xhref(cdata->dav.mailbox, cdata->dav.resource);
    json_object_set_new(obj, "x-href", json_string(xhref));
    free(xhref);

    json_array_append_new(crock->get->list, obj);

    crock->rows++;

    return 0;
}

static const jmap_property_t contact_props[] = {
    {
        "id",
        NULL,
        JMAP_PROP_SERVER_SET | JMAP_PROP_IMMUTABLE | JMAP_PROP_ALWAYS_GET
    },
    {
        "uid",
        NULL,
        JMAP_PROP_IMMUTABLE
    },
    {
        "isFlagged",
        NULL,
        0
    },
    {
        "avatar",
        NULL,
        0
    },
    {
        "prefix",
        NULL,
        0
    },
    {
        "firstName",
        NULL,
        0
    },
    {
        "lastName",
        NULL,
        0
    },
    {
        "suffix",
        NULL,
        0
    },
    {
        "nickname",
        NULL,
        0
    },
    {
        "birthday",
        NULL,
        0
    },
    {
        "anniversary",
        NULL,
        0
    },
    {
        "company",
        NULL,
        0
    },
    {
        "department",
        NULL,
        0
    },
    {
        "jobTitle",
        NULL,
        0
    },
    {
        "emails",
        NULL,
        0
    },
    {
        "phones",
        NULL,
        0
    },
    {
        "online",
        NULL,
        0
    },
    {
        "addresses",
        NULL,
        0
    },
    {
        "notes",
        NULL,
        0
    },

    /* FM extensions */
    {
        "addressbookId",
        JMAP_CONTACTS_EXTENSION,
        0
    },
    {
        "x-href",
        JMAP_CONTACTS_EXTENSION,
        JMAP_PROP_SERVER_SET | JMAP_PROP_IMMUTABLE
    }, // AJAXUI only
    {
        "x-hasPhoto",
        JMAP_CONTACTS_EXTENSION,
        JMAP_PROP_SERVER_SET
    }, // AJAXUI only
    {
        "importance",
        JMAP_CONTACTS_EXTENSION,
        0
    },  // JMAPUI only
    {
        "blobId",
        JMAP_CONTACTS_EXTENSION,
        JMAP_PROP_SERVER_SET | JMAP_PROP_SKIP_GET
    },

    { NULL, NULL, 0 }
};

static const jmap_property_t group_props[] = {
    {
        "id",
        NULL,
        JMAP_PROP_SERVER_SET | JMAP_PROP_IMMUTABLE | JMAP_PROP_ALWAYS_GET
    },
    {
        "uid",
        NULL,
        JMAP_PROP_IMMUTABLE
    },
    {
        "name",
        NULL,
        0
    },
    {
        "contactIds",
        NULL,
        0
    },

    // FM extensions */
    {
        "addressbookId",
        JMAP_CONTACTS_EXTENSION,
        0
    },
    {
        "x-href",
        JMAP_CONTACTS_EXTENSION,
        JMAP_PROP_SERVER_SET | JMAP_PROP_IMMUTABLE
    }, // AJAXUI only
    {
        "otherAccountContactIds",
        JMAP_CONTACTS_EXTENSION,
        0
    }, // Both AJAXUI and JMAPUI

    { NULL, NULL, 0 }
};

static int _contact_getargs_parse(jmap_req_t *req __attribute__((unused)),
                                  struct jmap_parser *parser __attribute__((unused)),
                                  const char *key,
                                  json_t *arg,
                                  void *rock)
{
    const char **addressbookId = (const char **) rock;
    int r = 1;

    /* Non-JMAP spec addressbookId argument */
    if (!strcmp(key, "addressbookId") && json_is_string(arg)) {
        *addressbookId = json_string_value(arg);
    }

    else r = 0;

    return r;
}

static void cachecards_cb(uint64_t rowid, void *payload, void *vrock)
{
    const char *eventrep = payload;
    struct cards_rock *rock = vrock;

    // there's no way to return errors, but luckily it doesn't matter if we
    // fail to cache
    carddav_write_jmapcache(rock->db, rowid,
                            JMAPCACHE_CONTACTVERSION, eventrep);
}

static int _contacts_get(struct jmap_req *req, carddav_cb_t *cb, int kind)
{
    struct jmap_parser parser = JMAP_PARSER_INITIALIZER;
    struct jmap_get get;
    json_t *err = NULL;
    struct carddav_db *db = NULL;
    int r = 0;

    r = carddav_create_defaultaddressbook(req->accountid);
    if (r == IMAP_MAILBOX_NONEXISTENT) {
        /* The account exists but does not have a root mailbox. */
        jmap_error(req, json_pack("{s:s}", "type", "accountNoAddressbooks"));
        return 0;
    } else if (r) return r;

    /* Build callback data */
    struct cards_rock rock = { NULL, req, &get, NULL /*mailbox*/,
                               HASHU64_TABLE_INITIALIZER, 0 /*rows */ };

    construct_hashu64_table(&rock.jmapcache, 512, 0);

    /* Parse request */
    char *mboxname = NULL;
    const char *addressbookId = NULL;
    jmap_get_parse(req, &parser,
                   kind == CARDDAV_KIND_GROUP ? group_props : contact_props,
                   /* allow_null_ids */ 1,
                   &_contact_getargs_parse, &addressbookId, &get, &err);
    if (err) {
        jmap_error(req, err);
        goto done;
    }

    rock.db = db = carddav_open_userid(req->accountid);
    if (!db) {
        syslog(LOG_ERR,
               "carddav_open_mailbox failed for user %s", req->accountid);
        r = IMAP_INTERNAL;
        goto done;
    }

    mboxname = addressbookId ?
        carddav_mboxname(req->accountid, addressbookId) : NULL;

    /* Does the client request specific events? */
    if (JNOTNULL(get.ids)) {
        size_t i;
        json_t *jval;
        json_array_foreach(get.ids, i, jval) {
            rock.rows = 0;
            const char *id = json_string_value(jval);

            r = carddav_get_cards(db, mboxname, id, kind, cb, &rock);
            if (r || !rock.rows) {
                json_array_append(get.not_found, jval);
            }
            r = 0; // we don't ever fail the whole request from this
        }
    }
    else {
        rock.rows = 0;
        r = carddav_get_cards(db, mboxname, NULL, kind, cb, &rock);
        if (r) goto done;
    }

    if (hashu64_count(&rock.jmapcache)) {
        r = carddav_begin(db);
        if (!r) hashu64_enumerate(&rock.jmapcache, cachecards_cb, &rock);
        if (r) carddav_abort(db);
        else r = carddav_commit(db);
        if (r) goto done;
    }

    /* Build response */
    json_t *jstate = jmap_getstate(req, MBTYPE_ADDRESSBOOK, /*refresh*/0);
    get.state = xstrdup(json_string_value(jstate));
    json_decref(jstate);
    jmap_ok(req, jmap_get_reply(&get));

  done:
    jmap_parser_fini(&parser);
    jmap_get_fini(&get);
    free(mboxname);
    mailbox_close(&rock.mailbox);
    free_hashu64_table(&rock.jmapcache, free);
    carddav_close(db);
    return r;
}

static int jmap_contactgroup_get(struct jmap_req *req)
{
    return _contacts_get(req, &getgroups_cb, CARDDAV_KIND_GROUP);
}

static const char *_json_array_get_string(const json_t *obj, size_t index)
{
    const json_t *jval = json_array_get(obj, index);
    if (!jval) return NULL;
    const char *val = json_string_value(jval);
    return val;
}


static int getchanges_cb(void *rock, struct carddav_data *cdata)
{
    struct changes_rock *urock = (struct changes_rock *) rock;
    struct dav_data dav = cdata->dav;
    const char *uid = cdata->vcard_uid;

    if (!jmap_hasrights_byname(urock->req, dav.mailbox, DACL_READ))
        return 0;

    /* Count, but don't process items that exceed the maximum record count. */
    if (urock->changes->max_changes &&
        ++(urock->seen_records) > urock->changes->max_changes) {
        urock->changes->has_more_changes = 1;
        return 0;
    }

    /* Report item as updated or destroyed. */
    if (dav.alive) {
        if (dav.createdmodseq <= urock->changes->since_modseq)
            json_array_append_new(urock->changes->updated, json_string(uid));
        else
            json_array_append_new(urock->changes->created, json_string(uid));
    } else {
        if (dav.createdmodseq <= urock->changes->since_modseq)
            json_array_append_new(urock->changes->destroyed, json_string(uid));
    }

    /* Fetch record to determine modseq. */
    if (dav.modseq > urock->highestmodseq) {
        urock->highestmodseq = dav.modseq;
    }

    return 0;
}

static int _contacts_changes(struct jmap_req *req, int kind)
{
    struct jmap_parser parser = JMAP_PARSER_INITIALIZER;
    struct jmap_changes changes;
    json_t *err = NULL;
    struct carddav_db *db = carddav_open_userid(req->accountid);
    if (!db) return -1;
    int r = -1;

    /* Parse request */
    char *mboxname = NULL;
    const char *addressbookId = NULL;
    jmap_changes_parse(req, &parser, &_contact_getargs_parse, &addressbookId,
                       &changes, &err);
    if (err) {
        jmap_error(req, err);
        goto done;
    }

    mboxname = addressbookId ?
        carddav_mboxname(req->accountid, addressbookId) : NULL;

    r = carddav_create_defaultaddressbook(req->accountid);
    if (r) goto done;

    /* Lookup updates. */
    struct changes_rock rock = { req, &changes, 0 /*seen_records*/, 0 /*highestmodseq*/};
    r = carddav_get_updates(db, changes.since_modseq, mboxname, kind,
                            -1 /*max_records*/, &getchanges_cb, &rock);
    if (r) goto done;

    strip_spurious_deletes(&rock);

    /* Determine new state. */
    changes.new_modseq = changes.has_more_changes ?
        rock.highestmodseq : jmap_highestmodseq(req, MBTYPE_ADDRESSBOOK);

    /* Build response */
    jmap_ok(req, jmap_changes_reply(&changes));

  done:
    jmap_changes_fini(&changes);
    jmap_parser_fini(&parser);
    carddav_close(db);
    free(mboxname);

    return r;
}

static int jmap_contactgroup_changes(struct jmap_req *req)
{
    return _contacts_changes(req, CARDDAV_KIND_GROUP);
}

static const char *_resolve_contactid(struct jmap_req *req, const char *id)
{
    if (id && *id == '#') {
        return jmap_lookup_id(req, id + 1);
    }
    return id;
}

static int _add_group_entries(struct jmap_req *req,
                              struct vparse_card *card, json_t *members,
                              json_t *invalid)
{
    vparse_delete_entries(card, NULL, "X-ADDRESSBOOKSERVER-MEMBER");
    int r = 0;
    size_t index;
    struct buf buf = BUF_INITIALIZER;

    for (index = 0; index < json_array_size(members); index++) {
        const char *item = _json_array_get_string(members, index);
        const char *uid = _resolve_contactid(req, item);
        if (!item || !uid) {
            buf_printf(&buf, "contactIds[%zu]", index);
            json_array_append_new(invalid, json_string(buf_cstring(&buf)));
            buf_reset(&buf);
            continue;
        }
        buf_setcstr(&buf, "urn:uuid:");
        buf_appendcstr(&buf, uid);
        vparse_add_entry(card, NULL,
                         "X-ADDRESSBOOKSERVER-MEMBER", buf_cstring(&buf));
        buf_reset(&buf);
    }

    buf_free(&buf);
    return r;
}

static int _add_othergroup_entries(struct jmap_req *req,
                                   struct vparse_card *card, json_t *members,
                                   json_t *invalid)
{
    vparse_delete_entries(card, NULL, "X-FM-OTHERACCOUNT-MEMBER");
    int r = 0;
    struct buf buf = BUF_INITIALIZER;
    const char *key;
    json_t *arg;
    json_object_foreach(members, key, arg) {
        unsigned i;
        for (i = 0; i < json_array_size(arg); i++) {
            const char *item = json_string_value(json_array_get(arg, i));
            const char *uid = _resolve_contactid(req, item);
            if (!item || !uid) {
                buf_printf(&buf, "otherAccountContactIds[%s]", key);
                json_array_append_new(invalid, json_string(buf_cstring(&buf)));
                buf_reset(&buf);
                continue;
            }
            buf_setcstr(&buf, "urn:uuid:");
            buf_appendcstr(&buf, uid);
            struct vparse_entry *entry =
                vparse_add_entry(card, NULL,
                                 "X-FM-OTHERACCOUNT-MEMBER", buf_cstring(&buf));
            vparse_add_param(entry, "USERID", key);
            buf_reset(&buf);
        }
    }
    buf_free(&buf);
    return r;
}

static void _contacts_set(struct jmap_req *req, unsigned kind)
{
    struct jmap_parser parser = JMAP_PARSER_INITIALIZER;
    struct jmap_set set;
    json_t *err = NULL;
    int r = 0;

    struct mailbox *mailbox = NULL;
    struct mailbox *newmailbox = NULL;

    struct carddav_db *db = carddav_open_userid(req->accountid);
    if (!db) {
        r = IMAP_INTERNAL;
        goto done;
    }

    /* Parse arguments */
    jmap_set_parse(req, &parser,
                   kind == CARDDAV_KIND_GROUP ? group_props : contact_props,
                   NULL, NULL, &set, &err);
    if (err) {
        jmap_error(req, err);
        goto done;
    }

    if (set.if_in_state) {
        /* TODO rewrite state function to use char* not json_t* */
        json_t *jstate = json_string(set.if_in_state);
        if (jmap_cmpstate(req, jstate, MBTYPE_ADDRESSBOOK)) {
            jmap_error(req, json_pack("{s:s}", "type", "stateMismatch"));
            json_decref(jstate);
            goto done;
        }
        json_decref(jstate);
        set.old_state = xstrdup(set.if_in_state);
    }
    else {
        json_t *jstate = jmap_getstate(req, MBTYPE_ADDRESSBOOK, /*refresh*/0);
        set.old_state = xstrdup(json_string_value(jstate));
        json_decref(jstate);
    }

    r = carddav_create_defaultaddressbook(req->accountid);
    if (r) goto done;

    /* create */
    const char *key;
    json_t *arg;
    json_object_foreach(set.create, key, arg) {
        char *uid = NULL;
        json_t *invalid = json_pack("[]");
        r = _contact_set_create(req, kind, arg,
                                NULL, &mailbox, &uid, invalid);
        if (r) {
            json_t *err;
            switch (r) {
                case HTTP_FORBIDDEN:
                case IMAP_PERMISSION_DENIED:
                    err = json_pack("{s:s}", "type", "forbidden");
                    break;
                case IMAP_QUOTA_EXCEEDED:
                    err = json_pack("{s:s}", "type", "overQuota");
                    break;
                default:
                    err = jmap_server_error(r);
            }
            json_object_set_new(set.not_created, key, err);
            r = 0;
            free(uid);
            continue;
        }
        if (json_array_size(invalid)) {
            json_t *err = json_pack("{s:s s:o}",
                                    "type", "invalidProperties",
                                    "properties", invalid);
            json_object_set_new(set.not_created, key, err);
            free(uid);
            continue;
        }
        json_decref(invalid);

        /* Report calendar event as created. */
        json_object_set_new(set.created, key, json_pack("{s:s}", "id", uid));

        /* Register creation id */
        jmap_add_id(req, key, uid);
        free(uid);
    }

    /* update */
    const char *uid;
    json_object_foreach(set.update, uid, arg) {
        struct carddav_data *cdata = NULL;
        r = carddav_lookup_uid(db, uid, &cdata);
        uint32_t olduid;
        char *resource = NULL;
        int do_move = 0;
        json_t *jupdated = NULL;

        /* is it a valid contact? */
        if (r || !cdata || !cdata->dav.imap_uid || cdata->kind != kind) {
            r = 0;
            json_t *err = json_pack("{s:s}", "type", "notFound");
            json_object_set_new(set.not_updated, uid, err);
            continue;
        }

        json_t *abookid = json_object_get(arg, "addressbookId");
        if (abookid && json_string_value(abookid)) {
            const char *mboxname =
                mboxname_abook(req->accountid, json_string_value(abookid));
            if (strcmp(mboxname, cdata->dav.mailbox)) {
                /* move */
                if (!jmap_hasrights_byname(req, mboxname, DACL_WRITECONT)) {
                    json_t *err = json_pack("{s:s s:[s]}",
                                            "type", "invalidProperties",
                                            "properties", "addressbookId");
                    json_object_set_new(set.not_updated, uid, err);
                    continue;
                }
                r = jmap_openmbox(req, mboxname, &newmailbox, 1);
                if (r) {
                    syslog(LOG_ERR, "IOERROR: failed to open %s", mboxname);
                    goto done;
                }
                do_move = 1;
            }
            json_object_del(arg, "addressbookId");
        }

        int needrights = do_move ?
            (DACL_READ | DACL_RMRSRC) : required_set_rights(arg);

        if (!jmap_hasrights_byname(req, cdata->dav.mailbox, needrights)) {
            int rights = jmap_myrights_byname(req, cdata->dav.mailbox);
            json_t *err = json_pack("{s:s}", "type",
                                    rights & ACL_READ ?
                                    "accountReadOnly" : "notFound");
            json_object_set_new(set.not_updated, uid, err);
            continue;
        }

        if (!mailbox || strcmp(mailbox->name, cdata->dav.mailbox)) {
            jmap_closembox(req, &mailbox);
            r = jmap_openmbox(req, cdata->dav.mailbox, &mailbox, 1);
            if (r) {
                syslog(LOG_ERR, "IOERROR: failed to open %s",
                       cdata->dav.mailbox);
                goto done;
            }
        }

        struct index_record record;

        r = mailbox_find_index_record(mailbox, cdata->dav.imap_uid, &record);
        if (r) goto done;

        olduid = cdata->dav.imap_uid;
        resource = xstrdup(cdata->dav.resource);

        struct entryattlist *annots = NULL;
        strarray_t *flags = NULL;

        /* Load message containing the resource and parse vcard data */
        struct vparse_card *vcard = record_to_vcard(mailbox, &record);
        if (!vcard || !vcard->objects) {
            syslog(LOG_ERR, "record_to_vcard failed for record %u:%s",
                   cdata->dav.imap_uid, mailbox->name);
            r = 0;
            json_t *err = json_pack("{s:s}", "type", "parseError");
            json_object_set_new(set.not_updated, uid, err);
            goto finish;
        }

        struct vparse_card *card = vcard->objects;
        vparse_replace_entry(card, NULL, "VERSION", "3.0");
        vparse_replace_entry(card, NULL, "PRODID", _prodid);

        json_t *invalid = json_pack("[]");

        if (kind == CARDDAV_KIND_GROUP) {
            json_t *namep = NULL;
            json_t *members = NULL;
            json_t *others = NULL;
            json_t *jval;
            const char *key;

            json_object_foreach(arg, key, jval) {
                if (!strcmp(key, "name")) {
                    if (json_is_string(jval))
                        namep = jval;
                    else json_array_append_new(invalid, json_string("name"));
                }
                else if (!strcmp(key, "contactIds")) {
                    members = jval;
                }
                else if (!strcmp(key, "otherAccountContactIds")) {
                    others = jval;
                }
                else if (!strncmp(key, "otherAccountContactIds/", 23)) {
                    /* Read and apply patch to current card */
                    json_t *jcurrent = jmap_group_from_vcard(card);
                    if (!jcurrent) {
                        syslog(LOG_ERR, "can't read vcard %u:%s for update",
                                cdata->dav.imap_uid, mailbox->name);
                        r = 0;
                        json_t *err = json_pack("{s:s s:s}",
                                "type", "serverError", "description", "invalid current card");
                        json_object_set_new(set.not_updated, uid, err);
                        goto finish;
                    }
                    jupdated = jmap_patchobject_apply(jcurrent, arg, NULL);
                    json_decref(jcurrent);
                    if (JNOTNULL(jupdated)) {
                        json_object_del(jupdated, "addressbookId");
                        /* Now read the updated property value */
                        others = json_object_get(jupdated, "otherAccountContactIds");
                    }
                }
                else if (!strcmp(key, "id") || !strcmp(key, "uid")) {
                    if (cdata && strcmpnull(cdata->vcard_uid, json_string_value(jval))) {
                        json_array_append_new(invalid, json_string(key));
                    }
                }
            }

            if (namep) {
                const char *name = json_string_value(namep);
                if (name) {
                    vparse_replace_entry(card, NULL, "FN", name);
                    vparse_replace_entry(card, NULL, "N", name);
                }
            }
            else if (!vparse_get_entry(card, NULL, "N")) {
                struct vparse_entry *entry = vparse_get_entry(card, NULL, "FN");
                if (entry) vparse_replace_entry(card, NULL, "N", entry->v.value);
            }
            if (members) {
                _add_group_entries(req, card, members, invalid);
            }
            if (others) {
                _add_othergroup_entries(req, card, others, invalid);
            }
        }
        else {
            flags = mailbox_extract_flags(mailbox, &record, req->accountid);
            annots = mailbox_extract_annots(mailbox, &record);

            r = _json_to_card(req, cdata, card, arg, flags, &annots, invalid);
            if (r == HTTP_NO_CONTENT) {
                r = 0;
                if (!newmailbox) {
                    /* just bump the modseq
                       if in the same mailbox and no data change */
                    syslog(LOG_NOTICE, "jmap: touch contact %s/%s",
                           req->accountid, resource);
                    if (strarray_find_case(flags, "\\Flagged", 0) >= 0)
                        record.system_flags |= FLAG_FLAGGED;
                    else
                        record.system_flags &= ~FLAG_FLAGGED;
                    annotate_state_t *state = NULL;
                    r = mailbox_get_annotate_state(mailbox, record.uid, &state);
                    annotate_state_set_auth(state, 0,
                                            req->accountid, req->authstate);
                    if (!r) r = annotate_state_store(state, annots);
                    if (!r) r = mailbox_rewrite_index_record(mailbox, &record);
                    if (!r) json_object_set_new(set.updated, uid, json_null());
                    goto finish;
                }
            }
        }

        if (!r && !json_array_size(invalid)) {
            syslog(LOG_NOTICE, "jmap: update %s %s/%s",
                   kind == CARDDAV_KIND_GROUP ? "group" : "contact",
                   req->accountid, resource);
            r = carddav_store(newmailbox ? newmailbox : mailbox, card, resource,
                              record.createdmodseq, flags, annots, req->accountid,
                              req->authstate, ignorequota);
            if (!r)
                r = carddav_remove(mailbox, olduid,
                                   /*isreplace*/!newmailbox, req->accountid);
        }

        if (json_array_size(invalid)) {
            json_t *err = json_pack("{s:s s:O}",
                                    "type", "invalidProperties",
                                    "properties", invalid);
            json_object_set_new(set.not_updated, uid, err);
            goto finish;
        }
        else if (r) {
            json_t *err = NULL;
            switch (r) {
                case HTTP_FORBIDDEN:
                case IMAP_PERMISSION_DENIED:
                    err = json_pack("{s:s}", "type", "forbidden");
                    break;
                case IMAP_QUOTA_EXCEEDED:
                    err = json_pack("{s:s}", "type", "overQuota");
                    break;
                default:
                    err = jmap_server_error(r);
            }
            json_object_set_new(set.not_updated, uid, err);
            goto finish;
        }
        else json_object_set_new(set.updated, uid, json_null());

      finish:
        strarray_free(flags);
        freeentryatts(annots);
        jmap_closembox(req, &newmailbox);
        vparse_free_card(vcard);
        free(resource);
        json_decref(jupdated);
        json_decref(invalid);
        r = 0;
    }


    /* destroy */
    size_t index;
    for (index = 0; index < json_array_size(set.destroy); index++) {
        const char *uid = _json_array_get_string(set.destroy, index);
        if (!uid) {
            json_t *err = json_pack("{s:s}", "type", "invalidArguments");
            json_object_set_new(set.not_destroyed, uid, err);
            continue;
        }
        struct carddav_data *cdata = NULL;
        uint32_t olduid;
        r = carddav_lookup_uid(db, uid, &cdata);

        /* is it a valid contact? */
        if (r || !cdata || !cdata->dav.imap_uid || cdata->kind != kind) {
            r = 0;
            json_t *err = json_pack("{s:s}", "type", "notFound");
            json_object_set_new(set.not_destroyed, uid, err);
            continue;
        }
        olduid = cdata->dav.imap_uid;

        if (!jmap_hasrights_byname(req, cdata->dav.mailbox, DACL_RMRSRC)) {
            int rights = jmap_myrights_byname(req, cdata->dav.mailbox);
            json_t *err = json_pack("{s:s}", "type",
                                    rights & ACL_READ ?
                                    "accountReadOnly" : "notFound");
            json_object_set_new(set.not_destroyed, uid, err);
            continue;
        }

        if (!mailbox || strcmp(mailbox->name, cdata->dav.mailbox)) {
            jmap_closembox(req, &mailbox);
            r = jmap_openmbox(req, cdata->dav.mailbox, &mailbox, 1);
            if (r) goto done;
        }

        syslog(LOG_NOTICE,
               "jmap: remove %s %s/%s",
               kind == CARDDAV_KIND_GROUP ? "group" : "contact",
               req->accountid, uid);
        r = carddav_remove(mailbox, olduid, /*isreplace*/0, req->accountid);
        if (r) {
            syslog(LOG_ERR, "IOERROR: Contact%s/set remove failed for %s %u",
                   kind == CARDDAV_KIND_GROUP ? "Group" : "",
                   mailbox->name, olduid);
            goto done;
        }

        json_array_append_new(set.destroyed, json_string(uid));
    }

    /* force modseq to stable */
    if (mailbox) mailbox_unlock_index(mailbox, NULL);

    // TODO refactor jmap_getstate to return a string, once
    // all code has been migrated to the new JMAP parser.
    json_t *jstate = jmap_getstate(req, MBTYPE_ADDRESSBOOK, /*refresh*/1);
    set.new_state = xstrdup(json_string_value(jstate));
    json_decref(jstate);

    jmap_ok(req, jmap_set_reply(&set));
    r = 0;

done:
    if (r) jmap_error(req, jmap_server_error(r));
    jmap_parser_fini(&parser);
    jmap_set_fini(&set);
    jmap_closembox(req, &newmailbox);
    jmap_closembox(req, &mailbox);

    carddav_close(db);
}

static int jmap_contactgroup_set(struct jmap_req *req)
{
    _contacts_set(req, CARDDAV_KIND_GROUP);
    return 0;
}

/* Extract separate y,m,d from YYYY-MM-DD or (with ignore_hyphens) YYYYMMDD
 *
 * This handles birthday/anniversary and BDAY/ANNIVERSARY for JMAP and vCard
 *
 * JMAP dates are _always_ YYYY-MM-DD, so use require_hyphens = 1
 *
 * For vCard, this handles "date-value" from RFC2426 (which is "date" from
 * RFC2425), used by BDAY (ANNIVERSARY isn't in vCard 3). vCard 4 says BDAY and
 * ANNIVERSARY is date-and-or-time, which is far more complicated. I haven't
 * seen that in the wild yet and hope I never do.
 */
static int _parse_date(const char *date, unsigned *y,
                       unsigned *m, unsigned *d, int require_hyphens)
{
    /* there isn't a convenient libc function that will let us convert parts of
     * a string to integer and only take digit characters, so we just pull it
     * apart ourselves */

    const char *yp = NULL, *mp = NULL, *dp = NULL;

    /* getting pointers to the ymd components, skipping hyphens if necessary.
     * format checking as we go. no need to strlen() beforehand, it will fall
     * out of the range checks. */
    yp = date;
    if (yp[0] < '0' || yp[0] > '9' ||
        yp[1] < '0' || yp[1] > '9' ||
        yp[2] < '0' || yp[2] > '9' ||
        yp[3] < '0' || yp[3] > '9') return -1;

    mp = &yp[4];

    if (*mp == '-') mp++;
    else if (require_hyphens) return -1;

    if (mp[0] < '0' || mp[0] > '9' ||
        mp[1] < '0' || mp[1] > '9') return -1;

    dp = &mp[2];

    if (*dp == '-') dp++;
    else if (require_hyphens) return -1;

    if (dp[0] < '0' || dp[0] > '9' ||
        dp[1] < '0' || dp[1] > '9') return -1;

    if (dp[2] != '\0') return -1;

    /* convert to integer. ascii digits are 0x30-0x37, so we can take bottom
     * four bits and multiply */
    *y =
        (yp[0] & 0xf) * 1000 +
        (yp[1] & 0xf) * 100 +
        (yp[2] & 0xf) * 10 +
        (yp[3] & 0xf);

    *m =
        (mp[0] & 0xf) * 10 +
        (mp[1] & 0xf);

    *d =
        (dp[0] & 0xf) * 10 +
        (dp[1] & 0xf);

    return 0;
}

static void _date_to_jmap(struct vparse_entry *entry, struct buf *buf)
{
    if (!entry)
        goto no_date;

    unsigned y, m, d;
    if (_parse_date(entry->v.value, &y, &m, &d, 0))
        goto no_date;

    if (y < 1604 || m > 12 || d > 31)
        goto no_date;

    const struct vparse_param *param;
    for (param = entry->params; param; param = param->next) {
        if (!strcasecmp(param->name, "x-apple-omit-year"))
            /* XXX compare value with actual year? */
            y = 0;
        if (!strcasecmp(param->name, "x-fm-no-month"))
            m = 0;
        if (!strcasecmp(param->name, "x-fm-no-day"))
            d = 0;
    }

    /* sigh, magic year 1604 has been seen without X-APPLE-OMIT-YEAR, making
     * me wonder what the bloody point is */
    if (y == 1604)
        y = 0;

    buf_reset(buf);
    buf_printf(buf, "%04d-%02d-%02d", y, m, d);
    return;

no_date:
    buf_setcstr(buf, "0000-00-00");
}

static const char *_servicetype(const char *type)
{
    /* add new services here */
    if (!strcasecmp(type, "aim")) return "AIM";
    if (!strcasecmp(type, "facebook")) return "Facebook";
    if (!strcasecmp(type, "flickr")) return "Flickr";
    if (!strcasecmp(type, "gadugadu")) return "GaduGadu";
    if (!strcasecmp(type, "github")) return "GitHub";
    if (!strcasecmp(type, "googletalk")) return "GoogleTalk";
    if (!strcasecmp(type, "icq")) return "ICQ";
    if (!strcasecmp(type, "jabber")) return "Jabber";
    if (!strcasecmp(type, "linkedin")) return "LinkedIn";
    if (!strcasecmp(type, "msn")) return "MSN";
    if (!strcasecmp(type, "myspace")) return "MySpace";
    if (!strcasecmp(type, "qq")) return "QQ";
    if (!strcasecmp(type, "skype")) return "Skype";
    if (!strcasecmp(type, "twitter")) return "Twitter";
    if (!strcasecmp(type, "yahoo")) return "Yahoo";

    syslog(LOG_NOTICE, "unknown service type %s", type);
    return type;
}

/* Convert the VCARD card to jmap properties */
static json_t *jmap_contact_from_vcard(struct vparse_card *card,
                                       struct mailbox *mailbox,
                                       struct index_record *record)
{
    strarray_t *empty = NULL;
    json_t *obj = json_pack("{}");
    struct buf buf = BUF_INITIALIZER;
    struct vparse_entry *entry;

    const strarray_t *n = vparse_multival(card, "n");
    const strarray_t *org = vparse_multival(card, "org");
    if (!n) n = empty ? empty : (empty = strarray_new());
    if (!org) org = empty ? empty : (empty = strarray_new());

    /* name fields: Family; Given; Middle; Prefix; Suffix. */

    const char *family = strarray_safenth(n, 0);
    json_object_set_new(obj, "lastName", json_string(family));

    /* JMAP doesn't have a separate field for Middle (aka "Additional
     * Names"), so we just mash them into firstName. See reverse of this in
     * _json_to_card */
    const char *given = strarray_safenth(n, 1);
    const char *middle = strarray_safenth(n, 2);
    buf_setcstr(&buf, given);
    if (*middle) {
        buf_putc(&buf, ' ');
        buf_appendcstr(&buf, middle);
    }
    json_object_set_new(obj, "firstName", json_string(buf_cstring(&buf)));

    const char *prefix = strarray_safenth(n, 3);
    json_object_set_new(obj, "prefix",
                        json_string(prefix)); /* just prefix */

    const char *suffix = strarray_safenth(n, 4);
    json_object_set_new(obj, "suffix",
                        json_string(suffix)); /* just suffix */

    json_object_set_new(obj, "company",
                        json_string(strarray_safenth(org, 0)));
    json_object_set_new(obj, "department",
                        json_string(strarray_safenth(org, 1)));

    /* we used to store jobTitle in ORG[2] instead of TITLE, which confused
     * CardDAV clients. that's fixed, but there's now lots of cards with it
     * stored in the wrong place, so check both */
    const char *item = vparse_stringval(card, "title");
    if (!item)
        item = strarray_safenth(org, 2);
    json_object_set_new(obj, "jobTitle", json_string(item));

    json_t *adr = json_array();

    for (entry = card->properties; entry; entry = entry->next) {
        if (strcasecmp(entry->name, "adr")) continue;
        json_t *item = json_pack("{}");

        /* XXX - type and label */
        const strarray_t *a = entry->v.values;

        const struct vparse_param *param;
        const char *type = "other";
        const char *label = NULL;
        for (param = entry->params; param; param = param->next) {
            if (!strcasecmp(param->name, "type")) {
                if (!strcasecmp(param->value, "home")) {
                    type = "home";
                }
                else if (!strcasecmp(param->value, "work")) {
                    type = "work";
                }
                else if (!strcasecmp(param->value, "billing")) {
                    type = "billing";
                }
                else if (!strcasecmp(param->value, "postal")) {
                    type = "postal";
                }
            }
            else if (!strcasecmp(param->name, "label")) {
                label = param->value;
            }
        }
        json_object_set_new(item, "type", json_string(type));
        json_object_set_new(item, "label", label ? json_string(label) : json_null());

        const char *pobox = strarray_safenth(a, 0);
        const char *extended = strarray_safenth(a, 1);
        const char *street = strarray_safenth(a, 2);
        buf_reset(&buf);
        if (*pobox) {
            buf_appendcstr(&buf, pobox);
            if (extended || street) buf_putc(&buf, '\n');
        }
        if (*extended) {
            buf_appendcstr(&buf, extended);
            if (street) buf_putc(&buf, '\n');
        }
        if (*street) {
            buf_appendcstr(&buf, street);
        }

        json_object_set_new(item, "street",
                            json_string(buf_cstring(&buf)));
        json_object_set_new(item, "locality",
                            json_string(strarray_safenth(a, 3)));
        json_object_set_new(item, "region",
                            json_string(strarray_safenth(a, 4)));
        json_object_set_new(item, "postcode",
                            json_string(strarray_safenth(a, 5)));
        json_object_set_new(item, "country",
                            json_string(strarray_safenth(a, 6)));

        json_array_append_new(adr, item);
    }

    json_object_set_new(obj, "addresses", adr);

    /* emails - we need to open code this, because it's repeated */
    json_t *emails = json_array();

    int defaultIndex = -1;
    int i = 0;
    for (entry = card->properties; entry; entry = entry->next) {
        if (strcasecmp(entry->name, "email")) continue;
        json_t *item = json_pack("{}");
        const struct vparse_param *param;
        const char *type = "other";
        const char *label = NULL;
        for (param = entry->params; param; param = param->next) {
            if (!strcasecmp(param->name, "type")) {
                if (!strcasecmp(param->value, "home")) {
                    type = "personal";
                }
                else if (!strcasecmp(param->value, "work")) {
                    type = "work";
                }
                else if (!strcasecmp(param->value, "pref")) {
                    if (defaultIndex < 0)
                        defaultIndex = i;
                }
            }
            else if (!strcasecmp(param->name, "label")) {
                label = param->value;
            }
        }
        json_object_set_new(item, "type", json_string(type));
        if (label) json_object_set_new(item, "label", json_string(label));

        json_object_set_new(item, "value", json_string(entry->v.value));

        json_array_append_new(emails, item);
        i++;
    }

    if (defaultIndex < 0)
        defaultIndex = 0;
    int size = json_array_size(emails);
    for (i = 0; i < size; i++) {
        json_t *item = json_array_get(emails, i);
        json_object_set_new(item, "isDefault",
                            i == defaultIndex ? json_true() : json_false());
    }

    json_object_set_new(obj, "emails", emails);

    /* address - we need to open code this, because it's repeated */
    json_t *phones = json_array();

    for (entry = card->properties; entry; entry = entry->next) {
        if (strcasecmp(entry->name, "tel")) continue;
        json_t *item = json_pack("{}");
        const struct vparse_param *param;
        const char *type = "other";
        const char *label = NULL;
        for (param = entry->params; param; param = param->next) {
            if (!strcasecmp(param->name, "type")) {
                if (!strcasecmp(param->value, "home")) {
                    type = "home";
                }
                else if (!strcasecmp(param->value, "work")) {
                    type = "work";
                }
                else if (!strcasecmp(param->value, "cell")) {
                    type = "mobile";
                }
                else if (!strcasecmp(param->value, "mobile")) {
                    type = "mobile";
                }
                else if (!strcasecmp(param->value, "fax")) {
                    type = "fax";
                }
                else if (!strcasecmp(param->value, "pager")) {
                    type = "pager";
                }
            }
            else if (!strcasecmp(param->name, "label")) {
                label = param->value;
            }
        }
        json_object_set_new(item, "type", json_string(type));
        if (label) json_object_set_new(item, "label", json_string(label));

        json_object_set_new(item, "value", json_string(entry->v.value));

        json_array_append_new(phones, item);
    }

    json_object_set_new(obj, "phones", phones);

    /* address - we need to open code this, because it's repeated */
    json_t *online = json_array();

    for (entry = card->properties; entry; entry = entry->next) {
        if (!strcasecmp(entry->name, "url")) {
            json_t *item = json_pack("{}");
            const struct vparse_param *param;
            const char *label = NULL;
            for (param = entry->params; param; param = param->next) {
                if (!strcasecmp(param->name, "label")) {
                    label = param->value;
                }
            }
            json_object_set_new(item, "type", json_string("uri"));
            if (label) json_object_set_new(item, "label", json_string(label));
            json_object_set_new(item, "value", json_string(entry->v.value));
            json_array_append_new(online, item);
        }
        if (!strcasecmp(entry->name, "impp")) {
            json_t *item = json_pack("{}");
            const struct vparse_param *param;
            const char *label = NULL;
            for (param = entry->params; param; param = param->next) {
                if (!strcasecmp(param->name, "x-service-type")) {
                    label = _servicetype(param->value);
                }
            }
            json_object_set_new(item, "type", json_string("username"));
            if (label) json_object_set_new(item, "label", json_string(label));
            json_object_set_new(item, "value", json_string(entry->v.value));
            json_array_append_new(online, item);
        }
        if (!strcasecmp(entry->name, "x-social-profile")) {
            json_t *item = json_pack("{}");
            const struct vparse_param *param;
            const char *label = NULL;
            const char *value = NULL;
            for (param = entry->params; param; param = param->next) {
                if (!strcasecmp(param->name, "type")) {
                    label = _servicetype(param->value);
                }
                if (!strcasecmp(param->name, "x-user")) {
                    value = param->value;
                }
            }
            json_object_set_new(item, "type", json_string("username"));
            if (label) json_object_set_new(item, "label", json_string(label));
            json_object_set_new(item, "value",
                                json_string(value ? value : entry->v.value));
            json_array_append_new(online, item);
        }
        if (!strcasecmp(entry->name, "x-fm-online-other")) {
            json_t *item = json_pack("{}");
            const struct vparse_param *param;
            const char *label = NULL;
            for (param = entry->params; param; param = param->next) {
                if (!strcasecmp(param->name, "label")) {
                    label = param->value;
                }
            }
            json_object_set_new(item, "type", json_string("other"));
            if (label) json_object_set_new(item, "label", json_string(label));
            json_object_set_new(item, "value", json_string(entry->v.value));
            json_array_append_new(online, item);
        }
    }

    json_object_set_new(obj, "online", online);

    item = vparse_stringval(card, "nickname");
    json_object_set_new(obj, "nickname", json_string(item ? item : ""));

    entry = vparse_get_entry(card, NULL, "anniversary");
    _date_to_jmap(entry, &buf);
    json_object_set_new(obj, "anniversary", json_string(buf_cstring(&buf)));

    entry = vparse_get_entry(card, NULL, "bday");
    _date_to_jmap(entry, &buf);
    json_object_set_new(obj, "birthday", json_string(buf_cstring(&buf)));

    item = vparse_stringval(card, "note");
    json_object_set_new(obj, "notes", json_string(item ? item : ""));

    item = vparse_stringval(card, "photo");
    json_object_set_new(obj, "x-hasPhoto",
                        item ? json_true() : json_false());

    struct vparse_entry *photo = vparse_get_entry(card, NULL, "photo");
    struct message_guid guid;
    char *type = NULL;
    json_t *file;

    if (photo &&
        (size = vcard_prop_decode_value(photo, NULL, &type, &guid))) {
        char blob_id[JMAP_BLOBID_SIZE];
        jmap_set_blobid(&guid, blob_id);

        file = json_pack("{s:s s:i s:s? s:n}",
                         "blobId", blob_id, "size", size,
                         "type", type, "name");
    }
    else file = json_null();

    json_object_set_new(obj, "avatar", file);
    free(type);

    // record properties

    json_object_set_new(obj, "isFlagged",
                        record->system_flags & FLAG_FLAGGED ? json_true() :
                        json_false());

    const char *annot = DAV_ANNOT_NS "<" XML_NS_CYRUS ">importance";
    // NOTE: using buf_free here because annotatemore_msg_lookup uses
    // buf_init_ro on the buffer, which blats the base pointer.
    buf_free(&buf);
    annotatemore_msg_lookup(mailbox->name, record->uid, annot, "", &buf);
    double val = 0;
    if (buf.len) val = strtod(buf_cstring(&buf), NULL);

    // need to keep the x- version while AJAXUI is around
    json_object_set_new(obj, "importance", json_real(val));

    /* XXX - other fields */

    buf_free(&buf);
    if (empty) strarray_free(empty);

    return obj;
}

static const char *_encode_contact_blobid(struct carddav_data *cdata,
                                          struct buf *dst)
{
    /* Set vCard smart blob prefix */
    buf_putc(dst, 'V');

    /* Encode vCard UID */
    char *b64uid =
        jmap_encode_base64_nopad(cdata->vcard_uid, strlen(cdata->vcard_uid));
    if (!b64uid) {
        buf_reset(dst);
        return NULL;
    }
    buf_appendcstr(dst, b64uid);
    free(b64uid);

    /* Encode modseq */
    buf_printf(dst, "-" MODSEQ_FMT, cdata->dav.modseq);

    return buf_cstring(dst);
}

static int _decode_contact_blobid(const char *blobid,
                                  char **uidptr,
                                  modseq_t *modseqptr)
{
    char *uid = NULL;
    modseq_t modseq = 0;
    int is_valid = 0;

    /* Decode vCard UID */
    const char *base = blobid+1;
    const char *p = strchr(base, '-');
    if (!p) goto done;
    uid = jmap_decode_base64_nopad(base, p-base);
    if (!uid) goto done;
    base = p + 1;

    /* Decode modseq */
    if (*base == '\0') goto done;
    char *endptr = NULL;
    errno = 0;
    modseq = strtoull(base, &endptr, 10);
    if (errno == ERANGE || (*endptr && *endptr != '-')) {
        goto done;
    }
    base = endptr;

    /* All done */
    *uidptr = uid;
    *modseqptr = modseq;
    is_valid = 1;

done:
    if (!is_valid) {
        free(uid);
    }
    return is_valid;
}

static int jmap_contact_getblob(jmap_req_t *req,
                                const char *blobid,
                                const char *accept_mime)
{
    struct carddav_db *db = NULL;
    struct carddav_data *cdata = NULL;
    struct mailbox *mailbox = NULL;
    struct vparse_card *vcard = NULL;
    char *uid = NULL;
    modseq_t modseq;
    struct buf buf = BUF_INITIALIZER;
    int res = 0;
    int r;

    if (*blobid != 'V') return 0;

    if (!_decode_contact_blobid(blobid, &uid, &modseq)) {
        res = HTTP_BAD_REQUEST;
        goto done;
    }

    /* Lookup uid in CarddavDB */
    db = carddav_open_userid(req->accountid);
    if (!db) {
        req->txn->error.desc = "no addressbook db";
        res = HTTP_SERVER_ERROR;
        goto done;
    }
    if (carddav_lookup_uid(db, uid, &cdata)) {
        res = HTTP_NOT_FOUND;
        goto done;
    }
    if (!jmap_hasrights_byname(req, cdata->dav.mailbox, DACL_READ)) {
        res = HTTP_NOT_FOUND;
        goto done;
    }

    /* Validate modseq */
    if (modseq != cdata->dav.modseq) {
        res = HTTP_NOT_FOUND;
        goto done;
    }

    /* Open mailbox, we need it now */
    if ((r = jmap_openmbox(req, cdata->dav.mailbox, &mailbox, 0))) {
        req->txn->error.desc = error_message(r);
        res = HTTP_SERVER_ERROR;
        goto done;
    }

    /* Make sure client can handle blob type. */
    if (accept_mime) {
        if (strcmp(accept_mime, "application/octet-stream") &&
            strcmp(accept_mime, "text/vcard")) {
            res = HTTP_NOT_ACCEPTABLE;
            goto done;
        }
    }

    /* Load vCard data */
    struct index_record record;
    if (!mailbox_find_index_record(mailbox, cdata->dav.imap_uid, &record)) {
        vcard = record_to_vcard(mailbox, &record);
    }
    if (!vcard) {
        req->txn->error.desc = "failed to load record";
        res = HTTP_SERVER_ERROR;
        goto done;
    }

    /* Write blob to socket */
    char *content_type = NULL;
    if (!accept_mime || !strcmp(accept_mime, "text/vcard")) {
        struct vparse_entry *entry =
            vparse_get_entry(vcard->objects, NULL, "VERSION");
        if (entry) {
            content_type =
                strconcat("text/calendar; version=", entry->v.value, NULL);
            req->txn->resp_body.type = content_type;
        }
    }
    if (!req->txn->resp_body.type) {
        req->txn->resp_body.type = accept_mime;
    }

    /* Write body */
    vparse_tobuf(vcard, &buf);
    req->txn->resp_body.len = buf_len(&buf);
    write_body(HTTP_OK, req->txn, buf_base(&buf), buf_len(&buf));
    free(content_type);
    res = HTTP_OK;

done:
    if (res != HTTP_OK && !req->txn->error.desc) {
        const char *desc = NULL;
        switch (res) {
            case HTTP_BAD_REQUEST:
                desc = "invalid contact blobid";
                break;
            case HTTP_NOT_FOUND:
                desc = "failed to find blob by contact blobid";
                break;
            default:
                desc = error_message(res);
        }
        req->txn->error.desc = desc;
    }
    if (vcard) vparse_free_card(vcard);
    if (mailbox) jmap_closembox(req, &mailbox);
    if (db) carddav_close(db);
    buf_free(&buf);
    free(uid);
    return res;
}

static int getcontacts_cb(void *rock, struct carddav_data *cdata)
{
    struct cards_rock *crock = (struct cards_rock *) rock;
    struct index_record record;
    json_t *obj = NULL;
    int r = 0;

    if (!jmap_hasrights_byname(crock->req, cdata->dav.mailbox, DACL_READ))
        return 0;

    if (cdata->jmapversion == JMAPCACHE_CONTACTVERSION) {
        json_error_t jerr;
        obj = json_loads(cdata->jmapdata, 0, &jerr);
        goto gotvalue;
    }

    if (!crock->mailbox || strcmp(crock->mailbox->name, cdata->dav.mailbox)) {
        mailbox_close(&crock->mailbox);
        r = mailbox_open_irl(cdata->dav.mailbox, &crock->mailbox);
        if (r) return r;
    }

    r = mailbox_find_index_record(crock->mailbox, cdata->dav.imap_uid, &record);
    if (r) return r;

    /* Load message containing the resource and parse vcard data */
    struct vparse_card *vcard = record_to_vcard(crock->mailbox, &record);
    if (!vcard || !vcard->objects) {
        syslog(LOG_ERR, "record_to_vcard failed for record %u:%s",
                cdata->dav.imap_uid, crock->mailbox->name);
        vparse_free_card(vcard);
        return IMAP_INTERNAL;
    }

    /* Convert the VCARD to a JMAP contact. */
    obj = jmap_contact_from_vcard(vcard->objects, crock->mailbox, &record);
    vparse_free_card(vcard);

gotvalue:
    jmap_filterprops(obj, crock->get->props);

    if (jmap_wantprop(crock->get->props, "x-href")) {
        char *xhref = jmap_xhref(cdata->dav.mailbox, cdata->dav.resource);
        json_object_set_new(obj, "x-href", json_string(xhref));
        free(xhref);
    }
    if (jmap_wantprop(crock->get->props, "blobId")) {
        json_t *jblobid = json_null();
        struct buf blobid = BUF_INITIALIZER;
        if (_encode_contact_blobid(cdata, &blobid)) {
            jblobid = json_string(buf_cstring(&blobid));
        }
        buf_free(&blobid);
        json_object_set_new(obj, "blobId", jblobid);
    }

    json_object_set_new(obj, "id", json_string(cdata->vcard_uid));
    json_object_set_new(obj, "uid", json_string(cdata->vcard_uid));

    json_object_set_new(obj, "addressbookId",
                        json_string(strrchr(cdata->dav.mailbox, '.')+1));

    json_array_append_new(crock->get->list, obj);
    crock->rows++;

    return 0;
}

static int jmap_contact_get(struct jmap_req *req)
{
    return _contacts_get(req, &getcontacts_cb, CARDDAV_KIND_CONTACT);
}

static int jmap_contact_changes(struct jmap_req *req)
{
    return _contacts_changes(req, CARDDAV_KIND_CONTACT);
}

typedef struct contact_filter {
    hash_table *inContactGroup;
    json_t *isFlagged;
    const char *text;
    const char *prefix;
    const char *firstName;
    const char *lastName;
    const char *suffix;
    const char *nickname;
    const char *company;
    const char *department;
    const char *jobTitle;
    const char *email;
    const char *phone;
    const char *online;
    const char *address;
    const char *notes;
} contact_filter;

typedef struct contact_filter_rock {
    struct carddav_db *carddavdb;
    struct carddav_data *cdata;
    json_t *contact;
} contact_filter_rock;

/* Match the contact in rock against filter. */
static int contact_filter_match(void *vf, void *rock)
{
    contact_filter *f = (contact_filter *) vf;
    contact_filter_rock *cfrock = (contact_filter_rock*) rock;
    json_t *contact = cfrock->contact;
    struct carddav_data *cdata = cfrock->cdata;
    struct carddav_db *db = cfrock->carddavdb;

    /* isFlagged */
    if (JNOTNULL(f->isFlagged)) {
        json_t *isFlagged = json_object_get(contact, "isFlagged");
        if (f->isFlagged != isFlagged) {
            return 0;
        }
    }
    /* text */
    if (f->text && !jmap_match_jsonprop(contact, NULL, f->text)) {
        return 0;
    }
    /*  prefix */
    if (f->prefix && !jmap_match_jsonprop(contact, "prefix", f->prefix)) {
        return 0;
    }
    /* firstName */
    if (f->firstName &&
        !jmap_match_jsonprop(contact, "firstName", f->firstName)) {
        return 0;
    }
    /* lastName */
    if (f->lastName && !jmap_match_jsonprop(contact, "lastName", f->lastName)) {
        return 0;
    }
    /*  suffix */
    if (f->suffix && !jmap_match_jsonprop(contact, "suffix", f->suffix)) {
        return 0;
    }
    /*  nickname */
    if (f->nickname && !jmap_match_jsonprop(contact, "nickname", f->nickname)) {
        return 0;
    }
    /*  company */
    if (f->company && !jmap_match_jsonprop(contact, "company", f->company)) {
        return 0;
    }
    /*  department */
    if (f->department &&
        !jmap_match_jsonprop(contact, "department", f->department)) {
        return 0;
    }
    /*  jobTitle */
    if (f->jobTitle && !jmap_match_jsonprop(contact, "jobTitle", f->jobTitle)) {
        return 0;
    }
    /* email */
    if (f->email && json_object_get(contact, "emails")) {
        size_t i;
        json_t *email;
        int m = 0;
        json_array_foreach(json_object_get(contact, "emails"), i, email) {
            m = jmap_match_jsonprop(email, NULL, f->email);
            if (m) break;
        }
        if (!m) return 0;
    }
    /*  phone */
    if (f->phone && json_object_get(contact, "phones")) {
        size_t i;
        json_t *phone;
        int m = 0;
        json_array_foreach(json_object_get(contact, "phones"), i, phone) {
            m = jmap_match_jsonprop(phone, NULL, f->phone);
            if (m) break;
        }
        if (!m) return 0;
    }
    /*  online */
    if (f->online && json_object_get(contact, "online")) {
        size_t i;
        json_t *online;
        int m = 0;
        json_array_foreach(json_object_get(contact, "online"), i, online) {
            m = jmap_match_jsonprop(online, NULL, f->online);
            if (m) break;
        }
        if (!m) return 0;
    }
    /* address */
    if (f->address && json_object_get(contact, "addresses")) {
        size_t i;
        json_t *address;
        int m = 0;
        json_array_foreach(json_object_get(contact, "addresses"), i, address) {
            m = jmap_match_jsonprop(address, NULL, f->address);
            if (m) break;
        }
        if (!m) return 0;
    }
    /*  notes */
    if (f->notes && !jmap_match_jsonprop(contact, "notes", f->notes)) {
        return 0;
    }
    /* inContactGroup */
    if (f->inContactGroup) {
        /* XXX Calling carddav_db for every contact isn't really efficient. If
         * this turns out to be a performance issue, the carddav_db API might
         * support lookup contacts by group ids. */
        strarray_t *gids = carddav_getuid_groups(db, cdata->vcard_uid);
        if (!gids) {
            syslog(LOG_INFO,
                   "carddav_getuid_groups(%s) returned NULL group array",
                   cdata->vcard_uid);
            return 0;
        }
        int i, m = 0;
        for (i = 0; i < gids->count; i++) {
            if (hash_lookup(strarray_nth(gids, i), f->inContactGroup)) {
                m = 1;
                break;
            }
        }
        strarray_free(gids);
        if (!m) return 0;
    }

    /* All matched. */
    return 1;
}

/* Free the memory allocated by this contact filter. */
static void contact_filter_free(void *vf)
{
    contact_filter *f = (contact_filter*) vf;
    if (f->inContactGroup) {
        free_hash_table(f->inContactGroup, NULL);
        free(f->inContactGroup);
    }
    free(f);
}

/* Parse the JMAP Contact FilterCondition in arg.
 * Report any invalid properties in invalid, prefixed by prefix.
 * Return NULL on error. */
static void *contact_filter_parse(json_t *arg)
{
    contact_filter *f =
        (contact_filter *) xzmalloc(sizeof(struct contact_filter));

    /* inContactGroup */
    json_t *inContactGroup = json_object_get(arg, "inContactGroup");
    if (inContactGroup) {
        f->inContactGroup = xmalloc(sizeof(struct hash_table));
        construct_hash_table(f->inContactGroup,
                             json_array_size(inContactGroup)+1, 0);
        size_t i;
        json_t *val;
        json_array_foreach(inContactGroup, i, val) {
            const char *id;
            if (json_unpack(val, "s", &id) != -1) {
                hash_insert(id, (void*)1, f->inContactGroup);
            }
        }
    }

    /* isFlagged */
    f->isFlagged = json_object_get(arg, "isFlagged");

    /* text */
    if (JNOTNULL(json_object_get(arg, "text"))) {
        jmap_readprop(arg, "text", 0, NULL, "s", &f->text);
    }
    /* prefix */
    if (JNOTNULL(json_object_get(arg, "prefix"))) {
        jmap_readprop(arg, "prefix", 0, NULL, "s", &f->prefix);
    }
    /* firstName */
    if (JNOTNULL(json_object_get(arg, "firstName"))) {
        jmap_readprop(arg, "firstName", 0, NULL, "s", &f->firstName);
    }
    /* lastName */
    if (JNOTNULL(json_object_get(arg, "lastName"))) {
        jmap_readprop(arg, "lastName", 0, NULL, "s", &f->lastName);
    }
    /* suffix */
    if (JNOTNULL(json_object_get(arg, "suffix"))) {
        jmap_readprop(arg, "suffix", 0, NULL, "s", &f->suffix);
    }
    /* nickname */
    if (JNOTNULL(json_object_get(arg, "nickname"))) {
        jmap_readprop(arg, "nickname", 0, NULL, "s", &f->nickname);
    }
    /* company */
    if (JNOTNULL(json_object_get(arg, "company"))) {
        jmap_readprop(arg, "company", 0, NULL, "s", &f->company);
    }
    /* department */
    if (JNOTNULL(json_object_get(arg, "department"))) {
        jmap_readprop(arg, "department", 0, NULL, "s", &f->department);
    }
    /* jobTitle */
    if (JNOTNULL(json_object_get(arg, "jobTitle"))) {
        jmap_readprop(arg, "jobTitle", 0, NULL, "s", &f->jobTitle);
    }
    /* email */
    if (JNOTNULL(json_object_get(arg, "email"))) {
        jmap_readprop(arg, "email", 0, NULL, "s", &f->email);
    }
    /* phone */
    if (JNOTNULL(json_object_get(arg, "phone"))) {
        jmap_readprop(arg, "phone", 0, NULL, "s", &f->phone);
    }
    /* online */
    if (JNOTNULL(json_object_get(arg, "online"))) {
        jmap_readprop(arg, "online", 0, NULL, "s", &f->online);
    }
    /* address */
    if (JNOTNULL(json_object_get(arg, "address"))) {
        jmap_readprop(arg, "address", 0, NULL, "s", &f->address);
    }
    /* notes */
    if (JNOTNULL(json_object_get(arg, "notes"))) {
        jmap_readprop(arg, "notes", 0, NULL, "s", &f->notes);
    }

    return f;
}

static void validatefilter(jmap_req_t *req __attribute__((unused)),
                           struct jmap_parser *parser,
                           json_t *filter,
                           json_t *unsupported __attribute__((unused)),
                           void *rock __attribute__((unused)),
                           json_t **err __attribute__((unused)))
{
    const char *field;
    json_t *arg;

    json_object_foreach(filter, field, arg) {
        if (!strcmp(field, "inContactGroup")) {
            if (!json_is_array(arg)) {
                jmap_parser_invalid(parser, field);
            }
            else {
                jmap_parse_strings(arg, parser, field);
            }
        }
        else if (!strcmp(field, "isFlagged")) {
            if (!json_is_boolean(arg)) {
                jmap_parser_invalid(parser, field);
            }
        }
        else if (!strcmp(field, "text") ||
                 !strcmp(field, "prefix") ||
                 !strcmp(field, "firstName") ||
                 !strcmp(field, "lastName") ||
                 !strcmp(field, "suffix") ||
                 !strcmp(field, "nickname") ||
                 !strcmp(field, "company") ||
                 !strcmp(field, "department") ||
                 !strcmp(field, "jobTitle") ||
                 !strcmp(field, "email") ||
                 !strcmp(field, "phone") ||
                 !strcmp(field, "online") ||
                 !strcmp(field, "address") ||
                 !strcmp(field, "notes")) {
            if (!json_is_string(arg)) {
                jmap_parser_invalid(parser, field);
            }
        }
        else {
            jmap_parser_invalid(parser, field);
        }
    }
}

static int validatecomparator(jmap_req_t *req __attribute__((unused)),
                              struct jmap_comparator *comp,
                              void *rock __attribute__((unused)),
                              json_t **err __attribute__((unused)))
{
    /* Reject any collation */
    if (comp->collation) {
        return 0;
    }
    if (!strcmp(comp->property, "isFlagged") ||
        !strcmp(comp->property, "firstName") ||
        !strcmp(comp->property, "lastName") ||
        !strcmp(comp->property, "nickname") ||
        !strcmp(comp->property, "company")) {
        return 1;
    }
    return 0;
}

struct contactquery_rock {
    jmap_req_t *req;
    struct jmap_query *query;
    jmap_filter *filter;

    struct mailbox *mailbox;
    struct carddav_db *carddavdb;
};

static int getcontactquery_cb(void *rock, struct carddav_data *cdata)
{
    struct contactquery_rock *crock = (struct contactquery_rock*) rock;
    struct index_record record;
    struct contact_filter_rock cfrock;
    json_t *contact = NULL;
    int r = 0;

    if (!cdata->dav.alive || !cdata->dav.rowid || !cdata->dav.imap_uid) {
        return 0;
    }

    /* Ignore anything but contacts. */
    if (cdata->kind != CARDDAV_KIND_CONTACT) {
        return 0;
    }

    if (!jmap_hasrights_byname(crock->req, cdata->dav.mailbox, DACL_READ))
        return 0;

    if (cdata->jmapversion == JMAPCACHE_CONTACTVERSION) {
        json_error_t jerr;
        contact = json_loads(cdata->jmapdata, 0, &jerr);
        if (contact) goto gotvalue;
    }

    /* Open mailbox. */
    if (!crock->mailbox || strcmp(crock->mailbox->name, cdata->dav.mailbox)) {
        mailbox_close(&crock->mailbox);
        r = mailbox_open_irl(cdata->dav.mailbox, &crock->mailbox);
        if (r) goto done;
    }

    /* Load record. */
    r = mailbox_find_index_record(crock->mailbox, cdata->dav.imap_uid, &record);
    if (r) goto done;

    /* Load contact from record. */
    struct vparse_card *vcard = record_to_vcard(crock->mailbox, &record);
    if (!vcard || !vcard->objects) {
        syslog(LOG_ERR, "record_to_vcard failed for record %u:%s",
                cdata->dav.imap_uid, crock->mailbox->name);
        vparse_free_card(vcard);
        r = IMAP_INTERNAL;
        goto done;
    }

    /* Convert the VCARD to a JMAP contact. */
    /* XXX If this conversion turns out to waste too many cycles, then first
     * initialize props with any non-NULL field in filter f or its subconditions.
     */
    contact = jmap_contact_from_vcard(vcard->objects, crock->mailbox, &record);
    vparse_free_card(vcard);

gotvalue:

    /* Match the contact against the filter and update statistics. */
    cfrock.carddavdb = crock->carddavdb;
    cfrock.cdata = cdata;
    cfrock.contact = contact;
    if (crock->filter &&
        !jmap_filter_match(crock->filter, &contact_filter_match, &cfrock)) {
        goto done;
    }
    crock->query->total++;
    if (crock->query->position > (ssize_t) crock->query->total) {
        goto done;
    }
    if (crock->query->limit &&
        crock->query->limit >= json_array_size(crock->query->ids)) {
        goto done;
    }

    /* All done. Add the contact identifier. */
    json_array_append_new(crock->query->ids, json_string(cdata->vcard_uid));

done:
    if (contact) json_decref(contact);
    return r;
}

static int jmap_contact_query(struct jmap_req *req)
{
    struct jmap_parser parser = JMAP_PARSER_INITIALIZER;
    struct jmap_query query;
    struct carddav_db *db;
    jmap_filter *parsed_filter = NULL;
    int r = 0;

    db = carddav_open_userid(req->accountid);
    if (!db) {
        syslog(LOG_ERR,
               "carddav_open_userid failed for user %s", req->accountid);
        r = IMAP_INTERNAL;
        goto done;
    }

    /* Parse request */
    json_t *err = NULL;
    jmap_query_parse(req, &parser, NULL, NULL,
                     validatefilter, NULL,
                     validatecomparator, NULL,
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

    /* Build filter */
    json_t *filter = json_object_get(req->args, "filter");
    if (JNOTNULL(filter)) {
        parsed_filter = jmap_buildfilter(filter, contact_filter_parse);
    }

    /* Inspect every entry in this accounts addressbook mailboxes. */
    struct contactquery_rock rock = {
        req, &query, parsed_filter, NULL, db
    };
    r = carddav_foreach(db, NULL, getcontactquery_cb, &rock);
    if (rock.mailbox) mailbox_close(&rock.mailbox);
    if (r) {
        err = jmap_server_error(r);
        jmap_error(req, err);
        goto done;
    }

    /* Build response */
    json_t *jstate = jmap_getstate(req, MBTYPE_ADDRESSBOOK, /*refresh*/0);
    query.query_state = xstrdup(json_string_value(jstate));
    json_decref(jstate);

    json_t *res = jmap_query_reply(&query);
    jmap_ok(req, res);

done:
    jmap_query_fini(&query);
    jmap_parser_fini(&parser);
    if (parsed_filter) jmap_filter_free(parsed_filter, contact_filter_free);
    if (db) carddav_close(db);
    return 0;
}

static struct vparse_entry *_card_multi(struct vparse_card *card,
                                        const char *name, char sepchar)
{
    struct vparse_entry *res = vparse_get_entry(card, NULL, name);
    if (!res) {
        res = vparse_add_entry(card, NULL, name, NULL);
        res->multivaluesep = sepchar;
        res->v.values = strarray_new();
    }
    return res;
}

static int _emails_to_card(struct vparse_card *card,
                           json_t *arg, json_t *invalid)
{
    vparse_delete_entries(card, NULL, "email");

    int i;
    int size = json_array_size(arg);
    struct buf buf = BUF_INITIALIZER;
    for (i = 0; i < size; i++) {
        json_t *item = json_array_get(arg, i);

        buf_printf(&buf, "emails[%d]", i);
        const char *prefix = buf_cstring(&buf);

        /* Parse properties. */
        const char *type = NULL;
        const char *label = NULL;
        const char *value = NULL;

        jmap_readprop_full(item, prefix, "type", 1, invalid, "s", &type);
        if (type) {
            if (strcmp(type, "personal") && strcmp(type, "work") && strcmp(type, "other")) {
                char *tmp = strconcat(prefix, ".type", NULL);
                json_array_append_new(invalid, json_string(tmp));
                free(tmp);
            }
        }
        jmap_readprop_full(item, prefix, "value", 1, invalid, "s", &value);
        if (JNOTNULL(json_object_get(item, "label"))) {
            jmap_readprop_full(item, prefix, "label", 1, invalid, "s", &label);
        }
        json_t *jisDefault = json_object_get(item, "isDefault");

        /* Bail out for any property errors. */
        if (!type || !value || json_array_size(invalid)) {
            buf_free(&buf);
            return -1;
        }

        /* Update card. */
        struct vparse_entry *entry =
            vparse_add_entry(card, NULL, "EMAIL", value);

        if (!strcmpsafe(type, "personal"))
            type = "home";
        if (strcmpsafe(type, "other"))
            vparse_add_param(entry, "TYPE", type);

        if (label)
            vparse_add_param(entry, "LABEL", label);

        if (jisDefault && json_is_true(jisDefault))
            vparse_add_param(entry, "TYPE", "pref");

        buf_reset(&buf);
    }
    buf_free(&buf);
    return 0;
}

static int _phones_to_card(struct vparse_card *card,
                           json_t *arg, json_t *invalid)
{
    vparse_delete_entries(card, NULL, "tel");

    int i;
    int size = json_array_size(arg);
    struct buf buf = BUF_INITIALIZER;
    for (i = 0; i < size; i++) {
        json_t *item = json_array_get(arg, i);

        buf_printf(&buf, "phones[%d]", i);
        const char *prefix = buf_cstring(&buf);

        /* Parse properties. */
        const char *type = NULL;
        const char *label = NULL;
        const char *value = NULL;

        jmap_readprop_full(item, prefix, "type", 1, invalid, "s", &type);
        if (type) {
            if (strcmp(type, "home") && strcmp(type, "work") && strcmp(type, "mobile") &&
                strcmp(type, "fax") && strcmp(type, "pager") && strcmp(type, "other")) {
                char *tmp = strconcat(prefix, ".type", NULL);
                json_array_append_new(invalid, json_string(tmp));
                free(tmp);
            }
        }
        jmap_readprop_full(item, prefix, "value", 1, invalid, "s", &value);
        if (JNOTNULL(json_object_get(item, "label"))) {
            jmap_readprop_full(item, prefix, "label", 1, invalid, "s", &label);
        }

        /* Bail out for any property errors. */
        if (!type || !value || json_array_size(invalid)) {
            buf_free(&buf);
            return -1;
        }

        /* Update card. */
        struct vparse_entry *entry = vparse_add_entry(card, NULL, "TEL", value);

        if (!strcmp(type, "mobile"))
            vparse_add_param(entry, "TYPE", "CELL");
        else if (strcmp(type, "other"))
            vparse_add_param(entry, "TYPE", type);

        if (label)
            vparse_add_param(entry, "LABEL", label);

        buf_reset(&buf);
    }
    buf_free(&buf);
    return 0;
}

static int _is_im(const char *type)
{
    /* add new services here */
    if (!strcasecmp(type, "aim")) return 1;
    if (!strcasecmp(type, "facebook")) return 1;
    if (!strcasecmp(type, "gadugadu")) return 1;
    if (!strcasecmp(type, "googletalk")) return 1;
    if (!strcasecmp(type, "icq")) return 1;
    if (!strcasecmp(type, "jabber")) return 1;
    if (!strcasecmp(type, "msn")) return 1;
    if (!strcasecmp(type, "qq")) return 1;
    if (!strcasecmp(type, "skype")) return 1;
    if (!strcasecmp(type, "twitter")) return 1;
    if (!strcasecmp(type, "yahoo")) return 1;

    return 0;
}

static int _online_to_card(struct vparse_card *card,
                           json_t *arg, json_t *invalid)
{
    vparse_delete_entries(card, NULL, "url");
    vparse_delete_entries(card, NULL, "impp");
    vparse_delete_entries(card, NULL, "x-social-profile");
    vparse_delete_entries(card, NULL, "x-fm-online-other");

    int i;
    int size = json_array_size(arg);
    struct buf buf = BUF_INITIALIZER;
    for (i = 0; i < size; i++) {
        json_t *item = json_array_get(arg, i);

        buf_printf(&buf, "online[%d]", i);
        const char *prefix = buf_cstring(&buf);

        /* Parse properties. */
        const char *type = NULL;
        const char *label = NULL;
        const char *value = NULL;

        jmap_readprop_full(item, prefix, "type", 1, invalid, "s", &type);
        if (type) {
            if (strcmp(type, "uri") && strcmp(type, "username") && strcmp(type, "other")) {
                char *tmp = strconcat(prefix, ".type", NULL);
                json_array_append_new(invalid, json_string(tmp));
                free(tmp);
            }
        }
        jmap_readprop_full(item, prefix, "value", 1, invalid, "s", &value);
        if (JNOTNULL(json_object_get(item, "label"))) {
            jmap_readprop_full(item, prefix, "label", 1, invalid, "s", &label);
        }

        /* Bail out for any property errors. */
        if (!type || !value || json_array_size(invalid)) {
            buf_free(&buf);
            return -1;
        }

        /* Update card. */
        if (!strcmp(type, "uri")) {
            struct vparse_entry *entry =
                vparse_add_entry(card, NULL, "URL", value);
            if (label)
                vparse_add_param(entry, "LABEL", label);
        }
        else if (!strcmp(type, "username")) {
            if (label && _is_im(label)) {
                struct vparse_entry *entry =
                    vparse_add_entry(card, NULL, "IMPP", value);
                vparse_add_param(entry, "X-SERVICE-TYPE", label);
            }
            else {
                struct vparse_entry *entry =
                    vparse_add_entry(card, NULL, "X-SOCIAL-PROFILE", ""); // XXX - URL calculated, ick
                if (label)
                    vparse_add_param(entry, "TYPE", label);
                vparse_add_param(entry, "X-USER", value);
            }
        }
        else if (!strcmp(type, "other")) {
            struct vparse_entry *entry =
                vparse_add_entry(card, NULL, "X-FM-ONLINE-OTHER", value);
            if (label)
                vparse_add_param(entry, "LABEL", label);
        }
    }
    buf_free(&buf);
    return 0;
}

static int _addresses_to_card(struct vparse_card *card,
                              json_t *arg, json_t *invalid)
{
    vparse_delete_entries(card, NULL, "adr");

    int i;
    int size = json_array_size(arg);
    struct buf buf = BUF_INITIALIZER;
    for (i = 0; i < size; i++) {
        json_t *item = json_array_get(arg, i);

        buf_printf(&buf, "addresses[%d]", i);
        const char *prefix = buf_cstring(&buf);

        /* Parse properties. */
        const char *type = NULL;
        const char *label = NULL;
        const char *street = NULL;
        const char *locality = NULL;
        const char *region = NULL;
        const char *postcode = NULL;
        const char *country = NULL;
        int pe; /* parse error */

        /* Mandatory */
        pe = jmap_readprop_full(item, prefix, "type", 1, invalid, "s", &type);
        if (type) {
            if (strcmp(type, "home") && strcmp(type, "work") && strcmp(type, "billing") &&
                strcmp(type, "postal") && strcmp(type, "other")) {
                char *tmp = strconcat(prefix, ".type", NULL);
                json_array_append_new(invalid, json_string(tmp));
                free(tmp);
            }
        }
        pe = jmap_readprop_full(item, prefix, "street", 1, invalid, "s", &street);
        pe = jmap_readprop_full(item, prefix, "locality", 1, invalid, "s", &locality);
        pe = jmap_readprop_full(item, prefix, "region", 1, invalid, "s", &region);
        pe = jmap_readprop_full(item, prefix, "postcode", 1, invalid, "s", &postcode);
        pe = jmap_readprop_full(item, prefix, "country", 1, invalid, "s", &country);

        /* Optional */
        if (JNOTNULL(json_object_get(item, "label"))) {
            pe = jmap_readprop_full(item, prefix, "label", 0, invalid, "s", &label);
        }

        /* Bail out for any property errors. */
        if (!type || !street || !locality ||
            !region || !postcode || !country || pe < 0) {
            buf_free(&buf);
            return -1;
        }

        /* Update card. */
        struct vparse_entry *entry = vparse_add_entry(card, NULL, "ADR", NULL);

        if (strcmpsafe(type, "other"))
            vparse_add_param(entry, "TYPE", type);

        if (label)
            vparse_add_param(entry, "LABEL", label);

        entry->multivaluesep = ';';
        entry->v.values = strarray_new();
        strarray_append(entry->v.values, ""); // PO Box
        strarray_append(entry->v.values, ""); // Extended Address
        strarray_append(entry->v.values, street);
        strarray_append(entry->v.values, locality);
        strarray_append(entry->v.values, region);
        strarray_append(entry->v.values, postcode);
        strarray_append(entry->v.values, country);

        buf_reset(&buf);
    }

    buf_free(&buf);
    return 0;
}

static int _date_to_card(struct vparse_card *card,
                         const char *key, json_t *jval)
{
    if (!jval)
        return -1;
    const char *val = json_string_value(jval);
    if (!val)
        return -1;

    /* JMAP dates are always YYYY-MM-DD */
    unsigned y, m, d;
    if (_parse_date(val, &y, &m, &d, 1))
        return -1;

    /* range checks. month and day just get basic sanity checks because we're
     * not carrying a full calendar implementation here. JMAP says zero is valid
     * so we'll allow that and deal with it later on */
    if (m > 12 || d > 31)
        return -1;

    /* all years are valid in JMAP, but ISO8601 only allows Gregorian ie >= 1583.
     * moreover, iOS uses 1604 as a magic number for "unknown", so we'll say 1605
     * is the minimum */
    if (y > 0 && y < 1605)
        return -1;

    /* everything in range. now comes the fun bit. vCard v3 says BDAY is
     * YYYY-MM-DD. It doesn't reference ISO8601 (vCard v4 does) and make no
     * provision for "unknown" date components, so there's no way to represent
     * JMAP's "unknown" values. Apple worked around this for year by using the
     * year 1604 and adding the parameter X-APPLE-OMIT-YEAR=1604 (value
     * apparently ignored). We will use a similar hack for month and day so we
     * can convert it back into a JMAP date */

    int no_year = 0;
    if (y == 0) {
        no_year = 1;
        y = 1604;
    }

    int no_month = 0;
    if (m == 0) {
        no_month = 1;
        m = 1;
    }

    int no_day = 0;
    if (d == 0) {
        no_day = 1;
        d = 1;
    }

    vparse_delete_entries(card, NULL, key);

    /* no values, we're done! */
    if (no_year && no_month && no_day)
        return 0;

    /* build the value */
    static char buf[11];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
    struct vparse_entry *entry = vparse_add_entry(card, NULL, key, buf);

    /* set all the round-trip flags, sigh */
    if (no_year)
        vparse_add_param(entry, "X-APPLE-OMIT-YEAR", "1604");
    if (no_month)
        vparse_add_param(entry, "X-FM-NO-MONTH", "1");
    if (no_day)
        vparse_add_param(entry, "X-FM-NO-DAY", "1");

    return 0;
}

static int _kv_to_card(struct vparse_card *card, const char *key, json_t *jval)
{
    if (!jval)
        return -1;
    const char *val = json_string_value(jval);
    if (!val)
        return -1;
    vparse_replace_entry(card, NULL, key, val);
    return 0;
}

static int _blob_to_card(struct jmap_req *req,
                         struct vparse_card *card, const char *key, json_t *file)
{
    struct buf blob_buf = BUF_INITIALIZER;
    msgrecord_t *mr = NULL;
    struct mailbox *mbox = NULL;
    struct body *body = NULL;
    const struct body *part = NULL;
    const char *blobid = NULL;
    const char *accountid = NULL;
    char *encbuf = NULL;
    char *decbuf = NULL;
    json_t *val;
    int r = -1;

    if (!file) goto done;

    /* Extract blobId */
    val = json_object_get(file, "blobId");
    if (val) blobid = json_string_value(val);
    if (!blobid) goto done;

    /* Find body part containing blob */
    if (!strcmp(req->method, "Contact/copy")) {
        accountid = json_string_value(json_object_get(req->args, "fromAccountId"));
    }
    r = jmap_findblob(req, accountid, blobid,
                      &mbox, &mr, &body, &part, &blob_buf);
    if (r) goto done;

    /* Fetch blob contents and decode */
    const char *base = buf_base(&blob_buf);
    size_t len = buf_len(&blob_buf);

    if (part) {
        /* Map into body part */
        base += part->content_offset;
        len = part->content_size;

        /* Determine encoding */
        int encoding = part->charset_enc & 0xff;
        base = charset_decode_mimebody(base, len, encoding, &decbuf, &len);
    }

    /* Pre-flight base64 encoder to determine length */
    size_t len64 = 0;
    charset_encode_mimebody(NULL, len, NULL, &len64, NULL, 0 /* no wrap */);

    /* Now encode the blob */
    encbuf = xmalloc(len64+1);
    charset_encode_mimebody(base, len, encbuf, &len64, NULL, 0 /* no wrap */);
    encbuf[len64] = '\0';
    base = encbuf;

    /* (Re)write vCard property */
    vparse_delete_entries(card, NULL, key);

    struct vparse_entry *entry = vparse_add_entry(card, NULL, key, base);

    vparse_add_param(entry, "ENCODING", "b");

    val = json_object_get(file, "type");
    if (JNOTNULL(val)) {
        r = -1;
        const char *type = json_string_value(val);
        if (!type) goto done;
        char *subtype = xstrdupnull(strchr(type, '/'));
        if (!subtype) goto done;

        vparse_add_param(entry, "TYPE", ucase(subtype+1));
        free(subtype);
    }

    r = 0;

  done:
    free(decbuf);
    free(encbuf);
    if (body) {
        message_free_body(body);
        free(body);
    }
    msgrecord_unref(&mr);
    jmap_closembox(req, &mbox);
    buf_free(&blob_buf);

    return r;
}

static void _make_fn(struct vparse_card *card)
{
    struct vparse_entry *n = vparse_get_entry(card, NULL, "N");
    strarray_t *name = strarray_new();
    const char *v;

    if (n) {
        v = strarray_safenth(n->v.values, 3); // prefix
        if (*v) strarray_append(name, v);

        v = strarray_safenth(n->v.values, 1); // first
        if (*v) strarray_append(name, v);

        v = strarray_safenth(n->v.values, 2); // middle
        if (*v) strarray_append(name, v);

        v = strarray_safenth(n->v.values, 0); // last
        if (*v) strarray_append(name, v);

        v = strarray_safenth(n->v.values, 4); // suffix
        if (*v) strarray_append(name, v);
    }

    if (!strarray_size(name)) {
        v = vparse_stringval(card, "NICKNAME");
        if (v && v[0]) strarray_append(name, v);
    }

    char *fn = NULL;
    if (strarray_size(name))
        fn = strarray_join(name, " ");
    else
        fn = xstrdup(" ");

    strarray_free(name);
    vparse_replace_entry(card, NULL, "FN", fn);
    free(fn);
}

static int _json_to_card(struct jmap_req *req,
                         struct carddav_data *cdata,
                         struct vparse_card *card,
                         json_t *arg, strarray_t *flags,
                         struct entryattlist **annotsp,
                         json_t *invalid)
{
    const char *key;
    json_t *jval;
    struct vparse_entry *n = vparse_get_entry(card, NULL, "N");
    int name_is_dirty = 0;
    int has_noncontent = 0;
    int record_is_dirty = 0;

    /* we'll be updating you later anyway... create early so that it's
     * at the top of the card */
    if (!n) {
        /* _card_multi repeats some work, but we don't care */
        n = _card_multi(card, "N", ';');
        record_is_dirty = 1;
    }

    if (!vparse_get_entry(card, NULL, "FN")) {
        /* adding first to get position near the top */
        vparse_add_entry(card, NULL, "FN", "No Name");
        name_is_dirty = 1;
    }

    json_object_foreach(arg, key, jval) {
        if (cdata) {
            if (!strcmp(key, "id")) {
                if (strcmpnull(cdata->vcard_uid, json_string_value(jval))) {
                    json_array_append_new(invalid, json_string("id"));
                }
                continue;
            }
            if (!strcmp(key, "uid")) {
                if (strcmpnull(cdata->vcard_uid, json_string_value(jval))) {
                    json_array_append_new(invalid, json_string("uid"));
                }
                continue;
            }
            else if (!strcmp(key, "x-href")) {
                char *xhref = jmap_xhref(cdata->dav.mailbox, cdata->dav.resource);
                if (strcmpnull(json_string_value(jval), xhref)) {
                    json_array_append_new(invalid, json_string("x-href"));
                }
                free(xhref);
                continue;
            }
            else if (!strcmp(key, "x-hasPhoto")) {
                if ((vparse_stringval(card, "photo") && !json_is_true(jval)) ||
                    !json_is_false(jval)) {
                    json_array_append_new(invalid, json_string("x-hasPhoto"));
                }
                continue;
            }
        }

        if (!strcmp(key, "uid")) {
            if (!json_is_string(jval)) {
                json_array_append_new(invalid, json_string("uid"));
            }
        }
        else if (!strcmp(key, "isFlagged")) {
            has_noncontent = 1;
            if (json_is_true(jval)) {
                strarray_add_case(flags, "\\Flagged");
            } else if (json_is_false(jval)) {
                strarray_remove_all_case(flags, "\\Flagged");
            } else {
                json_array_append_new(invalid, json_string("isFlagged"));
            }
        }
        else if (!strcmp(key, "importance")) {
            has_noncontent = 1;
            double dval = json_number_value(jval);
            const char *ns = DAV_ANNOT_NS "<" XML_NS_CYRUS ">importance";
            const char *attrib = "value.shared";
            struct buf buf = BUF_INITIALIZER;
            if (dval) {
                buf_printf(&buf, "%e", dval);
            }
            setentryatt(annotsp, ns, attrib, &buf);
            buf_free(&buf);
        }
        else if (!strcmp(key, "avatar")) {
            if (!json_is_null(jval)) {
                int r = _blob_to_card(req, card, "PHOTO", jval);
                if (r) {
                    json_array_append_new(invalid, json_string("avatar"));
                    continue;
                }
                record_is_dirty = 1;
            }
        }
        else if (!strcmp(key, "prefix")) {
            const char *val = json_string_value(jval);
            if (!val) {
                json_array_append_new(invalid, json_string("prefix"));
                continue;
            }

            name_is_dirty = 1;
            strarray_set(n->v.values, 3, val);
        }
        else if (!strcmp(key, "firstName")) {
            const char *val = json_string_value(jval);
            if (!val) {
                json_array_append_new(invalid, json_string("firstName"));
                continue;
            }
            name_is_dirty = 1;
            /* JMAP doesn't have a separate field for Middle (aka "Additional
             * Names"), so any extra names are probably in firstName, and we
             * should split them out. See reverse of this in getcontacts_cb */
            const char *middle = strchr(val, ' ');
            if (middle) {
                /* multiple worlds, first to First, rest to Middle */
                strarray_setm(n->v.values, 1, xstrndup(val, middle-val));
                strarray_set(n->v.values, 2, ++middle);
            }
            else {
                /* single word, set First, clear Middle */
                strarray_set(n->v.values, 1, val);
                strarray_set(n->v.values, 2, "");
            }
        }
        else if (!strcmp(key, "lastName")) {
            const char *val = json_string_value(jval);
            if (!val) {
                json_array_append_new(invalid, json_string("lastName"));
                continue;
            }
            name_is_dirty = 1;
            strarray_set(n->v.values, 0, val);
        }
        else if (!strcmp(key, "suffix")) {
            const char *val = json_string_value(jval);
            if (!val) {
                json_array_append_new(invalid, json_string("suffix"));
                continue;
            }
            name_is_dirty = 1;
            strarray_set(n->v.values, 4, val);
        }
        else if (!strcmp(key, "nickname")) {
            const char *val = json_string_value(jval);
            if (!val) {
                json_array_append_new(invalid, json_string("nickname"));
                continue;
            }
            struct vparse_entry *nick = _card_multi(card, "NICKNAME", ',');
            strarray_truncate(nick->v.values, 0);
            if (*val) strarray_set(nick->v.values, 0, val);
            record_is_dirty = 1;
        }
        else if (!strcmp(key, "birthday")) {
            int r = _date_to_card(card, "BDAY", jval);
            if (r) {
                json_array_append_new(invalid, json_string("birthday"));
                continue;
            }
            record_is_dirty = 1;
        }
        else if (!strcmp(key, "anniversary")) {
            int r = _date_to_card(card, "ANNIVERSARY", jval);
            if (r) {
                json_array_append_new(invalid, json_string("anniversary"));
                continue;
            }
            record_is_dirty = 1;
        }
        else if (!strcmp(key, "jobTitle")) {
            int r = _kv_to_card(card, "TITLE", jval);
            if (r) {
                json_array_append_new(invalid, json_string("jobTitle"));
                continue;
            }
            record_is_dirty = 1;
        }
        else if (!strcmp(key, "company")) {
            const char *val = json_string_value(jval);
            if (!val) {
                json_array_append_new(invalid, json_string("company"));
                continue;
            }
            struct vparse_entry *org = _card_multi(card, "ORG", ';');
            strarray_set(org->v.values, 0, val);
            record_is_dirty = 1;
        }
        else if (!strcmp(key, "department")) {
            const char *val = json_string_value(jval);
            if (!val) {
                json_array_append_new(invalid, json_string("department"));
                continue;
            }
            struct vparse_entry *org = _card_multi(card, "ORG", ';');
            strarray_set(org->v.values, 1, val);
            record_is_dirty = 1;
        }
        else if (!strcmp(key, "emails")) {
            int r = _emails_to_card(card, jval, invalid);
            if (r) continue;
            record_is_dirty = 1;
        }
        else if (!strcmp(key, "phones")) {
            int r = _phones_to_card(card, jval, invalid);
            if (r) continue;
            record_is_dirty = 1;
        }
        else if (!strcmp(key, "online")) {
            int r = _online_to_card(card, jval, invalid);
            if (r) continue;
            record_is_dirty = 1;
        }
        else if (!strcmp(key, "addresses")) {
            int r = _addresses_to_card(card, jval, invalid);
            if (r) continue;
            record_is_dirty = 1;
        }
        else if (!strcmp(key, "notes")) {
            int r = _kv_to_card(card, "NOTE", jval);
            if (r) {
                json_array_append_new(invalid, json_string("notes"));
                continue;
            }
            record_is_dirty = 1;
        }
        else {
            json_array_append_new(invalid, json_string(key));
        }
    }

    if (json_array_size(invalid)) return -1;

    if (name_is_dirty) {
        _make_fn(card);
        record_is_dirty = 1;
    }

    if (!record_is_dirty && has_noncontent)
        return HTTP_NO_CONTENT;  /* no content */

    return 0;
}

static int required_set_rights(json_t *props)
{
    int needrights = 0;
    const char *name;
    json_t *val;

    json_object_foreach(props, name, val) {
        if (!strcmp(name, "id") ||
            !strcmp(name, "x-href") ||
            !strcmp(name, "x-hasPhoto") ||
            !strcmp(name, "addressbookId")) {
            /* immutable */
        }
        else if (!strcmp(name, "importance")) {
            /* writing shared meta-data (per RFC 5257) */
            needrights |= DACL_PROPRSRC;
        }
        else if (!strcmp(name, "isFlagged")) {
            /* writing private meta-data */
            needrights |= DACL_PROPCOL;
        }
        else {
            /* writing vCard data */
            needrights |= DACL_WRITECONT | DACL_RMRSRC;
        }
    }

    return needrights;
}

static int _contact_set_create(jmap_req_t *req, unsigned kind,
                               json_t *jcard, struct carddav_data *cdata,
                               struct mailbox **mailbox, char **uidptr,
                               json_t *invalid)
{
    struct entryattlist *annots = NULL;
    strarray_t *flags = NULL;
    struct vparse_card *card = NULL;
    char *uid = NULL;
    int r = 0;
    char *resourcename = NULL;

    *uidptr = NULL;

    if ((uid = (char *) json_string_value(json_object_get(jcard, "uid")))) {
        /* Use custom vCard UID from request object */
        uid = xstrdup(uid);
    }  else {
        /* Create a vCard UID */
        uid = xstrdup(makeuuid());
    }

    /* Determine mailbox and resource name of card.
     * We attempt to reuse the UID as DAV resource name; but
     * only if it looks like a reasonable URL path segment. */
    struct buf buf = BUF_INITIALIZER;
    const char *p;
    for (p = uid; *p; p++) {
        if ((*p >= '0' && *p <= '9') ||
            (*p >= 'a' && *p <= 'z') ||
            (*p >= 'A' && *p <= 'Z') ||
            (p > uid &&
                (*p == '@' || *p == '.' ||
                 *p == '_' || *p == '-'))) {
            continue;
        }
        break;
    }
    if (*p == '\0' && p - uid >= 16 && p - uid <= 200) {
        buf_setcstr(&buf, uid);
    } else {
        buf_setcstr(&buf, makeuuid());
    }
    buf_appendcstr(&buf, ".vcf");
    resourcename = buf_newcstring(&buf);
    buf_free(&buf);

    const char *addressbookId = "Default";
    json_t *abookid = json_object_get(jcard, "addressbookId");
    if (abookid && json_string_value(abookid)) {
        /* XXX - invalid arguments */
        addressbookId = json_string_value(abookid);
    }
    char *mboxname = mboxname_abook(req->accountid, addressbookId);
    json_object_del(jcard, "addressbookId");
    addressbookId = NULL;

    int needrights = required_set_rights(jcard);

    /* Check permissions. */
    if (!jmap_hasrights_byname(req, mboxname, needrights)) {
        json_array_append_new(invalid, json_string("addressbookId"));
        goto done;
    }

    card = vparse_new_card("VCARD");
    vparse_add_entry(card, NULL, "PRODID", _prodid);
    vparse_add_entry(card, NULL, "VERSION", "3.0");
    vparse_add_entry(card, NULL, "UID", uid);

    /* we need to create and append a record */
    if (!*mailbox || strcmp((*mailbox)->name, mboxname)) {
        jmap_closembox(req, mailbox);
        r = jmap_openmbox(req, mboxname, mailbox, 1);
        if (r == IMAP_MAILBOX_NONEXISTENT) {
            json_array_append_new(invalid, json_string("addressbookId"));
            r = 0;
            goto done;
        }
        else if (r) goto done;
    }

    const char *name = NULL;
    const char *logfmt = NULL;

    if (kind == CARDDAV_KIND_GROUP) {
        jmap_readprop(jcard, "name", 1, invalid, "s", &name);

        vparse_add_entry(card, NULL, "N", name);
        vparse_add_entry(card, NULL, "FN", name);
        vparse_add_entry(card, NULL, "X-ADDRESSBOOKSERVER-KIND", "group");

        /* it's legal to create an empty group */
        json_t *members = json_object_get(jcard, "contactIds");
        if (members) {
            _add_group_entries(req, card, members, invalid);
        }

        /* it's legal to create an empty group */
        json_t *others = json_object_get(jcard, "otherAccountContactIds");
        if (others) {
            _add_othergroup_entries(req, card, others, invalid);
        }

        logfmt = "jmap: create group %s/%s/%s (%s)";
    }
    else {
        flags = strarray_new();
        r = _json_to_card(req, cdata, card, jcard, flags, &annots, invalid);

        logfmt = "jmap: create contact %s/%s (%s)";
    }

    if (r || json_array_size(invalid)) {
        r = 0;
        goto done;
    }

    syslog(LOG_NOTICE, logfmt, req->accountid, mboxname, uid, name);
    r = carddav_store(*mailbox, card, resourcename, 0, flags, annots,
                      req->accountid, req->authstate, ignorequota);
    if (r && r != HTTP_CREATED && r != HTTP_NO_CONTENT) {
        syslog(LOG_ERR, "carddav_store failed for user %s: %s",
               req->accountid, error_message(r));
        goto done;
    }
    r = 0;
    *uidptr = uid;

done:
    if (!*uidptr) free(uid);
    vparse_free_card(card);
    free(mboxname);
    free(resourcename);
    strarray_free(flags);
    freeentryatts(annots);

    return r;
}

static int jmap_contact_set(struct jmap_req *req)
{
    _contacts_set(req, CARDDAV_KIND_CONTACT);
    return 0;
}

const struct body *jmap_contact_findblob(struct message_guid *content_guid,
                                         const char *part_id,
                                         struct mailbox *mbox,
                                         msgrecord_t *mr,
                                         struct buf *blob)
{
    const struct body *ret = NULL;
    struct index_record record;
    struct vparse_card *vcard;
    const char *proppath = strstr(part_id, "/VCARD#");

    if (!proppath) return NULL;

    msgrecord_get_index_record(mr, &record);
    vcard = record_to_vcard(mbox, &record);

    if (vcard) {
        static struct body subpart;
        struct buf propval = BUF_INITIALIZER;
        char *type = NULL;
        struct vparse_entry *entry =
            vparse_get_entry(vcard->objects, NULL, proppath+7);

        memset(&subpart, 0, sizeof(struct body));

        if (entry && vcard_prop_decode_value(entry, &propval,
                                             &type, &subpart.content_guid) &&
            !message_guid_cmp(content_guid, &subpart.content_guid)) {
            /* Build a body part for the property */
            subpart.charset_enc = ENCODING_NONE;
            subpart.encoding = "BINARY";
            subpart.header_offset = 0;
            subpart.content_size = buf_len(&propval);
            ret = &subpart;

            buf_reset(blob);
            buf_printf(blob, "User-Agent: Cyrus-JMAP/%s\r\n", CYRUS_VERSION);

            struct buf from = BUF_INITIALIZER;
            if (strchr(httpd_userid, '@')) {
                /* XXX  This needs to be done via an LDAP/DB lookup */
                buf_printf(&from, "<%s>", httpd_userid);
            }
            else {
                buf_printf(&from, "<%s@%s>", httpd_userid, config_servername);
            }
            
            char *mimehdr = charset_encode_mimeheader(buf_cstring(&from),
                                                      buf_len(&from), 0);

            buf_printf(blob, "From: %s\r\n", mimehdr);
            free(mimehdr);
            buf_free(&from);

            char datestr[80];
            time_to_rfc5322(time(NULL), datestr, sizeof(datestr));
            buf_printf(blob, "Date: %s\r\n", datestr);

            if (!type) type = xstrdup("application/octet-stream");
            buf_printf(blob, "Content-Type: %s\r\n", type);

            buf_printf(blob, "Content-Transfer-Encoding: %s\r\n",
                       subpart.encoding);

            buf_printf(blob, "Content-Length: %u\r\n", subpart.content_size);

            buf_appendcstr(blob, "MIME-Version: 1.0\r\n\r\n");

            subpart.content_offset = subpart.header_size = buf_len(blob);

            buf_append(blob, &propval);
            buf_free(&propval);
        }
        else buf_free(&propval);

        free(type);
        vparse_free_card(vcard);
    }

    return ret;
}

static void _contact_copy(jmap_req_t *req,
                          json_t *jcard,
                          struct carddav_db *src_db,
                          json_t **new_card,
                          json_t **set_err)
{
    struct jmap_parser myparser = JMAP_PARSER_INITIALIZER;
    struct vparse_card *vcard = NULL;
    json_t *dst_card = NULL;
    struct mailbox *src_mbox = NULL;
    struct mailbox *dst_mbox = NULL;
    int r = 0;

    /* Read mandatory properties */
    const char *src_id = json_string_value(json_object_get(jcard, "id"));
    if (!src_id) {
        jmap_parser_invalid(&myparser, "id");
    }
    if (json_array_size(myparser.invalid)) {
        *set_err = json_pack("{s:s s:O}", "type", "invalidProperties",
                                          "properties", myparser.invalid);
        goto done;
    }

    /* Lookup event */
    struct carddav_data *cdata = NULL;
    r = carddav_lookup_uid(src_db, src_id, &cdata);
    if (r && r != CYRUSDB_NOTFOUND) {
        syslog(LOG_ERR, "carddav_lookup_uid(%s) failed: %s",
               src_id, error_message(r));
        goto done;
    }
    if (r == CYRUSDB_NOTFOUND || !cdata->dav.alive ||
        !cdata->dav.rowid || !cdata->dav.imap_uid) {
        *set_err = json_pack("{s:s}", "type", "notFound");
        goto done;
    }
    if (!jmap_hasrights_byname(req, cdata->dav.mailbox, DACL_READ)) {
        *set_err = json_pack("{s:s}", "type", "notFound");
        goto done;
    }

    /* Read source event */
    r = jmap_openmbox(req, cdata->dav.mailbox, &src_mbox, /*rw*/0);
    if (r) goto done;
    struct index_record record;
    r = mailbox_find_index_record(src_mbox, cdata->dav.imap_uid, &record);
    if (!r) vcard = record_to_vcard(src_mbox, &record);
    if (!vcard || !vcard->objects) {
        syslog(LOG_ERR, "contact_copy: can't convert %s to JMAP", src_id);
        r = IMAP_INTERNAL;
        goto done;
    }

    /* Patch JMAP event */
    json_t *src_card = jmap_contact_from_vcard(vcard->objects, src_mbox, &record);
    if (src_card) {
        json_object_del(src_card, "x-href");  // immutable and WILL change
        json_object_del(src_card, "x-hasPhoto");  // immutable and WILL change
        dst_card = jmap_patchobject_apply(src_card, jcard, NULL);
    }
    json_decref(src_card);

    /* Create vcard */
    json_t *invalid = json_array();
    char *dst_uid = NULL;
    r = _contact_set_create(req, CARDDAV_KIND_CONTACT, dst_card,
                           cdata, &dst_mbox, &dst_uid, invalid);
    if (r || json_array_size(invalid)) {
        if (!r) {
            *set_err = json_pack("{s:s s:o}", "type", "invalidProperties",
                                              "properties", invalid);
        }
        goto done;
    }
    json_decref(invalid);
    *new_card = json_pack("{s:s}", "id", dst_uid);
    free(dst_uid);

done:
    if (r && *set_err == NULL) {
        if (r == CYRUSDB_NOTFOUND)
            *set_err = json_pack("{s:s}", "type", "notFound");
        else
            *set_err = jmap_server_error(r);
        return;
    }
    jmap_closembox(req, &dst_mbox);
    jmap_closembox(req, &src_mbox);
    if (vcard) vparse_free_card(vcard);
    json_decref(dst_card);
    jmap_parser_fini(&myparser);
}

static int jmap_contact_copy(struct jmap_req *req)
{
    struct jmap_parser parser = JMAP_PARSER_INITIALIZER;
    struct jmap_copy copy;
    json_t *err = NULL;
    struct carddav_db *src_db = NULL;
    json_t *destroy_cards = json_array();

    /* Parse request */
    jmap_copy_parse(req, &parser, NULL, NULL, &copy, &err);
    if (err) {
        jmap_error(req, err);
        goto done;
    }

    src_db = carddav_open_userid(copy.from_account_id);
    if (!src_db) {
        jmap_error(req, json_pack("{s:s}", "type", "fromAccountNotFound"));
        goto done;
    }

    /* Process request */
    const char *creation_id;
    json_t *jcard;
    json_object_foreach(copy.create, creation_id, jcard) {
        /* Copy event */
        json_t *set_err = NULL;
        json_t *new_card = NULL;

        _contact_copy(req, jcard, src_db, /*dst_db,*/ &new_card, &set_err);
        if (set_err) {
            json_object_set_new(copy.not_created, creation_id, set_err);
            continue;
        }

        // extract the id for later deletion
        json_array_append(destroy_cards, json_object_get(jcard, "id"));

        /* Report event as created */
        json_object_set_new(copy.created, creation_id, new_card);
        const char *card_id = json_string_value(json_object_get(new_card, "id"));
        jmap_add_id(req, creation_id, card_id);
    }

    /* Build response */
    jmap_ok(req, jmap_copy_reply(&copy));

    /* Destroy originals, if requested */
    if (copy.on_success_destroy_original && json_array_size(destroy_cards)) {
        json_t *subargs = json_object();
        json_object_set(subargs, "destroy", destroy_cards);
        json_object_set_new(subargs, "accountId", json_string(copy.from_account_id));
        jmap_add_subreq(req, "Contact/set", subargs, NULL);
    }

done:
    json_decref(destroy_cards);
    if (src_db) carddav_close(src_db);
    jmap_parser_fini(&parser);
    jmap_copy_fini(&copy);
    return 0;
}
