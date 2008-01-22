#include <stdlib.h>
#include <string.h>
#include "mas_param.h"
#include "idl.h"

typedef struct {
	FILE *out;
	options_t *options;
	int error_count;
	int echo_only;
	int pass;
	char *function_suffix;
} idl_t;

#define IDL_WARNING \
"; This file is automatically generated by mas_param!\n\n"

#define STR_FLAT \
"function str_flat,a\n"\
"    s = ''\n"\
"    for i=0,n_elements(a)-1 do begin\n"\
"        s=s+' '+strcompress(string(a(i)))\n"\
"    endfor\n"\
"    return,s\n"\
"end\n\n"

#define LOAD_INT \
"function mas_load_int,filename,key\n"\
"    spawn,'mas_param -s '+filename+' get '+key,r,exit_status=status\n"\
"    if status eq 0 then return, fix(strsplit(r,/extract))\n"\
"    print,'Failed to load parameter '+key\n"\
"    return,0\n"\
"end\n\n"\

#define LOAD_FLOAT \
"function mas_load_float,filename,key\n"\
"    spawn,'mas_param -s '+filename+' get '+key,r,exit_status=status\n"\
"    if status eq 0 then return, float(strsplit(r,/extract))\n"\
"    print,'Failed to load parameter '+key\n"\
"    return,0\n"\
"end\n\n"\

#define LOAD_STRING \
"function mas_load_string,filename,key\n"\
"    spawn,'mas_param -s '+filename+' get '+key,r,exit_status=status\n"\
"    if status eq 0 then return,r\n"\
"    print,'Failed to load parameter '+key\n"\
"    return,0\n"\
"end\n\n"\

int  idl_init(unsigned user_data, const options_t *options);
int  idl_item(unsigned user_data, const mas_param_t *p);
int  idl_cleanup(unsigned user_data);

int idl_crawler(options_t *options, crawler_t *crawler)
{
	idl_t *idl = (idl_t*)malloc(sizeof(idl_t));
	if (idl==NULL) {
		fprintf(stderr, "Memory allocation error\n");
		return -1;
	}
	memset(idl, 0, sizeof(*idl));
	idl->options = options;
	idl->echo_only = 0;
	idl->pass = -1;
	idl->function_suffix = options->param_name;

	crawler->init = idl_init;
	crawler->cleanup = idl_cleanup;
	crawler->item = idl_item;

	crawler->passes = 2;
	crawler->user_data = (unsigned) idl;

	return 0;	
}

int  idl_init(unsigned user_data, const options_t *options)
{
	idl_t *idl = (idl_t*)user_data;
	idl->out = stdout;
	
	if (idl->pass < 0) {
		if (idl->options->output_on && 
		    (idl->out=fopen(idl->options->output_file, "w")) == NULL) {
			fprintf(stderr, "mceparam could not open '%s' for output.\n",
				idl->options->output_file);
			return -1;
		}
	}
	idl->pass++;
	
	switch (idl->pass) {

	case 0:
		fprintf(idl->out, IDL_WARNING STR_FLAT LOAD_INT LOAD_FLOAT LOAD_STRING);
		fprintf(idl->out,
			"pro save_%s,m,filename\n",
			idl->function_suffix );
		break;
	case 1:
		fprintf(idl->out,
			"pro load_%s,filename,m\n"
			"    m = create_struct('_source',filename",
			idl->function_suffix );
		break;
	}
	
	return 0;
}

int idl_cleanup(unsigned user_data)
{
	idl_t *idl = (idl_t*) user_data;
	switch (idl->pass) {
	case 0:
		fprintf(idl->out, "end\n\n\n");
		break;
	case 1:
		fprintf(idl->out, "    )\n"
			"end\n");
		free(idl);
	}

	return 0;
}

int idl_item_load(idl_t *idl, const mas_param_t *p)
{
	switch (p->type) {
	case CFG_STR:
		fprintf(idl->out, ",  $\n"
			"        '%s',mas_load_string(filename,'%s')",
			p->data_name, p->data_name);
		return 0;
	case CFG_DBL:
		fprintf(idl->out, ",  $\n"
			"        '%s',mas_load_float(filename,'%s')",
			p->data_name, p->data_name);
		return 0;
	case CFG_INT:
		fprintf(idl->out, ",  $\n"
			"        '%s',mas_load_int(filename,'%s')",
			p->data_name, p->data_name);
		return 0;
	}

	return -1;
}

int idl_item_save(idl_t *idl, const mas_param_t *p)
{
	switch(p->type) {

	case CFG_STR:
		fprintf(idl->out,
			"    spawn,'mas_param -s '+filename+' set %s \"'+str_flat(m.%s)+'\"'\n",
			p->data_name, p->data_name);
		break;

	default:
		fprintf(idl->out,
			"    spawn,'mas_param -s '+filename+' set %s '+str_flat(m.%s)\n",
			p->data_name, p->data_name);

	}
	return 0;
}

int  idl_item(unsigned user_data, const mas_param_t *p)
{
	idl_t *idl = (idl_t*) user_data;

	switch(idl->pass) {
	case 0:
		return idl_item_save(idl, p);
	case 1:
		return idl_item_load(idl, p);
	}
	return -1;
}
