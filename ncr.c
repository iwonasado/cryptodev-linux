/*
 * New driver for /dev/crypto device (aka CryptoDev)

 * Copyright (c) 2009,2010 Nikos Mavrogiannopoulos <nmav@gnutls.org>
 *
 * This file is part of linux cryptodev.
 *
 * cryptodev is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * cryptodev is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/crypto.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/random.h>
#include "cryptodev.h"
#include <asm/uaccess.h>
#include <asm/ioctl.h>
#include <linux/scatterlist.h>
#include "ncr.h"
#include "ncr_int.h"

#define err() printk(KERN_DEBUG"ncr: %s: %d\n", __func__, __LINE__)

static void _ncr_data_item_put( struct data_item* item);

void* ncr_init_lists(void)
{
	struct ncr_lists *lst;

	lst = kmalloc(sizeof(*lst), GFP_KERNEL);
	if(!lst) {
		err();
		return NULL;
	}

	memset(lst, 0, sizeof(*lst));

	init_MUTEX(&lst->data_sem);
	INIT_LIST_HEAD(&lst->data_list);

	return lst;
}

void ncr_deinit_lists(struct ncr_lists *lst)
{
	if(lst) {
		struct data_item * item, *tmp;

		down(&lst->data_sem);
		
		list_for_each_entry_safe(item, tmp, &lst->data_list, list) {
			list_del(&item->list);
			_ncr_data_item_put( item); /* decrement ref count */
		}
		up(&lst->data_sem);

		kfree(lst);
	}
	
}

/* must be called with data semaphore down
 */
static ncr_data_t _ncr_data_get_new_desc( struct ncr_lists* lst)
{
struct data_item* item;
int mx = 0;

	list_for_each_entry(item, &lst->data_list, list) {
		mx = max(mx, item->desc);
	}
	mx++;

	return mx;
}

/* returns the data item corresponding to desc */
static struct data_item* ncr_data_item_get( struct ncr_lists* lst, ncr_data_t desc)
{
struct data_item* item;

	down(&lst->data_sem);
	list_for_each_entry(item, &lst->data_list, list) {
		if (item->desc == desc) {
			atomic_inc(&item->refcnt);
			up(&lst->data_sem);
			return item;
		}
	}
	up(&lst->data_sem);

	err();
	return NULL;
}

static void* data_alloc(unsigned int uid, size_t size)
{
	/* FIXME: enforce a maximum memory limit per user */
	if (size > 64*1024) {
		err();
		return NULL;
	}
	return kmalloc(size, GFP_KERNEL);
}

static void data_free(struct data_item * data)
{
	/* FIXME: enforce a maximum memory limit per user */
	kfree(data->data);
}

static void _ncr_data_item_put( struct data_item* item)
{
	if (atomic_dec_and_test(&item->refcnt)) {
			data_free(item);
			kfree(item);
	}
}

static int ncr_data_new(unsigned int uid, struct ncr_lists* lst, void __user* arg)
{
	struct ncr_data_init_st init;
	struct data_item* data;
	
	copy_from_user( &init, arg, sizeof(init));

	data = kmalloc(sizeof(*data), GFP_KERNEL);
	if (data == NULL) {
		err();
		return -ENOMEM;
	}

	memset(data, 0, sizeof(*data));

	data->flags = init.flags;
	atomic_set(&data->refcnt, 1);

	data->data = data_alloc(uid, init.max_object_size);
	if (data->data == NULL) {
		kfree(data);
		err();
		return -ENOMEM;
	}
	data->max_data_size = init.max_object_size;

	down(&lst->data_sem);

	data->desc = _ncr_data_get_new_desc(lst);
	data->uid = uid;

	if (init.initial_data != NULL) {
		copy_from_user(data->data, init.initial_data, init.initial_data_size);
		data->data_size = init.initial_data_size;
	}

	list_add(&data->list, &lst->data_list);
	
	up(&lst->data_sem);

	init.desc = data->desc;
	copy_to_user(arg, &init, sizeof(init));

	return 0;
}


static int ncr_data_deinit(struct ncr_lists* lst, void __user* arg)
{
	ncr_data_t desc;
	struct data_item * item, *tmp;

	copy_from_user( &desc, arg, sizeof(desc));

	down(&lst->data_sem);
	
	list_for_each_entry_safe(item, tmp, &lst->data_list, list) {
		if(item->desc == desc) {
			list_del(&item->list);
			_ncr_data_item_put( item); /* decrement ref count */
			break;
		}
	}
	
	up(&lst->data_sem);

	return 0;
}

static int ncr_data_get(struct ncr_lists* lst, void __user* arg)
{
	struct ncr_data_st get;
	struct data_item * data;
	size_t len;
	
	copy_from_user( &get, arg, sizeof(get));

	data = ncr_data_item_get( lst, get.desc);

	if (data == NULL) {
		err();
		return -EINVAL;
	}

	if (!(data->flags & NCR_DATA_FLAG_EXPORTABLE)) {
		err();
		return -EPERM;
	}

	len = min(get.data_size, data->data_size);

	/* update length */
	get.data_size = len;
	copy_to_user(arg, &get, sizeof(get));

	if (len > 0)
		copy_to_user(get.data, data->data, len);

	_ncr_data_item_put( data);

	return 0;
}

static int ncr_data_set(struct ncr_lists* lst, void __user* arg)
{
	struct ncr_data_st get;
	struct data_item * data;
	int ret;
	
	copy_from_user( &get, arg, sizeof(get));

	data = ncr_data_item_get( lst, get.desc);

	if (data == NULL) {
		err();
		return -EINVAL;
	}

	if ((get.data_size > data->max_data_size) ||
		(get.data == NULL && get.data_size != 0)) {
		err();
		ret = -EINVAL;
		goto cleanup;
	}

	if (!get.append_flag) {
		if (get.data != NULL)
			copy_from_user(data->data, get.data, get.data_size);
		data->data_size = get.data_size;
	} else {
		if (get.data_size+data->data_size > data->max_data_size) {
			err();
			ret = -EINVAL;
			goto cleanup;
		}
		if (get.data != NULL)
			copy_from_user(&data->data[data->data_size], get.data, get.data_size);
		data->data_size += get.data_size;
	}
	ret = 0;

cleanup:
	_ncr_data_item_put( data);

	return ret;
}

int
ncr_ioctl(unsigned int uid, struct ncr_lists* lst,
		unsigned int cmd, unsigned long __user arg)
{

	if (unlikely(!lst))
		BUG();

	switch (cmd) {
		case NCRIO_DATA_INIT:
			return ncr_data_new(uid, lst, (void*)arg);
		case NCRIO_DATA_GET:
			return ncr_data_get(lst, (void*)arg);
		case NCRIO_DATA_SET:
			return ncr_data_set(lst, (void*)arg);
		case NCRIO_DATA_DEINIT:
			return ncr_data_deinit(lst, (void*)arg);
		default:
			return -EINVAL;
	}
}



