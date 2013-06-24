#include <stdlib.h>
#include <string.h>
#include <stddef.h>
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

static void 
print_LocationResult(CXCursor cursor, CXSourceLocation loc)
{
  CXFile cxfile;
  unsigned l;
  unsigned c;
  CXString cxstrfile;
  CXString cxstrcursor;

  clang_getSpellingLocation( loc, &cxfile, &l, &c, NULL );

  if ( clang_equalLocations( loc, clang_getNullLocation() ) )
    return;

  cxstrcursor = clang_getCursorKindSpelling( cursor.kind );
  cxstrfile = clang_getFileName( cxfile );

  fprintf(stdout, "LOCATE:\ndesc:%s\nfile:%s\nline:%d\ncolumn:%d\n", 
	  clang_getCString( cxstrcursor ),
	  clang_getCString( cxstrfile ),
	  l,
	  c);

  clang_disposeString( cxstrfile );
  clang_disposeString( cxstrcursor );
}

enum CXChildVisitResult
myvisitor(CXCursor c, CXCursor p, CXClientData d)
{
  CXSourceLocation loc;

  loc = clang_getCursorLocation( c );
  print_LocationResult( c, loc );

  clang_visitChildren( c, myvisitor, NULL );

  //return CXChildVisit_Continue;
  return CXChildVisit_Recurse;
}

typedef struct {
  CXCursor ccursor;
  CXFile ccur_file;
  int ccur_line;
  int ccur_column;

  CXFile orig_file;
  int orig_line;
  int orig_column;
} ClosestCursor;

void updateClosestCursor(ClosestCursor *cc, CXCursor c, CXFile file,
			 unsigned line, unsigned col )
{
  cc->ccursor = c;
  cc->ccur_file = file;
  cc->ccur_line = line;
  cc->ccur_column = col;
}

void findRefsInFiles(CXCursor c, CXFile *files)
{
  
}

enum CXChildVisitResult
closestCursorVistitor(CXCursor c, CXCursor p, CXClientData d)
{
  ClosestCursor *cc = (ClosestCursor *)d;
  CXSourceLocation loc = clang_getCursorLocation( c );
  CXFile file;
  unsigned line;
  unsigned col;

  clang_getSpellingLocation( loc, &file, &line, &col, NULL );
  print_LocationResult( c, loc );

  if ( file != cc->orig_file )
    return CXChildVisit_Continue;

  if ( line <= cc->orig_line )
    updateClosestCursor( cc, c, file, line, col );

  /* else if ( l == cc->orig_line ) { */
  /*   int odelta = abs( (int)cc->orig_column - (int)col ); */
  /*   int cdelta = abs( (int) cc->ccur_column - (int)cc->orig_column ); */

  /*   if ( odelta < cdelta  ) */
  /*     updateClosestCursor( cc, c, line, col ); */
  /* } */

  else
    return CXChildVisit_Break;

  //  return CXChildVisit_Recurse;
  return CXChildVisit_Continue;
}

LocationResult
findClosestCursor(CXTranslationUnit tu, CXFile file, 
		  int line, int column)
{
  LocationResult lr = { 0, 0, 0 };
  CXCursor rootcursor = clang_getTranslationUnitCursor( tu );
  ClosestCursor cc = { rootcursor, NULL, 1, 1, file, line, column };

  clang_visitChildren( rootcursor, closestCursorVistitor, &cc );

  lr.file = cc.ccur_file;
  lr.line = cc.ccur_line;
  lr.column = cc.ccur_column;

  return lr;
}

LocationResult
completion_locateAt(completion_Session *session, int line, int column)
{
  CXSourceLocation loc;
  CXFile file = clang_getFile( session->cx_tu, 
			       session->src_filename );
  LocationResult lr = { 0, 0, 0 };

  CXCursor cursor;
  CXCursor defcursor;
  CXCursor prevcursor;
  CXString cxfname;
  unsigned l;
  unsigned c;

  fprintf(stdout, "Checking file %s line %d col %d\n",
	  session->src_filename,
	  line,
	  column);

  if (!file) {
    return lr;
  }

  loc = clang_getLocation( session->cx_tu, file, line, column);
  cursor = clang_getCursor( session->cx_tu, loc );
  print_LocationResult(cursor, loc);

  if ( clang_Cursor_isNull( cursor ) ) {
    fprintf(stdout, "Cursor is NULL\n");
    return lr;
  }

  if ( cursor.kind >= CXCursor_FirstInvalid && 
       cursor.kind <= CXCursor_LastInvalid ) {
    fprintf(stdout, "InVALID Cursor! FINDING CLOSEST CURSOR\n");
    return findClosestCursor( session->cx_tu, file, line, column );
  }
  
  //clang_visitChildren( cursor, myvisitor, NULL );

  while ( clang_isReference( cursor.kind ) ) {
    fprintf(stdout, "Check reveals cursor isReference!\n");
    cursor = clang_getCursorReferenced( cursor );
    fprintf(stdout, "New Cursor Kind: %d\n", clang_getCursorKind(cursor));
  }
  
  if ( cursor.kind >= CXCursor_FirstRef && cursor.kind <= CXCursor_LastRef ) {
    fprintf(stdout, "REFERENCE TYPE!\n");
    cursor = clang_getCursorReferenced( cursor );
  }

  CXType type;
  switch( cursor.kind ) {
  case CXCursor_CompoundStmt:
    fprintf(stdout, "COMPOUND STMT!\n");
    clang_visitChildren( cursor, myvisitor, 0 );
    return findClosestCursor( session->cx_tu, file, line, column );
    break;

  case CXCursor_TypedefDecl:
    fprintf(stdout, "TYPEDEF DECL\n");
    type = clang_getCursorType( cursor );
    cursor = clang_getTypeDeclaration(type);
    break;

  case CXCursor_MacroExpansion:
    /* clang_visitChildren( cursor, myvisitor, NULL ); */
    /* clang_getExpansionLocation( loc, &file, &l, &c, NULL ); */
    /* loc = clang_getLocation( session->cx_tu, file, l, c ); */

  case CXCursor_CallExpr:
  case CXCursor_DeclRefExpr:
  case CXCursor_MemberRefExpr:
    prevcursor = cursor;

    cursor = clang_getCursorReferenced( cursor );
    if ( clang_Cursor_isNull( cursor ) ) {
      type = clang_getCursorType( prevcursor );
      cursor = clang_getTypeDeclaration(type);
    }

    break;

  case CXCursor_ClassTemplate:
    fprintf(stdout, "CLASSTEMPLATE!\n");
    defcursor = clang_getCursorDefinition( cursor );
    if (!clang_equalCursors(defcursor, clang_getNullCursor())) {
      fprintf(stdout, "Found Definition!\n");
      cursor = defcursor;
    } else {
      fprintf(stdout, "No Definition...\n");
    }
    break;

  case CXCursor_InclusionDirective:
    fprintf(stdout, "INCLUSION DIRECTIVE!\n");
    lr.file = clang_getIncludedFile( cursor );
    lr.line = 1;
    lr.column = 1;
    return lr;

  default:
    break;
  }

  loc = clang_getCursorLocation( cursor );
  clang_getSpellingLocation( loc, &file, &l, &c, NULL );

  
  lr.file = file;
  lr.line = l;
  lr.column = c;

  return lr;
}
