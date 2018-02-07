#include <linux/module.h>
#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/ftrace.h>
#include <linux/kfifo.h>
#include <linux/semaphore.h>
#include <linux/random.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Module pr5");
MODULE_AUTHOR("Germán Franco Dorca - Álvaro Velasco García");

#define MAX_BUFFER_LEN 128

DEFINE_SPINLOCK(buff_lock);
DEFINE_SEMAPHORE(list_lock);
DEFINE_SEMAPHORE(cond_lectura);
DEFINE_SEMAPHORE(lock_open);

/* Default Values*/
struct timer_list my_timer; /* Structure that describes the kernel timer */
static unsigned int timer_period_ms = 1000;
static unsigned int emergency_threshold = 75; /* Max occupation percent */
static unsigned int max_random = 300;

struct kfifo buffer;
static struct work_struct transfer_task;
static unsigned int nr_readers_waiting = 0;

static struct proc_dir_entry *proc_entry;
static struct proc_dir_entry *config_proc_entry;

static struct list_head randlist;
typedef struct {
	struct list_head links;
	int num;
} list_item_t;

static void clear_list(struct list_head* list);


/************************************\
|     _____ _                        |
|    |_   _(_)_ __ ___   ___ _ __    |
|      | | | | '_ ` _ \ / _ \ '__|   |
|      | | | | | | | | |  __/ |      |
|      |_| |_|_| |_| |_|\___|_|      |
|                                    |
\************************************/

/* Function invoked when timer expires (fires) */
static void fire_timer(unsigned long data) {
	int num;
	unsigned long flags;
	int size;
	int cpu;

	spin_lock_irqsave(&buff_lock, flags);
	num = get_random_int() %  max_random;
	kfifo_in(&buffer, &num, sizeof(int));
	size = kfifo_len(&buffer);
	spin_unlock_irqrestore(&buff_lock, flags);

	printk(KERN_INFO "RANDOM: %d\n", num);

	if(size * 100 > emergency_threshold * MAX_BUFFER_LEN && !work_pending(&transfer_task)) {
		cpu = smp_processor_id();
		printk(KERN_INFO "TIMER REQUESTED FLUSH FROM CPU %d\n", cpu);
		schedule_work_on((cpu == 0 ? 1 : cpu - 1), &transfer_task);
	} else {
		printk(KERN_INFO "BUFFER CAPACITY=%d%%\n", (size * 100 / MAX_BUFFER_LEN));
	}

	/* Re-activate the timer one second from now */
	mod_timer(&my_timer, jiffies + msecs_to_jiffies(timer_period_ms));
}

int modt_init_timer (void) {
	/* Create timer */
	init_timer(&my_timer);
	/* Initialize fields */
	my_timer.data = 0;
	my_timer.function = fire_timer;
	my_timer.expires = jiffies + msecs_to_jiffies(timer_period_ms);

	return 0;
}

/*****************************************\
|   __        __         _                |
|   \ \      / /__  _ __| | _____ _ __    |
|    \ \ /\ / / _ \| '__| |/ / _ \ '__|   |
|     \ V  V / (_) | |  |   <  __/ |      |
|      \_/\_/ \___/|_|  |_|\_\___|_|      |
|                                         |
\*****************************************/

static void copy_items_into_list(struct work_struct *work) {
	int i;
	unsigned long flags;
	struct list_head templist;
	list_item_t* node;
	int kbuffer[MAX_BUFFER_LEN];
	unsigned int size;

	INIT_LIST_HEAD(&templist);

	printk(KERN_INFO "WORKER DOES FLUSH FROM CPU %d\n", smp_processor_id());

	/* Copy buffer. Idea: agilizar la concurrencia. */
	spin_lock_irqsave(&buff_lock, flags);
	size = kfifo_out(&buffer, kbuffer, kfifo_len(&buffer));
	spin_unlock_irqrestore(&buff_lock, flags);

	for(i = 0; i < size / sizeof(int); i++) {
		node = vmalloc(sizeof (list_item_t));
		if(node == NULL) {
			clear_list(&templist);
			return;
		}
		node->num = kbuffer[i];
		list_add(&node->links, &templist);
	}

	if(down_interruptible(&list_lock))
		return;

	/* Link temp list into our list */
	templist.prev->next = randlist.next; /* El puntero next de el ultimo nodo de templist apunta al primer nodo de randlist.*/
	randlist.next->prev = templist.prev; /* El puntero prev de el primer nodo de randlist apunta al ultimo nodo de templist.*/
	templist.next->prev = &randlist; /* El puntero prev de el primer nodo de templist apunta al ultimo nodo de randlist*/
	randlist.next = templist.next; /* El puntero next de el primer nodo de randlist apunta al primer nodo de randlist.*/

	if(nr_readers_waiting > 0) {
		nr_readers_waiting--;
		up(&cond_lectura);
	}
	up(&list_lock);

	printk(KERN_INFO "FLUSH BUFFER!!\n");
}

/**
 * Remove all elements from the list.
 */
static void clear_list(struct list_head* list) {
	list_item_t* cur;
	list_item_t* aux;
	/* Recorremos la lista para eliminar todos los nodos de la lista.*/
	list_for_each_entry_safe(cur, aux, list, links) {
		list_del( &(cur->links) );
		vfree(cur);
	}
}

/***************************************************************************************\
|         __                       __                   _ _   _                         |
|        / / __  _ __ ___   ___   / / __ ___   ___   __| | |_(_)_ __ ___   ___ _ __     |
|       / / '_ \| '__/ _ \ / __| / / '_ ` _ \ / _ \ / _` | __| | '_ ` _ \ / _ \ '__|    |
|      / /| |_) | | | (_) | (__ / /| | | | | | (_) | (_| | |_| | | | | | |  __/ |       |
|     /_/ | .__/|_|  \___/ \___/_/ |_| |_| |_|\___/ \__,_|\__|_|_| |_| |_|\___|_|       |
|         |_|                                                                           |
|                                                                                       |
\***************************************************************************************/

static ssize_t modtimer_read(struct file * file, char *buff, size_t len, loff_t * offset) {
	char numstr[21]; /* 2^64 + \n =  20 digits + 1 */
	int num;
	int ret = 0;
	list_item_t* last;

	if(down_interruptible(&list_lock))
		return -EINTR;

	while (list_empty(&randlist)) {
		nr_readers_waiting++; // cond_wait(cons,mtx);
		up(&list_lock);
		if(down_interruptible(&cond_lectura)) /* Cuando no hay elementos se bloquea.*/
			return -EINTR;
		if(down_interruptible(&list_lock))/* Randlist.*/
			return -EINTR;
	}

	last = list_entry(randlist.prev, list_item_t, links);
	list_del(randlist.prev);
	up(&list_lock);

	num = last->num;
	vfree(last);

	ret = snprintf(numstr, sizeof(numstr), "%d\n", num);
	if(ret > len)
		return -ENOMEM;

	if (copy_to_user(buff,numstr,ret))
		return -EFAULT;

	return ret;
}

static int modtimer_open(struct inode * inode, struct file * file) {
	if(down_interruptible(&lock_open))
		return -EINTR;

	if(module_refcount(THIS_MODULE) > 0) {
		up(&lock_open);
		return -EAGAIN;
	}
	try_module_get(THIS_MODULE);

	up(&lock_open);

	/* Activate the timer for the first time */
	add_timer(&my_timer);

	return 0;
}

static int modtimer_release(struct inode * inode, struct file * file) {

	unsigned long flags;

	// eliminar el timer
	del_timer_sync(&my_timer);

	flush_work(&transfer_task);

	// vaciar el buffer
	spin_lock_irqsave(&buff_lock, flags);
	kfifo_reset(&buffer);
	spin_unlock_irqrestore(&buff_lock, flags);

	// vaciar lista
	if(down_interruptible(&list_lock))
		return -EINTR;
	clear_list(&randlist);
	up(&list_lock);

	if(down_interruptible(&lock_open))
		return -EINTR;
	module_put(THIS_MODULE);
	up(&lock_open);
	return 0;
}

static const struct file_operations proc_entry_fops = {
	.open = modtimer_open,
	.read = modtimer_read,
	.release = modtimer_release
};

/***************************************************************************************\
|        __                       __                   _                  __ _          |
|       / / __  _ __ ___   ___   / / __ ___   ___   __| | ___ ___  _ __  / _(_) __ _    |
|      / / '_ \| '__/ _ \ / __| / / '_ ` _ \ / _ \ / _` |/ __/ _ \| '_ \| |_| |/ _` |   |
|     / /| |_) | | | (_) | (__ / /| | | | | | (_) | (_| | (_| (_) | | | |  _| | (_| |   |
|    /_/ | .__/|_|  \___/ \___/_/ |_| |_| |_|\___/ \__,_|\___\___/|_| |_|_| |_|\__, |   |
|        |_|                                                                   |___/    |
|                                                                                       |
\***************************************************************************************/

/* Shows this:
	timer_period_ms=500     // 16 + 20 + 1  chars
	emergency_threshold=75  // 20 + 20 + 1  chars
	max_random=300          // 11 + 20 + 1  chars
	----------------------- // TOTAL 110 + 1 chars
*/
static ssize_t modconfig_read(struct file * file, char *buff, size_t len, loff_t * offset) {
	char str[111];
	int ret = snprintf(str, sizeof(str),
			"timer_period_ms=%u\n"
			"emergency_threshold=%u\n"
			"max_random=%u\n",
			timer_period_ms, emergency_threshold, max_random);

	if(ret > len)
		return -ENOMEM;

	if (copy_to_user(buff,str,ret))
		return -EFAULT;
	return ret;
}

static ssize_t modconfig_write(struct file * file, const char __user *buff, size_t len, loff_t * offset) {
	char str[42];
	unsigned int num;

	if(len > sizeof(str))
		return -EINVAL;
	if (copy_from_user(str,buff,len))
		return -EFAULT;

	if(sscanf(str, "timer_period_ms=%u", &num)) {
		timer_period_ms = num;
	} else if(sscanf(str, "emergency_threshold=%u", &num)) {
		emergency_threshold = num;
	} else if(sscanf(str, "max_random=%u", &num)) {
		max_random = num;
	} else {
		return -EINVAL;
	}

	return len;
}

static const struct file_operations config_proc_entry_fops = {
	.read = modconfig_read,
	.write = modconfig_write
};


/*****************************************\
|     __  __           _       _          |
|    |  \/  | ___   __| |_   _| | ___     |
|    | |\/| |/ _ \ / _` | | | | |/ _ \    |
|    | |  | | (_) | (_| | |_| | |  __/    |
|    |_|  |_|\___/ \__,_|\__,_|_|\___|    |
|                                         |
\*****************************************/

int init_module(void) {
	int ret = 0;

	proc_entry = proc_create( "modtimer", 0666, NULL, &proc_entry_fops);
	config_proc_entry = proc_create( "modconfig", 0666, NULL, &config_proc_entry_fops);
	if (proc_entry == NULL || config_proc_entry == NULL) {
		ret = -ENOMEM;
		printk(KERN_INFO "modtimer: Can't create one or both of /proc entry\n");
	} else {

		if(kfifo_alloc(&buffer, MAX_BUFFER_LEN, GFP_KERNEL))
			return -ENOMEM;

		INIT_LIST_HEAD(&randlist);
		INIT_WORK(&transfer_task, copy_items_into_list);

		modt_init_timer();

		printk(KERN_INFO "modtimer: Module loaded\n");
	}

	return ret;
}


void cleanup_module( void ) {
	remove_proc_entry("modtimer", NULL);
	kfifo_free(&buffer);
	printk(KERN_INFO "modtimer: Module unloaded.\n");
}
