/**
* Licensed to the Apache Software Foundation (ASF) under one
* or more contributor license agreements.  See the NOTICE file
* distributed with this work for additional information
* regarding copyright ownership.  The ASF licenses this file
* to you under the Apache License, Version 2.0 (the
* "License"); you may not use this file except in compliance
* with the License.  You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include <stdlib.h>
#include "zookeeper.jute.h"

int serialize_Id(struct oarchive *out, const char *tag, struct Id *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_String(out, "scheme", &v->scheme);
    rc = rc ? : out->serialize_String(out, "id", &v->id);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_Id(struct iarchive *in, const char *tag, struct Id*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_String(in, "scheme", &v->scheme);
    rc = rc ? : in->deserialize_String(in, "id", &v->id);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_Id(struct Id*v){
    deallocate_String(&v->scheme);
    deallocate_String(&v->id);
}
int serialize_ACL(struct oarchive *out, const char *tag, struct ACL *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_Int(out, "perms", &v->perms);
    rc = rc ? : serialize_Id(out, "id", &v->id);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_ACL(struct iarchive *in, const char *tag, struct ACL*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_Int(in, "perms", &v->perms);
    rc = rc ? : deserialize_Id(in, "id", &v->id);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_ACL(struct ACL*v){
    deallocate_Id(&v->id);
}
int serialize_Stat(struct oarchive *out, const char *tag, struct Stat *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_Long(out, "czxid", &v->czxid);
    rc = rc ? : out->serialize_Long(out, "mzxid", &v->mzxid);
    rc = rc ? : out->serialize_Long(out, "ctime", &v->ctime);
    rc = rc ? : out->serialize_Long(out, "mtime", &v->mtime);
    rc = rc ? : out->serialize_Int(out, "version", &v->version);
    rc = rc ? : out->serialize_Int(out, "cversion", &v->cversion);
    rc = rc ? : out->serialize_Int(out, "aversion", &v->aversion);
    rc = rc ? : out->serialize_Long(out, "ephemeralOwner", &v->ephemeralOwner);
    rc = rc ? : out->serialize_Int(out, "dataLength", &v->dataLength);
    rc = rc ? : out->serialize_Int(out, "numChildren", &v->numChildren);
    rc = rc ? : out->serialize_Long(out, "pzxid", &v->pzxid);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_Stat(struct iarchive *in, const char *tag, struct Stat*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_Long(in, "czxid", &v->czxid);
    rc = rc ? : in->deserialize_Long(in, "mzxid", &v->mzxid);
    rc = rc ? : in->deserialize_Long(in, "ctime", &v->ctime);
    rc = rc ? : in->deserialize_Long(in, "mtime", &v->mtime);
    rc = rc ? : in->deserialize_Int(in, "version", &v->version);
    rc = rc ? : in->deserialize_Int(in, "cversion", &v->cversion);
    rc = rc ? : in->deserialize_Int(in, "aversion", &v->aversion);
    rc = rc ? : in->deserialize_Long(in, "ephemeralOwner", &v->ephemeralOwner);
    rc = rc ? : in->deserialize_Int(in, "dataLength", &v->dataLength);
    rc = rc ? : in->deserialize_Int(in, "numChildren", &v->numChildren);
    rc = rc ? : in->deserialize_Long(in, "pzxid", &v->pzxid);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_Stat(struct Stat*v){
}
int serialize_StatPersisted(struct oarchive *out, const char *tag, struct StatPersisted *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_Long(out, "czxid", &v->czxid);
    rc = rc ? : out->serialize_Long(out, "mzxid", &v->mzxid);
    rc = rc ? : out->serialize_Long(out, "ctime", &v->ctime);
    rc = rc ? : out->serialize_Long(out, "mtime", &v->mtime);
    rc = rc ? : out->serialize_Int(out, "version", &v->version);
    rc = rc ? : out->serialize_Int(out, "cversion", &v->cversion);
    rc = rc ? : out->serialize_Int(out, "aversion", &v->aversion);
    rc = rc ? : out->serialize_Long(out, "ephemeralOwner", &v->ephemeralOwner);
    rc = rc ? : out->serialize_Long(out, "pzxid", &v->pzxid);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_StatPersisted(struct iarchive *in, const char *tag, struct StatPersisted*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_Long(in, "czxid", &v->czxid);
    rc = rc ? : in->deserialize_Long(in, "mzxid", &v->mzxid);
    rc = rc ? : in->deserialize_Long(in, "ctime", &v->ctime);
    rc = rc ? : in->deserialize_Long(in, "mtime", &v->mtime);
    rc = rc ? : in->deserialize_Int(in, "version", &v->version);
    rc = rc ? : in->deserialize_Int(in, "cversion", &v->cversion);
    rc = rc ? : in->deserialize_Int(in, "aversion", &v->aversion);
    rc = rc ? : in->deserialize_Long(in, "ephemeralOwner", &v->ephemeralOwner);
    rc = rc ? : in->deserialize_Long(in, "pzxid", &v->pzxid);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_StatPersisted(struct StatPersisted*v){
}
int serialize_StatPersistedV1(struct oarchive *out, const char *tag, struct StatPersistedV1 *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_Long(out, "czxid", &v->czxid);
    rc = rc ? : out->serialize_Long(out, "mzxid", &v->mzxid);
    rc = rc ? : out->serialize_Long(out, "ctime", &v->ctime);
    rc = rc ? : out->serialize_Long(out, "mtime", &v->mtime);
    rc = rc ? : out->serialize_Int(out, "version", &v->version);
    rc = rc ? : out->serialize_Int(out, "cversion", &v->cversion);
    rc = rc ? : out->serialize_Int(out, "aversion", &v->aversion);
    rc = rc ? : out->serialize_Long(out, "ephemeralOwner", &v->ephemeralOwner);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_StatPersistedV1(struct iarchive *in, const char *tag, struct StatPersistedV1*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_Long(in, "czxid", &v->czxid);
    rc = rc ? : in->deserialize_Long(in, "mzxid", &v->mzxid);
    rc = rc ? : in->deserialize_Long(in, "ctime", &v->ctime);
    rc = rc ? : in->deserialize_Long(in, "mtime", &v->mtime);
    rc = rc ? : in->deserialize_Int(in, "version", &v->version);
    rc = rc ? : in->deserialize_Int(in, "cversion", &v->cversion);
    rc = rc ? : in->deserialize_Int(in, "aversion", &v->aversion);
    rc = rc ? : in->deserialize_Long(in, "ephemeralOwner", &v->ephemeralOwner);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_StatPersistedV1(struct StatPersistedV1*v){
}
int serialize_op_result_t(struct oarchive *out, const char *tag, struct op_result_t *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_Int(out, "rc", &v->rc);
    rc = rc ? : out->serialize_Int(out, "op", &v->op);
    rc = rc ? : out->serialize_Buffer(out, "response", &v->response);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_op_result_t(struct iarchive *in, const char *tag, struct op_result_t*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_Int(in, "rc", &v->rc);
    rc = rc ? : in->deserialize_Int(in, "op", &v->op);
    rc = rc ? : in->deserialize_Buffer(in, "response", &v->response);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_op_result_t(struct op_result_t*v){
    deallocate_Buffer(&v->response);
}
int serialize_ConnectRequest(struct oarchive *out, const char *tag, struct ConnectRequest *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_Int(out, "protocolVersion", &v->protocolVersion);
    rc = rc ? : out->serialize_Long(out, "lastZxidSeen", &v->lastZxidSeen);
    rc = rc ? : out->serialize_Int(out, "timeOut", &v->timeOut);
    rc = rc ? : out->serialize_Long(out, "sessionId", &v->sessionId);
    rc = rc ? : out->serialize_Buffer(out, "passwd", &v->passwd);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_ConnectRequest(struct iarchive *in, const char *tag, struct ConnectRequest*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_Int(in, "protocolVersion", &v->protocolVersion);
    rc = rc ? : in->deserialize_Long(in, "lastZxidSeen", &v->lastZxidSeen);
    rc = rc ? : in->deserialize_Int(in, "timeOut", &v->timeOut);
    rc = rc ? : in->deserialize_Long(in, "sessionId", &v->sessionId);
    rc = rc ? : in->deserialize_Buffer(in, "passwd", &v->passwd);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_ConnectRequest(struct ConnectRequest*v){
    deallocate_Buffer(&v->passwd);
}
int serialize_ConnectResponse(struct oarchive *out, const char *tag, struct ConnectResponse *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_Int(out, "protocolVersion", &v->protocolVersion);
    rc = rc ? : out->serialize_Int(out, "timeOut", &v->timeOut);
    rc = rc ? : out->serialize_Long(out, "sessionId", &v->sessionId);
    rc = rc ? : out->serialize_Buffer(out, "passwd", &v->passwd);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_ConnectResponse(struct iarchive *in, const char *tag, struct ConnectResponse*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_Int(in, "protocolVersion", &v->protocolVersion);
    rc = rc ? : in->deserialize_Int(in, "timeOut", &v->timeOut);
    rc = rc ? : in->deserialize_Long(in, "sessionId", &v->sessionId);
    rc = rc ? : in->deserialize_Buffer(in, "passwd", &v->passwd);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_ConnectResponse(struct ConnectResponse*v){
    deallocate_Buffer(&v->passwd);
}
int allocate_String_vector(struct String_vector *v, int32_t len) {
    if (!len) {
        v->count = 0;
        v->data = 0;
    } else {
        v->count = len;
        v->data = calloc(sizeof(*v->data), len);
    }
    return 0;
}
int deallocate_String_vector(struct String_vector *v) {
    if (v->data) {
        int32_t i;
        for(i=0;i<v->count; i++) {
            deallocate_String(&v->data[i]);
        }
        free(v->data);
        v->data = 0;
    }
    return 0;
}
int serialize_String_vector(struct oarchive *out, const char *tag, struct String_vector *v)
{
    int32_t count = v->count;
    int rc = 0;
    int32_t i;
    rc = out->start_vector(out, tag, &count);
    for(i=0;i<v->count;i++) {
    rc = rc ? : out->serialize_String(out, "data", &v->data[i]);
    }
    rc = rc ? : out->end_vector(out, tag);
    return rc;
}
int deserialize_String_vector(struct iarchive *in, const char *tag, struct String_vector *v)
{
    int rc = 0;
    int32_t i;
    rc = in->start_vector(in, tag, &v->count);
    v->data = calloc(v->count, sizeof(*v->data));
    for(i=0;i<v->count;i++) {
    rc = rc ? : in->deserialize_String(in, "value", &v->data[i]);
    }
    rc = in->end_vector(in, tag);
    return rc;
}
int serialize_SetWatches(struct oarchive *out, const char *tag, struct SetWatches *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_Long(out, "relativeZxid", &v->relativeZxid);
    rc = rc ? : serialize_String_vector(out, "dataWatches", &v->dataWatches);
    rc = rc ? : serialize_String_vector(out, "existWatches", &v->existWatches);
    rc = rc ? : serialize_String_vector(out, "childWatches", &v->childWatches);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_SetWatches(struct iarchive *in, const char *tag, struct SetWatches*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_Long(in, "relativeZxid", &v->relativeZxid);
    rc = rc ? : deserialize_String_vector(in, "dataWatches", &v->dataWatches);
    rc = rc ? : deserialize_String_vector(in, "existWatches", &v->existWatches);
    rc = rc ? : deserialize_String_vector(in, "childWatches", &v->childWatches);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_SetWatches(struct SetWatches*v){
    deallocate_String_vector(&v->dataWatches);
    deallocate_String_vector(&v->existWatches);
    deallocate_String_vector(&v->childWatches);
}
int serialize_RequestHeader(struct oarchive *out, const char *tag, struct RequestHeader *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_Int(out, "xid", &v->xid);
    rc = rc ? : out->serialize_Int(out, "type", &v->type);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_RequestHeader(struct iarchive *in, const char *tag, struct RequestHeader*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_Int(in, "xid", &v->xid);
    rc = rc ? : in->deserialize_Int(in, "type", &v->type);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_RequestHeader(struct RequestHeader*v){
}
int serialize_AuthPacket(struct oarchive *out, const char *tag, struct AuthPacket *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_Int(out, "type", &v->type);
    rc = rc ? : out->serialize_String(out, "scheme", &v->scheme);
    rc = rc ? : out->serialize_Buffer(out, "auth", &v->auth);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_AuthPacket(struct iarchive *in, const char *tag, struct AuthPacket*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_Int(in, "type", &v->type);
    rc = rc ? : in->deserialize_String(in, "scheme", &v->scheme);
    rc = rc ? : in->deserialize_Buffer(in, "auth", &v->auth);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_AuthPacket(struct AuthPacket*v){
    deallocate_String(&v->scheme);
    deallocate_Buffer(&v->auth);
}
int serialize_ReplyHeader(struct oarchive *out, const char *tag, struct ReplyHeader *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_Int(out, "xid", &v->xid);
    rc = rc ? : out->serialize_Long(out, "zxid", &v->zxid);
    rc = rc ? : out->serialize_Int(out, "err", &v->err);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_ReplyHeader(struct iarchive *in, const char *tag, struct ReplyHeader*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_Int(in, "xid", &v->xid);
    rc = rc ? : in->deserialize_Long(in, "zxid", &v->zxid);
    rc = rc ? : in->deserialize_Int(in, "err", &v->err);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_ReplyHeader(struct ReplyHeader*v){
}
int serialize_GetDataRequest(struct oarchive *out, const char *tag, struct GetDataRequest *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_String(out, "path", &v->path);
    rc = rc ? : out->serialize_Bool(out, "watch", &v->watch);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_GetDataRequest(struct iarchive *in, const char *tag, struct GetDataRequest*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_String(in, "path", &v->path);
    rc = rc ? : in->deserialize_Bool(in, "watch", &v->watch);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_GetDataRequest(struct GetDataRequest*v){
    deallocate_String(&v->path);
}
int serialize_SetDataRequest(struct oarchive *out, const char *tag, struct SetDataRequest *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_String(out, "path", &v->path);
    rc = rc ? : out->serialize_Buffer(out, "data", &v->data);
    rc = rc ? : out->serialize_Int(out, "version", &v->version);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_SetDataRequest(struct iarchive *in, const char *tag, struct SetDataRequest*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_String(in, "path", &v->path);
    rc = rc ? : in->deserialize_Buffer(in, "data", &v->data);
    rc = rc ? : in->deserialize_Int(in, "version", &v->version);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_SetDataRequest(struct SetDataRequest*v){
    deallocate_String(&v->path);
    deallocate_Buffer(&v->data);
}
int serialize_SetDataResponse(struct oarchive *out, const char *tag, struct SetDataResponse *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : serialize_Stat(out, "stat", &v->stat);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_SetDataResponse(struct iarchive *in, const char *tag, struct SetDataResponse*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : deserialize_Stat(in, "stat", &v->stat);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_SetDataResponse(struct SetDataResponse*v){
    deallocate_Stat(&v->stat);
}
int allocate_ACL_vector(struct ACL_vector *v, int32_t len) {
    if (!len) {
        v->count = 0;
        v->data = 0;
    } else {
        v->count = len;
        v->data = calloc(sizeof(*v->data), len);
    }
    return 0;
}
int deallocate_ACL_vector(struct ACL_vector *v) {
    if (v->data) {
        int32_t i;
        for(i=0;i<v->count; i++) {
            deallocate_ACL(&v->data[i]);
        }
        free(v->data);
        v->data = 0;
    }
    return 0;
}
int serialize_ACL_vector(struct oarchive *out, const char *tag, struct ACL_vector *v)
{
    int32_t count = v->count;
    int rc = 0;
    int32_t i;
    rc = out->start_vector(out, tag, &count);
    for(i=0;i<v->count;i++) {
    rc = rc ? : serialize_ACL(out, "data", &v->data[i]);
    }
    rc = rc ? : out->end_vector(out, tag);
    return rc;
}
int deserialize_ACL_vector(struct iarchive *in, const char *tag, struct ACL_vector *v)
{
    int rc = 0;
    int32_t i;
    rc = in->start_vector(in, tag, &v->count);
    v->data = calloc(v->count, sizeof(*v->data));
    for(i=0;i<v->count;i++) {
    rc = rc ? : deserialize_ACL(in, "value", &v->data[i]);
    }
    rc = in->end_vector(in, tag);
    return rc;
}
int serialize_CreateRequest(struct oarchive *out, const char *tag, struct CreateRequest *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_String(out, "path", &v->path);
    rc = rc ? : out->serialize_Buffer(out, "data", &v->data);
    rc = rc ? : serialize_ACL_vector(out, "acl", &v->acl);
    rc = rc ? : out->serialize_Int(out, "flags", &v->flags);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_CreateRequest(struct iarchive *in, const char *tag, struct CreateRequest*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_String(in, "path", &v->path);
    rc = rc ? : in->deserialize_Buffer(in, "data", &v->data);
    rc = rc ? : deserialize_ACL_vector(in, "acl", &v->acl);
    rc = rc ? : in->deserialize_Int(in, "flags", &v->flags);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_CreateRequest(struct CreateRequest*v){
    deallocate_String(&v->path);
    deallocate_Buffer(&v->data);
    deallocate_ACL_vector(&v->acl);
}
int serialize_DeleteRequest(struct oarchive *out, const char *tag, struct DeleteRequest *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_String(out, "path", &v->path);
    rc = rc ? : out->serialize_Int(out, "version", &v->version);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_DeleteRequest(struct iarchive *in, const char *tag, struct DeleteRequest*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_String(in, "path", &v->path);
    rc = rc ? : in->deserialize_Int(in, "version", &v->version);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_DeleteRequest(struct DeleteRequest*v){
    deallocate_String(&v->path);
}
int serialize_GetChildrenRequest(struct oarchive *out, const char *tag, struct GetChildrenRequest *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_String(out, "path", &v->path);
    rc = rc ? : out->serialize_Bool(out, "watch", &v->watch);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_GetChildrenRequest(struct iarchive *in, const char *tag, struct GetChildrenRequest*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_String(in, "path", &v->path);
    rc = rc ? : in->deserialize_Bool(in, "watch", &v->watch);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_GetChildrenRequest(struct GetChildrenRequest*v){
    deallocate_String(&v->path);
}
int serialize_GetChildren2Request(struct oarchive *out, const char *tag, struct GetChildren2Request *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_String(out, "path", &v->path);
    rc = rc ? : out->serialize_Bool(out, "watch", &v->watch);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_GetChildren2Request(struct iarchive *in, const char *tag, struct GetChildren2Request*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_String(in, "path", &v->path);
    rc = rc ? : in->deserialize_Bool(in, "watch", &v->watch);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_GetChildren2Request(struct GetChildren2Request*v){
    deallocate_String(&v->path);
}
int serialize_GetMaxChildrenRequest(struct oarchive *out, const char *tag, struct GetMaxChildrenRequest *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_String(out, "path", &v->path);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_GetMaxChildrenRequest(struct iarchive *in, const char *tag, struct GetMaxChildrenRequest*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_String(in, "path", &v->path);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_GetMaxChildrenRequest(struct GetMaxChildrenRequest*v){
    deallocate_String(&v->path);
}
int serialize_GetMaxChildrenResponse(struct oarchive *out, const char *tag, struct GetMaxChildrenResponse *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_Int(out, "max", &v->max);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_GetMaxChildrenResponse(struct iarchive *in, const char *tag, struct GetMaxChildrenResponse*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_Int(in, "max", &v->max);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_GetMaxChildrenResponse(struct GetMaxChildrenResponse*v){
}
int serialize_SetMaxChildrenRequest(struct oarchive *out, const char *tag, struct SetMaxChildrenRequest *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_String(out, "path", &v->path);
    rc = rc ? : out->serialize_Int(out, "max", &v->max);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_SetMaxChildrenRequest(struct iarchive *in, const char *tag, struct SetMaxChildrenRequest*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_String(in, "path", &v->path);
    rc = rc ? : in->deserialize_Int(in, "max", &v->max);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_SetMaxChildrenRequest(struct SetMaxChildrenRequest*v){
    deallocate_String(&v->path);
}
int serialize_SyncRequest(struct oarchive *out, const char *tag, struct SyncRequest *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_String(out, "path", &v->path);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_SyncRequest(struct iarchive *in, const char *tag, struct SyncRequest*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_String(in, "path", &v->path);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_SyncRequest(struct SyncRequest*v){
    deallocate_String(&v->path);
}
int serialize_SyncResponse(struct oarchive *out, const char *tag, struct SyncResponse *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_String(out, "path", &v->path);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_SyncResponse(struct iarchive *in, const char *tag, struct SyncResponse*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_String(in, "path", &v->path);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_SyncResponse(struct SyncResponse*v){
    deallocate_String(&v->path);
}
int serialize_GetACLRequest(struct oarchive *out, const char *tag, struct GetACLRequest *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_String(out, "path", &v->path);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_GetACLRequest(struct iarchive *in, const char *tag, struct GetACLRequest*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_String(in, "path", &v->path);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_GetACLRequest(struct GetACLRequest*v){
    deallocate_String(&v->path);
}
int serialize_SetACLRequest(struct oarchive *out, const char *tag, struct SetACLRequest *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_String(out, "path", &v->path);
    rc = rc ? : serialize_ACL_vector(out, "acl", &v->acl);
    rc = rc ? : out->serialize_Int(out, "version", &v->version);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_SetACLRequest(struct iarchive *in, const char *tag, struct SetACLRequest*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_String(in, "path", &v->path);
    rc = rc ? : deserialize_ACL_vector(in, "acl", &v->acl);
    rc = rc ? : in->deserialize_Int(in, "version", &v->version);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_SetACLRequest(struct SetACLRequest*v){
    deallocate_String(&v->path);
    deallocate_ACL_vector(&v->acl);
}
int serialize_SetACLResponse(struct oarchive *out, const char *tag, struct SetACLResponse *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : serialize_Stat(out, "stat", &v->stat);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_SetACLResponse(struct iarchive *in, const char *tag, struct SetACLResponse*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : deserialize_Stat(in, "stat", &v->stat);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_SetACLResponse(struct SetACLResponse*v){
    deallocate_Stat(&v->stat);
}
int serialize_WatcherEvent(struct oarchive *out, const char *tag, struct WatcherEvent *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_Int(out, "type", &v->type);
    rc = rc ? : out->serialize_Int(out, "state", &v->state);
    rc = rc ? : out->serialize_String(out, "path", &v->path);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_WatcherEvent(struct iarchive *in, const char *tag, struct WatcherEvent*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_Int(in, "type", &v->type);
    rc = rc ? : in->deserialize_Int(in, "state", &v->state);
    rc = rc ? : in->deserialize_String(in, "path", &v->path);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_WatcherEvent(struct WatcherEvent*v){
    deallocate_String(&v->path);
}
int serialize_CreateResponse(struct oarchive *out, const char *tag, struct CreateResponse *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_String(out, "path", &v->path);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_CreateResponse(struct iarchive *in, const char *tag, struct CreateResponse*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_String(in, "path", &v->path);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_CreateResponse(struct CreateResponse*v){
    deallocate_String(&v->path);
}
int serialize_ExistsRequest(struct oarchive *out, const char *tag, struct ExistsRequest *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_String(out, "path", &v->path);
    rc = rc ? : out->serialize_Bool(out, "watch", &v->watch);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_ExistsRequest(struct iarchive *in, const char *tag, struct ExistsRequest*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_String(in, "path", &v->path);
    rc = rc ? : in->deserialize_Bool(in, "watch", &v->watch);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_ExistsRequest(struct ExistsRequest*v){
    deallocate_String(&v->path);
}
int serialize_ExistsResponse(struct oarchive *out, const char *tag, struct ExistsResponse *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : serialize_Stat(out, "stat", &v->stat);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_ExistsResponse(struct iarchive *in, const char *tag, struct ExistsResponse*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : deserialize_Stat(in, "stat", &v->stat);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_ExistsResponse(struct ExistsResponse*v){
    deallocate_Stat(&v->stat);
}
int serialize_GetDataResponse(struct oarchive *out, const char *tag, struct GetDataResponse *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_Buffer(out, "data", &v->data);
    rc = rc ? : serialize_Stat(out, "stat", &v->stat);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_GetDataResponse(struct iarchive *in, const char *tag, struct GetDataResponse*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_Buffer(in, "data", &v->data);
    rc = rc ? : deserialize_Stat(in, "stat", &v->stat);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_GetDataResponse(struct GetDataResponse*v){
    deallocate_Buffer(&v->data);
    deallocate_Stat(&v->stat);
}
int serialize_GetChildrenResponse(struct oarchive *out, const char *tag, struct GetChildrenResponse *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : serialize_String_vector(out, "children", &v->children);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_GetChildrenResponse(struct iarchive *in, const char *tag, struct GetChildrenResponse*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : deserialize_String_vector(in, "children", &v->children);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_GetChildrenResponse(struct GetChildrenResponse*v){
    deallocate_String_vector(&v->children);
}
int serialize_GetChildren2Response(struct oarchive *out, const char *tag, struct GetChildren2Response *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : serialize_String_vector(out, "children", &v->children);
    rc = rc ? : serialize_Stat(out, "stat", &v->stat);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_GetChildren2Response(struct iarchive *in, const char *tag, struct GetChildren2Response*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : deserialize_String_vector(in, "children", &v->children);
    rc = rc ? : deserialize_Stat(in, "stat", &v->stat);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_GetChildren2Response(struct GetChildren2Response*v){
    deallocate_String_vector(&v->children);
    deallocate_Stat(&v->stat);
}
int serialize_GetACLResponse(struct oarchive *out, const char *tag, struct GetACLResponse *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : serialize_ACL_vector(out, "acl", &v->acl);
    rc = rc ? : serialize_Stat(out, "stat", &v->stat);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_GetACLResponse(struct iarchive *in, const char *tag, struct GetACLResponse*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : deserialize_ACL_vector(in, "acl", &v->acl);
    rc = rc ? : deserialize_Stat(in, "stat", &v->stat);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_GetACLResponse(struct GetACLResponse*v){
    deallocate_ACL_vector(&v->acl);
    deallocate_Stat(&v->stat);
}
int allocate_Id_vector(struct Id_vector *v, int32_t len) {
    if (!len) {
        v->count = 0;
        v->data = 0;
    } else {
        v->count = len;
        v->data = calloc(sizeof(*v->data), len);
    }
    return 0;
}
int deallocate_Id_vector(struct Id_vector *v) {
    if (v->data) {
        int32_t i;
        for(i=0;i<v->count; i++) {
            deallocate_Id(&v->data[i]);
        }
        free(v->data);
        v->data = 0;
    }
    return 0;
}
int serialize_Id_vector(struct oarchive *out, const char *tag, struct Id_vector *v)
{
    int32_t count = v->count;
    int rc = 0;
    int32_t i;
    rc = out->start_vector(out, tag, &count);
    for(i=0;i<v->count;i++) {
    rc = rc ? : serialize_Id(out, "data", &v->data[i]);
    }
    rc = rc ? : out->end_vector(out, tag);
    return rc;
}
int deserialize_Id_vector(struct iarchive *in, const char *tag, struct Id_vector *v)
{
    int rc = 0;
    int32_t i;
    rc = in->start_vector(in, tag, &v->count);
    v->data = calloc(v->count, sizeof(*v->data));
    for(i=0;i<v->count;i++) {
    rc = rc ? : deserialize_Id(in, "value", &v->data[i]);
    }
    rc = in->end_vector(in, tag);
    return rc;
}
int serialize_QuorumPacket(struct oarchive *out, const char *tag, struct QuorumPacket *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_Int(out, "type", &v->type);
    rc = rc ? : out->serialize_Long(out, "zxid", &v->zxid);
    rc = rc ? : out->serialize_Buffer(out, "data", &v->data);
    rc = rc ? : serialize_Id_vector(out, "authinfo", &v->authinfo);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_QuorumPacket(struct iarchive *in, const char *tag, struct QuorumPacket*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_Int(in, "type", &v->type);
    rc = rc ? : in->deserialize_Long(in, "zxid", &v->zxid);
    rc = rc ? : in->deserialize_Buffer(in, "data", &v->data);
    rc = rc ? : deserialize_Id_vector(in, "authinfo", &v->authinfo);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_QuorumPacket(struct QuorumPacket*v){
    deallocate_Buffer(&v->data);
    deallocate_Id_vector(&v->authinfo);
}
int serialize_FileHeader(struct oarchive *out, const char *tag, struct FileHeader *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_Int(out, "magic", &v->magic);
    rc = rc ? : out->serialize_Int(out, "version", &v->version);
    rc = rc ? : out->serialize_Long(out, "dbid", &v->dbid);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_FileHeader(struct iarchive *in, const char *tag, struct FileHeader*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_Int(in, "magic", &v->magic);
    rc = rc ? : in->deserialize_Int(in, "version", &v->version);
    rc = rc ? : in->deserialize_Long(in, "dbid", &v->dbid);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_FileHeader(struct FileHeader*v){
}
int serialize_TxnHeader(struct oarchive *out, const char *tag, struct TxnHeader *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_Long(out, "clientId", &v->clientId);
    rc = rc ? : out->serialize_Int(out, "cxid", &v->cxid);
    rc = rc ? : out->serialize_Long(out, "zxid", &v->zxid);
    rc = rc ? : out->serialize_Long(out, "time", &v->time);
    rc = rc ? : out->serialize_Int(out, "type", &v->type);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_TxnHeader(struct iarchive *in, const char *tag, struct TxnHeader*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_Long(in, "clientId", &v->clientId);
    rc = rc ? : in->deserialize_Int(in, "cxid", &v->cxid);
    rc = rc ? : in->deserialize_Long(in, "zxid", &v->zxid);
    rc = rc ? : in->deserialize_Long(in, "time", &v->time);
    rc = rc ? : in->deserialize_Int(in, "type", &v->type);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_TxnHeader(struct TxnHeader*v){
}
int serialize_CreateTxn(struct oarchive *out, const char *tag, struct CreateTxn *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_String(out, "path", &v->path);
    rc = rc ? : out->serialize_Buffer(out, "data", &v->data);
    rc = rc ? : serialize_ACL_vector(out, "acl", &v->acl);
    rc = rc ? : out->serialize_Bool(out, "ephemeral", &v->ephemeral);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_CreateTxn(struct iarchive *in, const char *tag, struct CreateTxn*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_String(in, "path", &v->path);
    rc = rc ? : in->deserialize_Buffer(in, "data", &v->data);
    rc = rc ? : deserialize_ACL_vector(in, "acl", &v->acl);
    rc = rc ? : in->deserialize_Bool(in, "ephemeral", &v->ephemeral);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_CreateTxn(struct CreateTxn*v){
    deallocate_String(&v->path);
    deallocate_Buffer(&v->data);
    deallocate_ACL_vector(&v->acl);
}
int serialize_DeleteTxn(struct oarchive *out, const char *tag, struct DeleteTxn *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_String(out, "path", &v->path);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_DeleteTxn(struct iarchive *in, const char *tag, struct DeleteTxn*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_String(in, "path", &v->path);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_DeleteTxn(struct DeleteTxn*v){
    deallocate_String(&v->path);
}
int serialize_SetDataTxn(struct oarchive *out, const char *tag, struct SetDataTxn *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_String(out, "path", &v->path);
    rc = rc ? : out->serialize_Buffer(out, "data", &v->data);
    rc = rc ? : out->serialize_Int(out, "version", &v->version);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_SetDataTxn(struct iarchive *in, const char *tag, struct SetDataTxn*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_String(in, "path", &v->path);
    rc = rc ? : in->deserialize_Buffer(in, "data", &v->data);
    rc = rc ? : in->deserialize_Int(in, "version", &v->version);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_SetDataTxn(struct SetDataTxn*v){
    deallocate_String(&v->path);
    deallocate_Buffer(&v->data);
}
int serialize_SetACLTxn(struct oarchive *out, const char *tag, struct SetACLTxn *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_String(out, "path", &v->path);
    rc = rc ? : serialize_ACL_vector(out, "acl", &v->acl);
    rc = rc ? : out->serialize_Int(out, "version", &v->version);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_SetACLTxn(struct iarchive *in, const char *tag, struct SetACLTxn*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_String(in, "path", &v->path);
    rc = rc ? : deserialize_ACL_vector(in, "acl", &v->acl);
    rc = rc ? : in->deserialize_Int(in, "version", &v->version);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_SetACLTxn(struct SetACLTxn*v){
    deallocate_String(&v->path);
    deallocate_ACL_vector(&v->acl);
}
int serialize_SetMaxChildrenTxn(struct oarchive *out, const char *tag, struct SetMaxChildrenTxn *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_String(out, "path", &v->path);
    rc = rc ? : out->serialize_Int(out, "max", &v->max);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_SetMaxChildrenTxn(struct iarchive *in, const char *tag, struct SetMaxChildrenTxn*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_String(in, "path", &v->path);
    rc = rc ? : in->deserialize_Int(in, "max", &v->max);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_SetMaxChildrenTxn(struct SetMaxChildrenTxn*v){
    deallocate_String(&v->path);
}
int serialize_CreateSessionTxn(struct oarchive *out, const char *tag, struct CreateSessionTxn *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_Int(out, "timeOut", &v->timeOut);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_CreateSessionTxn(struct iarchive *in, const char *tag, struct CreateSessionTxn*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_Int(in, "timeOut", &v->timeOut);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_CreateSessionTxn(struct CreateSessionTxn*v){
}
int serialize_ErrorTxn(struct oarchive *out, const char *tag, struct ErrorTxn *v){
    int rc;
    rc = out->start_record(out, tag);
    rc = rc ? : out->serialize_Int(out, "err", &v->err);
    rc = rc ? : out->end_record(out, tag);
    return rc;
}
int deserialize_ErrorTxn(struct iarchive *in, const char *tag, struct ErrorTxn*v){
    int rc;
    rc = in->start_record(in, tag);
    rc = rc ? : in->deserialize_Int(in, "err", &v->err);
    rc = rc ? : in->end_record(in, tag);
    return rc;
}
void deallocate_ErrorTxn(struct ErrorTxn*v){
}
