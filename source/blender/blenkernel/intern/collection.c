/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Dalai Felinto
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/blenkernel/intern/collection.c
 *  \ingroup bke
 */

#include <string.h>

#include "BLI_blenlib.h"
#include "BLI_ghash.h"
#include "BLI_iterator.h"
#include "BLI_listbase.h"
#include "BLT_translation.h"
#include "BLI_string_utils.h"

#include "BKE_collection.h"
#include "BKE_group.h"
#include "BKE_idprop.h"
#include "BKE_layer.h"
#include "BKE_library.h"
#include "BKE_main.h"
#include "BKE_scene.h"

#include "DNA_group_types.h"
#include "DNA_ID.h"
#include "DNA_layer_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "MEM_guardedalloc.h"

/* Prototypes. */
static bool is_collection_in_tree(const struct SceneCollection *sc_reference, struct SceneCollection *sc_parent);

static SceneCollection *collection_master_from_id(const ID *id)
{
	switch (GS(id->name)) {
		case ID_SCE:
			return ((Scene *)id)->collection;
		case ID_GR:
			return ((Group *)id)->collection;
		default:
			BLI_assert(!"ID doesn't support collections");
			return NULL;
	}
}

/**
 * Add a collection to a collection ListBase and syncronize all render layers
 * The ListBase is NULL when the collection is to be added to the master collection
 */
SceneCollection *BKE_collection_add(ID *id, SceneCollection *sc_parent, const int type, const char *name)
{
	SceneCollection *sc_master = collection_master_from_id(id);
	SceneCollection *sc = MEM_callocN(sizeof(SceneCollection), "New Collection");
	sc->type = type;

	if (!name) {
		name = DATA_("New Collection");
	}

	if (!sc_parent) {
		sc_parent = sc_master;
	}

	BKE_collection_rename((Scene *)id, sc, name);
	BLI_addtail(&sc_parent->scene_collections, sc);

	BKE_layer_sync_new_scene_collection(id, sc_parent, sc);
	return sc;
}

/**
 * Free the collection items recursively
 */
static void collection_free(SceneCollection *sc, const bool do_id_user)
{
	if (do_id_user) {
		for (LinkData *link = sc->objects.first; link; link = link->next) {
			id_us_min(link->data);
		}
		for (LinkData *link = sc->filter_objects.first; link; link = link->next) {
			id_us_min(link->data);
		}
	}

	BLI_freelistN(&sc->objects);
	BLI_freelistN(&sc->filter_objects);

	for (SceneCollection *nsc = sc->scene_collections.first; nsc; nsc = nsc->next) {
		collection_free(nsc, do_id_user);
	}
	BLI_freelistN(&sc->scene_collections);
}

/**
 * Unlink the collection recursively
 * \return true if unlinked.
 */
static bool collection_remlink(SceneCollection *sc_parent, SceneCollection *sc_gone)
{
	for (SceneCollection *sc = sc_parent->scene_collections.first; sc; sc = sc->next) {
		if (sc == sc_gone) {
			BLI_remlink(&sc_parent->scene_collections, sc_gone);
			return true;
		}

		if (collection_remlink(sc, sc_gone)) {
			return true;
		}
	}
	return false;
}

/**
 * Recursively remove any instance of this SceneCollection
 */
static void layer_collection_remove(SceneLayer *sl, ListBase *lb, const SceneCollection *sc)
{
	LayerCollection *lc = lb->first;
	while (lc) {
		if (lc->scene_collection == sc) {
			BKE_layer_collection_free(sl, lc);
			BLI_remlink(lb, lc);

			LayerCollection *lc_next = lc->next;
			MEM_freeN(lc);
			lc = lc_next;

			/* only the "top-level" layer collections may have the
			 * same SceneCollection in a sibling tree.
			 */
			if (lb != &sl->layer_collections) {
				return;
			}
		}

		else {
			layer_collection_remove(sl, &lc->layer_collections, sc);
			lc = lc->next;
		}
	}
}

/**
 * Remove a collection from the scene, and syncronize all render layers
 */
bool BKE_collection_remove(Scene *scene, SceneCollection *sc)
{
	SceneCollection *sc_master = BKE_collection_master(scene);

	/* the master collection cannot be removed */
	if (sc == sc_master) {
		return false;
	}

	/* unlink from the respective collection tree */
	if (!collection_remlink(sc_master, sc)) {
		BLI_assert(false);
	}

	/* clear the collection items */
	collection_free(sc, true);

	/* check all layers that use this collection and clear them */
	for (SceneLayer *sl = scene->render_layers.first; sl; sl = sl->next) {
		layer_collection_remove(sl, &sl->layer_collections, sc);
		sl->active_collection = 0;
	}

	MEM_freeN(sc);
	return true;
}

/**
 * Copy SceneCollection tree but keep pointing to the same objects
 *
 * \param flag  Copying options (see BKE_library.h's LIB_ID_COPY_... flags for more).
 */
void BKE_collection_copy_data(SceneCollection *sc_dst, SceneCollection *sc_src, const int flag)
{
	BLI_duplicatelist(&sc_dst->objects, &sc_src->objects);
	if ((flag & LIB_ID_CREATE_NO_USER_REFCOUNT) == 0) {
		for (LinkData *link = sc_dst->objects.first; link; link = link->next) {
			id_us_plus(link->data);
		}
	}

	BLI_duplicatelist(&sc_dst->filter_objects, &sc_src->filter_objects);
	if ((flag & LIB_ID_CREATE_NO_USER_REFCOUNT) == 0) {
		for (LinkData *link = sc_dst->filter_objects.first; link; link = link->next) {
			id_us_plus(link->data);
		}
	}

	BLI_duplicatelist(&sc_dst->scene_collections, &sc_src->scene_collections);
	for (SceneCollection *nsc_src = sc_src->scene_collections.first, *nsc_dst = sc_dst->scene_collections.first;
	     nsc_src;
	     nsc_src = nsc_src->next, nsc_dst = nsc_dst->next)
	{
		BKE_collection_copy_data(nsc_dst, nsc_src, flag);
	}
}

/**
 * Returns the master collection
 */
SceneCollection *BKE_collection_master(const Scene *scene)
{
	return scene->collection;
}

/**
 * Returns the master collection of the group
 */
SceneCollection *BKE_collection_group_master(const Group *group)
{
	return group->collection;
}

static SceneCollection *master_collection_from_id(const ID *id)
{
	switch (GS(id->name)) {
		case ID_SCE:
			return BKE_collection_master((const Scene *)id);
		case ID_GR:
			return BKE_collection_group_master((const Group *)id);
		default:
			BLI_assert(!"ID doesn't support scene collection");
			return NULL;
	}
}

struct UniqueNameCheckData {
	ListBase *lb;
	SceneCollection *lookup_sc;
};

static bool collection_unique_name_check(void *arg, const char *name)
{
	struct UniqueNameCheckData *data = arg;

	for (SceneCollection *sc = data->lb->first; sc; sc = sc->next) {
		struct UniqueNameCheckData child_data = {.lb = &sc->scene_collections, .lookup_sc = data->lookup_sc};

		if (sc != data->lookup_sc) {
			if (STREQ(sc->name, name)) {
				return true;
			}
		}
		if (collection_unique_name_check(&child_data, name)) {
			return true;
		}
	}

	return false;
}

static void collection_rename(const ID *id, SceneCollection *sc, const char *name)
{
	SceneCollection *sc_master = collection_master_from_id(id);
	struct UniqueNameCheckData data = {.lb = &sc_master->scene_collections, .lookup_sc = sc};

	BLI_strncpy(sc->name, name, sizeof(sc->name));
	BLI_uniquename_cb(collection_unique_name_check, &data, DATA_("Collection"), '.', sc->name, sizeof(sc->name));
}

void BKE_collection_rename(const Scene *scene, SceneCollection *sc, const char *name)
{
	collection_rename(&scene->id, sc, name);
}

/**
 * Free (or release) any data used by the master collection (does not free the master collection itself).
 * Used only to clear the entire scene data since it's not doing re-syncing of the LayerCollection tree
 */
void BKE_collection_master_free(Scene *scene, const bool do_id_user)
{
	collection_free(BKE_collection_master(scene), do_id_user);
}

void BKE_collection_master_group_free(Group *group)
{
	collection_free(BKE_collection_group_master(group), false);
}

static void collection_object_add(const ID *id, SceneCollection *sc, Object *ob)
{
	BLI_addtail(&sc->objects, BLI_genericNodeN(ob));

	if (GS(id->name) == ID_SCE) {
		id_us_plus((ID *)ob);
	}
	else {
		BLI_assert(GS(id->name) == ID_GR);
	}

	BKE_layer_sync_object_link(id, sc, ob);
}

/**
 * Add object to collection
 */
bool BKE_collection_object_add(const ID *id, SceneCollection *sc, Object *ob)
{
	if (BLI_findptr(&sc->objects, ob, offsetof(LinkData, data))) {
		/* don't add the same object twice */
		return false;
	}

	collection_object_add(id, sc, ob);
	return true;
}

/**
 * Add object to all collections that reference objects is in
 * (used to copy objects)
 */
void BKE_collection_object_add_from(Scene *scene, Object *ob_src, Object *ob_dst)
{
	FOREACH_SCENE_COLLECTION(scene, sc)
	{
		if (BLI_findptr(&sc->objects, ob_src, offsetof(LinkData, data))) {
			collection_object_add(&scene->id, sc, ob_dst);
		}
	}
	FOREACH_SCENE_COLLECTION_END

	for (SceneLayer *sl = scene->render_layers.first; sl; sl = sl->next) {
		Base *base_src = BKE_scene_layer_base_find(sl, ob_src);
		if (base_src != NULL) {
			if (base_src->collection_properties == NULL) {
				continue;
			}
			Base *base_dst = BKE_scene_layer_base_find(sl, ob_dst);
			IDP_MergeGroup(base_dst->collection_properties, base_src->collection_properties, true);
		}
	}
}

/**
 * Remove object from collection.
 * \param bmain: Can be NULL if free_us is false.
 */
bool BKE_collection_object_remove(Main *bmain, ID *id, SceneCollection *sc, Object *ob, const bool free_us)
{
	LinkData *link = BLI_findptr(&sc->objects, ob, offsetof(LinkData, data));

	if (link == NULL) {
		return false;
	}

	BLI_remlink(&sc->objects, link);
	MEM_freeN(link);

	TODO_LAYER_SYNC_FILTER; /* need to remove all instances of ob in scene collections -> filter_objects */
	BKE_layer_sync_object_unlink(id, sc, ob);

	if (free_us) {
		BKE_libblock_free_us(bmain, ob);
	}
	else {
		if (GS(id->name) == ID_SCE) {
			id_us_min(&ob->id);
		}
		else {
			BLI_assert(GS(id->name) == ID_GR);
		}
	}
	return true;
}

/**
 * Move object from a collection into another
 */
void BKE_collection_object_move(Scene *scene, SceneCollection *sc_dst, SceneCollection *sc_src, Object *ob)
{
	BKE_collection_object_add(&scene->id, sc_dst, ob);
	BKE_collection_object_remove(NULL, &scene->id, sc_src, ob, false);
}

/**
 * Remove object from all collections of scene
 */
bool BKE_collections_object_remove(Main *bmain, ID *id, Object *ob, const bool free_us)
{
	bool removed = false;
	if (GS(id->name) == ID_SCE) {
		BKE_scene_remove_rigidbody_object((Scene *)id, ob);
	}
	else {
		BLI_assert(GS(id->name) == ID_GR);
	}

	FOREACH_SCENE_COLLECTION(id, sc)
	{
		removed |= BKE_collection_object_remove(bmain, id, sc, ob, free_us);
	}
	FOREACH_SCENE_COLLECTION_END
	return removed;
}

#if 0
static void collection_group_set_linking(ListBase *lb, const SceneCollection *sc)
{
	for (LayerCollection *lc = sl->layer_collections.first; lc; lc = lc->next) {
		if (lc->scene_collection == sc) {
			//XXX link
		}
		else {
			collection_group_set_linking(&lc->layer_collections, sc);
		}
	}
}
#endif

/**
 * Define a group for a group collection, and populate the collections accordingly
 *
 * \param group can be NULL
 */
void BKE_collection_group_set(Scene *UNUSED(scene), SceneCollection *sc, Group *group)
{
	BLI_assert(sc->type == COLLECTION_TYPE_GROUP);
	/* Add */
	sc->group = group;
}

/**
 * @brief collection_group_convert_layer_collections
 * \param lb: ListBase of LayerCollection elements.
 */
static void collection_group_convert_layer_collections(const Group *group, SceneLayer *sl,
                                                       const SceneCollection *sc, ListBase *lb)
{
	for (LayerCollection *lc = lb->first; lc; lc = lc->next) {
		if (lc->scene_collection == sc) {
			BKE_layer_collection_convert(sl, lc, COLLECTION_TYPE_GROUP);
		}
		else {
			collection_group_convert_layer_collections(group, sl, sc, &lc->layer_collections);
		}
	}
}

static void layer_collection_sync(LayerCollection *lc_dst, LayerCollection *lc_src)
{
	lc_dst->flag = lc_src->flag;
	lc_dst->flag_evaluated = lc_src->flag_evaluated;

	/* Pending: sync overrides. */
	TODO_LAYER_OVERRIDE;

	/* Continue recursively. */
	LayerCollection *lc_dst_nested, *lc_src_nested;
	lc_src_nested = lc_src->layer_collections.first;
	for (lc_dst_nested = lc_dst->layer_collections.first;
	     lc_dst_nested && lc_src_nested;
	     lc_dst_nested = lc_dst_nested->next, lc_src_nested = lc_src_nested->next)
	{
		layer_collection_sync(lc_dst_nested, lc_src_nested);
	}
}

/**
 * Leave only the master collection in, remove everything else.
 * @param group
 */
static void collection_group_cleanup(Group *group)
{
	/* Unlink all the LayerCollections. */
	while (group->scene_layer->layer_collections.last != NULL) {
		BKE_collection_unlink(group->scene_layer, group->scene_layer->layer_collections.last);
	}

	/* Remove all the SceneCollections but the master. */
	collection_free(group->collection, false);
}

/**
 * Convert a collection into a group
 *
 * Any SceneLayer that may have this the related SceneCollection linked is converted
 * to a Group Collection.
 */
Group *BKE_collection_group_create(Main *bmain, Scene *scene, LayerCollection *lc_src)
{
	SceneCollection *sc_dst, *sc_src = lc_src->scene_collection;
	LayerCollection *lc_dst;

	/* We can't convert group collections into groups. */
	if (sc_src->type == COLLECTION_TYPE_GROUP) {
		return NULL;
	}

	/* The master collection can't be converted. */
	if (sc_src == BKE_collection_master(scene)) {
		return NULL;
	}

	/* If a sub-collection of sc_dst is directly linked into a SceneLayer we can't convert. */
	for (SceneLayer *sl = scene->render_layers.first; sl; sl = sl->next) {
		for (LayerCollection *lc_child = sl->layer_collections.first; lc_child; lc_child = lc_child->next) {
			if (is_collection_in_tree(lc_child->scene_collection, sc_src)) {
				return NULL;
			}
		}
	}

	/* Create new group with the same data as the original collection. */
	Group *group = BKE_group_add(bmain, sc_src->name);
	collection_group_cleanup(group);

	sc_dst = BKE_collection_add(&group->id, NULL, COLLECTION_TYPE_GROUP_INTERNAL, sc_src->name);
	BKE_collection_copy_data(sc_dst, sc_src, LIB_ID_CREATE_NO_USER_REFCOUNT);
	FOREACH_SCENE_COLLECTION(&group->id, sc_group)
	{
		sc_group->type = COLLECTION_TYPE_GROUP_INTERNAL;
	}
	FOREACH_SCENE_COLLECTION_END

	lc_dst = BKE_collection_link(group->scene_layer, sc_dst);
	layer_collection_sync(lc_dst, lc_src);

	/* Convert existing collections into group collections. */
	for (SceneLayer *sl = scene->render_layers.first; sl; sl = sl->next) {
		collection_group_convert_layer_collections(group, sl, sc_src, &sl->layer_collections);
	}

	/* Convert original SceneCollection into a group collection. */
	sc_src->type = COLLECTION_TYPE_GROUP;
	BKE_collection_group_set(scene, sc_src, group);
	collection_free(sc_src, true);

	return group;
}

/* ---------------------------------------------------------------------- */
/* Outliner drag and drop */

/**
 * Find and return the SceneCollection that has \a sc_child as one of its directly
 * nested SceneCollection.
 *
 * \param sc_parent Initial SceneCollection to look into recursively, usually the master collection
 */
static SceneCollection *find_collection_parent(const SceneCollection *sc_child, SceneCollection *sc_parent)
{
	for (SceneCollection *sc_nested = sc_parent->scene_collections.first; sc_nested; sc_nested = sc_nested->next) {
		if (sc_nested == sc_child) {
			return sc_parent;
		}

		SceneCollection *found = find_collection_parent(sc_child, sc_nested);
		if (found) {
			return found;
		}
	}

	return NULL;
}

/**
 * Check if \a sc_reference is nested to \a sc_parent SceneCollection
 */
static bool is_collection_in_tree(const SceneCollection *sc_reference, SceneCollection *sc_parent)
{
	return find_collection_parent(sc_reference, sc_parent) != NULL;
}

bool BKE_collection_move_above(const Scene *scene, SceneCollection *sc_dst, SceneCollection *sc_src)
{
	/* Find the SceneCollection the sc_src belongs to */
	SceneCollection *sc_master = BKE_collection_master(scene);

	/* Master Layer can't be moved around*/
	if (ELEM(sc_master, sc_src, sc_dst)) {
		return false;
	}

	/* collection is already where we wanted it to be */
	if (sc_dst->prev == sc_src) {
		return false;
	}

	/* We can't move a collection fs the destiny collection
	 * is nested to the source collection */
	if (is_collection_in_tree(sc_dst, sc_src)) {
		return false;
	}

	SceneCollection *sc_src_parent = find_collection_parent(sc_src, sc_master);
	SceneCollection *sc_dst_parent = find_collection_parent(sc_dst, sc_master);
	BLI_assert(sc_src_parent);
	BLI_assert(sc_dst_parent);

	/* Remove sc_src from its parent */
	BLI_remlink(&sc_src_parent->scene_collections, sc_src);

	/* Re-insert it where it belongs */
	BLI_insertlinkbefore(&sc_dst_parent->scene_collections, sc_dst, sc_src);

	/* Update the tree */
	BKE_layer_collection_resync(scene, sc_src_parent);
	BKE_layer_collection_resync(scene, sc_dst_parent);

	return true;
}

bool BKE_collection_move_below(const Scene *scene, SceneCollection *sc_dst, SceneCollection *sc_src)
{
	/* Find the SceneCollection the sc_src belongs to */
	SceneCollection *sc_master = BKE_collection_master(scene);

	/* Master Layer can't be moved around*/
	if (ELEM(sc_master, sc_src, sc_dst)) {
		return false;
	}

	/* Collection is already where we wanted it to be */
	if (sc_dst->next == sc_src) {
		return false;
	}

	/* We can't move a collection if the destiny collection
	 * is nested to the source collection */
	if (is_collection_in_tree(sc_dst, sc_src)) {
		return false;
	}

	SceneCollection *sc_src_parent = find_collection_parent(sc_src, sc_master);
	SceneCollection *sc_dst_parent = find_collection_parent(sc_dst, sc_master);
	BLI_assert(sc_src_parent);
	BLI_assert(sc_dst_parent);

	/* Remove sc_src from its parent */
	BLI_remlink(&sc_src_parent->scene_collections, sc_src);

	/* Re-insert it where it belongs */
	BLI_insertlinkafter(&sc_dst_parent->scene_collections, sc_dst, sc_src);

	/* Update the tree */
	BKE_layer_collection_resync(scene, sc_src_parent);
	BKE_layer_collection_resync(scene, sc_dst_parent);

	return true;
}

bool BKE_collection_move_into(const Scene *scene, SceneCollection *sc_dst, SceneCollection *sc_src)
{
	/* Find the SceneCollection the sc_src belongs to */
	SceneCollection *sc_master = BKE_collection_master(scene);
	if (sc_src == sc_master) {
		return false;
	}

	/* We can't move a collection if the destiny collection
	 * is nested to the source collection */
	if (is_collection_in_tree(sc_dst, sc_src)) {
		return false;
	}

	SceneCollection *sc_src_parent = find_collection_parent(sc_src, sc_master);
	BLI_assert(sc_src_parent);

	/* collection is already where we wanted it to be */
	if (sc_dst->scene_collections.last == sc_src) {
		return false;
	}

	/* Remove sc_src from it */
	BLI_remlink(&sc_src_parent->scene_collections, sc_src);

	/* Insert sc_src into sc_dst */
	BLI_addtail(&sc_dst->scene_collections, sc_src);

	/* Update the tree */
	BKE_layer_collection_resync(scene, sc_src_parent);
	BKE_layer_collection_resync(scene, sc_dst);

	return true;
}

/* ---------------------------------------------------------------------- */
/* Iteractors */
/* scene collection iteractor */

typedef struct SceneCollectionsIteratorData {
	ID *id;
	void **array;
	int tot, cur;
} SceneCollectionsIteratorData;

static void scene_collection_callback(SceneCollection *sc, BKE_scene_collections_Cb callback, void *data)
{
	callback(sc, data);

	for (SceneCollection *nsc = sc->scene_collections.first; nsc; nsc = nsc->next) {
		scene_collection_callback(nsc, callback, data);
	}
}

static void scene_collections_count(SceneCollection *UNUSED(sc), void *data)
{
	int *tot = data;
	(*tot)++;
}

static void scene_collections_build_array(SceneCollection *sc, void *data)
{
	SceneCollection ***array = data;
	**array = sc;
	(*array)++;
}

static void scene_collections_array(ID *id, SceneCollection ***collections_array, int *tot)
{
	SceneCollection *sc;
	SceneCollection **array;

	*collections_array = NULL;
	*tot = 0;

	if (id == NULL) {
		return;
	}

	sc = master_collection_from_id(id);
	BLI_assert(sc != NULL);
	scene_collection_callback(sc, scene_collections_count, tot);

	if (*tot == 0)
		return;

	*collections_array = array = MEM_mallocN(sizeof(SceneCollection *) * (*tot), "SceneCollectionArray");
	scene_collection_callback(sc, scene_collections_build_array, &array);
}

/**
 * Only use this in non-performance critical situations
 * (it iterates over all scene collections twice)
 */
void BKE_scene_collections_iterator_begin(BLI_Iterator *iter, void *data_in)
{
	ID *id = data_in;
	SceneCollectionsIteratorData *data = MEM_callocN(sizeof(SceneCollectionsIteratorData), __func__);

	data->id = id;
	iter->data = data;

	scene_collections_array(id, (SceneCollection ***)&data->array, &data->tot);
	BLI_assert(data->tot != 0);

	data->cur = 0;
	iter->current = data->array[data->cur];
	iter->valid = true;
}

void BKE_scene_collections_iterator_next(struct BLI_Iterator *iter)
{
	SceneCollectionsIteratorData *data = iter->data;

	if (++data->cur < data->tot) {
		iter->current = data->array[data->cur];
	}
	else {
		iter->valid = false;
	}
}

void BKE_scene_collections_iterator_end(struct BLI_Iterator *iter)
{
	SceneCollectionsIteratorData *data = iter->data;

	if (data) {
		if (data->array) {
			MEM_freeN(data->array);
		}
		MEM_freeN(data);
	}
	iter->valid = false;
}


/* scene objects iteractor */

typedef struct SceneObjectsIteratorData {
	GSet *visited;
	LinkData *link_next;
	BLI_Iterator scene_collection_iter;
} SceneObjectsIteratorData;

void BKE_scene_objects_iterator_begin(BLI_Iterator *iter, void *data_in)
{
	Scene *scene = data_in;
	SceneObjectsIteratorData *data = MEM_callocN(sizeof(SceneObjectsIteratorData), __func__);
	iter->data = data;

	/* lookup list ot make sure each object is object called once */
	data->visited = BLI_gset_ptr_new(__func__);

	/* we wrap the scenecollection iterator here to go over the scene collections */
	BKE_scene_collections_iterator_begin(&data->scene_collection_iter, scene);

	SceneCollection *sc = data->scene_collection_iter.current;
	iter->current = sc->objects.first ? ((LinkData *)sc->objects.first)->data : NULL;
	iter->valid = true;

	if (iter->current == NULL) {
		BKE_scene_objects_iterator_next(iter);
	}
}

/**
 * Gets the first unique object in the sequence
 */
static LinkData *object_base_unique(GSet *gs, LinkData *link)
{
	for (; link != NULL; link = link->next) {
		Object *ob = link->data;
		void **ob_key_p;
		if (!BLI_gset_ensure_p_ex(gs, ob, &ob_key_p)) {
			*ob_key_p = ob;
			return link;
		}
	}
	return NULL;
}

void BKE_scene_objects_iterator_next(BLI_Iterator *iter)
{
	SceneObjectsIteratorData *data = iter->data;
	LinkData *link = data->link_next ? object_base_unique(data->visited, data->link_next) : NULL;

	if (link) {
		data->link_next = link->next;
		iter->current = link->data;
	}
	else {
		/* if this is the last object of this ListBase look at the next SceneCollection */
		SceneCollection *sc;
		BKE_scene_collections_iterator_next(&data->scene_collection_iter);
		do {
			sc = data->scene_collection_iter.current;
			/* get the first unique object of this collection */
			LinkData *new_link = object_base_unique(data->visited, sc->objects.first);
			if (new_link) {
				data->link_next = new_link->next;
				iter->current = new_link->data;
				return;
			}
			BKE_scene_collections_iterator_next(&data->scene_collection_iter);
		} while (data->scene_collection_iter.valid);

		if (!data->scene_collection_iter.valid) {
			iter->valid = false;
		}
	}
}

void BKE_scene_objects_iterator_end(BLI_Iterator *iter)
{
	SceneObjectsIteratorData *data = iter->data;
	if (data) {
		BKE_scene_collections_iterator_end(&data->scene_collection_iter);
		BLI_gset_free(data->visited, NULL);
		MEM_freeN(data);
	}
}
