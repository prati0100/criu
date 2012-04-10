#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <linux/limits.h>

#include <sys/types.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "crtools.h"

#include "files.h"
#include "image.h"
#include "list.h"
#include "util.h"
#include "util-net.h"
#include "lock.h"
#include "sockets.h"

static struct fdinfo_list_entry *fdinfo_list;
static int nr_fdinfo_list;

#define FDESC_HASH_SIZE	64
static struct list_head file_descs[FDESC_HASH_SIZE];

int prepare_shared_fdinfo(void)
{
	int i;

	fdinfo_list = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, 0, 0);
	if (fdinfo_list == MAP_FAILED) {
		pr_perror("Can't map fdinfo_list");
		return -1;
	}

	for (i = 0; i < FDESC_HASH_SIZE; i++)
		INIT_LIST_HEAD(&file_descs[i]);

	return 0;
}

void file_desc_add(struct file_desc *d, int type, u32 id,
		struct file_desc_ops *ops)
{
	d->type = type;
	d->id = id;
	d->ops = ops;
	INIT_LIST_HEAD(&d->fd_info_head);

	list_add_tail(&d->hash, &file_descs[id % FDESC_HASH_SIZE]);
}

struct file_desc *find_file_desc_raw(int type, u32 id)
{
	struct file_desc *d;
	struct list_head *chain;

	chain = &file_descs[id % FDESC_HASH_SIZE];
	list_for_each_entry(d, chain, hash)
		if (d->type == type && d->id == id)
			return d;

	return NULL;
}

static inline struct file_desc *find_file_desc(struct fdinfo_entry *fe)
{
	return find_file_desc_raw(fe->type, fe->id);
}

struct fdinfo_list_entry *file_master(struct file_desc *d)
{
	BUG_ON(list_empty(&d->fd_info_head));
	return list_first_entry(&d->fd_info_head,
			struct fdinfo_list_entry, list);
}

struct reg_file_info {
	struct reg_file_entry rfe;
	char *path;
	struct file_desc d;
};

void show_saved_files(void)
{
	int i;
	struct file_desc *fd;

	pr_info("File descs:\n");
	for (i = 0; i < FDESC_HASH_SIZE; i++)
		list_for_each_entry(fd, &file_descs[i], hash) {
			struct fdinfo_list_entry *le;

			pr_info(" `- type %d ID %x\n", fd->type, fd->id);
			list_for_each_entry(le, &fd->fd_info_head, list)
				pr_info("   `- FD %d pid %d\n", le->fd, le->pid);
		}
}

int restore_fown(int fd, fown_t *fown)
{
	struct f_owner_ex owner;
	uid_t uids[3];
	pid_t pid = getpid();

	if (fown->signum) {
		if (fcntl(fd, F_SETSIG, fown->signum)) {
			pr_perror("%d: Can't set signal", pid);
			return -1;
		}
	}

	/* May be untouched */
	if (!fown->pid)
		return 0;

	if (getresuid(&uids[0], &uids[1], &uids[2])) {
		pr_perror("%d: Can't get current UIDs", pid);
		return -1;
	}

	if (setresuid(fown->uid, fown->euid, uids[2])) {
		pr_perror("%d: Can't set UIDs", pid);
		return -1;
	}

	owner.type = fown->pid_type;
	owner.pid = fown->pid;

	if (fcntl(fd, F_SETOWN_EX, &owner)) {
		pr_perror("%d: Can't setup %d file owner pid",
			  pid, fd);
		return -1;
	}

	if (setresuid(uids[0], uids[1], uids[2])) {
		pr_perror("%d: Can't revert UIDs back", pid);
		return -1;
	}

	return 0;
}

static int open_fe_fd(struct file_desc *d);

static struct file_desc_ops reg_desc_ops = {
	.open = open_fe_fd,
};

int collect_reg_files(void)
{
	struct reg_file_info *rfi = NULL;
	int fd, ret = -1;

	fd = open_image_ro(CR_FD_REG_FILES);
	if (fd < 0)
		return -1;

	while (1) {
		int len;

		rfi = xmalloc(sizeof(*rfi));
		ret = -1;
		if (rfi == NULL)
			break;

		rfi->path = NULL;
		ret = read_img_eof(fd, &rfi->rfe);
		if (ret <= 0)
			break;

		len = rfi->rfe.len;
		rfi->path = xmalloc(len + 1);
		ret = -1;
		if (rfi->path == NULL)
			break;

		ret = read_img_buf(fd, rfi->path, len);
		if (ret < 0)
			break;

		rfi->path[len] = '\0';

		pr_info("Collected [%s] ID %x\n", rfi->path, rfi->rfe.id);
		file_desc_add(&rfi->d, FDINFO_REG, rfi->rfe.id,
				&reg_desc_ops);
	}

	if (rfi) {
		xfree(rfi->path);
		xfree(rfi);
	}

	close(fd);
	return ret;
}

static int collect_fd(int pid, struct fdinfo_entry *e)
{
	int i;
	struct fdinfo_list_entry *l, *le = &fdinfo_list[nr_fdinfo_list];
	struct file_desc *fdesc;

	pr_info("Collect fdinfo pid=%d fd=%d id=%16x\n",
		pid, e->fd, e->id);

	nr_fdinfo_list++;
	if ((nr_fdinfo_list) * sizeof(struct fdinfo_list_entry) >= 4096) {
		pr_err("OOM storing fdinfo_list_entries\n");
		return -1;
	}

	le->pid = pid;
	le->fd = e->fd;
	le->flags = e->flags;
	futex_init(&le->real_pid);

	fdesc = find_file_desc(e);
	if (fdesc == NULL) {
		pr_err("No file for fd %d id %d\n", e->fd, e->id);
		return -1;
	}

	list_for_each_entry(l, &fdesc->fd_info_head, list)
		if (l->pid > le->pid)
			break;

	list_add_tail(&le->list, &l->list);
	return 0;
}

int prepare_fd_pid(int pid)
{
	int fdinfo_fd, ret = 0;
	u32 type = 0;

	fdinfo_fd = open_image_ro(CR_FD_FDINFO, pid);
	if (fdinfo_fd < 0) {
		if (errno == ENOENT)
			return 0;
		else
			return -1;
	}

	while (1) {
		struct fdinfo_entry e;

		ret = read_img_eof(fdinfo_fd, &e);
		if (ret <= 0)
			break;

		ret = collect_fd(pid, &e);
		if (ret < 0)
			break;
	}

	close(fdinfo_fd);
	return ret;
}

static int open_fe_fd(struct file_desc *d)
{
	struct reg_file_info *rfi;
	int tmp;

	rfi = container_of(d, struct reg_file_info, d);

	tmp = open(rfi->path, rfi->rfe.flags);
	if (tmp < 0) {
		pr_perror("Can't open file %s", rfi->path);
		return -1;
	}

	lseek(tmp, rfi->rfe.pos, SEEK_SET);

	if (restore_fown(tmp, &rfi->rfe.fown))
		return -1;

	return tmp;
}
int open_reg_by_id(u32 id)
{
	struct file_desc *fd;

	fd = find_file_desc_raw(FDINFO_REG, id);
	if (fd == NULL) {
		pr_perror("Can't find regfile for %x\n", id);
		return -1;
	}

	return open_fe_fd(fd);
}

#define SETFL_MASK (O_APPEND | O_NONBLOCK | O_NDELAY | O_DIRECT | O_NOATIME)
int set_fd_flags(int fd, int flags)
{
	int old;

	old = fcntl(fd, F_GETFL, 0);
	if (old < 0)
		return old;

	flags = (SETFL_MASK & flags) | (old & ~SETFL_MASK);

	return fcntl(fd, F_SETFL, flags);
}

static void transport_name_gen(struct sockaddr_un *addr, int *len,
		int pid, int fd)
{
	addr->sun_family = AF_UNIX;
	snprintf(addr->sun_path, UNIX_PATH_MAX, "x/crtools-fd-%d-%d", pid, fd);
	*len = SUN_LEN(addr);
	*addr->sun_path = '\0';
}

static int should_open_transport(struct fdinfo_entry *fe, struct file_desc *fd)
{
	if (fd->ops->want_transport)
		return fd->ops->want_transport(fe, fd);
	else
		return 0;
}

static int open_transport_fd(int pid, struct fdinfo_entry *fe, struct file_desc *d)
{
	struct fdinfo_list_entry *fle;
	struct sockaddr_un saddr;
	int sock;
	int ret, sun_len;

	fle = file_master(d);

	if (fle->pid == pid) {
		if (fle->fd == fe->fd) {
			/* file master */
			if (!should_open_transport(fe, d))
				return 0;
		} else
			return 0;
	}

	transport_name_gen(&saddr, &sun_len, getpid(), fe->fd);

	pr_info("\t%d: Create transport fd for %d\n", pid, fe->fd);

	list_for_each_entry(fle, &d->fd_info_head, list)
		if ((fle->pid == pid) && (fle->fd == fe->fd))
			break;

	BUG_ON(&d->fd_info_head == &fle->list);

	sock = socket(PF_UNIX, SOCK_DGRAM, 0);
	if (sock < 0) {
		pr_perror("Can't create socket");
		return -1;
	}
	ret = bind(sock, &saddr, sun_len);
	if (ret < 0) {
		pr_perror("Can't bind unix socket %s", saddr.sun_path + 1);
		return -1;
	}

	ret = reopen_fd_as(fe->fd, sock);
	if (ret < 0)
		return -1;

	pr_info("Wake up fdinfo pid=%d fd=%d\n", fle->pid, fle->fd);
	futex_set_and_wake(&fle->real_pid, getpid());

	return 0;
}

int send_fd_to_peer(int fd, struct fdinfo_list_entry *fle, int tsk)
{
	struct sockaddr_un saddr;
	int len;

	pr_info("Wait fdinfo pid=%d fd=%d\n", fle->pid, fle->fd);
	futex_wait_while(&fle->real_pid, 0);
	transport_name_gen(&saddr, &len,
			futex_get(&fle->real_pid), fle->fd);
	pr_info("Send fd %d to %s\n", fd, saddr.sun_path + 1);
	return send_fd(tsk, &saddr, len, fd);
}

static int open_fd(int pid, struct fdinfo_entry *fe,
		struct file_desc *d, int *fdinfo_fd)
{
	int tmp;
	int serv, sock;
	struct fdinfo_list_entry *fle;

	fle = file_master(d);
	if ((fle->pid != pid) || (fe->fd != fle->fd))
		return 0;

	tmp = d->ops->open(d);
	if (tmp < 0)
		return -1;

	if (reopen_fd_as(fe->fd, tmp))
		return -1;

	fcntl(tmp, F_SETFD, fe->flags);

	sock = socket(PF_UNIX, SOCK_DGRAM, 0);
	if (sock < 0) {
		pr_perror("Can't create socket");
		return -1;
	}

	pr_info("\t%d: Create fd for %d\n", pid, fe->fd);

	list_for_each_entry(fle, &d->fd_info_head, list) {
		if (pid == fle->pid) {
			pr_info("\t\tGoing to dup %d into %d\n", fe->fd, fle->fd);
			if (fe->fd == fle->fd)
				continue;

			if (move_img_fd(&sock, fle->fd))
				return -1;
			if (move_img_fd(fdinfo_fd, fle->fd))
				return -1;

			if (dup2(fe->fd, fle->fd) != fle->fd) {
				pr_perror("Can't dup local fd %d -> %d",
						fe->fd, fle->fd);
				return -1;
			}

			fcntl(fle->fd, F_SETFD, fle->flags);

			continue;
		}

		if (send_fd_to_peer(fe->fd, fle, sock)) {
			pr_perror("Can't send file descriptor");
			return -1;
		}
	}

	close(sock);
out:
	return 0;
}

static int receive_fd(int pid, struct fdinfo_entry *fe, struct file_desc *d)
{
	int tmp;
	struct fdinfo_list_entry *fle;

	fle = file_master(d);

	if (fle->pid == pid)
		return 0;

	pr_info("\t%d: Receive fd for %d\n", pid, fe->fd);

	tmp = recv_fd(fe->fd);
	if (tmp < 0) {
		pr_err("Can't get fd %d\n", tmp);
		return -1;
	}
	close(fe->fd);

	if (reopen_fd_as(fe->fd, tmp) < 0)
		return -1;

	fcntl(tmp, F_SETFD, fe->flags);
	return 0;
}

static int open_fdinfo(int pid, struct fdinfo_entry *fe, int *fdinfo_fd, int state)
{
	u32 mag;
	int ret = 0;
	struct file_desc *fdesc;

	fdesc = find_file_desc(fe);
	if (move_img_fd(fdinfo_fd, fe->fd))
		return -1;

	pr_info("\t%d: Got fd for %d (state -> %d)\n", pid, fe->fd, state);

	switch (state) {
	case FD_STATE_PREP:
		ret = open_transport_fd(pid, fe, fdesc);
		break;
	case FD_STATE_CREATE:
		ret = open_fd(pid, fe, fdesc, fdinfo_fd);
		break;
	case FD_STATE_RECV:
		ret = receive_fd(pid, fe, fdesc);
		break;
	}

	return ret;
}

int prepare_fds(int pid)
{
	u32 type = 0, ret;
	int fdinfo_fd;
	int state;

	struct fdinfo_entry fe;
	int nr = 0;

	pr_info("%d: Opening fdinfo-s\n", pid);

	fdinfo_fd = open_image_ro(CR_FD_FDINFO, pid);
	if (fdinfo_fd < 0) {
		pr_perror("%d: Can't open pipes img", pid);
		return -1;
	}

	for (state = 0; state < FD_STATE_MAX; state++) {
		lseek(fdinfo_fd, MAGIC_OFFSET, SEEK_SET);

		while (1) {
			ret = read_img_eof(fdinfo_fd, &fe);
			if (ret <= 0)
				break;

			ret = open_fdinfo(pid, &fe, &fdinfo_fd, state);
			if (ret)
				break;
		}

		if (ret)
			break;
	}

	close(fdinfo_fd);

	if (!ret)
		ret = run_unix_connections();

	return ret;
}

int prepare_fs(int pid)
{
	int ifd, cwd;
	struct fs_entry fe;

	ifd = open_image_ro(CR_FD_FS, pid);
	if (ifd < 0)
		return -1;

	if (read_img(ifd, &fe) < 0)
		return -1;

	cwd = open_reg_by_id(fe.cwd_id);
	if (cwd < 0)
		return -1;

	if (fchdir(cwd) < 0) {
		pr_perror("Can't change root");
		return -1;
	}

	close(cwd);
	close(ifd);

	/*
	 * FIXME: restore task's root. Don't want to do it now, since
	 * it's not yet clean how we're going to resolve tasks' paths
	 * relative to the dumper/restorer and all this logic is likely
	 * to be hidden in a couple of calls (open_fe_fd is one od them)
	 * but for chroot there's no fchroot call, we have to chroot
	 * by path thus exposing this (yet unclean) logic here.
	 */

	return 0;
}

int get_filemap_fd(int pid, struct vma_entry *vma_entry)
{
	return open_reg_by_id(vma_entry->shmid);
}
