#include <linux/module.h>
#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/vmalloc.h>
#include <asm-generic/uaccess.h>
#include <linux/ftrace.h>
#include <linux/spinlock.h>


MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Module pr1");
MODULE_AUTHOR("Germán Franco Dorca - Álvaro Velasco García");

struct list_head mylist;
static struct proc_dir_entry *proc_entry;
/* Number of elements in the list */
static int length;
static int t_chars;

DEFINE_RWLOCK(rw);


typedef struct {
	int data;
	struct list_head links;
} list_item_t;

/* Calculates number of digits of a number */
static int intlen(int num) {
	int i = 0;
	if(num <= 0) {
		i = 1;
		num = -num;
	}
	while(num > 0) {
		num = num / 10;
		i++;
	}
	return i;
}

/**
 * Remove all elements containing "num" from the list.
 */
static void remove_from_list(int num) {
	list_item_t* cur;
	list_item_t* aux;

	write_lock(&rw); // Lock
	list_for_each_entry_safe(cur, aux, &mylist, links) {
		if(cur->data == num){
			list_del( &(cur->links) );
			length--;
			vfree(cur);
			t_chars -= intlen(num);
		}
	}
	write_unlock(&rw); // Unlock
}

/**
 * Remove all elements from the list.
 */
static void clear_list(void) {
	list_item_t* cur;
	list_item_t* aux;

	write_lock(&rw); // Lock
	list_for_each_entry_safe(cur, aux, &mylist, links) {
		list_del( &(cur->links) );
		vfree(cur);
	}
	length = 0;
	t_chars = 0;
	write_unlock(&rw); // Unlock
}

/**
 * Insert a new element in the list.
 */
static void insert_new(int num) {
	list_item_t* new_node = vmalloc(sizeof (list_item_t));
	if(new_node) {
		new_node->data = num;
		write_lock(&rw); // Lock
		list_add(&new_node->links, &mylist);
		length++;
		t_chars += intlen(num);
		write_unlock(&rw); // Unlock
	}
}


static ssize_t list_write(struct file *filp, const char __user *buf, size_t len, loff_t *off) {
	char* args;
	int num;

	if ((*off) > 0)  /* The application can write in this entry just once !! */
		return 0;

	args = vmalloc(len+1);

    	/* Transfer data from user to kernel space */
	if (copy_from_user(args, buf, len))
		return -EFAULT;

	/* Parse input.
	   Format --> op param(int)
	*/
	if(sscanf(args, "add %d", &num) == 1) {
		insert_new(num);
	} else if(sscanf(args, "remove %d", &num) == 1){
		remove_from_list(num);
	} else if(strncmp(args, "cleanup", 7) == 0){
		clear_list();
	}

	vfree(args);

	*off+=len; /* Update the file pointer */

	return len;
}


static ssize_t list_read(struct file *filp, char __user *buf, size_t len, loff_t *off) {
	list_item_t* cur;
	int nr_bytes;
	char* aux;
	int real_bytes;
	int i = 0;
	
	if ((*off) > 0) /* Tell the application that there is nothing left to read */
		return 0;
	
	/* aux size =  # ints + # \n + 1 (\0)*/
	nr_bytes = t_chars * sizeof(char) + length * sizeof(char); // Dirty read

	if (len < nr_bytes)
		return -ENOSPC;
	
	aux = vmalloc(nr_bytes + 1);
	
	/* Iterate over the list and store the contents in aux */
	read_lock(&rw);
	real_bytes = t_chars * sizeof(char) + length * sizeof(char);
	if(real_bytes > nr_bytes) {
		read_unlock(&rw);
		vfree(aux);
		return -ENOSPC;
	}
	list_for_each_entry(cur, &mylist, links) {
		i += sprintf(&aux[i], "%d\n", cur->data);
	}
	read_unlock(&rw);
	aux[i] = '\0';


	/* Transfer data from user to kernel space */
	if (copy_to_user(buf,aux,nr_bytes))
		return -EINVAL;

	(*off)+=len;
	vfree(aux);
	
	return nr_bytes; 
}

static const struct file_operations proc_entry_fops = {
    .read = list_read,
    .write = list_write,
};


int init_list_module( void ) {
  int ret = 0;

	proc_entry = proc_create( "my_mod", 0666, NULL, &proc_entry_fops);
	if (proc_entry == NULL) {
		ret = -ENOMEM;
		printk(KERN_INFO "my_mod: Can't create /proc entry\n");
	} else {
		printk(KERN_INFO "my_mod: Module loaded\n");
		length = 0;
		t_chars = 0;
		INIT_LIST_HEAD(&mylist);
	}

  return ret;
}


void exit_list_module( void ) {
	remove_proc_entry("my_mod", NULL);
	clear_list();

	printk(KERN_INFO "my_mod: Module unloaded.\n");
}

module_init( init_list_module );
module_exit( exit_list_module );
