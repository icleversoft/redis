/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "redis.h"

#include <signal.h>
#include <ctype.h>

void SlotToKeyAdd(robj *key);
void SlotToKeyDel(robj *key);

/*-----------------------------------------------------------------------------
 * C-level DB API
 *----------------------------------------------------------------------------*/

robj *lookupKey(redisDb *db, robj *key) {
  dictEntry *de = dictFind(db->dict,key->ptr);
  if (de) {
    robj *val = dictGetVal(de);

    /* Update the access time for the ageing algorithm.
     * Don't do it if we have a saving child, as this will trigger
     * a copy on write madness. */
    if (server.rdb_child_pid == -1 && server.aof_child_pid == -1)
      val->lru = server.lruclock;
    return val;
  } else {
    return NULL;
  }
}

robj *lookupKeyRead(redisDb *db, robj *key) {
  robj *val;

  expireIfNeeded(db,key);
  val = lookupKey(db,key);
  if (val == NULL)
    server.stat_keyspace_misses++;
  else
    server.stat_keyspace_hits++;
  return val;
}

robj *lookupKeyWrite(redisDb *db, robj *key) {
  expireIfNeeded(db,key);
  return lookupKey(db,key);
}

robj *lookupKeyReadOrReply(redisClient *c, robj *key, robj *reply) {
  robj *o = lookupKeyRead(c->db, key);
  if (!o) addReply(c,reply);
  return o;
}

robj *lookupKeyWriteOrReply(redisClient *c, robj *key, robj *reply) {
  robj *o = lookupKeyWrite(c->db, key);
  if (!o) addReply(c,reply);
  return o;
}

/* Add the key to the DB. It's up to the caller to increment the reference
 * counter of the value if needed.
 *
 * The program is aborted if the key already exists. */
void dbAdd(redisDb *db, robj *key, robj *val) {
  sds copy = sdsdup(key->ptr);
  int retval = dictAdd(db->dict, copy, val);

  redisAssertWithInfo(NULL,key,retval == REDIS_OK);
}

/* Overwrite an existing key with a new value. Incrementing the reference
 * count of the new value is up to the caller.
 * This function does not modify the expire time of the existing key.
 *
 * The program is aborted if the key was not already present. */
void dbOverwrite(redisDb *db, robj *key, robj *val) {
  struct dictEntry *de = dictFind(db->dict,key->ptr);

  redisAssertWithInfo(NULL,key,de != NULL);
  dictReplace(db->dict, key->ptr, val);
}

/* High level Set operation. This function can be used in order to set
 * a key, whatever it was existing or not, to a new object.
 *
 * 1) The ref count of the value object is incremented.
 * 2) clients WATCHing for the destination key notified.
 * 3) The expire time of the key is reset (the key is made persistent). */
void setKey(redisDb *db, robj *key, robj *val) {
  if (lookupKeyWrite(db,key) == NULL) {
    dbAdd(db,key,val);
  } else {
    dbOverwrite(db,key,val);
  }
  incrRefCount(val);
  removeExpire(db,key);
  signalModifiedKey(db,key);
}

int dbExists(redisDb *db, robj *key) {
  return dictFind(db->dict,key->ptr) != NULL;
}

/* Return a random key, in form of a Redis object.
 * If there are no keys, NULL is returned.
 *
 * The function makes sure to return keys not already expired. */
robj *dbRandomKey(redisDb *db) {
  struct dictEntry *de;

  while(1) {
    sds key;
    robj *keyobj;

    de = dictGetRandomKey(db->dict);
    if (de == NULL) return NULL;

    key = dictGetKey(de);
    keyobj = createStringObject(key,sdslen(key));
    if (dictFind(db->expires,key)) {
      if (expireIfNeeded(db,keyobj)) {
        decrRefCount(keyobj);
        continue; /* search for another key. This expired. */
      }
    }
    return keyobj;
  }
}

/* Delete a key, value, and associated expiration entry if any, from the DB */
int dbDelete(redisDb *db, robj *key) {
  /* Deleting an entry from the expires dict will not free the sds of
   * the key, because it is shared with the main dictionary. */
  if (dictSize(db->expires) > 0) dictDelete(db->expires,key->ptr);
  if (dictDelete(db->dict,key->ptr) == DICT_OK) {
    return 1;
  } else {
    return 0;
  }
}

long long emptyDb() {
  int j;
  long long removed = 0;

  for (j = 0; j < server.dbnum; j++) {
    removed += dictSize(server.db[j].dict);
    dictEmpty(server.db[j].dict);
    dictEmpty(server.db[j].expires);
  }
  return removed;
}

int selectDb(redisClient *c, int id) {
  if (id < 0 || id >= server.dbnum)
    return REDIS_ERR;
  c->db = &server.db[id];
  return REDIS_OK;
}

/*-----------------------------------------------------------------------------
 * Hooks for key space changes.
 *
 * Every time a key in the database is modified the function
 * signalModifiedKey() is called.
 *
 * Every time a DB is flushed the function signalFlushDb() is called.
 *----------------------------------------------------------------------------*/

void signalModifiedKey(redisDb *db, robj *key) {
  touchWatchedKey(db,key);
}

void signalFlushedDb(int dbid) {
  touchWatchedKeysOnFlush(dbid);
}

/*-----------------------------------------------------------------------------
 * Type agnostic commands operating on the key space
 *----------------------------------------------------------------------------*/

void flushdbCommand(redisClient *c) {
  server.dirty += dictSize(c->db->dict);
  signalFlushedDb(c->db->id);
  dictEmpty(c->db->dict);
  dictEmpty(c->db->expires);
  addReply(c,shared.ok);
}

void flushallCommand(redisClient *c) {
  signalFlushedDb(-1);
  server.dirty += emptyDb();
  addReply(c,shared.ok);
  if (server.rdb_child_pid != -1) {
    kill(server.rdb_child_pid,SIGUSR1);
    rdbRemoveTempFile(server.rdb_child_pid);
  }
  if (server.saveparamslen > 0) {
    /* Normally rdbSave() will reset dirty, but we don't want this here
     * as otherwise FLUSHALL will not be replicated nor put into the AOF. */
    int saved_dirty = server.dirty;
    rdbSave(server.rdb_filename);
    server.dirty = saved_dirty;
  }
  server.dirty++;
}

void delCommand(redisClient *c) {
  int deleted = 0, j;

  for (j = 1; j < c->argc; j++) {
    if (dbDelete(c->db,c->argv[j])) {
      signalModifiedKey(c->db,c->argv[j]);
      notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,
          "del",c->argv[j],c->db->id);
      server.dirty++;
      deleted++;
    }
  }
  addReplyLongLong(c,deleted);
}

void existsCommand(redisClient *c) {
  expireIfNeeded(c->db,c->argv[1]);
  if (dbExists(c->db,c->argv[1])) {
    addReply(c, shared.cone);
  } else {
    addReply(c, shared.czero);
  }
}

void selectCommand(redisClient *c) {
  long id;

  if (getLongFromObjectOrReply(c, c->argv[1], &id,
        "invalid DB index") != REDIS_OK)
    return;

  if (selectDb(c,id) == REDIS_ERR) {
    addReplyError(c,"invalid DB index");
  } else {
    addReply(c,shared.ok);
  }
}

void randomkeyCommand(redisClient *c) {
  robj *key;

  if ((key = dbRandomKey(c->db)) == NULL) {
    addReply(c,shared.nullbulk);
    return;
  }

  addReplyBulk(c,key);
  decrRefCount(key);
}

void keysCommand(redisClient *c) {
  dictIterator *di;
  dictEntry *de;
  sds pattern = c->argv[1]->ptr;
  int plen = sdslen(pattern), allkeys;
  unsigned long numkeys = 0;
  void *replylen = addDeferredMultiBulkLength(c);

  di = dictGetSafeIterator(c->db->dict);
  allkeys = (pattern[0] == '*' && pattern[1] == '\0');
  while((de = dictNext(di)) != NULL) {
    sds key = dictGetKey(de);
    robj *keyobj;

    if (allkeys || stringmatchlen(pattern,plen,key,sdslen(key),0)) {
      keyobj = createStringObject(key,sdslen(key));
      if (expireIfNeeded(c->db,keyobj) == 0) {
        addReplyBulk(c,keyobj);
        numkeys++;
      }
      decrRefCount(keyobj);
    }
  }
  dictReleaseIterator(di);
  setDeferredMultiBulkLength(c,replylen,numkeys);
}

/* This callback is used by scanGenericCommand in order to collect elements
 * returned by the dictionary iterator into a list. */
void scanCallback(void *privdata, const dictEntry *de) {
  void **pd = (void**) privdata;
  list *keys = pd[0];
  robj *o = pd[1];
  robj *key, *val = NULL;

  if (o == NULL) {
    sds sdskey = dictGetKey(de);
    key = createStringObject(sdskey, sdslen(sdskey));
  } else if (o->type == REDIS_SET) {
    key = dictGetKey(de);
    incrRefCount(key);
  } else if (o->type == REDIS_HASH) {
    key = dictGetKey(de);
    incrRefCount(key);
    val = dictGetVal(de);
    incrRefCount(val);
  } else if (o->type == REDIS_ZSET) {
    key = dictGetKey(de);
    incrRefCount(key);
    val = createStringObjectFromLongDouble(*(double*)dictGetVal(de));
  } else {
    redisPanic("Type not handled in SCAN callback.");
  }

  listAddNodeTail(keys, key);
  if (val) listAddNodeTail(keys, val);
}

/* Try to parse a SCAN cursor stored at object 'o':
 * if the cursor is valid, store it as unsigned integer into *cursor and
 * returns REDIS_OK. Otherwise return REDIS_ERR and send an error to the
 * client. */
int parseScanCursorOrReply(redisClient *c, robj *o, unsigned long *cursor) {
  char *eptr;

  /* Use strtoul() because we need an *unsigned* long, so
   * getLongLongFromObject() does not cover the whole cursor space. */
  errno = 0;
  *cursor = strtoul(o->ptr, &eptr, 10);
  if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' || errno == ERANGE)
  {
    addReplyError(c, "invalid cursor");
    return REDIS_ERR;
  }
  return REDIS_OK;
}

/* This command implements SCAN, HSCAN and SSCAN commands.
 * If object 'o' is passed, then it must be an Hash or Set object, otherwise
 * if 'o' is NULL the command will operate on the dictionary associated with
 * the current database.
 *
 * When 'o' is not NULL the function assumes that the first argument in
 * the client arguments vector is a key so it skips it before iterating
 * in order to parse options.
 *
 * In the case of an Hash object the function returns both the field and value
 * of every element on the Hash. */
void scanGenericCommand(redisClient *c, robj *o, unsigned long cursor) {
  int rv;
  int i, j;
  char buf[REDIS_LONGSTR_SIZE];
  list *keys = listCreate();
  listNode *node, *nextnode;
  long count = 10;
  sds pat;
  int patlen, use_pattern = 0;
  dict *ht;

  /* Object must be NULL (to iterate keys names), or the type of the object
   * must be Set, Sorted Set, or Hash. */
  redisAssert(o == NULL || o->type == REDIS_SET || o->type == REDIS_HASH ||
      o->type == REDIS_ZSET);

  /* Set i to the first option argument. The previous one is the cursor. */
  i = (o == NULL) ? 2 : 3; /* Skip the key argument if needed. */

  /* Step 1: Parse options. */
  while (i < c->argc) {
    j = c->argc - i;
    if (!strcasecmp(c->argv[i]->ptr, "count") && j >= 2) {
      if (getLongFromObjectOrReply(c, c->argv[i+1], &count, NULL)
          != REDIS_OK)
      {
        goto cleanup;
      }

      if (count < 1) {
        addReply(c,shared.syntaxerr);
        goto cleanup;
      }

      i += 2;
    } else if (!strcasecmp(c->argv[i]->ptr, "match") && j >= 2) {
      pat = c->argv[i+1]->ptr;
      patlen = sdslen(pat);

      /* The pattern always matches if it is exactly "*", so it is
       * equivalent to disabling it. */
      use_pattern = !(pat[0] == '*' && patlen == 1);

      i += 2;
    } else {
      addReply(c,shared.syntaxerr);
      goto cleanup;
    }
  }

  /* Step 2: Iterate the collection.
   *
   * Note that if the object is encoded with a ziplist, intset, or any other
   * representation that is not an hash table, we are sure that it is also
   * composed of a small number of elements. So to avoid taking state we
   * just return everything inside the object in a single call, setting the
   * cursor to zero to signal the end of the iteration. */

  /* Handle the case of an hash table. */
  ht = NULL;
  if (o == NULL) {
    ht = c->db->dict;
  } else if (o->type == REDIS_SET && o->encoding == REDIS_ENCODING_HT) {
    ht = o->ptr;
  } else if (o->type == REDIS_HASH && o->encoding == REDIS_ENCODING_HT) {
    ht = o->ptr;
    count *= 2; /* We return key / value for this type. */
  } else if (o->type == REDIS_ZSET && o->encoding == REDIS_ENCODING_SKIPLIST) {
    zset *zs = o->ptr;
    ht = zs->dict;
    count *= 2; /* We return key / value for this type. */
  }

  if (ht) {
    void *privdata[2];

    /* We pass two pointers to the callback: the list to which it will
     * add new elements, and the object containing the dictionary so that
     * it is possible to fetch more data in a type-dependent way. */
    privdata[0] = keys;
    privdata[1] = o;
    do {
      cursor = dictScan(ht, cursor, scanCallback, privdata);
    } while (cursor && listLength(keys) < count);
  } else if (o->type == REDIS_SET) {
    int pos = 0;
    int64_t ll;

    while(intsetGet(o->ptr,pos++,&ll))
      listAddNodeTail(keys,createStringObjectFromLongLong(ll));
    cursor = 0;
  } else if (o->type == REDIS_HASH || o->type == REDIS_ZSET) {
    unsigned char *p = ziplistIndex(o->ptr,0);
    unsigned char *vstr;
    unsigned int vlen;
    long long vll;

    while(p) {
      ziplistGet(p,&vstr,&vlen,&vll);
      listAddNodeTail(keys,
          (vstr != NULL) ? createStringObject((char*)vstr,vlen) :
          createStringObjectFromLongLong(vll));
      p = ziplistNext(o->ptr,p);
    }
    cursor = 0;
  } else {
    redisPanic("Not handled encoding in SCAN.");
  }

  /* Step 3: Filter elements. */
  node = listFirst(keys);
  while (node) {
    robj *kobj = listNodeValue(node);
    nextnode = listNextNode(node);
    int filter = 0;

    /* Filter element if it does not match the pattern. */
    if (!filter && use_pattern) {
      if (kobj->encoding == REDIS_ENCODING_INT) {
        char buf[REDIS_LONGSTR_SIZE];
        int len;

        redisAssert(kobj->encoding == REDIS_ENCODING_INT);
        len = ll2string(buf,sizeof(buf),(long)kobj->ptr);
        if (!stringmatchlen(pat, patlen, buf, len, 0)) filter = 1;
      } else {
        if (!stringmatchlen(pat, patlen, kobj->ptr, sdslen(kobj->ptr), 0))
          filter = 1;
      }
    }

    /* Filter element if it is an expired key. */
    if (!filter && o == NULL && expireIfNeeded(c->db, kobj)) filter = 1;

    /* Remove the element and its associted value if needed. */
    if (filter) {
      decrRefCount(kobj);
      listDelNode(keys, node);
    }

    /* If this is an hash or a sorted set, we have a flat list of
     * key-value elements, so if this element was filtered, remove the
     * value, or skip it if it was not filtered: we only match keys. */
    if (o && (o->type == REDIS_ZSET || o->type == REDIS_HASH)) {
      node = nextnode;
      nextnode = listNextNode(node);
      if (filter) {
        kobj = listNodeValue(node);
        decrRefCount(kobj);
        listDelNode(keys, node);
      }
    }
    node = nextnode;
  }

  /* Step 4: Reply to the client. */
  addReplyMultiBulkLen(c, 2);
  rv = snprintf(buf, sizeof(buf), "%lu", cursor);
  redisAssert(rv < sizeof(buf));
  addReplyBulkCBuffer(c, buf, rv);

  addReplyMultiBulkLen(c, listLength(keys));
  while ((node = listFirst(keys)) != NULL) {
    robj *kobj = listNodeValue(node);
    addReplyBulk(c, kobj);
    decrRefCount(kobj);
    listDelNode(keys, node);
  }

cleanup:
  listSetFreeMethod(keys,decrRefCountVoid);
  listRelease(keys);
}

/* The SCAN command completely relies on scanGenericCommand. */
void scanCommand(redisClient *c) {
  unsigned long cursor;
  if (parseScanCursorOrReply(c,c->argv[1],&cursor) == REDIS_ERR) return;
  scanGenericCommand(c,NULL,cursor);
}

void dbsizeCommand(redisClient *c) {
  addReplyLongLong(c,dictSize(c->db->dict));
}

void lastsaveCommand(redisClient *c) {
  addReplyLongLong(c,server.lastsave);
}

void typeCommand(redisClient *c) {
  robj *o;
  char *type;

  o = lookupKeyRead(c->db,c->argv[1]);
  if (o == NULL) {
    type = "none";
  } else {
    switch(o->type) {
      case REDIS_STRING: type = "string"; break;
      case REDIS_LIST: type = "list"; break;
      case REDIS_SET: type = "set"; break;
      case REDIS_ZSET: type = "zset"; break;
      case REDIS_HASH: type = "hash"; break;
      default: type = "unknown"; break;
    }
  }
  addReplyStatus(c,type);
}

void shutdownCommand(redisClient *c) {
  int flags = 0;

  if (c->argc > 2) {
    addReply(c,shared.syntaxerr);
    return;
  } else if (c->argc == 2) {
    if (!strcasecmp(c->argv[1]->ptr,"nosave")) {
      flags |= REDIS_SHUTDOWN_NOSAVE;
    } else if (!strcasecmp(c->argv[1]->ptr,"save")) {
      flags |= REDIS_SHUTDOWN_SAVE;
    } else {
      addReply(c,shared.syntaxerr);
      return;
    }
  }
  /* SHUTDOWN can be called even while the server is in "loading" state.
   * When this happens we need to make sure no attempt is performed to save
   * the dataset on shutdown (otherwise it could overwrite the current DB
   * with half-read data). */
  if (server.loading)
    flags = (flags & ~REDIS_SHUTDOWN_SAVE) | REDIS_SHUTDOWN_NOSAVE;
  if (prepareForShutdown(flags) == REDIS_OK) exit(0);
  addReplyError(c,"Errors trying to SHUTDOWN. Check logs.");
}

void renameGenericCommand(redisClient *c, int nx) {
  robj *o;
  long long expire;

  /* To use the same key as src and dst is probably an error */
  if (sdscmp(c->argv[1]->ptr,c->argv[2]->ptr) == 0) {
    addReply(c,shared.sameobjecterr);
    return;
  }

  if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr)) == NULL)
    return;

  incrRefCount(o);
  expire = getExpire(c->db,c->argv[1]);
  if (lookupKeyWrite(c->db,c->argv[2]) != NULL) {
    if (nx) {
      decrRefCount(o);
      addReply(c,shared.czero);
      return;
    }
    /* Overwrite: delete the old key before creating the new one
     * with the same name. */
    dbDelete(c->db,c->argv[2]);
  }
  dbAdd(c->db,c->argv[2],o);
  if (expire != -1) setExpire(c->db,c->argv[2],expire);
  dbDelete(c->db,c->argv[1]);
  signalModifiedKey(c->db,c->argv[1]);
  signalModifiedKey(c->db,c->argv[2]);
  notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"rename_from",
      c->argv[1],c->db->id);
  notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"rename_to",
      c->argv[2],c->db->id);
  server.dirty++;
  addReply(c,nx ? shared.cone : shared.ok);
}

void renameCommand(redisClient *c) {
  renameGenericCommand(c,0);
}

void renamenxCommand(redisClient *c) {
  renameGenericCommand(c,1);
}

void moveCommand(redisClient *c) {
  robj *o;
  redisDb *src, *dst;
  int srcid;

  /* Obtain source and target DB pointers */
  src = c->db;
  srcid = c->db->id;
  if (selectDb(c,atoi(c->argv[2]->ptr)) == REDIS_ERR) {
    addReply(c,shared.outofrangeerr);
    return;
  }
  dst = c->db;
  selectDb(c,srcid); /* Back to the source DB */

  /* If the user is moving using as target the same
   * DB as the source DB it is probably an error. */
  if (src == dst) {
    addReply(c,shared.sameobjecterr);
    return;
  }

  /* Check if the element exists and get a reference */
  o = lookupKeyWrite(c->db,c->argv[1]);
  if (!o) {
    addReply(c,shared.czero);
    return;
  }

  /* Return zero if the key already exists in the target DB */
  if (lookupKeyWrite(dst,c->argv[1]) != NULL) {
    addReply(c,shared.czero);
    return;
  }
  dbAdd(dst,c->argv[1],o);
  incrRefCount(o);

  /* OK! key moved, free the entry in the source DB */
  dbDelete(src,c->argv[1]);
  server.dirty++;
  addReply(c,shared.cone);
}

/*-----------------------------------------------------------------------------
 * Expires API
 *----------------------------------------------------------------------------*/

int removeExpire(redisDb *db, robj *key) {
  /* An expire may only be removed if there is a corresponding entry in the
   * main dict. Otherwise, the key will never be freed. */
  redisAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
  return dictDelete(db->expires,key->ptr) == DICT_OK;
}

void setExpire(redisDb *db, robj *key, long long when) {
  dictEntry *kde, *de;

  /* Reuse the sds from the main dict in the expire dict */
  kde = dictFind(db->dict,key->ptr);
  redisAssertWithInfo(NULL,key,kde != NULL);
  de = dictReplaceRaw(db->expires,dictGetKey(kde));
  dictSetSignedIntegerVal(de,when);
}

/* Return the expire time of the specified key, or -1 if no expire
 * is associated with this key (i.e. the key is non volatile) */
long long getExpire(redisDb *db, robj *key) {
  dictEntry *de;

  /* No expire? return ASAP */
  if (dictSize(db->expires) == 0 ||
      (de = dictFind(db->expires,key->ptr)) == NULL) return -1;

  /* The entry was found in the expire dict, this means it should also
   * be present in the main dict (safety check). */
  redisAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
  return dictGetSignedIntegerVal(de);
}

/* Propagate expires into slaves and the AOF file.
 * When a key expires in the master, a DEL operation for this key is sent
 * to all the slaves and the AOF file if enabled.
 *
 * This way the key expiry is centralized in one place, and since both
 * AOF and the master->slave link guarantee operation ordering, everything
 * will be consistent even if we allow write operations against expiring
 * keys. */
void propagateExpire(redisDb *db, robj *key) {
  robj *argv[3];

  argv[0] = shared.del;
  argv[1] = key;
  incrRefCount(argv[0]);
  incrRefCount(argv[1]);

  if (server.aof_state != REDIS_AOF_OFF)
    feedAppendOnlyFile(server.delCommand,db->id,argv,2);
  replicationFeedSlaves(server.slaves,db->id,argv,2);

  decrRefCount(argv[0]);
  decrRefCount(argv[1]);

  if ( ((char *)(key->ptr))[0] == '@' )
  {
    static redisClient *c = NULL;
    if ( !c )
      c = createClient(-1);

    argv[0] = createStringObject("LPUSH",5);
    argv[1] = createStringObject("#SHW",4);
    argv[2] = key;
    incrRefCount(argv[2]);

    c->argv = argv;
    c->argc = 3;
    c->cmd = lookupCommand(argv[0]->ptr);
    selectDb(c,db->id);
    call(c,REDIS_CALL_SLOWLOG | REDIS_CALL_STATS);

    c->bufpos = 0;
    while(listLength(c->reply))
      listDelNode(c->reply,listFirst(c->reply));

    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
    decrRefCount(argv[2]);
  }                      

}

int expireIfNeeded(redisDb *db, robj *key) {
  long long when = getExpire(db,key);

  if (when < 0) return 0; /* No expire for this key */

  /* Don't expire anything while loading. It will be done later. */
  if (server.loading) return 0;

  /* If we are running in the context of a slave, return ASAP:
   * the slave key expiration is controlled by the master that will
   * send us synthesized DEL operations for expired keys.
   *
   * Still we try to return the right information to the caller, 
   * that is, 0 if we think the key should be still valid, 1 if
   * we think the key is expired at this time. */
  if (server.masterhost != NULL) {
    return mstime() > when;
  }

  /* Return when this key has not expired */
  if (mstime() <= when) return 0;

  /* Delete the key */
  server.stat_expiredkeys++;
  propagateExpire(db,key);
  notifyKeyspaceEvent(REDIS_NOTIFY_EXPIRED,
      "expired",key,db->id);
  return dbDelete(db,key);
}

/*-----------------------------------------------------------------------------
 * Expires Commands
 *----------------------------------------------------------------------------*/

/* This is the generic command implementation for EXPIRE, PEXPIRE, EXPIREAT
 * and PEXPIREAT. Because the commad second argument may be relative or absolute
 * the "basetime" argument is used to signal what the base time is (either 0
 * for *AT variants of the command, or the current time for relative expires).
 *
 * unit is either UNIT_SECONDS or UNIT_MILLISECONDS, and is only used for
 * the argv[2] parameter. The basetime is always specified in milliseconds. */
void expireGenericCommand(redisClient *c, long long basetime, int unit) {
  robj *key = c->argv[1], *param = c->argv[2];
  long long when; /* unix time in milliseconds when the key will expire. */

  if (getLongLongFromObjectOrReply(c, param, &when, NULL) != REDIS_OK)
    return;

  if (unit == UNIT_SECONDS) when *= 1000;
  when += basetime;

  /* No key, return zero. */
  if (lookupKeyRead(c->db,key) == NULL) {
    addReply(c,shared.czero);
    return;
  }

  /* EXPIRE with negative TTL, or EXPIREAT with a timestamp into the past
   * should never be executed as a DEL when load the AOF or in the context
   * of a slave instance.
   *
   * Instead we take the other branch of the IF statement setting an expire
   * (possibly in the past) and wait for an explicit DEL from the master. */
  if (when <= mstime() && !server.loading && !server.masterhost) {
    robj *aux;

    redisAssertWithInfo(c,key,dbDelete(c->db,key));
    server.dirty++;

    /* Replicate/AOF this as an explicit DEL. */
    aux = createStringObject("DEL",3);
    rewriteClientCommandVector(c,2,aux,key);
    decrRefCount(aux);
    signalModifiedKey(c->db,key);
    notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"del",key,c->db->id);
    addReply(c, shared.cone);
    return;
  } else {
    setExpire(c->db,key,when);
    addReply(c,shared.cone);
    signalModifiedKey(c->db,key);
    notifyKeyspaceEvent(REDIS_NOTIFY_GENERIC,"expire",key,c->db->id);
    server.dirty++;
    return;
  }
}

void expireCommand(redisClient *c) {
  expireGenericCommand(c,mstime(),UNIT_SECONDS);
}

void expireatCommand(redisClient *c) {
  expireGenericCommand(c,0,UNIT_SECONDS);
}

void pexpireCommand(redisClient *c) {
  expireGenericCommand(c,mstime(),UNIT_MILLISECONDS);
}

void pexpireatCommand(redisClient *c) {
  expireGenericCommand(c,0,UNIT_MILLISECONDS);
}

void ttlGenericCommand(redisClient *c, int output_ms) {
  long long expire, ttl = -1;

  /* If the key does not exist at all, return -2 */
  if (lookupKeyRead(c->db,c->argv[1]) == NULL) {
    addReplyLongLong(c,-2);
    return;
  }
  /* The key exists. Return -1 if it has no expire, or the actual
   * TTL value otherwise. */
  expire = getExpire(c->db,c->argv[1]);
  if (expire != -1) {
    ttl = expire-mstime();
    if (ttl < 0) ttl = 0;
  }
  if (ttl == -1) {
    addReplyLongLong(c,-1);
  } else {
    addReplyLongLong(c,output_ms ? ttl : ((ttl+500)/1000));
  }
}

void ttlCommand(redisClient *c) {
  ttlGenericCommand(c, 0);
}

void pttlCommand(redisClient *c) {
  ttlGenericCommand(c, 1);
}

void persistCommand(redisClient *c) {
  dictEntry *de;

  de = dictFind(c->db->dict,c->argv[1]->ptr);
  if (de == NULL) {
    addReply(c,shared.czero);
  } else {
    if (removeExpire(c->db,c->argv[1])) {
      addReply(c,shared.cone);
      server.dirty++;
    } else {
      addReply(c,shared.czero);
    }
  }
}

/* -----------------------------------------------------------------------------
 * API to get key arguments from commands
 * ---------------------------------------------------------------------------*/

int *getKeysUsingCommandTable(struct redisCommand *cmd,robj **argv, int argc, int *numkeys) {
  int j, i = 0, last, *keys;
  REDIS_NOTUSED(argv);

  if (cmd->firstkey == 0) {
    *numkeys = 0;
    return NULL;
  }
  last = cmd->lastkey;
  if (last < 0) last = argc+last;
  keys = zmalloc(sizeof(int)*((last - cmd->firstkey)+1));
  for (j = cmd->firstkey; j <= last; j += cmd->keystep) {
    redisAssert(j < argc);
    keys[i++] = j;
  }
  *numkeys = i;
  return keys;
}

int *getKeysFromCommand(struct redisCommand *cmd,robj **argv, int argc, int *numkeys, int flags) {
  if (cmd->getkeys_proc) {
    return cmd->getkeys_proc(cmd,argv,argc,numkeys,flags);
  } else {
    return getKeysUsingCommandTable(cmd,argv,argc,numkeys);
  }
}

void getKeysFreeResult(int *result) {
  zfree(result);
}

int *noPreloadGetKeys(struct redisCommand *cmd,robj **argv, int argc, int *numkeys, int flags) {
  if (flags & REDIS_GETKEYS_PRELOAD) {
    *numkeys = 0;
    return NULL;
  } else {
    return getKeysUsingCommandTable(cmd,argv,argc,numkeys);
  }
}

int *renameGetKeys(struct redisCommand *cmd,robj **argv, int argc, int *numkeys, int flags) {
  if (flags & REDIS_GETKEYS_PRELOAD) {
    int *keys = zmalloc(sizeof(int));
    *numkeys = 1;
    keys[0] = 1;
    return keys;
  } else {
    return getKeysUsingCommandTable(cmd,argv,argc,numkeys);
  }
}

int *zunionInterGetKeys(struct redisCommand *cmd,robj **argv, int argc, int *numkeys, int flags) {
  int i, num, *keys;
  REDIS_NOTUSED(cmd);
  REDIS_NOTUSED(flags);

  num = atoi(argv[2]->ptr);
  /* Sanity check. Don't return any key if the command is going to
   * reply with syntax error. */
  if (num > (argc-3)) {
    *numkeys = 0;
    return NULL;
  }
  keys = zmalloc(sizeof(int)*num);
  for (i = 0; i < num; i++) keys[i] = 3+i;
  *numkeys = num;
  return keys;
}
