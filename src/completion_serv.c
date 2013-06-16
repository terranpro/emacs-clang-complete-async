#include <stdlib.h>
#include <string.h>
#include "completion.h"



/* Copy command line parameters (except source filename) to cmdline_args  */
static void __copy_cmdlineArgs(int argc, char *argv[], completion_Session *session)
{
    int i_arg = 0;
    session->num_args = argc - 2;  /* argv[0] and argv[argc - 2] should be discarded */
    session->cmdline_args = (char**)calloc(sizeof(char*), session->num_args);

    /* copy argv[1..argc-1] to cmdline_args */
    for ( ; i_arg < session->num_args; i_arg++)
    {
        session->cmdline_args[i_arg] = 
            (char*)calloc(sizeof(char), strlen(argv[i_arg + 1]) + 1);

        strcpy(session->cmdline_args[i_arg], argv[i_arg + 1]);
    }
}

/* Initialize basic information for completion, such as source filename, initial source 
   buffer and command line arguments for clang */
void 
__initialize_completionSession(int argc, char *argv[], completion_Session *session)
{
    /* filename shall be the last parameter */
    session->src_filename = argv[argc - 1];
    session->src_length = 0;      /* we haven't read any source code yet. */
    session->buffer_capacity = INITIAL_SRC_BUFFER_SIZE;
    session->src_buffer = (char*)calloc(sizeof(char), session->buffer_capacity);

    __copy_cmdlineArgs(argc, argv, session);
}


/* Initialize session object and launch the completion server, preparse the source file and 
   build the AST for furture code completion requests  
*/
void startup_completionSession(int argc, char *argv[], completion_Session *session)
{
    __initialize_completionSession(argc, argv, session);

    /* default parameters */
    session->ParseOptions      = DEFAULT_PARSE_OPTIONS;
    session->CompleteAtOptions = DEFAULT_COMPLETEAT_OPTIONS;

    session->cx_index = clang_createIndex(0, 0);
    completion_parseTranslationUnit(session);
    completion_reparseTranslationUnit(session);
}


/* Simple wrappers for clang parser functions */

static struct CXUnsavedFile __get_CXUnsavedFile(const completion_Session *session)
{
    struct CXUnsavedFile unsaved_files;
    unsaved_files.Filename = session->src_filename;
    unsaved_files.Contents = session->src_buffer;
    unsaved_files.Length   = session->src_length;

    return unsaved_files;
}

CXTranslationUnit 
completion_parseTranslationUnit(completion_Session *session)
{
    struct CXUnsavedFile unsaved_files = __get_CXUnsavedFile(session);
    session->cx_tu = 
        clang_parseTranslationUnit(
            session->cx_index, session->src_filename, 
            (const char * const *) session->cmdline_args, session->num_args, 
            &unsaved_files, 1,
            session->ParseOptions);

    return session->cx_tu;
}

int completion_reparseTranslationUnit(completion_Session *session)
{
    struct CXUnsavedFile unsaved_files = __get_CXUnsavedFile(session);
    return 
        clang_reparseTranslationUnit(
            session->cx_tu, 1, &unsaved_files, session->ParseOptions);
}

CXCodeCompleteResults* 
completion_codeCompleteAt(
    completion_Session *session, int line, int column)
{
    struct CXUnsavedFile unsaved_files = __get_CXUnsavedFile(session);
    return 
        clang_codeCompleteAt(
            session->cx_tu, session->src_filename, line, column, 
            &unsaved_files, 1, session->CompleteAtOptions);
}

int myvisitor(CXCursor c, CXCursor p, CXClientData d)
{
  printf("CursorKind\t%s\n",
	 clang_getCString(clang_getCursorKindSpelling(c.kind)));
  if ( c.kind == CXCursor_ClassTemplate ) {
    c = clang_getCursorReferenced(c);
    printf("CursorKind\t%s\n",
	   clang_getCString(clang_getCursorKindSpelling(c.kind)));

      CXSourceLocation loc;
      loc = clang_getCursorLocation( c );
      CXFile file;
      unsigned line;
      unsigned col;

      clang_getSpellingLocation( loc, &file, &line, &col, NULL );
      printf("File %s Line %d Col %d\n",
	     clang_getCString( clang_getFileName(file) ), line, col);
  }

  return CXChildVisit_Recurse;
}

LocationResult
completion_locateAt(completion_Session *session, int line, int column)
{
  CXSourceLocation loc;
  CXFile file = clang_getFile( session->cx_tu, 
			       session->src_filename );
  LocationResult lr;

  fprintf(stdout, "Checking file %s line %d col %d\n",
	  session->src_filename,
	  line,
	  column);

  if (!file) {
    fprintf(stdout, "DogBaby!\n");
    return lr;
  }

  loc = clang_getLocation( session->cx_tu, file, line, column);

  CXCursor cursor;
  CXCursor defcursor;
  CXCursor prevcursor;
  CXString cxfname;
  unsigned l;
  unsigned c;

  cursor = clang_getCursor( session->cx_tu, loc );
  fprintf(stdout, "Cursor Kind: %d\n", clang_getCursorKind(cursor));

  //clang_visitChildren( cursor, myvisitor, NULL );

  prevcursor = clang_getNullCursor();

  if ( clang_isReference( cursor.kind ) ) {
    fprintf(stdout, "Initial check reveals isReference!\n");
    cursor = clang_getCursorReferenced( cursor );
    fprintf(stdout, "New Cursor Kind: %d\n", clang_getCursorKind(cursor));
  }

  switch( cursor.kind ) {
  case CXCursor_TypedefDecl:
    fprintf(stdout, "TYPEDEF DECL\n");
    //clang_visitChildren( cursor, myvisitor,  NULL);
    CXType type = clang_getTypedefDeclUnderlyingType(cursor);
    cursor = clang_getTypeDeclaration(type);
    break;

  case CXCursor_DeclRefExpr:
  case CXCursor_MemberRefExpr:
    fprintf(stdout, "DECLREF EXPR!\n");
    //cursor = clang_getCursorDefinition( cursor );
    cursor = clang_getCursorReferenced( cursor );
    break;

  case CXCursor_ClassTemplate:
    fprintf(stdout, "CLASSTEMPLATE!\n");
    //clang_visitChildren( cursor, myvisitor,  NULL);
    defcursor = clang_getCursorDefinition( cursor );
    //defcursor = clang_getSpecializedCursorTemplate( cursor );
    /* defcursor =  */
    /*   clang_getCursorDefinition(clang_getCursor(session->cx_tu,clang_getCursorLocation(clang_getCursorDefinition(cursor)))); */

    //defcursor = clang_getCursorReferenced( cursor );
    if (!clang_equalCursors(defcursor, clang_getNullCursor())) {
      fprintf(stdout, "Found Definition!\n");
      cursor = defcursor;
    } else {
      fprintf(stdout, "No Definition...\n");
    }
    break;

  case CXCursor_CallExpr:
    fprintf(stdout, "CALL EXPR!\n");
    cursor = clang_getCursorReferenced( cursor );
    break;

  default:
    break;
  }

  loc = clang_getCursorLocation( cursor );
  clang_getSpellingLocation( loc, &file, &l, &c, NULL );

  cxfname = clang_getFileName( file );

  lr.filename = clang_getCString( cxfname );
  lr.line = l;
  lr.column = c;


  //cursor = clang_getCursorReferenced( cursor );

  return lr;
}
