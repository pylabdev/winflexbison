/* Top level entry point of Bison.

   Copyright (C) 1984, 1986, 1989, 1992, 1995, 2000-2002, 2004-2015,
   2018-2019 Free Software Foundation, Inc.

   This file is part of Bison, the GNU Compiler Compiler.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include <config.h>
#include "system.h"

#include <bitset.h>
#include <bitset_stats.h>
//#include <configmake.h>
#include <progname.h>
#include <quotearg.h>
#include <relocatable.h> /* relocate2 */
#include <timevar.h>

#include "LR0.h"
#include "closeout.h"
#include "complain.h"
#include "conflicts.h"
#include "derives.h"
#include "files.h"
#include "fixits.h"
#include "getargs.h"
#include "gram.h"
#include "lalr.h"
#include "ielr.h"
#include "muscle-tab.h"
#include "nullable.h"
#include "output.h"
#include "print.h"
#include "print_graph.h"
#include "print-xml.h"
#include <quote.h>
#include "reader.h"
#include "reduce.h"
#include "scan-code.h"
#include "scan-gram.h"
#include "scan-skel.h"
#include "symtab.h"
#include "tables.h"
#include "uniqstr.h"

/* extracts basename from path, optionally stripping the extension "\.*"
 * (same concept as /bin/sh `basename`, but different handling of extension). */
static char *basename2 (path, strip_ext)
     char   *path;
     int strip_ext;		/* boolean */
{
	char   *b, *e = 0;

	b = path;
	for (b = path; *path; path++)
		if (*path == '/')
			b = path + 1;
		else if (*path == '\\')
			b = path + 1;
		else if (*path == '.')
			e = path;

	if (strip_ext && e && e > b)
		*e = '\0';
	return b;
}

extern const char* get_app_path();

char* get_local_pkgdatadir()
{
	const char* program_path = 0;
	const char* dir = 0;
	const char* last_divider = 0;
	char* local_pkgdatadir = NULL;
	size_t dir_len = 0;
	size_t local_pkgdatadir_len = 0;

	program_path = dir = get_app_path();

	while (*dir)
	{
		if (*dir == '\\' || *dir == '/')
			last_divider = dir;
		++dir;
	}

	if (!last_divider)
		return PKGDATADIR;

	++last_divider;

	dir_len = last_divider - program_path;
	local_pkgdatadir_len = dir_len+strlen(PKGDATADIR);
	local_pkgdatadir = (char*)malloc((local_pkgdatadir_len+1)*sizeof(char));
	strncpy(local_pkgdatadir, program_path, dir_len);
	strcpy(&local_pkgdatadir[dir_len], PKGDATADIR);

	return local_pkgdatadir;
}

char* local_pkgdatadir = 0;

int
main (int argc, char *argv[])
{
#define DEPENDS_ON_LIBINTL 1
  set_program_name (argv[0]);
  program_name = basename2 (argv[0], 1);
  local_pkgdatadir = get_local_pkgdatadir();

  setlocale (LC_ALL, "");
  {
    char *cp = NULL;
    char const *localedir = relocate2 (LOCALEDIR, &cp);
    (void) bindtextdomain (PACKAGE, localedir);
    (void) bindtextdomain ("bison-runtime", localedir);
    free (cp);
  }
  (void) textdomain (PACKAGE);

  {
    char const *cp = getenv ("LC_CTYPE");
    if (cp && STREQ (cp, "C"))
      set_custom_quoting (&quote_quoting_options, "'", "'");
    else
      set_quoting_style (&quote_quoting_options, locale_quoting_style);
  }

  atexit (close_stdout);

  uniqstrs_new ();
  muscle_init ();
  complain_init ();

  getargs (argc, argv);

  timevar_enabled = trace_flag & trace_time;
  timevar_init ();
  timevar_start (tv_total);

  if (trace_flag & trace_bitsets)
    bitset_stats_enable ();

  /* Read the input.  Copy some parts of it to FGUARD, FACTION, FTABLE
     and FATTRS.  In file reader.c.  The other parts are recorded in
     the grammar; see gram.h.  */

  timevar_push (tv_reader);
  reader ();
  timevar_pop (tv_reader);

  if (complaint_status == status_complaint)
    goto finish;

  /* Find useless nonterminals and productions and reduce the grammar. */
  timevar_push (tv_reduce);
  reduce_grammar ();
  timevar_pop (tv_reduce);

  /* Record other info about the grammar.  In files derives and
     nullable.  */
  timevar_push (tv_sets);
  derives_compute ();
  nullable_compute ();
  timevar_pop (tv_sets);

  /* Compute LR(0) parser states.  See state.h for more info.  */
  timevar_push (tv_lr0);
  generate_states ();
  timevar_pop (tv_lr0);

  /* Add lookahead sets to parser states.  Except when LALR(1) is
     requested, split states to eliminate LR(1)-relative
     inadequacies.  */
  ielr ();

  /* Find and record any conflicts: places where one token of
     lookahead is not enough to disambiguate the parsing.  In file
     conflicts.  Also resolve s/r conflicts based on precedence
     declarations.  */
  timevar_push (tv_conflicts);
  conflicts_solve ();
  if (!muscle_percent_define_flag_if ("lr.keep-unreachable-state"))
    {
      state_number *old_to_new = xnmalloc (nstates, sizeof *old_to_new);
      state_number nstates_old = nstates;
      state_remove_unreachable_states (old_to_new);
      lalr_update_state_numbers (old_to_new, nstates_old);
      conflicts_update_state_numbers (old_to_new, nstates_old);
      free (old_to_new);
    }
  conflicts_print ();
  timevar_pop (tv_conflicts);

  /* Compute the parser tables.  */
  timevar_push (tv_actions);
  tables_generate ();
  timevar_pop (tv_actions);

  grammar_rules_useless_report (_("rule useless in parser due to conflicts"));

  print_precedence_warnings ();

  if (!update_flag)
    {
      /* Output file names. */
      compute_output_file_names ();

      /* Output the detailed report on the grammar.  */
      if (report_flag)
        {
          timevar_push (tv_report);
          print_results ();
          timevar_pop (tv_report);
        }

      /* Output the graph.  */
      if (graph_flag)
        {
          timevar_push (tv_graph);
          print_graph ();
          timevar_pop (tv_graph);
        }

      /* Output xml.  */
      if (xml_flag)
        {
          timevar_push (tv_xml);
          print_xml ();
          timevar_pop (tv_xml);
        }
    }

  /* Stop if there were errors, to avoid trashing previous output
     files.  */
  if (complaint_status == status_complaint)
    goto finish;

  /* Lookahead tokens are no longer needed. */
  timevar_push (tv_free);
  lalr_free ();
  timevar_pop (tv_free);

  /* Output the tables and the parser to ftable.  In file output.  */
  if (!update_flag)
    {
      timevar_push (tv_parser);
      output ();
      timevar_pop (tv_parser);
    }

  timevar_push (tv_free);
  nullable_free ();
  derives_free ();
  tables_free ();
  states_free ();
  reduce_free ();
  conflicts_free ();
  grammar_free ();
  output_file_names_free ();

  /* The scanner memory cannot be released right after parsing, as it
     contains things such as user actions, prologue, epilogue etc.  */
  gram_scanner_free ();
  muscle_free ();
  code_scanner_free ();
  skel_scanner_free ();
  quotearg_free ();
  timevar_pop (tv_free);

  if (trace_flag & trace_bitsets)
    bitset_stats_dump (stderr);

 finish:
  free(local_pkgdatadir);

  /* Stop timing and print the times.  */
  timevar_stop (tv_total);
  timevar_print (stderr);

  cleanup_caret ();

  /* Fix input file now, even if there are errors: that's less
     warnings in the following runs.  */
  if (!fixits_empty ())
    {
      if (update_flag)
        fixits_run ();
      else
        complain (NULL, Wother,
                  _("fix-its can be applied.  Rerun with option '--update'."));
      fixits_free ();
    }
  uniqstrs_free ();

  return complaint_status ? EXIT_FAILURE : EXIT_SUCCESS;
}
