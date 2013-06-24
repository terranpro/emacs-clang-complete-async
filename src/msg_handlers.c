#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include "msg_callback.h"


/* discard all remaining contents on this line, jump to the beginning of the
   next line */
static void __skip_the_rest(FILE *fp)
{
    char crlf[LINE_MAX];
    fgets(crlf, LINE_MAX, fp);
}

/* read len bytes from fp to buffer */
static void __read_n_bytes(FILE *fp, char *buffer, int len)
{
    while (len-- != 0) {
        *buffer++ = (char)fgetc(fp);
    }
}


/* Read the source file portion of the message to the source code buffer in 
   specified session object. 

   Sourcefile segment always starts with source_length: [#len#], followed by a
   newline character, then the actual source code. Length of the source code is
   indicated by [#len#] so we know how much bytes we should read from fp.
*/
static void completion_readSourcefile(completion_Session *session, FILE *fp)
{
    int source_length;
    fscanf(fp, "source_length:%d", &source_length);
    __skip_the_rest(fp);

    if (source_length >= session->buffer_capacity) /* we're running out of space */
    {
        /* expand the buffer two-fold of source size */
        session->buffer_capacity = source_length * 2;
        session->src_buffer = 
            (char*)realloc(session->src_buffer, session->buffer_capacity);
    }

    /* read source code from fp to buffer */
    session->src_length = source_length;
    __read_n_bytes(fp, session->src_buffer, source_length);
}


/* Read completion request (where to complete at and current source code) from message 
   header and calculate completion candidates.

   Message format:
       row: [#row_number#]
       column: [#column_number#]
       source_length: [#src_length#]
       <# SOURCE CODE #>
*/
void completion_doCompletion(completion_Session *session, FILE *fp)
{
    CXCodeCompleteResults *res;

    /* get where to complete at */
    int row, column;
    char prefix[512];
    fscanf(fp, "row:%d",    &row);    __skip_the_rest(fp);
    fscanf(fp, "column:%d", &column); __skip_the_rest(fp);
    fscanf(fp, "prefix:"); 
    fgets(prefix, sizeof(prefix), fp);
    //__skip_the_rest(fp);

    /* get a copy of fresh source file */
    completion_readSourcefile(session, fp);

    /* calculate and show code completions results */
    res = completion_codeCompleteAt(session, row, column);
    clang_sortCodeCompletionResults(res->Results, res->NumResults);

    int len = strlen(prefix);
    if (prefix[len-1] == '\n')
      prefix[len-1] = '\0';

    //fprintf(stdout, "COMPLETION: %s : [#%d#]\n", prefix, strlen(prefix));
    /* if (strcmp(prefix, "") == 0 || prefix[0] == '\0') { */
    /*   prefix[0] = '$'; */
    /*   prefix[1] = '\0'; */
    /* } */

    /* fprintf(stderr, "code completion results: %d\n", res->NumResults); */
    completion_printCodeCompletionResults(res, stdout, prefix);

    //fprintf(stdout, "COMPLETION: %s\n", prefix);
    /* fprintf(stdout, "COMPLETION: before: %lu\n", res->NumResults); */
    /* fprintf(stdout, "COMPLETION: after: %lu\n", clang_codeCompleteGetContexts(res->Results)); */
    /* fprintf(stdout, "COMPLETION: %d\n", */
    /* 	    clang_codeCompleteGetContainerKind(res, NULL)); */

    fprintf(stdout, "\n$"); fflush(stdout);    /* we need to inform emacs that all 
                                                candidates has already been sent */
    clang_disposeCodeCompleteResults(res);
}


/* Reparse the source code to refresh the translation unit */
void completion_doReparse(completion_Session *session, FILE *fp)
{
    (void) fp;     /* get rid of unused parameter warning */
    completion_reparseTranslationUnit(session);
}

/* Update source code in src_buffer */
void completion_doSourcefile(completion_Session *session, FILE *fp)
{
    (void) fp;     /* get rid of unused parameter warning  */
    completion_readSourcefile(session, fp);
}


/* dispose command line arguments of session */
static void completion_freeCmdlineArgs(completion_Session *session)
{
    /* free each command line arguments */
    int i_arg = 0;
    for ( ; i_arg < session->num_args; i_arg++) {
        free(session->cmdline_args[i_arg]);
    }

    /* and dispose the arg vector */
    free(session->cmdline_args);
}

/* Update command line arguments passing to clang translation unit. Format
   of the coming CMDLINEARGS message is as follows:
   
       num_args: [#n_args#]
       arg1 arg2 ... (there should be n_args items here)
*/
void completion_doCmdlineArgs(completion_Session *session, FILE *fp)
{
    int i_arg = 0;
    char arg[LINE_MAX];

    /* destroy command line args, and we will rebuild it later */
    completion_freeCmdlineArgs(session);

    /* get number of arguments */
    fscanf(fp, "num_args:%d", &(session->num_args)); __skip_the_rest(fp);
    session->cmdline_args = (char**)calloc(sizeof(char*), session->num_args);

    /* rebuild command line arguments vector according to the message */
    for ( ; i_arg < session->num_args; i_arg++)
    {
        /* fetch an argument from message */
        fscanf(fp, "%s", arg);

        /* and add it to cmdline_args */
        session->cmdline_args[i_arg] = (char*)calloc(sizeof(char), strlen(arg) + 1);
        strcpy(session->cmdline_args[i_arg], arg);
    }

    /* we have to rebuild our translation units to make these cmdline args changes 
       take place */
    clang_disposeTranslationUnit(session->cx_tu);
    completion_parseTranslationUnit(session);
    completion_reparseTranslationUnit(session);  /* dump PCH for acceleration */
}

/* Update command line arguments and source files passing to clang translation unit. Format
   of the coming FILECHANGED message is as follows:
       filename: [#new filename#]  
       num_args: [#n_args#]
       arg1 arg2 ... (there should be n_args items here)
*/
void completion_doFileChanged(completion_Session *session, FILE *fp)
{
    int i_arg = 0;
    char arg[LINE_MAX];

    /* destroy command line args, and we will rebuild it later */
    completion_freeCmdlineArgs(session);

    char filename[LINE_MAX];
    fscanf(fp, "filename:%s", filename); __skip_the_rest(fp);
    session->src_filename = strdup(filename);
    session->src_length = 0;      /* we haven't read any source code yet. */
    session->buffer_capacity = INITIAL_SRC_BUFFER_SIZE;
    free(session->src_buffer);
    session->src_buffer = (char*)calloc(sizeof(char), session->buffer_capacity);

    /* get number of arguments */
    fscanf(fp, "num_args:%d", &(session->num_args)); __skip_the_rest(fp);
    session->cmdline_args = (char**)calloc(sizeof(char*), session->num_args);

    /* rebuild command line arguments vector according to the message */
    for ( ; i_arg < session->num_args; i_arg++)
    {
        /* fetch an argument from message */
        fscanf(fp, "%s", arg);

        /* and add it to cmdline_args */
        session->cmdline_args[i_arg] = (char*)calloc(sizeof(char), strlen(arg) + 1);
        strcpy(session->cmdline_args[i_arg], arg);
    }
    
    /* we have to rebuild our translation units to make these cmdline args changes 
       take place */
    clang_disposeTranslationUnit(session->cx_tu);
    completion_parseTranslationUnit(session);
    completion_reparseTranslationUnit(session);  /* dump PCH for acceleration */
}

/* Handle syntax checking request, message format:
       source_length: [#src_length#]
       <# SOURCE CODE #>
*/
void completion_doSyntaxCheck(completion_Session *session, FILE *fp)
{
    unsigned int i_diag = 0, n_diag;
    CXDiagnostic diag;
    CXString     dmsg;

    /* get a copy of fresh source file */
    completion_readSourcefile(session, fp);

    /* reparse the source to retrieve diagnostic message */
    completion_reparseTranslationUnit(session);

    /* dump all diagnostic messages to fp */
    n_diag = clang_getNumDiagnostics(session->cx_tu);
    for ( ; i_diag < n_diag; i_diag++)
    {
        diag = clang_getDiagnostic(session->cx_tu, i_diag);
        dmsg = clang_formatDiagnostic(diag, clang_defaultDiagnosticDisplayOptions());
        fprintf(stdout, "%s\n", clang_getCString(dmsg));
        clang_disposeString(dmsg);
        clang_disposeDiagnostic(diag);
    }

    fprintf(stdout, "$"); fflush(stdout);    /* end of output */
}

static completion_Project projects[64];
static int next_project = 0;

void projectNew(completion_Project *prj)
{
  prj->index = clang_createIndex(0, 0);
  prj->parsed = 0;
  prj->active_tunit = -1;
  prj->session = NULL;
  prj->tunits = NULL;
  prj->src_count = -1;
  prj->src_filenames = NULL;
}

void projectOptions(completion_Project *prj, int argc, char **argv)
{
  prj->arg_count = argc;
  prj->args = argv;
}

void projectAdd(completion_Project *prj, char const *src_file)
{
  if ( prj->src_count < 0 || prj->src_count % 4 == 0 ) {
    ++prj->src_count;
    char **old_srcs = prj->src_filenames;
    prj->src_filenames = 
      (char **) realloc( prj->src_filenames,
			       sizeof(char *) * prj->src_count * 4);

    CXTranslationUnit *old_tunits = prj->tunits;
    prj->tunits = 
      (CXTranslationUnit *)realloc(prj->tunits, 
				   sizeof(CXTranslationUnit *) *
				   prj->src_count * 4 );

    if ( old_srcs != NULL) {
      const char *src = old_srcs[0];
      char *dst = prj->src_filenames[0];

      CXTranslationUnit *srctu = &old_tunits[0];
      CXTranslationUnit *dsttu = &prj->tunits[0];

      while( src ) {
	*dst = *src;
	++dst;
	++src;
	(*dsttu) = (*srctu);
	++dsttu;
	++srctu;
      }

      dst = NULL;
      dsttu = NULL;
    }

  }

  prj->src_filenames[ prj->src_count ] = strdup( src_file );
  
  prj->tunits[ prj->src_count ] = 
    clang_parseTranslationUnit(prj->index, 
			       src_file, prj->args, prj->arg_count, 
			       NULL, 0, DEFAULT_COMPLETEAT_OPTIONS);

  prj->src_count++;
}

void completion_doProject(completion_Session *session, FILE *fp)
{
  int id;
  char subcmd[512];
  fgets(subcmd, sizeof(subcmd), fp);
  fprintf(stdout, "SUBCMD = %s\n", subcmd);
  if ( subcmd[ strlen(subcmd) - 1 ] == '\n' )
    subcmd[ strlen(subcmd) - 1 ] = '\0';

  if ( strcmp( subcmd, "ADD_SRC" ) == 0 )
  {
    fscanf(fp, "PROJECTID:%d", &id); __skip_the_rest(fp);
    fgets(subcmd, sizeof(subcmd), fp);
    projectAdd( &projects[ id ], subcmd );
  }
  else if ( strcmp(subcmd, "NEW") == 0 ) {
    projectNew( &projects[next_project] );
    fprintf(stdout, "PROJECTID:%d\n", next_project++);
  }
  else if ( strcmp(subcmd, "OPTIONS") == 0 ) {
    fscanf(fp, "PROJECTID:%d", &id); __skip_the_rest(fp);
    fgets(subcmd, sizeof(subcmd), fp);
    fprintf(stdout, "OPTIONS = %s\n", subcmd);
    int argc;
    char **argv = (char **)malloc( sizeof(char *) * 1024 );
    size_t curarg_index = 0;
    char *curarg;
    char *startstr = subcmd;

    while( (curarg = strtok( startstr, " " )) != NULL ) {
      startstr = NULL;
      fprintf(stdout, "curarg = %s\n", curarg);
      argv[curarg_index++] = curarg;
    }

    argc = curarg_index + 1;
    projectOptions( &projects[ id ], argc, argv );
  }
  else {

  }

  fprintf(stdout, "$"); 
  fflush(stdout);

}

/* Locate the definition (or declaration?!) of tag */
void completion_doLocate(completion_Session *session, FILE *fp)
{
    int row, column;
    char prefix[512];

    fscanf(fp, "row:%d",    &row);    __skip_the_rest(fp);
    fscanf(fp, "column:%d", &column); __skip_the_rest(fp);
    fscanf(fp, "prefix:"); 
    fgets(prefix, sizeof(prefix), fp);

    /* get a copy of fresh source file */
    completion_readSourcefile(session, fp);
    completion_reparseTranslationUnit(session);

    LocationResult loc = completion_locateAt(session, row, column);
    CXString cxfstr = clang_getFileName( loc.file );
    fprintf(stdout, "LOCATE:\n");
    fprintf(stdout, "file:%s\n", clang_getCString( cxfstr ));
    fprintf(stdout, "line:%d\n", loc.line);
    fprintf(stdout, "column:%d\n", loc.column);

    fprintf(stdout, "$"); 
    fflush(stdout);

    clang_disposeString( cxfstr );
}

/* When emacs buffer is killed, a SHUTDOWN message is sent automatically by a hook 
   function to inform the completion server (this program) to terminate. */
void completion_doShutdown(completion_Session *session, FILE *fp)
{
    (void) fp;    /* this parameter is unused, the server will shutdown
                   * directly without sending any messages to its client */

    /* free clang parser infrastructures */
    clang_disposeTranslationUnit(session->cx_tu);
    clang_disposeIndex(session->cx_index);

    /* free session properties */
    completion_freeCmdlineArgs(session);
    free(session->src_buffer);

    exit(0);   /* terminate completion process */
}






