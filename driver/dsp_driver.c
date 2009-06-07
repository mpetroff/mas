/*
  dsp_pci.c

  Contains all PCI related code, including register definitions and
  lowest level i/o routines.

  Spoofing can be accomplished at this level by setting up alternate
  handlers for reads and writes to the PCI card.
*/

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/interrupt.h>

#include "mce_options.h"
#include "kversion.h"

#ifdef REALTIME
#include <rtai.h>
#endif

#ifndef OLD_KERNEL
#  include <linux/dma-mapping.h>
/* Newer 2.6 kernels use IRQF_SHARED instead of SA_SHIRQ */
#  ifdef IRQF_SHARED
#    define IRQ_FLAGS IRQF_SHARED
#  else
#    define IRQ_FLAGS SA_SHIRQ
#  endif
#else
#  define IRQ_FLAGS 0
#endif

#include "dsp_driver.h"
#include "mce/dsp_ioctl.h"

/*
 *  dsp_driver.c includes
 */

#include <linux/module.h>
#include <linux/init.h>

#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/fcntl.h>
#include <asm/uaccess.h>

#include "mce_driver.h"
#include "dsp_ops.h"
#include "proc.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR ("Matthew Hasselfield"); 


/* Internal prototypes */

void  dsp_driver_remove(struct pci_dev *pci);

int   dsp_driver_probe(struct pci_dev *pci, const struct pci_device_id *id);


/* Data for PCI enumeration */

static const struct pci_device_id pci_ids[] = {
	{ PCI_DEVICE(DSP_VENDORID, DSP_DEVICEID) },
	{ 0 },
};

static struct pci_driver pci_driver = {
	.name = "mce_dsp",
	.id_table = pci_ids,
	.probe = dsp_driver_probe,
	.remove = dsp_driver_remove,
};

/*
  Each command is mapped to a particular DSP host control vector
  address.  Those can be firmware specific, but aren't so far.
*/

enum dsp_vector_type {
	VECTOR_STANDARD, // 4 words into htxr
	VECTOR_QUICK,    // no htxr, no reply expected
};

struct dsp_vector {
	u32 key;
	u32 vector;
	enum dsp_vector_type type;
};

#define NUM_DSP_CMD 17

#define MY_HMNI 0 /* HCVR_HNMI */

static struct dsp_vector dsp_vector_set[NUM_DSP_CMD] = {
	{DSP_WRM, MY_HMNI | 0x0079, VECTOR_STANDARD},
	{DSP_RDM, MY_HMNI | 0x007B, VECTOR_STANDARD},
	{DSP_VER, MY_HMNI | 0x007B, VECTOR_STANDARD},
	{DSP_GOA, MY_HMNI | 0x007D, VECTOR_STANDARD},
	{DSP_STP, MY_HMNI | 0x007F, VECTOR_STANDARD},
	{DSP_RST, MY_HMNI | 0x0081, VECTOR_STANDARD},
	{DSP_CON, MY_HMNI | 0x0083, VECTOR_STANDARD},
	{DSP_HST, MY_HMNI | 0x0085, VECTOR_STANDARD},
	{DSP_RCO, MY_HMNI | 0x0087, VECTOR_STANDARD},
	{DSP_QTS, MY_HMNI | 0x0089, VECTOR_STANDARD},
	{DSP_INT_RST, HCVR_INT_RST, VECTOR_QUICK},
	{DSP_INT_DON, HCVR_INT_DON, VECTOR_QUICK},
	{DSP_INT_RPC, HCVR_INT_RPC, VECTOR_QUICK},
	{DSP_SYS_ERR, HCVR_SYS_ERR, VECTOR_QUICK},
	{DSP_SYS_RST, HCVR_SYS_RST, VECTOR_QUICK},
};

/* DSP register wrappers */

static inline int dsp_read_hrxs(dsp_reg_t *dsp) {
	return ioread32((void*)&(dsp->htxr_hrxs));
}

static inline int dsp_read_hstr(dsp_reg_t *dsp) {
	return ioread32((void*)&(dsp->hstr));
}

static inline int dsp_read_hcvr(dsp_reg_t *dsp) {
	return ioread32((void*)&(dsp->hcvr));
}

static inline int dsp_read_hctr(dsp_reg_t *dsp) {
	return ioread32((void*)&(dsp->hctr));
}

static inline void dsp_write_htxr(dsp_reg_t *dsp, u32 value) {
	iowrite32(value, (void*)&(dsp->htxr_hrxs));
}

static inline void dsp_write_hcvr(dsp_reg_t *dsp, u32 value) {
	iowrite32(value, (void*)&(dsp->hcvr));
}

static inline void dsp_write_hctr(dsp_reg_t *dsp, u32 value) {
	iowrite32(value, (void*)&(dsp->hctr));
}

typedef struct {

	u32 code;
	dsp_handler handler;
	unsigned long data;

} dsp_handler_entry;

#define MAX_HANDLERS 16


typedef enum {

	DDAT_IDLE = 0,
	DDAT_CMD,

} dsp_state_t;

struct dsp_local {

	struct semaphore sem;
	wait_queue_head_t queue;
	dsp_message *msg;
	int flags;
#define   LOCAL_CMD 0x01
#define   LOCAL_REP 0x02
#define   LOCAL_ERR 0x08

};


/* Mode bits in DSP firmware (U0105+) - in particular, we want to set
 * up NOIRQ and HANDSHAKE before issuing any DSP commands that will
 * interrupt and reply.  */

#define DSP_MODE_APP          0x0001
#define DSP_MODE_MCE          0x0002
#define DSP_MODE_QUIETDA      0x0004
#define DSP_MODE_QUIETRP      0x0008

#define DSP_PCI_MODE_HANDSHAKE    HCTR_HF1
#define DSP_PCI_MODE_NOIRQ        HCTR_HF2

struct dsp_dev_t {

	struct pci_dev *pci;

	dsp_reg_t *dsp;

	int comm_mode;
	irq_handler_t int_handler;

 	struct tasklet_struct handshake_tasklet;
	struct timer_list tim_poll;

	struct dsp_local local;

	struct timer_list tim_dsp;

	spinlock_t lock;
	int cmd_count;
	int rep_count;

	dsp_state_t state;
	int version;
	char version_string[32];

	int n_handlers;
	dsp_handler_entry handlers[MAX_HANDLERS];

	dsp_callback callback;

} dsp_dev[MAX_CARDS];


#define DDAT_LOCK    spin_lock_irqsave(&dev->lock, irqflags)
#define DDAT_UNLOCK  spin_unlock_irqrestore(&dev->lock, irqflags)

void dsp_ack_int_or_schedule(unsigned long data)
{
	struct dsp_dev_t *dev = (struct dsp_dev_t*)data;
	/* Check that DSP has dropped HF3 */
	if (dsp_read_hstr(dev->dsp) & HSTR_HC3) {
		PRINT_ERR("Rescheduling int ack.");
		tasklet_schedule(&dev->handshake_tasklet);
	} else {
		dsp_write_hctr(dev->dsp, dev->comm_mode);
	}
}


#define SUBNAME "pci_int_handler: "
irqreturn_t pci_int_handler(int irq, void *dev_id, struct pt_regs *regs)
{
	/* Note that the regs argument is deprecated in newer kernels,
	   do not use it.  It is left here for compatibility with
	   -2.6.18                                                    */

	struct dsp_dev_t *dev = NULL;
	dsp_reg_t *dsp = NULL;
	dsp_message msg;
	int i=0, j=0, k=0;
	int n = sizeof(msg) / sizeof(u32);

	for(k=0; k<MAX_CARDS; k++) {
		dev = dsp_dev + k;
		if (dev_id == dev) {
			dsp = dev->dsp;
			break;
		}
	}

	if (dev_id != dev) return IRQ_NONE;

	//Verify handshake bit
	if ( !(dsp_read_hstr(dsp) & HSTR_HC3) ) {
		//FIX ME: Continuous stream of general interrupts
		PRINT_ERR(SUBNAME "irq entry without HF3 bit!\n");
		return IRQ_NONE;
	}

	// Interrupt hand-shaking changed in U0105.
	if (dev->comm_mode & DSP_PCI_MODE_HANDSHAKE) {
		// Raise HF0 to acknowledge that IRQ is being handled.
		// DSP will lower INTA and then HF3, and wait for HF0 to fall.
		dsp_write_hctr(dev->dsp, dev->comm_mode | HCTR_HF0);
	} else {
		// Host command to clear INTA
		dsp_write_hcvr(dsp, HCVR_INT_RST | HCVR_HC);
	}

	// Read data into dsp_message structure
	while ( i<n && (dsp_read_hstr(dsp) & HSTR_HRRQ) ) {
		((u32*)&msg)[i++] = dsp_read_hrxs(dsp) & DSP_DATAMASK;
	}
	if (i<n)
   	        PRINT_ERR(SUBNAME "incomplete message %i/%i.\n", i, n);

	// We are done with the DSP, so release it.
	if (dev->comm_mode & DSP_PCI_MODE_HANDSHAKE) {
		dsp_ack_int_or_schedule((unsigned long)dev);
	} else {
		// Host command to clear HF3
		dsp_write_hcvr(dsp, HCVR_INT_DON | HCVR_HC);
	}

	// Discover message handler 	
	for (j=0; j < dev->n_handlers; j++) {
		if ( (dev->handlers[j].code == msg.type) &&
		     (dev->handlers[j].handler != NULL) ) {
			dev->handlers[j].handler(&msg, dev->handlers[j].data);
		}
	}

	PRINT_INFO(SUBNAME "ok\n");
	return IRQ_HANDLED;
}
#undef SUBNAME


/* 
 * This will handle REP interrupts from the DSP, which correspond to
 * replies to DSP commands.
 */
#define SUBNAME "dsp_reply_handler: "
int dsp_reply_handler(dsp_message *msg, unsigned long data)
{
	unsigned long irqflags;
  	struct dsp_dev_t *dev = (struct dsp_dev_t *)data;
	dsp_callback callback = NULL;

	DDAT_LOCK;
	if (dev->state == DDAT_CMD) {
		PRINT_INFO(SUBNAME
			   "REP received, calling back.\n");
		// Store a copy of the callback address before going to IDLE
		callback = dev->callback;
		dev->state = DDAT_IDLE;
		dev->rep_count++;
	} else {
		PRINT_ERR(SUBNAME
			  "unexpected REP received [state=%i, %i %i].\n",
			  dev->state, dev->cmd_count, dev->rep_count);
	}
	PRINT_ERR(SUBNAME "%i %x %x %x %x\n", dev->rep_count, msg->type,
		  msg->command, msg->reply, msg->data);
	DDAT_UNLOCK;
		
	// Command state is IDLE, so callback routines may issue DSP cmds.
	if (callback != NULL) {
		int card = dev - dsp_dev;
		callback(0, msg, card);
	}

	return 0;
}
#undef SUBNAME

/* This will handle HEY interrupts from the DSP, which are generic
 * communications from the DSP typically used for debugging.
 */
#define SUBNAME "dsp_hey_handler: "
int dsp_hey_handler(dsp_message *msg, unsigned long data)
{
	PRINT_ERR(SUBNAME "dsp HEY received: %06x %06x %06x\n",
		  msg->command, msg->reply, msg->data);
	return 0;
}
#undef SUBNAME


#define SUBNAME "dsp_timeout: "
void dsp_timeout(unsigned long data)
{
	unsigned long irqflags;
	struct dsp_dev_t *dev = (struct dsp_dev_t*)data;
	dsp_callback callback = dev->callback;

	DDAT_LOCK;
	if (dev->state == DDAT_CMD) {
		callback = dev->callback;
		dev->state = DDAT_IDLE;
		DDAT_UNLOCK;

		PRINT_ERR(SUBNAME "dsp reply timed out!\n");
		if (callback != NULL) {
			int card = dev - dsp_dev;
			callback(-DSP_ERR_TIMEOUT, NULL, card);
		}
	} else {
		DDAT_UNLOCK;
		PRINT_INFO(SUBNAME "timer ignored\n");
	}
}
#undef SUBNAME


/*
  DSP command sending framework

  Modules issue dsp commands and register a callback function with

  dsp_send_command_now

  This looks up the vector address by calling dsp_lookup_vector,
  registers the callback in the device structure, and issues the
  command to the correct vector by calling
  dsp_send_command_now_vector.
*/


#define SUBNAME "dsp_quick_command: "
int dsp_quick_command(u32 vector, int card) 
{
	struct dsp_dev_t *dev = dsp_dev + card;
	PRINT_INFO(SUBNAME "sending vector %#x\n", vector);
	dsp_write_hcvr(dev->dsp, vector | HCVR_HC);
	return 0;
}
#undef SUBNAME


#define SUBNAME "dsp_lookup_vector: "
struct dsp_vector *dsp_lookup_vector(dsp_command *cmd)
{
	int i;
	for (i = 0; i < NUM_DSP_CMD; i++)
		if (dsp_vector_set[i].key == cmd->command)
			return dsp_vector_set+i;
	
	PRINT_ERR(SUBNAME "could not identify command %#x\n",
		  cmd->command);
	return NULL;
}
#undef SUBNAME


#define SUBNAME "dsp_send_command_now_vector: "
int dsp_send_command_now_vector(dsp_command *cmd, u32 vector, int card) 
{
	struct dsp_dev_t *dev = dsp_dev + card;
	int i = 0;
	int n = sizeof(dsp_command) / sizeof(u32);

	// DSP may block while HCVR interrupts in some cases.
	if (dsp_read_hcvr(dev->dsp) & HCVR_HC)
		return -EAGAIN;

	// HSTR must be ready to receive
	if ( !(dsp_read_hstr(dev->dsp) & HSTR_TRDY) ) {
		PRINT_ERR(SUBNAME "HSTR not ready to transmit!\n");
		return -EIO;
	}

	//Write bytes and interrupt
	while ( i<n && (dsp_read_hstr(dev->dsp) & HSTR_HTRQ)!=0 )
		dsp_write_htxr(dev->dsp, ((u32*)cmd)[i++]);

	if (i<n) {
		PRINT_ERR(SUBNAME "HTXR filled up during write! HSTR=%#x\n",
			  dsp_read_hstr(dev->dsp));
		return -EIO;
	}
	
	dsp_write_hcvr(dev->dsp, vector | HCVR_HC);

	return 0;
}
#undef SUBNAME


#define SUBNAME "dsp_send_command_now: "
int dsp_send_command_now(dsp_command *cmd, int card) 
{
	struct dsp_vector *vect = dsp_lookup_vector(cmd);

	PRINT_INFO(SUBNAME "cmd=%06x\n", cmd->command);

	if (vect==NULL) return -ERESTARTSYS;

	switch (vect->type) {

	case VECTOR_STANDARD:
		return dsp_send_command_now_vector(cmd, vect->vector, card);

	case VECTOR_QUICK:
		// FIXME: these don't reply so they'll always time out.
		return dsp_quick_command(vect->vector, card);

	}

	PRINT_ERR(SUBNAME
		  "unimplemented vector command type %06x for command %06x\n",
		  vect->type, cmd->command);
	return -ERESTARTSYS;
}
#undef SUBNAME


/*
  DSP command sending framework

  - The most straight-forward approach to sending DSP commands is to
    call dsp_command with pointers to the full dsp_command structure
    and an empty dsp_message structure.

  - A non-blocking version of the above that allows the caller to
    specify a callback routine that will be run upon receipt of the
    DSP ack/err interrupt message is exposed as dsp_command_nonblock.
    The calling module must notify the dsp module that the message has
    been processed by calling dsp_clear_commflags; this cannot be done
    from within the callback routine!

  - The raw, no-semaphore-obtaining, no-flag-checking-or-setting,
    non-invalid-command-rejecting command issuer is dsp_command_now.
    Don't use it.  Only I'm allowed to use it.

*/


/* For better or worse, this routine returns linux error codes.
   
   The callback error codes are 0 (success, msg will be non-null) or
   DSP_ERR_TIMEOUT (failure, msg will be null).                    */
#define SUBNAME "dsp_send_command: "
int dsp_send_command(dsp_command *cmd, dsp_callback callback, int card)
{
	unsigned int irqflags;
  	struct dsp_dev_t *dev = dsp_dev + card;
	int err = 0;

	DDAT_LOCK;
	if (dev->state != DDAT_IDLE) {
		PRINT_ERR(SUBNAME "ddat not idle at %i, EAGAIN.\n", dev->cmd_count);
		DDAT_UNLOCK;
		return -EAGAIN;
	}
	
	
	PRINT_INFO(SUBNAME "entry\n");

	PRINT_ERR(SUBNAME "send %i\n", dev->cmd_count+1);
	if ((err = dsp_send_command_now(cmd, card)) == 0) {
		dev->cmd_count++;
		dev->callback = callback;
		mod_timer(&dev->tim_dsp, jiffies + DSP_DEFAULT_TIMEOUT);
		dev->state = DDAT_CMD;
	}

	PRINT_INFO(SUBNAME "returning [%i]\n", err);
	DDAT_UNLOCK;
	return err;
}
#undef SUBNAME


#define SUBNAME "dsp_send_command_wait_callback"
int dsp_send_command_wait_callback(int error, dsp_message *msg, int card)
{
	struct dsp_dev_t *dev = dsp_dev + card;

	wake_up_interruptible(&dev->local.queue);

	if (dev->local.flags != LOCAL_CMD) {
		PRINT_ERR(SUBNAME "unexpected flags, cmd=%x rep=%x err=%x\n",
			  dev->local.flags & LOCAL_CMD,
			  dev->local.flags & LOCAL_REP,
			  dev->local.flags & LOCAL_ERR);
		return -1;
	}
	memcpy(dev->local.msg, msg, sizeof(*dev->local.msg));
	dev->local.flags |= LOCAL_REP;

	return 0;
}
#undef SUBNAME


#define SUBNAME "dsp_send_command_wait: "
int dsp_send_command_wait(dsp_command *cmd,
			  dsp_message *msg, int card)
{
	struct dsp_dev_t *dev = dsp_dev + card;
	int err = 0;

	PRINT_INFO(SUBNAME "entry\n");

	// Try to get the default sem (spinlock!)
	if (down_trylock(&dev->local.sem))
		return -ERESTARTSYS;

	//Register message for our callback to fill
	dev->local.msg = msg;
	dev->local.flags = LOCAL_CMD;
	
	if ((err=dsp_send_command(cmd, dsp_send_command_wait_callback, card)) != 0)
		goto up_and_out;

	PRINT_INFO(SUBNAME "commanded, waiting\n");
	if (wait_event_interruptible(dev->local.queue,
				     dev->local.flags
				     & (LOCAL_REP | LOCAL_ERR))) {
		dev->local.flags = 0;
		err = -ERESTARTSYS;
		goto up_and_out;
	}
	
	err = (dev->local.flags & LOCAL_ERR) ? -EIO : 0;
	
 up_and_out:

	PRINT_INFO(SUBNAME "returning %x\n", err);
	up(&dev->local.sem);
	return err;
}
#undef SUBNAME

/*
 *  Initialization and clean-up
 */


#define SUBNAME "dsp_query_version: "
int dsp_query_version(int card)
{
  	struct dsp_dev_t *dev = dsp_dev + card;
	int err = 0;
	dsp_command cmd = { DSP_VER, {0,0,0} };
	dsp_message msg;
	strcpy(dev->version_string,  "<=U0103");
	
	dev->version = 0;
	if ( (err=dsp_send_command_wait(&cmd, &msg, card)) != 0 )
		return err;

	dev->version = DSP_U0103;

	if (msg.reply == DSP_ACK) {
	
		dev->version_string[0] = msg.data >> 16;
		sprintf(dev->version_string+1, "%02i%02i",
			(msg.data >> 8) & 0xff, msg.data & 0xff);

		dev->version = msg.data;
	}
		
	PRINT_ERR(SUBNAME " discovered PCI card DSP code version %s\n",
		  dev->version_string);
	return 0;
}
#undef SUBNAME

void dsp_clear_RP(int card)
{
	dsp_command cmd = { DSP_INT_RPC, {0, 0, 0} };
	
	// This is interrupt safe because it is a vector command
	dsp_send_command_now(&cmd, card);
}

#define SUBNAME "dsp_timer_function: "
void dsp_timer_function(unsigned long data)
{
	struct dsp_dev_t *dev = (struct dsp_dev_t *)data;
	PRINT_INFO(SUBNAME "entry\n");
	pci_int_handler(0, dev, NULL);
	mod_timer(&dev->tim_poll, jiffies + DSP_POLL_JIFFIES);
}
#undef SUBNAME


void dsp_clear_interrupt(dsp_reg_t *dsp)
{
	// Clear interrupt flags
	dsp_write_hcvr(dsp, HCVR_INT_RST | HCVR_HC);
	dsp_write_hcvr(dsp, HCVR_INT_DON | HCVR_HC);
}


/*
  Memory allocation

  This is lame, but DMA-able memory likes a pci device in old kernels.
*/


void* dsp_allocate_dma(ssize_t size, unsigned long* bus_addr_p)
{
#ifdef OLD_KERNEL
//FIX_ME: mce will call this with out card info currently
	return pci_alloc_consistent(dev->pci, size, bus_addr_p);
#else
	return dma_alloc_coherent(NULL, size, (dma_addr_t*)bus_addr_p,
				  GFP_KERNEL);
#endif
}

void  dsp_free_dma(void* buffer, int size, unsigned long bus_addr)
{
#ifdef OLD_KERNEL
//FIX_ME: mce will call this with out card info currently
 	  pci_free_consistent (dev->pci, size, buffer, bus_addr);
#else
	  dma_free_coherent(NULL, size, buffer, bus_addr);
#endif
}

#define SUBNAME "dsp_pci_flush: "
int dsp_pci_flush()
{
	//FIX ME: no current called, needs card info
	struct dsp_dev_t *dev = dsp_dev;
	dsp_reg_t *dsp = dev->dsp;

	int count = 0, tmp;

	PRINT_INFO("dsp_pci_flush:");
	while ((dsp_read_hstr(dsp) & HSTR_HRRQ) && (count++ < PCI_MAX_FLUSH))
	{
		tmp = dsp_read_hrxs(dsp);
		if (count<4) PRINT_INFO(" %x", tmp);
		else if (count==4) PRINT_INFO(" ...");
	}

	PRINT_INFO("\n");

	if (dsp_read_hstr(dsp) & HSTR_HRRQ) {
		PRINT_ERR("dsp_pci_flush: could not empty HRXS!\n");
		return -1;
	}

	return 0;
}
#undef SUBNAME


#define SUBNAME "dsp_pci_remove_handler: "
int dsp_pci_remove_handler(struct dsp_dev_t *dev)
{
	struct pci_dev *pci = dev->pci;

	if (dev->int_handler==NULL) {
		PRINT_INFO(SUBNAME "no handler installed\n");
		return 0;
	}

#ifdef REALTIME
	rt_disable_irq(pci->irq);
	rt_free_global_irq(pci->irq);
#else
	free_irq(pci->irq, dev);
#endif
	dev->int_handler = NULL;

	return 0;
}
#undef SUBNAME


#define SUBNAME "dsp_pci_set_handler: "
int dsp_pci_set_handler(int card, irq_handler_t handler,
			char *dev_name)
{
	struct dsp_dev_t *dev = dsp_dev + card;
	struct pci_dev *pci = dev->pci;	
	int err = 0;
	int cfg_irq = 0;

	if (pci==NULL || dev==NULL) {
		PRINT_ERR(SUBNAME "Null pointers! pci=%lx dev=%lx\n",
			  (long unsigned)pci, (long unsigned)dev);
		return -ERESTARTSYS;
	}
	
	pci_read_config_byte(pci, PCI_INTERRUPT_LINE, (char*)&cfg_irq);
	PRINT_INFO(SUBNAME "pci has irq %i and config space has irq %i\n",
		   pci->irq, cfg_irq);

	// Free existing handler
	if (dev->int_handler!=NULL)
		dsp_pci_remove_handler(dev);
	
#ifdef REALTIME
	PRINT_ERR(SUBNAME "request REALTIME irq %#x\n", pci->irq);
	rt_disable_irq(pci->irq);
	err = rt_request_global_irq(pci->irq, (void*)handler);
#else
	PRINT_INFO(SUBNAME "requesting irq %#x\n", pci->irq);
	err = request_irq(pci->irq, handler, IRQ_FLAGS, dev_name, dev);
#endif

	if (err!=0) {
		PRINT_ERR(SUBNAME "irq request failed with error code %#x\n",
			  -err);
		return err;
	}

#ifdef REALTIME
	rt_startup_irq(pci->irq);
	rt_enable_irq(pci->irq);
#endif

	dev->int_handler = handler;
	return 0;
}
#undef SUBNAME


//Can be called in the future to clear a handler, not in use atm
#define SUBNAME "dsp_clear_handler: "
int dsp_clear_handler(u32 code, int card)
{
  	struct dsp_dev_t *dev = dsp_dev + card;
	int i = 0;

	PRINT_INFO(SUBNAME "entry\n");

	for (i=0; i<dev->n_handlers; i++) {
		if (dev->handlers[i].code == code)
			break;
	}
	
	if (i>=dev->n_handlers)
		return -1;
	
	dev->n_handlers--;

	// Move entries i+1 to i.
	for ( ; i<dev->n_handlers; i++) {
		memcpy(dev->handlers + i, dev->handlers + i + 1,
		       sizeof(*dev->handlers));
	}

	// Clear the removed entry
	memset(dev->handlers + dev->n_handlers, 0, sizeof(*dev->handlers));

	PRINT_INFO(SUBNAME "ok\n");
	return 0;
}
#undef SUBNAME


#define SUBNAME "dsp_set_msg_handler: "
int dsp_set_msg_handler(u32 code, dsp_handler handler, unsigned long data, int card)
{
	struct dsp_dev_t *dev = dsp_dev + card;
	int i;

	PRINT_INFO(SUBNAME "entry\n");

	// Replace handler if it exists
	for (i=0; i<dev->n_handlers; i++) {
		if (dev->handlers[i].code == code) {
			dev->handlers[i].handler = handler;
			dev->handlers[i].data = data;
			return 0;
		}
	}

	// Add to end of list
	if (i < MAX_HANDLERS) {
		dev->handlers[i].code = code;
		dev->handlers[i].handler = handler;
		dev->handlers[i].data = data;
	        dev->n_handlers++;
		PRINT_INFO(SUBNAME "ok\n");
		return 0;
	}

	PRINT_ERR(SUBNAME "no available handler slots\n");
	return -1;
}
#undef SUBNAME


/*
 *  IOCTL, for what it's worth...
 */

#define SUBNAME "dsp_driver_ioctl: "
int dsp_driver_ioctl(unsigned int iocmd, unsigned long arg, int card)
{
  	struct dsp_dev_t *dev = dsp_dev + card;

	switch(iocmd) {

	case DSPDEV_IOCT_SPEAK:
		PRINT_IOCT(SUBNAME "state=%#x\n", dev->state);
		break;

	case DSPDEV_IOCT_CORE:
		
		if (dev->pci == NULL) {
			PRINT_IOCT(SUBNAME "pci structure is null\n");
			return 0;
		}
		if (dev->dsp == NULL) {
			PRINT_IOCT(SUBNAME
				   "pci-dsp memory structure is null\n");
			return 0;
		}
		PRINT_IOCT(SUBNAME "hstr=%#06x hctr=%#06x\n",
			   dsp_read_hstr(dev->dsp), dsp_read_hctr(dev->dsp));		
		break;

	case DSPDEV_IOCT_CORE_IRQ:
		if (arg) {
			PRINT_IOCT(SUBNAME "Enabling interrupt\n");
			/* Cast on handler is necessary for backward compat. */
			if (dsp_pci_set_handler(card, (irq_handler_t)pci_int_handler,
						"mce_hacker") < 0) {
				PRINT_ERR(SUBNAME "Could not install interrupt handler!\n");
				return -1;
			}
		} else {
			PRINT_IOCT(SUBNAME "Disabling interrupt\n");
			dsp_pci_remove_handler(dev);
		}
		break;

	default:
		PRINT_ERR(SUBNAME "I don't handle iocmd=%ui\n", iocmd);
		return -1;
	}

	return 0;

}
#undef SUBNAME


int dsp_proc(char *buf, int count, int card)
{
	struct dsp_dev_t *dev = dsp_dev + card;
	int len = 0;

	PRINT_INFO("dsp_proc: card = %d\n", card);
	if (dev->pci == NULL) 
		return len;
	if (len < count) {
		len += sprintf(buf+len, "    %-15s %25s\n",
			       "bus address:", pci_name(dev->pci));
	}
	if (len < count) {
		len += sprintf(buf+len, "    %-15s %25s\n",
			       "interrupt:",
			       (dev->comm_mode & DSP_PCI_MODE_NOIRQ) ? "polling" : "enabled");
	}
	if (len < count) {
		len += sprintf(buf+len, "    %-32s %#08x\n    %-32s %#08x\n"
			       "    %-32s %#08x\n",
			       "hstr:", dsp_read_hstr(dev->dsp),
			       "hctr:", dsp_read_hctr(dev->dsp),
			       "hcvr:", dsp_read_hcvr(dev->dsp));
	}
	if (len < count) {
		len += sprintf(buf+len, "    %-20s %20s\n",
			       "firmware version:", dev->version_string);
	}
	if (len < count) {
		len += sprintf(buf+len, "    %-30s ", "state:");
		switch (dev->state) {
		case DDAT_IDLE:
			len += sprintf(buf+len, "      idle\n");
			break;
		case DDAT_CMD:
			len += sprintf(buf+len, " commanded\n");
			break;
		default:
			len += sprintf(buf+len, "   unknown! [%i]\n", dev->state);
			break;
		}
	}

	return len;
}


#define SUBNAME "dsp_configure: "
int dsp_configure(struct pci_dev *pci)
{
	int err = 0;
	int card;
	struct dsp_dev_t *dev;

	PRINT_INFO(SUBNAME "entry\n");

	// Find a free slot in dsp_dev array; this defines the card id
	if (pci==NULL) {
		PRINT_ERR(SUBNAME "Called with NULL pci_dev!\n");
		return -EPERM;
	}
	for (card=0; card<MAX_CARDS; card++) {
		dev = dsp_dev + card;
		if(dev->pci == NULL) break;
	} 	
	if (dev->pci != NULL) {
		PRINT_ERR(SUBNAME "too many cards, dsp_dev[] is full.\n");
		return -EPERM;
	}

        // Initialize device structure
	memset(dev, 0, sizeof(*dev));
	dev->pci = pci;

	tasklet_init(&dev->handshake_tasklet,
		     dsp_ack_int_or_schedule, (unsigned long)dev);
	spin_lock_init(&dev->lock);
/* 	init_MUTEX(&dev->sem); */
	init_MUTEX(&dev->local.sem);
	init_waitqueue_head(&dev->local.queue);

	init_timer(&dev->tim_dsp);
	dev->tim_dsp.function = dsp_timeout;
	dev->tim_dsp.data = (unsigned long)dev;
	dev->state = DDAT_IDLE;

	// PCI paperwork
	err = pci_enable_device(dev->pci);
	if (err) goto fail;
	if (pci_request_regions(dev->pci, DEVICE_NAME)!=0) {
		PRINT_ERR(SUBNAME "pci_request_regions failed.\n");
		err = -1;
		goto fail;
	}
	dev->dsp = (dsp_reg_t *)ioremap_nocache(pci_resource_start(dev->pci, 0) & 
						PCI_BASE_ADDRESS_MEM_MASK,
						sizeof(*dev->dsp));
	if (dev->dsp==NULL) {
		PRINT_ERR(SUBNAME "Could not map PCI registers!\n");
		pci_release_regions(dev->pci);			
		err = -EIO;
		goto fail;
	}
	pci_set_master(dev->pci);

	/* Card configuration - now we're done with the kernel and
	 * talk to the card */

	// Clear any outstanding interrupts
	dsp_clear_interrupt(dev->dsp);

	// Set the mode of the data path for reads (24->32 conversion)
	dev->comm_mode = DSP_PCI_MODE;
#ifdef NO_INTERRUPTS
	dev->comm_mode |= DSP_PCI_MODE_NOIRQ;
#endif /* NO_INTERRUPTS */
	dsp_write_hctr(dev->dsp, dev->comm_mode);

	// Install interrupt handler or polling timer
	dev->int_handler = NULL;
	if (dev->comm_mode & DSP_PCI_MODE_NOIRQ) {
		// Create timer for soft poll interrupt generation
		init_timer(&dev->tim_poll);
		dev->tim_poll.function = dsp_timer_function;
		dev->tim_poll.data = (unsigned long)dev;
		mod_timer(&dev->tim_poll, jiffies + DSP_POLL_JIFFIES);
	} else {
		// Install the interrupt handler (cast necessary for backward compat.)
		err = dsp_pci_set_handler(card, (irq_handler_t)pci_int_handler,
					  "mce_dsp");		
		if (err) goto fail;
	}

	// Assign handlers for REP and HEY interrupts.  These are for
	// DSP communications (rather than the MCE protocol).
	dsp_set_msg_handler(DSP_REP, dsp_reply_handler, (unsigned long)dev, card);
	dsp_set_msg_handler(DSP_HEY, dsp_hey_handler, (unsigned long)dev, card);

	PRINT_INFO(SUBNAME "ok\n");
	return 0;

fail:
	PRINT_ERR(SUBNAME "failed!\n");
	return err;
}
#undef SUBNAME


#define SUBNAME "dsp_unconfigure: "
int dsp_unconfigure(int card)
{
	struct dsp_dev_t *dev = dsp_dev + card;

	// Remove int handler or poll timer
	if (dev->comm_mode & DSP_PCI_MODE_NOIRQ) {
		del_timer_sync(&dev->tim_poll);
	} else {
		dsp_pci_remove_handler(dev);
	}
		
	if (dev->dsp!=NULL) {
		// Clear any outstanding interrupts from the card
/* 		dsp_clear_interrupt(dev->dsp); */

		// PCI un-paperwork
		iounmap(dev->dsp);
		dev->dsp = NULL;
	}

	if (dev->pci != NULL) {
		pci_disable_device(dev->pci);
		pci_release_regions(dev->pci);
		dev->pci = NULL;
	}

	return card;
}
#undef SUBNAME

#define SUBNAME "dsp_driver_remove: "
void dsp_driver_remove(struct pci_dev *pci)
{
	int card;
	struct dsp_dev_t *dev;

	PRINT_INFO(SUBNAME "entry\n");
	if (pci == NULL) {
		PRINT_ERR(SUBNAME "called with null pointer, ignoring.\n");
		return;
	}

	// Match to existing card
	for (card=0; card < MAX_CARDS; card++) {
		dev = dsp_dev + card;
		if (pci == dev->pci)
			break;
	}
	if (card >= MAX_CARDS) {
		PRINT_ERR(SUBNAME "could not match configured device, ignoring.\n");
		return;
	}
			
	// Disable higher-level features first
	mce_remove(card);
	del_timer_sync(&dev->tim_dsp);

	// Hopefully this isn't still running...
	tasklet_kill(&dev->handshake_tasklet);

	// Revert card to default mode
	dsp_write_hctr(dev->dsp, DSP_PCI_MODE);

	// Do DSP cleanup, free PCI resources
	dsp_unconfigure(card);

	PRINT_INFO(SUBNAME "ok\n");
}
#undef SUBNAME

/*
  dsp_driver_probe

  Called by kernel's PCI manager with each PCI device.  We first find
  a place to keep the card, then do the PCI level initialization

*/

#define SUBNAME "dsp_driver_probe: "
int dsp_driver_probe(struct pci_dev *pci, const struct pci_device_id *id)
{
	int card;
	struct dsp_dev_t *dev = NULL;
	PRINT_INFO(SUBNAME "entry\n");

	// Setup data structure for the card, configure PCI stuff and
	// the DSP.  After this call, the DSP is ready to go.
	if ((card = dsp_configure(pci)) < 0)
		goto fail;
	dev = dsp_dev + card;

	// Get DSP version, which MCE driver likes to know...
	if (dsp_query_version(card) != 0)
		goto fail;

	// Enable interrupt hand-shaking for newer firmware
	if (dev->version >= DSP_U0105) {
		dev->comm_mode |= DSP_PCI_MODE_HANDSHAKE;
		dsp_write_hctr(dev->dsp, dev->comm_mode);
	}

	// Enable the character device for this card.
	if(dsp_ops_probe(card) != 0)
		goto fail;

	// DSP is ready, setup a structure for MCE driver
	if (mce_probe(card, dsp_dev[card].version))
		goto fail;

	PRINT_INFO(SUBNAME "ok\n");
	return 0;

fail:
	PRINT_ERR(SUBNAME "failed, calling removal routine.\n");
	dsp_driver_remove(pci);
	return -1;
}
#undef SUBNAME


#define SUBNAME "cleanup_module: "
void dsp_driver_cleanup(void)
{
	int i = 0;

	PRINT_INFO(SUBNAME "entry\n");

#ifdef FAKEMCE
	dsp_driver_remove();
	dsp_fake_cleanup();
#else
	pci_unregister_driver(&pci_driver);

	for(i=0; i < MAX_CARDS; i++) {
		struct dsp_dev_t *dev = dsp_dev + i;
		if ( dev->pci != NULL ) {
			PRINT_ERR(SUBNAME "failed to zero dev->pci for card; %i!\n", i);
		}
	}
#endif
	dsp_ops_cleanup();

	mce_cleanup();

	remove_proc_entry("mce_dsp", NULL);

	PRINT_INFO(SUBNAME "driver removed\n");
}    
#undef SUBNAME


#define SUBNAME "init_module: "
inline int dsp_driver_init(void)
{
	int i = 0;
	int err = 0;

	PRINT_INFO(SUBNAME "driver init...\n");
	for(i=0; i<MAX_CARDS; i++) {
		struct dsp_dev_t *dev = dsp_dev + i;
		memset(dev, 0, sizeof(*dev));
	}
  
	create_proc_read_entry("mce_dsp", 0, NULL, read_proc, NULL);

	err = dsp_ops_init();
	if(err != 0) goto out;

	err = mce_init();
	if(err != 0) goto out;

#ifdef FAKEMCE
	dsp_fake_init( DSPDEV_NAME );
#else
	err = pci_register_driver(&pci_driver);

	if (err) {
		PRINT_ERR(SUBNAME "pci_register_driver failed with code %i.\n", err);
		err = -1;
		goto out;
	}			  
#endif //FAKEMCE

	PRINT_INFO(SUBNAME "ok\n");
	return 0;
out:
	PRINT_ERR(SUBNAME "exiting with error\n");
	return err;
}

module_init(dsp_driver_init);
module_exit(dsp_driver_cleanup);

#undef SUBNAME
