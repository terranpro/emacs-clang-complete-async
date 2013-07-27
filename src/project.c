#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include "msg_callback.h"
#include "project.h"

static completion_Project projects[64];
static int next_project = 0;

struct node 
{
  char *key;
  void *data;
  struct node *next;
};

struct bucket 
{
  int entries;
  struct node *root;
};
  
typedef struct hash_table_
{
  struct bucket *buckets;
  int bucket_count;
} hash_table;

size_t
hash_table_hash( char const *key, int nbuckets )
{
  size_t sum = 0;
  while ( *key != '\0' )
    sum += *key++;
  return sum % nbuckets;
}
  
void *
hash_table_find( hash_table *ht, char const *key )
{
  size_t bindex = hash_table_hash( key, ht->bucket_count );
  struct bucket *bucket = &ht->buckets[ bindex ];
  
  if ( bucket->entries == 0 )
    return NULL;
  else if ( bucket->entries == 1 )
    return ht->buckets[ bindex ].root->data;

  char *result;
  struct node *root = bucket->root;
  while( root != NULL ) {
    if ( strcmp( key, root->key ) == 0 )
      return root->data;
    root = root->next;
  }
  return NULL;
}

void
hash_table_bucket_add_entry( struct bucket *bucket, char const *key, 
			     void *data )
{
  struct node *thisnode = NULL;
  
  if ( bucket->entries == 0 && bucket->root == NULL ) {
    bucket->root = (struct node *)malloc( sizeof( struct node ) );
    thisnode = bucket->root;
  } else {
    thisnode = bucket->root;
    while( thisnode->next != NULL )
      thisnode = thisnode->next;
  }
  
  thisnode->next = NULL;
  thisnode->key = strdup( key );
  thisnode->data = data;
  bucket->entries++;
  return;
}

void
hash_table_add( hash_table *ht, char const *key, void *data )
{
  size_t bindex = hash_table_hash( key, ht->bucket_count );
  hash_table_bucket_add_entry( &ht->buckets[bindex], key, data );
  return ;
}

#define NBUCKETS 32

hash_table *
hash_table_new()
{
  hash_table *ht = (hash_table *)malloc( sizeof( hash_table ) );
  ht->buckets = (struct bucket *)malloc( sizeof( struct bucket ) * NBUCKETS );
  ht->bucket_count = NBUCKETS;
  int i;
  for ( i = 0; i < ht->bucket_count; ++i ) {
    ht->buckets[i].root = NULL;
    ht->buckets[i].entries = 0;
  }
  return ht;
}

void 
hash_table_bucket_free( struct bucket *bucket )
{
  struct node *node = bucket->root;
  struct node *next = NULL;
  
  while( node != NULL ) {
    next = node->next;
    free( node->key );
    free( node );
    node = next;
  }
}

void 
hash_table_buckets_free( hash_table *ht )
{
  int i;
  for ( i = 0; i < ht->bucket_count; ++i )
    hash_table_bucket_free( &ht->buckets[i] );
  free( ht->buckets );
}

void
hash_table_free(hash_table *ht )
{
  hash_table_buckets_free( ht );
  free( ht );
}

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

static void 
print_LocationResult(CXCursor cursor, CXSourceLocation loc)
{
  BARK;
  CXFile cxfile;
  unsigned l;
  unsigned c;
  CXString cxstrfile;
  CXString cxstr_cursorkind_spelling;
  CXString cxstr_cursor_spelling;

  clang_getSpellingLocation( loc, &cxfile, &l, &c, NULL );

  if ( clang_equalLocations( loc, clang_getNullLocation() ) ) {
    printf("null cursor!\n");
    return;
  }

  cxstr_cursorkind_spelling = clang_getCursorKindSpelling( cursor.kind );
  cxstrfile = clang_getFileName( cxfile );

  cxstr_cursor_spelling =  clang_getCursorSpelling( cursor );

  fprintf(stdout,
	  "%s\ndesc:%s ! %s\nfile:%s\nline:%d\ncolumn:%d\ndefinition:%s\n",
	  "PRJ_LOCATE:",
	  clang_getCString( cxstr_cursorkind_spelling ),
	  clang_getCString( cxstr_cursor_spelling ),
	  clang_getCString( cxstrfile ),
	  l,
	  c,
	  clang_isCursorDefinition( cursor ) ? "true" : "false"
	  );

  clang_disposeString( cxstr_cursorkind_spelling );
  clang_disposeString( cxstr_cursor_spelling );
  clang_disposeString( cxstrfile );
}

void projectNew(completion_Project *prj)
{
  BARK;
  prj->index = clang_createIndex(0, 1);
  prj->parsed = 0;
  prj->active_tunit = -1;
  prj->tunits = NULL;
  prj->src_count = -1;
  prj->src_filenames = NULL;
  prj->cxunfile_ht = hash_table_new();
}

int projectFindId(char const *src_file)
{
  BARK;
  int cur_prj = 0;
  completion_Project *prj = NULL;

  while ( cur_prj < next_project ) {
    prj = &projects[ cur_prj ];
    int cur_src = 0;

    printf("Searching Project %d : Src Count = %d\n", 
	   cur_prj, prj->src_count );
    fflush(stdout);

    while ( cur_src < prj->src_count ) {
      printf("Searching File %s\n", prj->src_filenames[ cur_src ] );
      fflush(stdout);
      if ( strcmp( prj->src_filenames[ cur_src ], src_file ) == 0 )
	return cur_prj;

      ++cur_src;
    }

    ++cur_prj;
  }

  return -1;
}


void projectOptions(completion_Project *prj, int argc, char **argv)
{
  BARK;
  prj->arg_count = argc;
  prj->args = argv;
}

void projectAdd(completion_Project *prj, char const *src_file)
{
  BARK;
  const int alloc_size = 512;

  if ( prj->src_count < 0 || prj->src_count % alloc_size == 0 ) {
    printf("realloc required! new size = %d\n",
			 sizeof(char *) * prj->src_count * alloc_size);
    fflush(stdout);
    ++prj->src_count;
    char **old_srcs = prj->src_filenames;
    prj->src_filenames = 
      (char **) malloc( sizeof(char *) * (prj->src_count + 1) * alloc_size);

    CXTranslationUnit *old_tunits = prj->tunits;
    prj->tunits = 
      (CXTranslationUnit *)malloc(sizeof(CXTranslationUnit) *
				  (prj->src_count + 1) * alloc_size );

    if ( old_srcs != NULL) {
      char **src = &old_srcs[0];
      char **dst = &prj->src_filenames[0];

      CXTranslationUnit *srctu = &old_tunits[0];

      CXTranslationUnit *dsttu = &prj->tunits[0];

      while( *src ) {
	*dst = *src;
	++dst;
	++src;
	(*dsttu) = (*srctu);
	++dsttu;
	++srctu;
      }

      *dst = NULL;
      *dsttu = NULL;

      --prj->src_count;
    }

  }

  prj->src_filenames[ prj->src_count ] = strdup( src_file );
  
  fprintf(stdout, "src = %s\ncount = %d\n", 
	  prj->src_filenames[ prj->src_count ],
	  prj->src_count);

  printf("Making TU for file %s\n", prj->src_filenames[ prj->src_count ] );
  /* printf("Args: "); */

  /* int butthole = 0; */
  /* for ( butthole; butthole < prj->arg_count; ++butthole ) { */
  /*   printf("%s ", prj->args[ butthole ]); */
  /* } */
  /* printf("\n"); */

  prj->tunits[ prj->src_count ] = 
    clang_createTranslationUnitFromSourceFile(prj->index, 
					      prj->src_filenames[ prj->src_count], prj->arg_count, prj->args, 0, NULL);

    /* clang_parseTranslationUnit(prj->index,  */
    /* 			       src_file, prj->args, prj->arg_count,  */
    /* 			       NULL, 0, DEFAULT_PARSE_OPTIONS); */

  if ( prj->tunits[ prj->src_count ] == NULL ) {
    printf("Translation unit NULL - failure!\n");
    return;
  }

  int n = clang_getNumDiagnostics( prj->tunits[ prj->src_count ] );
  printf("Num diagnostics = %d\n", n);

  prj->src_count++;
}
static size_t result_count = 0;
static const size_t max_result_count = 255;

enum CXChildVisitResult
usrmatcher(CXCursor c, CXCursor p, CXClientData d)
{
  CXString *usr = (CXString *)d;
  CXString this_usr = clang_getCursorUSR( c );
  enum CXChildVisitResult result = CXChildVisit_Recurse;

  if ( strcmp( clang_getCString( this_usr ), clang_getCString( *usr )) == 0 ) {
    CXString s = clang_getCursorSpelling( c );
    printf("FOUND @ %s\n", clang_getCString( s ));
    clang_disposeString( s );

    CXSourceLocation loc = clang_getCursorLocation( c );
    print_LocationResult( c, loc );

    result = CXChildVisit_Continue;
    ++result_count;
  } else {
    //printf("FISHED!\n");
    //CXSourceLocation loc = clang_getCursorLocation( c );
    //print_LocationResult( c, loc );
    //fflush(stdout);
  }

  if ( result_count >= max_result_count )
    result = CXChildVisit_Break;

  clang_disposeString( this_usr );
  
  return result;
}

static void locate_include(completion_Project *prj, CXCursor cursor)
{
  BARK;
  CXSourceLocation loc = clang_getCursorLocation( cursor );
  CXFile file = clang_getIncludedFile( cursor );
  loc = clang_getLocation( prj->tunits[ prj->active_tunit ], file, 1, 1 );
  CXCursor inc_cursor = clang_getCursor( prj->tunits[ prj->active_tunit ],
					 loc );
  print_LocationResult( inc_cursor, loc );
}

static void locate_functiondecl(completion_Project *prj, CXCursor cursor)
{
  BARK;
  CXCursor prevcursor = cursor;
  CXType type;

  cursor = clang_getCursorReferenced( cursor );
  if ( clang_Cursor_isNull( cursor ) ) {
    type = clang_getCursorType( prevcursor );
    cursor = clang_getTypeDeclaration(type);
  }

  CXSourceLocation loc =
    clang_getCursorLocation( cursor );

  print_LocationResult(cursor, loc);

  CXString cursor_usr = clang_getCursorUSR( cursor );
  if ( clang_getCursorLinkage( cursor ) > CXLinkage_Internal ) {
    
    int tu_count = 0;
    printf("Cursor USR Spelling: %s\n", clang_getCString( cursor_usr ));

    result_count = 0;

    while( tu_count < prj->src_count && prj->tunits[tu_count] != NULL ) {
      CXCursor c = clang_getTranslationUnitCursor( prj->tunits[tu_count] );
      printf("Scanning file: %s\n", prj->src_filenames[ tu_count ] );

      if (!clang_Cursor_isNull( c ) )
	clang_visitChildren( c , usrmatcher, &cursor_usr );

      ++tu_count;
    }
  }

  clang_disposeString( cursor_usr );
}

static void locate_declrefexpr(completion_Project *prj, CXCursor cursor)
{
  BARK;
  CXCursor prevcursor = cursor;
  CXType type;

  cursor = clang_getCursorReferenced( cursor );
  if ( clang_Cursor_isNull( cursor ) ) {
    type = clang_getCursorType( prevcursor );
    cursor = clang_getTypeDeclaration(type);
  }

  CXSourceLocation loc =
    clang_getCursorLocation( cursor );

  print_LocationResult(cursor, loc);

  CXString cursor_usr = clang_getCursorUSR( cursor );
  if ( clang_getCursorLinkage( cursor ) > CXLinkage_Internal ) {
    
    int tu_count = 0;
    printf("Cursor USR Spelling: %s\n", clang_getCString( cursor_usr ));

    result_count = 0;

    while( tu_count < prj->src_count && prj->tunits[tu_count] != NULL ) {
      CXCursor c = clang_getTranslationUnitCursor( prj->tunits[tu_count] );
      printf("Scanning file: %s\n", prj->src_filenames[ tu_count ] );

      if (!clang_Cursor_isNull( c ) )
	clang_visitChildren( c , usrmatcher, &cursor_usr );

      ++tu_count;
    }
  }

  clang_disposeString( cursor_usr );
}

//static void locate_declrefexpr(completion_Project *prj, CXCursor cursor)
//{
//}

static void locate_classtemplate(completion_Project *prj, CXCursor cursor)
{
  BARK;
  CXCursor defcursor = clang_getCursorDefinition( cursor );
  if (!clang_equalCursors(defcursor, clang_getNullCursor())) {
    fprintf(stdout, "Found Definition!\n");
    cursor = defcursor;
  } else {
    fprintf(stdout, "No Definition...\n");
  }

  CXString cursor_usr = clang_getCursorUSR( cursor );
  printf("Cursor USR Spelling: %s\n", clang_getCString( cursor_usr ));

  result_count = 0;
  int tu_count = 0;
  while( prj->tunits[tu_count] != NULL ) {
    CXCursor c = clang_getTranslationUnitCursor( prj->tunits[tu_count] );
    //printf("Scanning file: %s\n", prj->src_filenames[ tu_count ] );
    clang_visitChildren( c , usrmatcher, &cursor_usr );
    ++tu_count;
  }

  clang_disposeString( cursor_usr );

}

static void locate_classdecl(completion_Project *prj, CXCursor cursor)
{
  BARK;
  CXCursor prevcursor = cursor;
  CXType type;

  cursor = clang_getCursorReferenced( cursor );
  if ( clang_Cursor_isNull( cursor ) ) {
    type = clang_getCursorType( prevcursor );
    cursor = clang_getTypeDeclaration(type);
  }

  print_LocationResult( cursor, clang_getCursorLocation( cursor ));

  CXString cursor_usr = clang_getCursorUSR( cursor );
  printf("Cursor USR Spelling: %s\n", clang_getCString( cursor_usr ));

  result_count = 0;
  int tu_count = 0;
  while( prj->tunits[tu_count] != NULL ) {
    CXCursor c = clang_getTranslationUnitCursor( prj->tunits[tu_count] );
    //printf("Scanning file: %s\n", prj->src_filenames[ tu_count ] );
    clang_visitChildren( c , usrmatcher, &cursor_usr );
    ++tu_count;
  }

  clang_disposeString( cursor_usr );
}

static void locate_typedefdecl(completion_Project *prj, CXCursor cursor)
{
  BARK;
  CXType type = clang_getCursorType( cursor );
  cursor = clang_getTypeDeclaration(type);
  print_LocationResult( cursor, clang_getCursorLocation(cursor) );
}

enum CXChildVisitResult
virtualmatcher(CXCursor c, CXCursor p, CXClientData d)
{
  enum CXChildVisitResult result = CXChildVisit_Recurse;
  if ( c.kind != CXCursor_CXXMethod )
    return CXChildVisit_Recurse;

  CXCursor *overrides = NULL;
  unsigned num_overrides;
  clang_getOverriddenCursors( c, &overrides, &num_overrides );

  CXCursor *orig_cursor = (CXCursor *)d;
  CXString orig_spelling = clang_getCursorSpelling( *orig_cursor );

  unsigned override = 0;
  for ( ; override < num_overrides; ++override ) {
    fprintf(stdout, "Comparing cursors: %s and %s\n",
	    clang_getCString( orig_spelling ),
	    clang_getCString( clang_getCursorSpelling( overrides[override] )));

    if ( strcmp( clang_getCString( orig_spelling ),
		 clang_getCString( clang_getCursorSpelling( overrides[override] ))) == 0 ) {

      print_LocationResult( overrides[override], 
			    clang_getCursorLocation( overrides[override] ));
      print_LocationResult( c, clang_getCursorLocation(c) );
    }


  }

  clang_disposeOverriddenCursors( overrides );

  return CXChildVisit_Continue;
}

static void locate_cxxmethod(completion_Project *prj, CXCursor cursor)
{
  BARK;
  if ( clang_CXXMethod_isVirtual( cursor ) ) {
    fprintf(stdout, "Method is virtual! scanning!\n");
    int tu_index = 0;
    for ( ; tu_index < prj->src_count; ++tu_index ) {
      CXCursor tuparent = 
	clang_getTranslationUnitCursor( prj->tunits[tu_index] );

      clang_visitChildren( tuparent, virtualmatcher, &cursor );
    }

  }

  locate_classtemplate( prj, cursor );
}

static void locate_memberrefexpr(completion_Project *prj, CXCursor cursor)
{
  BARK;
  CXCursor prevcursor = cursor;
  CXType type;

  cursor = clang_getCursorReferenced( cursor );
  if ( clang_Cursor_isNull( cursor ) ) {
    type = clang_getCursorType( prevcursor );
    cursor = clang_getTypeDeclaration(type);
  }

  print_LocationResult( cursor, clang_getCursorLocation( cursor ));

  if ( cursor.kind == CXCursor_CXXMethod )
    locate_cxxmethod( prj, cursor );
}

enum CXChildVisitResult
namespace_matcher(CXCursor c, CXCursor p, CXClientData d)
{
  if ( c.kind == CXCursor_NamespaceRef )
    c = clang_getCursorReferenced( c );

  if ( c.kind != CXCursor_Namespace )
    return CXChildVisit_Recurse;

  print_LocationResult( c, clang_getCursorLocation( c ));
  return CXChildVisit_Continue;
}

static void locate_namespace(completion_Project *prj, CXCursor cursor)
{
  BARK;
  CXCursor prevcursor = cursor;
  cursor = clang_getCursorReferenced( cursor );
  if ( clang_Cursor_isNull( cursor ) ) {
    cursor = prevcursor;
  }

  size_t tu_index = 0;
  for ( ; tu_index < prj->src_count; ++tu_index ) {
    CXCursor tuparent = 
      clang_getTranslationUnitCursor( prj->tunits[tu_index] );
    clang_visitChildren( tuparent, namespace_matcher, &cursor );
  }

}

enum CXChildVisitResult
printvisitor(CXCursor c, CXCursor p, CXClientData d)
{
  print_LocationResult( c, clang_getCursorLocation(c) );
  return CXChildVisit_Continue;
}

static void locate_macrodefinition(completion_Project *prj, CXCursor cursor)
{
  BARK;
  print_LocationResult( cursor, clang_getCursorLocation(cursor) );
  clang_visitChildren( cursor, printvisitor, NULL );
}

static void locate_enumdecl(completion_Project *prj, CXCursor cursor)
{
  BARK;
  print_LocationResult( cursor, clang_getCursorLocation(cursor) );
  clang_visitChildren( cursor, printvisitor, NULL );
}

void locate_cursorDispatch(completion_Project *prj, CXCursor cursor)
{
  BARK;
  switch(cursor.kind) {
  case CXCursor_InclusionDirective:
    locate_include( prj , cursor );
    break;

  case CXCursor_ParmDecl:
  case CXCursor_VarDecl:
  case CXCursor_TypedefDecl:
    locate_typedefdecl( prj, cursor );
    break;

  case CXCursor_Namespace:
  case CXCursor_NamespaceRef:
    locate_namespace( prj, cursor );
    break;

  case CXCursor_MacroDefinition:
    locate_macrodefinition( prj, cursor );
    break;

  case CXCursor_EnumDecl:
    locate_enumdecl( prj, cursor );
    break;

  case CXCursor_MacroExpansion:
  case CXCursor_CallExpr:
  case CXCursor_DeclRefExpr:
    locate_declrefexpr( prj, cursor );
    break;

  case CXCursor_MemberRefExpr:
    locate_memberrefexpr( prj, cursor );
    break;

  case CXCursor_ClassTemplate:
    locate_classtemplate( prj, cursor );
    break;

  case CXCursor_FunctionDecl:
    locate_functiondecl( prj, cursor );
    break;

  case CXCursor_FieldDecl:
  case CXCursor_ClassDecl:
    locate_classdecl( prj, cursor );

  case CXCursor_Constructor:
    locate_classtemplate( prj, cursor );
    break;

  case CXCursor_CXXMethod:
    locate_cxxmethod( prj, cursor );
    break;

  default:
    fprintf(stdout, "Unhandled Cursor Dispatch case: %d\n", cursor.kind);
    break;
  }
}

void projectFileSrc(FILE *fp, int proj_id)
{
  unsigned long source_length;
  char *src_buffer;
  char filebuf[1024];
  
  fscanf(fp, "file:%s", filebuf );
  __skip_the_rest(fp);
  
  fscanf(fp, "source_length:%d", &source_length);
  __skip_the_rest(fp);

  src_buffer = (char *)malloc( sizeof(char) * source_length );

  /* read source code from fp to buffer */
  __read_n_bytes(fp, src_buffer, source_length);

  struct CXUnsavedFile *unfile = 
    (struct CXUnsavedFile *) hash_table_find( projects[proj_id].cxunfile_ht, 
					      filebuf );
  if ( unfile == NULL ) {
    unfile = (struct CXUnsavedFile *)malloc( sizeof( struct CXUnsavedFile ) );
    unfile->Filename = strdup( filebuf );
    hash_table_add( projects[proj_id].cxunfile_ht, unfile->Filename, unfile);
  } else {
    free( unfile->Contents );
  }
  unfile->Length = source_length;
  unfile->Contents = src_buffer;
}

void project_locate(completion_Project *prj, int line, int column)
{
  BARK;
  CXCursor cursor;
  CXSourceLocation loc;
  ssize_t i = prj->active_tunit;

  fprintf(stdout, "Active TU # = %d\n", i);

  if ( !prj->tunits[ i ] ) {
    fprintf(stdout, "Active TU is NULL - Creating...\n");
    prj->tunits[ i ] = 
      clang_createTranslationUnitFromSourceFile(prj->index, 
						prj->src_filenames[ i ], 
						prj->arg_count,
						prj->args, 0, NULL);

    if ( !prj->tunits[i] ) {
      fprintf(stdout, "Creating TU FAILED!\n");
      return;
    }
  }
  
  else if ( clang_reparseTranslationUnit( prj->tunits[ i ], 0, NULL, 
				     DEFAULT_PARSE_OPTIONS ) != 0 ) {
    fprintf(stdout, "Reparsing Translation Unit Failed!\n");
    clang_disposeTranslationUnit( prj->tunits[ i ] );
    prj->tunits[ i ] = NULL;
    return;
  }

  CXFile file = clang_getFile( prj->tunits[i], prj->src_filenames[i] );

  loc = clang_getLocation( prj->tunits[i], file, line, column);
  cursor = clang_getCursor( prj->tunits[i], loc );

  CXString s = clang_getFileName( file );
  CXString cursor_spelling = clang_getCursorSpelling( cursor );
  printf("ART OF LOCATE %s @ %s %d, %d\n",
	 clang_getCString( cursor_spelling ),
	 clang_getCString(s),
	 line, column);
  clang_disposeString(s);

  print_LocationResult(cursor, loc);

  fflush(stdout);

  while ( clang_isReference( cursor.kind ) ) {
    fprintf(stdout, "Check reveals cursor isReference!\n");
    cursor = clang_getCursorReferenced( cursor );
    fprintf(stdout, "New Cursor Kind: %d\n", clang_getCursorKind(cursor));
  }
  
  if ( cursor.kind >= CXCursor_FirstRef && cursor.kind <= CXCursor_LastRef ) {
    fprintf(stdout, "REFERENCE TYPE!\n");
    cursor = clang_getCursorReferenced( cursor );
  }

  locate_cursorDispatch( prj , cursor );
}

void project_dispatch(FILE *fp)
{
  int id;
  char subcmd[2048 * 100];
  fgets(subcmd, sizeof(subcmd), fp);
  fprintf(stdout, "SUBCMD = %s\n", subcmd);
  if ( subcmd[ strlen(subcmd) - 1 ] == '\n' )
    subcmd[ strlen(subcmd) - 1 ] = '\0';

  if ( strcmp( subcmd, "ADD_SRC" ) == 0 )
  {
    fscanf(fp, "PROJECTID:%d", &id); __skip_the_rest(fp);
    fscanf(fp, "%s", subcmd); __skip_the_rest(fp);
    //fgets(subcmd, sizeof(subcmd), fp);
    projectAdd( &projects[ id ], subcmd );
  }
  else if ( strcmp(subcmd, "NEW") == 0 ) {
    projectNew( &projects[next_project] );
    fprintf(stdout, "PROJECTID:%d\n", next_project++);
  }
  else if ( strcmp(subcmd, "FIND_ID") == 0 ) {
    fscanf(fp, "%s", subcmd); __skip_the_rest(fp);
    id = projectFindId( subcmd );
    fprintf(stdout, "PROJECTID:%d\n", id);
  }
  else if ( strcmp(subcmd, "FILE_SRC") == 0 ) {
    fscanf(fp, "PROJECTID:%d", &id); __skip_the_rest(fp);
    projectFileSrc( fp, id );
  }
  else if ( strcmp(subcmd, "OPTIONS") == 0 ) {
    fscanf(fp, "PROJECTID:%d", &id); __skip_the_rest(fp);
    fgets(subcmd, sizeof(subcmd), fp);

    subcmd[ strlen(subcmd) - 1 ] = '\0';

    fprintf(stdout, "OPTIONS = %s\n", subcmd);
    int argc;
    char **argv = (char **)malloc( sizeof(char *) * 1024 );
    size_t curarg_index = 0;
    char *curarg;
    char *startstr = subcmd;

    while( (curarg = strtok( startstr, " " )) != NULL ) {
      startstr = NULL;
      fprintf(stdout, "curarg = %s\n", curarg);
      argv[curarg_index++] = strdup( curarg );
    }

    //argv[curarg_index] = NULL;
    argc = curarg_index;

    projectOptions( &projects[ id ], argc, argv );
  }
  else if ( strcmp(subcmd, "LOCATE") == 0 ) {
    int row;
    int column;
    char prefix[512];

    fscanf(fp, "PROJECTID:%d", &id); __skip_the_rest(fp);
    fscanf(fp, "src:%s", subcmd); __skip_the_rest(fp);
    fscanf(fp, "row:%d",    &row);    __skip_the_rest(fp);
    fscanf(fp, "column:%d", &column); __skip_the_rest(fp);
    fscanf(fp, "prefix:"); 
    fgets(prefix, sizeof(prefix), fp);

    printf("src = %s\n", subcmd);
    int x = 0;

    fflush(stdout);

    printf("x = %d prj_id = %d src_count = %d\n", x, id, projects[id].src_count);
    fflush(stdout);

    while( projects[id].src_count > 0 &&
	   x < projects[id].src_count &&
	   projects[id].src_filenames[x] &&  
	   strcmp( projects[id].src_filenames[x], subcmd ) != 0 ) {
      printf("comparing %s to %s\n", projects[id].src_filenames[x], subcmd);
      fflush(stdout);
      x++;
    }
    printf("x = %d prj_id = %d src_count = %d\n", x, id, projects[id].src_count);
    fflush(stdout);

    if ( x < projects[id].src_count ) {
      projects[id].active_tunit = x;
      project_locate( &projects[ id ], row, column );
    }
    else {
      // Unknown file or possibly include?!
      fprintf(stdout, "Unknown file or possibly include?! Adding...\n");
      fflush(stdout);
      projectAdd( &projects[id], subcmd );
      projects[id].active_tunit = projects[id].src_count - 1;
      project_locate( &projects[ id ], row, column );
    }

  }
  else {
    fprintf(stdout, "Unknown PROJECT subcommand!?!?!\n");
  }

  fprintf(stdout, "$"); 
  fflush(stdout);
}
