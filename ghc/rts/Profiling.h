/* -----------------------------------------------------------------------------

 * $Id: Profiling.h,v 1.3 2001/11/22 14:25:12 simonmar Exp $
 *
 * (c) The GHC Team, 1998-2000
 *
 * Support for profiling
 *
 * ---------------------------------------------------------------------------*/

#if defined(PROFILING) || defined(DEBUG)
void initProfiling1 ( void );
void initProfiling2 ( void );
void endProfiling   ( void );

extern FILE *prof_file;
extern FILE *hp_file;
#endif

#ifdef PROFILING

void gen_XML_logfile     ( void );
void report_ccs_profiling ( void );
void heap_profile_finish (void);

void PrintNewStackDecls ( void );

void print_ccs (FILE *, CostCentreStack *);

# define DEFAULT_INTERVAL TICK_FREQUENCY

extern rtsBool time_profiling;

extern lnat total_prof_ticks;

extern void fprintCCS( FILE *f, CostCentreStack *ccs );


#endif
