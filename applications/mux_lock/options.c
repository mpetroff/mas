/* -*- mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *      vim: sw=4 ts=4 et tw=80
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <mce/defaults.h>

#include "options.h"
#include "../../defaults/config.h"

/*
  This is for MAS configuration file specification, mostly.  Since
  other arguments are processed by the main routines, the function
  must return the offset of the first unprocessed argument.
*/

#if MAX_FIBRE_CARD == 1
#  define USAGE_OPTION_N "        -n <card number>       ignored\n"
#else
#  define USAGE_OPTION_N \
  "        -n <card number>       use the specified fibre card\n"
#endif

#define USAGE_MESSAGE "" \
"  Initial options (MAS config):\n" \
USAGE_OPTION_N \
"        -w <hardware file>      override default hardware configuration file\n"\
"        -m <MAS config file>    override default MAS configuration file\n"\
"        -s <experiment file>    override default experiment configuration file\n"\
"        -p <steps>              enable preservoing for some number of steps\n"\
"        -E [0|1]                force old/new semantics\n"\
""

void usage()
{
      printf(USAGE_MESSAGE);
}

int process_options(option_t *options, int argc, char **argv)
{
#if MULTICARD
  char *s;
#endif
  int option;
  // Note the "+" at the beginning of the options string forces the
  // first non-option string to return a -1, which is what we want.
  while ( (option = getopt(argc, argv, "+?hm:n:w:p:s:E:0123456789")) >=0) {

    switch(option) {
    case '?':
    case 'h':
      usage();
      return -1;

    case 'm':
      if (options->config_file)
          free(options->config_file);
      options->config_file = strdup(optarg);
      break;

    case 'n':
#if MULTICARD
      options->fibre_card = (int)strtol(optarg, &s, 10);
      if (*optarg == '\0' || *s != '\0' || options->fibre_card < 0) {
          fprintf(stderr, "%s: invalid fibre card number\n", argv[0]);
          return -1;
      }
#endif
    break;

  case 'w':
    if (options->hardware_file)
        free(options->hardware_file);
    options->hardware_file = strdup(optarg);
    break;

  case 'p':
    options->preservo = atoi(optarg);
    break;

  case 's':
    if (options->experiment_file)
        free(options->experiment_file);
    options->experiment_file = strdup(optarg);
    break;

  case 'E':
    options->argument_opts = !(atoi(optarg));
    break;

  case '0' ... '9':
    //It's a number! Get out of here!
    optind--;
    break;

  default:
    printf("Unimplemented option '-%c'!\n", option);
    }
  }

    /* set file defaults */
  if ((options->data_device = mcelib_data_device(options->fibre_card)) == NULL) {
      fprintf(stderr, "Unable to obtain path to default data device!\n");
      return -1;
  }
  if ((options->cmd_device = mcelib_cmd_device(options->fibre_card)) == NULL) {
      fprintf(stderr, "Unable to obtain path to default command device!\n");
      return -1;
  }
  if (options->hardware_file == NULL) {
      options->hardware_file = mcelib_default_hardwarefile(options->fibre_card);
      if (options->hardware_file == NULL) {
          fprintf(stderr, "Unable to obtain path to default mce.cfg!\n");
          return -1;
      }
  }
  if (options->experiment_file == NULL) {
      options->experiment_file = mcelib_default_experimentfile(options->fibre_card);
      if (options->experiment_file == NULL) {
          fprintf(stderr, "Unable to obtain path to default experiment.cfg!\n");
          return -1;
      }
  }
  if (options->config_file == NULL) {
      options->config_file = mcelib_default_masfile();
      if (options->config_file == NULL) {
          fprintf(stderr, "Unable to obtain path to default mas.cfg!\n");
          return -1;
      }
  }

  return optind;
}
