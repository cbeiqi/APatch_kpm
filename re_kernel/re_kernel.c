/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2024 bmax121. All Rights Reserved.
 */

/*   SPDX-License-Identifier: GPL-3.0-only   */
/*
 * Copyright (C) 2024 Nep-Timeline. All Rights Reserved.
 * Copyright (C) 2024 lzghzr. All Rights Reserved.
 */

#include <compiler.h>
#include <hook.h>
#include <kpmodule.h>
#include <kputils.h>
#include <taskext.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <asm/atomic.h>

#ifdef CONFIG_DEBUG
#include <uapi/linux/limits.h>
#endif /* CONFIG_DEBUG */

#include "re_kernel.h"
#include "re_utils.h"

KPM_NAME("re_kernel");
KPM_VERSION(RK_VERSION);
KPM_LICENSE("GPL v3");
KPM_AUTHOR("Nep-Timeline, lzghzr");
KPM_DESCRIPTION("Re:Kernel, support 4.4, 4.9, 4.14, 4.19, 5.4, 5.10, 5.15");

#define NETLINK_REKERNEL_MAX 26
#define NETLINK_REKERNEL_MIN 22
#define USER_PORT 100
#define PACKET_SIZE 128
#define MIN_USERAPP_UID 10000
#define MAX_SYSTEM_UID 2000

enum report_type {
  BINDER,
  SIGNAL,
#ifdef CONFIG_NETWORK
  NETWORK,
#endif /* CONFIG_NETWORK */
};
enum binder_type {
  REPLY,
  TRANSACTION,
  OVERFLOW,
};
static const char* binder_type[] = {
    "reply",
    "transaction",
    "free_buffer_full",
};

#define IZERO (1UL << 0x10)
#define UZERO (1UL << 0x20)

// cgroup_freezing, cgroupv1_freeze
static bool (*cgroup_freezing)(struct task_struct* task);
// send_netlink_message
struct sk_buff* kfunc_def(__alloc_skb)(unsigned int size, gfp_t gfp_mask, int flags, int node);
struct nlmsghdr* kfunc_def(__nlmsg_put)(struct sk_buff* skb, u32 portid, u32 seq, int type, int len, int flags);
void kfunc_def(kfree_skb)(struct sk_buff* skb);
int kfunc_def(netlink_unicast)(struct sock* ssk, struct sk_buff* skb, u32 portid, int nonblock);
// start_rekernel_server
static struct net kvar_def(init_net);
struct sock* kfunc_def(__netlink_kernel_create)(struct net* net, int unit, struct module* module, struct netlink_kernel_cfg* cfg);
void kfunc_def(netlink_kernel_release)(struct sock* sk);
// prco
struct proc_dir_entry* kfunc_def(proc_mkdir)(const char* name, struct proc_dir_entry* parent);
struct proc_dir_entry* kfunc_def(proc_create_data)(const char* name, umode_t mode, struct proc_dir_entry* parent, const struct file_operations* proc_fops, void* data);
void kfunc_def(proc_remove)(struct proc_dir_entry* de);

ssize_t kfunc_def(seq_read)(struct file* file, char __user* buf, size_t size, loff_t* ppos);
loff_t kfunc_def(seq_lseek)(struct file* file, loff_t offset, int whence);
void kfunc_def(seq_printf)(struct seq_file* m, const char* f, ...);
int kfunc_def(single_open)(struct file* file, int (*show)(struct seq_file*, void*), void* data);
int kfunc_def(single_release)(struct inode* inode, struct file* file);
// hook binder_proc_transaction
static int (*binder_proc_transaction)(struct binder_transaction* t, struct binder_proc* proc, struct binder_thread* thread);
// free the outdated transaction and buffer
static void (*binder_transaction_buffer_release)(struct binder_proc* proc, struct binder_thread* thread, struct binder_buffer* buffer, binder_size_t off_end_offset, bool is_failure);
static void (*binder_transaction_buffer_release_v4)(struct binder_proc* proc, struct binder_buffer* buffer, binder_size_t failed_at, bool is_failure);
static void (*binder_transaction_buffer_release_v3)(struct binder_proc* proc, struct binder_buffer* buffer, binder_size_t* failed_at);
static void(*binder_alloc_free_buf)(struct binder_alloc* alloc, struct binder_buffer* buffer);
void kfunc_def(kfree)(const void* objp);
static struct binder_stats kvar_def(binder_stats);
// hook do_send_sig_info
static int (*do_send_sig_info)(int sig, struct siginfo* info, struct task_struct* p, enum pid_type type);

#ifdef CONFIG_NETWORK
// netfilter
kuid_t kfunc_def(sock_i_uid)(struct sock* sk);
// hook tcp_rcv
static int (*tcp_v4_rcv)(struct sk_buff* skb);
static int (*tcp_v6_rcv)(struct sk_buff* skb);
#endif /* CONFIG_NETWORK */

// _raw_spin_lock && _raw_spin_unlock
void kfunc_def(_raw_spin_lock)(raw_spinlock_t* lock);
void kfunc_def(_raw_spin_unlock)(raw_spinlock_t* lock);
// trace
int kfunc_def(tracepoint_probe_register)(struct tracepoint* tp, void* probe, void* data);
int kfunc_def(tracepoint_probe_unregister)(struct tracepoint* tp, void* probe, void* data);
// trace_binder_transaction
struct tracepoint kvar_def(__tracepoint_binder_transaction);
#ifdef CONFIG_DEBUG_CMDLINE
int kfunc_def(get_cmdline)(struct task_struct* task, char* buffer, int buflen);
#endif /* CONFIG_DEBUG_CMDLINE */

// 最好初始化一个大于 0xffffffff 的值, 否则编译器优化后, 全局变量可能出错
static uint64_t task_struct_jobctl_offset = UZERO, task_struct_pid_offset = UZERO, task_struct_group_leader_offset = UZERO,
binder_proc_alloc_offset = UZERO, binder_proc_context_offset = UZERO, binder_proc_inner_lock_offset = UZERO, binder_proc_outer_lock_offset = UZERO, binder_proc_is_frozen = UZERO,
binder_alloc_pid_offset = UZERO, binder_alloc_buffer_size_offset = UZERO, binder_alloc_free_async_space_offset = UZERO, binder_alloc_vma_offset = UZERO,
// 实际上会被编译器优化为 bool
binder_transaction_buffer_release_ver5 = UZERO, binder_transaction_buffer_release_ver4 = UZERO;

static struct sock* rekernel_netlink;
static unsigned long rekernel_netlink_unit = UZERO, trace = UZERO;
static struct proc_dir_entry* rekernel_dir, * rekernel_unit_entry;

// pid
static inline pid_t task_pid(struct task_struct* task) {
  pid_t pid = *(pid_t*)((uintptr_t)task + task_struct_pid_offset);
  return pid;
}
// tgid
static inline pid_t task_tgid(struct task_struct* task) {
  pid_t tgid = *(pid_t*)((uintptr_t)task + task_struct_pid_offset + 0x4);
  return tgid;
}

// binder_node_lock
static inline void binder_node_lock(struct binder_node* node) {
  spin_lock(&node->lock);
}
// binder_node_unlock
static inline void binder_node_unlock(struct binder_node* node) {
  spin_unlock(&node->lock);
}
// binder_inner_proc_lock
static inline void binder_inner_proc_lock(struct binder_proc* proc) {
  spinlock_t* inner_lock = (spinlock_t*)((uintptr_t)proc + binder_proc_inner_lock_offset);
  spin_lock(inner_lock);
}
// binder_inner_proc_unlock
static inline void binder_inner_proc_unlock(struct binder_proc* proc) {
  spinlock_t* inner_lock = (spinlock_t*)((uintptr_t)proc + binder_proc_inner_lock_offset);
  spin_unlock(inner_lock);
}

// binder_is_frozen
static inline bool binder_is_frozen(struct binder_proc* proc) {
  bool is_frozen = false;
  if (binder_proc_is_frozen != UZERO) {
    is_frozen = *(bool*)((uintptr_t)proc + binder_proc_is_frozen);
  }
  return is_frozen;
}

// cgroupv2_freeze
static inline bool jobctl_frozen(struct task_struct* task) {
  unsigned long jobctl = *(unsigned long*)((uintptr_t)task + task_struct_jobctl_offset);
  return ((jobctl & JOBCTL_TRAP_FREEZE) != 0);
}
// 判断线程是否进入 frozen 状态
static inline bool frozen_task_group(struct task_struct* task) {
  return (jobctl_frozen(task) || cgroup_freezing(task));
}

// 创建 netlink 服务
static __noinline void netlink_rcv_msg(struct sk_buff* skb) { }

static int rekernel_unit_show(struct seq_file* m, void* v) {
  kfunc(seq_printf)(m, "%d\n", rekernel_netlink_unit);
  return 0;
}
static __noinline int rekernel_unit_open(struct inode* inode, struct file* file) {
  return single_open(file, rekernel_unit_show, NULL);
}
static __noinline ssize_t seq_read(struct file* file, char __user* buf, size_t size, loff_t* ppos) {
  return kfunc(seq_read)(file, buf, size, ppos);
}
static __noinline loff_t seq_lseek(struct file* file, loff_t offset, int whence) {
  return kfunc(seq_lseek)(file, offset, whence);
}
static __noinline int single_release(struct inode* inode, struct file* file) {
  return kfunc(single_release)(inode, file);
}
static const struct file_operations rekernel_unit_fops = {
    .open = rekernel_unit_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
    .owner = THIS_MODULE,
};

static int start_rekernel_server(void) {
  if (rekernel_netlink_unit != UZERO) {
    return 0;
  }

  struct netlink_kernel_cfg rekernel_cfg = {
      .input = netlink_rcv_msg,
  };

  for (rekernel_netlink_unit = NETLINK_REKERNEL_MAX; rekernel_netlink_unit >= NETLINK_REKERNEL_MIN; rekernel_netlink_unit--) {
    rekernel_netlink = netlink_kernel_create(kvar(init_net), rekernel_netlink_unit, &rekernel_cfg);
    if (rekernel_netlink != NULL) {
      break;
    }
  }
  if (rekernel_netlink == NULL) {
    printk("Failed to create Re:Kernel server!\n");
    return -1;
  }
  printk("Created Re:Kernel server! NETLINK UNIT: %d\n", rekernel_netlink_unit);

  rekernel_dir = proc_mkdir("rekernel", NULL);
  if (!rekernel_dir) {
    printk("create /proc/rekernel failed!\n");
  } else {
    char buff[32];
    sprintf(buff, "%d", rekernel_netlink_unit);
    rekernel_unit_entry = proc_create(buff, 0644, rekernel_dir, &rekernel_unit_fops);
    if (!rekernel_unit_entry) {
      printk("create rekernel unit failed!\n");
    }
  }

  return 0;
}
// 发送 netlink 消息
static int send_netlink_message(char* msg, uint16_t len) {
  struct sk_buff* skbuffer;
  struct nlmsghdr* nlhdr;

  skbuffer = nlmsg_new(len, GFP_ATOMIC);
  if (!skbuffer) {
    printk("netlink alloc failure.\n");
    return -1;
  }

  nlhdr = nlmsg_put(skbuffer, 0, 0, rekernel_netlink_unit, len, 0);
  if (!nlhdr) {
    printk("nlmsg_put failaure.\n");
    nlmsg_free(skbuffer);
    return -1;
  }

  memcpy(nlmsg_data(nlhdr), msg, len);
  return netlink_unicast(rekernel_netlink, skbuffer, USER_PORT, MSG_DONTWAIT);
}

static void rekernel_report(int reporttype, int type, pid_t src_pid, struct task_struct* src, pid_t dst_pid, struct task_struct* dst, bool oneway) {
  if (start_rekernel_server() != 0)
    return;

#ifdef CONFIG_NETWORK
  if (reporttype == NETWORK) {
    char binder_kmsg[PACKET_SIZE];
    snprintf(binder_kmsg, sizeof(binder_kmsg), "type=Network,target=%d;", dst_pid);
#ifdef CONFIG_DEBUG
    printk("re_kernel: %s\n", binder_kmsg);
#endif /* CONFIG_DEBUG */
    send_netlink_message(binder_kmsg, strlen(binder_kmsg));
    return;
  }
#endif /* CONFIG_NETWORK */

  if (!frozen_task_group(dst))
    return;

  if (task_uid(src).val == task_uid(dst).val)
    return;

  char binder_kmsg[PACKET_SIZE];
  switch (reporttype) {
  case BINDER:
    snprintf(binder_kmsg, sizeof(binder_kmsg), "type=Binder,bindertype=%s,oneway=%d,from_pid=%d,from=%d,target_pid=%d,target=%d;", binder_type[type], oneway, src_pid, task_uid(src).val, dst_pid, task_uid(dst).val);
    break;
  case SIGNAL:
    snprintf(binder_kmsg, sizeof(binder_kmsg), "type=Signal,signal=%d,killer_pid=%d,killer=%d,dst_pid=%d,dst=%d;", type, src_pid, task_uid(src).val, dst_pid, task_uid(dst).val);
    break;
  default:
    return;
  }
#ifdef CONFIG_DEBUG
  printk("re_kernel: %s\n", binder_kmsg);
  printk("re_kernel: src_comm=%s,dst_comm=%s\n", get_task_comm(src), get_task_comm(dst));
#endif /* CONFIG_DEBUG */
#ifdef CONFIG_DEBUG_CMDLINE
  char src_cmdline[PATH_MAX], dst_cmdline[PATH_MAX];
  memset(&src_cmdline, 0, PATH_MAX);
  memset(&dst_cmdline, 0, PATH_MAX);
  int res = 0;
  res = get_cmdline(src, src_cmdline, PATH_MAX - 1);
  src_cmdline[res] = '\0';
  res = get_cmdline(dst, dst_cmdline, PATH_MAX - 1);
  dst_cmdline[res] = '\0';
  printk("re_kernel: src_cmdline=%s,dst_cmdline=%s\n", src_cmdline, dst_cmdline);
#endif /* CONFIG_DEBUG_CMDLINE */
  send_netlink_message(binder_kmsg, strlen(binder_kmsg));
}

static void binder_reply_handler(pid_t src_pid, struct task_struct* src, pid_t dst_pid, struct task_struct* dst, bool oneway) {
  if (unlikely(!dst))
    return;
  if (task_uid(dst).val > MAX_SYSTEM_UID || src_pid == dst_pid)
    return;

  // oneway=0
  rekernel_report(BINDER, REPLY, src_pid, src, dst_pid, dst, oneway);
}

static void binder_trans_handler(pid_t src_pid, struct task_struct* src, pid_t dst_pid, struct task_struct* dst, bool oneway) {
  if (unlikely(!dst))
    return;
  if ((task_uid(dst).val <= MIN_USERAPP_UID) || src_pid == dst_pid)
    return;

  rekernel_report(BINDER, TRANSACTION, src_pid, src, dst_pid, dst, oneway);
}

static void binder_overflow_handler(pid_t src_pid, struct task_struct* src, pid_t dst_pid, struct task_struct* dst, bool oneway) {
  if (unlikely(!dst))
    return;

  // oneway=1
  rekernel_report(BINDER, OVERFLOW, src_pid, src, dst_pid, dst, oneway);
}

static void rekernel_binder_transaction(void* data, bool reply, struct binder_transaction* t, struct binder_node* target_node) {
  if (!t->to_proc)
    return;
  // binder 冻结时不再传递消息
  if (binder_is_frozen(t->to_proc))
    return;

  if (reply) {
    binder_reply_handler(task_pid(current), current, t->to_proc->pid, t->to_proc->tsk, false);
  } else if (t->from) {
    if (t->from->proc) {
      binder_trans_handler(t->from->proc->pid, t->from->proc->tsk, t->to_proc->pid, t->to_proc->tsk, false);
    }
  } else { // oneway=1
    // binder_trans_handler(task_pid(current), current, t->to_proc->pid, t->to_proc->tsk, true);

    struct binder_alloc* target_alloc = (struct binder_alloc*)((uintptr_t)t->to_proc + binder_proc_alloc_offset);
    size_t free_async_space = *(size_t*)((uintptr_t)target_alloc + binder_alloc_free_async_space_offset);
    size_t buffer_size = *(size_t*)((uintptr_t)target_alloc + binder_alloc_buffer_size_offset);
    if (free_async_space < (buffer_size / 10 + 0x300)) {
      binder_overflow_handler(task_pid(current), current, t->to_proc->pid, t->to_proc->tsk, true);
    }
  }
}

static bool binder_can_update_transaction(struct binder_transaction* t1, struct binder_transaction* t2) {
  if ((t1->flags & t2->flags & TF_ONE_WAY) != TF_ONE_WAY || !t1->to_proc || !t2->to_proc)
    return false;
  if (t1->to_proc->tsk == t2->to_proc->tsk
    && t1->code == t2->code
    && t1->flags == t2->flags
    // && t1->buffer->pid == t2->buffer->pid // 4.19 以下无此数据
    && t1->buffer->target_node->ptr == t2->buffer->target_node->ptr
    && t1->buffer->target_node->cookie == t2->buffer->target_node->cookie)
    return true;
  return false;
}

static struct binder_transaction* binder_find_outdated_transaction_ilocked(struct binder_transaction* t, struct list_head* target_list) {
  struct binder_work* w;

  list_for_each_entry(w, target_list, entry) {
    struct binder_transaction* t_queued;

    if (w->type != BINDER_WORK_TRANSACTION)
      continue;
    t_queued = container_of(w, struct binder_transaction, work);
    if (binder_can_update_transaction(t_queued, t))
      return t_queued;
  }
  return NULL;
}

static inline void binder_release_entire_buffer(struct binder_proc* proc, struct binder_thread* thread, struct binder_buffer* buffer, bool is_failure) {
  if (binder_transaction_buffer_release_ver5 == IZERO) {
    binder_size_t off_end_offset = ALIGN(buffer->data_size, sizeof(void*));
    off_end_offset += buffer->offsets_size;

    binder_transaction_buffer_release(proc, thread, buffer, off_end_offset, is_failure);
  } else if (binder_transaction_buffer_release_ver4 == IZERO) {
    binder_transaction_buffer_release_v4(proc, buffer, 0, is_failure);
  } else {
    binder_transaction_buffer_release_v3(proc, buffer, NULL);
  }
}

static inline void binder_stats_deleted(enum binder_stat_types type) {
  atomic_inc(&kvar(binder_stats)->obj_deleted[type]);
}

static void binder_proc_transaction_before(hook_fargs3_t* args, void* udata) {
  struct binder_transaction* t = (struct binder_transaction*)args->arg0;
  struct binder_proc* proc = (struct binder_proc*)args->arg1;
  // binder 冻结时不再清理过时消息
  if (binder_is_frozen(proc))
    return;

  // 兼容不支持 trace 的内核
  if (trace == UZERO) {
    rekernel_binder_transaction(NULL, false, t, NULL);
  }

  if ((t->flags & TF_ONE_WAY)
    && t->to_proc
    && t->to_proc->tsk
    && frozen_task_group(t->to_proc->tsk)) {
    struct binder_node* node = t->buffer->target_node;
    if (!node)
      return;

    struct binder_alloc* target_alloc = (struct binder_alloc*)((uintptr_t)t->to_proc + binder_proc_alloc_offset);

    binder_node_lock(node);
    binder_inner_proc_lock(proc);

    struct binder_transaction* t_outdated = binder_find_outdated_transaction_ilocked(t, &node->async_todo);
    if (t_outdated) {
      list_del_init(&t_outdated->work.entry);
    }

    binder_inner_proc_unlock(proc);
    binder_node_unlock(node);

    if (t_outdated) {
#ifdef CONFIG_DEBUG
      printk("re_kernel: free_outdated pid=%d,uid=%d,data_size=%d\n", t->to_proc->pid, task_uid(t->to_proc->tsk).val, t_outdated->buffer->data_size);
#endif /* CONFIG_DEBUG */
      struct binder_buffer* buffer = t_outdated->buffer;

      t_outdated->buffer = NULL;
      buffer->transaction = NULL;
      binder_release_entire_buffer(t->to_proc, NULL, buffer, false);
      binder_alloc_free_buf(target_alloc, buffer);
      kfree(t_outdated);
      binder_stats_deleted(BINDER_STAT_TRANSACTION);
    }
  }
}

static void do_send_sig_info_before(hook_fargs4_t* args, void* udata) {
  int sig = (int)args->arg0;
  struct task_struct* dst = (struct task_struct*)args->arg2;

  if (sig == SIGKILL || sig == SIGTERM || sig == SIGABRT || sig == SIGQUIT) {
    rekernel_report(SIGNAL, sig, task_tgid(current), current, task_tgid(dst), dst, NULL);
  }
}

#ifdef CONFIG_NETWORK
static inline bool sk_fullsock(const struct sock* sk) {
  return (1 << sk->sk_state) & ~(TCPF_TIME_WAIT | TCPF_NEW_SYN_RECV);
}

static void tcp_rcv_before(hook_fargs1_t* args, void* udata) {
  struct sk_buff* skb = (struct sk_buff*)args->arg0;
  struct sock* sk = skb->sk;;
  if (sk == NULL || !sk_fullsock(sk))
    return;

  uid_t uid = sock_i_uid(sk).val;
  if (uid < MIN_USERAPP_UID)
    return;

  rekernel_report(NETWORK, NULL, NULL, NULL, uid, NULL, true);
}
#endif /* CONFIG_NETWORK */

static long calculate_offsets() {
  // 获取 binder_transaction_buffer_release 版本, 以参数数量做判断
  uint32_t* binder_transaction_buffer_release_src = (uint32_t*)binder_transaction_buffer_release;
  for (u32 i = 0; i < 0x10; i++) {
#ifdef CONFIG_DEBUG
    printk("re_kernel: binder_transaction_buffer_release %x %llx\n", i, binder_transaction_buffer_release_src[i]);
#endif /* CONFIG_DEBUG */
    if (binder_transaction_buffer_release_src[i] == ARM64_RET) {
      break;
    } else if ((binder_transaction_buffer_release_src[i] & MASK_STR_Rn_SP_Rt_4) == INST_STR_Rn_SP_Rt_4) {
      binder_transaction_buffer_release_ver5 = IZERO;
    } else if ((binder_transaction_buffer_release_src[i] & MASK_MOV_Rm_4_Rn_WZR) == INST_MOV_Rm_4_Rn_WZR) {
      binder_transaction_buffer_release_ver5 = IZERO;
    } else if ((binder_transaction_buffer_release_src[i] & MASK_STR_Rn_SP_Rt_3) == INST_STR_Rn_SP_Rt_3) {
      binder_transaction_buffer_release_ver4 = IZERO;
    } else if ((binder_transaction_buffer_release_src[i] & MASK_MOV_Rm_3_Rn_WZR) == INST_MOV_Rm_3_Rn_WZR) {
      binder_transaction_buffer_release_ver4 = IZERO;
    }
  }
  // 获取 binder_proc->is_frozen, 没有就是不支持
  uint32_t* binder_proc_transaction_src = (uint32_t*)binder_proc_transaction;
  for (u32 i = 0; i < 0x100; i++) {
#ifdef CONFIG_DEBUG
    printk("re_kernel: binder_proc_transaction %x %llx\n", i, binder_proc_transaction_src[i]);
#endif /* CONFIG_DEBUG */
    if (binder_proc_transaction_src[i] == ARM64_RET) {
      break;
    } else if ((binder_proc_transaction_src[i] & MASK_MOVZ_imm16_0x7212) == INST_MOVZ_imm16_0x7212) {
      for (u32 j = 0; j < 0x5; j++) {
        if ((binder_proc_transaction_src[i - j] & MASK_LDRB) == INST_LDRB) {
          uint64_t imm12 = bits32(binder_proc_transaction_src[i - j], 21, 10);
          binder_proc_is_frozen = sign64_extend((imm12), 16u);
          break;
        }
      }
      break;
    }
  }
  // 获取 task_struct->jobctl
  void (*task_clear_jobctl_trapping)(struct task_struct* t);
  lookup_name(task_clear_jobctl_trapping);

  uint32_t* task_clear_jobctl_trapping_src = (uint32_t*)task_clear_jobctl_trapping;
  for (u32 i = 0; i < 0x10; i++) {
#ifdef CONFIG_DEBUG
    printk("re_kernel: task_clear_jobctl_trapping %x %llx\n", i, task_clear_jobctl_trapping_src[i]);
#endif /* CONFIG_DEBUG */
    if (task_clear_jobctl_trapping_src[i] == ARM64_RET) {
      break;
    } else if ((task_clear_jobctl_trapping_src[i] & MASK_LDR_64_Rn_X0) == INST_LDR_64_Rn_X0) {
      uint64_t imm12 = bits32(task_clear_jobctl_trapping_src[i], 21, 10);
      task_struct_jobctl_offset = sign64_extend((imm12 << 0b11u), 16u);
      break;
    }
  }
  if (task_struct_jobctl_offset == UZERO) {
    return -11;
  }
  // 获取 binder_proc->context, binder_proc->inner_lock, binder_proc->outer_lock
  void (*binder_transaction)(struct binder_proc* proc, struct binder_thread* thread, struct binder_transaction_data* tr, int reply, binder_size_t extra_buffers_size);
  lookup_name(binder_transaction);

  bool mov_x22_x0 = false;
  uint32_t* binder_transaction_src = (uint32_t*)binder_transaction;
  for (u32 i = 0; i < 0x20; i++) {
#ifdef CONFIG_DEBUG
    printk("re_kernel: binder_transaction %x %llx\n", i, binder_transaction_src[i]);
#endif /* CONFIG_DEBUG */
    if (binder_transaction_src[i] == ARM64_RET) {
      break;
    } else if (binder_transaction_src[i] == 0xAA0003F6u) { // mov x22, x0
      mov_x22_x0 = true;
    } else if (((binder_transaction_src[i] & MASK_LDR_64_Rn_X0) == INST_LDR_64_Rn_X0 && (binder_transaction_src[i] & MASK_LDR_64_Rn_X0_Rt_X0) != INST_LDR_64_Rn_X0_Rt_X0)
      || (mov_x22_x0 && (binder_transaction_src[i] & MASK_LDR_64_X22) == INST_LDR_64_X22)) {
      uint64_t imm12 = bits32(binder_transaction_src[i], 21, 10);
      binder_proc_context_offset = sign64_extend((imm12 << 0b11u), 16u);
      binder_proc_inner_lock_offset = binder_proc_context_offset + 0x8;
      binder_proc_outer_lock_offset = binder_proc_context_offset + 0xC;
      break;
    }
  }
  if (binder_proc_context_offset == UZERO || binder_proc_inner_lock_offset == UZERO || binder_proc_outer_lock_offset == UZERO) {
    return -11;
  }
  // 获取 binder_proc->alloc
  void (*binder_free_proc)(struct binder_proc* proc);
  binder_free_proc = (typeof(binder_free_proc))kallsyms_lookup_name("binder_free_proc");
  if (!binder_free_proc) {
    binder_free_proc = (typeof(binder_free_proc))kallsyms_lookup_name("binder_proc_dec_tmpref");
  }

  uint32_t* binder_free_proc_src = (uint32_t*)binder_free_proc;
  for (u32 i = 0x10; i < 0x100; i++) {
#ifdef CONFIG_DEBUG
    printk("re_kernel: binder_free_proc %x %llx\n", i, binder_free_proc_src[i]);
#endif /* CONFIG_DEBUG */
    if (binder_free_proc_src[i] == ARM64_MOV_x29_SP) {
      break;
    } else if ((binder_free_proc_src[i] & MASK_ADD_64_Rn_X19_Rd_X0) == INST_ADD_64_Rn_X19_Rd_X0 && (binder_free_proc_src[i + 1] & MASK_BL) == INST_BL) {
      uint32_t sh = bit(binder_free_proc_src[i], 22);
      uint64_t imm12 = imm12 = bits32(binder_free_proc_src[i], 21, 10);
      if (sh) {
        binder_proc_alloc_offset = sign64_extend((imm12 << 12u), 16u);
      } else {
        binder_proc_alloc_offset = sign64_extend((imm12), 16u);
      }
      break;
    }
  }
  if (binder_proc_alloc_offset == UZERO) {
    return -11;
  }
  // 获取 binder_alloc->pid, task_struct->pid, task_struct->group_leader
  void (*binder_alloc_init)(struct task_struct* t);
  lookup_name(binder_alloc_init);

  uint32_t* binder_alloc_init_src = (uint32_t*)binder_alloc_init;
  for (u32 i = 0; i < 0x20; i++) {
#ifdef CONFIG_DEBUG
    printk("re_kernel: binder_alloc_init %x %llx\n", i, binder_alloc_init_src[i]);
#endif /* CONFIG_DEBUG */
    if (binder_alloc_init_src[i] == ARM64_RET) {
      break;
    } else if ((binder_alloc_init_src[i] & MASK_STR_32_x0) == INST_STR_32_x0) {
      uint64_t imm12 = bits32(binder_alloc_init_src[i], 21, 10);
      binder_alloc_pid_offset = sign64_extend((imm12 << 0b10u), 16u);        // 0x74
      binder_alloc_buffer_size_offset = binder_alloc_pid_offset - 0xC;       // 0x68
      binder_alloc_free_async_space_offset = binder_alloc_pid_offset - 0x1C; // 0x58
      binder_alloc_vma_offset = binder_alloc_pid_offset - 0x54;              // 0x20
      break;
    } else if ((binder_alloc_init_src[i] & MASK_LDR_32_) == INST_LDR_32_) {
      uint64_t imm12 = bits32(binder_alloc_init_src[i], 21, 10);
      task_struct_pid_offset = sign64_extend((imm12 << 0b10u), 16u);
    } else if ((binder_alloc_init_src[i] & MASK_LDR_64_) == INST_LDR_64_) {
      uint64_t imm12 = bits32(binder_alloc_init_src[i], 21, 10);
      task_struct_group_leader_offset = sign64_extend((imm12 << 0b11u), 16u);
    }
  }
  if (binder_alloc_pid_offset == UZERO || task_struct_pid_offset == UZERO || task_struct_group_leader_offset == UZERO) {
    return -11;
  }

  return 0;
}

static long inline_hook_init(const char* args, const char* event, void* __user reserved) {
  lookup_name(cgroup_freezing);

  kfunc_lookup_name(__alloc_skb);
  kfunc_lookup_name(__nlmsg_put);
  kfunc_lookup_name(kfree_skb);
  kfunc_lookup_name(netlink_unicast);

  kvar_lookup_name(init_net);
  kfunc_lookup_name(__netlink_kernel_create);
  kfunc_lookup_name(netlink_kernel_release);

  kfunc_lookup_name(proc_mkdir);
  kfunc_lookup_name(proc_create_data);
  kfunc_lookup_name(proc_remove);

  kfunc_lookup_name(seq_read);
  kfunc_lookup_name(seq_lseek);
  kfunc_lookup_name(seq_printf);
  kfunc_lookup_name(single_open);
  kfunc_lookup_name(single_release);

  kfunc_lookup_name(tracepoint_probe_register);
  kfunc_lookup_name(tracepoint_probe_unregister);

  kfunc_lookup_name(_raw_spin_lock);
  kfunc_lookup_name(_raw_spin_unlock);
  kvar_lookup_name(__tracepoint_binder_transaction);

  lookup_name(binder_transaction_buffer_release);
  binder_transaction_buffer_release_v4 = (typeof(binder_transaction_buffer_release_v4))binder_transaction_buffer_release;
  binder_transaction_buffer_release_v3 = (typeof(binder_transaction_buffer_release_v3))binder_transaction_buffer_release;
  lookup_name(binder_alloc_free_buf);
  kfunc_lookup_name(kfree);
  kvar_lookup_name(binder_stats);

  lookup_name(binder_proc_transaction);
  lookup_name(do_send_sig_info);

#ifdef CONFIG_NETWORK
  kfunc_lookup_name(sock_i_uid);

  lookup_name(tcp_v4_rcv);
  lookup_name(tcp_v6_rcv);
#endif /* CONFIG_NETWORK */
#ifdef CONFIG_DEBUG_CMDLINE
  kfunc_lookup_name(get_cmdline);
#endif /* CONFIG_DEBUG_CMDLINE */

  int rc = 0;
  rc = calculate_offsets();
  if (rc < 0)
    return rc;

  rc = tracepoint_probe_register(kvar(__tracepoint_binder_transaction), rekernel_binder_transaction, NULL);
  if (rc == 0) {
    trace = IZERO;
  }

  hook_func(binder_proc_transaction, 3, binder_proc_transaction_before, NULL, NULL);
  hook_func(do_send_sig_info, 4, do_send_sig_info_before, NULL, NULL);

#ifdef CONFIG_NETWORK
  hook_func(tcp_v4_rcv, 1, tcp_rcv_before, NULL, NULL);
  hook_func(tcp_v6_rcv, 1, tcp_rcv_before, NULL, NULL);
#endif /* CONFIG_NETWORK */

  return 0;
}

static long inline_hook_control0(const char* ctl_args, char* __user out_msg, int outlen) {
  printk("\
re_kernel: task_struct_jobctl_offset=0x%llx\n\
re_kernel: task_struct_pid_offset=0x%llx\n\
re_kernel: task_struct_group_leader_offset=0x%llx\n",
task_struct_jobctl_offset,
task_struct_pid_offset,
task_struct_group_leader_offset);
  printk("\
re_kernel: binder_alloc_pid_offset=0x%llx\n\
re_kernel: binder_alloc_buffer_size_offset=0x%llx\n\
re_kernel: binder_alloc_free_async_space_offset=0x%llx\n\
re_kernel: binder_alloc_vma_offset=0x%llx\n",
binder_alloc_pid_offset,
binder_alloc_buffer_size_offset,
binder_alloc_free_async_space_offset,
binder_alloc_vma_offset);
  printk("\
re_kernel: binder_proc_alloc_offset=0x%llx\n\
re_kernel: binder_proc_context_offset=0x%llx\n\
re_kernel: binder_proc_inner_lock_offset=0x%llx\n\
re_kernel: binder_proc_outer_lock_offset=0x%llx\n\
re_kernel: binder_proc_is_frozen=0x%llx\n",
binder_proc_alloc_offset,
binder_proc_context_offset,
binder_proc_inner_lock_offset,
binder_proc_outer_lock_offset,
binder_proc_is_frozen);
  printk("\
re_kernel: binder_transaction_buffer_release_ver5=0x%llx\n\
re_kernel: binder_transaction_buffer_release_ver4=0x%llx\n",
binder_transaction_buffer_release_ver5,
binder_transaction_buffer_release_ver4);
  char msg[64];
  snprintf(msg, sizeof(msg), "_(._.)_");
  compat_copy_to_user(out_msg, msg, sizeof(msg));
  return 0;
}

static long inline_hook_exit(void* __user reserved) {
  if (rekernel_netlink) {
    netlink_kernel_release(rekernel_netlink);
  }
  if (rekernel_dir) {
    proc_remove(rekernel_dir);
  }

  tracepoint_probe_unregister(kvar(__tracepoint_binder_transaction), rekernel_binder_transaction, NULL);

  unhook_func(binder_proc_transaction);
  unhook_func(do_send_sig_info);

#ifdef CONFIG_NETWORK
  unhook_func(tcp_v4_rcv);
  unhook_func(tcp_v6_rcv);
#endif /* CONFIG_NETWORK */

  return 0;
}

KPM_INIT(inline_hook_init);
KPM_CTL0(inline_hook_control0);
KPM_EXIT(inline_hook_exit);
