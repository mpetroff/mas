

#include <linux/proc_fs.h>

#include "mce_options.h"
#include "data.h"
#include "mce_driver.h"
#include "dsp_driver.h"
#include "version.h"

#ifdef FAKEMCE
#  include <dsp_fake.h>
#else
#  include "dsp_pci.h"
#endif

int read_proc(char *buf, char **start, off_t offset, int count, int *eof,
	      void *data)
{
	int i = 0;
	int len = 0;
	int limit = count - 80;

	if (len < limit) {
		len += sprintf(buf+len,"\nmce_dsp driver version %s\n",
			       VERSION_STRING);
		len += sprintf(buf+len,"    fakemce:  "
#ifdef FAKEMCE
			       "yes\n"
#else
			       "no\n"
#endif
			);
		len += sprintf(buf+len,"    realtime: "
#ifdef REALTIME
			       "yes\n"
#else
			       "no\n"
#endif
			);
		len += sprintf(buf+len,"    bigphys:  "
#ifdef BIGPHYS
			       "yes\n"
#else
			       "no\n"
#endif
			);
	}

	for(i=0; i<MAX_CARDS; i++) {
	  
	  if (len < limit) {
	    len += sprintf(buf+len,"\nCARD: %d\n\n", i);
	  }

		PRINT_ERR("proc: i=%d\n", i);
		if (len < limit) {
		  len += sprintf(buf+len,"  data buffer:\n");
		  len += data_proc(buf+len, limit-len, i);
		}
		if (len < limit) {
		  len += sprintf(buf+len,"  mce commander:\n");
		  len += mce_proc(buf+len, limit-len, i);
		}
		if (len < limit) {
	          len += sprintf(buf+len,"  dsp commander:\n");
	          len += dsp_proc(buf+len, limit-len, i);
		}
		if (len < limit) {
	          len += sprintf(buf+len,"  dsp pci registers:\n");
	          len += dsp_pci_proc(buf+len, limit-len, i);
		}
		*eof = 1;
		
	}

	if (len < limit) {
	  len += sprintf(buf+len,"\n");
	}

	return len;
}
