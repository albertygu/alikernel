/*
 * Copyright Gavin Shan, Alibaba Inc 2018.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/writeback.h>

#include "ext4.h"
#include "xattr.h"

#include <trace/events/ext4.h>

enum {
	Opt_ext_delay_update_time,
	Opt_ext_wb_nice,
	Opt_ext_err,
};

static const match_table_t ext_tokens = {
	{ Opt_ext_delay_update_time,	"delayupdatetime=%d" },
	{ Opt_ext_wb_nice,		"wbnice" },
	{ Opt_ext_err,			NULL },
};

int ext4_handle_ext_mount_opt(struct super_block *sb, char *opt,
			      int ptoken, substring_t *param,
			      unsigned long *journal_devnum,
			      unsigned int *journal_ioprio,
			      int is_remount)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_ext_sb_info *sebi = &sbi->s_ext_sb_info;
	char *options, *p;
	substring_t args[MAX_OPT_ARGS];
	u64 val;
	int token, ret = 0;

	sebi->s_opt |= EXT4_EXT_OPT_VALID;
	if (!is_remount)
		mutex_init(&sebi->s_mutex);

	/* Copy over the options */
	options = match_strdup(param);
	if (!options) {
		ext4_msg(sb, KERN_ERR, "%s: Out of memory!\n", __func__);
		return -ENOMEM;
	}

	/* Parse the options */
	while ((p = strsep(&options, ";"))) {
		if (!*p)
			continue;

		args[0].from = args[0].to = NULL;
		token = match_token(p, ext_tokens, args);

		switch (token) {
		case Opt_ext_delay_update_time:
			sebi->s_opt |= EXT4_EXT_OPT_DELAY_UPDATE_TIME;
			sebi->s_delay_update_time =
				EXT4_EXT_DEFAULT_DELAY_UPDATE_TIME;

			if (!args->from)
				break;

			if (match_u64(args, &val)) {
				ret = -EINVAL;
				break;
			}

			sebi->s_delay_update_time = val;
			break;
		case Opt_ext_wb_nice:
			sebi->s_opt |= EXT4_EXT_OPT_WB_NICE;
			sebi->s_wb_enable = 1;
			break;
		default:
			ext4_msg(sb, KERN_WARNING,
				 "%s: Extended option %s not supported\n",
				 __func__, opt);
			ret = -EINVAL;
			goto out;
		}
	}

out:
	kfree(options);
	return ret;
}

struct ext4_ext_attr {
	struct attribute	attr;
	unsigned int		offset;
	unsigned int		len;
	unsigned int		opts;
};

#define EXT4_EXT_ATTR(aname, amode, astruct, aename, aopts)		\
	static struct ext4_ext_attr ext4_ext_attr_##aname = {		\
		.attr	= {						\
			.name	= __stringify(aname),			\
			.mode	= amode,				\
		},							\
		.offset	= offsetof(astruct, aename),			\
		.len	= offsetofend(astruct, aename) -		\
			  offsetof(astruct, aename),			\
		.opts	= aopts,					\
	}
#define EXT4_EXT_ATTR_RO(aname, astruct, aename, aopts)			\
	EXT4_EXT_ATTR(aname, 0444, astruct, aename, aopts)
#define EXT4_EXT_ATTR_RW(aname, astruct, aename, aopts)			\
	EXT4_EXT_ATTR(aname, 0644, astruct, aename, aopts)

EXT4_EXT_ATTR_RW(delay_update_time,
		 struct ext4_ext_sb_info,
		 s_delay_update_time,
		 EXT4_EXT_OPT_DELAY_UPDATE_TIME);

EXT4_EXT_ATTR_RW(wb_enable,
		struct ext4_ext_sb_info,
		s_wb_enable,
		EXT4_EXT_OPT_WB_NICE);

static struct attribute *ext4_ext_attrs[] = {
	&ext4_ext_attr_delay_update_time.attr,
	&ext4_ext_attr_wb_enable.attr,
	NULL
};

static ssize_t ext4_ext_attr_show(struct kobject *kobj,
				  struct attribute *attr, char *buf)
{
	struct ext4_ext_sb_info *sebi = container_of(kobj,
						struct ext4_ext_sb_info,
						s_kobj);
	struct ext4_ext_attr *ea = container_of(attr,
						struct ext4_ext_attr, attr);
	unsigned long val = 0;
	void *pval;

	pval = (void *)sebi + ea->offset;
	mutex_lock(&sebi->s_mutex);
	memcpy(&val, pval, ea->len);
	mutex_unlock(&sebi->s_mutex);

	return snprintf(buf, PAGE_SIZE, "%lu\n", val);
}

static ssize_t ext4_ext_attr_store(struct kobject *kobj,
				   struct attribute *attr,
				   const char *buf, size_t len)
{
	struct ext4_ext_sb_info *sebi = container_of(kobj,
						struct ext4_ext_sb_info,
						s_kobj);
	struct ext4_ext_attr *ea = container_of(attr,
						struct ext4_ext_attr, attr);
	unsigned long val = 0;
	void *pval;
	int ret;

	ret = kstrtoul(skip_spaces(buf), 0, &val);
	if (ret)
		return ret;

	pval = (void *)sebi + ea->offset;
	mutex_lock(&sebi->s_mutex);
	memcpy(pval, &val, ea->len);
	mutex_unlock(&sebi->s_mutex);

	return len;
}

static const struct sysfs_ops ext4_ext_attr_ops = {
	.show	= ext4_ext_attr_show,
	.store	= ext4_ext_attr_store,
};

static struct kobj_type ext4_ext_sb_ktype = {
	.default_attrs	= NULL,
	.sysfs_ops	= &ext4_ext_attr_ops,
};

int ext4_register_ext_sysfs(struct super_block *sb)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_ext_sb_info *sebi = &sbi->s_ext_sb_info;
	struct ext4_ext_attr *ea;
	struct attribute *attr;
	int i, ret;

	if (!(sebi->s_opt & EXT4_EXT_OPT_VALID))
		return 0;

	ret = kobject_init_and_add(&sebi->s_kobj, &ext4_ext_sb_ktype,
				   &sbi->s_kobj, "extend");
	if (ret)
		return ret;

	for (i = 0; (attr = ext4_ext_attrs[i]) != NULL; i++) {
		ea = container_of(attr, struct ext4_ext_attr, attr);
		if (ea->opts && (sebi->s_opt & ea->opts) != ea->opts)
			continue;

		ret = sysfs_create_file(&sebi->s_kobj, attr);
		if (ret) {
			kobject_del(&sebi->s_kobj);
			kobject_put(&sebi->s_kobj);
			return ret;
		}
	}

	return 0;
}

void ext4_unregister_ext_sysfs(struct super_block *sb)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_ext_sb_info *sebi = &sbi->s_ext_sb_info;

	if (!(sebi->s_opt & EXT4_EXT_OPT_VALID))
		return;

	kobject_del(&sebi->s_kobj);
	kobject_put(&sebi->s_kobj);
}

static inline bool ext4_ext_should_update_time(struct timespec *old,
					       struct timespec *new,
					       unsigned int delay)
{
	unsigned long elapse;

	elapse = ((new->tv_sec - old->tv_sec) * MSEC_PER_SEC) +
		 ((new->tv_nsec - old->tv_nsec) / NSEC_PER_MSEC);
	return (elapse >= delay);
}

int ext4_ext_update_time(struct inode *inode, struct timespec *tm, int flags)
{
	struct super_block *sb = inode->i_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_ext_sb_info *sebi = &sbi->s_ext_sb_info;
	unsigned int delay = READ_ONCE(sebi->s_delay_update_time);

	if (!(sebi->s_opt & EXT4_EXT_OPT_DELAY_UPDATE_TIME))
		goto update;

	if (!delay)
		goto update;

	if (flags & S_VERSION)
		goto update;

	if ((flags & S_ATIME))
		goto update;

	if ((flags & S_CTIME) &&
	    ext4_ext_should_update_time(&inode->i_ctime, tm, delay))
		goto update;

	if ((flags & S_MTIME) &&
	    ext4_ext_should_update_time(&inode->i_mtime, tm, delay))
		goto update;

	return 0;

update:
	return generic_update_time(inode, tm, flags);
}

long ext4_ext_limit_writeback(struct inode *inode,
				struct writeback_control *wbc)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	struct ext4_ext_sb_info *sebi = &sbi->s_ext_sb_info;
	long nr_to_write = wbc->nr_to_write;
	const char *name = "wbnice";
	char buf[256];
	long nice, pages;
	int size;

	/*
	 * We have 4MB minimal chunk size, compatible with
	 * MIN_WRITEBACK_PAGES defined in fs/fs-writeback.c.
	 */
	#define EXT4_EXT_MIN_WB_PAGES	(0x400000 / PAGE_SIZE)

	if (!((sebi->s_opt & EXT4_EXT_OPT_WB_NICE) && (sebi->s_wb_enable)))
		goto out;

	/* No limitation on explicit synchronization */
	if (wbc->nr_to_write == LONG_MAX)
		goto out;

	/* Get user.wbnice attribute value size */
	size = ext4_xattr_get(inode, EXT4_XATTR_INDEX_USER, name, NULL, 0);
	if (size <= 0 || size >= sizeof(buf))
		goto out;

	/* Get user.wbnice attribute value */
	buf[size] = '\0';
	size = ext4_xattr_get(inode, EXT4_XATTR_INDEX_USER,
			name, buf, sizeof(buf));
	nice = simple_strtoul(buf, NULL, 0);
	if (nice <= 0)
		goto out;
	if (nice > 255)
		nice = 255;

	/*
	 * Round nice to next power of 2 and square, and calculate writeback
	 * page number inversely with nice.
	 */
	pages = wbc->nr_to_write / roundup_pow_of_two(nice);
	wbc->nr_to_write = round_down(pages + EXT4_EXT_MIN_WB_PAGES,
			EXT4_EXT_MIN_WB_PAGES);
	trace_ext4_ext_writepages(inode, nr_to_write, wbc);
out:
	return wbc->nr_to_write;
}
