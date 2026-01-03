#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/mutex.h>
#include <linux/ioctl.h>

#define DEVICE_NAME "myQueue"
#define Q_CAP 10  // ظرفیت هر صف (جمع کل 3×10 = 30)

// ✅ اضافه کردن IOCTL commands
#define QUEUE_IOC_MAGIC 'q'
#define QUEUE_SET_MODE _IOW(QUEUE_IOC_MAGIC, 1, int)
#define QUEUE_GET_MODE _IOR(QUEUE_IOC_MAGIC, 2, int)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS Student");
MODULE_DESCRIPTION("Ready Queue Kernel Module");

// ساختار task با u64 برای زمان
struct task {
    int priority;           // 0 = highest, 1, 2
    int task_id;
    int exec_time;
    u64 arrival_time_ns;    // زمان ورود به نانوثانیه
};

// ساختار task با شماره ورود برای FCFS
struct queued_task {
    struct task task_data;
    unsigned long sequence;  // شماره ورود به سیستم
};

// ساختار صف برای هر اولویت
struct priority_queue {
    struct queued_task tasks[Q_CAP];
    int head;
    int tail;
    int count;
};

// متغیرهای global
static int major_number;
static int operation_mode = 0;  // 0=FCFS, 1=Priority
static struct priority_queue queues[3];  // سه صف برای اولویت‌های 0, 1, 2
static unsigned long global_sequence = 0;  // شماره ورود global برای FCFS
static DEFINE_MUTEX(queue_lock);  // mutex برای همگام‌سازی

module_param(operation_mode, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(operation_mode, "0=FCFS, 1=Priority");

// تابع باز کردن دستگاه
static int dev_open(struct inode *inodep, struct file *filep) {
    printk(KERN_INFO "myQueue: Device opened (mode=%s)\n", 
           operation_mode == 0 ? "FCFS" : "Priority");
    return 0;
}

// تابع بستن دستگاه
static int dev_release(struct inode *inodep, struct file *filep) {
    printk(KERN_INFO "myQueue: Device closed\n");
    return 0;
}

// ✅ تابع IOCTL برای تغییر حالت
static long dev_ioctl(struct file *filep, unsigned int cmd, unsigned long arg) {
    int mode;
    
    switch (cmd) {
        case QUEUE_SET_MODE:
            if (copy_from_user(&mode, (int __user *)arg, sizeof(int))) {
                return -EFAULT;
            }
            
            if (mode != 0 && mode != 1) {
                printk(KERN_ALERT "myQueue: Invalid mode %d\n", mode);
                return -EINVAL;
            }
            
            mutex_lock(&queue_lock);
            operation_mode = mode;
            mutex_unlock(&queue_lock);
            
            printk(KERN_INFO "myQueue: Mode changed to %s\n", 
                   mode == 0 ? "FCFS" : "Priority");
            return 0;
            
        case QUEUE_GET_MODE:
            if (copy_to_user((int __user *)arg, &operation_mode, sizeof(int))) {
                return -EFAULT;
            }
            return 0;
            
        default:
            return -ENOTTY;
    }
}

// تابع نوشتن - اضافه کردن task به صف
static ssize_t dev_write(struct file *filep, const char *buffer, size_t len, loff_t *offset) {
    struct task new_task;
    struct queued_task queued;
    int priority;
    ssize_t result;
    
    if (len != sizeof(struct task)) {
        printk(KERN_ALERT "myQueue: Invalid data size\n");
        return -EINVAL;
    }
    
    // کپی کردن task از user space
    if (copy_from_user(&new_task, buffer, sizeof(struct task))) {
        printk(KERN_ALERT "myQueue: Failed to copy from user\n");
        return -EFAULT;
    }
    
    priority = new_task.priority;
    
    // بررسی Poison Pill (task_id == -1)
    if (new_task.task_id == -1) {
        // Poison pill را به صف 0 اضافه می‌کنیم (اولویت بالا)
        priority = 0;
        printk(KERN_INFO "myQueue: Poison pill received, adding to queue 0\n");
    } else {
        // بررسی اعتبار اولویت برای taskهای عادی
        if (priority < 0 || priority > 2) {
            printk(KERN_ALERT "myQueue: Invalid priority %d\n", priority);
            return -EINVAL;
        }
    }
    
    // قفل کردن برای جلوگیری از race condition
    mutex_lock(&queue_lock);
    
    // بررسی پر بودن صف
    if (queues[priority].count >= Q_CAP) {
        printk(KERN_INFO "myQueue: Queue %d is full\n", priority);
        mutex_unlock(&queue_lock);
        return -EAGAIN;
    }
    
    // آماده‌سازی task با شماره ورود
    queued.task_data = new_task;
    queued.sequence = global_sequence++;  // اختصاص شماره ورود یکتا
    
    // اضافه کردن task به صف
    queues[priority].tasks[queues[priority].tail] = queued;
    queues[priority].tail = (queues[priority].tail + 1) % Q_CAP;
    queues[priority].count++;
    
    if (new_task.task_id == -1) {
        printk(KERN_INFO "myQueue: [%s] Poison pill added to queue %d (seq=%lu, count=%d)\n",
               operation_mode == 0 ? "FCFS" : "PRIORITY",
               priority, queued.sequence, queues[priority].count);
    } else {
        printk(KERN_INFO "myQueue: [%s] Task %d (prio=%d) added to queue %d (seq=%lu, count=%d)\n",
               operation_mode == 0 ? "FCFS" : "PRIORITY",
               new_task.task_id, new_task.priority, priority, 
               queued.sequence, queues[priority].count);
    }
    
    result = sizeof(struct task);
    *offset = 0;
    
    mutex_unlock(&queue_lock);
    
    return result;
}

// تابع خواندن - برداشتن task از صف
static ssize_t dev_read(struct file *filep, char *buffer, size_t len, loff_t *offset) {
    struct queued_task selected_task;
    int i;
    int selected_priority = -1;
    unsigned long earliest_sequence = ULONG_MAX;
    ssize_t result;
    int current_mode;
    
    if (len < sizeof(struct task)) {
        return -EINVAL;
    }
    
    // قفل کردن برای جلوگیری از race condition
    mutex_lock(&queue_lock);
    current_mode = operation_mode;  // ذخیره mode فعلی
    
    if (current_mode == 0) {
        // =====================================
        // حالت FCFS - بر اساس شماره ورود GLOBAL
        // =====================================
        // پیدا کردن قدیمی‌ترین task از بین همه صف‌ها
        for (i = 0; i < 3; i++) {
            if (queues[i].count > 0) {
                unsigned long seq = queues[i].tasks[queues[i].head].sequence;
                if (seq < earliest_sequence) {
                    earliest_sequence = seq;
                    selected_priority = i;
                }
            }
        }
        
        if (selected_priority != -1) {
            printk(KERN_DEBUG "myQueue: [FCFS] Selected queue %d (seq=%lu)\n", 
                   selected_priority, earliest_sequence);
        }
        
    } else {
        // =====================================
        // حالت Priority - از بالاترین اولویت + FCFS داخل صف
        // =====================================
        for (i = 0; i < 3; i++) {
            if (queues[i].count > 0) {
                selected_priority = i;
                // در حالت Priority، اولین task در صف (FCFS داخل هر صف)
                printk(KERN_DEBUG "myQueue: [PRIORITY] Selected queue %d (highest priority with tasks)\n", 
                       selected_priority);
                break;
            }
        }
    }
    
    // اگر هیچ task‌ای نبود
    if (selected_priority == -1) {
        mutex_unlock(&queue_lock);
        return -EAGAIN;
    }
    
    // برداشتن task از صف
    selected_task = queues[selected_priority].tasks[queues[selected_priority].head];
    queues[selected_priority].head = (queues[selected_priority].head + 1) % Q_CAP;
    queues[selected_priority].count--;
    
    if (selected_task.task_data.task_id == -1) {
        printk(KERN_INFO "myQueue: [%s] Poison pill read from queue %d (seq=%lu, count=%d)\n",
               current_mode == 0 ? "FCFS" : "PRIORITY",
               selected_priority, selected_task.sequence, queues[selected_priority].count);
    } else {
        printk(KERN_INFO "myQueue: [%s] Task %d (prio=%d) read from queue %d (seq=%lu, count=%d)\n",
               current_mode == 0 ? "FCFS" : "PRIORITY",
               selected_task.task_data.task_id, selected_task.task_data.priority,
               selected_priority, selected_task.sequence, queues[selected_priority].count);
    }
    
    mutex_unlock(&queue_lock);
    
    // کپی کردن به user space (فقط task_data، نه sequence)
    if (copy_to_user(buffer, &selected_task.task_data, sizeof(struct task))) {
        printk(KERN_ALERT "myQueue: Failed to copy to user\n");
        return -EFAULT;
    }
    
    result = sizeof(struct task);
    *offset = 0;
    
    return result;
}

// ساختار file operations
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = dev_open,
    .read = dev_read,
    .write = dev_write,
    .release = dev_release,
    .unlocked_ioctl = dev_ioctl,  // ✅ اضافه شد
};

// تابع راه‌اندازی ماژول
static int __init simple_init(void) {
    int i;
    
    // مقداردهی اولیه صف‌ها
    for (i = 0; i < 3; i++) {
        queues[i].head = 0;
        queues[i].tail = 0;
        queues[i].count = 0;
    }
    
    global_sequence = 0;
    
    // ثبت character device
    major_number = register_chrdev(0, DEVICE_NAME, &fops);
    
    if (major_number < 0) {
        printk(KERN_ALERT "myQueue: Failed to register a major number\n");
        return major_number;
    }
    
    printk(KERN_INFO "myQueue: Registered with major number %d\n", major_number);
    printk(KERN_INFO "myQueue: Operation mode: %s\n", 
           operation_mode == 0 ? "FCFS (sequence-based)" : "Priority");
    printk(KERN_INFO "myQueue: Queue capacity per priority: %d\n", Q_CAP);
    printk(KERN_INFO "myQueue: Total capacity: %d tasks\n", Q_CAP * 3);
    printk(KERN_INFO "myQueue: Create device with: sudo mknod /dev/myQueue c %d 0\n", 
           major_number);
    
    return 0;
}

// تابع پاکسازی ماژول
static void __exit simple_exit(void) {
    unregister_chrdev(major_number, DEVICE_NAME);
    printk(KERN_INFO "myQueue: Module unloaded (processed %lu tasks total)\n", global_sequence);
}

module_init(simple_init);
module_exit(simple_exit);