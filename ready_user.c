#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sched.h>
#include <sys/ioctl.h>

#define DEVICE_PATH "/dev/myQueue"
#define TOTAL_TASKS 50  // تعداد کل taskها (طبق صورت سوال)
#define Q_CAP 10        // ظرفیت هر صف
#define TOTAL_CAP (Q_CAP * 3)  // کل ظرفیت = 30

// ✅ اضافه کردن IOCTL commands (همان‌ها که در kernel تعریف شده)
#define QUEUE_IOC_MAGIC 'q'
#define QUEUE_SET_MODE _IOW(QUEUE_IOC_MAGIC, 1, int)
#define QUEUE_GET_MODE _IOR(QUEUE_IOC_MAGIC, 2, int)

// ساختار task با u64 برای سازگاری با kernel
struct task {
    int priority;
    int task_id;
    int exec_time;
    unsigned long long arrival_time_ns;  // زمان ورود به نانوثانیه
};

// ساختار برای ذخیره آمار هر task
struct task_stats {
    int task_id;
    int priority;
    int exec_time;
    struct timespec arrival_time;
    struct timespec start_time;
    struct timespec finish_time;
    double wait_time;      // ms
    double turnaround_time; // ms
    int reader_id;
};

// متغیرهای global
sem_t empty, full;
pthread_mutex_t queue_mutex;
pthread_mutex_t stats_mutex;
int fd;
int tasks_produced = 0;
int tasks_consumed = 0;
struct task_stats stats[50];  // آرایه برای 50 task
int stats_count = 0;

// تعداد خوانندگان و هسته‌ها
int num_readers = 1;
int num_cores = 1;
int scheduling_mode = 0;  // ✅ 0=FCFS, 1=Priority

// آرایه برای شمارش task‌های هر اولویت
int priority_count[3] = {0, 0, 0};
pthread_mutex_t priority_mutex;

// محاسبه اختلاف زمان به میلی‌ثانیه
double time_diff_ms(struct timespec *start, struct timespec *end) {
    return (end->tv_sec - start->tv_sec) * 1000.0 + 
           (end->tv_nsec - start->tv_nsec) / 1000000.0;
}

// تبدیل timespec به nanoseconds
unsigned long long timespec_to_ns(struct timespec *ts) {
    return (unsigned long long)ts->tv_sec * 1000000000ULL + ts->tv_nsec;
}

// شبیه‌سازی اجرای CPU-bound بدون sleep
void simulate_execution(int exec_time_ms) {
    struct timespec start, current;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    double elapsed = 0;
    while (elapsed < exec_time_ms) {
        // busy loop - CPU را مشغول نگه می‌دارد
        for (volatile int i = 0; i < 100000; i++);
        
        clock_gettime(CLOCK_MONOTONIC, &current);
        elapsed = time_diff_ms(&start, &current);
    }
}

// ✅ تابع تنظیم حالت scheduling
int set_scheduling_mode(int mode) {
    if (ioctl(fd, QUEUE_SET_MODE, &mode) < 0) {
        perror("Failed to set scheduling mode");
        return -1;
    }
    printf("[Main] Scheduling mode set to: %s\n", 
           mode == 0 ? "FCFS" : "Priority-based");
    return 0;
}

// تابع تولیدکننده
void* writer_thread(void* arg) {
    int core_id = *(int*)arg;
    
    // تنظیم CPU affinity
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    
    printf("[Writer] Running on core %d (Mode: %s)\n", 
           core_id, scheduling_mode == 0 ? "FCFS" : "Priority");
    
    srand(time(NULL) ^ getpid());
    
    for (int i = 0; i < TOTAL_TASKS; i++) {
        struct task new_task;
        struct timespec ts;
        
        // تولید task تصادفی
        new_task.task_id = i;
        new_task.priority = rand() % 3;  // 0, 1, 2
        new_task.exec_time = 14 + rand() % 131;  // 14-144 ms
        
        clock_gettime(CLOCK_MONOTONIC, &ts);
        new_task.arrival_time_ns = timespec_to_ns(&ts);
        
        // شمارش اولویت‌ها
        pthread_mutex_lock(&priority_mutex);
        priority_count[new_task.priority]++;
        pthread_mutex_unlock(&priority_mutex);
        
        // همگام‌سازی
        sem_wait(&empty);
        pthread_mutex_lock(&queue_mutex);
        
        // نوشتن در دستگاه
        ssize_t ret = write(fd, &new_task, sizeof(struct task));
        
        if (ret < 0) {
            if (errno == EAGAIN) {
                printf("[Writer] Queue full, retrying...\n");
                pthread_mutex_unlock(&queue_mutex);
                sem_post(&empty);
                i--;  // تلاش دوباره
                usleep(1000);
                continue;
            } else {
                perror("[Writer] Write failed");
                pthread_mutex_unlock(&queue_mutex);
                sem_post(&empty);
                break;
            }
        }
        
        pthread_mutex_lock(&stats_mutex);
        tasks_produced++;
        pthread_mutex_unlock(&stats_mutex);
        
        printf("[Writer] Task %d produced (Priority=%d, ExecTime=%d ms)\n",
               new_task.task_id, new_task.priority, new_task.exec_time);
        
        pthread_mutex_unlock(&queue_mutex);
        sem_post(&full);
        
        // تاخیر تصادفی بین تولید task‌ها
        int delay = 14 + rand() % 131;
        usleep(delay * 1000);
    }
    
    // ارسال Poison Pills برای پایان دادن به readerها
    printf("[Writer] Sending poison pills to %d readers...\n", num_readers);
    for (int i = 0; i < num_readers; i++) {
        struct task poison = {
            .task_id = -1,
            .priority = 0,
            .exec_time = 0,
            .arrival_time_ns = 0
        };
        
        sem_wait(&empty);
        pthread_mutex_lock(&queue_mutex);
        
        ssize_t ret = write(fd, &poison, sizeof(struct task));
        if (ret < 0) {
            printf("[Writer] Failed to send poison pill %d\n", i);
        }
        
        pthread_mutex_unlock(&queue_mutex);
        sem_post(&full);
    }
    
    printf("[Writer] Finished producing %d tasks\n", tasks_produced);
    return NULL;
}

// تابع خواننده
void* reader_thread(void* arg) {
    int reader_id = *(int*)arg;
    int core_id = (num_cores == 1) ? 0 : (reader_id + 1) % num_cores;
    
    // تنظیم CPU affinity
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    
    printf("[Reader %d] Running on core %d (Mode: %s)\n", 
           reader_id, core_id, scheduling_mode == 0 ? "FCFS" : "Priority");
    
    while (1) {
        sem_wait(&full);
        pthread_mutex_lock(&queue_mutex);
        
        struct task read_task;
        ssize_t ret = read(fd, &read_task, sizeof(struct task));
        
        if (ret < 0) {
            if (errno == EAGAIN) {
                pthread_mutex_unlock(&queue_mutex);
                usleep(1000);
                continue;
            } else {
                perror("[Reader] Read failed");
                pthread_mutex_unlock(&queue_mutex);
                sem_post(&empty);
                break;
            }
        }
        
        // بررسی Poison Pill
        if (read_task.task_id == -1) {
            printf("[Reader %d] Received poison pill, exiting...\n", reader_id);
            pthread_mutex_unlock(&queue_mutex);
            sem_post(&empty);
            break;
        }
        
        struct timespec start_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time);
        
        pthread_mutex_unlock(&queue_mutex);
        sem_post(&empty);
        
        // شبیه‌سازی اجرای task
        printf("[Reader %d] Executing Task %d (Priority=%d, ExecTime=%d ms)\n",
               reader_id, read_task.task_id, read_task.priority, read_task.exec_time);
        
        simulate_execution(read_task.exec_time);
        
        struct timespec finish_time;
        clock_gettime(CLOCK_MONOTONIC, &finish_time);
        
        pthread_mutex_lock(&stats_mutex);
        if (stats_count < TOTAL_TASKS) {
            stats[stats_count].task_id = read_task.task_id;
            stats[stats_count].priority = read_task.priority;
            stats[stats_count].exec_time = read_task.exec_time;
            
            // تبدیل arrival_time_ns به timespec برای محاسبات
            stats[stats_count].arrival_time.tv_sec = read_task.arrival_time_ns / 1000000000ULL;
            stats[stats_count].arrival_time.tv_nsec = read_task.arrival_time_ns % 1000000000ULL;
            
            stats[stats_count].start_time = start_time;
            stats[stats_count].finish_time = finish_time;
            stats[stats_count].wait_time = time_diff_ms(&stats[stats_count].arrival_time, &start_time);
            stats[stats_count].turnaround_time = time_diff_ms(&stats[stats_count].arrival_time, &finish_time);
            stats[stats_count].reader_id = reader_id;
            stats_count++;
        }
        tasks_consumed++;
        pthread_mutex_unlock(&stats_mutex);
    }
    
    printf("[Reader %d] Finished\n", reader_id);
    return NULL;
}

// ✅ محاسبه و نمایش نتایج با جزئیات بیشتر
void print_statistics() {
    printf("\n========================================\n");
    printf("         PERFORMANCE METRICS\n");
    printf("   Scheduling Mode: %s\n", scheduling_mode == 0 ? "FCFS" : "Priority-based");
    printf("========================================\n\n");
    
    // تعداد task‌های تولیدشده به تفکیک اولویت
    printf("Tasks produced by priority:\n");
    printf("  Priority 0 (High):   %d tasks\n", priority_count[0]);
    printf("  Priority 1 (Medium): %d tasks\n", priority_count[1]);
    printf("  Priority 2 (Low):    %d tasks\n", priority_count[2]);
    printf("  Total: %d tasks\n\n", tasks_produced);
    
    if (stats_count == 0) {
        printf("No statistics available.\n");
        return;
    }
    
    // محاسبه آمار کلی
    double min_arrival_sec = stats[0].arrival_time.tv_sec + 
                             stats[0].arrival_time.tv_nsec / 1e9;
    double max_finish_sec = stats[0].finish_time.tv_sec + 
                            stats[0].finish_time.tv_nsec / 1e9;
    
    double total_wait = 0, total_turnaround = 0;
    double wait_by_priority[3] = {0, 0, 0};
    double turnaround_by_priority[3] = {0, 0, 0};
    int count_by_priority[3] = {0, 0, 0};
    
    for (int i = 0; i < stats_count; i++) {
        total_wait += stats[i].wait_time;
        total_turnaround += stats[i].turnaround_time;
        
        int prio = stats[i].priority;
        wait_by_priority[prio] += stats[i].wait_time;
        turnaround_by_priority[prio] += stats[i].turnaround_time;
        count_by_priority[prio]++;
        
        double arr = stats[i].arrival_time.tv_sec + stats[i].arrival_time.tv_nsec / 1e9;
        double fin = stats[i].finish_time.tv_sec + stats[i].finish_time.tv_nsec / 1e9;
        
        if (arr < min_arrival_sec) min_arrival_sec = arr;
        if (fin > max_finish_sec) max_finish_sec = fin;
    }
    
    double avg_wait = total_wait / stats_count;
    double avg_turnaround = total_turnaround / stats_count;
    
    // محاسبه throughput
    double total_time_sec = max_finish_sec - min_arrival_sec;
    double throughput = stats_count / total_time_sec;
    
    printf("=== OVERALL STATISTICS ===\n");
    printf("Average Wait Time: %.2f ms\n", avg_wait);
    printf("Average Turnaround Time: %.2f ms\n", avg_turnaround);
    printf("Throughput: %.2f tasks/second\n", throughput);
    printf("Total Execution Time: %.2f seconds\n\n", total_time_sec);
    
    // ✅ آمار به تفکیک اولویت (تفاوت اصلی FCFS و Priority اینجاست!)
    printf("=== STATISTICS BY PRIORITY ===\n");
    for (int p = 0; p < 3; p++) {
        if (count_by_priority[p] > 0) {
            printf("Priority %d (%s):\n", p, 
                   p == 0 ? "High" : (p == 1 ? "Medium" : "Low"));
            printf("  Tasks completed: %d\n", count_by_priority[p]);
            printf("  Avg Wait Time: %.2f ms\n", 
                   wait_by_priority[p] / count_by_priority[p]);
            printf("  Avg Turnaround Time: %.2f ms\n\n", 
                   turnaround_by_priority[p] / count_by_priority[p]);
        }
    }
    
    // ✅ نمایش تفاوت بین اولویت‌ها
    if (scheduling_mode == 1 && count_by_priority[0] > 0 && count_by_priority[2] > 0) {
        double high_prio_wait = wait_by_priority[0] / count_by_priority[0];
        double low_prio_wait = wait_by_priority[2] / count_by_priority[2];
        printf("*** Priority Scheduling Effect ***\n");
        printf("High priority wait time: %.2f ms\n", high_prio_wait);
        printf("Low priority wait time: %.2f ms\n", low_prio_wait);
        printf("Difference: %.2f ms (%.1fx)\n", 
               low_prio_wait - high_prio_wait,
               low_prio_wait / high_prio_wait);
        printf("(Low priority tasks wait %.1fx longer!)\n\n", 
               low_prio_wait / high_prio_wait);
    }
    
    // محاسبه CPU utilization برای هر reader
    if (num_readers > 1) {
        printf("=== CPU UTILIZATION PER READER ===\n");
        for (int r = 0; r < num_readers; r++) {
            double cpu_busy_time = 0;
            double first_start = -1, last_finish = -1;
            
            for (int i = 0; i < stats_count; i++) {
                if (stats[i].reader_id == r) {
                    cpu_busy_time += stats[i].exec_time;
                    
                    double start = stats[i].start_time.tv_sec + stats[i].start_time.tv_nsec / 1e9;
                    double finish = stats[i].finish_time.tv_sec + stats[i].finish_time.tv_nsec / 1e9;
                    
                    if (first_start < 0 || start < first_start) first_start = start;
                    if (last_finish < 0 || finish > last_finish) last_finish = finish;
                }
            }
            
            if (first_start >= 0 && last_finish >= 0) {
                double total_time = (last_finish - first_start) * 1000.0;
                double utilization = (cpu_busy_time / total_time) * 100.0;
                printf("  Reader %d: %.2f%%\n", r, utilization);
            }
        }
    } else {
        // محاسبه utilization کل برای single-core
        double cpu_busy_time = 0;
        for (int i = 0; i < stats_count; i++) {
            cpu_busy_time += stats[i].exec_time;
        }
        double utilization = (cpu_busy_time / (total_time_sec * 1000.0)) * 100.0;
        printf("=== CPU UTILIZATION ===\n");
        printf("Total CPU Utilization: %.2f%%\n", utilization);
    }
    
    printf("\n========================================\n");
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s <scenario> <scheduling_mode>\n", argv[0]);
        printf("  scenario:\n");
        printf("    1: Single-Core (1 writer + 1 reader)\n");
        printf("    2: Multi-Core (1 writer + 2 readers)\n");
        printf("    4: Multi-Core (1 writer + 4 readers)\n");
        printf("  scheduling_mode:\n");
        printf("    0: FCFS (First-Come-First-Served)\n");
        printf("    1: Priority-based\n");
        return 1;
    }
    
    int scenario = atoi(argv[1]);
    scheduling_mode = atoi(argv[2]);
    
    if (scheduling_mode != 0 && scheduling_mode != 1) {
        printf("Invalid scheduling mode. Use 0 (FCFS) or 1 (Priority)\n");
        return 1;
    }
    
    if (scenario == 1) {
        num_readers = 1;
        num_cores = 1;
        printf("Running Single-Core scenario (1 writer + 1 reader)\n");
    } else if (scenario == 2) {
        num_readers = 2;
        num_cores = 2;
        printf("Running Multi-Core scenario (1 writer + 2 readers)\n");
    } else if (scenario == 4) {
        num_readers = 4;
        num_cores = 4;
        printf("Running Multi-Core scenario (1 writer + 4 readers)\n");
    } else {
        printf("Invalid scenario. Use 1, 2, or 4\n");
        return 1;
    }
    
    printf("Scheduling Mode: %s\n\n", scheduling_mode == 0 ? "FCFS" : "Priority-based");
    
    // باز کردن دستگاه
    fd = open(DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        perror("Failed to open device");
        return 1;
    }
    
    // ✅ تنظیم حالت scheduling در kernel
    if (set_scheduling_mode(scheduling_mode) < 0) {
        close(fd);
        return 1;
    }
    
    // مقداردهی اولیه semaphores و mutex
    sem_init(&empty, 0, TOTAL_CAP);
    sem_init(&full, 0, 0);
    pthread_mutex_init(&queue_mutex, NULL);
    pthread_mutex_init(&stats_mutex, NULL);
    pthread_mutex_init(&priority_mutex, NULL);
    
    // ایجاد نخ‌ها
    pthread_t writer;
    pthread_t readers[num_readers];
    
    int writer_core = 0;
    pthread_create(&writer, NULL, writer_thread, &writer_core);
    
    int reader_ids[num_readers];
    for (int i = 0; i < num_readers; i++) {
        reader_ids[i] = i;
        pthread_create(&readers[i], NULL, reader_thread, &reader_ids[i]);
    }
    
    // انتظار برای اتمام نخ‌ها
    pthread_join(writer, NULL);
    for (int i = 0; i < num_readers; i++) {
        pthread_join(readers[i], NULL);
    }
    
    // نمایش نتایج
    print_statistics();
    
    // پاکسازی
    close(fd);
    sem_destroy(&empty);
    sem_destroy(&full);
    pthread_mutex_destroy(&queue_mutex);
    pthread_mutex_destroy(&stats_mutex);
    pthread_mutex_destroy(&priority_mutex);
    
    return 0;
}