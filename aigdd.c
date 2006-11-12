#include "aiger.h"

#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static aiger * src;
static const char * dst_name;
static unsigned * stable;
static unsigned * unstable;
static int verbose;

static void
msg (const char * fmt, ...)
{
  va_list ap;
  if (!verbose)
    return;
  fputs ("[aigdd] ", stderr);
  va_start (ap, fmt);
  vfprintf (stderr, fmt, ap);
  va_end (ap);
  fputc ('\n', stderr);
  fflush (stderr);
}

static void
die (const char * fmt, ...)
{
  va_list ap;
  fputs ("*** [aigdd] ", stderr);
  va_start (ap, fmt);
  vfprintf (stderr, fmt, ap);
  va_end (ap);
  fputc ('\n', stderr);
  fflush (stderr);
  exit (1);
}

static unsigned
deref (unsigned lit)
{
  unsigned sign = lit & 1;
  unsigned idx = lit / 2;
  assert (idx <= src->maxvar);
  return unstable[idx] ^ sign;
}

static void
write_unstable_to_dst (void)
{
  aiger_symbol * symbol;
  aiger_and * and;
  unsigned i, lit;
  aiger * dst;
  
  dst = aiger_init ();

  for (i = 0; i < src->num_inputs; i++)
    {
      symbol = src->inputs + i;
      lit = symbol->lit;
      if (deref (lit) == lit)
	aiger_add_input (dst, lit, symbol->name);
    }

  for (i = 0; i < src->num_latches; i++)
    {
      symbol = src->latches + i;
      lit = symbol->lit;
      if (deref (lit) == lit)
	aiger_add_latch (dst, lit, deref (symbol->next), symbol->name);
    }

  for (i = 0; i < src->num_ands; i++)
    {
      and = src->ands + i;
      if (deref (and->lhs) == and->lhs)
	aiger_add_and (dst, and->lhs, deref (and->rhs0), deref (and->rhs1));
    }

  for (i = 0; i < src->num_outputs; i++)
    {
      symbol = src->outputs + i;
      aiger_add_output (dst, deref (symbol->lit), symbol->name);
    }

  assert (!aiger_check (dst));

  unlink (dst_name);
  if (!aiger_open_and_write_to_file (dst, dst_name))
    die ("failed to write '%s'", dst_name);
  aiger_reset (dst);
}

static void
copy_stable_to_unstable (void)
{
  unsigned i;

  for (i = 0; i <= src->maxvar; i++)
    unstable[i] = stable[i];
}

#define USAGE \
  "usage: aigdd src dst [run]\n"

#if 0
#define CMD "%s %s 1>/dev/null 2>/dev/null"
#else
#define CMD "%s %s"
#endif

int
main (int argc, char ** argv)
{
  const char * src_name, * run_name, * err;
  int i, changed, delta, j, expected, res;
  char * cmd;

  src_name = dst_name = run_name = 0;

  for (i = 1; i < argc; i++)
    {
      if (!strcmp (argv[i], "-h"))
	{
	  fprintf (stderr, USAGE);
	  exit (0);
	}
      else if (!strcmp (argv[i], "-v"))
	{
	  verbose = 1;
	}
      else if (src_name && dst_name && run_name)
	die ("more than three files");
      else if (dst_name)
	run_name = argv[i];
      else if (src_name)
	dst_name = argv[i];
      else
	src_name = argv[i];
    }

  if (!src_name || !dst_name)
    die ("expected exactly two files");

  if (!run_name)
    run_name = "./run";

  cmd = malloc (strlen (src_name) + strlen (run_name) + strlen (CMD) + 1);
  sprintf (cmd, CMD, run_name, src_name);
  expected = system (cmd);
  msg ("'%s' returns %d", cmd, expected);
  free (cmd);

  cmd = malloc (strlen (dst_name) + strlen (run_name) + strlen (CMD) + 1);
  sprintf (cmd, CMD, run_name, dst_name);

  stable = malloc (sizeof (stable[0]) * (src->maxvar + 1));
  unstable = malloc (sizeof (unstable[0]) * (src->maxvar + 1));

  for (i = 0; i <= src->maxvar; i++)
    stable[i] = 2 * i;

  copy_stable_to_unstable ();
  write_unstable_to_dst ();

  res = system (cmd);
  if (res != expected)
    die ("different return value (%d instead of %d)", res, expected);

  src = aiger_init ();
  if ((err = aiger_open_and_read_from_file (src, src_name)))
    die ("%s: %s", src_name, err);

  for (delta = src->maxvar; delta; delta /= 2)
    {
      i = 1;

      do {
	for (j = 1; j < i; j++)
	  unstable[j] = stable[j];

	changed = 0;
	for (j = i; j < i + delta && j <= src->maxvar; j++)
	  {
	    if (stable[j])		/* replace '1' by '0' as well */
	      {
		unstable[j] = 0;
		changed++;
	      }
	    else
	      unstable[j] = 0;		/* always favor 'zero' */
	  }

	if (changed)
	  {
	    for (j = i + delta; j <= src->maxvar; j++)
	      unstable[j] = stable[j];

	    write_unstable_to_dst ();
	    res = system (cmd);
	    if (res == expected)
	      {
		msg ("[%d,%d] set to 0 (%d out of %d)",
		     i, i + delta - 1, changed, delta);

		for (j = i; j < i + delta && j <= src->maxvar; j++)
		  stable[j] = unstable[j];
	      }
	    else			/* try setting to 'one' */
	      {
		msg ("[%d,%d] can not be set to 0 (%d out of %d)",
		     i, i + delta - 1, changed, delta);

		for (j = 1; j < i; j++)
		  unstable[j] = stable[j];

		changed = 0;
		for (j = i; j < i + delta && j <= src->maxvar; j++)
		  {
		    if (stable[j])
		      {
			if (stable[j] > 1)
			  {
			    unstable[j] = 1;
			    changed++;
			  }
			else
			  unstable[j] = 1;
		      }
		    else
		      unstable[j] = 0;	/* always favor '0' */
		  }

		if (changed)
		  {
		    for (j = i + delta; j <= src->maxvar; j++)
		      unstable[j] = stable[j];

		    write_unstable_to_dst ();
		    res = system (cmd);
		    if (res == expected)
		      {
			msg ("[%d,%d] set to 1 (%d out of %d)",
			     i, i + delta - 1, changed, delta);

			for (j = i; j < i + delta && j <= src->maxvar; j++)
			  stable[j] = unstable[j];
		      }
		    else
		      msg ("[%d,%d] can neither set to 1 (%d out of %d)",
			   i, i + delta - 1, changed, delta);
		  }
	      }
	  }
	else
	  msg ("[%d,%d] stabilized to 0", i, i + delta - 1);

	i += delta;
      } while (i <= src->maxvar);
    }

  copy_stable_to_unstable ();
  write_unstable_to_dst ();

  changed = 0;
  for (i = 1; i <= src->maxvar; i++)
    if (stable[i] <= 1)
      changed++;

  msg ("changed %d", changed);

  free (stable);
  free (unstable);
  free (cmd);
  aiger_reset (src);

  return 0;
}