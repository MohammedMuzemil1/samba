/*
 * Unix SMB/CIFS implementation.
 * Samba internal messaging functions
 * Copyright (C) 2013 by Volker Lendecke
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "includes.h"
#include "lib/util/data_blob.h"
#include "lib/util/debug.h"
#include "lib/unix_msg/unix_msg.h"
#include "system/filesys.h"
#include "lib/messages_dgm.h"
#include "lib/param/param.h"
#include "poll_funcs/poll_funcs_tevent.h"
#include "unix_msg/unix_msg.h"

struct messaging_dgm_context {
	pid_t pid;
	struct poll_funcs *msg_callbacks;
	void *tevent_handle;
	struct unix_msg_ctx *dgm_ctx;
	char *cache_dir;
	int lockfile_fd;

	void (*recv_cb)(const uint8_t *msg,
			size_t msg_len,
			void *private_data);
	void *recv_cb_private_data;

	bool *have_dgm_context;
};

static void messaging_dgm_recv(struct unix_msg_ctx *ctx,
			       uint8_t *msg, size_t msg_len,
			       void *private_data);

static int messaging_dgm_context_destructor(struct messaging_dgm_context *c);

static int messaging_dgm_lockfile_create(TALLOC_CTX *tmp_ctx,
					 const char *cache_dir,
					 uid_t dir_owner, pid_t pid,
					 int *plockfile_fd, uint64_t unique)
{
	fstring buf;
	char *dir;
	char *lockfile_name;
	int lockfile_fd;
	struct flock lck;
	int unique_len, ret;
	ssize_t written;
	bool ok;

	dir = talloc_asprintf(tmp_ctx, "%s/lck", cache_dir);
	if (dir == NULL) {
		return ENOMEM;
	}

	ok = directory_create_or_exist_strict(dir, dir_owner, 0755);
	if (!ok) {
		ret = errno;
		DEBUG(1, ("%s: Could not create lock directory: %s\n",
			  __func__, strerror(ret)));
		TALLOC_FREE(dir);
		return ret;
	}

	lockfile_name = talloc_asprintf(tmp_ctx, "%s/%u", dir,
					(unsigned)pid);
	TALLOC_FREE(dir);
	if (lockfile_name == NULL) {
		DEBUG(1, ("%s: talloc_asprintf failed\n", __func__));
		return ENOMEM;
	}

	/* no O_EXCL, existence check is via the fcntl lock */

	lockfile_fd = open(lockfile_name, O_NONBLOCK|O_CREAT|O_WRONLY, 0644);
	if (lockfile_fd == -1) {
		ret = errno;
		DEBUG(1, ("%s: open failed: %s\n", __func__, strerror(errno)));
		goto fail_free;
	}

	lck = (struct flock) {
		.l_type = F_WRLCK,
		.l_whence = SEEK_SET
	};

	ret = fcntl(lockfile_fd, F_SETLK, &lck);
	if (ret == -1) {
		ret = errno;
		DEBUG(1, ("%s: fcntl failed: %s\n", __func__, strerror(ret)));
		goto fail_close;
	}

	unique_len = snprintf(buf, sizeof(buf), "%"PRIu64, unique);

	/* shorten a potentially preexisting file */

	ret = ftruncate(lockfile_fd, unique_len);
	if (ret == -1) {
		ret = errno;
		DEBUG(1, ("%s: ftruncate failed: %s\n", __func__,
			  strerror(ret)));
		goto fail_unlink;
	}

	written = write(lockfile_fd, buf, unique_len);
	if (written != unique_len) {
		ret = errno;
		DEBUG(1, ("%s: write failed: %s\n", __func__, strerror(ret)));
		goto fail_unlink;
	}

	TALLOC_FREE(lockfile_name);
	*plockfile_fd = lockfile_fd;
	return 0;

fail_unlink:
	unlink(lockfile_name);
fail_close:
	close(lockfile_fd);
fail_free:
	TALLOC_FREE(lockfile_name);
	return ret;
}

static int messaging_dgm_lockfile_remove(TALLOC_CTX *tmp_ctx,
					 const char *cache_dir, pid_t pid)
{
	char *lockfile_name;
	int ret;

	lockfile_name = talloc_asprintf(
		tmp_ctx, "%s/lck/%u", cache_dir, (unsigned)pid);
	if (lockfile_name == NULL) {
		return ENOMEM;
	}

	ret = unlink(lockfile_name);
	if (ret == -1) {
		ret = errno;
		DEBUG(10, ("%s: unlink(%s) failed: %s\n", __func__,
			   lockfile_name, strerror(ret)));
	}

	TALLOC_FREE(lockfile_name);
	return ret;
}

int messaging_dgm_init(TALLOC_CTX *mem_ctx,
		       struct tevent_context *ev,
		       struct server_id pid,
		       const char *cache_dir,
		       uid_t dir_owner,
		       void (*recv_cb)(const uint8_t *msg,
				       size_t msg_len,
				       void *private_data),
		       void *recv_cb_private_data,
		       struct messaging_dgm_context **pctx)
{
	struct messaging_dgm_context *ctx;
	int ret;
	bool ok;
	char *socket_dir;
	struct sockaddr_un socket_address;
	size_t sockname_len;
	uint64_t cookie;
	static bool have_dgm_context = false;

	if (have_dgm_context) {
		return EEXIST;
	}

	ctx = talloc_zero(mem_ctx, struct messaging_dgm_context);
	if (ctx == NULL) {
		goto fail_nomem;
	}
	ctx->pid = pid.pid;
	ctx->recv_cb = recv_cb;
	ctx->recv_cb_private_data = recv_cb_private_data;

	ctx->cache_dir = talloc_strdup(ctx, cache_dir);
	if (ctx->cache_dir == NULL) {
		goto fail_nomem;
	}
	socket_dir = talloc_asprintf(ctx, "%s/msg", cache_dir);
	if (socket_dir == NULL) {
		goto fail_nomem;
	}

	socket_address = (struct sockaddr_un) { .sun_family = AF_UNIX };
	sockname_len = snprintf(socket_address.sun_path,
				sizeof(socket_address.sun_path),
				"%s/%u", socket_dir, (unsigned)pid.pid);
	if (sockname_len >= sizeof(socket_address.sun_path)) {
		TALLOC_FREE(ctx);
		return ENAMETOOLONG;
	}

	ret = messaging_dgm_lockfile_create(ctx, cache_dir, dir_owner, pid.pid,
					    &ctx->lockfile_fd, pid.unique_id);
	if (ret != 0) {
		DEBUG(1, ("%s: messaging_dgm_create_lockfile failed: %s\n",
			  __func__, strerror(ret)));
		TALLOC_FREE(ctx);
		return ret;
	}

	ctx->msg_callbacks = poll_funcs_init_tevent(ctx);
	if (ctx->msg_callbacks == NULL) {
		goto fail_nomem;
	}

	ctx->tevent_handle = poll_funcs_tevent_register(
		ctx, ctx->msg_callbacks, ev);
	if (ctx->tevent_handle == NULL) {
		goto fail_nomem;
	}

	ok = directory_create_or_exist_strict(socket_dir, dir_owner, 0700);
	if (!ok) {
		DEBUG(1, ("Could not create socket directory\n"));
		TALLOC_FREE(ctx);
		return EACCES;
	}
	TALLOC_FREE(socket_dir);

	unlink(socket_address.sun_path);

	generate_random_buffer((uint8_t *)&cookie, sizeof(cookie));

	ret = unix_msg_init(&socket_address, ctx->msg_callbacks, 1024, cookie,
			    messaging_dgm_recv, ctx, &ctx->dgm_ctx);
	if (ret != 0) {
		DEBUG(1, ("unix_msg_init failed: %s\n", strerror(ret)));
		TALLOC_FREE(ctx);
		return ret;
	}
	talloc_set_destructor(ctx, messaging_dgm_context_destructor);

	ctx->have_dgm_context = &have_dgm_context;

	*pctx = ctx;
	return 0;

fail_nomem:
	TALLOC_FREE(ctx);
	return ENOMEM;
}

static int messaging_dgm_context_destructor(struct messaging_dgm_context *c)
{
	/*
	 * First delete the socket to avoid races. The lockfile is the
	 * indicator that we're still around.
	 */
	unix_msg_free(c->dgm_ctx);

	if (getpid() == c->pid) {
		(void)messaging_dgm_lockfile_remove(c, c->cache_dir, c->pid);
	}
	close(c->lockfile_fd);

	if (c->have_dgm_context != NULL) {
		*c->have_dgm_context = false;
	}

	return 0;
}

int messaging_dgm_send(struct messaging_dgm_context *ctx, pid_t pid,
		       const struct iovec *iov, int iovlen)
{
	struct sockaddr_un dst;
	ssize_t dst_pathlen;
	int ret;

	dst = (struct sockaddr_un) { .sun_family = AF_UNIX };

	dst_pathlen = snprintf(dst.sun_path, sizeof(dst.sun_path),
			       "%s/msg/%u", ctx->cache_dir, (unsigned)pid);
	if (dst_pathlen >= sizeof(dst.sun_path)) {
		return ENAMETOOLONG;
	}

	DEBUG(10, ("%s: Sending message to %u\n", __func__, (unsigned)pid));

	ret = unix_msg_send(ctx->dgm_ctx, &dst, iov, iovlen);

	return ret;
}

static void messaging_dgm_recv(struct unix_msg_ctx *ctx,
			       uint8_t *msg, size_t msg_len,
			       void *private_data)
{
	struct messaging_dgm_context *dgm_ctx = talloc_get_type_abort(
		private_data, struct messaging_dgm_context);

	dgm_ctx->recv_cb(msg, msg_len, dgm_ctx->recv_cb_private_data);
}

int messaging_dgm_cleanup(struct messaging_dgm_context *ctx, pid_t pid)
{
	char *lockfile_name, *socket_name;
	int fd, ret;
	struct flock lck = {};

	lockfile_name = talloc_asprintf(talloc_tos(), "%s/lck/%u",
					ctx->cache_dir, (unsigned)pid);
	if (lockfile_name == NULL) {
		return ENOMEM;
	}
	socket_name = talloc_asprintf(lockfile_name, "%s/msg/%u",
				      ctx->cache_dir, (unsigned)pid);
	if (socket_name == NULL) {
		TALLOC_FREE(lockfile_name);
		return ENOMEM;
	}

	fd = open(lockfile_name, O_NONBLOCK|O_WRONLY, 0);
	if (fd == -1) {
		ret = errno;
		DEBUG(10, ("%s: open(%s) failed: %s\n", __func__,
			   lockfile_name, strerror(ret)));
		TALLOC_FREE(lockfile_name);
		return ret;
	}

	lck.l_type = F_WRLCK;
	lck.l_whence = SEEK_SET;
	lck.l_start = 0;
	lck.l_len = 0;

	ret = fcntl(fd, F_SETLK, &lck);
	if (ret != 0) {
		ret = errno;
		DEBUG(10, ("%s: Could not get lock: %s\n", __func__,
			   strerror(ret)));
		TALLOC_FREE(lockfile_name);
		close(fd);
		return ret;
	}

	(void)unlink(socket_name);
	(void)unlink(lockfile_name);
	(void)close(fd);

	TALLOC_FREE(lockfile_name);
	return 0;
}

int messaging_dgm_wipe(struct messaging_dgm_context *ctx)
{
	char *msgdir_name;
	DIR *msgdir;
	struct dirent *dp;
	pid_t our_pid = getpid();
	int ret;

	/*
	 * We scan the socket directory and not the lock directory. Otherwise
	 * we would race against messaging_dgm_lockfile_create's open(O_CREAT)
	 * and fcntl(SETLK).
	 */

	msgdir_name = talloc_asprintf(talloc_tos(), "%s/msg", ctx->cache_dir);
	if (msgdir_name == NULL) {
		return ENOMEM;
	}

	msgdir = opendir(msgdir_name);
	if (msgdir == NULL) {
		ret = errno;
		TALLOC_FREE(msgdir_name);
		return ret;
	}
	TALLOC_FREE(msgdir_name);

	while ((dp = readdir(msgdir)) != NULL) {
		unsigned long pid;

		pid = strtoul(dp->d_name, NULL, 10);
		if (pid == 0) {
			/*
			 * . and .. and other malformed entries
			 */
			continue;
		}
		if (pid == our_pid) {
			/*
			 * fcntl(F_GETLK) will succeed for ourselves, we hold
			 * that lock ourselves.
			 */
			continue;
		}

		ret = messaging_dgm_cleanup(ctx, pid);
		DEBUG(10, ("messaging_dgm_cleanup(%lu) returned %s\n",
			   pid, ret ? strerror(ret) : "ok"));
	}
	closedir(msgdir);

	return 0;
}

void *messaging_dgm_register_tevent_context(TALLOC_CTX *mem_ctx,
					    struct messaging_dgm_context *ctx,
					    struct tevent_context *ev)
{
	return poll_funcs_tevent_register(mem_ctx, ctx->msg_callbacks, ev);
}