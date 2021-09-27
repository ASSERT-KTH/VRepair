/* libcomps - C alternative to yum.comps library
 * Copyright (C) 2013 Jindrich Luza
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to  Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA
 */

#include "comps_objmradix.h"
#include "comps_set.h"
#include <stdio.h>

void comps_objmrtree_data_destroy(COMPS_ObjMRTreeData * rtd) {
    free(rtd->key);
    COMPS_OBJECT_DESTROY(rtd->data);
    comps_hslist_destroy(&rtd->subnodes);
    free(rtd);
}

inline void comps_objmrtree_data_destroy_v(void * rtd) {
    comps_objmrtree_data_destroy((COMPS_ObjMRTreeData*)rtd);
}

static COMPS_ObjMRTreeData * __comps_objmrtree_data_create(char * key,
                                                    size_t keylen,
                                                    COMPS_Object *data) {

    COMPS_ObjMRTreeData * rtd;
    if ((rtd = malloc(sizeof(*rtd))) == NULL)
        return NULL;
    if ((rtd->key = malloc(sizeof(char) * (keylen+1))) == NULL) {
        free(rtd);
        return NULL;
    }
    memcpy(rtd->key, key, sizeof(char)*keylen);
    rtd->key[keylen] = '\0';
    rtd->is_leaf = 1;
    rtd->data = COMPS_OBJECT_CREATE(COMPS_ObjList, NULL);
    if (data)
        comps_objlist_append_x(rtd->data, data);
    rtd->subnodes = comps_hslist_create();
    comps_hslist_init(rtd->subnodes, NULL,
                                     NULL,
                                     &comps_objmrtree_data_destroy_v);
    return rtd;
}

COMPS_ObjMRTreeData * comps_objmrtree_data_create(char *key, COMPS_Object *data){
    COMPS_ObjMRTreeData * rtd;
    rtd = __comps_objmrtree_data_create(key, strlen(key), data);
    return rtd;
}

COMPS_ObjMRTreeData * comps_objmrtree_data_create_n(char * key, unsigned keylen,
                                                    void * data) {
    COMPS_ObjMRTreeData * rtd;
    rtd = __comps_objmrtree_data_create(key, keylen, data);
    return rtd;
}
static void comps_objmrtree_create(COMPS_ObjMRTree *rtree, COMPS_Object **args){
    (void)args;
    rtree->subnodes = comps_hslist_create();
    comps_hslist_init(rtree->subnodes, NULL, NULL, &comps_objmrtree_data_destroy_v);
    if (rtree->subnodes == NULL) {
        COMPS_OBJECT_DESTROY(rtree);
        return;
    }
    rtree->len = 0;
}
void comps_objmrtree_create_u(COMPS_Object * obj, COMPS_Object **args) {
    (void)args;
    comps_objmrtree_create((COMPS_ObjMRTree*)obj, NULL);
}

static void comps_objmrtree_destroy(COMPS_ObjMRTree * rt) {
    comps_hslist_destroy(&(rt->subnodes));
}
void comps_objmrtree_destroy_u(COMPS_Object *obj) {
    comps_objmrtree_destroy((COMPS_ObjMRTree*)obj);
}

void comps_objmrtree_values_walk(COMPS_ObjMRTree * rt, void* udata,
                              void (*walk_f)(void*, void*)) {
    COMPS_HSList *tmplist, *tmp_subnodes;
    COMPS_HSListItem *it, *it2;
    tmplist = comps_hslist_create();
    comps_hslist_init(tmplist, NULL, NULL, NULL);
    comps_hslist_append(tmplist, rt->subnodes, 0);
    while (tmplist->first != NULL) {
        it = tmplist->first;
        comps_hslist_remove(tmplist, tmplist->first);
        tmp_subnodes = (COMPS_HSList*)it->data;
        free(it);
        for (it = tmp_subnodes->first; it != NULL; it=it->next) {
            if (((COMPS_ObjMRTreeData*)it->data)->subnodes->first) {
                comps_hslist_append(tmplist,
                                    ((COMPS_ObjMRTreeData*)it->data)->subnodes, 0);
            }
            for (it2 = (COMPS_HSListItem*)((COMPS_ObjMRTreeData*)it->data)->data->first;
                 it2 != NULL; it2 = it2->next) {
                walk_f(udata, it2->data);
            }
        }
    }
    comps_hslist_destroy(&tmplist);
}

void comps_objmrtree_copy(COMPS_ObjMRTree *ret, COMPS_ObjMRTree *rt){
    COMPS_HSList * to_clone, *tmplist, *new_subnodes;
    COMPS_HSListItem *it, *it2;
    COMPS_ObjMRTreeData *rtdata;
    COMPS_ObjList *new_data_list;

    to_clone = comps_hslist_create();
    comps_hslist_init(to_clone, NULL, NULL, NULL);

    for (it = rt->subnodes->first; it != NULL; it = it->next) {
        rtdata = comps_objmrtree_data_create(
                                       ((COMPS_ObjMRTreeData*)it->data)->key,
                                       NULL);
        new_data_list = (COMPS_ObjList*)
                        COMPS_OBJECT_COPY(((COMPS_ObjMRTreeData*)it->data)->data);
        COMPS_OBJECT_DESTROY(&rtdata->data);
        comps_hslist_destroy(&rtdata->subnodes);
        rtdata->subnodes = ((COMPS_ObjMRTreeData*)it->data)->subnodes;
        rtdata->data = new_data_list;
        comps_hslist_append(ret->subnodes, rtdata, 0);

        comps_hslist_append(to_clone, rtdata, 0);
    }

    while (to_clone->first) {
        it2 = to_clone->first;
        tmplist = ((COMPS_ObjMRTreeData*)it2->data)->subnodes;
        comps_hslist_remove(to_clone, to_clone->first);

        new_subnodes = comps_hslist_create();
        comps_hslist_init(new_subnodes, NULL, NULL, &comps_objmrtree_data_destroy_v);
        for (it = tmplist->first; it != NULL; it = it->next) {
            rtdata = comps_objmrtree_data_create(
                                          ((COMPS_ObjMRTreeData*)it->data)->key,
                                          NULL);
            new_data_list = (COMPS_ObjList*)
                            COMPS_OBJECT_COPY(((COMPS_ObjMRTreeData*)it->data)->data);

            comps_hslist_destroy(&rtdata->subnodes);
            COMPS_OBJECT_DESTROY(rtdata->data);
            rtdata->subnodes = ((COMPS_ObjMRTreeData*)it->data)->subnodes;
            rtdata->data = new_data_list;
            comps_hslist_append(new_subnodes, rtdata, 0);

            comps_hslist_append(to_clone, rtdata, 0);
        }
        ((COMPS_ObjMRTreeData*)it2->data)->subnodes = new_subnodes;
        free(it2);
    }
    ret->len = rt->len;
    comps_hslist_destroy(&to_clone);
}
COMPS_COPY_u(objmrtree, COMPS_ObjMRTree) /*comps_utils.h macro*/

void comps_objmrtree_copy_shallow(COMPS_ObjMRTree *ret, COMPS_ObjMRTree *rt){
    COMPS_HSList * to_clone, *tmplist, *new_subnodes;
    COMPS_HSListItem *it, *it2;
    COMPS_ObjMRTreeData *rtdata;
    COMPS_ObjList *new_data_list;

    to_clone = comps_hslist_create();
    comps_hslist_init(to_clone, NULL, NULL, NULL);

    for (it = rt->subnodes->first; it != NULL; it = it->next) {
        rtdata = comps_objmrtree_data_create(
                                       ((COMPS_ObjMRTreeData*)it->data)->key,
                                       NULL);
        new_data_list = (COMPS_ObjList*)
                        COMPS_OBJECT_COPY(((COMPS_ObjMRTreeData*)it->data)->data);
        COMPS_OBJECT_DESTROY(&rtdata->data);
        comps_hslist_destroy(&rtdata->subnodes);
        rtdata->subnodes = ((COMPS_ObjMRTreeData*)it->data)->subnodes;
        rtdata->data = new_data_list;
        comps_hslist_append(ret->subnodes, rtdata, 0);

        comps_hslist_append(to_clone, rtdata, 0);
    }

    while (to_clone->first) {
        it2 = to_clone->first;
        tmplist = ((COMPS_ObjMRTreeData*)it2->data)->subnodes;
        comps_hslist_remove(to_clone, to_clone->first);

        new_subnodes = comps_hslist_create();
        comps_hslist_init(new_subnodes, NULL, NULL, &comps_objmrtree_data_destroy_v);
        for (it = tmplist->first; it != NULL; it = it->next) {
            rtdata = comps_objmrtree_data_create(
                                          ((COMPS_ObjMRTreeData*)it->data)->key,
                                          NULL);
            new_data_list = ((COMPS_ObjMRTreeData*)it->data)->data;

            comps_hslist_destroy(&rtdata->subnodes);
            COMPS_OBJECT_DESTROY(rtdata->data);
            rtdata->subnodes = ((COMPS_ObjMRTreeData*)it->data)->subnodes;
            rtdata->data = new_data_list;
            comps_hslist_append(new_subnodes, rtdata, 0);

            comps_hslist_append(to_clone, rtdata, 0);
        }
        ((COMPS_ObjMRTreeData*)it2->data)->subnodes = new_subnodes;
        free(it2);
    }
    ret->len = rt->len;
    comps_hslist_destroy(&to_clone);
}

COMPS_ObjMRTree * comps_objmrtree_clone(COMPS_ObjMRTree * rt) {
    COMPS_HSList * to_clone, *tmplist, *new_subnodes;
    COMPS_ObjMRTree * ret;
    COMPS_HSListItem *it, *it2;
    COMPS_ObjMRTreeData *rtdata;
    COMPS_ObjList *new_data_list;

    to_clone = comps_hslist_create();
    comps_hslist_init(to_clone, NULL, NULL, NULL);
    ret = COMPS_OBJECT_CREATE(COMPS_ObjMRTree, NULL);

    for (it = rt->subnodes->first; it != NULL; it = it->next) {
        rtdata = comps_objmrtree_data_create(
                                       ((COMPS_ObjMRTreeData*)it->data)->key,
                                       NULL);
        new_data_list = (COMPS_ObjList*)
                        COMPS_OBJECT_COPY(((COMPS_ObjMRTreeData*)it->data)->data);
        COMPS_OBJECT_DESTROY(&rtdata->data);
        comps_hslist_destroy(&rtdata->subnodes);
        rtdata->subnodes = ((COMPS_ObjMRTreeData*)it->data)->subnodes;
        rtdata->data = new_data_list;
        comps_hslist_append(ret->subnodes, rtdata, 0);

        comps_hslist_append(to_clone, rtdata, 0);
    }

    while (to_clone->first) {
        it2 = to_clone->first;
        tmplist = ((COMPS_ObjMRTreeData*)it2->data)->subnodes;
        comps_hslist_remove(to_clone, to_clone->first);

        new_subnodes = comps_hslist_create();
        comps_hslist_init(new_subnodes, NULL, NULL, &comps_objmrtree_data_destroy_v);
        for (it = tmplist->first; it != NULL; it = it->next) {
            rtdata = comps_objmrtree_data_create(
                                          ((COMPS_ObjMRTreeData*)it->data)->key,
                                          NULL);
            new_data_list = (COMPS_ObjList*)
                            COMPS_OBJECT_COPY(((COMPS_ObjMRTreeData*)it->data)->data);

            comps_hslist_destroy(&rtdata->subnodes);
            COMPS_OBJECT_DESTROY(rtdata->data);
            rtdata->subnodes = ((COMPS_ObjMRTreeData*)it->data)->subnodes;
            rtdata->data = new_data_list;
            comps_hslist_append(new_subnodes, rtdata, 0);

            comps_hslist_append(to_clone, rtdata, 0);
        }
        ((COMPS_ObjMRTreeData*)it2->data)->subnodes = new_subnodes;
        free(it2);
    }
    ret->len = rt->len;
    comps_hslist_destroy(&to_clone);
    return ret;
}

void comps_objmrtree_unite(COMPS_ObjMRTree *rt1, COMPS_ObjMRTree *rt2) {
    COMPS_HSList *tmplist, *tmp_subnodes;
    COMPS_HSListItem *it;
    COMPS_ObjListIt *it2;
    struct Pair {
        COMPS_HSList * subnodes;
        char * key;
        char added;
    } *pair, *parent_pair;

    pair = malloc(sizeof(struct Pair));
    pair->subnodes = rt2->subnodes;
    pair->key = NULL;

    tmplist = comps_hslist_create();
    comps_hslist_init(tmplist, NULL, NULL, &free);
    comps_hslist_append(tmplist, pair, 0);

    while (tmplist->first != NULL) {
        it = tmplist->first;
        comps_hslist_remove(tmplist, tmplist->first);
        tmp_subnodes = ((struct Pair*)it->data)->subnodes;
        parent_pair = (struct Pair*) it->data;
        free(it);

        pair->added = 0;
        for (it = tmp_subnodes->first; it != NULL; it=it->next) {
            pair = malloc(sizeof(struct Pair));
            pair->subnodes = ((COMPS_ObjMRTreeData*)it->data)->subnodes;

            if (parent_pair->key != NULL) {
                pair->key =
                    malloc(sizeof(char)
                           * (strlen(((COMPS_ObjMRTreeData*)it->data)->key)
                           + strlen(parent_pair->key) + 1));
                memcpy(pair->key, parent_pair->key,
                       sizeof(char) * strlen(parent_pair->key));
                memcpy(pair->key+strlen(parent_pair->key),
                       ((COMPS_ObjMRTreeData*)it->data)->key,
                       sizeof(char)*(strlen(((COMPS_ObjMRTreeData*)it->data)->key)+1));
            } else {
                pair->key = malloc(sizeof(char)*
                                (strlen(((COMPS_ObjMRTreeData*)it->data)->key) +
                                1));
                memcpy(pair->key, ((COMPS_ObjMRTreeData*)it->data)->key,
                       sizeof(char)*(strlen(((COMPS_ObjMRTreeData*)it->data)->key)+1));
            }
            /* current node has data */
            if (((COMPS_ObjMRTreeData*)it->data)->data->first != NULL) {
                for (it2 = ((COMPS_ObjMRTreeData*)it->data)->data->first;
                     it2 != NULL; it2 = it2->next) {
                    comps_objmrtree_set(rt1, pair->key, it2->comps_obj);
                }

                if (((COMPS_ObjMRTreeData*)it->data)->subnodes->first) {
                    comps_hslist_append(tmplist, pair, 0);
                } else {
                    free(pair->key);
                    free(pair);
                }
            /* current node hasn't data */
            } else {
                if (((COMPS_ObjMRTreeData*)it->data)->subnodes->first) {
                    comps_hslist_append(tmplist, pair, 0);
                } else {
                    free(pair->key);
                    free(pair);
                }
            }
        }
        free(parent_pair->key);
        free(parent_pair);
    }
    comps_hslist_destroy(&tmplist);
}

void comps_objmrtree_set_x(COMPS_ObjMRTree *rt, char *key, COMPS_Object *data) {
    __comps_objmrtree_set(rt, key, strlen(key), data);
}
void comps_objmrtree_set(COMPS_ObjMRTree *rt, char *key, COMPS_Object *data) {
    __comps_objmrtree_set(rt, key, strlen(key), comps_object_incref(data));
}

void __comps_objmrtree_set(COMPS_ObjMRTree *rt, char *key,
                           size_t len, COMPS_Object *ndata) {
    static COMPS_HSListItem *it;
    COMPS_HSList *subnodes;
    COMPS_ObjMRTreeData *rtd;
    static COMPS_ObjMRTreeData *rtdata;

    size_t _len, offset=0;
    unsigned x, found = 0;
    char ended;//, tmpch;

    if (rt->subnodes == NULL)
        return;

    subnodes = rt->subnodes;
    while (offset != len)
    {
        found = 0;
        for (it = subnodes->first; it != NULL; it = it->next) {
            if (((COMPS_ObjMRTreeData*)it->data)->key[0] == key[offset]) {
                found = 1;
                break;
            }
        }
        if (!found) { // not found in subnodes; create new subnode
            rtd = comps_objmrtree_data_create(key+offset, ndata);
            comps_hslist_append(subnodes, rtd, 0);
            rt->len++;
            return;
        } else {
            rtdata = (COMPS_ObjMRTreeData*)it->data;
            ended = 0;
            for (x=1; ;x++) {
                if (rtdata->key[x] == 0) ended += 1;
                if (x == len - offset) ended += 2;
                if (ended != 0) break;
                if (key[offset+x] != rtdata->key[x]) break;
            }
            if (ended == 3) { //keys equals; append new data
                comps_objlist_append_x(rtdata->data, ndata);
                rt->len++;
                return;
            } else if (ended == 2) { //global key ends first; make global leaf
                comps_hslist_remove(subnodes, it);
                it->next = NULL;
                rtd = comps_objmrtree_data_create(key+offset, ndata);
                comps_hslist_append(subnodes, rtd, 0);
                ((COMPS_ObjMRTreeData*)subnodes->last->data)->subnodes->last = it;
                ((COMPS_ObjMRTreeData*)subnodes->last->data)->subnodes->first = it;
                _len = strlen(key + offset);
                memmove(rtdata->key,rtdata->key + _len,
                                    strlen(rtdata->key) - _len);
                rtdata->key[strlen(rtdata->key) - _len] = 0;
                rtdata->key = realloc(rtdata->key,
                                      sizeof(char)* (strlen(rtdata->key)+1));
                rt->len++;
                return;
            } else if (ended == 1) { //local key ends first; go deeper
                subnodes = rtdata->subnodes;
                offset += x;
            } else { /* keys differ */
                COMPS_ObjList *tmpdata = rtdata->data;
                COMPS_HSList *tmphslist = rtdata->subnodes;

                rtdata->subnodes = comps_hslist_create();
                comps_hslist_init(rtdata->subnodes, NULL, NULL,
                                  &comps_objmrtree_data_destroy_v);
                int cmpret = strcmp(key+offset+x, rtdata->key+x);
                rtdata->data = COMPS_OBJECT_CREATE(COMPS_ObjList, NULL);

                if (cmpret > 0) {
                    rtd = comps_objmrtree_data_create(rtdata->key+x,
                                                      (COMPS_Object*)tmpdata);
                    comps_hslist_destroy(&rtd->subnodes);
                    rtd->subnodes = tmphslist;

                    comps_hslist_append(rtdata->subnodes, rtd, 0);
                    rtd = comps_objmrtree_data_create(key+offset+x,
                                                     (COMPS_Object*)ndata);
                    comps_hslist_append(rtdata->subnodes, rtd, 0);

                } else {
                    rtd = comps_objmrtree_data_create(key+offset+x,
                                                     (COMPS_Object*)ndata);
                    comps_hslist_append(rtdata->subnodes, rtd, 0);
                    rtd = comps_objmrtree_data_create(rtdata->key+x,
                                                     (COMPS_Object*)tmpdata);
                    comps_hslist_destroy(&rtd->subnodes);
                    rtd->subnodes = tmphslist;
                    comps_hslist_append(rtdata->subnodes, rtd, 0);
                }
                rtdata->key = realloc(rtdata->key, sizeof(char)*(x+1));
                rtdata->key[x] = 0;
                rt->len++;
                return;
            }
        }
    }
}

void comps_objmrtree_set_n(COMPS_ObjMRTree *rt, char *key,
                        size_t len, void *ndata) {
    __comps_objmrtree_set(rt, key, len, ndata);
}

COMPS_ObjList * comps_objmrtree_get(COMPS_ObjMRTree * rt, const char * key) {
    COMPS_HSList * subnodes;
    COMPS_HSListItem * it = NULL;
    COMPS_ObjMRTreeData * rtdata;
    unsigned int offset, len, x;
    char found, ended;

    len = strlen(key);
    offset = 0;
    subnodes = rt->subnodes;
    while (offset != len) {
        found = 0;
        for (it = subnodes->first; it != NULL; it=it->next) {
            if (((COMPS_ObjMRTreeData*)it->data)->key[0] == key[offset]) {
                found = 1;
                break;
            }
        }
        if (!found)
            return NULL;
        rtdata = (COMPS_ObjMRTreeData*)it->data;

        for (x=1; ;x++) {
            ended=0;
            if (rtdata->key[x] == 0) ended += 1;
            if (x == len - offset) ended += 2;
            if (ended != 0) break;
            if (key[offset+x] != rtdata->key[x]) break;
        }
        if (ended == 3) return (COMPS_ObjList*)
                               comps_object_incref((COMPS_Object*)rtdata->data);
        else if (ended == 1) offset+=x;
        else return NULL;
        subnodes = ((COMPS_ObjMRTreeData*)it->data)->subnodes;
    }
    if (it)
        return ((COMPS_ObjMRTreeData*)it->data)->data;
    else return NULL;
}

void comps_objmrtree_unset(COMPS_ObjMRTree * rt, const char * key) {
    COMPS_HSList * subnodes;
    COMPS_HSListItem * it;
    COMPS_ObjMRTreeData * rtdata;
    unsigned int offset, len, x;
    char found, ended;
    COMPS_HSList * path;

    struct Relation {
        COMPS_HSList * parent_nodes;
        COMPS_HSListItem * child_it;
    } *relation;

    path = comps_hslist_create();
    comps_hslist_init(path, NULL, NULL, &free);

    len = strlen(key);
    offset = 0;
    subnodes = rt->subnodes;
    while (offset != len) {
        found = 0;
        for (it = subnodes->first; it != NULL; it=it->next) {
            if (((COMPS_ObjMRTreeData*)it->data)->key[0] == key[offset]) {
                found = 1;
                break;
            }
        }
        if (!found) {
            comps_hslist_destroy(&path);
            return;
        }
        rtdata = (COMPS_ObjMRTreeData*)it->data;

        for (x=1; ;x++) {
            ended=0;
            if (rtdata->key[x] == 0) ended += 1;
            if (x == len - offset) ended += 2;
            if (ended != 0) break;
            if (key[offset+x] != rtdata->key[x]) break;
        }
        if (ended == 3) {
            /* remove node from tree only if there's no descendant*/
            if (rtdata->subnodes->last == NULL) {
                comps_hslist_remove(subnodes, it);
                rt->len -= rtdata->data->len;
                comps_objmrtree_data_destroy(rtdata);
                free(it);
            }
            else {
                rt->len -= rtdata->data->len;
                comps_objlist_clear(rtdata->data);
                rtdata->is_leaf = 0;
            }

            if (path->last == NULL) {
                comps_hslist_destroy(&path);
                return;
            }
            rtdata = (COMPS_ObjMRTreeData*)
                     ((struct Relation*)path->last->data)->child_it->data;

            /*remove all predecessor of deleted node (recursive) with no childs*/
            while (rtdata->subnodes->last == NULL) {
                //printf("removing '%s'\n", rtdata->key);
                comps_objmrtree_data_destroy(rtdata);
                comps_hslist_remove(
                            ((struct Relation*)path->last->data)->parent_nodes,
                            ((struct Relation*)path->last->data)->child_it);
                free(((struct Relation*)path->last->data)->child_it);
                it = path->last;
                comps_hslist_remove(path, path->last);
                free(it);
                rtdata = (COMPS_ObjMRTreeData*)
                         ((struct Relation*)path->last->data)->child_it->data;
            }
            comps_hslist_destroy(&path);
            return;
        }
        else if (ended == 1) offset+=x;
        else {
            comps_hslist_destroy(&path);
            return;
        }
        if ((relation = malloc(sizeof(struct Relation))) == NULL) {
            comps_hslist_destroy(&path);
            return;
        }
        subnodes = ((COMPS_ObjMRTreeData*)it->data)->subnodes;
        relation->parent_nodes = subnodes;
        relation->child_it = it;
        comps_hslist_append(path, (void*)relation, 0);
    }
    comps_hslist_destroy(&path);
    return;
}

inline void comps_objmrtree_pair_destroy_v(void * pair) {
    free(((COMPS_ObjMRTreePair *)pair)->key);
    free(pair);
}

inline COMPS_HSList* __comps_objmrtree_all(COMPS_ObjMRTree * rt, char keyvalpair) {
    COMPS_HSList *to_process, *ret;
    COMPS_HSListItem *hsit, *oldit;
    size_t x;
    struct Pair {
        char *key;
        void *data;
        COMPS_HSList *subnodes;
    } *pair, *current_pair=NULL;//, *oldpair=NULL;
    COMPS_ObjMRTreePair *rtpair;

    to_process = comps_hslist_create();
    comps_hslist_init(to_process, NULL, NULL, &free);

    ret = comps_hslist_create();
    if (keyvalpair == 0)
        comps_hslist_init(ret, NULL, NULL, &free);
    else if (keyvalpair == 1)
        comps_hslist_init(ret, NULL, NULL, NULL);
    else
        comps_hslist_init(ret, NULL, NULL, &comps_objmrtree_pair_destroy_v);

    for (hsit = rt->subnodes->first; hsit != NULL; hsit = hsit->next) {
        pair = malloc(sizeof(struct Pair));
        pair->key = __comps_strcpy(((COMPS_ObjMRTreeData*)hsit->data)->key);
        pair->data = ((COMPS_ObjMRTreeData*)hsit->data)->data;
        pair->subnodes = ((COMPS_ObjMRTreeData*)hsit->data)->subnodes;
        comps_hslist_append(to_process, pair, 0);
    }
    while (to_process->first) {
        //oldpair = current_pair;
        current_pair = to_process->first->data;
        oldit = to_process->first;
        comps_hslist_remove(to_process, to_process->first);
        if (current_pair->data) {
            if (keyvalpair == 0) {
                comps_hslist_append(ret, __comps_strcpy(current_pair->key), 0);
            } else if (keyvalpair == 1) {
                comps_hslist_append(ret, current_pair->data, 0);
            } else {
                rtpair = malloc(sizeof(COMPS_ObjMRTreePair));
                rtpair->key = __comps_strcpy(current_pair->key);
                rtpair->data = current_pair->data;
                comps_hslist_append(ret, rtpair, 0);
            }
        }
        for (hsit = current_pair->subnodes->first, x = 0;
             hsit != NULL; hsit = hsit->next, x++) {
            pair = malloc(sizeof(struct Pair));
            pair->key = __comps_strcat(current_pair->key,
                                       ((COMPS_ObjMRTreeData*)hsit->data)->key);
            pair->data = ((COMPS_ObjMRTreeData*)hsit->data)->data;
            pair->subnodes = ((COMPS_ObjMRTreeData*)hsit->data)->subnodes;
            comps_hslist_insert_at(to_process, x, pair, 0);
        }
        free(current_pair->key);
        free(current_pair);
        free(oldit);
    }

    comps_hslist_destroy(&to_process);
    return ret;
}

COMPS_HSList* comps_objmrtree_keys(COMPS_ObjMRTree * rt) {
    return __comps_objmrtree_all(rt, 0);
}

COMPS_HSList* comps_objmrtree_values(COMPS_ObjMRTree * rt) {
    return __comps_objmrtree_all(rt, 1);
}

COMPS_HSList* comps_objmrtree_pairs(COMPS_ObjMRTree * rt) {
    return __comps_objmrtree_all(rt, 2);
}

void comps_objmrtree_clear(COMPS_ObjMRTree * rt) {
    COMPS_HSListItem *it, *oldit;
    if (rt == NULL) return;
    if (rt->subnodes == NULL) return;
    oldit = rt->subnodes->first;
    it = (oldit)?oldit->next:NULL;
    for (;it != NULL; it=it->next) {
        if (rt->subnodes->data_destructor != NULL)
            rt->subnodes->data_destructor(oldit->data);
        free(oldit);
        oldit = it;
    }
    if (oldit) {
        if (rt->subnodes->data_destructor != NULL)
            rt->subnodes->data_destructor(oldit->data);
        free(oldit);
    }
}

char comps_objmrtree_paircmp(void *obj1, void *obj2) {
    if (strcmp(((COMPS_ObjMRTreePair*)obj1)->key,
               ((COMPS_ObjMRTreePair*)obj2)->key) != 0)
        return 0;
    return comps_object_cmp((COMPS_Object*)((COMPS_ObjMRTreePair*)obj1)->data,
                            (COMPS_Object*)((COMPS_ObjMRTreePair*)obj1)->data);
}

signed char comps_objmrtree_cmp(COMPS_ObjMRTree *ort1, COMPS_ObjMRTree *ort2) {
    COMPS_HSList *values1, *values2;
    COMPS_HSListItem *it;
    COMPS_Set *set1, *set2;
    signed char ret;
    values1 = comps_objmrtree_pairs(ort1);
    values2 = comps_objmrtree_pairs(ort2);
    set1 = comps_set_create();
    comps_set_init(set1, NULL, NULL, NULL, &comps_objmrtree_paircmp);
    set2 = comps_set_create();
    comps_set_init(set2, NULL, NULL, NULL, &comps_objmrtree_paircmp);
    for (it = values1->first; it != NULL; it = it->next) {
        comps_set_add(set1, it->data);
    }
    for (it = values2->first; it != NULL; it = it->next) {
        comps_set_add(set2, it->data);
    }

    ret = comps_set_cmp(set1, set2);
    comps_set_destroy(&set1);
    comps_set_destroy(&set2);
    //printf("objmrtree cmp %d\n", !ret);

    comps_hslist_destroy(&values1);
    comps_hslist_destroy(&values2);
    return !ret;
}
COMPS_CMP_u(objmrtree, COMPS_ObjMRTree)

COMPS_ObjectInfo COMPS_ObjMRTree_ObjInfo = {
    .obj_size = sizeof(COMPS_ObjMRTree),
    .constructor = &comps_objmrtree_create_u,
    .destructor = &comps_objmrtree_destroy_u,
    .copy = &comps_objmrtree_copy_u,
    .obj_cmp = &comps_objmrtree_cmp_u
};
