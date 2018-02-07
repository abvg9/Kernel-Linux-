// opcional
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

static struct proc_dir_entry *proc_entry;
static struct proc_dir_entry *config_proc_entry;

typedef struct {
	struct semaphore sem;
	unsigned int waiting;
} sem_cond_t;

DEFINE_SEMAPHORE(lock_open);
DEFINE_SEMAPHORE(wait_open);
static unsigned int readers_in = 0;

struct timer_list my_timer; /* Structure that describes the kernel timer */
static struct work_struct transfer_task;
static struct workqueue_struct* workqueue;

/* Default Values*/
static unsigned int timer_period_ms = 1000;
static unsigned int emergency_threshold = 75; /* Max occupation percent */
static unsigned int max_random = 300;

#define MAX_BUFFER_LEN 128
DEFINE_SPINLOCK(buff_lock);
struct kfifo buffer;

static sem_cond_t wait_even;
static sem_cond_t wait_odd;
DEFINE_SEMAPHORE(even_list_lock);
DEFINE_SEMAPHORE(odd_list_lock);
static struct list_head even_list;
static struct list_head odd_list;



typedef struct {
	struct list_head links;
	int num;
} list_item_t;

static void clear_list(struct list_head* list);

static void sem_cond_init(sem_cond_t* cond) {
	sema_init(&cond->sem, 0);
	cond->waiting = 0;
}

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
		queue_work_on((cpu == 0 ? 1 : cpu - 1), workqueue, &transfer_task);
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

/* l1 <-- into -- l2 */
static void insert_all(struct list_head* l1, struct list_head* l2) {
	if(!list_empty(l2)) {
		l2->prev->next = l1->next; /* El puntero next de el ultimo nodo de templist apunta al primer nodo de randlist.*/
		l1->next->prev = l2->prev; /* El puntero prev de el primer nodo de randlist apunta al ultimo nodo de templist.*/
		l2->next->prev = l1; /* El puntero prev de el primer nodo de templist apunta al ultimo nodo de randlist*/
		l1->next = l2->next; /* El puntero next de el primer nodo de randlist apunta al primer nodo de randlist.*/
	}
	INIT_LIST_HEAD(l2);
}

static void copy_items_into_list(struct work_struct *work) {
	int i;
	unsigned long flags;
	struct list_head even_templist;
	struct list_head odd_templist;
	list_item_t* node;
	int kbuffer[MAX_BUFFER_LEN];
	unsigned int size;

	INIT_LIST_HEAD(&even_templist);
	INIT_LIST_HEAD(&odd_templist);

	printk(KERN_INFO "WORKER DOES FLUSH FROM CPU %d\n", smp_processor_id());

	/* Copy buffer. Idea: agilizar la concurrencia. */
	spin_lock_irqsave(&buff_lock, flags);
	size = kfifo_out(&buffer, kbuffer, kfifo_len(&buffer));
	spin_unlock_irqrestore(&buff_lock, flags);

	for(i = 0; i < size / sizeof(int); i++) {
		node = vmalloc(sizeof (list_item_t));
		if(node == NULL) {
			clear_list(&even_templist);
			clear_list(&odd_templist);
			return;
		}
		node->num = kbuffer[i];
		if(node->num % 2 == 0) {
			list_add(&node->links, &even_templist);
		} else {
			list_add(&node->links, &odd_templist);
		}
	}

	/* Link even temp list into even list */
	if(down_interruptible(&even_list_lock))
		return;
	insert_all(&even_list, &even_templist);
	if(wait_even.waiting > 0) {
		wait_even.waiting--;
		up(&wait_even.sem);
	}
	up(&even_list_lock);

	/* Link odd temp list into odd list */
	if(down_interruptible(&odd_list_lock))
		return;
	insert_all(&odd_list, &odd_templist);
	if(wait_odd.waiting > 0) {
		wait_odd.waiting--;
		up(&wait_odd.sem);
	}
	up(&odd_list_lock);

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


static int list_pop_sync(struct list_head* list, struct semaphore* lock, sem_cond_t* cond) {
	int num;
	list_item_t* last;

	if(down_interruptible(lock))
		return -EINTR;

	while (list_empty(list)) {
		cond->waiting++; // cond_wait(cons,mtx);
		up(lock);
		if(down_interruptible(&cond->sem)) /* Cuando no hay elementos se bloquea.*/
			return -EINTR;
		if(down_interruptible(lock))/* Randlist.*/
			return -EINTR;
	}

	last = list_entry(list->prev, list_item_t, links);
	list_del(list->prev);
	up(lock);

	num = last->num;
	vfree(last);

	return num;
}

static ssize_t modtimer_read(struct file * file, char *buff, size_t len, loff_t * offset) {
	char numstr[21]; /* 2^64 + \n =  20 digits + 1 */
	int num;
	int ret = 0;

	if(file->private_data == 0) {
		num = list_pop_sync(&even_list, &even_list_lock, &wait_even);
	} else {
		num = list_pop_sync(&odd_list, &odd_list_lock, &wait_odd);
	}

	ret = snprintf(numstr, sizeof(numstr), "%d\n", num);
	if(ret > len)
		return -ENOMEM;

	if(copy_to_user(buff,numstr,ret))
		return -EFAULT;

	return ret;
}

static int modtimer_open(struct inode * inode, struct file * file) {
	int opened;
	if(down_interruptible(&lock_open))
		return -EINTR;

	opened = module_refcount(THIS_MODULE);
	if(opened > 1) {
		up(&lock_open);
		return -EAGAIN;
	} else {
		try_module_get(THIS_MODULE);
		readers_in++;
		up(&lock_open);

		if(opened == 0) { /* First that entered */
			file->private_data = (void*) 0;
			if(down_interruptible(&wait_open))
				return -EINTR;
		} else {
			file->private_data = (void*) 1;
			up(&wait_open);
			add_timer(&my_timer); /* Activate the timer for the first time */
		}
	}

	return 0;
}


static int modtimer_release(struct inode * inode, struct file * file) {
	unsigned long flags;

	if(down_interruptible(&lock_open))
		return -EINTR;

	readers_in--;
	if(readers_in == 0) {
		// eliminar el timer
		del_timer_sync(&my_timer);

		flush_work(&transfer_task);

		// vaciar el buffer
		spin_lock_irqsave(&buff_lock, flags);
		kfifo_reset(&buffer);
		spin_unlock_irqrestore(&buff_lock, flags);

		// Clear list of even numbers
		if(down_interruptible(&even_list_lock))
			return -EINTR;
		clear_list(&even_list);
		up(&even_list_lock);

		// Clear list of odd numbers
		if(down_interruptible(&odd_list_lock))
			return -EINTR;
		clear_list(&odd_list);
		up(&odd_list_lock);

		module_put(THIS_MODULE);
		module_put(THIS_MODULE);
	}

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
		sema_init(&wait_open, 0);
		sem_cond_init(&wait_even);
		sem_cond_init(&wait_odd);
		if(kfifo_alloc(&buffer, MAX_BUFFER_LEN, GFP_KERNEL))
			return -ENOMEM;

		INIT_LIST_HEAD(&even_list);
		INIT_LIST_HEAD(&odd_list);

		workqueue = create_workqueue("modtimerwq");
		if(workqueue == NULL)
			return -ENOMEM;
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
