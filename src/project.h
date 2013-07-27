#ifndef PROJECT_H_
#define PROJECT_H_

#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

#include "msg_callback.h"

#define BARK fprintf(stdout, "!! BARK !! %s : %s:%d\n", __PRETTY_FUNCTION__, \
		     __FILE__, __LINE__)

struct hash_table_;

typedef struct completion_project__ {
  char **src_filenames;
  struct hash_table_ *cxunfile_ht;
  CXTranslationUnit *tunits;
  CXIndex index;
  ssize_t active_tunit;
  ssize_t src_count;
  char parsed;
  int arg_count;
  char **args;
  
} completion_Project;

void project_locate(completion_Project *prj, int line, int column);

#endif /* PROJECT_H_ */
