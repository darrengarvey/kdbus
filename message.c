/*
 * Copyright (C) 2013 Kay Sievers
 * Copyright (C) 2013 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
 * Copyright (C) 2013 Daniel Mack <daniel@zonque.org>
 * Copyright (C) 2013 Linux Foundation
 *
 * kdbus is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/device.h>
#include <linux/idr.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/init.h>
#include <linux/poll.h>
#include "kdbus.h"

#include "kdbus_internal.h"

#define KDBUS_MSG_DATA_SIZE(s) \
	ALIGN((s) + offsetof(struct kdbus_msg_data, data), sizeof(u64))
#define KDBUS_MSG_HEADER_SIZE offsetof(struct kdbus_msg, data)
#define KDBUS_KMSG_HEADER_SIZE offsetof(struct kdbus_kmsg, msg)

static void kdbus_msg_dump(const struct kdbus_msg *msg);

static void __kdbus_kmsg_free(struct kref *kref)
{
	struct kdbus_kmsg *kmsg = container_of(kref, struct kdbus_kmsg, kref);
	kfree(kmsg);
}

void kdbus_kmsg_unref(struct kdbus_kmsg *kmsg)
{
	kref_put(&kmsg->kref, __kdbus_kmsg_free);
}

static struct kdbus_kmsg *kdbus_kmsg_ref(struct kdbus_kmsg *kmsg)
{
	kref_get(&kmsg->kref);
	return kmsg;
}

int kdbus_kmsg_new(u64 extra_size, struct kdbus_kmsg **m)
{
	u64 size = sizeof(struct kdbus_kmsg) + KDBUS_MSG_DATA_SIZE(extra_size);
	struct kdbus_kmsg *kmsg = kzalloc(size, GFP_KERNEL);

	if (!kmsg)
		return -ENOMEM;

	kref_init(&kmsg->kref);

	kmsg->msg.size = size - KDBUS_KMSG_HEADER_SIZE;
	kmsg->msg.data[0].size = KDBUS_MSG_DATA_SIZE(extra_size);

	*m = kmsg;
	return 0;
}

static int kdbus_msg_validate_from_user(const struct kdbus_msg *msg)
{
	u64 size = msg->size - KDBUS_MSG_HEADER_SIZE;
	const struct kdbus_msg_data *data = msg->data;
	u64 data_size = 0;
	u64 payload_size;
	int num_fds;
	bool payload = false;
	bool fds = false;
	bool bloom = false;
	bool name = false;

	if (msg->src_id == KDBUS_SRC_ID_KERNEL)
		return -EINVAL;

	while (size > 0 && size >= data->size) {
		/* Ensure we actually have some data */
		if (data->size <= KDBUS_MSG_DATA_SIZE(0))
			return -EINVAL;
		payload_size = data->size - KDBUS_MSG_DATA_SIZE(0);

		switch (data->type) {
		case KDBUS_MSG_PAYLOAD:
			payload = true;
			data_size += payload_size;
			break;

		case KDBUS_MSG_PAYLOAD_REF:
			payload = true;
			data_size += data->data_ref.size;
			break;

		case KDBUS_MSG_UNIX_FDS:
			/* FIXME: also need to check this is not multi-cast */
			fds = true;
			num_fds = payload_size / sizeof(__u32);
			break;

		case KDBUS_MSG_BLOOM:
			/* FIXME: check payload size is correct */
			bloom = true;
			break;

		case KDBUS_MSG_DST_NAME:
			name = true;
			data_size += payload_size;
			break;
		default:
			return -EINVAL;
		}

		size -= data->size;
		data = (struct kdbus_msg_data *)(((u8 *)data) + data->size);
	}

	/* Can't combine a data message with any other type of message */
	/* FIXME, look up better bit twiddling algo to reduce this to a simple
	 * OR and compare */
	if (payload) {
		if (fds || bloom || name) {
			return -EINVAL;
		}
	}
	if (fds) {
		if (bloom || name) {
			return -EINVAL;
		}
	}
	if (bloom && name)
		return -EINVAL;

	/* FIXME: do something real with data_size and num_fds, we need that for later */
//	kdbus_msg_dump(msg);
//	pr_info("%s: data_size=%llu\n", __func__, (unsigned long long)data_size);
	return 0;
}

int kdbus_kmsg_new_from_user(void __user *buf, struct kdbus_kmsg **m)
{
	struct kdbus_kmsg *kmsg;
	u64 size;
	int ret;

	if (kdbus_size_user(size, buf, struct kdbus_msg, size))
		return -EFAULT;

	if (size < sizeof(struct kdbus_msg) || size > 0xffff)
		return -EMSGSIZE;

	size += offsetof(struct kdbus_kmsg, msg);

	kmsg = kmalloc(size, GFP_KERNEL);
	if (!kmsg)
		return -ENOMEM;

	if (copy_from_user(&kmsg->msg, buf, size)) {
		ret = -EFAULT;
		goto out_ret;
	}

	ret = kdbus_msg_validate_from_user(&kmsg->msg);
	if (ret < 0)
		goto out_ret;

	kref_init(&kmsg->kref);

	*m = kmsg;
	return 0;

out_ret:
	kfree(m);
	return ret;
}

static const struct kdbus_msg_data *kdbus_msg_get_data(struct kdbus_msg *msg,
						       u64 type, int index)
{
	u64 size = msg->size - KDBUS_MSG_HEADER_SIZE;
	const struct kdbus_msg_data *data = msg->data;

	while (size > 0 && size >= data->size) {
		if (data->type == type && index-- == 0)
			return data;

		size -= data->size;
		data = (struct kdbus_msg_data *) (((u8 *) data) + data->size);
	}

	return NULL;
}


static void __maybe_unused kdbus_msg_dump(const struct kdbus_msg *msg)
{
	u64 size = msg->size - KDBUS_MSG_HEADER_SIZE;
	const struct kdbus_msg_data *data = msg->data;

	pr_info("msg size=%llu, flags=0x%llx, dst_id=%llu, src_id=%llu, "
		"cookie=0x%llx payload_type=0x%llx, timeout=%llu\n",
		(unsigned long long) msg->size,
		(unsigned long long) msg->flags,
		(unsigned long long) msg->dst_id,
		(unsigned long long) msg->src_id,
		(unsigned long long) msg->cookie,
		(unsigned long long) msg->payload_type,
		(unsigned long long) msg->timeout);

	while (size > 0 && size >= data->size) {
		pr_info("`- msg_data size=%llu, type=0x%llx\n",
			data->size, data->type);

		size -= data->size;
		data = (struct kdbus_msg_data *) (((u8 *) data) + data->size);
	}
}

static struct kdbus_kmsg __must_check *
kdbus_kmsg_append_data(struct kdbus_kmsg *kmsg, u64 extra_size,
		       struct kdbus_msg_data **data)
{
	struct kdbus_msg *msg;
	struct kdbus_msg_data *d;
	u64 new_size = offsetof(struct kdbus_kmsg, msg) +
			kmsg->msg.size + extra_size + 100;

	kmsg = krealloc(kmsg, new_size, GFP_KERNEL);
	if (!kmsg)
		return NULL;

	msg = &kmsg->msg;

	d = (struct kdbus_msg_data *) ((u8 *) msg + msg->size);
	d->size = extra_size;
	msg->size += extra_size;

	*data = d;

	return kmsg;
}

static struct kdbus_kmsg __must_check *
kdbus_kmsg_append_timestamp(struct kdbus_kmsg *kmsg, u64 *now_ns)
{
	struct kdbus_msg_data *data = NULL;
	u64 size = KDBUS_MSG_DATA_SIZE(sizeof(u64));
	struct timespec ts;

	kmsg = kdbus_kmsg_append_data(kmsg, size, &data);
	if (!kmsg || !data)
		return NULL;

	ktime_get_ts(&ts);
	data->type = KDBUS_MSG_TIMESTAMP;
	data->data_u64[0] = (ts.tv_sec * 1000000000ULL) + ts.tv_nsec;

	if (now_ns)
		*now_ns = data->ts_ns;

	return kmsg;
}

static struct kdbus_kmsg __must_check *
kdbus_kmsg_append_cred(struct kdbus_kmsg *kmsg)
{
	struct kdbus_msg_data *data = NULL;
	u64 size = KDBUS_MSG_DATA_SIZE(sizeof(struct kdbus_creds));

	kmsg = kdbus_kmsg_append_data(kmsg, size, &data);
	if (!kmsg || !data)
		return NULL;

	data->size = size;
	data->type = KDBUS_MSG_SRC_CREDS;
	data->creds.uid = current_uid();
	data->creds.gid = current_gid();
	data->creds.pid = current->pid;
	data->creds.tid = current->tgid;

	return kmsg;
}

static int kdbus_conn_enqueue_kmsg(struct kdbus_conn *conn,
				   struct kdbus_kmsg *kmsg)
{
	struct kdbus_msg_list_entry *entry;

	if (!conn->active)
		return -EAGAIN;

	/* TODO: implement filtering */

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->kmsg = kdbus_kmsg_ref(kmsg);
	INIT_LIST_HEAD(&entry->list);

	mutex_lock(&conn->msg_lock);
	list_add_tail(&entry->list, &conn->msg_list);
	mutex_unlock(&conn->msg_lock);

	wake_up_interruptible(&conn->ep->wait);

	return 0;
}

int kdbus_kmsg_send(struct kdbus_ep *ep, struct kdbus_kmsg **_kmsg)
{
	struct kdbus_conn *conn_dst = NULL;
	struct kdbus_kmsg *kmsg = *_kmsg;
	struct kdbus_msg *msg;
	u64 now_ns = 0;
	int ret = 0;

	/*
	 * FIXME: we need to lock some things here (connection names,
	 * connection list, etc.), or properly implement reference counting for
	 * the connections, and then drop the reference after using it.
	 */

	/* augment incoming message */
	kmsg = kdbus_kmsg_append_timestamp(kmsg, &now_ns);
	kmsg = kdbus_kmsg_append_cred(kmsg);

	msg = &kmsg->msg;
//	kdbus_msg_dump(msg);

	if (msg->dst_id == KDBUS_DST_ID_WELL_KNOWN_NAME) {
		const struct kdbus_msg_data *name_data;
		const struct kdbus_name_entry *name_entry;

		name_data = kdbus_msg_get_data(msg, KDBUS_MSG_DST_NAME, 0);
		if (!name_data) {
			pr_err("message %llu does not contain KDBUS_MSG_DST_NAME\n",
				(unsigned long long) msg->cookie);
			return -EINVAL;
		}

		/* lookup and determine conn_dst ... */
		name_entry = kdbus_name_lookup(ep->bus->name_registry,
					       name_data->data, 0);
		if (name_entry)
			conn_dst = name_entry->conn;

		if (!conn_dst)
			return -ENOENT;

		if ((msg->flags & KDBUS_MSG_FLAGS_NO_AUTO_START) && conn_dst->starter)
			return -ENOENT;

	} else if (msg->dst_id != KDBUS_DST_ID_BROADCAST) {
		/* direct message */
		conn_dst = kdbus_bus_find_conn_by_id(ep->bus, msg->dst_id);
		if (!conn_dst)
			return -ENOENT;
	}

	if (conn_dst) {
		/* direct message */
		if (msg->timeout)
			kmsg->deadline = now_ns + msg->timeout;

		ret = kdbus_conn_enqueue_kmsg(conn_dst, kmsg);

		if (msg->timeout)
			kdbus_conn_scan_timeout(conn_dst);
	} else {
		/* broadcast */
		/* timeouts are not allowed for broadcasts */
		if (msg->timeout)
			return -EINVAL;

		list_for_each_entry(conn_dst, &ep->connection_list,
				    connection_entry) {
			if (conn_dst->type != KDBUS_CONN_EP)
				continue;

			if (conn_dst->id == msg->src_id)
				continue;

			ret = kdbus_conn_enqueue_kmsg(conn_dst, kmsg);
			if (ret < 0)
				break;
		}
	}

	*_kmsg = kmsg;

	return ret;
}

int kdbus_kmsg_recv(struct kdbus_conn *conn, void __user *buf)
{
	struct kdbus_msg_list_entry *entry;
	struct kdbus_msg *msg;
	u64 size;
	int ret;

	if (kdbus_size_user(size, buf, struct kdbus_msg, size))
		return -EFAULT;

	mutex_lock(&conn->msg_lock);
	entry = list_first_entry(&conn->msg_list, struct kdbus_msg_list_entry, list);
	if (!entry) {
		ret = -ENOENT;
		goto out_unlock;
	}

	msg = &entry->kmsg->msg;
	if (size < msg->size) {
		ret = -ENOSPC;
		goto out_unlock;
	}

	ret = copy_to_user(buf, msg, msg->size);
	if (ret == 0) {
		list_del(&entry->list);
		kdbus_kmsg_unref(entry->kmsg);
		kfree(entry);
	}

out_unlock:
	mutex_unlock(&conn->msg_lock);

	return ret;
}
