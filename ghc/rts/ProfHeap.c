/* -----------------------------------------------------------------------------
 * $Id: ProfHeap.c,v 1.26 2001/11/22 14:25:12 simonmar Exp $
 *
 * (c) The GHC Team, 1998-2000
 *
 * Support for heap profiling
 *
 * ---------------------------------------------------------------------------*/

#if defined(DEBUG) && !defined(PROFILING)
#define DEBUG_HEAP_PROF
#else
#undef DEBUG_HEAP_PROF
#endif

#if defined(PROFILING) || defined(DEBUG_HEAP_PROF)

#include "PosixSource.h"
#include "Rts.h"
#include "RtsUtils.h"
#include "RtsFlags.h"
#include "Profiling.h"
#include "Storage.h"
#include "ProfHeap.h"
#include "Stats.h"
#include "Hash.h"
#include "StrHash.h"
#include "RetainerProfile.h"
#include "LdvProfile.h"

#ifdef DEBUG_HEAP_PROF
#include "Printer.h"
static void initSymbolHash(void);
static void clear_table_data(void);
static void fprint_data(FILE *fp);
#endif

/* -----------------------------------------------------------------------------
 * Hash tables.
 *
 * For profiling by module, constructor or closure type we need to be
 * able to get from a string describing the category to a structure
 * containing the counters for that category.  The strings aren't
 * unique (although gcc will do a fairly good job of commoning them up
 * where possible), so we have a many->one mapping.
 *
 * We represent the many->one mapping with a hash table.  In order to
 * find the unique counter associated with a string the first time we
 * encounter a particular string, we need another hash table, mapping
 * hashed strings to buckets of counters.  The string is hashed, then
 * the bucket is searched for an existing counter for the same
 * string. 
 *
 * -------------------------------------------------------------------------- */

#ifdef PROFILING
typedef struct _ctr {
    const char *str;
    unsigned long mem_resid;
    struct _ctr *next;
    struct _ctr *next_bucket;
} prof_ctr;

/* Linked list of all existing ctr structs */
prof_ctr *all_ctrs;

/* Hash table mapping (char *) -> (struct _ctr) */
HashTable *str_to_ctr;

/* Hash table mapping hash_t (hashed string) -> (struct _ctr) */
HashTable *hashstr_to_ctrs;

static void
initHashTables( void )
{
    str_to_ctr      = allocHashTable();
    hashstr_to_ctrs = allocHashTable();
    all_ctrs = NULL;
}

static prof_ctr *
strToCtr(const char *str)
{
    prof_ctr *ctr;

    ctr = lookupHashTable( str_to_ctr, (W_)str );

    if (ctr != NULL) { return ctr; }

    else {
	hash_t str_hash = hash_str((char *)str);
	prof_ctr *prev;

	ctr = lookupHashTable( hashstr_to_ctrs, (W_)str_hash );
	prev = NULL;

	for (; ctr != NULL; prev = ctr, ctr = ctr->next_bucket ) {
	    if (!strcmp(ctr->str, str)) {
		insertHashTable( str_to_ctr, (W_)str, ctr );
#ifdef DEBUG_CTR
		fprintf(stderr,"strToCtr: existing ctr for `%s'\n",str);
#endif
		return ctr;
	    }
	}

	ctr = stgMallocBytes(sizeof(prof_ctr), "strToCtr");
	ctr->mem_resid = 0;
	ctr->str = str;
	ctr->next_bucket = NULL;
	ctr->next = all_ctrs;
	all_ctrs = ctr;

#ifdef DEBUG_CTR
	fprintf(stderr,"strToCtr: new ctr for `%s'\n",str);
#endif

	if (prev != NULL) {
	    prev->next_bucket = ctr;
	} else {
	    insertHashTable( hashstr_to_ctrs, str_hash, ctr );
	}
	insertHashTable( str_to_ctr, (W_)str, ctr);
	return ctr;
    }
}

static void
clearCtrResid( void )
{
    prof_ctr *ctr;
    
    for (ctr = all_ctrs; ctr != NULL; ctr = ctr->next) {
	ctr->mem_resid = 0;
    }
}

static void
reportCtrResid(FILE *fp)
{
    prof_ctr *ctr;
    
    for (ctr = all_ctrs; ctr != NULL; ctr = ctr->next) {
	if (ctr->mem_resid != 0) {
	    fprintf(fp,"   %s %ld\n", ctr->str, ctr->mem_resid * sizeof(W_));
	}
    }
}
#endif /* PROFILING */

/* -------------------------------------------------------------------------- */

#ifdef DEBUG_HEAP_PROF
FILE *hp_file;

void initProfiling1( void )
{
}

void initProfiling2( void )
{
  initHeapProfiling();
}

void endProfiling( void )
{
  endHeapProfiling();
}
#endif /* DEBUG_HEAP_PROF */

nat
initHeapProfiling(void)
{
    if (! RtsFlags.ProfFlags.doHeapProfile) {
        return 0;
    }

    fprintf(hp_file, "JOB \"%s", prog_argv[0]);

#ifdef PROFILING
    {
	int count;
	for(count = 1; count < prog_argc; count++)
	    fprintf(hp_file, " %s", prog_argv[count]);
	fprintf(hp_file, " +RTS ");
	for(count = 0; count < rts_argc; count++)
	    fprintf(hp_file, "%s ", rts_argv[count]);
	fprintf(hp_file, "\n");
    }
#endif /* PROFILING */

    fprintf(hp_file, "\"\n" );

    fprintf(hp_file, "DATE \"%s\"\n", time_str());

    fprintf(hp_file, "SAMPLE_UNIT \"seconds\"\n");
    fprintf(hp_file, "VALUE_UNIT \"bytes\"\n");

    fprintf(hp_file, "BEGIN_SAMPLE 0.00\n");
    fprintf(hp_file, "END_SAMPLE 0.00\n");

#ifdef DEBUG_HEAP_PROF
    DEBUG_LoadSymbols(prog_argv[0]);
    initSymbolHash();
#endif

#ifdef PROFILING
    initHashTables();
#endif

    return 0;
}

void
endHeapProfiling(void)
{
    StgDouble seconds;

    if (! RtsFlags.ProfFlags.doHeapProfile) {
        return;
    }

#ifdef PROFILING
    switch (RtsFlags.ProfFlags.doHeapProfile) {
    case HEAP_BY_RETAINER:
	endRetainerProfiling();
	break;
    case HEAP_BY_LDV:
	endLdvProfiling();
	break;
    }
#endif

    seconds = mut_user_time();
    fprintf(hp_file, "BEGIN_SAMPLE %0.2f\n", seconds);
    fprintf(hp_file, "END_SAMPLE %0.2f\n", seconds);
    fclose(hp_file);
}

#ifdef DEBUG_HEAP_PROF
/* -----------------------------------------------------------------------------
   Hash table for symbols.
   -------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    void *ptr;
    nat data;
} symbol_info;

#define SYMBOL_HASH_SIZE 0x3fff

symbol_info symbol_hash[SYMBOL_HASH_SIZE];

static inline nat
hash(void *ptr)
{
    return ((W_)ptr)>>4 & 0x3fff;
}

static void
initSymbolHash(void)
{
    nat i;

    for (i=0; i < SYMBOL_HASH_SIZE; i++) {
	symbol_hash[i].ptr = NULL;
    }
}

static nat
lookup_symbol(void *addr)
{
    nat orig_bucket = hash(addr);
    nat bucket;

    bucket = orig_bucket;
    while (bucket < SYMBOL_HASH_SIZE && symbol_hash[bucket].ptr != NULL) {
	if (symbol_hash[bucket].ptr == addr) {
	    return bucket;
	}
	bucket++;
    }
    if (bucket == SYMBOL_HASH_SIZE) {
	bucket = 0;
	while (bucket < orig_bucket && symbol_hash[bucket].ptr != NULL) {
	    if (symbol_hash[bucket].ptr == addr) {
		return bucket;
	    }
	    bucket++;
	}
	if (bucket == orig_bucket) {
	    barf("out of symbol table space");
	}
    }
    
    symbol_hash[bucket].ptr  = addr;
    lookupGHCName(addr,&symbol_hash[bucket].name);
    symbol_hash[bucket].data = 0;
    return bucket;
}

static void
clear_table_data(void)
{
    nat i;

    for (i = 0; i < SYMBOL_HASH_SIZE; i++) {
	symbol_hash[i].data = 0;
    }
}

static void
fprint_data(FILE *fp)
{
    nat i;
    for (i = 0; i < SYMBOL_HASH_SIZE; i++) {
	if (symbol_hash[i].data > 0) {
	    fprintf(fp, "   %s %lu\n", symbol_hash[i].name, (unsigned long)symbol_hash[i].data);
	}
    }
}

static inline void
add_data(void *addr, nat data)
{
    symbol_hash[lookup_symbol(addr)].data += data;
}

/* -----------------------------------------------------------------------------
   Closure Type Profiling;

   PROBABLY TOTALLY OUT OF DATE -- ToDo (SDM)
   -------------------------------------------------------------------------- */

static nat closure_types[N_CLOSURE_TYPES];

static char *type_names[] = {
      "INVALID_OBJECT"
    , "CONSTR"
    , "CONSTR_INTLIKE"
    , "CONSTR_CHARLIKE"
    , "CONSTR_STATIC"
    , "CONSTR_NOCAF_STATIC"

    , "FUN"
    , "FUN_STATIC"

    , "THUNK"
    , "THUNK_STATIC"
    , "THUNK_SELECTOR"

    , "BCO"
    , "AP_UPD"

    , "PAP"

    , "IND"
    , "IND_OLDGEN"
    , "IND_PERM"
    , "IND_OLDGEN_PERM"
    , "IND_STATIC"

    , "RET_BCO"
    , "RET_SMALL"
    , "RET_VEC_SMALL"
    , "RET_BIG"
    , "RET_VEC_BIG"
    , "RET_DYN"
    , "UPDATE_FRAME"
    , "CATCH_FRAME"
    , "STOP_FRAME"
    , "SEQ_FRAME"

    , "BLACKHOLE"
    , "BLACKHOLE_BQ"
    , "MVAR"

    , "ARR_WORDS"

    , "MUT_ARR_PTRS"
    , "MUT_ARR_PTRS_FROZEN"
    , "MUT_VAR"

    , "WEAK"
    , "FOREIGN"
  
    , "TSO"

    , "BLOCKED_FETCH"
    , "FETCH_ME"

    , "EVACUATED"
};

static void 
fprint_closure_types(FILE *fp)
{
  nat i;

  for (i = 0; i < N_CLOSURE_TYPES; i++) {
    if (closure_types[i]) {
      fprintf(fp, "   %s %lu\n", type_names[i], (unsigned long)closure_types[i]);
    }
  }
}

#endif /* DEBUG_HEAP_PROF */


#ifdef PROFILING
static void
clearCCSResid(CostCentreStack *ccs)
{
  IndexTable *i;

  ccs->mem_resid = 0;

  for (i = ccs->indexTable; i != 0; i = i->next) {
    if (!i->back_edge) {
      clearCCSResid(i->ccs);
    }
  }
}

static void
fprint_ccs(FILE *fp, CostCentreStack *ccs, nat max_length)
{
    char buf[max_length+1];
    nat next_offset = 0;
    nat written;
    char *template;

    // MAIN on its own gets printed as "MAIN", otherwise we ignore MAIN.
    if (ccs == CCS_MAIN) {
	fprintf(fp, "MAIN");
	return;
    }

    // keep printing components of the stack until we run out of space
    // in the buffer.  If we run out of space, end with "...".
    for (; ccs != NULL && ccs != CCS_MAIN; ccs = ccs->prevStack) {

	// CAF cost centres print as M.CAF, but we leave the module
	// name out of all the others to save space.
	if (!strcmp(ccs->cc->label,"CAF")) {
	    written = snprintf(buf+next_offset, 
			       (int)max_length-3-(int)next_offset,
			       "%s.CAF", ccs->cc->module);
	} else {
	    if (ccs->prevStack != NULL && ccs->prevStack != CCS_MAIN) {
		template = "%s/";
	    } else {
		template = "%s";
	    }
	    written = snprintf(buf+next_offset, 
			       (int)max_length-3-(int)next_offset,
			       template, ccs->cc->label);
	}

	if (next_offset+written >= max_length-4) {
	    sprintf(buf+max_length-4, "...");
	    break;
	} else {
	    next_offset += written;
	}
    }
    fprintf(fp, "%s", buf);
}

static void
reportCCSResid(FILE *fp, CostCentreStack *ccs)
{
  IndexTable *i;

  if (ccs->mem_resid != 0) {
    fprintf(fp,"   ");
    // print as much of the CCS as possible in 20 chars, ending with "..."
    fprint_ccs(fp,ccs,30);
    fprintf(fp," %ld\n", ccs->mem_resid * sizeof(W_));
  }

  for (i = ccs->indexTable; i != 0; i = i->next) {
    if (!i->back_edge) {
      reportCCSResid(fp,i->ccs);
    }
  }
}

static rtsBool
str_matches_selector( char* str, char* sel )
{
   char* p;
   // fprintf(stderr, "str_matches_selector %s %s\n", str, sel);
   while (1) {
       // Compare str against wherever we've got to in sel.
       p = str;
       while (*p != '\0' && *sel != ',' && *sel != '\0' && *p == *sel) {
	   p++; sel++;
       }
       // Match if all of str used and have reached the end of a sel fragment.
       if (*p == '\0' && (*sel == ',' || *sel == '\0'))
	   return rtsTrue;
       
       // No match.  Advance sel to the start of the next elem.
       while (*sel != ',' && *sel != '\0') sel++;
       if (*sel == ',') sel++;
       
       /* Run out of sel ?? */
       if (*sel == '\0') return rtsFalse;
   }
}

// Figure out whether a closure should be counted in this census, by
// testing against all the specified constraints.
rtsBool
closureSatisfiesConstraints( StgClosure* p )
{
   rtsBool b;
   if (RtsFlags.ProfFlags.modSelector) {
       b = str_matches_selector( ((StgClosure *)p)->header.prof.ccs->cc->module,
				 RtsFlags.ProfFlags.modSelector );
       if (!b) return rtsFalse;
   }
   if (RtsFlags.ProfFlags.descrSelector) {
       b = str_matches_selector( (get_itbl((StgClosure *)p))->prof.closure_desc,
				 RtsFlags.ProfFlags.descrSelector );
       if (!b) return rtsFalse;
   }
   if (RtsFlags.ProfFlags.typeSelector) {
       b = str_matches_selector( (get_itbl((StgClosure *)p))->prof.closure_type,
                                RtsFlags.ProfFlags.typeSelector );
       if (!b) return rtsFalse;
   }
   if (RtsFlags.ProfFlags.ccSelector) {
       b = str_matches_selector( ((StgClosure *)p)->header.prof.ccs->cc->label,
				 RtsFlags.ProfFlags.ccSelector );
       if (!b) return rtsFalse;
   }
   return rtsTrue;
}
#endif /* PROFILING */

/* -----------------------------------------------------------------------------
 * Code to perform a heap census.
 * -------------------------------------------------------------------------- */
static void
heapCensusChain( bdescr *bd )
{
    StgPtr p;
    StgInfoTable *info;
    nat size;
#ifdef PROFILING
    nat real_size;
#endif

    for (; bd != NULL; bd = bd->link) {
	p = bd->start;
	while (p < bd->free) {
	    info = get_itbl((StgClosure *)p);
	    
	    switch (info->type) {

	    case CONSTR:
	    case BCO:
	    case FUN:
	    case THUNK:
	    case IND_PERM:
	    case IND_OLDGEN_PERM:
	    case CAF_BLACKHOLE:
	    case SE_CAF_BLACKHOLE:
	    case SE_BLACKHOLE:
	    case BLACKHOLE:
	    case BLACKHOLE_BQ:
	    case WEAK:
	    case FOREIGN:
	    case STABLE_NAME:
	    case MVAR:
	    case MUT_VAR:
	    case MUT_CONS:
	    case CONSTR_INTLIKE:
	    case CONSTR_CHARLIKE:
	    case FUN_1_0:
	    case FUN_0_1:
	    case FUN_1_1:
	    case FUN_0_2:
	    case FUN_2_0:
	    case THUNK_1_1:
	    case THUNK_0_2:
	    case THUNK_2_0:
	    case CONSTR_1_0:
	    case CONSTR_0_1:
	    case CONSTR_1_1:
	    case CONSTR_0_2:
	    case CONSTR_2_0:
		size = sizeW_fromITBL(info);
		break;
		
	    case THUNK_1_0:		/* ToDo - shouldn't be here */
	    case THUNK_0_1:		/* "  ditto  " */
	    case THUNK_SELECTOR:
		size = sizeofW(StgHeader) + MIN_UPD_SIZE;
		break;

	    case PAP:
	    case AP_UPD:
		size = pap_sizeW((StgPAP *)p);
		break;
		
	    case ARR_WORDS:
		size = arr_words_sizeW(stgCast(StgArrWords*,p));
		break;
		
	    case MUT_ARR_PTRS:
	    case MUT_ARR_PTRS_FROZEN:
		size = mut_arr_ptrs_sizeW((StgMutArrPtrs *)p);
		break;
		
	    case TSO:
		size = tso_sizeW((StgTSO *)p);
		break;
		
	    default:
		barf("heapCensus");
	    }
	    
#ifdef DEBUG_HEAP_PROF
	    switch (RtsFlags.ProfFlags.doHeapProfile) {
	    case HEAP_BY_INFOPTR:
		add_data((void *)(*p), size * sizeof(W_));
		break;
	    case HEAP_BY_CLOSURE_TYPE:
		closure_types[info->type] += size * sizeof(W_);
		break;
	    }
#endif
	    
#ifdef PROFILING
	    // subtract the profiling overhead
	    real_size = size - sizeofW(StgProfHeader);

	    if (closureSatisfiesConstraints((StgClosure*)p)) {
		switch (RtsFlags.ProfFlags.doHeapProfile) {
		case HEAP_BY_CCS:
		    ((StgClosure *)p)->header.prof.ccs->mem_resid += real_size;
		    break;
		case HEAP_BY_MOD:
		    strToCtr(((StgClosure *)p)->header.prof.ccs->cc->module)
			->mem_resid += real_size;
		    break;
		case HEAP_BY_DESCR:
		    strToCtr(get_itbl(((StgClosure *)p))->prof.closure_desc)->mem_resid 
			+= real_size;
		    break;
		case HEAP_BY_TYPE:
		    strToCtr(get_itbl(((StgClosure *)p))->prof.closure_type)->mem_resid
			+= real_size;
		    break;
		default:
		    barf("heapCensus; doHeapProfile");
		}
	    }
#endif
	    p += size;
	}
    }
}

void
heapCensus( void )
{
  StgDouble time;
  nat g, s;
    
#ifdef DEBUG_HEAP_PROF
  switch (RtsFlags.ProfFlags.doHeapProfile) {
  case HEAP_BY_INFOPTR:
    clear_table_data();
    break;
  case HEAP_BY_CLOSURE_TYPE:
#if 0
#   error fix me      
    memset(closure_types, 0, N_CLOSURE_TYPES * sizeof(nat));
#endif
    break;
  default:
    return;
  }
#endif

#ifdef PROFILING
  switch (RtsFlags.ProfFlags.doHeapProfile) {
  case NO_HEAP_PROFILING:
      return;
  case HEAP_BY_CCS:
      /* zero all the residency counters */
      clearCCSResid(CCS_MAIN);
      break;
  case HEAP_BY_MOD:
  case HEAP_BY_DESCR:
  case HEAP_BY_TYPE:
      clearCtrResid();
      break;
  default:
      barf("heapCensus; doHeapProfile");
  }
#endif

  time = mut_user_time_during_GC();  
  fprintf(hp_file, "BEGIN_SAMPLE %0.2f\n", time);

  if (RtsFlags.GcFlags.generations == 1) {
      heapCensusChain( g0s0->to_blocks );
  } else {
      for (g = 0; g < RtsFlags.GcFlags.generations; g++) {
	  for (s = 0; s < generations[g].n_steps; s++) {
	      heapCensusChain( generations[g].steps[s].blocks );
	  }
      }
  }

#ifdef DEBUG_HEAP_PROF
  switch (RtsFlags.ProfFlags.doHeapProfile) {
  case HEAP_BY_INFOPTR:
      fprint_data(hp_file);
      break;
  case HEAP_BY_CLOSURE_TYPE:
      fprint_closure_types(hp_file);
      break;
  }
#endif
    
#ifdef PROFILING
  switch (RtsFlags.ProfFlags.doHeapProfile) {
  case HEAP_BY_CCS:
      reportCCSResid(hp_file,CCS_MAIN);
      break;
  case HEAP_BY_MOD:
  case HEAP_BY_DESCR:
  case HEAP_BY_TYPE:
      reportCtrResid(hp_file);
      break;
  default:
      barf("heapCensus; doHeapProfile");
  }
#endif

  fprintf(hp_file, "END_SAMPLE %0.2f\n", time);
}    

#endif /* PROFILING || DEBUG_HEAP_PROF */

