#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/kfifo.h>
#include <linux/semaphore.h>
#include <linux/fs.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Module pr4");
MODULE_AUTHOR("Germán Franco Dorca - Álvaro Velasco García");

#define MAX_CBUFFER_LEN 64
#define MAX_KBUF 200
#define DEVICE_NAME "prodcons"

static struct kfifo cbuffer;
static struct semaphore mtx; /* Para garantizar exclusión mutua */
static struct semaphore sem_prod; /* Cola de espera para productor(es) */
static struct semaphore sem_cons; /* Cola de espera para consumidor(es) */

static volatile int prod_count = 0; /* Número de procesos que abrieron la entrada /proc para escritura (productores) */
static volatile int cons_count = 0; /* Número de procesos que abrieron la entrada /proc para lectura (consumidores) */
static volatile int nr_prod_waiting = 0; /* Número de procesos productores esperando */
static volatile int nr_cons_waiting = 0; /* Número de procesos consumidores esperando */

static int major;

/* Se invoca al hacer open() de entrada /dev */
static int fifodev_open(struct inode * inode, struct file * file) {
	if(down_interruptible(&mtx))
			return -EINTR;

		cons_count++;
		while(nr_prod_waiting > 0) {   // cond_signal(prod);
			nr_prod_waiting--;
			up(&sem_prod);
		}
		while(prod_count < 1) {
			nr_cons_waiting++; // cond_wait(cons,mtx);
			up(&mtx);
			if(down_interruptible(&sem_cons))
				return -EINTR;
			if(down_interruptible(&mtx))
					return -EINTR;
		}
	} else {    // Productores
		prod_count++;
		while(nr_cons_waiting > 0) {   // cond_signal(cons);
			nr_cons_waiting--;
			up(&sem_cons);
		}
		while(cons_count < 1) {
			nr_prod_waiting++; // cond_wait(cons,mtx);
			up(&mtx);
			if(down_interruptible(&sem_prod))
				return -EINTR;
			if(down_interruptible(&mtx))
					return -EINTR;
		}
	}

	up(&mtx);
	return 0;
}

/* Se invoca al hacer close() de entrada /dev */
static int fifodev_release(struct inode * inode, struct file * file) {
	if(down_interruptible(&mtx))
			return -EINTR;
	if(file->f_mode & FMODE_READ) { // Lectores (consumidores)
		cons_count--;
		while(nr_prod_waiting > 0) {
			nr_prod_waiting--;
			up(&sem_prod);
		}
	} else { // Escritores (productores)
		prod_count--;
		while(nr_cons_waiting > 0) {
			nr_cons_waiting--;
			up(&sem_cons);
		}
	}

	if(prod_count == 0 && cons_count == 0) 
		kfifo_reset(&cbuffer);

	up(&mtx);
	return 0;
}

/* Se invoca al hacer read() de entrada /dev */
static ssize_t fifodev_read(struct file * file, char *buff, size_t len, loff_t * offset) {
	char kbuffer[MAX_KBUF];
	if (len > MAX_CBUFFER_LEN || len > MAX_KBUF)
		return -ENOMEM;

	if(down_interruptible(&mtx))
		return -EINTR;

	/* Esperar hasta que haya elementos para consumir (debe haber productores) */
	while (kfifo_len(&cbuffer)<len && prod_count>0) {
		nr_cons_waiting++; // cond_wait(cons,mtx);
		up(&mtx);
		if(down_interruptible(&sem_cons))
			return -EINTR;

	}

	/* Detectar fin de comunicación por error (consumidor cierra FIFO antes) */
	if (prod_count==0 && kfifo_is_empty(&cbuffer)) {
		up(&mtx);
		return 0;
	}
	kfifo_out(&cbuffer,kbuffer,len);

	/* Despertar a posible productor bloqueado */
	if(nr_prod_waiting > 0) {   // cond_signal(prod);
		nr_prod_waiting--;
		up(&sem_prod);
	}
	up(&mtx);

	if (copy_to_user(buff,kbuffer,len))
		return -EFAULT;

	return len;
}

/* Se invoca al hacer write() de entrada /dev */
static ssize_t fifodev_write(struct file * file, const char *buff, size_t len, loff_t * offset) {
	char kbuffer[MAX_KBUF];

	if (len > MAX_CBUFFER_LEN || len> MAX_KBUF)
		return -ENOMEM;

	if (copy_from_user(kbuffer,buff,len))
		return -EFAULT;

	if(down_interruptible(&mtx))
		return -EINTR;
	/* Esperar hasta que haya hueco para insertar (debe haber consumidores) */
	while (kfifo_avail(&cbuffer) < len && cons_count > 0) {
		nr_prod_waiting++;
		up(&mtx);
		if(down_interruptible(&sem_prod))
			return -EINTR;
	}
	/* Detectar fin de comunicación por error (consumidor cierra FIFO antes) */
	if (cons_count==0) { up(&mtx); return -EPIPE; }
	kfifo_in(&cbuffer,kbuffer,len);
	/* Despertar a posible consumidor bloqueado */

	if(nr_cons_waiting > 0) {   // cond_signal(cons);
		nr_cons_waiting--;
		up(&sem_cons);
	}
	up(&mtx);
	return len;
}

struct file_operations dev_entry_fops = {
	.owner = THIS_MODULE,
	.read = fifodev_read,
	.write = fifodev_write,
	.open = fifodev_open,
	.release = fifodev_release
};

int init_module( void ) {
	int ret = 0;
	major = register_chrdev(0, DEVICE_NAME, &dev_entry_fops);

	if(major < 0) {
		ret = -ENOMEM;
		printk(KERN_INFO "prodcons: Can't create /dev entry\n");
		return ret;
	}
	if(kfifo_alloc(&cbuffer, MAX_CBUFFER_LEN, GFP_KERNEL)) {
		return -ENOMEM;
	}
	sema_init(&mtx, 1);
	sema_init(&sem_prod,0);
	sema_init(&sem_cons,0);

	return ret;
}


void cleanup_module( void ) {
	kfifo_free(&cbuffer);
	/* liberamos el dispositivo */
	unregister_chrdev(major, DEVICE_NAME);
}



