/* Syscall entry points. */
#ifndef _KERNEL_KE_SYSCALLS_H
#define _KERNEL_KE_SYSCALLS_H

#include <kernel/uapi/types.h>

/* Every syscall entry point, for the dispatcher in ke/syscall.c and for the
 * few implementations that call a sibling (shmat maps through sys_mmap). */

int64_t sys_accept(uint64_t a1, uint64_t a2, uint64_t a3);
int64_t sys_accept4(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
int64_t sys_access(uint64_t pathname, uint64_t mode);
int64_t sys_alarm(uint64_t seconds);
int64_t sys_arch_prctl(uint64_t code, uint64_t addr);
int64_t sys_bind(uint64_t a1, uint64_t a2, uint64_t a3);
int64_t sys_brk(uint64_t new_brk);
int64_t sys_chdir(uint64_t pathname);
int64_t sys_chmod(uint64_t pathname, uint64_t mode);
int64_t sys_chown(uint64_t pathname, uint64_t owner, uint64_t group);
int64_t sys_chroot(uint64_t pathname);
int64_t sys_clock_getres(uint64_t clk_id, uint64_t res_ptr);
int64_t sys_clock_gettime(uint64_t clk_id, uint64_t tp_ptr);
int64_t sys_clone(uint64_t flags, uint64_t child_stack,
		  uint64_t parent_tidptr, uint64_t child_tidptr,
		  uint64_t tls);
int64_t sys_close(uint64_t fd);
int64_t sys_connect(uint64_t a1, uint64_t a2, uint64_t a3);
int64_t sys_debug_dump(void);
int64_t sys_dhcp_control(uint64_t a1);
int64_t sys_dns_resolve(uint64_t a1, uint64_t a2);
int64_t sys_dns_resolve_reverse(uint64_t a1, uint64_t a2, uint64_t a3);
int64_t sys_dup(uint64_t oldfd);
int64_t sys_dup2(uint64_t oldfd, uint64_t newfd);
int64_t sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags);
int64_t sys_epoll_create(void);
int64_t sys_epoll_create1(uint64_t a1);
int64_t sys_epoll_ctl(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
__attribute__((noinline)) int64_t
sys_epoll_wait(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
int64_t sys_execve(uint64_t pathname, uint64_t argv_ptr,
		   uint64_t envp_ptr);
__attribute__((noreturn)) void sys_exit(uint64_t status);
void sys_exit_group(uint64_t status);
int64_t sys_faccessat(uint64_t dirfd, uint64_t pathname, uint64_t mode,
		      uint64_t flags);
int64_t sys_fchmod(uint64_t fd, uint64_t mode);
int64_t sys_fchown(uint64_t fd, uint64_t owner, uint64_t group);
int64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg);
int64_t sys_fgetxattr(uint64_t fd, uint64_t u_name, uint64_t u_val,
		      uint64_t size);
int64_t sys_flistxattr(uint64_t fd, uint64_t u_list, uint64_t size);
int64_t sys_fork(void);
int64_t sys_fremovexattr(uint64_t fd, uint64_t u_name);
int64_t sys_fsetxattr(uint64_t fd, uint64_t u_name, uint64_t u_val,
		      uint64_t size, uint64_t flags);
int64_t sys_fstat(uint64_t fd, uint64_t stat_buf);
int64_t sys_fstatat(uint64_t dirfd, uint64_t pathname, uint64_t stat_buf,
		    uint64_t flags);
int64_t sys_fstatfs(uint64_t fd, uint64_t u_buf);
int64_t sys_fsync(uint64_t fd);
int64_t sys_ftruncate(uint64_t fd, uint64_t length);
int64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val,
		  uint64_t timeout, uint64_t uaddr2, uint64_t val3);
int64_t sys_get_robust_list(uint64_t pid, uint64_t head_ptr,
			    uint64_t len_ptr);
int64_t sys_getcwd(uint64_t buf, uint64_t size);
int64_t sys_getdents(uint64_t fd, uint64_t dirp, uint64_t count);
int64_t sys_getdents64(uint64_t fd, uint64_t dirp, uint64_t count);
int64_t sys_getegid(void);
int64_t sys_geteuid(void);
int64_t sys_getgid(void);
int64_t sys_getgroups(uint64_t size, uint64_t list);
int64_t sys_gethostname(uint64_t name, uint64_t len);
int64_t sys_getitimer(uint64_t which, uint64_t curr_value_ptr);
int64_t sys_getpeername(uint64_t a1, uint64_t a2, uint64_t a3);
int64_t sys_getpgid(uint64_t pid);
int64_t sys_getpgrp(void);
int64_t sys_getpid(void);
int64_t sys_getppid(void);
int64_t sys_getprocinfo(uint64_t buf_ptr, uint64_t max_count);
int64_t sys_getprocmaps(uint64_t pid, uint64_t info_ptr,
			uint64_t buf_ptr, uint64_t max_count);
int64_t sys_getrandom(uint64_t a1, uint64_t a2, uint64_t a3);
int64_t sys_getresgid(uint64_t rgid, uint64_t egid, uint64_t sgid);
int64_t sys_getresuid(uint64_t ruid, uint64_t euid, uint64_t suid);
int64_t sys_getrusage(uint64_t who, uint64_t uptr);
int64_t sys_getsid(uint64_t pid);
int64_t sys_getsockname(uint64_t a1, uint64_t a2, uint64_t a3);
int64_t sys_getsockopt(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
int64_t sys_gettid(void);
int64_t sys_gettimeofday(uint64_t tv, uint64_t tz);
int64_t sys_getuid(void);
int64_t sys_getxattr(uint64_t u_path, uint64_t u_name, uint64_t u_val,
		     uint64_t size, uint64_t nofollow);
int64_t sys_ioctl(uint64_t fd, uint64_t req, uint64_t argp);
int64_t sys_kill(uint64_t pid, uint64_t sig);
int64_t sys_klogctl(uint64_t type, uint64_t bufp, uint64_t len);
int64_t sys_link(uint64_t oldpath, uint64_t newpath);
int64_t sys_listen(uint64_t a1, uint64_t a2);
int64_t sys_listxattr(uint64_t u_path, uint64_t u_list, uint64_t size,
		      uint64_t nofollow);
int64_t sys_lseek(uint64_t fd, int64_t offset, uint64_t whence);
int64_t sys_lstat(uint64_t pathname, uint64_t stat_buf);
int64_t sys_madvise(uint64_t addr, uint64_t length, uint64_t advice);
int64_t sys_mincore(uint64_t addr, uint64_t length, uint64_t vec);
int64_t sys_memstats(uint64_t a1, uint64_t a2);
int64_t sys_mkdir(uint64_t pathname, uint64_t mode);
int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
		 uint64_t flags, uint64_t fd, uint64_t offset);
int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot);
int64_t sys_munmap(uint64_t addr, uint64_t length);
int64_t sys_nanosleep(uint64_t req_ptr, uint64_t rem_ptr);
int64_t sys_mremap(uint64_t old_addr, uint64_t old_size, uint64_t new_size,
		   uint64_t flags, uint64_t new_addr);
int64_t sys_eventfd2(uint64_t initval, uint64_t flags);
int64_t sys_pread64(uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset);
int64_t sys_pwrite64(uint64_t fd, uint64_t buf, uint64_t count,
		     uint64_t offset);
int64_t sys_fallocate(uint64_t fd, uint64_t mode, uint64_t offset,
		      uint64_t len);
int64_t sys_timerfd_create(uint64_t clockid, uint64_t flags);
int64_t sys_timerfd_settime(uint64_t fd, uint64_t flags, uint64_t new_ptr,
			    uint64_t old_ptr);
int64_t sys_timerfd_gettime(uint64_t fd, uint64_t cur_ptr);
int64_t sys_memfd_create(uint64_t name_ptr, uint64_t flags);
int64_t sys_signalfd4(uint64_t fd, uint64_t mask_ptr, uint64_t sizemask,
		      uint64_t flags);
int64_t sys_clock_nanosleep(uint64_t clk_id, uint64_t flags, uint64_t req_ptr,
			    uint64_t rem_ptr);
int64_t sys_net_getinfo(uint64_t a1, uint64_t a2, uint64_t a3);
int64_t sys_open(uint64_t pathname, uint64_t flags, uint64_t mode);
int64_t sys_openat(uint64_t dirfd, uint64_t pathname, uint64_t flags,
		   uint64_t mode);
int64_t sys_pause(void);
int64_t sys_pipe(uint64_t pipefd_ptr);
__attribute__((noinline)) int64_t
sys_poll(uint64_t a1, uint64_t a2, uint64_t a3);
__attribute__((noinline)) int64_t
sys_ppoll(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
__attribute__((noinline)) int64_t
sys_pselect6(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
	     uint64_t a5, uint64_t a6);
int64_t sys_ptrace(uint64_t request, uint64_t pid, uint64_t addr,
		   uint64_t data);
int64_t sys_raw_recv(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
int64_t sys_raw_send(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count);
int64_t sys_readlink(uint64_t pathname, uint64_t buf, uint64_t bufsiz);
int64_t sys_readv(uint64_t fd, uint64_t iovp, uint64_t iovcnt);
int64_t sys_reboot(uint64_t magic1, uint64_t magic2, uint64_t cmd,
		   uint64_t arg);
int64_t sys_recv(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
int64_t sys_recvfrom(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
int64_t sys_recvmsg(uint64_t a1, uint64_t a2, uint64_t a3);
int64_t sys_removexattr(uint64_t u_path, uint64_t u_name,
			uint64_t nofollow);
int64_t sys_rename(uint64_t oldpath, uint64_t newpath);
int64_t sys_rmdir(uint64_t pathname);
int64_t sys_rt_sigaction(uint64_t sig, uint64_t act_ptr,
			 uint64_t oldact_ptr, uint64_t sigsetsize);
int64_t sys_rt_sigpending(uint64_t set_ptr, uint64_t sigsetsize);
int64_t sys_rt_sigprocmask(uint64_t how, uint64_t set_ptr,
			   uint64_t oldset_ptr, uint64_t sigsetsize);
int64_t sys_rt_sigqueueinfo(uint64_t pid, uint64_t sig,
			    uint64_t info_ptr);
int64_t sys_rt_sigreturn(void);
int64_t sys_rt_sigsuspend(uint64_t mask_ptr, uint64_t sigsetsize);
int64_t sys_rt_sigtimedwait(uint64_t set_ptr, uint64_t info_ptr,
			    uint64_t timeout_ptr, uint64_t sigsetsize);
int64_t sys_sched_get_priority_max(uint64_t policy);
int64_t sys_sched_get_priority_min(uint64_t policy);
int64_t sys_sched_getaffinity(uint64_t pid, uint64_t cpusetsize,
			      uint64_t mask_ptr);
int64_t sys_sched_getparam(uint64_t pid, uint64_t param_ptr);
int64_t sys_sched_getscheduler(uint64_t pid);
int64_t sys_sched_rr_get_interval(uint64_t pid, uint64_t tp_ptr);
int64_t sys_sched_setaffinity(uint64_t pid, uint64_t cpusetsize,
			      uint64_t mask_ptr);
int64_t sys_sched_setparam(uint64_t pid, uint64_t param_ptr);
int64_t sys_sched_setscheduler(uint64_t pid, uint64_t policy,
			       uint64_t param_ptr);
__attribute__((noinline)) int64_t
sys_select(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
	   uint64_t a5);
int64_t sys_send(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
int64_t sys_sendfile(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
int64_t sys_sendmsg(uint64_t a1, uint64_t a2, uint64_t a3);
int64_t sys_sendto(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
int64_t sys_set_dns_server(uint64_t a1, uint64_t a2);
int64_t sys_set_robust_list(uint64_t head, uint64_t len);
int64_t sys_set_tid_address(uint64_t tidptr);
int64_t sys_setegid(uint64_t gid);
int64_t sys_seteuid(uint64_t uid);
int64_t sys_setgid(uint64_t gid);
int64_t sys_setgroups(uint64_t size, uint64_t list);
int64_t sys_sethostname(uint64_t a1, uint64_t a2);
int64_t sys_setitimer(uint64_t which, uint64_t new_value_ptr,
		      uint64_t old_value_ptr);
int64_t sys_setpgid(uint64_t pid, uint64_t pgid);
int64_t sys_setresgid(uint64_t rgid, uint64_t egid, uint64_t sgid);
int64_t sys_setresuid(uint64_t ruid, uint64_t euid, uint64_t suid);
int64_t sys_setsid(void);
int64_t sys_setsockopt(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
int64_t sys_settimeofday(uint64_t tv_ptr, uint64_t tz);
int64_t sys_setuid(uint64_t uid);
int64_t sys_setxattr(uint64_t u_path, uint64_t u_name, uint64_t u_val,
		     uint64_t size, uint64_t flags);
int64_t sys_shmat(uint64_t shmid, uint64_t shmaddr, uint64_t shmflg);
int64_t sys_shmctl(uint64_t shmid, uint64_t cmd, uint64_t buf);
int64_t sys_shmdt(uint64_t shmaddr);
int64_t sys_shmget(uint64_t key, uint64_t size, uint64_t shmflg);
int64_t sys_shutdown(uint64_t a1, uint64_t a2);
int64_t sys_sigaltstack(uint64_t ss_ptr, uint64_t old_ss_ptr);
int64_t sys_signalfd(uint64_t fd, uint64_t mask_ptr, uint64_t flags);
int64_t sys_socket(uint64_t a1, uint64_t a2, uint64_t a3);
int64_t sys_socketpair(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
int64_t sys_stat(uint64_t pathname, uint64_t stat_buf);
int64_t sys_statfs(uint64_t u_path, uint64_t u_buf);
int64_t sys_symlink(uint64_t target, uint64_t linkpath);
int64_t sys_sync(void);
int64_t sys_sysinfo(uint64_t info_ptr);
int64_t sys_tcgetpgrp(uint64_t fd);
int64_t sys_tcsetpgrp(uint64_t fd, uint64_t pgrp);
int64_t sys_tgkill(uint64_t tgid, uint64_t tid, uint64_t sig);
int64_t sys_time(uint64_t tloc);
int64_t sys_timer_create(uint64_t clockid, uint64_t sevp_ptr,
			 uint64_t timerid_ptr);
int64_t sys_timer_delete(uint64_t timerid);
int64_t sys_timer_getoverrun(uint64_t timerid);
int64_t sys_timer_gettime(uint64_t timerid, uint64_t curr_value_ptr);
int64_t sys_timer_settime(uint64_t timerid, uint64_t flags,
			  uint64_t new_value_ptr, uint64_t old_value_ptr);
int64_t sys_tkill(uint64_t tid, uint64_t sig);
int64_t sys_umask(uint64_t mask);
int64_t sys_uname(uint64_t buf);
int64_t sys_unlink(uint64_t pathname);
int64_t sys_unlinkat(uint64_t dirfd, uint64_t pathname, uint64_t flags);
int64_t sys_utimensat(uint64_t dirfd, uint64_t pathname, uint64_t times,
		      uint64_t flags);
int64_t sys_vfork(void);
int64_t sys_waitpid(int64_t pid, uint64_t status_ptr, uint64_t options,
		    uint64_t rusage_ptr);
int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count);
int64_t sys_writev(uint64_t fd, uint64_t iovp, uint64_t iovcnt);
int64_t sys_yield(void);

#endif /* _KERNEL_KE_SYSCALLS_H */
