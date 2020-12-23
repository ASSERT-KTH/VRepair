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

#include "comps_objradix.h"
#include "comps_set.h"
#include <stdio.h>

void comps_objrtree_data_destroy(COMPS_ObjRTreeData * rtd) {
    free(rtd->key);
    comps_object_destroy(rtd->data);
    comps_hslist_destroy(&rtd->subnodes);
    free(rtd);
}

inline void comps_objrtree_data_destroy_v(void * rtd) {
    comps_objrtree_data_destroy((COMPS_ObjRTreeData*)rtd);
}

inline COMPS_ObjRTreeData * __comps_objrtree_data_create(char *key,
                                                   size_t keylen,
                                                   COMPS_Object *data){
    COMPS_ObjRTreeData * rtd;
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
    rtd->subnodes = comps_hslist_create();
    comps_hslist_init(rtd->subnodes, NULL, NULL, &comps_objrtree_data_destroy_v);
    return rtd;
}

COMPS_ObjRTreeData * comps_objrtree_data_create(char *key, COMPS_Object *data) {
    COMPS_ObjRTreeData * rtd;
    rtd = __comps_objrtree_data_create(key, strlen(key), data);
    return rtd;
}

COMPS_ObjRTreeData * comps_objrtree_data_create_n(char *key, size_t keylen,
                                                  COMPS_Object *data) {
    COMPS_ObjRTreeData * rtd;
    rtd = __comps_objrtree_data_create(key, keylen, data);
    return rtd;
}

static void comps_objrtree_create(COMPS_ObjRTree *rtree, COMPS_Object **args) {
    (void)args;
    rtree->subnodes = comps_hslist_create();
    comps_hslist_init(rtree->subnodes, NULL, NULL, &comps_objrtree_data_destroy_v);
    if (rtree->subnodes == NULL) {
        COMPS_OBJECT_DESTROY(rtree);
        return;
    }
    rtree->len = 0;
}
void comps_objrtree_create_u(COMPS_Object * obj, COMPS_Object **args) {
    (void)args;
    comps_objrtree_create((COMPS_ObjRTree*)obj, NULL);
}

static void comps_objrtree_destroy(COMPS_ObjRTree * rt) {
    comps_hslist_destroy(&(rt->subnodes));
}
void comps_objrtree_destroy_u(COMPS_Object *obj) {
    comps_objrtree_destroy((COMPS_ObjRTree*)obj);
}

COMPS_ObjRTree * comps_objrtree_clone(COMPS_ObjRTree *rt) {

    COMPS_HSList *to_clone, *tmplist, *new_subnodes;
    COMPS_ObjRTree *ret;
    COMPS_HSListItem *it, *it2;
    COMPS_ObjRTreeData *rtdata;
    COMPS_Object *new_data;

    if (!rt) return NULL;

    to_clone = comps_hslist_create();
    comps_hslist_init(to_clone, NULL, NULL, NULL);
    ret = COMPS_OBJECT_CREATE(COMPS_ObjRTree, NULL);
    ret->len = rt->len;

    for (it = rt->subnodes->first; it != NULL; it = it->next) {
        rtdata = comps_objrtree_data_create(
                                    ((COMPS_ObjRTreeData*)it->data)->key, NULL);
        if (((COMPS_ObjRTreeData*)it->data)->data != NULL)
            new_data = comps_object_copy(((COMPS_ObjRTreeData*)it->data)->data);
        else
            new_data = NULL;
        comps_hslist_destroy(&rtdata->subnodes);
        rtdata->subnodes = ((COMPS_ObjRTreeData*)it->data)->subnodes;
        rtdata->data = new_data;
        comps_hslist_append(ret->subnodes, rtdata, 0);
        comps_hslist_append(to_clone, rtdata, 0);
    }

    while (to_clone->first) {
        it2 = to_clone->first;
        tmplist = ((COMPS_ObjRTreeData*)it2->data)->subnodes;
        comps_hslist_remove(to_clone, to_clone->first);

        new_subnodes = comps_hslist_create();
        comps_hslist_init(new_subnodes, NULL, NULL, &comps_objrtree_data_destroy_v);
        for (it = tmplist->first; it != NULL; it = it->next) {
            rtdata = comps_objrtree_data_create(
                                      ((COMPS_ObjRTreeData*)it->data)->key, NULL);
            if (((COMPS_ObjRTreeData*)it->data)->data != NULL)
                new_data = comps_object_copy(((COMPS_ObjRTreeData*)it->data)->data);
            else
                new_data = NULL;
            comps_hslist_destroy(&rtdata->subnodes);
            rtdata->subnodes = ((COMPS_ObjRTreeData*)it->data)->subnodes;
            rtdata->data = new_data;
            comps_hslist_append(new_subnodes, rtdata, 0);
            comps_hslist_append(to_clone, rtdata, 0);
        }
        ((COMPS_ObjRTreeData*)it2->data)->subnodes = new_subnodes;
        free(it2);
    }
    comps_hslist_destroy(&to_clone);
    return ret;
}
void comps_objrtree_copy(COMPS_ObjRTree *rt1, COMPS_ObjRTree *rt2){
    COMPS_HSList *to_clone, *tmplist, *new_subnodes;
    COMPS_HSListItem *it, *it2;
    COMPS_ObjRTreeData *rtdata;
    COMPS_Object *new_data;

    rt1->subnodes = comps_hslist_create();
    comps_hslist_init(rt1->subnodes, NULL, NULL, &comps_objrtree_data_destroy_v);
    if (rt1->subnodes == NULL) {
        COMPS_OBJECT_DESTROY(rt1);
        return;
    }
    rt1->len = 0;

    to_clone = comps_hslist_create();
    comps_hslist_init(to_clone, NULL, NULL, NULL);

    for (it = rt2->subnodes->first; it != NULL; it = it->next) {
        rtdata = comps_objrtree_data_create(
                                    ((COMPS_ObjRTreeData*)it->data)->key, NULL);
        if (((COMPS_ObjRTreeData*)it->data)->data != NULL)
            new_data = comps_object_copy(((COMPS_ObjRTreeData*)it->data)->data);
        else
            new_data = NULL;
        comps_hslist_destroy(&rtdata->subnodes);
        rtdata->subnodes = ((COMPS_ObjRTreeData*)it->data)->subnodes;
        rtdata->data = new_data;
        comps_hslist_append(rt1->subnodes, rtdata, 0);
        comps_hslist_append(to_clone, rtdata, 0);
    }

    while (to_clone->first) {
        it2 = to_clone->first;
        tmplist = ((COMPS_ObjRTreeData*)it2->data)->subnodes;
        comps_hslist_remove(to_clone, to_clone->first);

        new_subnodes = comps_hslist_create();
        comps_hslist_init(new_subnodes, NULL, NULL, &comps_objrtree_data_destroy_v);
        for (it = tmplist->first; it != NULL; it = it->next) {
            rtdata = comps_objrtree_data_create(
                                      ((COMPS_ObjRTreeData*)it->data)->key, NULL);
            if (((COMPS_ObjRTreeData*)it->data)->data != NULL)
                new_data = comps_object_copy(((COMPS_ObjRTreeData*)it->data)->data);
            else
                new_data = NULL;
            comps_hslist_destroy(&rtdata->subnodes);
            rtdata->subnodes = ((COMPS_ObjRTreeData*)it->data)->subnodes;
            rtdata->data = new_data;
            comps_hslist_append(new_subnodes, rtdata, 0);
            comps_hslist_append(to_clone, rtdata, 0);
        }
        ((COMPS_ObjRTreeData*)it2->data)->subnodes = new_subnodes;
        free(it2);
    }
    comps_hslist_destroy(&to_clone);

}
COMPS_COPY_u(objrtree, COMPS_ObjRTree) /*comps_utils.h macro*/

void comps_objrtree_copy_shallow(COMPS_ObjRTree *rt1, COMPS_ObjRTree *rt2){
    COMPS_HSList *to_clone, *tmplist, *new_subnodes;
    COMPS_HSListItem *it, *it2;
    COMPS_ObjRTreeData *rtdata;
    COMPS_Object *new_data;

    rt1->subnodes = comps_hslist_create();
    comps_hslist_init(rt1->subnodes, NULL, NULL, &comps_objrtree_data_destroy_v);
    if (rt1->subnodes == NULL) {
        COMPS_OBJECT_DESTROY(rt1);
        return;
    }
    rt1->len = 0;

    to_clone = comps_hslist_create();
    comps_hslist_init(to_clone, NULL, NULL, NULL);

    for (it = rt2->subnodes->first; it != NULL; it = it->next) {
        rtdata = comps_objrtree_data_create(
                                    ((COMPS_ObjRTreeData*)it->data)->key, NULL);
        if (((COMPS_ObjRTreeData*)it->data)->data != NULL)
            new_data = COMPS_OBJECT_INCREF(((COMPS_ObjRTreeData*)it->data)->data);
        else
            new_data = NULL;
        comps_hslist_destroy(&rtdata->subnodes);
        rtdata->subnodes = ((COMPS_ObjRTreeData*)it->data)->subnodes;
        rtdata->data = new_data;
        comps_hslist_append(rt1->subnodes, rtdata, 0);
        comps_hslist_append(to_clone, rtdata, 0);
    }

    while (to_clone->first) {
        it2 = to_clone->first;
        tmplist = ((COMPS_ObjRTreeData*)it2->data)->subnodes;
        comps_hslist_remove(to_clone, to_clone->first);

        new_subnodes = comps_hslist_create();
        comps_hslist_init(new_subnodes, NULL, NULL, &comps_objrtree_data_destroy_v);
        for (it = tmplist->first; it != NULL; it = it->next) {
            rtdata = comps_objrtree_data_create(
                                      ((COMPS_ObjRTreeData*)it->data)->key, NULL);
            if (((COMPS_ObjRTreeData*)it->data)->data != NULL)
                new_data = comps_object_incref(((COMPS_ObjRTreeData*)it->data)->data);
            else
                new_data = NULL;
            comps_hslist_destroy(&rtdata->subnodes);
            rtdata->subnodes = ((COMPS_ObjRTreeData*)it->data)->subnodes;
            rtdata->data = new_data;
            comps_hslist_append(new_subnodes, rtdata, 0);
            comps_hslist_append(to_clone, rtdata, 0);
        }
        ((COMPS_ObjRTreeData*)it2->data)->subnodes = new_subnodes;
        free(it2);
    }
    comps_hslist_destroy(&to_clone);
}

void comps_objrtree_values_walk(COMPS_ObjRTree * rt, void* udata,
                              void (*walk_f)(void*, COMPS_Object*)) {
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
            if (((COMPS_ObjRTreeData*)it->data)->subnodes->first) {
                comps_hslist_append(tmplist,
                                    ((COMPS_ObjRTreeData*)it->data)->subnodes, 0);
            }
            if (((COMPS_ObjRTreeData*)it->data)->data != NULL) {
               walk_f(udata, ((COMPS_ObjRTreeData*)it->data)->data);
            }
        }
    }
    comps_hslist_destroy(&tmplist);
}

char comps_objrtree_paircmp(void *obj1, void *obj2) {
    //printf("comparing %s with %s\n", ((COMPS_ObjRTreePair*)obj1)->key,
    //                               ((COMPS_ObjRTreePair*)obj2)->key);
    if (strcmp(((COMPS_ObjRTreePair*)obj1)->key,
               ((COMPS_ObjRTreePair*)obj2)->key) != 0)
        return 0;
    return comps_object_cmp(((COMPS_ObjRTreePair*)obj1)->data,
                            ((COMPS_ObjRTreePair*)obj2)->data);
}


signed char comps_objrtree_cmp(COMPS_ObjRTree *ort1, COMPS_ObjRTree *ort2) {
    COMPS_HSList *values1, *values2;
    COMPS_HSListItem *it;
    COMPS_Set *set1, *set2;
    signed char ret;
    values1 = comps_objrtree_pairs(ort1);
    values2 = comps_objrtree_pairs(ort2);
    set1 = comps_set_create();
    comps_set_init(set1, NULL, NULL, NULL, &comps_objrtree_paircmp);
    set2 = comps_set_create();
    comps_set_init(set2, NULL, NULL, NULL, &comps_objrtree_paircmp);
    for (it = values1->first; it != NULL; it = it->next) {
        comps_set_add(set1, it->data);
    }
    for (it = values2->first; it != NULL; it = it->next) {
        comps_set_add(set2, it->data);
    }

    ret = comps_set_cmp(set1, set2);
    comps_set_destroy(&set1);
    comps_set_destroy(&set2);
    //printf("objrtree cmp %d\n", !ret);
    
    //char *str;
    /*for (it = values1->first; it != NULL; it = it->next) {
        str = comps_object_tostr(((COMPS_ObjRTreePair*)it->data)->data);
        printf("dict item %s=%s\n", ((COMPS_ObjRTreePair*)it->data)->key, str);
        free(str);
    }
    printf("----------\n");
    for (it = values2->first; it != NULL; it = it->next) {
        str = comps_object_tostr(((COMPS_ObjRTreePair*)it->data)->data);
        printf("dict item %s=%s\n", ((COMPS_ObjRTreePair*)it->data)->key, str);
        free(str);
    }
    printf("cmp objrtree ret:%d\n", ret);*/
    comps_hslist_destroy(&values1);
    comps_hslist_destroy(&values2);
    return ret==0;
}
COMPS_CMP_u(objrtree, COMPS_ObjRTree)

void __comps_objrtree_set(COMPS_ObjRTree *rt, char *key, size_t len,
                          COMPS_Object *ndata) {

    COMPS_HSListItem *it, *lesser;
    COMPS_HSList *subnodes;
    COMPS_ObjRTreeData *rtd;
    static COMPS_ObjRTreeData *rtdata;

    size_t _len, offset=0;
    unsigned x, found = 0;
    char ended;

    //len = strlen(key);

    if (rt->subnodes == NULL)
        return;

    subnodes = rt->subnodes;
    while (offset != len)
    {
        found = 0;
        lesser = NULL;
        for (it = subnodes->first; it != NULL; it=it->next) {
            if (((COMPS_ObjRTreeData*)it->data)->key[0] == key[offset]) {
                found = 1;
                break;
            } else if (((COMPS_ObjRTreeData*)it->data)->key[0] < key[offset]) {
                lesser = it;
            }
        }
        if (!found) { // not found in subnodes; create new subnode
            rtd = comps_objrtree_data_create_n(key+offset, len-offset, ndata);
            if (!lesser) {
                comps_hslist_prepend(subnodes, rtd, 0);
            } else {
                comps_hslist_insert_after(subnodes, lesser, rtd, 0);
            }
            rt->len++;
            return;
        } else {
            rtdata = (COMPS_ObjRTreeData*)it->data;
            ended = 0;
            for (x=1; ;x++) {
                if (rtdata->key[x] == 0) ended += 1;
                if (x == len - offset) ended += 2;
                if (ended != 0) break;
                if (key[offset+x] != rtdata->key[x]) break;
            }
            if (ended == 3) { //keys equals; data replacement
                comps_object_destroy(rtdata->data);
                rtdata->data = ndata;
                return;
            } else if (ended == 2) { //global key ends first; make global leaf
                //printf("ended2\n");
                comps_hslist_remove(subnodes, it);
                it->next = NULL;
                rtd = comps_objrtree_data_create_n(key+offset, len-offset, ndata);
                comps_hslist_append(subnodes, rtd, 0);
                ((COMPS_ObjRTreeData*)subnodes->last->data)->subnodes->last = it;
                ((COMPS_ObjRTreeData*)subnodes->last->data)->subnodes->first = it;
                _len = len - offset;

                memmove(rtdata->key,rtdata->key+_len,
                        strlen(rtdata->key) - _len);
                rtdata->key[strlen(rtdata->key) - _len] = 0;
                rtdata->key = realloc(rtdata->key,
                                      sizeof(char)* (strlen(rtdata->key)+1));
                rt->len++;
                return;
            } else if (ended == 1) { //local key ends first; go deeper
                subnodes = rtdata->subnodes;
                offset += x;
            } else {
                COMPS_Object *tmpdata = rtdata->data;
                COMPS_HSList *tmphslist = rtdata->subnodes;
                //tmpch = rtdata->key[x];             // split mutual key
                rtdata->subnodes = comps_hslist_create();
                comps_hslist_init(rtdata->subnodes, NULL, NULL,
                                  &comps_objrtree_data_destroy_v);
                int cmpret = strcmp(key+offset+x, rtdata->key+x);
                rtdata->data = NULL;

                if (cmpret > 0) {
                    rtd = comps_objrtree_data_create(rtdata->key+x, tmpdata);
                    comps_hslist_destroy(&rtd->subnodes);
                    rtd->subnodes = tmphslist;

                    comps_hslist_append(rtdata->subnodes,rtd, 0);
                    rtd = comps_objrtree_data_create(key+offset+x, ndata);
                    comps_hslist_append(rtdata->subnodes, rtd, 0);

                } else {
                    rtd = comps_objrtree_data_create(key+offset+x, ndata);
                    comps_hslist_append(rtdata->subnodes, rtd, 0);
                    rtd = comps_objrtree_data_create(rtdata->key+x, tmpdata);
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

void comps_objrtree_set_x(COMPS_ObjRTree *rt, char *key, COMPS_Object *data) {
    __comps_objrtree_set(rt, key, strlen(key), data);
}
void comps_objrtree_set(COMPS_ObjRTree *rt, char *key, COMPS_Object *data) {
    __comps_objrtree_set(rt, key, strlen(key), comps_object_incref(data));
}
void comps_objrtree_set_n(COMPS_ObjRTree *rt, char *key, size_t len,
                          COMPS_Object *data) {
    __comps_objrtree_set(rt, key, len, data);
}
void comps_objrtree_set_nx(COMPS_ObjRTree *rt, char *key, size_t len,
                           COMPS_Object *data) {
    __comps_objrtree_set(rt, key, len, comps_object_incref(data));
}

COMPS_Object* __comps_objrtree_get(COMPS_ObjRTree * rt, const char * key) {
    COMPS_HSList * subnodes;
    COMPS_HSListItem * it = NULL;
    COMPS_ObjRTreeData * rtdata;
    unsigned int offset, len, x;
    char found, ended;

    len = strlen(key);
    offset = 0;
    subnodes = rt->subnodes;

    while (offset != len) {
        found = 0;
        for (it = subnodes->first; it != NULL; it=it->next) {
            if (((COMPS_ObjRTreeData*)it->data)->key[0] == key[offset]) {
                found = 1;
                break;
            }
        }
        if (!found) {
            return NULL;
        }
        rtdata = (COMPS_ObjRTreeData*)it->data;

        for (x=1; ;x++) {
            ended=0;
            if (x == strlen(rtdata->key)) ended += 1;
            if (x == len-offset) ended += 2;
            if (ended != 0) break;
            if (key[offset+x] != rtdata->key[x]) break;
        }
        if (ended == 3) {
            return rtdata->data;
        }
        else if (ended == 1) offset+=x;
        else {
            return NULL;
        }
        subnodes = ((COMPS_ObjRTreeData*)it->data)->subnodes;
    }
    if (it != NULL) {
        return ((COMPS_ObjRTreeData*)it->data)->data;
    }
    else {
        return NULL;
    }
}
COMPS_Object* comps_objrtree_get(COMPS_ObjRTree * rt, const char * key) {
    return comps_object_incref(__comps_objrtree_get(rt, key));
}
COMPS_Object* comps_objrtree_get_x(COMPS_ObjRTree * rt, const char * key) {
    return __comps_objrtree_get(rt, key);
}

void comps_objrtree_unset(COMPS_ObjRTree * rt, const char * key) {
    COMPS_HSList * subnodes;
    COMPS_HSListItem * it;
    COMPS_ObjRTreeData * rtdata;
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
            if (((COMPS_ObjRTreeData*)it->data)->key[0] == key[offset]) {
                found = 1;
                break;
            }
        }
        if (!found) {
            comps_hslist_destroy(&path);
            return;
        }
        rtdata = (COMPS_ObjRTreeData*)it->data;

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
                comps_objrtree_data_destroy(rtdata);
                free(it);
            }
            else {
                //printf("removing data only\n");
                comps_object_destroy(rtdata->data);
                rtdata->is_leaf = 0;
                rtdata->data = NULL;
            }

            if (path->last == NULL) {
                comps_hslist_destroy(&path);
                return;
            }
            rtdata = (COMPS_ObjRTreeData*)
                     ((struct Relation*)path->last->data)->child_it->data;

            /*remove all predecessor of deleted node (recursive) with no childs*/
            while (rtdata->subnodes->last == NULL) {
                //printf("removing '%s'\n", rtdata->key);
                comps_objrtree_data_destroy(rtdata);
                comps_hslist_remove(
                            ((struct Relation*)path->last->data)->parent_nodes,
                            ((struct Relation*)path->last->data)->child_it);
                free(((struct Relation*)path->last->data)->child_it);
                it = path->last;
                comps_hslist_remove(path, path->last);
                free(it);
                rtdata = (COMPS_ObjRTreeData*)
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
        subnodes = ((COMPS_ObjRTreeData*)it->data)->subnodes;
        relation->parent_nodes = subnodes;
        relation->child_it = it;
        comps_hslist_append(path, (void*)relation, 0);
    }
    comps_hslist_destroy(&path);
    return;
}

void comps_objrtree_clear(COMPS_ObjRTree * rt) {
    COMPS_HSListItem *it, *oldit;
    if (rt==NULL) return;
    if (rt->subnodes == NULL) return;
    oldit = rt->subnodes->first;
    it = (oldit)?oldit->next:NULL;
    for (;it != NULL; it=it->next) {
        comps_object_destroy(oldit->data);
        free(oldit);
        oldit = it;
    }
    if (oldit) {
        comps_object_destroy(oldit->data);
        free(oldit);
    }
}

inline COMPS_HSList* __comps_objrtree_all(COMPS_ObjRTree * rt, char keyvalpair) {
    COMPS_HSList *to_process, *ret;
    COMPS_HSListItem *hsit, *oldit;
    size_t x;
    struct Pair {
        char *key;
        void *data;
        COMPS_HSList *subnodes;
    } *pair, *current_pair=NULL;//, *oldpair=NULL;
    COMPS_ObjRTreePair *rtpair;

    to_process = comps_hslist_create();
    comps_hslist_init(to_process, NULL, NULL, &free);

    ret = comps_hslist_create();
    if (keyvalpair == 0)
        comps_hslist_init(ret, NULL, NULL, &free);
    else if (keyvalpair == 1)
        comps_hslist_init(ret, NULL, NULL, NULL);
    else
        comps_hslist_init(ret, NULL, NULL, &comps_objrtree_pair_destroy_v);

    for (hsit = rt->subnodes->first; hsit != NULL; hsit = hsit->next) {
        pair = malloc(sizeof(struct Pair));
        pair->key = __comps_strcpy(((COMPS_ObjRTreeData*)hsit->data)->key);
        pair->data = ((COMPS_ObjRTreeData*)hsit->data)->data;
        pair->subnodes = ((COMPS_ObjRTreeData*)hsit->data)->subnodes;
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
                rtpair = malloc(sizeof(COMPS_ObjRTreePair));
                rtpair->key = __comps_strcpy(current_pair->key);
                rtpair->data = current_pair->data;
                comps_hslist_append(ret, rtpair, 0);
            }
        }
        for (hsit = current_pair->subnodes->first, x = 0;
             hsit != NULL; hsit = hsit->next, x++) {
            pair = malloc(sizeof(struct Pair));
            pair->key = __comps_strcat(current_pair->key,
                                       ((COMPS_ObjRTreeData*)hsit->data)->key);
            pair->data = ((COMPS_ObjRTreeData*)hsit->data)->data;
            pair->subnodes = ((COMPS_ObjRTreeData*)hsit->data)->subnodes;
            comps_hslist_insert_at(to_process, x, pair, 0);
        }
        free(current_pair->key);
        free(current_pair);
        free(oldit);
    }

    comps_hslist_destroy(&to_process);
    return ret;
}

void comps_objrtree_unite(COMPS_ObjRTree *rt1, COMPS_ObjRTree *rt2) {
    COMPS_HSList *tmplist, *tmp_subnodes;
    COMPS_HSListItem *it;
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
        //printf("key-part:%s\n", parent_pair->key);
        free(it);

        //pair->added = 0;
        for (it = tmp_subnodes->first; it != NULL; it=it->next) {
            pair = malloc(sizeof(struct Pair));
            pair->subnodes = ((COMPS_ObjRTreeData*)it->data)->subnodes;

            if (parent_pair->key != NULL) {
                pair->key = malloc(sizeof(char)
                               * (strlen(((COMPS_ObjRTreeData*)it->data)->key)
                               + strlen(parent_pair->key) + 1));
                memcpy(pair->key, parent_pair->key,
                       sizeof(char) * strlen(parent_pair->key));
                memcpy(pair->key + strlen(parent_pair->key),
                       ((COMPS_ObjRTreeData*)it->data)->key,
                       sizeof(char)*(strlen(((COMPS_ObjRTreeData*)it->data)->key)+1));
            } else {
                pair->key = malloc(sizeof(char)*
                                (strlen(((COMPS_ObjRTreeData*)it->data)->key) +1));
                memcpy(pair->key, ((COMPS_ObjRTreeData*)it->data)->key,
                       sizeof(char)*(strlen(((COMPS_ObjRTreeData*)it->data)->key)+1));
            }
            /* current node has data */
            if (((COMPS_ObjRTreeData*)it->data)->data != NULL) {
                    comps_objrtree_set(rt1, pair->key,
                                      (((COMPS_ObjRTreeData*)it->data)->data));
            }
            if (((COMPS_ObjRTreeData*)it->data)->subnodes->first) {
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

COMPS_ObjRTree* comps_objrtree_union(COMPS_ObjRTree *rt1, COMPS_ObjRTree *rt2){
    COMPS_ObjRTree *ret;
    ret = comps_objrtree_clone(rt1);
    comps_objrtree_unite(ret, rt2);
    return ret;
}


COMPS_HSList* comps_objrtree_keys(COMPS_ObjRTree * rt) {
    return __comps_objrtree_all(rt, 0);
}

COMPS_HSList* comps_objrtree_values(COMPS_ObjRTree * rt) {
    return __comps_objrtree_all(rt, 1);
}

COMPS_HSList* comps_objrtree_pairs(COMPS_ObjRTree * rt) {
    return __comps_objrtree_all(rt, 2);
}


inline void comps_objrtree_pair_destroy(COMPS_ObjRTreePair * pair) {
    free(pair->key);
    free(pair);
}

inline void comps_objrtree_pair_destroy_v(void * pair) {
    free(((COMPS_ObjRTreePair *)pair)->key);
    free(pair);
}

COMPS_ObjectInfo COMPS_ObjRTree_ObjInfo = {
    .obj_size = sizeof(COMPS_ObjRTree),
    .constructor = &comps_objrtree_create_u,
    .destructor = &comps_objrtree_destroy_u,
    .copy = &comps_objrtree_copy_u,
    .obj_cmp = &comps_objrtree_cmp_u
};
