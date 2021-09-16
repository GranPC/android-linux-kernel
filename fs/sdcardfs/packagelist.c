/*
 * fs/sdcardfs/packagelist.c
 *
 * Copyright (c) 2013 Samsung Electronics Co. Ltd
 *   Authors: Daeho Jeong, Woojoong Lee, Seunghwan Hyun,
 *               Sunghwan Yun, Sungjong Seo
 *
 * This program has been developed as a stackable file system based on
 * the WrapFS which written by
 *
 * Copyright (c) 1998-2011 Erez Zadok
 * Copyright (c) 2009     Shrikar Archak
 * Copyright (c) 2003-2011 Stony Brook University
 * Copyright (c) 2003-2011 The Research Foundation of SUNY
 *
 * This file is dual licensed.  It may be redistributed and/or modified
 * under the terms of the Apache 2.0 License OR version 2 of the GNU
 * General Public License.
 */

#include "sdcardfs.h"
#include <linux/hashtable.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/radix-tree.h>
#include <linux/dcache.h>

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

#include <linux/configfs.h>

#ifdef CONFIG_BBSECURE_SDBASE
#include "bbry_policy.h"
#endif /* CONFIG_BBSECURE_SDBASE */

struct hashtable_entry {
	struct hlist_node hlist;
	struct hlist_node dlist; /* for deletion cleanup */
	struct qstr key;
	atomic_t value;
};

static DEFINE_HASHTABLE(package_to_appid, 8);
static DEFINE_HASHTABLE(package_to_userid, 8);
static DEFINE_HASHTABLE(ext_to_groupid, 8);


static struct kmem_cache *hashtable_entry_cachep;

#ifdef CONFIG_SDCARD_FS_LOCKER
#define LOCKER_MAX_WL 10
static const char *s_locker_wl[LOCKER_MAX_WL];
static const char *s_locker_wl_internal[] = {
	"com.android.cts.externalstorageapp",
	"com.android.cts.storagestatsapp",
	NULL
};
#endif /* CONFIG_SDCARD_FS_LOCKER */

static unsigned int full_name_case_hash(const unsigned char *name, unsigned int len)
{
	unsigned long hash = init_name_hash();

	while (len--)
		hash = partial_name_hash(tolower(*name++), hash);
	return end_name_hash(hash);
}

static inline void qstr_init(struct qstr *q, const char *name)
{
	q->name = name;
	q->len = strlen(q->name);
	q->hash = full_name_case_hash(q->name, q->len);
}

static inline int qstr_copy(const struct qstr *src, struct qstr *dest)
{
	dest->name = kstrdup(src->name, GFP_KERNEL);
	dest->hash_len = src->hash_len;
	return !!dest->name;
}


static appid_t __get_appid(const struct qstr *key)
{
	struct hashtable_entry *hash_cur;
	unsigned int hash = key->hash;
	appid_t ret_id;

	rcu_read_lock();
	hash_for_each_possible_rcu(package_to_appid, hash_cur, hlist, hash) {
		if (qstr_case_eq(key, &hash_cur->key)) {
			ret_id = atomic_read(&hash_cur->value);
			rcu_read_unlock();
			return ret_id;
		}
	}
	rcu_read_unlock();
	return 0;
}

appid_t get_appid(const char *key)
{
	struct qstr q;

	qstr_init(&q, key);
	return __get_appid(&q);
}

static appid_t __get_ext_gid(const struct qstr *key)
{
	struct hashtable_entry *hash_cur;
	unsigned int hash = key->hash;
	appid_t ret_id;

	rcu_read_lock();
	hash_for_each_possible_rcu(ext_to_groupid, hash_cur, hlist, hash) {
		if (qstr_case_eq(key, &hash_cur->key)) {
			ret_id = atomic_read(&hash_cur->value);
			rcu_read_unlock();
			return ret_id;
		}
	}
	rcu_read_unlock();
	return 0;
}

appid_t get_ext_gid(const char *key)
{
	struct qstr q;

	qstr_init(&q, key);
	return __get_ext_gid(&q);
}

static appid_t __is_excluded(const struct qstr *app_name, userid_t user)
{
	struct hashtable_entry *hash_cur;
	unsigned int hash = app_name->hash;

	rcu_read_lock();
	hash_for_each_possible_rcu(package_to_userid, hash_cur, hlist, hash) {
		if (atomic_read(&hash_cur->value) == user &&
				qstr_case_eq(app_name, &hash_cur->key)) {
			rcu_read_unlock();
			return 1;
		}
	}
	rcu_read_unlock();
	return 0;
}

appid_t is_excluded(const char *key, userid_t user)
{
	struct qstr q;
	qstr_init(&q, key);
	return __is_excluded(&q, user);
}

#ifdef CONFIG_SDCARD_FS_LOCKER
static int locker_check_caller_access_to_name_by_uid(struct inode *parent_node, const struct qstr *name)
{
	struct qstr q_Locker = QSTR_LITERAL(CONFIG_SDCARD_FS_LOCKER_NAME);
	uid_t caller_uid;
	appid_t appid;
	struct sdcardfs_inode_data *parent_data;
	int i;
	unsigned int tmp;

	if (!parent_node) {
		return 1;
	}
	if (s_locker_wl[0] == NULL) {
		return 1;
	}

	parent_data = SDCARDFS_I(parent_node)->data;
	caller_uid = from_kuid(&init_user_ns, current_fsuid());
	//pr_info("sdcardfs: %s: caller_uid=%d\n", __FUNCTION__, caller_uid);

	if (parent_data->under_locker || ((parent_data->perm == PERM_ROOT) && qstr_case_eq(name, &q_Locker))) {
		for (i = 0; s_locker_wl[i]; ++i) {
			appid = get_appid(s_locker_wl[i]);
			if (appid == 0) {
				if (kstrtouint(s_locker_wl[i], 10, &tmp)) {
					continue;
				}
				appid = tmp;
			}
			//pr_info("sdcardfs: %s: appid=%d\n", __FUNCTION__, appid);

			if (caller_uid == multiuser_get_uid(parent_data->userid, appid)) {
				return 1;
			}
		}
		for (i = 0; s_locker_wl_internal[i]; ++i) {
			appid = get_appid(s_locker_wl_internal[i]);
			if (appid == 0) {
				if (kstrtouint(s_locker_wl_internal[i], 10, &tmp)) {
					continue;
				}
				appid = tmp;
			}
			//pr_info("sdcardfs: %s: appid=%d\n", __FUNCTION__, appid);

			if (caller_uid == multiuser_get_uid(parent_data->userid, appid)) {
				return 1;
			}
		}
		return 0;
	}

	return 1;
}
#endif /* CONFIG_SDCARD_FS_LOCKER */

#ifdef CONFIG_BBSECURE_SDBASE
int policy_check_caller_access_to_name(struct inode *parent_node,
		const struct qstr *name, fmode_t mode)
{
	struct sdcardfs_sb_info *sb;
	struct sdcardfs_mount_options *opts;
	userid_t caller_userid;
	userid_t man_prof_userid = -1;
	bool is_managed_profile = false;
#ifndef CONFIG_BBSECURE_SDAFW
	bool is_removable_storage = false;
#else
	enum storage_t is_removable_storage;
#endif /* CONFIG_BBSECURE_SDAFW */

	if (!parent_node)
		return 1;

	sb = SDCARDFS_SB(parent_node->i_sb);
	if (!sb)
		return 1;

	/* Do not want to block emulated storage operations */
	opts = &sb->options;
#ifndef CONFIG_BBSECURE_SDAFW
	is_removable_storage = opts->primary_only;
	if (!is_removable_storage) {
#else
	is_removable_storage = strncmp(opts->primary_only, "sd\0", 3) == 0 ?
		SD_STORE : strncmp(opts->primary_only, "otg\0", 4) == 0 ?
		OTG_STORE : strlen(opts->primary_only) == 0 ?
		EMU_STORE : INVALID_STORE;

	if (is_removable_storage == INVALID_STORE) {
		pr_info("sdcardfs: %s: primary_only flag invalid:%s\n",
		__func__, opts->primary_only);

		return 0;
	}

	if (is_removable_storage == EMU_STORE) {
#endif /* CONFIG_BBSECURE_SDAFW */
#ifdef CONFIG_BBSECURE_ADBAFW
		kuid_t uid = current_fsuid();
		if (uid.val == AID_SHELL) {
			struct sdcardfs_sb_info *sb;
			sb = SDCARDFS_SB(parent_node->i_sb);
#ifdef CONFIG_BBSECURE_SDAFW
			if (sb && (strlen(sb->options.primary_only) == 0)) {
#elif defined(CONFIG_BBSECURE_SDBASE)
			if (sb && !sb->options.primary_only) {
#endif /* CONFIG_BBSECURE_SDBASE || CONFIG_BBSECURE_SDAFW */
				char *buf = NULL;
				char *path = NULL;
				size_t path_length;
				struct dentry *d;
				d =  hlist_entry(parent_node->i_dentry.first, struct dentry, d_u.d_alias);
				if(NULL != d) {
					buf = kmalloc(PATH_MAX, GFP_NOWAIT);

					/* deny access if we are unable to allocate the buffer */
					if (NULL == buf)
						return 0;
					path = dentry_path(d, buf, PATH_MAX);
				}
				if (!IS_ERR_OR_NULL(path)) {
					char slash;
					userid_t aid;
					path_length = strlen(path);
					if( 1 < path_length && path_length < PATH_MAX) {
						if( 0 < sscanf(path, "%c%d", &slash, &aid) ) {
							if( 1 == get_adb_disabled(aid) ) {
								//deny access
								kfree(buf);
								return 0;
							}
						}
					}
				}
				if (buf != NULL)
					kfree(buf);
			}
		}
#endif /* CONFIG_BBSECURE_ADBAFW */
		return 1;
	}

	caller_userid = from_kuid(&init_user_ns, current_fsuid()) / AID_USER_OFFSET;

	if (get_managed_profile(&man_prof_userid) == 0)
		is_managed_profile = !!(caller_userid == man_prof_userid);

	if (caller_userid != 0 &&
		(!is_managed_profile || (is_managed_profile && mode == FMODE_WRITE)))
	{
		return 0;
	}

#ifdef CONFIG_BBSECURE_SDAFW
	{
		int i;
		userid_t policy_disabled[MAX_USERS_POLICY];
		memset(policy_disabled, (userid_t)(-1), MAX_USERS_POLICY * sizeof(userid_t));
		switch (is_removable_storage) {
		case SD_STORE:
			get_mediacard_disabled(policy_disabled, sizeof(policy_disabled));
			break;
		case OTG_STORE:
			get_usbotg_disabled(policy_disabled, sizeof(policy_disabled));
			break;
		default:
			return 1;
			break;
		}
		for (i = 0; i < MAX_USERS_POLICY; i++) {
			if (policy_disabled[i] == caller_userid)
				return 0;
		}
	}
#endif /* CONFIG_BBSECURE_SDAFW */
	return 1;
}
#endif /* CONFIG_BBSECURE_SDBASE */

/* Kernel has already enforced everything we returned through
 * derive_permissions_locked(), so this is used to lock down access
 * even further, such as enforcing that apps hold sdcard_rw.
 */
#ifndef CONFIG_BBSECURE_SDBASE
int check_caller_access_to_name(struct inode *parent_node, const struct qstr *name)
#else
int check_caller_access_to_name(struct inode *parent_node, const struct qstr *name, fmode_t mode)
#endif /* CONFIG_BBSECURE_SDBASE */
{
	struct qstr q_autorun = QSTR_LITERAL("autorun.inf");
	struct qstr q__android_secure = QSTR_LITERAL(".android_secure");
	struct qstr q_android_secure = QSTR_LITERAL("android_secure");

	/* Always block security-sensitive files at root */
	if (parent_node && SDCARDFS_I(parent_node)->data->perm == PERM_ROOT) {
		if (qstr_case_eq(name, &q_autorun)
			|| qstr_case_eq(name, &q__android_secure)
			|| qstr_case_eq(name, &q_android_secure)) {
			return 0;
		}
	}

	/* Root always has access; access for any other UIDs should always
	 * be controlled through packages.list.
	 */
	if (from_kuid(&init_user_ns, current_fsuid()) == 0)
		return 1;

#ifdef CONFIG_SDCARD_FS_LOCKER
	if (locker_check_caller_access_to_name_by_uid(parent_node, name) == 0) {
		return 0;
	}
#endif /* CONFIG_SDCARD_FS_LOCKER */

#ifdef CONFIG_BBSECURE_SDBASE
	if (policy_check_caller_access_to_name(parent_node, name, mode) == 0) {
		return 0;
	}
#endif /* CONFIG_BBSECURE_SDBASE */

	/* No extra permissions to enforce */
	return 1;
}

static struct hashtable_entry *alloc_hashtable_entry(const struct qstr *key,
		appid_t value)
{
	struct hashtable_entry *ret = kmem_cache_alloc(hashtable_entry_cachep,
			GFP_KERNEL);
	if (!ret)
		return NULL;
	INIT_HLIST_NODE(&ret->dlist);
	INIT_HLIST_NODE(&ret->hlist);

	if (!qstr_copy(key, &ret->key)) {
		kmem_cache_free(hashtable_entry_cachep, ret);
		return NULL;
	}

	atomic_set(&ret->value, value);
	return ret;
}

static int insert_packagelist_appid_entry_locked(const struct qstr *key, appid_t value)
{
	struct hashtable_entry *hash_cur;
	struct hashtable_entry *new_entry;
	unsigned int hash = key->hash;

	hash_for_each_possible_rcu(package_to_appid, hash_cur, hlist, hash) {
		if (qstr_case_eq(key, &hash_cur->key)) {
			atomic_set(&hash_cur->value, value);
			return 0;
		}
	}
	new_entry = alloc_hashtable_entry(key, value);
	if (!new_entry)
		return -ENOMEM;
	hash_add_rcu(package_to_appid, &new_entry->hlist, hash);
	return 0;
}

static int insert_ext_gid_entry_locked(const struct qstr *key, appid_t value)
{
	struct hashtable_entry *hash_cur;
	struct hashtable_entry *new_entry;
	unsigned int hash = key->hash;

	/* An extension can only belong to one gid */
	hash_for_each_possible_rcu(ext_to_groupid, hash_cur, hlist, hash) {
		if (qstr_case_eq(key, &hash_cur->key))
			return -EINVAL;
	}
	new_entry = alloc_hashtable_entry(key, value);
	if (!new_entry)
		return -ENOMEM;
	hash_add_rcu(ext_to_groupid, &new_entry->hlist, hash);
	return 0;
}

static int insert_userid_exclude_entry_locked(const struct qstr *key, userid_t value)
{
	struct hashtable_entry *hash_cur;
	struct hashtable_entry *new_entry;
	unsigned int hash = key->hash;

	/* Only insert if not already present */
	hash_for_each_possible_rcu(package_to_userid, hash_cur, hlist, hash) {
		if (atomic_read(&hash_cur->value) == value &&
				qstr_case_eq(key, &hash_cur->key))
			return 0;
	}
	new_entry = alloc_hashtable_entry(key, value);
	if (!new_entry)
		return -ENOMEM;
	hash_add_rcu(package_to_userid, &new_entry->hlist, hash);
	return 0;
}

static void fixup_all_perms_name(const struct qstr *key)
{
	struct sdcardfs_sb_info *sbinfo;
	struct limit_search limit = {
		.flags = BY_NAME,
		.name = QSTR_INIT(key->name, key->len),
	};
	list_for_each_entry(sbinfo, &sdcardfs_super_list, list) {
		if (sbinfo_has_sdcard_magic(sbinfo))
			fixup_perms_recursive(sbinfo->sb->s_root, &limit);
	}
}

static void fixup_all_perms_name_userid(const struct qstr *key, userid_t userid)
{
	struct sdcardfs_sb_info *sbinfo;
	struct limit_search limit = {
		.flags = BY_NAME | BY_USERID,
		.name = QSTR_INIT(key->name, key->len),
		.userid = userid,
	};
	list_for_each_entry(sbinfo, &sdcardfs_super_list, list) {
		if (sbinfo_has_sdcard_magic(sbinfo))
			fixup_perms_recursive(sbinfo->sb->s_root, &limit);
	}
}

static void fixup_all_perms_userid(userid_t userid)
{
	struct sdcardfs_sb_info *sbinfo;
	struct limit_search limit = {
		.flags = BY_USERID,
		.userid = userid,
	};
	list_for_each_entry(sbinfo, &sdcardfs_super_list, list) {
		if (sbinfo_has_sdcard_magic(sbinfo))
			fixup_perms_recursive(sbinfo->sb->s_root, &limit);
	}
}

static int insert_packagelist_entry(const struct qstr *key, appid_t value)
{
	int err;

	mutex_lock(&sdcardfs_super_list_lock);
	err = insert_packagelist_appid_entry_locked(key, value);
	if (!err)
		fixup_all_perms_name(key);
	mutex_unlock(&sdcardfs_super_list_lock);

	return err;
}

static int insert_ext_gid_entry(const struct qstr *key, appid_t value)
{
	int err;

	mutex_lock(&sdcardfs_super_list_lock);
	err = insert_ext_gid_entry_locked(key, value);
	mutex_unlock(&sdcardfs_super_list_lock);

	return err;
}

static int insert_userid_exclude_entry(const struct qstr *key, userid_t value)
{
	int err;

	mutex_lock(&sdcardfs_super_list_lock);
	err = insert_userid_exclude_entry_locked(key, value);
	if (!err)
		fixup_all_perms_name_userid(key, value);
	mutex_unlock(&sdcardfs_super_list_lock);

	return err;
}

static void free_hashtable_entry(struct hashtable_entry *entry)
{
	kfree(entry->key.name);
	kmem_cache_free(hashtable_entry_cachep, entry);
}

static void remove_packagelist_entry_locked(const struct qstr *key)
{
	struct hashtable_entry *hash_cur;
	unsigned int hash = key->hash;
	struct hlist_node *h_t;
	HLIST_HEAD(free_list);

	hash_for_each_possible_rcu(package_to_userid, hash_cur, hlist, hash) {
		if (qstr_case_eq(key, &hash_cur->key)) {
			hash_del_rcu(&hash_cur->hlist);
			hlist_add_head(&hash_cur->dlist, &free_list);
		}
	}
	hash_for_each_possible_rcu(package_to_appid, hash_cur, hlist, hash) {
		if (qstr_case_eq(key, &hash_cur->key)) {
			hash_del_rcu(&hash_cur->hlist);
			hlist_add_head(&hash_cur->dlist, &free_list);
			break;
		}
	}
	synchronize_rcu();
	hlist_for_each_entry_safe(hash_cur, h_t, &free_list, dlist)
		free_hashtable_entry(hash_cur);
}

static void remove_packagelist_entry(const struct qstr *key)
{
	mutex_lock(&sdcardfs_super_list_lock);
	remove_packagelist_entry_locked(key);
	fixup_all_perms_name(key);
	mutex_unlock(&sdcardfs_super_list_lock);
}

static void remove_ext_gid_entry_locked(const struct qstr *key, gid_t group)
{
	struct hashtable_entry *hash_cur;
	unsigned int hash = key->hash;

	hash_for_each_possible_rcu(ext_to_groupid, hash_cur, hlist, hash) {
		if (qstr_case_eq(key, &hash_cur->key) && atomic_read(&hash_cur->value) == group) {
			hash_del_rcu(&hash_cur->hlist);
			synchronize_rcu();
			free_hashtable_entry(hash_cur);
			break;
		}
	}
}

static void remove_ext_gid_entry(const struct qstr *key, gid_t group)
{
	mutex_lock(&sdcardfs_super_list_lock);
	remove_ext_gid_entry_locked(key, group);
	mutex_unlock(&sdcardfs_super_list_lock);
}

static void remove_userid_all_entry_locked(userid_t userid)
{
	struct hashtable_entry *hash_cur;
	struct hlist_node *h_t;
	HLIST_HEAD(free_list);
	int i;

	hash_for_each_rcu(package_to_userid, i, hash_cur, hlist) {
		if (atomic_read(&hash_cur->value) == userid) {
			hash_del_rcu(&hash_cur->hlist);
			hlist_add_head(&hash_cur->dlist, &free_list);
		}
	}
	synchronize_rcu();
	hlist_for_each_entry_safe(hash_cur, h_t, &free_list, dlist) {
		free_hashtable_entry(hash_cur);
	}
}

static void remove_userid_all_entry(userid_t userid)
{
	mutex_lock(&sdcardfs_super_list_lock);
	remove_userid_all_entry_locked(userid);
	fixup_all_perms_userid(userid);
	mutex_unlock(&sdcardfs_super_list_lock);
}

static void remove_userid_exclude_entry_locked(const struct qstr *key, userid_t userid)
{
	struct hashtable_entry *hash_cur;
	unsigned int hash = key->hash;

	hash_for_each_possible_rcu(package_to_userid, hash_cur, hlist, hash) {
		if (qstr_case_eq(key, &hash_cur->key) &&
				atomic_read(&hash_cur->value) == userid) {
			hash_del_rcu(&hash_cur->hlist);
			synchronize_rcu();
			free_hashtable_entry(hash_cur);
			break;
		}
	}
}

static void remove_userid_exclude_entry(const struct qstr *key, userid_t userid)
{
	mutex_lock(&sdcardfs_super_list_lock);
	remove_userid_exclude_entry_locked(key, userid);
	fixup_all_perms_name_userid(key, userid);
	mutex_unlock(&sdcardfs_super_list_lock);
}

static void packagelist_destroy(void)
{
	struct hashtable_entry *hash_cur;
	struct hlist_node *h_t;
	HLIST_HEAD(free_list);
	int i;

	mutex_lock(&sdcardfs_super_list_lock);
	hash_for_each_rcu(package_to_appid, i, hash_cur, hlist) {
		hash_del_rcu(&hash_cur->hlist);
		hlist_add_head(&hash_cur->dlist, &free_list);
	}
	hash_for_each_rcu(package_to_userid, i, hash_cur, hlist) {
		hash_del_rcu(&hash_cur->hlist);
		hlist_add_head(&hash_cur->dlist, &free_list);
	}
	synchronize_rcu();
	hlist_for_each_entry_safe(hash_cur, h_t, &free_list, dlist)
		free_hashtable_entry(hash_cur);
	mutex_unlock(&sdcardfs_super_list_lock);
	pr_info("sdcardfs: destroyed packagelist pkgld\n");
}

#define SDCARDFS_CONFIGFS_ATTR(_pfx, _name)			\
static struct configfs_attribute _pfx##attr_##_name = {	\
	.ca_name	= __stringify(_name),		\
	.ca_mode	= S_IRUGO | S_IWUGO,		\
	.ca_owner	= THIS_MODULE,			\
	.show		= _pfx##_name##_show,		\
	.store		= _pfx##_name##_store,		\
}

#define SDCARDFS_CONFIGFS_ATTR_RO(_pfx, _name)			\
static struct configfs_attribute _pfx##attr_##_name = {	\
	.ca_name	= __stringify(_name),		\
	.ca_mode	= S_IRUGO,			\
	.ca_owner	= THIS_MODULE,			\
	.show		= _pfx##_name##_show,		\
}

#define SDCARDFS_CONFIGFS_ATTR_WO(_pfx, _name)			\
static struct configfs_attribute _pfx##attr_##_name = {	\
	.ca_name	= __stringify(_name),		\
	.ca_mode	= S_IWUGO,			\
	.ca_owner	= THIS_MODULE,			\
	.store		= _pfx##_name##_store,		\
}

struct package_details {
	struct config_item item;
	struct qstr name;
};

static inline struct package_details *to_package_details(struct config_item *item)
{
	return item ? container_of(item, struct package_details, item) : NULL;
}

static ssize_t package_details_appid_show(struct config_item *item, char *page)
{
	return scnprintf(page, PAGE_SIZE, "%u\n", __get_appid(&to_package_details(item)->name));
}

static ssize_t package_details_appid_store(struct config_item *item,
				       const char *page, size_t count)
{
	unsigned int tmp;
	int ret;

	ret = kstrtouint(page, 10, &tmp);
	if (ret)
		return ret;

	ret = insert_packagelist_entry(&to_package_details(item)->name, tmp);

	if (ret)
		return ret;

	return count;
}

static ssize_t package_details_excluded_userids_show(struct config_item *item,
				      char *page)
{
	struct package_details *package_details = to_package_details(item);
	struct hashtable_entry *hash_cur;
	unsigned int hash = package_details->name.hash;
	int count = 0;

	rcu_read_lock();
	hash_for_each_possible_rcu(package_to_userid, hash_cur, hlist, hash) {
		if (qstr_case_eq(&package_details->name, &hash_cur->key))
			count += scnprintf(page + count, PAGE_SIZE - count,
					"%d ", atomic_read(&hash_cur->value));
	}
	rcu_read_unlock();
	if (count)
		count--;
	count += scnprintf(page + count, PAGE_SIZE - count, "\n");
	return count;
}

static ssize_t package_details_excluded_userids_store(struct config_item *item,
				       const char *page, size_t count)
{
	unsigned int tmp;
	int ret;

	ret = kstrtouint(page, 10, &tmp);
	if (ret)
		return ret;

	ret = insert_userid_exclude_entry(&to_package_details(item)->name, tmp);

	if (ret)
		return ret;

	return count;
}

static ssize_t package_details_clear_userid_store(struct config_item *item,
				       const char *page, size_t count)
{
	unsigned int tmp;
	int ret;

	ret = kstrtouint(page, 10, &tmp);
	if (ret)
		return ret;
	remove_userid_exclude_entry(&to_package_details(item)->name, tmp);
	return count;
}

static void package_details_release(struct config_item *item)
{
	struct package_details *package_details = to_package_details(item);

	pr_info("sdcardfs: removing %s\n", package_details->name.name);
	remove_packagelist_entry(&package_details->name);
	kfree(package_details->name.name);
	kfree(package_details);
}

SDCARDFS_CONFIGFS_ATTR(package_details_, appid);
SDCARDFS_CONFIGFS_ATTR(package_details_, excluded_userids);
SDCARDFS_CONFIGFS_ATTR_WO(package_details_, clear_userid);

static struct configfs_attribute *package_details_attrs[] = {
	&package_details_attr_appid,
	&package_details_attr_excluded_userids,
	&package_details_attr_clear_userid,
	NULL,
};

static struct configfs_item_operations package_details_item_ops = {
	.release = package_details_release,
};

static struct config_item_type package_appid_type = {
	.ct_item_ops	= &package_details_item_ops,
	.ct_attrs	= package_details_attrs,
	.ct_owner	= THIS_MODULE,
};

struct extensions_value {
	struct config_group group;
	unsigned int num;
};

struct extension_details {
	struct config_item item;
	struct qstr name;
	unsigned int num;
};

static inline struct extensions_value *to_extensions_value(struct config_item *item)
{
	return item ? container_of(to_config_group(item), struct extensions_value, group) : NULL;
}

static inline struct extension_details *to_extension_details(struct config_item *item)
{
	return item ? container_of(item, struct extension_details, item) : NULL;
}

static void extension_details_release(struct config_item *item)
{
	struct extension_details *extension_details = to_extension_details(item);

	pr_info("sdcardfs: No longer mapping %s files to gid %d\n",
			extension_details->name.name, extension_details->num);
	remove_ext_gid_entry(&extension_details->name, extension_details->num);
	kfree(extension_details->name.name);
	kfree(extension_details);
}

static struct configfs_item_operations extension_details_item_ops = {
	.release = extension_details_release,
};

static struct config_item_type extension_details_type = {
	.ct_item_ops = &extension_details_item_ops,
	.ct_owner = THIS_MODULE,
};

static struct config_item *extension_details_make_item(struct config_group *group, const char *name)
{
	struct extensions_value *extensions_value = to_extensions_value(&group->cg_item);
	struct extension_details *extension_details = kzalloc(sizeof(struct extension_details), GFP_KERNEL);
	const char *tmp;
	int ret;

	if (!extension_details)
		return ERR_PTR(-ENOMEM);

	tmp = kstrdup(name, GFP_KERNEL);
	if (!tmp) {
		kfree(extension_details);
		return ERR_PTR(-ENOMEM);
	}
	qstr_init(&extension_details->name, tmp);
	extension_details->num = extensions_value->num;
	ret = insert_ext_gid_entry(&extension_details->name, extensions_value->num);

	if (ret) {
		kfree(extension_details->name.name);
		kfree(extension_details);
		return ERR_PTR(ret);
	}
	config_item_init_type_name(&extension_details->item, name, &extension_details_type);

	return &extension_details->item;
}

static struct configfs_group_operations extensions_value_group_ops = {
	.make_item = extension_details_make_item,
};

static struct config_item_type extensions_name_type = {
	.ct_group_ops	= &extensions_value_group_ops,
	.ct_owner	= THIS_MODULE,
};

static struct config_group *extensions_make_group(struct config_group *group, const char *name)
{
	struct extensions_value *extensions_value;
	unsigned int tmp;
	int ret;

	extensions_value = kzalloc(sizeof(struct extensions_value), GFP_KERNEL);
	if (!extensions_value)
		return ERR_PTR(-ENOMEM);
	ret = kstrtouint(name, 10, &tmp);
	if (ret) {
		kfree(extensions_value);
		return ERR_PTR(ret);
	}

	extensions_value->num = tmp;
	config_group_init_type_name(&extensions_value->group, name,
						&extensions_name_type);
	return &extensions_value->group;
}

static void extensions_drop_group(struct config_group *group, struct config_item *item)
{
	struct extensions_value *value = to_extensions_value(item);

	pr_info("sdcardfs: No longer mapping any files to gid %d\n", value->num);
	kfree(value);
}

static struct configfs_group_operations extensions_group_ops = {
	.make_group	= extensions_make_group,
	.drop_item	= extensions_drop_group,
};

static struct config_item_type extensions_type = {
	.ct_group_ops	= &extensions_group_ops,
	.ct_owner	= THIS_MODULE,
};

struct config_group extension_group = {
	.cg_item = {
		.ci_namebuf = "extensions",
		.ci_type = &extensions_type,
	},
};

static struct config_item *packages_make_item(struct config_group *group, const char *name)
{
	struct package_details *package_details;
	const char *tmp;

	package_details = kzalloc(sizeof(struct package_details), GFP_KERNEL);
	if (!package_details)
		return ERR_PTR(-ENOMEM);
	tmp = kstrdup(name, GFP_KERNEL);
	if (!tmp) {
		kfree(package_details);
		return ERR_PTR(-ENOMEM);
	}
	qstr_init(&package_details->name, tmp);
	config_item_init_type_name(&package_details->item, name,
						&package_appid_type);

	return &package_details->item;
}

static ssize_t packages_list_show(struct config_item *item, char *page)
{
	struct hashtable_entry *hash_cur_app;
	struct hashtable_entry *hash_cur_user;
	int i;
	int count = 0, written = 0;
	const char errormsg[] = "<truncated>\n";
	unsigned int hash;

	rcu_read_lock();
	hash_for_each_rcu(package_to_appid, i, hash_cur_app, hlist) {
		written = scnprintf(page + count, PAGE_SIZE - sizeof(errormsg) - count, "%s %d\n",
					hash_cur_app->key.name, atomic_read(&hash_cur_app->value));
		hash = hash_cur_app->key.hash;
		hash_for_each_possible_rcu(package_to_userid, hash_cur_user, hlist, hash) {
			if (qstr_case_eq(&hash_cur_app->key, &hash_cur_user->key)) {
				written += scnprintf(page + count + written - 1,
					PAGE_SIZE - sizeof(errormsg) - count - written + 1,
					" %d\n", atomic_read(&hash_cur_user->value)) - 1;
			}
		}
		if (count + written == PAGE_SIZE - sizeof(errormsg) - 1) {
			count += scnprintf(page + count, PAGE_SIZE - count, errormsg);
			break;
		}
		count += written;
	}
	rcu_read_unlock();

	return count;
}

static ssize_t packages_remove_userid_store(struct config_item *item,
				       const char *page, size_t count)
{
	unsigned int tmp;
	int ret;

	ret = kstrtouint(page, 10, &tmp);
	if (ret)
		return ret;
	remove_userid_all_entry(tmp);
	return count;
}

static struct configfs_attribute packages_attr_packages_gid_list = {
	.ca_name	= "packages_gid.list",
	.ca_mode	= S_IRUGO,
	.ca_owner	= THIS_MODULE,
	.show		= packages_list_show,
};

SDCARDFS_CONFIGFS_ATTR_WO(packages_, remove_userid);

#ifdef CONFIG_SDCARD_FS_LOCKER
static ssize_t packages_locker_wl_show(struct config_item *item, char *page)
{
	int i;

	page[0] = 0;
	for (i = 0; s_locker_wl[i]; ++i) {
		strcat(page, s_locker_wl[i]);
		strcat(page, ";");
	}
	page[strlen(page) - 1] = '\n';

	return strlen(page) + 1;
}

static ssize_t packages_locker_wl_store(struct config_item *item,
				       const char *page, size_t count)
{
	int i;
	char *tmp;
	char *wl;
	char *pn;

	tmp = (char *) kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!tmp) {
		return -ENOMEM;
	}

	memset(tmp, 0, count);
	memcpy(tmp, page, count);

	//pr_info("sdcardfs: %s: count=%lu, page=%s\n", __FUNCTION__, count, page);

	for (i = 0; s_locker_wl[i]; ++i) {
		kfree(s_locker_wl[i]);
		s_locker_wl[i] = NULL;
	}

	i = 0;
	wl = tmp;
	while ((i <= LOCKER_MAX_WL) && ((pn = strsep(&wl, ";")) != NULL)) {
		if (!*pn) {
			continue;
		}
		s_locker_wl[i] = kstrdup(pn, GFP_KERNEL);
		if (s_locker_wl[i] == NULL) {
			pr_err("sdcardfs: failed creating s_locker_wl entry\n");
			kfree(tmp);
			return -ENOMEM;
		}
		++i;
	}

	kfree(tmp);

	return count;
}

SDCARDFS_CONFIGFS_ATTR(packages_, locker_wl);
#endif /* CONFIG_SDCARD_FS_LOCKER */

static struct configfs_attribute *packages_attrs[] = {
	&packages_attr_packages_gid_list,
	&packages_attr_remove_userid,
#ifdef CONFIG_SDCARD_FS_LOCKER
	&packages_attr_locker_wl,
#endif /* CONFIG_SDCARD_FS_LOCKER */
	NULL,
};

/*
 * Note that, since no extra work is required on ->drop_item(),
 * no ->drop_item() is provided.
 */
static struct configfs_group_operations packages_group_ops = {
	.make_item	= packages_make_item,
};

static struct config_item_type packages_type = {
	.ct_group_ops	= &packages_group_ops,
	.ct_attrs	= packages_attrs,
	.ct_owner	= THIS_MODULE,
};

struct config_group *sd_default_groups[] = {
	&extension_group,
	NULL,
};

static struct configfs_subsystem sdcardfs_packages = {
	.su_group = {
		.cg_item = {
			.ci_namebuf = "sdcardfs",
			.ci_type = &packages_type,
		},
		.default_groups = sd_default_groups,
	},
};

static int configfs_sdcardfs_init(void)
{
	int ret, i;
	struct configfs_subsystem *subsys = &sdcardfs_packages;

	for (i = 0; sd_default_groups[i]; i++)
		config_group_init(sd_default_groups[i]);
	config_group_init(&subsys->su_group);
	mutex_init(&subsys->su_mutex);
	ret = configfs_register_subsystem(subsys);
	if (ret) {
		pr_err("Error %d while registering subsystem %s\n",
		       ret,
		       subsys->su_group.cg_item.ci_namebuf);
	}
	return ret;
}

static void configfs_sdcardfs_exit(void)
{
	configfs_unregister_subsystem(&sdcardfs_packages);
}

#ifdef CONFIG_SDCARD_FS_LOCKER
static void locker_wl_init(void)
{
	int i;
	for (i = 0; i < LOCKER_MAX_WL; ++i) {
		s_locker_wl[i] = NULL;
	}
}

static void locker_wl_free(void)
{
	int i;
	for (i = 0; i < LOCKER_MAX_WL; ++i) {
		if (s_locker_wl[i]) {
			kfree(s_locker_wl[i]);
			s_locker_wl[i] = NULL;
		}
	}
}
#endif /* CONFIG_SDCARD_FS_LOCKER */

int packagelist_init(void)
{
	hashtable_entry_cachep =
		kmem_cache_create("packagelist_hashtable_entry",
					sizeof(struct hashtable_entry), 0, 0, NULL);
	if (!hashtable_entry_cachep) {
		pr_err("sdcardfs: failed creating pkgl_hashtable entry slab cache\n");
		return -ENOMEM;
	}

#ifdef CONFIG_SDCARD_FS_LOCKER
	locker_wl_init();
#endif /* CONFIG_SDCARD_FS_LOCKER */

#ifdef CONFIG_BBSECURE_SDBASE
	bbry_policy_init();
#endif /* CONFIG_BBSECURE_SDBASE */

	configfs_sdcardfs_init();
	return 0;
}

void packagelist_exit(void)
{
	configfs_sdcardfs_exit();

#ifdef CONFIG_SDCARD_FS_LOCKER
	locker_wl_free();
#endif /* CONFIG_SDCARD_FS_LOCKER */

#ifdef CONFIG_BBSECURE_SDBASE
	bbry_policy_exit();
#endif /* CONFIG_BBSECURE_SDBASE */

	packagelist_destroy();
	kmem_cache_destroy(hashtable_entry_cachep);
}