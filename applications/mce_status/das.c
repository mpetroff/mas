#include <stdlib.h>
#include <string.h>
#include "mce_status.h"
#include "das.h"

typedef struct {
	FILE *out;
	options_t *options;
	int error_count;
	int echo_only;
} das_t;

int  das_init(unsigned user_data, const options_t *options);
int  das_item(unsigned user_data, const mce_param_t *p);
int  das_cleanup(unsigned user_data);

int das_crawler(options_t *options, crawler_t *crawler)
{
	das_t *das = (das_t*)malloc(sizeof(das_t));
	if (das==NULL) {
		fprintf(stderr, "Memory allocation error\n");
		return -1;
	}
	memset(das, 0, sizeof(*das));
	das->options = options;
	das->echo_only = 0;

	crawler->init = das_init;
	crawler->cleanup = das_cleanup;
	crawler->item = das_item;

	crawler->user_data = (unsigned) das;

	return 0;	
}

int  das_init(unsigned user_data, const options_t *options)
{
	das_t *das = (das_t*)user_data;
	das->out = stdout;
	
	if (das->options->output_on && 
	    (das->out=fopen(das->options->output_file, "w")) == NULL) {
		fprintf(stderr, "DAS mcestatus could not open '%s' for output.\n",
			das->options->output_file);
		return -1;
	}
		
	fprintf(das->out, "<HEADER>\n");
	return 0;
}

int das_cleanup(unsigned user_data)
{
	das_t *das = (das_t*) user_data;
	fprintf(das->out, "</HEADER>\n");
	free(das);
	return 0;
}

int  das_item(unsigned user_data, const mce_param_t *p)
{
	das_t *das = (das_t*) user_data;
	u32 buf[MCE_REP_DATA_MAX];

	if (!(p->card.flags & MCE_PARAM_STAT) ||
	    !(p->param.flags & MCE_PARAM_STAT))
		return 0;
	
	if (p->param.type != MCE_CMD_MEM)
		return 0;

	fprintf(das->out, "<RB %s %s>", p->card.name, p->param.name);
	if (das->echo_only) {
		
		fprintf(das->out, "\n");
		return 0;
	}

	// Read some data
 	int err = mce_read_block(das->options->handle, p, p->param.count, buf);
	if ( err ) {
		fprintf(das->out, " ERROR");
		das->error_count++;
	} else {
		int i;
		for (i=0; i < p->param.count*p->card.card_count; i++) {
			fprintf(das->out, " %08i", buf[i]);
		}
	}
	
	fprintf(das->out, "\n");

	return 0;
}
