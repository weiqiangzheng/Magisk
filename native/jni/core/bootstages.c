/* bootstages.c - Core bootstage operations
 *
 * All bootstage operations, including simple mount in post-fs,
 * magisk mount in post-fs-data, various image handling, script
 * execution, load modules, install Magisk Manager etc.
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <dirent.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <selinux/selinux.h>

#include "magisk.h"
#include "db.h"
#include "utils.h"
#include "daemon.h"
#include "resetprop.h"
#include "magiskpolicy.h"

static char *buf, *buf2;
static struct vector module_list;

extern char **environ;

/******************
 * Node structure *
 ******************/

// Precedence: MODULE > SKEL > INTER > DUMMY
#define IS_DUMMY   0x01    /* mount from mirror */
#define IS_INTER   0x02    /* intermediate node */
#define IS_SKEL    0x04    /* mount from skeleton */
#define IS_MODULE  0x08    /* mount from module */

#define IS_DIR(n)  (n->type == DT_DIR)
#define IS_LNK(n)  (n->type == DT_LNK)
#define IS_REG(n)  (n->type == DT_REG)

struct node_entry {
	const char *module;    /* Only used when status & IS_MODULE */
	char *name;
	uint8_t type;
	uint8_t status;
	struct node_entry *parent;
	struct vector *children;
};

static void concat_path(struct node_entry *node) {
	if (node->parent)
		concat_path(node->parent);
	size_t len = strlen(buf);
	buf[len] = '/';
	strcpy(buf + len + 1, node->name);
}

static char *get_full_path(struct node_entry *node) {
	buf[0] = '\0';
	concat_path(node);
	return strdup(buf);
}

// Free the node
static void destroy_node(struct node_entry *node) {
	free(node->name);
	vec_destroy(node->children);
	free(node->children);
	free(node);
}

// Free the node and all children recursively
static void destroy_subtree(struct node_entry *node) {
	// Never free parent, since it shall be freed by themselves
	struct node_entry *e;
	vec_for_each(node->children, e) {
		destroy_subtree(e);
	}
	destroy_node(node);
}

// Return the child
static struct node_entry *insert_child(struct node_entry *p, struct node_entry *c) {
	c->parent = p;
	if (p->children == NULL) {
		p->children = xmalloc(sizeof(struct vector));
		vec_init(p->children);
	}
	struct node_entry *e;
	vec_for_each(p->children, e) {
		if (strcmp(e->name, c->name) == 0) {
			// Exist duplicate
			if (c->status > e->status) {
				// Precedence is higher, replace with new node
				destroy_subtree(e);
				vec_cur(p->children) = c;
				return c;
			} else {
				// Free the new entry, return old
				destroy_node(c);
				return e;
			}
		}
	}
	// New entry, push back
	vec_push_back(p->children, c);
	return c;
}

/***********
 * setenvs *
 ***********/

static void set_path(struct vector *v) {
	for (int i = 0; environ[i]; ++i) {
		if (strncmp(environ[i], "PATH=", 5) == 0) {
			vec_push_back(v, strdup("PATH=" BBPATH ":/sbin:" MIRRDIR "/system/bin:"
						MIRRDIR "/system/xbin:" MIRRDIR "/vendor/bin"));
		} else {
			vec_push_back(v, strdup(environ[i]));
		}
	}
	vec_push_back(v, NULL);
}

static void uninstall_env(struct vector *v) {
	vec_push_back(v, strdup("BOOTMODE=true"));
	set_path(v);
}

/***********
 * Scripts *
 ***********/

static void exec_common_script(const char* stage) {
	DIR *dir;
	struct dirent *entry;
	snprintf(buf2, PATH_MAX, "%s/%s.d", COREDIR, stage);

	if (!(dir = xopendir(buf2)))
		return;

	while ((entry = xreaddir(dir))) {
		if (entry->d_type == DT_REG) {
			snprintf(buf2, PATH_MAX, "%s/%s.d/%s", COREDIR, stage, entry->d_name);
			if (access(buf2, X_OK) == -1)
				continue;
			LOGI("%s.d: exec [%s]\n", stage, entry->d_name);
			int pid = exec_command(0, NULL, set_path, "sh", buf2, NULL);
			if (pid != -1)
				waitpid(pid, NULL, 0);
		}
	}

	closedir(dir);
}

static void exec_module_script(const char* stage) {
	char *module;
	vec_for_each(&module_list, module) {
		snprintf(buf2, PATH_MAX, "%s/%s/%s.sh", MOUNTPOINT, module, stage);
		snprintf(buf, PATH_MAX, "%s/%s/disable", MOUNTPOINT, module);
		if (access(buf2, F_OK) == -1 || access(buf, F_OK) == 0)
			continue;
		LOGI("%s: exec [%s.sh]\n", module, stage);
		int pid = exec_command(0, NULL, set_path, "sh", buf2, NULL);
		if (pid != -1)
			waitpid(pid, NULL, 0);
	}

}

/***************
 * Magic Mount *
 ***************/

static void construct_tree(const char *module, struct node_entry *parent) {
	DIR *dir;
	struct dirent *entry;
	struct node_entry *node;

	char *parent_path = get_full_path(parent);
	snprintf(buf, PATH_MAX, "%s/%s%s", MOUNTPOINT, module, parent_path);

	if (!(dir = xopendir(buf)))
		goto cleanup;

	while ((entry = xreaddir(dir))) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		// Create new node
		node = xcalloc(sizeof(*node), 1);
		node->module = module;
		node->name = strdup(entry->d_name);
		node->type = entry->d_type;
		snprintf(buf, PATH_MAX, "%s/%s", parent_path, node->name);

		/*
		 * Clone the parent in the following condition:
		 * 1. File in module is a symlink
		 * 2. Target file do not exist
		 * 3. Target file is a symlink, but not /system/vendor
		 */
		int clone = 0;
		if (IS_LNK(node) || access(buf, F_OK) == -1) {
			clone = 1;
		} else if (parent->parent != NULL || strcmp(node->name, "vendor") != 0) {
			struct stat s;
			xstat(buf, &s);
			if (S_ISLNK(s.st_mode))
				clone = 1;
		}

		if (clone) {
			// Mark the parent folder as a skeleton
			parent->status |= IS_SKEL;  /* This will not overwrite if parent is module */
			node->status = IS_MODULE;
		} else if (IS_DIR(node)) {
			// Check if marked as replace
			snprintf(buf2, PATH_MAX, "%s/%s%s/.replace", MOUNTPOINT, module, buf);
			if (access(buf2, F_OK) == 0) {
				// Replace everything, mark as leaf
				node->status = IS_MODULE;
			} else {
				// This will be an intermediate node
				node->status = IS_INTER;
			}
		} else if (IS_REG(node)) {
			// This is a leaf, mark as target
			node->status = IS_MODULE;
		}
		node = insert_child(parent, node);
		if (node->status & (IS_SKEL | IS_INTER)) {
			// Intermediate folder, travel deeper
			construct_tree(module, node);
		}
	}

	closedir(dir);

cleanup:
	free(parent_path);
}

static void clone_skeleton(struct node_entry *node) {
	DIR *dir;
	struct dirent *entry;
	struct node_entry *dummy, *child;

	// Clone the structure
	char *full_path = get_full_path(node);
	snprintf(buf, PATH_MAX, "%s%s", MIRRDIR, full_path);
	if (!(dir = xopendir(buf)))
		goto cleanup;
	while ((entry = xreaddir(dir))) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		// Create dummy node
		dummy = xcalloc(sizeof(*dummy), 1);
		dummy->name = strdup(entry->d_name);
		dummy->type = entry->d_type;
		dummy->status = IS_DUMMY;
		insert_child(node, dummy);
	}
	closedir(dir);

	if (node->status & IS_SKEL) {
		struct stat s;
		char *con;
		xstat(full_path, &s);
		getfilecon(full_path, &con);
		LOGI("mnt_tmpfs : %s\n", full_path);
		xmount("tmpfs", full_path, "tmpfs", 0, NULL);
		chmod(full_path, s.st_mode & 0777);
		chown(full_path, s.st_uid, s.st_gid);
		setfilecon(full_path, con);
		free(con);
	}

	vec_for_each(node->children, child) {
		snprintf(buf, PATH_MAX, "%s/%s", full_path, child->name);

		// Create the dummy file/directory
		if (IS_DIR(child))
			xmkdir(buf, 0755);
		else if (IS_REG(child))
			close(creat(buf, 0644));
		// Links will be handled later

		if (child->parent->parent == NULL && strcmp(child->name, "vendor") == 0) {
			if (IS_LNK(child)) {
				cp_afc(MIRRDIR "/system/vendor", "/system/vendor");
				LOGI("creat_link: %s <- %s\n", "/system/vendor", MIRRDIR "/system/vendor");
			}
			// Skip
			continue;
		} else if (child->status & IS_MODULE) {
			// Mount from module file to dummy file
			snprintf(buf2, PATH_MAX, "%s/%s%s/%s", MOUNTPOINT, child->module, full_path, child->name);
		} else if (child->status & (IS_SKEL | IS_INTER)) {
			// It's an intermediate folder, recursive clone
			clone_skeleton(child);
			continue;
		} else if (child->status & IS_DUMMY) {
			// Mount from mirror to dummy file
			snprintf(buf2, PATH_MAX, "%s%s/%s", MIRRDIR, full_path, child->name);
		}

		if (IS_LNK(child)) {
			// Copy symlinks directly
			cp_afc(buf2, buf);
#ifdef MAGISK_DEBUG
			LOGI("creat_link: %s <- %s\n",buf, buf2);
#else
			LOGI("creat_link: %s\n", buf);
#endif
		} else {
			snprintf(buf, PATH_MAX, "%s/%s", full_path, child->name);
			bind_mount(buf2, buf);
		}
	}

cleanup:
	free(full_path);
}

static void magic_mount(struct node_entry *node) {
	char *real_path;
	struct node_entry *child;

	if (node->status & IS_MODULE) {
		// The real deal, mount module item
		real_path = get_full_path(node);
		snprintf(buf, PATH_MAX, "%s/%s%s", MOUNTPOINT, node->module, real_path);
		bind_mount(buf, real_path);
		free(real_path);
	} else if (node->status & IS_SKEL) {
		// The node is labeled to be cloned with skeleton, lets do it
		clone_skeleton(node);
	} else if (node->status & IS_INTER) {
		// It's an intermediate node, travel deeper
		vec_for_each(node->children, child)
			magic_mount(child);
	}
	// The only thing goes here should be vendor placeholder
	// There should be no dummies, so don't need to handle it here
}

/****************
 * Simple Mount *
 ****************/

static void simple_mount(const char *path) {
	DIR *dir;
	struct dirent *entry;

	snprintf(buf, PATH_MAX, "%s%s", SIMPLEMOUNT, path);
	if (!(dir = opendir(buf)))
		return;

	while ((entry = xreaddir(dir))) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		// Target file path
		snprintf(buf2, PATH_MAX, "%s/%s", path, entry->d_name);
		// Only mount existing file
		if (access(buf2, F_OK) == -1)
			continue;
		if (entry->d_type == DT_DIR) {
			char *new_path = strdup(buf2);
			simple_mount(new_path);
			free(new_path);
		} else if (entry->d_type == DT_REG) {
			// Actual file path
			snprintf(buf, PATH_MAX, "%s%s", SIMPLEMOUNT, buf2);
			// Clone all attributes
			clone_attr(buf2, buf);
			// Finally, mount the file
			bind_mount(buf, buf2);
		}
	}

	closedir(dir);
}

/*****************
 * Miscellaneous *
 *****************/

#define alt_img ((char *[]) \
{ "/cache/magisk.img", "/data/magisk_merge.img", "/data/adb/magisk_merge.img", NULL })

static int prepare_img() {
	// Merge images
	for (int i = 0; alt_img[i]; ++i) {
		if (merge_img(alt_img[i], MAINIMG)) {
			LOGE("Image merge %s -> " MAINIMG " failed!\n", alt_img[i]);
			return 1;
		}
	}

	if (access(MAINIMG, F_OK) == -1) {
		if (create_img(MAINIMG, 64))
			return 1;
	}

	LOGI("* Mounting " MAINIMG "\n");
	// Mounting magisk image
	char *magiskloop = mount_image(MAINIMG, MOUNTPOINT);
	if (magiskloop == NULL)
		return 1;

	xmkdir(COREDIR, 0755);
	xmkdir(COREDIR "/post-fs-data.d", 0755);
	xmkdir(COREDIR "/service.d", 0755);
	xmkdir(COREDIR "/props", 0755);

	DIR *dir = xopendir(MOUNTPOINT);
	struct dirent *entry;
	while ((entry = xreaddir(dir))) {
		if (entry->d_type == DT_DIR) {
			if (strcmp(entry->d_name, ".") == 0 ||
				strcmp(entry->d_name, "..") == 0 ||
				strcmp(entry->d_name, ".core") == 0 ||
				strcmp(entry->d_name, "lost+found") == 0)
				continue;
			snprintf(buf, PATH_MAX, "%s/%s/remove", MOUNTPOINT, entry->d_name);
			if (access(buf, F_OK) == 0) {
				snprintf(buf, PATH_MAX, "%s/%s", MOUNTPOINT, entry->d_name);
				rm_rf(buf);
				continue;
			}
			snprintf(buf, PATH_MAX, "%s/%s/update", MOUNTPOINT, entry->d_name);
			unlink(buf);
			snprintf(buf, PATH_MAX, "%s/%s/disable", MOUNTPOINT, entry->d_name);
			if (access(buf, F_OK) == 0)
				continue;
			vec_push_back(&module_list, strdup(entry->d_name));
		}
	}

	closedir(dir);

	// Trim image
	umount_image(MOUNTPOINT, magiskloop);
	free(magiskloop);
	trim_img(MAINIMG);

	// Remount them back :)
	magiskloop = mount_image(MAINIMG, MOUNTPOINT);
	free(magiskloop);
	return 0;
}

void install_apk(const char *apk) {
	setfilecon(apk, "u:object_r:"SEPOL_FILE_DOMAIN":s0");
	while (1) {
		sleep(5);
		LOGD("apk_install: attempting to install APK");
		int apk_res = -1, pid;
		pid = exec_command(1, &apk_res, NULL, "/system/bin/pm", "install", "-r", apk, NULL);
		if (pid != -1) {
			int err = 0;
			while (fdgets(buf, PATH_MAX, apk_res) > 0) {
				LOGD("apk_install: %s", buf);
				err |= strstr(buf, "Error:") != NULL;
			}
			waitpid(pid, NULL, 0);
			close(apk_res);
			// Keep trying until pm is started
			if (err)
				continue;
			break;
		}
	}
	unlink(apk);
}

/****************
 * Entry points *
 ****************/

static void unblock_boot_process() {
	close(xopen(UNBLOCKFILE, O_RDONLY | O_CREAT, 0));
	pthread_exit(NULL);
}

static const char wrapper[] =
"#!/system/bin/sh\n"
"unset LD_LIBRARY_PATH\n"
"unset LD_PRELOAD\n"
"exec /sbin/magisk.bin \"${0##*/}\" \"$@\"\n";

void startup() {
	if (!check_data())
		unblock_boot_process();

	if (access(SECURE_DIR, F_OK) != 0) {
		/* If the folder is not automatically created by the system,
		 * do NOT proceed further. Manual creation of the folder
		 * will cause bootloops on FBE devices. */
		LOGE(SECURE_DIR" is not present, abort...");
		unblock_boot_process();
	}

	// No uninstaller or core-only mode
	if (access(UNINSTALLER, F_OK) != 0 && access(DISABLEFILE, F_OK) != 0) {
		// Allocate buffer
		buf = xmalloc(PATH_MAX);
		buf2 = xmalloc(PATH_MAX);

		simple_mount("/system");
		simple_mount("/vendor");
	}

	LOGI("** Initializing Magisk\n");

	// Unlock all blocks for rw
	unlock_blocks();

	LOGI("* Creating /sbin overlay");
	DIR *dir;
	struct dirent *entry;
	int root, sbin, fd;
	char buf[PATH_MAX];
	void *magisk, *init;
	size_t magisk_size, init_size;

	xmount(NULL, "/", NULL, MS_REMOUNT, NULL);

	// Remove some traits of Magisk
	unlink("/init.magisk.rc");

	// Create hardlink mirror of /sbin to /root
	mkdir("/root", 0750);
	full_read("/sbin/magisk", &magisk, &magisk_size);
	unlink("/sbin/magisk");
	full_read("/sbin/magiskinit", &init, &init_size);
	unlink("/sbin/magiskinit");
	root = xopen("/root", O_RDONLY | O_CLOEXEC);
	sbin = xopen("/sbin", O_RDONLY | O_CLOEXEC);
	link_dir(sbin, root);
	close(sbin);

	// Mount the /sbin tmpfs overlay
	xmount("tmpfs", "/sbin", "tmpfs", 0, NULL);
	chmod("/sbin", 0755);
	setfilecon("/sbin", "u:object_r:rootfs:s0");
	sbin = xopen("/sbin", O_RDONLY | O_CLOEXEC);

	// Create wrapper
	fd = creat("/sbin/magisk", 0755);
	xwrite(fd, wrapper, sizeof(wrapper) - 1);
	close(fd);
	setfilecon("/sbin/magisk", "u:object_r:"SEPOL_FILE_DOMAIN":s0");
	// Setup magisk symlinks
	fd = creat("/sbin/magisk.bin", 0755);
	xwrite(fd, magisk, magisk_size);
	close(fd);
	free(magisk);
	setfilecon("/sbin/magisk.bin", "u:object_r:"SEPOL_FILE_DOMAIN":s0");
	for (int i = 0; applet[i]; ++i) {
		snprintf(buf, PATH_MAX, "/sbin/%s", applet[i]);
		xsymlink("/sbin/magisk", buf);
	}

	// Setup magiskinit symlinks
	fd = creat("/sbin/magiskinit", 0755);
	xwrite(fd, init, init_size);
	close(fd);
	free(init);
	setfilecon("/sbin/magiskinit", "u:object_r:"SEPOL_FILE_DOMAIN":s0");
	for (int i = 0; init_applet[i]; ++i) {
		snprintf(buf, PATH_MAX, "/sbin/%s", init_applet[i]);
		xsymlink("/sbin/magiskinit", buf);
	}

	// Create symlinks pointing back to /root
	dir = xfdopendir(root);
	while((entry = xreaddir(dir))) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
		snprintf(buf, PATH_MAX, "/root/%s", entry->d_name);
		symlinkat(buf, sbin, entry->d_name);
	}

	close(sbin);
	close(root);

	// Alternative binaries paths
	char *alt_bin[] = { "/cache/data_bin", "/data/magisk",
						"/data/data/com.topjohnwu.magisk/install",
						"/data/user_de/0/com.topjohnwu.magisk/install", NULL };
	char *bin_path = NULL;
	for (int i = 0; alt_bin[i]; ++i) {
		if (access(alt_bin[i], F_OK) == 0) {
			bin_path = alt_bin[i];
			break;
		}
	}
	if (bin_path) {
		rm_rf(DATABIN);
		cp_afc(bin_path, DATABIN);
		rm_rf(bin_path);
	}

	// Remove legacy stuffs
	unlink("/data/magisk.img");
	unlink("/data/magisk_debug.log");

	// Create directories in tmpfs overlay
	xmkdirs(MIRRDIR "/system", 0755);
	xmkdir(MIRRDIR "/bin", 0755);
	xmkdir(BBPATH, 0755);
	xmkdir(MOUNTPOINT, 0755);

	LOGI("* Mounting mirrors");
	struct vector mounts;
	vec_init(&mounts);
	file_to_vector("/proc/mounts", &mounts);
	char *line;
	int skip_initramfs = 0;
	// Check whether skip_initramfs device
	vec_for_each(&mounts, line) {
		if (strstr(line, " /system_root ")) {
			bind_mount("/system_root/system", MIRRDIR "/system");
			skip_initramfs = 1;
		} else if (!skip_initramfs && strstr(line, " /system ")) {
			sscanf(line, "%s", buf);
			xmount(buf, MIRRDIR "/system", "ext4", MS_RDONLY, NULL);
#ifdef MAGISK_DEBUG
			LOGI("mount: %s <- %s\n", MIRRDIR "/system", buf);
#else
			LOGI("mount: %s\n", MIRRDIR "/system");
#endif
		} else if (strstr(line, " /vendor ")) {
			seperate_vendor = 1;
			sscanf(line, "%s", buf);
			xmkdir(MIRRDIR "/vendor", 0755);
			xmount(buf, MIRRDIR "/vendor", "ext4", MS_RDONLY, NULL);
#ifdef MAGISK_DEBUG
			LOGI("mount: %s <- %s\n", MIRRDIR "/vendor", buf);
#else
			LOGI("mount: %s\n", MIRRDIR "/vendor");
#endif
		}
		free(line);
	}
	vec_destroy(&mounts);
	if (!seperate_vendor) {
		xsymlink(MIRRDIR "/system/vendor", MIRRDIR "/vendor");
#ifdef MAGISK_DEBUG
		LOGI("link: %s <- %s\n", MIRRDIR "/vendor", MIRRDIR "/system/vendor");
#else
		LOGI("link: %s\n", MIRRDIR "/vendor");
#endif
	}
	xmkdirs(DATABIN, 0755);
	bind_mount(DATABIN, MIRRDIR "/bin");
	if (access(MIRRDIR "/bin/busybox", X_OK) == 0) {
		LOGI("* Setting up internal busybox");
		exec_command_sync(MIRRDIR "/bin/busybox", "--install", "-s", BBPATH, NULL);
		xsymlink(MIRRDIR "/bin/busybox", BBPATH "/busybox");
	}

	// uninstall
	if (access(UNINSTALLER, F_OK) == 0) {
		close(open(UNBLOCKFILE, O_RDONLY | O_CREAT));
		exec_command(0, NULL, uninstall_env, "sh", UNINSTALLER, NULL);
		return;
	}

	// Start post-fs-data mode
	execl("/sbin/magisk", "magisk", "--post-fs-data", NULL);
}

void post_fs_data(int client) {
	// ack
	write_int(client, 0);
	close(client);

	// If post-fs-data mode is started, it means startup succeeded
	setup_done = 1;

	// Start the debug log
	start_debug_full_log();

	LOGI("** post-fs-data mode running\n");

	xmount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL);
	full_patch_pid = exec_command(0, NULL, NULL, "/sbin/magiskpolicy", "--live", "allow "SEPOL_PROC_DOMAIN" * * *", NULL);

	// Allocate buffer
	buf = xmalloc(PATH_MAX);
	buf2 = xmalloc(PATH_MAX);
	vec_init(&module_list);

	// Merge, trim, mount magisk.img, which will also travel through the modules
	// After this, it will create the module list
	if (prepare_img())
		goto core_only; // Mounting fails, we can only do core only stuffs

	restorecon();
	chmod(SECURE_DIR, 0700);

	// Run common scripts
	LOGI("* Running post-fs-data.d scripts\n");
	exec_common_script("post-fs-data");

	// Core only mode
	if (access(DISABLEFILE, F_OK) == 0)
		goto core_only;

	// Execute module scripts
	LOGI("* Running module post-fs-data scripts\n");
	exec_module_script("post-fs-data");

	char *module;
	struct node_entry *sys_root, *ven_root = NULL, *child;

	// Create the system root entry
	sys_root = xcalloc(sizeof(*sys_root), 1);
	sys_root->name = strdup("system");
	sys_root->status = IS_INTER;

	int has_modules = 0;

	LOGI("* Loading modules\n");
	vec_for_each(&module_list, module) {
		// Read props
		snprintf(buf, PATH_MAX, "%s/%s/system.prop", MOUNTPOINT, module);
		if (access(buf, F_OK) == 0) {
			LOGI("%s: loading [system.prop]\n", module);
			read_prop_file(buf, 0);
		}
		// Check whether enable auto_mount
		snprintf(buf, PATH_MAX, "%s/%s/auto_mount", MOUNTPOINT, module);
		if (access(buf, F_OK) == -1)
			continue;
		// Double check whether the system folder exists
		snprintf(buf, PATH_MAX, "%s/%s/system", MOUNTPOINT, module);
		if (access(buf, F_OK) == -1)
			continue;

		// Construct structure
		has_modules = 1;
		LOGI("%s: constructing magic mount structure\n", module);
		// If /system/vendor exists in module, create a link outside
		snprintf(buf, PATH_MAX, "%s/%s/system/vendor", MOUNTPOINT, module);
		if (access(buf, F_OK) == 0) {
			snprintf(buf2, PATH_MAX, "%s/%s/vendor", MOUNTPOINT, module);
			unlink(buf2);
			xsymlink(buf, buf2);
		}
		construct_tree(module, sys_root);
	}

	if (has_modules) {
		// Extract the vendor node out of system tree and swap with placeholder
		vec_for_each(sys_root->children, child) {
			if (strcmp(child->name, "vendor") == 0) {
				ven_root = child;
				child = xcalloc(sizeof(*child), 1);
				child->type = seperate_vendor ? DT_LNK : DT_DIR;
				child->parent = ven_root->parent;
				child->name = strdup("vendor");
				child->status = 0;
				// Swap!
				vec_cur(sys_root->children) = child;
				ven_root->parent = NULL;
				break;
			}
		}

		// Magic!!
		magic_mount(sys_root);
		if (ven_root) magic_mount(ven_root);
	}

	// Cleanup memory
	destroy_subtree(sys_root);
	if (ven_root) destroy_subtree(ven_root);

core_only:
	// Systemless hosts
	if (access(HOSTSFILE, F_OK) == 0) {
		LOGI("* Enabling systemless hosts file support");
		bind_mount(HOSTSFILE, "/system/etc/hosts");
	}

	auto_start_magiskhide();
	unblock_boot_process();
}

void late_start(int client) {
	LOGI("** late_start service mode running\n");
	// ack
	write_int(client, 0);
	close(client);

	if (access(SECURE_DIR, F_OK) != 0) {
		// It's safe to create the folder at this point if the system didn't create it
		xmkdir(SECURE_DIR, 0700);
	}

	if (!setup_done) {
		// The setup failed for some reason, reboot and try again
		exec_command_sync("/system/bin/reboot", NULL);
		return;
	}

	// Allocate buffer
	if (buf == NULL) buf = xmalloc(PATH_MAX);
	if (buf2 == NULL) buf2 = xmalloc(PATH_MAX);

	// Wait till the full patch is done
	if (full_patch_pid > 0)
		waitpid(full_patch_pid, NULL, 0);

	// Run scripts after full patch, most reliable way to run scripts
	LOGI("* Running service.d scripts\n");
	exec_common_script("service");

	// Core only mode
	if (access(DISABLEFILE, F_OK) == 0)
		goto core_only;

	LOGI("* Running module service scripts\n");
	exec_module_script("service");

core_only:
	if (access(MANAGERAPK, F_OK) == 0) {
		// Install Magisk Manager if exists
		rename(MANAGERAPK, "/data/magisk.apk");
		install_apk("/data/magisk.apk");
	} else {
		// Check whether we have a valid manager installed
		sqlite3 *db = get_magiskdb();
		struct db_strings str;
		INIT_DB_STRINGS(&str);
		get_db_strings(db, SU_MANAGER, &str);
		if (validate_manager(str.s[SU_MANAGER], 0, NULL)) {
			// There is no manager installed, install the stub
			exec_command_sync("/sbin/magiskinit", "-x", "manager", "/data/magisk.apk", NULL);
			install_apk("/data/magisk.apk");
		}
	}

	// All boot stage done, cleanup everything
	free(buf);
	free(buf2);
	buf = buf2 = NULL;
	vec_deep_destroy(&module_list);

	stop_debug_full_log();
}
