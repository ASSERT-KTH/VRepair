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

#include "comps_radix.h"
#include "comps_utils.h"

#include <stdio.h>

void comps_rtree_data_destroy(COMPS_RTreeData * rtd) {
    free(rtd->key);
    if ((rtd->data) && (*rtd->data_destructor))
        (*rtd->data_destructor)(rtd->data);
    comps_hslist_destroy(&rtd->subnodes);
    free(rtd);
}

inline void comps_rtree_data_destroy_v(void * rtd) {
    comps_rtree_data_destroy((COMPS_RTreeData*)rtd);
}

inline COMPS_RTreeData * __comps_rtree_data_create(COMPS_RTree *rt, char *key,
                                                   unsigned int keylen,
                                                   void *data){
    COMPS_RTreeData * rtd;
    if ((rtd = malloc(sizeof(*rtd))) == NULL)
        return NULL;
    if ((rtd->key = malloc(sizeof(char) * (keylen+1))) == NULL) {
        free(rtd);
        return NULL;
    }
    memcpy(rtd->key, key, sizeof(char)*keylen);
    rtd->key[keylen] = 0;
    rtd->data = data;
    if (data != NULL) {
        rtd->is_leaf = 1;
    }
    rtd->data_destructor = &rt->data_destructor;
    rtd->subnodes = comps_hslist_create();
    comps_hslist_init(rtd->subnodes, NULL, NULL, &comps_rtree_data_destroy_v);
    return rtd;
}

COMPS_RTreeData * comps_rtree_data_create(COMPS_RTree *rt,char * key,
                                          void * data) {
    COMPS_RTreeData * rtd;
    rtd = __comps_rtree_data_create(rt, key, strlen(key), data);
    return rtd;
}

COMPS_RTreeData * comps_rtree_data_create_n(COMPS_RTree *rt, char * key,
                                            size_t keylen, void * data) {
    COMPS_RTreeData * rtd;
    rtd = __comps_rtree_data_create(rt, key, keylen, data);
    return rtd;
}

COMPS_RTree * comps_rtree_create(void* (*data_constructor)(void*),
                                 void* (*data_cloner)(void*),
                                 void (*data_destructor)(void*)) {
    COMPS_RTree *ret;
    if ((ret = malloc(sizeof(COMPS_RTree))) == NULL)
        return NULL;
    ret->subnodes = comps_hslist_create();
    comps_hslist_init(ret->subnodes, NULL, NULL, &comps_rtree_data_destroy_v);
    if (ret->subnodes == NULL) {
        free(ret);
        return NULL;
    }
    ret->data_constructor = data_constructor;
    ret->data_cloner = data_cloner;
    ret->data_destructor = data_destructor;
    return ret;
}

void comps_rtree_destroy(COMPS_RTree * rt) {
    if (!rt) return;
    comps_hslist_destroy(&(rt->subnodes));
    free(rt);
}

void comps_rtree_print(COMPS_HSList * hl, unsigned  deep) {
    COMPS_HSListItem * it;
    for (it = hl->first; it != NULL; it=it->next) {
        printf("%d %s\n",deep, (((COMPS_RTreeData*)it->data)->key));
        comps_rtree_print(((COMPS_RTreeData*)it->data)->subnodes, deep+1);
    }
}

COMPS_RTree * comps_rtree_clone(COMPS_RTree *rt) {

    COMPS_HSList *to_clone, *tmplist, *new_subnodes;
    COMPS_RTree *ret;
    COMPS_HSListItem *it, *it2;
    COMPS_RTreeData *rtdata;
    void *new_data;

    if (!rt) return NULL;

    to_clone = comps_hslist_create();
    comps_hslist_init(to_clone, NULL, NULL, NULL);
    ret = comps_rtree_create(rt->data_constructor, rt->data_cloner,
                             rt->data_destructor);


    for (it = rt->subnodes->first; it != NULL; it = it->next) {
        rtdata = comps_rtree_data_create(ret,
                                      ((COMPS_RTreeData*)it->data)->key, NULL);
        if (((COMPS_RTreeData*)it->data)->data != NULL)
            new_data = rt->data_cloner(((COMPS_RTreeData*)it->data)->data);
        else
            new_data = NULL;
        comps_hslist_destroy(&rtdata->subnodes);
        rtdata->subnodes = ((COMPS_RTreeData*)it->data)->subnodes;
        rtdata->data = new_data;
        comps_hslist_append(ret->subnodes, rtdata, 0);

        comps_hslist_append(to_clone, rtdata, 0);
    }


    while (to_clone->first) {
        it2 = to_clone->first;
        tmplist = ((COMPS_RTreeData*)it2->data)->subnodes;
        comps_hslist_remove(to_clone, to_clone->first);

        new_subnodes = comps_hslist_create();
        comps_hslist_init(new_subnodes, NULL, NULL, &comps_rtree_data_destroy_v);
        for (it = tmplist->first; it != NULL; it = it->next) {
            rtdata = comps_rtree_data_create(ret,
                                      ((COMPS_RTreeData*)it->data)->key, NULL);
            if (((COMPS_RTreeData*)it->data)->data != NULL)
                new_data = rt->data_cloner(((COMPS_RTreeData*)it->data)->data);
            else
                new_data = NULL;
            comps_hslist_destroy(&rtdata->subnodes);
            rtdata->subnodes = ((COMPS_RTreeData*)it->data)->subnodes;
            rtdata->data = new_data;
            comps_hslist_append(new_subnodes, rtdata, 0);

            comps_hslist_append(to_clone, rtdata, 0);
        }
        ((COMPS_RTreeData*)it2->data)->subnodes = new_subnodes;
        free(it2);
    }
    comps_hslist_destroy(&to_clone);
    return ret;
}

void comps_rtree_values_walk(COMPS_RTree * rt, void* udata,
                              void (*walk_f)(void*, void*)) {
    COMPS_HSList *tmplist, *tmp_subnodes;
    COMPS_HSListItem *it;
    tmplist = comps_hslist_create();
    comps_hslist_init(tmplist, NULL, NULL, NULL);
    comps_hslist_append(tmplist, rt->subnodes, 0);
    while (tmplist->first != NULL) {
        it = tmplist->first;
        comps_hslist_remove(tmplist, tmplist->first);
        tmp_subnodes = (COMPS_HSList*)it->data;
        for (it = tmp_subnodes->first; it != NULL; it=it->next) {
            if (((COMPS_RTreeData*)it->data)->subnodes->first) {
                comps_hslist_append(tmplist,
                                    ((COMPS_RTreeData*)it->data)->subnodes, 0);
            }
            if (((COMPS_RTreeData*)it->data)->data != NULL) {
               walk_f(udata, ((COMPS_RTreeData*)it->data)->data);
            }
        }
    }
    comps_hslist_destroy(&tmplist);
}

void __comps_rtree_set(COMPS_RTree * rt, char * key, size_t len, void * data)
{
    COMPS_HSListItem *it, *lesser;
    COMPS_HSList *subnodes;
    COMPS_RTreeData *rtd;
    static COMPS_RTreeData *rtdata;

    size_t offset=0, _len;
    unsigned x, found = 0;
    void *ndata;
    char ended;//, tmpch;

    if (rt->subnodes == NULL)
        return;
    if (rt->data_constructor) {
        ndata = rt->data_constructor(data);
    } else {
        ndata = data;
    }

    subnodes = rt->subnodes;
    while (offset != len)
    {
        found = 0;
        lesser = NULL;
        for (it = subnodes->first; it != NULL; it=it->next) {
            if (((COMPS_RTreeData*)it->data)->key[0] == key[offset]) {
                found = 1;
                break;
            } else if (((COMPS_RTreeData*)it->data)->key[0] < key[offset]) {
                lesser = it;
            }
        }
        if (!found) { // not found in subnodes; create new subnode
            rtd = comps_rtree_data_create(rt, key+offset, ndata);
            if (!lesser) {
                comps_hslist_prepend(subnodes, rtd, 0);
            } else {
                comps_hslist_insert_after(subnodes, lesser, rtd, 0);
            }
            return;
        } else {
            rtdata = (COMPS_RTreeData*)it->data;
            ended = 0;
            for (x=1; ;x++) {
                if (rtdata->key[x] == 0) ended += 1;
                if (x == len - offset) ended += 2;
                if (ended != 0) break;
                if (key[offset+x] != rtdata->key[x]) break;
            }
            if (ended == 3) { //keys equals; data replacement
                rt->data_destructor(rtdata->data);
                rtdata->data = ndata;
                return;
            } else if (ended == 2) { //global key ends first; make global leaf
                comps_hslist_remove(subnodes, it);
                it->next = NULL;
                rtd = comps_rtree_data_create_n(rt, key+offset,
                                                len-offset, ndata);
                comps_hslist_append(subnodes, rtd, 0);
                ((COMPS_RTreeData*)subnodes->last->data)->subnodes->last = it;
                ((COMPS_RTreeData*)subnodes->last->data)->subnodes->first = it;
                _len = len - offset;

                memmove(rtdata->key,rtdata->key + _len,
                                    strlen(rtdata->key) - _len);
                rtdata->key[strlen(rtdata->key) - _len] = 0;
                rtdata->key = realloc(rtdata->key,
                                      sizeof(char)* (strlen(rtdata->key)+1));
                return;
            } else if (ended == 1) { //local key ends first; go deeper
                subnodes = rtdata->subnodes;
                offset += x;
            } else {
                void *tmpdata = rtdata->data;
                COMPS_HSList *tmpnodes = rtdata->subnodes;

                int cmpret = strcmp(key+offset+x, rtdata->key+x);
                rtdata->subnodes = comps_hslist_create();
                comps_hslist_init(rtdata->subnodes, NULL, NULL,
                                  &comps_rtree_data_destroy_v);
                rtdata->data = NULL;

                if (cmpret > 0) {
                    rtd = comps_rtree_data_create(rt, rtdata->key+x, tmpdata);
                    comps_hslist_destroy(&rtd->subnodes);
                    rtd->subnodes = tmpnodes;
                    comps_hslist_append(rtdata->subnodes, rtd, 0);
                    rtd = comps_rtree_data_create(rt, key+offset+x, ndata);
                    comps_hslist_append(rtdata->subnodes, rtd, 0);
                } else {
                    rtd = comps_rtree_data_create(rt, key+offset+x, ndata);
                    comps_hslist_append(rtdata->subnodes, rtd, 0);
                    rtd = comps_rtree_data_create(rt, rtdata->key+x, tmpdata);
                    comps_hslist_destroy(&rtd->subnodes);
                    rtd->subnodes = tmpnodes;
                    comps_hslist_append(rtdata->subnodes, rtd, 0);
                }
                rtdata->key = realloc(rtdata->key, sizeof(char)*(x+1));
                rtdata->key[x] = 0;
                return;
            }
        }
    }
}

void comps_rtree_set(COMPS_RTree * rt, char * key, void * data)
{
    __comps_rtree_set(rt, key, strlen(key), data);
}
void comps_rtree_set_n(COMPS_RTree * rt, char * key, size_t keylen, void * data)
{
    __comps_rtree_set(rt, key, keylen, data);
}

void* comps_rtree_get(COMPS_RTree * rt, const char * key) {
    COMPS_HSList * subnodes;
    COMPS_HSListItem * it = NULL;
    COMPS_RTreeData * rtdata;
    unsigned int offset, len, x;
    char found, ended;

    len = strlen(key);
    offset = 0;
    subnodes = rt->subnodes;
    while (offset != len) {
        found = 0;
        for (it = subnodes->first; it != NULL; it=it->next) {
            if (((COMPS_RTreeData*)it->data)->key[0] == key[offset]) {
                found = 1;
                break;
            }
        }
        if (!found) {
            //printf("not found\n");
            return NULL;
        }
        rtdata = (COMPS_RTreeData*)it->data;

        for (x=1; ;x++) {
            ended=0;
            if (x == strlen(rtdata->key)) ended += 1;
            if (x == len-offset) ended += 2;
            if (ended != 0) break;
            if (key[offset+x] != rtdata->key[x]) break;
        }
        //printf("ended :%d key :%s|\n", ended, rtdata->key);
        if (ended == 3) return rtdata->data;
        else if (ended == 1) offset+=x;
        else return NULL;
        subnodes = ((COMPS_RTreeData*)it->data)->subnodes;
    }
    if (it != NULL)
        return ((COMPS_RTreeData*)it->data)->data;
    else return NULL;
}

void comps_rtree_unset(COMPS_RTree * rt, const char * key) {
    COMPS_HSList * subnodes;
    COMPS_HSListItem * it;
    COMPS_RTreeData * rtdata;
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
            if (((COMPS_RTreeData*)it->data)->key[0] == key[offset]) {
                found = 1;
                break;
            }
        }
        if (!found) {
            comps_hslist_destroy(&path);
            return;
        }
        rtdata = (COMPS_RTreeData*)it->data;

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
                //printf("removing all\n");
                comps_hslist_remove(subnodes, it);
                comps_rtree_data_destroy(rtdata);
                free(it);
            }
            else if (rtdata->data_destructor != NULL) {
                //printf("removing data only\n");
                (*rtdata->data_destructor)(rtdata->data);
                rtdata->is_leaf = 0;
                rtdata->data = NULL;
            }

            if (path->last == NULL) {
                comps_hslist_destroy(&path);
                return;
            }
            rtdata = (COMPS_RTreeData*)
                     ((struct Relation*)path->last->data)->child_it->data;

            /*remove all predecessor of deleted node (recursive) with no childs*/
            while (rtdata->subnodes->last == NULL) {
                //printf("removing '%s'\n", rtdata->key);
                comps_rtree_data_destroy(rtdata);
                comps_hslist_remove(
                            ((struct Relation*)path->last->data)->parent_nodes,
                            ((struct Relation*)path->last->data)->child_it);
                free(((struct Relation*)path->last->data)->child_it);
                it = path->last;
                comps_hslist_remove(path, path->last);
                free(it);
                rtdata = (COMPS_RTreeData*)
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
        subnodes = ((COMPS_RTreeData*)it->data)->subnodes;
        relation->parent_nodes = subnodes;
        relation->child_it = it;
        comps_hslist_append(path, (void*)relation, 0);
    }
    comps_hslist_destroy(&path);
    return;
}

void comps_rtree_clear(COMPS_RTree * rt) {
    COMPS_HSListItem *it, *oldit;
    if (rt==NULL) return;
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

inline COMPS_HSList* __comps_rtree_all(COMPS_RTree * rt, char keyvalpair) {
    COMPS_HSList *to_process, *ret;
    COMPS_HSListItem *hsit, *oldit;
    size_t x;
    struct Pair {
        char *key;
        void *data;
        COMPS_HSList *subnodes;
    } *pair, *current_pair=NULL;//, *oldpair=NULL;
    COMPS_RTreePair *rtpair;

    to_process = comps_hslist_create();
    comps_hslist_init(to_process, NULL, NULL, &free);

    ret = comps_hslist_create();
    if (keyvalpair == 0)
        comps_hslist_init(ret, NULL, NULL, &free);
    else if (keyvalpair == 1)
        comps_hslist_init(ret, NULL, NULL, NULL);
    else
        comps_hslist_init(ret, NULL, NULL, &comps_rtree_pair_destroy_v);

    for (hsit = rt->subnodes->first; hsit != NULL; hsit = hsit->next) {
        pair = malloc(sizeof(struct Pair));
        pair->key = __comps_strcpy(((COMPS_RTreeData*)hsit->data)->key);
        pair->data = ((COMPS_RTreeData*)hsit->data)->data;
        pair->subnodes = ((COMPS_RTreeData*)hsit->data)->subnodes;
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
                rtpair = malloc(sizeof(COMPS_RTreePair));
                rtpair->key = __comps_strcpy(current_pair->key);
                rtpair->data = current_pair->data;
                comps_hslist_append(ret, rtpair, 0);
            }
        }
        for (hsit = current_pair->subnodes->first, x = 0;
             hsit != NULL; hsit = hsit->next, x++) {
            pair = malloc(sizeof(struct Pair));
            pair->key = __comps_strcat(current_pair->key,
                                       ((COMPS_RTreeData*)hsit->data)->key);
            pair->data = ((COMPS_RTreeData*)hsit->data)->data;
            pair->subnodes = ((COMPS_RTreeData*)hsit->data)->subnodes;
            comps_hslist_insert_at(to_process, x, pair, 0);
        }
        free(current_pair->key);
        free(current_pair);
        free(oldit);
    }

    comps_hslist_destroy(&to_process);
    return ret;
}

void comps_rtree_unite(COMPS_RTree *rt1, COMPS_RTree *rt2) {
    COMPS_HSList *tmplist, *tmp_subnodes;
    COMPS_HSListItem *it;
    struct Pair {
        COMPS_HSList * subnodes;
        char * key;
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

        for (it = tmp_subnodes->first; it != NULL; it=it->next) {
            pair = malloc(sizeof(struct Pair));
            pair->subnodes = ((COMPS_RTreeData*)it->data)->subnodes;

            if (parent_pair->key != NULL) {
                pair->key = malloc(sizeof(char)
                               * (strlen(((COMPS_RTreeData*)it->data)->key)
                               + strlen(parent_pair->key) + 1));
                memcpy(pair->key, parent_pair->key,
                       sizeof(char) * strlen(parent_pair->key));
                memcpy(pair->key + strlen(parent_pair->key),
                       ((COMPS_RTreeData*)it->data)->key,
                       sizeof(char)*(strlen(((COMPS_RTreeData*)it->data)->key)+1));
            } else {
                pair->key = malloc(sizeof(char)*
                                (strlen(((COMPS_RTreeData*)it->data)->key) +1));
                memcpy(pair->key, ((COMPS_RTreeData*)it->data)->key,
                       sizeof(char)*(strlen(((COMPS_RTreeData*)it->data)->key)+1));
            }
            /* current node has data */
            if (((COMPS_RTreeData*)it->data)->data != NULL) {
                    comps_rtree_set(rt1,
                                    pair->key,
                        rt2->data_cloner(((COMPS_RTreeData*)it->data)->data));
            }
            if (((COMPS_RTreeData*)it->data)->subnodes->first) {
                comps_hslist_append(tmplist, pair, 0);
            } else {
                free(pair->key);
                free(pair);
            }
        }
        free(parent_pair->key);
        free(parent_pair);
    }
    comps_hslist_destroy(&tmplist);
}

COMPS_RTree* comps_rtree_union(COMPS_RTree *rt1, COMPS_RTree *rt2){
    COMPS_RTree *ret;
    ret = comps_rtree_clone(rt2);
    comps_rtree_unite(ret, rt1);
    return ret;
}


COMPS_HSList* comps_rtree_keys(COMPS_RTree * rt) {
    return __comps_rtree_all(rt, 0);
}

COMPS_HSList* comps_rtree_values(COMPS_RTree * rt) {
    return __comps_rtree_all(rt, 1);
}

COMPS_HSList* comps_rtree_pairs(COMPS_RTree * rt) {
    return __comps_rtree_all(rt, 2);
}

inline void comps_rtree_pair_destroy(COMPS_RTreePair * pair) {
    free(pair->key);
    free(pair);
}

inline void comps_rtree_pair_destroy_v(void * pair) {
    free(((COMPS_RTreePair *)pair)->key);
    free(pair);
}
