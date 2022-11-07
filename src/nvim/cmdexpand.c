// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

// cmdexpand.c: functions for command-line completion

#include "nvim/api/private/helpers.h"
#include "nvim/arglist.h"
#include "nvim/ascii.h"
#include "nvim/buffer.h"
#include "nvim/charset.h"
#include "nvim/cmdexpand.h"
#include "nvim/cmdhist.h"
#include "nvim/drawscreen.h"
#include "nvim/eval.h"
#include "nvim/eval/funcs.h"
#include "nvim/eval/userfunc.h"
#include "nvim/ex_cmds.h"
#include "nvim/ex_cmds2.h"
#include "nvim/ex_docmd.h"
#include "nvim/ex_getln.h"
#include "nvim/garray.h"
#include "nvim/getchar.h"
#include "nvim/help.h"
#include "nvim/highlight_group.h"
#include "nvim/locale.h"
#include "nvim/lua/executor.h"
#include "nvim/mapping.h"
#include "nvim/menu.h"
#include "nvim/option.h"
#include "nvim/os/os.h"
#include "nvim/popupmenu.h"
#include "nvim/profile.h"
#include "nvim/regexp.h"
#include "nvim/screen.h"
#include "nvim/search.h"
#include "nvim/sign.h"
#include "nvim/strings.h"
#include "nvim/syntax.h"
#include "nvim/tag.h"
#include "nvim/types.h"
#include "nvim/ui.h"
#include "nvim/usercmd.h"
#include "nvim/vim.h"
#include "nvim/window.h"

/// Type used by ExpandGeneric()
typedef char *(*CompleteListItemGetter)(expand_T *, int);

/// Type used by call_user_expand_func
typedef void *(*user_expand_func_T)(const char_u *, int, typval_T *);

#ifdef INCLUDE_GENERATED_DECLARATIONS
# include "cmdexpand.c.generated.h"
#endif

static int cmd_showtail;  ///< Only show path tail in lists ?

/// "compl_match_array" points the currently displayed list of entries in the
/// popup menu.  It is NULL when there is no popup menu.
static pumitem_T *compl_match_array = NULL;
static int compl_match_arraysize;
/// First column in cmdline of the matched item for completion.
static int compl_startcol;
static int compl_selected;

static int sort_func_compare(const void *s1, const void *s2)
{
  char *p1 = *(char **)s1;
  char *p2 = *(char **)s2;

  if (*p1 != '<' && *p2 == '<') {
    return -1;
  }
  if (*p1 == '<' && *p2 != '<') {
    return 1;
  }
  return strcmp(p1, p2);
}

static void ExpandEscape(expand_T *xp, char_u *str, int numfiles, char **files, int options)
{
  int i;
  char_u *p;
  const int vse_what = xp->xp_context == EXPAND_BUFFERS ? VSE_BUFFER : VSE_NONE;

  // May change home directory back to "~"
  if (options & WILD_HOME_REPLACE) {
    tilde_replace(str, numfiles, files);
  }

  if (options & WILD_ESCAPE) {
    if (xp->xp_context == EXPAND_FILES
        || xp->xp_context == EXPAND_FILES_IN_PATH
        || xp->xp_context == EXPAND_SHELLCMD
        || xp->xp_context == EXPAND_BUFFERS
        || xp->xp_context == EXPAND_DIRECTORIES) {
      // Insert a backslash into a file name before a space, \, %, #
      // and wildmatch characters, except '~'.
      for (i = 0; i < numfiles; i++) {
        // for ":set path=" we need to escape spaces twice
        if (xp->xp_backslash == XP_BS_THREE) {
          p = vim_strsave_escaped((char_u *)files[i], (char_u *)" ");
          xfree(files[i]);
          files[i] = (char *)p;
#if defined(BACKSLASH_IN_FILENAME)
          p = vim_strsave_escaped(files[i], (char_u *)" ");
          xfree(files[i]);
          files[i] = p;
#endif
        }
#ifdef BACKSLASH_IN_FILENAME
        p = (char_u *)vim_strsave_fnameescape((const char *)files[i], vse_what);
#else
        p = (char_u *)vim_strsave_fnameescape((const char *)files[i],
                                              xp->xp_shell ? VSE_SHELL : vse_what);
#endif
        xfree(files[i]);
        files[i] = (char *)p;

        // If 'str' starts with "\~", replace "~" at start of
        // files[i] with "\~".
        if (str[0] == '\\' && str[1] == '~' && files[i][0] == '~') {
          escape_fname(&files[i]);
        }
      }
      xp->xp_backslash = XP_BS_NONE;

      // If the first file starts with a '+' escape it.  Otherwise it
      // could be seen as "+cmd".
      if (*files[0] == '+') {
        escape_fname(&files[0]);
      }
    } else if (xp->xp_context == EXPAND_TAGS) {
      // Insert a backslash before characters in a tag name that
      // would terminate the ":tag" command.
      for (i = 0; i < numfiles; i++) {
        p = vim_strsave_escaped((char_u *)files[i], (char_u *)"\\|\"");
        xfree(files[i]);
        files[i] = (char *)p;
      }
    }
  }
}

/// Return FAIL if this is not an appropriate context in which to do
/// completion of anything, return OK if it is (even if there are no matches).
/// For the caller, this means that the character is just passed through like a
/// normal character (instead of being expanded).  This allows :s/^I^D etc.
///
/// @param options  extra options for ExpandOne()
/// @param escape  if true, escape the returned matches
int nextwild(expand_T *xp, int type, int options, bool escape)
{
  CmdlineInfo *const ccline = get_cmdline_info();
  int i, j;
  char_u *p1;
  char_u *p2;
  int difflen;

  if (xp->xp_numfiles == -1) {
    set_expand_context(xp);
    cmd_showtail = expand_showtail(xp);
  }

  if (xp->xp_context == EXPAND_UNSUCCESSFUL) {
    beep_flush();
    return OK;      // Something illegal on command line
  }
  if (xp->xp_context == EXPAND_NOTHING) {
    // Caller can use the character as a normal char instead
    return FAIL;
  }

  if (!(ui_has(kUICmdline) || ui_has(kUIWildmenu))) {
    msg_puts("...");  // show that we are busy
    ui_flush();
  }

  i = (int)(xp->xp_pattern - ccline->cmdbuff);
  assert(ccline->cmdpos >= i);
  xp->xp_pattern_len = (size_t)ccline->cmdpos - (size_t)i;

  if (type == WILD_NEXT || type == WILD_PREV || type == WILD_PUM_WANT) {
    // Get next/previous match for a previous expanded pattern.
    p2 = (char_u *)ExpandOne(xp, NULL, NULL, 0, type);
  } else {
    // Translate string into pattern and expand it.
    p1 = (char_u *)addstar(xp->xp_pattern, xp->xp_pattern_len, xp->xp_context);
    const int use_options = (options
                             | WILD_HOME_REPLACE
                             | WILD_ADD_SLASH
                             | WILD_SILENT
                             | (escape ? WILD_ESCAPE : 0)
                             | (p_wic ? WILD_ICASE : 0));
    p2 = (char_u *)ExpandOne(xp, (char *)p1, xstrnsave(&ccline->cmdbuff[i], xp->xp_pattern_len),
                             use_options, type);
    xfree(p1);

    // xp->xp_pattern might have been modified by ExpandOne (for example,
    // in lua completion), so recompute the pattern index and length
    i = (int)(xp->xp_pattern - ccline->cmdbuff);
    xp->xp_pattern_len = (size_t)ccline->cmdpos - (size_t)i;

    // Longest match: make sure it is not shorter, happens with :help.
    if (p2 != NULL && type == WILD_LONGEST) {
      for (j = 0; (size_t)j < xp->xp_pattern_len; j++) {
        if (ccline->cmdbuff[i + j] == '*'
            || ccline->cmdbuff[i + j] == '?') {
          break;
        }
      }
      if ((int)STRLEN(p2) < j) {
        XFREE_CLEAR(p2);
      }
    }
  }

  if (p2 != NULL && !got_int) {
    difflen = (int)STRLEN(p2) - (int)(xp->xp_pattern_len);
    if (ccline->cmdlen + difflen + 4 > ccline->cmdbufflen) {
      realloc_cmdbuff(ccline->cmdlen + difflen + 4);
      xp->xp_pattern = ccline->cmdbuff + i;
    }
    assert(ccline->cmdpos <= ccline->cmdlen);
    memmove(&ccline->cmdbuff[ccline->cmdpos + difflen],
            &ccline->cmdbuff[ccline->cmdpos],
            (size_t)ccline->cmdlen - (size_t)ccline->cmdpos + 1);
    memmove(&ccline->cmdbuff[i], p2, STRLEN(p2));
    ccline->cmdlen += difflen;
    ccline->cmdpos += difflen;
  }
  xfree(p2);

  redrawcmd();
  cursorcmd();

  // When expanding a ":map" command and no matches are found, assume that
  // the key is supposed to be inserted literally
  if (xp->xp_context == EXPAND_MAPPINGS && p2 == NULL) {
    return FAIL;
  }

  if (xp->xp_numfiles <= 0 && p2 == NULL) {
    beep_flush();
  } else if (xp->xp_numfiles == 1) {
    // free expanded pattern
    (void)ExpandOne(xp, NULL, NULL, 0, WILD_FREE);
  }

  return OK;
}

void cmdline_pum_display(bool changed_array)
{
  pum_display(compl_match_array, compl_match_arraysize, compl_selected,
              changed_array, compl_startcol);
}

bool cmdline_pum_active(void)
{
  // compl_match_array != NULL should already imply pum_visible() in Nvim.
  return compl_match_array != NULL;
}

/// Remove the cmdline completion popup menu
void cmdline_pum_remove(void)
{
  pum_undisplay(true);
  XFREE_CLEAR(compl_match_array);
}

void cmdline_pum_cleanup(CmdlineInfo *cclp)
{
  cmdline_pum_remove();
  wildmenu_cleanup(cclp);
}

/// Get the next or prev cmdline completion match. The index of the match is set
/// in "p_findex"
static char *get_next_or_prev_match(int mode, expand_T *xp, int *p_findex, char *orig_save)
{
  if (xp->xp_numfiles <= 0) {
    return NULL;
  }

  int findex = *p_findex;

  if (mode == WILD_PREV) {
    if (findex == -1) {
      findex = xp->xp_numfiles;
    }
    findex--;
  } else if (mode == WILD_NEXT) {
    findex++;
  } else {  // mode == WILD_PUM_WANT
    assert(pum_want.active);
    findex = pum_want.item;
  }

  // When wrapping around, return the original string, set findex to -1.
  if (findex < 0) {
    if (orig_save == NULL) {
      findex = xp->xp_numfiles - 1;
    } else {
      findex = -1;
    }
  }
  if (findex >= xp->xp_numfiles) {
    if (orig_save == NULL) {
      findex = 0;
    } else {
      findex = -1;
    }
  }
  if (compl_match_array) {
    compl_selected = findex;
    cmdline_pum_display(false);
  } else if (p_wmnu) {
    redraw_wildmenu(xp, xp->xp_numfiles, xp->xp_files, findex, cmd_showtail);
  }
  *p_findex = findex;

  return xstrdup(findex == -1 ? orig_save : xp->xp_files[findex]);
}

/// Start the command-line expansion and get the matches.
static char *ExpandOne_start(int mode, expand_T *xp, char *str, int options)
{
  int non_suf_match;  // number without matching suffix
  char *ss = NULL;

  // Do the expansion.
  if (ExpandFromContext(xp, (char_u *)str, &xp->xp_numfiles, &xp->xp_files, options) == FAIL) {
#ifdef FNAME_ILLEGAL
    // Illegal file name has been silently skipped.  But when there
    // are wildcards, the real problem is that there was no match,
    // causing the pattern to be added, which has illegal characters.
    if (!(options & WILD_SILENT) && (options & WILD_LIST_NOTFOUND)) {
      semsg(_(e_nomatch2), str);
    }
#endif
  } else if (xp->xp_numfiles == 0) {
    if (!(options & WILD_SILENT)) {
      semsg(_(e_nomatch2), str);
    }
  } else {
    // Escape the matches for use on the command line.
    ExpandEscape(xp, (char_u *)str, xp->xp_numfiles, xp->xp_files, options);

    // Check for matching suffixes in file names.
    if (mode != WILD_ALL && mode != WILD_ALL_KEEP
        && mode != WILD_LONGEST) {
      if (xp->xp_numfiles) {
        non_suf_match = xp->xp_numfiles;
      } else {
        non_suf_match = 1;
      }
      if ((xp->xp_context == EXPAND_FILES
           || xp->xp_context == EXPAND_DIRECTORIES)
          && xp->xp_numfiles > 1) {
        // More than one match; check suffix.
        // The files will have been sorted on matching suffix in
        // expand_wildcards, only need to check the first two.
        non_suf_match = 0;
        for (int i = 0; i < 2; i++) {
          if (match_suffix(xp->xp_files[i])) {
            non_suf_match++;
          }
        }
      }
      if (non_suf_match != 1) {
        // Can we ever get here unless it's while expanding
        // interactively?  If not, we can get rid of this all
        // together. Don't really want to wait for this message
        // (and possibly have to hit return to continue!).
        if (!(options & WILD_SILENT)) {
          emsg(_(e_toomany));
        } else if (!(options & WILD_NO_BEEP)) {
          beep_flush();
        }
      }
      if (!(non_suf_match != 1 && mode == WILD_EXPAND_FREE)) {
        ss = xstrdup(xp->xp_files[0]);
      }
    }
  }

  return ss;
}

/// Return the longest common part in the list of cmdline completion matches.
static char *find_longest_match(expand_T *xp, int options)
{
  size_t len = 0;

  for (size_t mb_len; xp->xp_files[0][len]; len += mb_len) {
    mb_len = (size_t)utfc_ptr2len(&xp->xp_files[0][len]);
    int c0 = utf_ptr2char(&xp->xp_files[0][len]);
    int i;
    for (i = 1; i < xp->xp_numfiles; i++) {
      int ci = utf_ptr2char(&xp->xp_files[i][len]);

      if (p_fic && (xp->xp_context == EXPAND_DIRECTORIES
                    || xp->xp_context == EXPAND_FILES
                    || xp->xp_context == EXPAND_SHELLCMD
                    || xp->xp_context == EXPAND_BUFFERS)) {
        if (mb_tolower(c0) != mb_tolower(ci)) {
          break;
        }
      } else if (c0 != ci) {
        break;
      }
    }
    if (i < xp->xp_numfiles) {
      if (!(options & WILD_NO_BEEP)) {
        vim_beep(BO_WILD);
      }
      break;
    }
  }

  return xstrndup(xp->xp_files[0], len);
}

/// Do wildcard expansion on the string "str".
/// Chars that should not be expanded must be preceded with a backslash.
/// Return a pointer to allocated memory containing the new string.
/// Return NULL for failure.
///
/// "orig" is the originally expanded string, copied to allocated memory.  It
/// should either be kept in orig_save or freed.  When "mode" is WILD_NEXT or
/// WILD_PREV "orig" should be NULL.
///
/// Results are cached in xp->xp_files and xp->xp_numfiles, except when "mode"
/// is WILD_EXPAND_FREE or WILD_ALL.
///
/// mode = WILD_FREE:        just free previously expanded matches
/// mode = WILD_EXPAND_FREE: normal expansion, do not keep matches
/// mode = WILD_EXPAND_KEEP: normal expansion, keep matches
/// mode = WILD_NEXT:        use next match in multiple match, wrap to first
/// mode = WILD_PREV:        use previous match in multiple match, wrap to first
/// mode = WILD_ALL:         return all matches concatenated
/// mode = WILD_LONGEST:     return longest matched part
/// mode = WILD_ALL_KEEP:    get all matches, keep matches
/// mode = WILD_APPLY:       apply the item selected in the cmdline completion
///                          popup menu and close the menu.
/// mode = WILD_CANCEL:      cancel and close the cmdline completion popup and
///                          use the original text.
/// mode = WILD_PUM_WANT:    use the match at index pum_want.item
///
/// options = WILD_LIST_NOTFOUND:    list entries without a match
/// options = WILD_HOME_REPLACE:     do home_replace() for buffer names
/// options = WILD_USE_NL:           Use '\n' for WILD_ALL
/// options = WILD_NO_BEEP:          Don't beep for multiple matches
/// options = WILD_ADD_SLASH:        add a slash after directory names
/// options = WILD_KEEP_ALL:         don't remove 'wildignore' entries
/// options = WILD_SILENT:           don't print warning messages
/// options = WILD_ESCAPE:           put backslash before special chars
/// options = WILD_ICASE:            ignore case for files
///
/// The variables xp->xp_context and xp->xp_backslash must have been set!
///
/// @param orig  allocated copy of original of expanded string
char *ExpandOne(expand_T *xp, char *str, char *orig, int options, int mode)
{
  char *ss = NULL;
  static int findex;
  static char *orig_save = NULL;      // kept value of orig
  int orig_saved = false;
  int i;

  // first handle the case of using an old match
  if (mode == WILD_NEXT || mode == WILD_PREV || mode == WILD_PUM_WANT) {
    return get_next_or_prev_match(mode, xp, &findex, orig_save);
  }

  if (mode == WILD_CANCEL) {
    ss = xstrdup(orig_save ? orig_save : "");
  } else if (mode == WILD_APPLY) {
    ss = xstrdup(findex == -1
                 ? (orig_save ? orig_save : "")
                 : xp->xp_files[findex]);
  }

  // free old names
  if (xp->xp_numfiles != -1 && mode != WILD_ALL && mode != WILD_LONGEST) {
    FreeWild(xp->xp_numfiles, xp->xp_files);
    xp->xp_numfiles = -1;
    XFREE_CLEAR(orig_save);
  }
  findex = 0;

  if (mode == WILD_FREE) {      // only release file name
    return NULL;
  }

  if (xp->xp_numfiles == -1 && mode != WILD_APPLY && mode != WILD_CANCEL) {
    xfree(orig_save);
    orig_save = orig;
    orig_saved = true;

    ss = ExpandOne_start(mode, xp, str, options);
  }

  // Find longest common part
  if (mode == WILD_LONGEST && xp->xp_numfiles > 0) {
    ss = find_longest_match(xp, options);
    findex = -1;  // next p_wc gets first one
  }

  // Concatenate all matching names.  Unless interrupted, this can be slow
  // and the result probably won't be used.
  // TODO(philix): use xstpcpy instead of strcat in a loop (ExpandOne)
  if (mode == WILD_ALL && xp->xp_numfiles > 0 && !got_int) {
    size_t len = 0;
    for (i = 0; i < xp->xp_numfiles; i++) {
      len += strlen(xp->xp_files[i]) + 1;
    }
    ss = xmalloc(len);
    *ss = NUL;
    for (i = 0; i < xp->xp_numfiles; i++) {
      STRCAT(ss, xp->xp_files[i]);
      if (i != xp->xp_numfiles - 1) {
        STRCAT(ss, (options & WILD_USE_NL) ? "\n" : " ");
      }
    }
  }

  if (mode == WILD_EXPAND_FREE || mode == WILD_ALL) {
    ExpandCleanup(xp);
  }

  // Free "orig" if it wasn't stored in "orig_save".
  if (!orig_saved) {
    xfree(orig);
  }

  return ss;
}

/// Prepare an expand structure for use.
void ExpandInit(expand_T *xp)
  FUNC_ATTR_NONNULL_ALL
{
  CLEAR_POINTER(xp);
  xp->xp_backslash = XP_BS_NONE;
  xp->xp_numfiles = -1;
}

/// Cleanup an expand structure after use.
void ExpandCleanup(expand_T *xp)
{
  if (xp->xp_numfiles >= 0) {
    FreeWild(xp->xp_numfiles, xp->xp_files);
    xp->xp_numfiles = -1;
  }
}

/// Show all matches for completion on the command line.
/// Returns EXPAND_NOTHING when the character that triggered expansion should
/// be inserted like a normal character.
int showmatches(expand_T *xp, int wildmenu)
{
  CmdlineInfo *const ccline = get_cmdline_info();
#define L_SHOWFILE(m) (showtail \
                       ? sm_gettail(files_found[m], false) : files_found[m])
  int num_files;
  char **files_found;
  int i, j, k;
  int maxlen;
  int lines;
  int columns;
  char_u *p;
  int lastlen;
  int attr;
  int showtail;

  if (xp->xp_numfiles == -1) {
    set_expand_context(xp);
    i = expand_cmdline(xp, (char_u *)ccline->cmdbuff, ccline->cmdpos,
                       &num_files, &files_found);
    showtail = expand_showtail(xp);
    if (i != EXPAND_OK) {
      return i;
    }
  } else {
    num_files = xp->xp_numfiles;
    files_found = xp->xp_files;
    showtail = cmd_showtail;
  }

  bool compl_use_pum = (ui_has(kUICmdline)
                        ? ui_has(kUIPopupmenu)
                        : wildmenu && (wop_flags & WOP_PUM))
                       || ui_has(kUIWildmenu);

  if (compl_use_pum) {
    assert(num_files >= 0);
    compl_match_arraysize = num_files;
    compl_match_array = xmalloc(sizeof(pumitem_T) * (size_t)compl_match_arraysize);
    for (i = 0; i < num_files; i++) {
      compl_match_array[i] = (pumitem_T){
        .pum_text = (char_u *)L_SHOWFILE(i),
        .pum_info = NULL,
        .pum_extra = NULL,
        .pum_kind = NULL,
      };
    }
    char *endpos = (showtail ? sm_gettail(xp->xp_pattern, true) : xp->xp_pattern);
    if (ui_has(kUICmdline)) {
      compl_startcol = (int)(endpos - ccline->cmdbuff);
    } else {
      compl_startcol = cmd_screencol((int)(endpos - ccline->cmdbuff));
    }
    compl_selected = -1;
    cmdline_pum_display(true);
    return EXPAND_OK;
  }

  if (!wildmenu) {
    msg_didany = false;                 // lines_left will be set
    msg_start();                        // prepare for paging
    msg_putchar('\n');
    ui_flush();
    cmdline_row = msg_row;
    msg_didany = false;                 // lines_left will be set again
    msg_start();                        // prepare for paging
  }

  if (got_int) {
    got_int = false;            // only int. the completion, not the cmd line
  } else if (wildmenu) {
    redraw_wildmenu(xp, num_files, files_found, -1, showtail);
  } else {
    // find the length of the longest file name
    maxlen = 0;
    for (i = 0; i < num_files; i++) {
      if (!showtail && (xp->xp_context == EXPAND_FILES
                        || xp->xp_context == EXPAND_SHELLCMD
                        || xp->xp_context == EXPAND_BUFFERS)) {
        home_replace(NULL, files_found[i], (char *)NameBuff, MAXPATHL, true);
        j = vim_strsize((char *)NameBuff);
      } else {
        j = vim_strsize(L_SHOWFILE(i));
      }
      if (j > maxlen) {
        maxlen = j;
      }
    }

    if (xp->xp_context == EXPAND_TAGS_LISTFILES) {
      lines = num_files;
    } else {
      // compute the number of columns and lines for the listing
      maxlen += 2;          // two spaces between file names
      columns = (Columns + 2) / maxlen;
      if (columns < 1) {
        columns = 1;
      }
      lines = (num_files + columns - 1) / columns;
    }

    attr = HL_ATTR(HLF_D);      // find out highlighting for directories

    if (xp->xp_context == EXPAND_TAGS_LISTFILES) {
      msg_puts_attr(_("tagname"), HL_ATTR(HLF_T));
      msg_clr_eos();
      msg_advance(maxlen - 3);
      msg_puts_attr(_(" kind file\n"), HL_ATTR(HLF_T));
    }

    // list the files line by line
    for (i = 0; i < lines; i++) {
      lastlen = 999;
      for (k = i; k < num_files; k += lines) {
        if (xp->xp_context == EXPAND_TAGS_LISTFILES) {
          msg_outtrans_attr(files_found[k], HL_ATTR(HLF_D));
          p = (char_u *)files_found[k] + strlen(files_found[k]) + 1;
          msg_advance(maxlen + 1);
          msg_puts((const char *)p);
          msg_advance(maxlen + 3);
          msg_outtrans_long_attr((char *)p + 2, HL_ATTR(HLF_D));
          break;
        }
        for (j = maxlen - lastlen; --j >= 0;) {
          msg_putchar(' ');
        }
        if (xp->xp_context == EXPAND_FILES
            || xp->xp_context == EXPAND_SHELLCMD
            || xp->xp_context == EXPAND_BUFFERS) {
          // highlight directories
          if (xp->xp_numfiles != -1) {
            // Expansion was done before and special characters
            // were escaped, need to halve backslashes.  Also
            // $HOME has been replaced with ~/.
            char_u *exp_path = expand_env_save_opt((char_u *)files_found[k], true);
            char_u *path = exp_path != NULL ? exp_path : (char_u *)files_found[k];
            char_u *halved_slash = (char_u *)backslash_halve_save((char *)path);
            j = os_isdir((char *)halved_slash);
            xfree(exp_path);
            if (halved_slash != path) {
              xfree(halved_slash);
            }
          } else {
            // Expansion was done here, file names are literal.
            j = os_isdir(files_found[k]);
          }
          if (showtail) {
            p = (char_u *)L_SHOWFILE(k);
          } else {
            home_replace(NULL, files_found[k], (char *)NameBuff, MAXPATHL, true);
            p = (char_u *)NameBuff;
          }
        } else {
          j = false;
          p = (char_u *)L_SHOWFILE(k);
        }
        lastlen = msg_outtrans_attr((char *)p, j ? attr : 0);
      }
      if (msg_col > 0) {        // when not wrapped around
        msg_clr_eos();
        msg_putchar('\n');
      }
      if (got_int) {
        got_int = false;
        break;
      }
    }

    // we redraw the command below the lines that we have just listed
    // This is a bit tricky, but it saves a lot of screen updating.
    cmdline_row = msg_row;      // will put it back later
  }

  if (xp->xp_numfiles == -1) {
    FreeWild(num_files, files_found);
  }

  return EXPAND_OK;
}

/// Private path_tail for showmatches() (and redraw_wildmenu()):
/// Find tail of file name path, but ignore trailing "/".
char *sm_gettail(char *s, bool eager)
{
  char_u *p;
  char_u *t = (char_u *)s;
  bool had_sep = false;

  for (p = (char_u *)s; *p != NUL;) {
    if (vim_ispathsep(*p)
#ifdef BACKSLASH_IN_FILENAME
        && !rem_backslash(p)
#endif
        ) {
      if (eager) {
        t = p + 1;
      } else {
        had_sep = true;
      }
    } else if (had_sep) {
      t = p;
      had_sep = false;
    }
    MB_PTR_ADV(p);
  }
  return (char *)t;
}

/// Return true if we only need to show the tail of completion matches.
/// When not completing file names or there is a wildcard in the path false is
/// returned.
static bool expand_showtail(expand_T *xp)
{
  char_u *s;
  char_u *end;

  // When not completing file names a "/" may mean something different.
  if (xp->xp_context != EXPAND_FILES
      && xp->xp_context != EXPAND_SHELLCMD
      && xp->xp_context != EXPAND_DIRECTORIES) {
    return false;
  }

  end = (char_u *)path_tail(xp->xp_pattern);
  if (end == (char_u *)xp->xp_pattern) {          // there is no path separator
    return false;
  }

  for (s = (char_u *)xp->xp_pattern; s < end; s++) {
    // Skip escaped wildcards.  Only when the backslash is not a path
    // separator, on DOS the '*' "path\*\file" must not be skipped.
    if (rem_backslash((char *)s)) {
      s++;
    } else if (vim_strchr("*?[", *s) != NULL) {
      return false;
    }
  }
  return true;
}

/// Prepare a string for expansion.
///
/// When expanding file names: The string will be used with expand_wildcards().
/// Copy "fname[len]" into allocated memory and add a '*' at the end.
/// When expanding other names: The string will be used with regcomp().  Copy
/// the name into allocated memory and prepend "^".
///
/// @param context EXPAND_FILES etc.
char *addstar(char *fname, size_t len, int context)
  FUNC_ATTR_NONNULL_RET
{
  char *retval;
  size_t i, j;
  size_t new_len;
  char *tail;
  int ends_in_star;

  if (context != EXPAND_FILES
      && context != EXPAND_FILES_IN_PATH
      && context != EXPAND_SHELLCMD
      && context != EXPAND_DIRECTORIES) {
    // Matching will be done internally (on something other than files).
    // So we convert the file-matching-type wildcards into our kind for
    // use with vim_regcomp().  First work out how long it will be:

    // For help tags the translation is done in find_help_tags().
    // For a tag pattern starting with "/" no translation is needed.
    if (context == EXPAND_HELP
        || context == EXPAND_CHECKHEALTH
        || context == EXPAND_COLORS
        || context == EXPAND_COMPILER
        || context == EXPAND_OWNSYNTAX
        || context == EXPAND_FILETYPE
        || context == EXPAND_PACKADD
        || ((context == EXPAND_TAGS_LISTFILES || context == EXPAND_TAGS)
            && fname[0] == '/')) {
      retval = xstrnsave(fname, len);
    } else {
      new_len = len + 2;                // +2 for '^' at start, NUL at end
      for (i = 0; i < len; i++) {
        if (fname[i] == '*' || fname[i] == '~') {
          new_len++;                    // '*' needs to be replaced by ".*"
                                        // '~' needs to be replaced by "\~"
        }
        // Buffer names are like file names.  "." should be literal
        if (context == EXPAND_BUFFERS && fname[i] == '.') {
          new_len++;                    // "." becomes "\."
        }
        // Custom expansion takes care of special things, match
        // backslashes literally (perhaps also for other types?)
        if ((context == EXPAND_USER_DEFINED
             || context == EXPAND_USER_LIST) && fname[i] == '\\') {
          new_len++;                    // '\' becomes "\\"
        }
      }
      retval = xmalloc(new_len);
      {
        retval[0] = '^';
        j = 1;
        for (i = 0; i < len; i++, j++) {
          // Skip backslash.  But why?  At least keep it for custom
          // expansion.
          if (context != EXPAND_USER_DEFINED
              && context != EXPAND_USER_LIST
              && fname[i] == '\\'
              && ++i == len) {
            break;
          }

          switch (fname[i]) {
          case '*':
            retval[j++] = '.';
            break;
          case '~':
            retval[j++] = '\\';
            break;
          case '?':
            retval[j] = '.';
            continue;
          case '.':
            if (context == EXPAND_BUFFERS) {
              retval[j++] = '\\';
            }
            break;
          case '\\':
            if (context == EXPAND_USER_DEFINED
                || context == EXPAND_USER_LIST) {
              retval[j++] = '\\';
            }
            break;
          }
          retval[j] = fname[i];
        }
        retval[j] = NUL;
      }
    }
  } else {
    retval = xmalloc(len + 4);
    STRLCPY(retval, fname, len + 1);

    // Don't add a star to *, ~, ~user, $var or `cmd`.
    // * would become **, which walks the whole tree.
    // ~ would be at the start of the file name, but not the tail.
    // $ could be anywhere in the tail.
    // ` could be anywhere in the file name.
    // When the name ends in '$' don't add a star, remove the '$'.
    tail = path_tail(retval);
    ends_in_star = (len > 0 && retval[len - 1] == '*');
#ifndef BACKSLASH_IN_FILENAME
    for (ssize_t k = (ssize_t)len - 2; k >= 0; k--) {
      if (retval[k] != '\\') {
        break;
      }
      ends_in_star = !ends_in_star;
    }
#endif
    if ((*retval != '~' || tail != retval)
        && !ends_in_star
        && vim_strchr(tail, '$') == NULL
        && vim_strchr(retval, '`') == NULL) {
      retval[len++] = '*';
    } else if (len > 0 && retval[len - 1] == '$') {
      len--;
    }
    retval[len] = NUL;
  }
  return retval;
}

/// Must parse the command line so far to work out what context we are in.
/// Completion can then be done based on that context.
/// This routine sets the variables:
///  xp->xp_pattern          The start of the pattern to be expanded within
///                              the command line (ends at the cursor).
///  xp->xp_context          The type of thing to expand.  Will be one of:
///
///  EXPAND_UNSUCCESSFUL     Used sometimes when there is something illegal on
///                          the command line, like an unknown command.  Caller
///                          should beep.
///  EXPAND_NOTHING          Unrecognised context for completion, use char like
///                          a normal char, rather than for completion.  eg
///                          :s/^I/
///  EXPAND_COMMANDS         Cursor is still touching the command, so complete
///                          it.
///  EXPAND_BUFFERS          Complete file names for :buf and :sbuf commands.
///  EXPAND_FILES            After command with EX_XFILE set, or after setting
///                          with P_EXPAND set.  eg :e ^I, :w>>^I
///  EXPAND_DIRECTORIES      In some cases this is used instead of the latter
///                          when we know only directories are of interest.  eg
///                          :set dir=^I
///  EXPAND_SHELLCMD         After ":!cmd", ":r !cmd"  or ":w !cmd".
///  EXPAND_SETTINGS         Complete variable names.  eg :set d^I
///  EXPAND_BOOL_SETTINGS    Complete boolean variables only,  eg :set no^I
///  EXPAND_TAGS             Complete tags from the files in p_tags.  eg :ta a^I
///  EXPAND_TAGS_LISTFILES   As above, but list filenames on ^D, after :tselect
///  EXPAND_HELP             Complete tags from the file 'helpfile'/tags
///  EXPAND_EVENTS           Complete event names
///  EXPAND_SYNTAX           Complete :syntax command arguments
///  EXPAND_HIGHLIGHT        Complete highlight (syntax) group names
///  EXPAND_AUGROUP          Complete autocommand group names
///  EXPAND_USER_VARS        Complete user defined variable names, eg :unlet a^I
///  EXPAND_MAPPINGS         Complete mapping and abbreviation names,
///                            eg :unmap a^I , :cunab x^I
///  EXPAND_FUNCTIONS        Complete internal or user defined function names,
///                            eg :call sub^I
///  EXPAND_USER_FUNC        Complete user defined function names, eg :delf F^I
///  EXPAND_EXPRESSION       Complete internal or user defined function/variable
///                          names in expressions, eg :while s^I
///  EXPAND_ENV_VARS         Complete environment variable names
///  EXPAND_USER             Complete user names
void set_expand_context(expand_T *xp)
{
  CmdlineInfo *const ccline = get_cmdline_info();

  // only expansion for ':', '>' and '=' command-lines
  if (ccline->cmdfirstc != ':'
      && ccline->cmdfirstc != '>' && ccline->cmdfirstc != '='
      && !ccline->input_fn) {
    xp->xp_context = EXPAND_NOTHING;
    return;
  }
  set_cmd_context(xp, (char_u *)ccline->cmdbuff, ccline->cmdlen, ccline->cmdpos, true);
}

/// Sets the index of a built-in or user defined command "cmd" in eap->cmdidx.
/// For user defined commands, the completion context is set in "xp" and the
/// completion flags in "complp".
///
/// @return  a pointer to the text after the command or NULL for failure.
static const char *set_cmd_index(const char *cmd, exarg_T *eap, expand_T *xp, int *complp)
{
  const char *p = NULL;
  size_t len = 0;

  // Isolate the command and search for it in the command table.
  // Exceptions:
  // - the 'k' command can directly be followed by any character, but
  //   do accept "keepmarks", "keepalt" and "keepjumps".
  // - the 's' command can be followed directly by 'c', 'g', 'i', 'I' or 'r'
  if (*cmd == 'k' && cmd[1] != 'e') {
    eap->cmdidx = CMD_k;
    p = cmd + 1;
  } else {
    p = cmd;
    while (ASCII_ISALPHA(*p) || *p == '*') {  // Allow * wild card
      p++;
    }
    // a user command may contain digits
    if (ASCII_ISUPPER(cmd[0])) {
      while (ASCII_ISALNUM(*p) || *p == '*') {
        p++;
      }
    }
    // for python 3.x: ":py3*" commands completion
    if (cmd[0] == 'p' && cmd[1] == 'y' && p == cmd + 2 && *p == '3') {
      p++;
      while (ASCII_ISALPHA(*p) || *p == '*') {
        p++;
      }
    }
    // check for non-alpha command
    if (p == cmd && vim_strchr("@*!=><&~#", *p) != NULL) {
      p++;
    }
    len = (size_t)(p - cmd);

    if (len == 0) {
      xp->xp_context = EXPAND_UNSUCCESSFUL;
      return NULL;
    }

    eap->cmdidx = excmd_get_cmdidx(cmd, len);

    if (cmd[0] >= 'A' && cmd[0] <= 'Z') {
      while (ASCII_ISALNUM(*p) || *p == '*') {  // Allow * wild card
        p++;
      }
    }
  }

  // If the cursor is touching the command, and it ends in an alphanumeric
  // character, complete the command name.
  if (*p == NUL && ASCII_ISALNUM(p[-1])) {
    return NULL;
  }

  if (eap->cmdidx == CMD_SIZE) {
    if (*cmd == 's' && vim_strchr("cgriI", cmd[1]) != NULL) {
      eap->cmdidx = CMD_substitute;
      p = cmd + 1;
    } else if (cmd[0] >= 'A' && cmd[0] <= 'Z') {
      eap->cmd = (char *)cmd;
      p = (const char *)find_ucmd(eap, (char *)p, NULL, xp, complp);
      if (p == NULL) {
        eap->cmdidx = CMD_SIZE;  // Ambiguous user command.
      }
    }
  }
  if (eap->cmdidx == CMD_SIZE) {
    // Not still touching the command and it was an illegal one
    xp->xp_context = EXPAND_UNSUCCESSFUL;
    return NULL;
  }

  return p;
}

/// Set the completion context for a command argument with wild card characters.
static void set_context_for_wildcard_arg(exarg_T *eap, const char *arg, bool usefilter,
                                         expand_T *xp, int *complp)
{
  bool in_quote = false;
  const char *bow = NULL;  // Beginning of word.
  size_t len = 0;

  // Allow spaces within back-quotes to count as part of the argument
  // being expanded.
  xp->xp_pattern = skipwhite(arg);
  const char *p = (const char *)xp->xp_pattern;
  while (*p != NUL) {
    int c = utf_ptr2char(p);
    if (c == '\\' && p[1] != NUL) {
      p++;
    } else if (c == '`') {
      if (!in_quote) {
        xp->xp_pattern = (char *)p;
        bow = p + 1;
      }
      in_quote = !in_quote;
      // An argument can contain just about everything, except
      // characters that end the command and white space.
    } else if (c == '|' || c == '\n' || c == '"' || ascii_iswhite(c)) {
      len = 0;  // avoid getting stuck when space is in 'isfname'
      while (*p != NUL) {
        c = utf_ptr2char(p);
        if (c == '`' || vim_isfilec_or_wc(c)) {
          break;
        }
        len = (size_t)utfc_ptr2len(p);
        MB_PTR_ADV(p);
      }
      if (in_quote) {
        bow = p;
      } else {
        xp->xp_pattern = (char *)p;
      }
      p -= len;
    }
    MB_PTR_ADV(p);
  }

  // If we are still inside the quotes, and we passed a space, just
  // expand from there.
  if (bow != NULL && in_quote) {
    xp->xp_pattern = (char *)bow;
  }
  xp->xp_context = EXPAND_FILES;

  // For a shell command more chars need to be escaped.
  if (usefilter || eap->cmdidx == CMD_bang || eap->cmdidx == CMD_terminal) {
#ifndef BACKSLASH_IN_FILENAME
    xp->xp_shell = true;
#endif
    // When still after the command name expand executables.
    if (xp->xp_pattern == skipwhite(arg)) {
      xp->xp_context = EXPAND_SHELLCMD;
    }
  }

  // Check for environment variable.
  if (*xp->xp_pattern == '$') {
    for (p = (const char *)xp->xp_pattern + 1; *p != NUL; p++) {
      if (!vim_isIDc((uint8_t)(*p))) {
        break;
      }
    }
    if (*p == NUL) {
      xp->xp_context = EXPAND_ENV_VARS;
      xp->xp_pattern++;
      // Avoid that the assignment uses EXPAND_FILES again.
      if (*complp != EXPAND_USER_DEFINED && *complp != EXPAND_USER_LIST) {
        *complp = EXPAND_ENV_VARS;
      }
    }
  }
  // Check for user names.
  if (*xp->xp_pattern == '~') {
    for (p = (const char *)xp->xp_pattern + 1; *p != NUL && *p != '/'; p++) {}
    // Complete ~user only if it partially matches a user name.
    // A full match ~user<Tab> will be replaced by user's home
    // directory i.e. something like ~user<Tab> -> /home/user/
    if (*p == NUL && p > (const char *)xp->xp_pattern + 1
        && match_user(xp->xp_pattern + 1) >= 1) {
      xp->xp_context = EXPAND_USER;
      xp->xp_pattern++;
    }
  }
}

/// Set the completion context in "xp" for command "cmd" with index "cmdidx".
/// The argument to the command is "arg" and the argument flags is "argt".
/// For user-defined commands and for environment variables, "context" has the
/// completion type.
///
/// @return  a pointer to the next command, or NULL if there is no next command.
static const char *set_context_by_cmdname(const char *cmd, cmdidx_T cmdidx, const char *arg,
                                          uint32_t argt, int context, expand_T *xp, bool forceit)
{
  const char *p;

  switch (cmdidx) {
  case CMD_find:
  case CMD_sfind:
  case CMD_tabfind:
    if (xp->xp_context == EXPAND_FILES) {
      xp->xp_context = EXPAND_FILES_IN_PATH;
    }
    break;
  case CMD_cd:
  case CMD_chdir:
  case CMD_lcd:
  case CMD_lchdir:
  case CMD_tcd:
  case CMD_tchdir:
    if (xp->xp_context == EXPAND_FILES) {
      xp->xp_context = EXPAND_DIRECTORIES;
    }
    break;
  case CMD_help:
    xp->xp_context = EXPAND_HELP;
    xp->xp_pattern = (char *)arg;
    break;

  // Command modifiers: return the argument.
  // Also for commands with an argument that is a command.
  case CMD_aboveleft:
  case CMD_argdo:
  case CMD_belowright:
  case CMD_botright:
  case CMD_browse:
  case CMD_bufdo:
  case CMD_cdo:
  case CMD_cfdo:
  case CMD_confirm:
  case CMD_debug:
  case CMD_folddoclosed:
  case CMD_folddoopen:
  case CMD_hide:
  case CMD_horizontal:
  case CMD_keepalt:
  case CMD_keepjumps:
  case CMD_keepmarks:
  case CMD_keeppatterns:
  case CMD_ldo:
  case CMD_leftabove:
  case CMD_lfdo:
  case CMD_lockmarks:
  case CMD_noautocmd:
  case CMD_noswapfile:
  case CMD_rightbelow:
  case CMD_sandbox:
  case CMD_silent:
  case CMD_tab:
  case CMD_tabdo:
  case CMD_topleft:
  case CMD_verbose:
  case CMD_vertical:
  case CMD_windo:
    return arg;

  case CMD_filter:
    if (*arg != NUL) {
      arg = (const char *)skip_vimgrep_pat((char *)arg, NULL, NULL);
    }
    if (arg == NULL || *arg == NUL) {
      xp->xp_context = EXPAND_NOTHING;
      return NULL;
    }
    return (const char *)skipwhite(arg);

  case CMD_match:
    if (*arg == NUL || !ends_excmd(*arg)) {
      // also complete "None"
      set_context_in_echohl_cmd(xp, arg);
      arg = (const char *)skipwhite(skiptowhite(arg));
      if (*arg != NUL) {
        xp->xp_context = EXPAND_NOTHING;
        arg = (const char *)skip_regexp((char *)arg + 1, (uint8_t)(*arg), p_magic);
      }
    }
    return (const char *)find_nextcmd(arg);

  // All completion for the +cmdline_compl feature goes here.

  case CMD_command:
    return set_context_in_user_cmd(xp, arg);

  case CMD_delcommand:
    xp->xp_context = EXPAND_USER_COMMANDS;
    xp->xp_pattern = (char *)arg;
    break;

  case CMD_global:
  case CMD_vglobal: {
    const int delim = (uint8_t)(*arg);  // Get the delimiter.
    if (delim) {
      arg++;  // Skip delimiter if there is one.
    }

    while (arg[0] != NUL && (uint8_t)arg[0] != delim) {
      if (arg[0] == '\\' && arg[1] != NUL) {
        arg++;
      }
      arg++;
    }
    if (arg[0] != NUL) {
      return arg + 1;
    }
    break;
  }
  case CMD_and:
  case CMD_substitute: {
    const int delim = (uint8_t)(*arg);
    if (delim) {
      // Skip "from" part.
      arg++;
      arg = (const char *)skip_regexp((char *)arg, delim, p_magic);
    }
    // Skip "to" part.
    while (arg[0] != NUL && (uint8_t)arg[0] != delim) {
      if (arg[0] == '\\' && arg[1] != NUL) {
        arg++;
      }
      arg++;
    }
    if (arg[0] != NUL) {  // Skip delimiter.
      arg++;
    }
    while (arg[0] && strchr("|\"#", arg[0]) == NULL) {
      arg++;
    }
    if (arg[0] != NUL) {
      return arg;
    }
    break;
  }
  case CMD_isearch:
  case CMD_dsearch:
  case CMD_ilist:
  case CMD_dlist:
  case CMD_ijump:
  case CMD_psearch:
  case CMD_djump:
  case CMD_isplit:
  case CMD_dsplit:
    // Skip count.
    arg = (const char *)skipwhite(skipdigits(arg));
    if (*arg == '/') {  // Match regexp, not just whole words.
      for (++arg; *arg && *arg != '/'; arg++) {
        if (*arg == '\\' && arg[1] != NUL) {
          arg++;
        }
      }
      if (*arg) {
        arg = (const char *)skipwhite(arg + 1);

        // Check for trailing illegal characters.
        if (*arg && strchr("|\"\n", *arg) == NULL) {
          xp->xp_context = EXPAND_NOTHING;
        } else {
          return arg;
        }
      }
    }
    break;
  case CMD_autocmd:
    return (const char *)set_context_in_autocmd(xp, (char *)arg, false);

  case CMD_doautocmd:
  case CMD_doautoall:
    return (const char *)set_context_in_autocmd(xp, (char *)arg, true);
  case CMD_set:
    set_context_in_set_cmd(xp, (char_u *)arg, 0);
    break;
  case CMD_setglobal:
    set_context_in_set_cmd(xp, (char_u *)arg, OPT_GLOBAL);
    break;
  case CMD_setlocal:
    set_context_in_set_cmd(xp, (char_u *)arg, OPT_LOCAL);
    break;
  case CMD_tag:
  case CMD_stag:
  case CMD_ptag:
  case CMD_ltag:
  case CMD_tselect:
  case CMD_stselect:
  case CMD_ptselect:
  case CMD_tjump:
  case CMD_stjump:
  case CMD_ptjump:
    if (wop_flags & WOP_TAGFILE) {
      xp->xp_context = EXPAND_TAGS_LISTFILES;
    } else {
      xp->xp_context = EXPAND_TAGS;
    }
    xp->xp_pattern = (char *)arg;
    break;
  case CMD_augroup:
    xp->xp_context = EXPAND_AUGROUP;
    xp->xp_pattern = (char *)arg;
    break;
  case CMD_syntax:
    set_context_in_syntax_cmd(xp, arg);
    break;
  case CMD_const:
  case CMD_let:
  case CMD_if:
  case CMD_elseif:
  case CMD_while:
  case CMD_for:
  case CMD_echo:
  case CMD_echon:
  case CMD_execute:
  case CMD_echomsg:
  case CMD_echoerr:
  case CMD_call:
  case CMD_return:
  case CMD_cexpr:
  case CMD_caddexpr:
  case CMD_cgetexpr:
  case CMD_lexpr:
  case CMD_laddexpr:
  case CMD_lgetexpr:
    set_context_for_expression(xp, (char *)arg, cmdidx);
    break;

  case CMD_unlet:
    while ((xp->xp_pattern = strchr(arg, ' ')) != NULL) {
      arg = (const char *)xp->xp_pattern + 1;
    }

    xp->xp_context = EXPAND_USER_VARS;
    xp->xp_pattern = (char *)arg;

    if (*xp->xp_pattern == '$') {
      xp->xp_context = EXPAND_ENV_VARS;
      xp->xp_pattern++;
    }

    break;

  case CMD_function:
  case CMD_delfunction:
    xp->xp_context = EXPAND_USER_FUNC;
    xp->xp_pattern = (char *)arg;
    break;

  case CMD_echohl:
    set_context_in_echohl_cmd(xp, arg);
    break;
  case CMD_highlight:
    set_context_in_highlight_cmd(xp, arg);
    break;
  case CMD_sign:
    set_context_in_sign_cmd(xp, (char *)arg);
    break;
  case CMD_bdelete:
  case CMD_bwipeout:
  case CMD_bunload:
    while ((xp->xp_pattern = strchr(arg, ' ')) != NULL) {
      arg = (const char *)xp->xp_pattern + 1;
    }
    FALLTHROUGH;
  case CMD_buffer:
  case CMD_sbuffer:
  case CMD_checktime:
    xp->xp_context = EXPAND_BUFFERS;
    xp->xp_pattern = (char *)arg;
    break;
  case CMD_diffget:
  case CMD_diffput:
    // If current buffer is in diff mode, complete buffer names
    // which are in diff mode, and different than current buffer.
    xp->xp_context = EXPAND_DIFF_BUFFERS;
    xp->xp_pattern = (char *)arg;
    break;

  case CMD_USER:
  case CMD_USER_BUF:
    if (context != EXPAND_NOTHING) {
      // EX_XFILE: file names are handled above.
      if (!(argt & EX_XFILE)) {
        if (context == EXPAND_MENUS) {
          return (const char *)set_context_in_menu_cmd(xp, cmd, (char *)arg, forceit);
        } else if (context == EXPAND_COMMANDS) {
          return arg;
        } else if (context == EXPAND_MAPPINGS) {
          return (const char *)set_context_in_map_cmd(xp, "map", (char_u *)arg, forceit,
                                                      false, false,
                                                      CMD_map);
        }
        // Find start of last argument.
        p = arg;
        while (*p) {
          if (*p == ' ') {
            // argument starts after a space
            arg = p + 1;
          } else if (*p == '\\' && *(p + 1) != NUL) {
            p++;  // skip over escaped character
          }
          MB_PTR_ADV(p);
        }
        xp->xp_pattern = (char *)arg;
      }
      xp->xp_context = context;
    }
    break;

  case CMD_map:
  case CMD_noremap:
  case CMD_nmap:
  case CMD_nnoremap:
  case CMD_vmap:
  case CMD_vnoremap:
  case CMD_omap:
  case CMD_onoremap:
  case CMD_imap:
  case CMD_inoremap:
  case CMD_cmap:
  case CMD_cnoremap:
  case CMD_lmap:
  case CMD_lnoremap:
  case CMD_smap:
  case CMD_snoremap:
  case CMD_xmap:
  case CMD_xnoremap:
    return (const char *)set_context_in_map_cmd(xp, (char *)cmd, (char_u *)arg, forceit, false,
                                                false, cmdidx);
  case CMD_unmap:
  case CMD_nunmap:
  case CMD_vunmap:
  case CMD_ounmap:
  case CMD_iunmap:
  case CMD_cunmap:
  case CMD_lunmap:
  case CMD_sunmap:
  case CMD_xunmap:
    return (const char *)set_context_in_map_cmd(xp, (char *)cmd, (char_u *)arg, forceit, false,
                                                true, cmdidx);
  case CMD_mapclear:
  case CMD_nmapclear:
  case CMD_vmapclear:
  case CMD_omapclear:
  case CMD_imapclear:
  case CMD_cmapclear:
  case CMD_lmapclear:
  case CMD_smapclear:
  case CMD_xmapclear:
    xp->xp_context = EXPAND_MAPCLEAR;
    xp->xp_pattern = (char *)arg;
    break;

  case CMD_abbreviate:
  case CMD_noreabbrev:
  case CMD_cabbrev:
  case CMD_cnoreabbrev:
  case CMD_iabbrev:
  case CMD_inoreabbrev:
    return (const char *)set_context_in_map_cmd(xp, (char *)cmd, (char_u *)arg, forceit, true,
                                                false, cmdidx);
  case CMD_unabbreviate:
  case CMD_cunabbrev:
  case CMD_iunabbrev:
    return (const char *)set_context_in_map_cmd(xp, (char *)cmd, (char_u *)arg, forceit, true,
                                                true, cmdidx);
  case CMD_menu:
  case CMD_noremenu:
  case CMD_unmenu:
  case CMD_amenu:
  case CMD_anoremenu:
  case CMD_aunmenu:
  case CMD_nmenu:
  case CMD_nnoremenu:
  case CMD_nunmenu:
  case CMD_vmenu:
  case CMD_vnoremenu:
  case CMD_vunmenu:
  case CMD_omenu:
  case CMD_onoremenu:
  case CMD_ounmenu:
  case CMD_imenu:
  case CMD_inoremenu:
  case CMD_iunmenu:
  case CMD_cmenu:
  case CMD_cnoremenu:
  case CMD_cunmenu:
  case CMD_tlmenu:
  case CMD_tlnoremenu:
  case CMD_tlunmenu:
  case CMD_tmenu:
  case CMD_tunmenu:
  case CMD_popup:
  case CMD_emenu:
    return (const char *)set_context_in_menu_cmd(xp, cmd, (char *)arg, forceit);

  case CMD_colorscheme:
    xp->xp_context = EXPAND_COLORS;
    xp->xp_pattern = (char *)arg;
    break;

  case CMD_compiler:
    xp->xp_context = EXPAND_COMPILER;
    xp->xp_pattern = (char *)arg;
    break;

  case CMD_ownsyntax:
    xp->xp_context = EXPAND_OWNSYNTAX;
    xp->xp_pattern = (char *)arg;
    break;

  case CMD_setfiletype:
    xp->xp_context = EXPAND_FILETYPE;
    xp->xp_pattern = (char *)arg;
    break;

  case CMD_packadd:
    xp->xp_context = EXPAND_PACKADD;
    xp->xp_pattern = (char *)arg;
    break;

#ifdef HAVE_WORKING_LIBINTL
  case CMD_language:
    p = (const char *)skiptowhite(arg);
    if (*p == NUL) {
      xp->xp_context = EXPAND_LANGUAGE;
      xp->xp_pattern = (char *)arg;
    } else {
      if (strncmp(arg, "messages", (size_t)(p - arg)) == 0
          || strncmp(arg, "ctype", (size_t)(p - arg)) == 0
          || strncmp(arg, "time", (size_t)(p - arg)) == 0
          || strncmp(arg, "collate", (size_t)(p - arg)) == 0) {
        xp->xp_context = EXPAND_LOCALES;
        xp->xp_pattern = skipwhite(p);
      } else {
        xp->xp_context = EXPAND_NOTHING;
      }
    }
    break;
#endif
  case CMD_profile:
    set_context_in_profile_cmd(xp, arg);
    break;
  case CMD_checkhealth:
    xp->xp_context = EXPAND_CHECKHEALTH;
    xp->xp_pattern = (char *)arg;
    break;
  case CMD_behave:
    xp->xp_context = EXPAND_BEHAVE;
    xp->xp_pattern = (char *)arg;
    break;

  case CMD_messages:
    xp->xp_context = EXPAND_MESSAGES;
    xp->xp_pattern = (char *)arg;
    break;

  case CMD_history:
    xp->xp_context = EXPAND_HISTORY;
    xp->xp_pattern = (char *)arg;
    break;
  case CMD_syntime:
    xp->xp_context = EXPAND_SYNTIME;
    xp->xp_pattern = (char *)arg;
    break;

  case CMD_argdelete:
    while ((xp->xp_pattern = vim_strchr(arg, ' ')) != NULL) {
      arg = (const char *)(xp->xp_pattern + 1);
    }
    xp->xp_context = EXPAND_ARGLIST;
    xp->xp_pattern = (char *)arg;
    break;

  case CMD_lua:
    xp->xp_context = EXPAND_LUA;
    break;

  default:
    break;
  }
  return NULL;
}

/// This is all pretty much copied from do_one_cmd(), with all the extra stuff
/// we don't need/want deleted.  Maybe this could be done better if we didn't
/// repeat all this stuff.  The only problem is that they may not stay
/// perfectly compatible with each other, but then the command line syntax
/// probably won't change that much -- webb.
///
/// @param buff  buffer for command string
static const char *set_one_cmd_context(expand_T *xp, const char *buff)
{
  size_t len = 0;
  exarg_T ea;
  int context = EXPAND_NOTHING;
  bool forceit = false;
  bool usefilter = false;  // Filter instead of file name.

  ExpandInit(xp);
  xp->xp_pattern = (char *)buff;
  xp->xp_line = (char *)buff;
  xp->xp_context = EXPAND_COMMANDS;  // Default until we get past command
  ea.argt = 0;

  // 1. skip comment lines and leading space, colons or bars
  const char *cmd;
  for (cmd = buff; vim_strchr(" \t:|", *cmd) != NULL; cmd++) {}
  xp->xp_pattern = (char *)cmd;

  if (*cmd == NUL) {
    return NULL;
  }
  if (*cmd == '"') {  // ignore comment lines
    xp->xp_context = EXPAND_NOTHING;
    return NULL;
  }

  // 3. skip over a range specifier of the form: addr [,addr] [;addr] ..
  cmd = (const char *)skip_range(cmd, &xp->xp_context);
  xp->xp_pattern = (char *)cmd;
  if (*cmd == NUL) {
    return NULL;
  }
  if (*cmd == '"') {
    xp->xp_context = EXPAND_NOTHING;
    return NULL;
  }

  if (*cmd == '|' || *cmd == '\n') {
    return cmd + 1;  // There's another command
  }

  // Get the command index.
  const char *p = set_cmd_index(cmd, &ea, xp, &context);
  if (p == NULL) {
    return NULL;
  }

  xp->xp_context = EXPAND_NOTHING;  // Default now that we're past command

  if (*p == '!') {  // forced commands
    forceit = true;
    p++;
  }

  // 6. parse arguments
  if (!IS_USER_CMDIDX(ea.cmdidx)) {
    ea.argt = excmd_get_argt(ea.cmdidx);
  }

  const char *arg = (const char *)skipwhite(p);

  // Skip over ++argopt argument
  if ((ea.argt & EX_ARGOPT) && *arg != NUL && strncmp(arg, "++", 2) == 0) {
    p = arg;
    while (*p && !ascii_isspace(*p)) {
      MB_PTR_ADV(p);
    }
    arg = (const char *)skipwhite(p);
  }

  if (ea.cmdidx == CMD_write || ea.cmdidx == CMD_update) {
    if (*arg == '>') {  // append
      if (*++arg == '>') {
        arg++;
      }
      arg = (const char *)skipwhite(arg);
    } else if (*arg == '!' && ea.cmdidx == CMD_write) {  // :w !filter
      arg++;
      usefilter = true;
    }
  }

  if (ea.cmdidx == CMD_read) {
    usefilter = forceit;  // :r! filter if forced
    if (*arg == '!') {    // :r !filter
      arg++;
      usefilter = true;
    }
  }

  if (ea.cmdidx == CMD_lshift || ea.cmdidx == CMD_rshift) {
    while (*arg == *cmd) {  // allow any number of '>' or '<'
      arg++;
    }
    arg = (const char *)skipwhite(arg);
  }

  // Does command allow "+command"?
  if ((ea.argt & EX_CMDARG) && !usefilter && *arg == '+') {
    // Check if we're in the +command
    p = arg + 1;
    arg = (const char *)skip_cmd_arg((char *)arg, false);

    // Still touching the command after '+'?
    if (*arg == NUL) {
      return p;
    }

    // Skip space(s) after +command to get to the real argument.
    arg = (const char *)skipwhite(arg);
  }

  // Check for '|' to separate commands and '"' to start comments.
  // Don't do this for ":read !cmd" and ":write !cmd".
  if ((ea.argt & EX_TRLBAR) && !usefilter) {
    p = arg;
    // ":redir @" is not the start of a comment
    if (ea.cmdidx == CMD_redir && p[0] == '@' && p[1] == '"') {
      p += 2;
    }
    while (*p) {
      if (*p == Ctrl_V) {
        if (p[1] != NUL) {
          p++;
        }
      } else if ((*p == '"' && !(ea.argt & EX_NOTRLCOM))
                 || *p == '|'
                 || *p == '\n') {
        if (*(p - 1) != '\\') {
          if (*p == '|' || *p == '\n') {
            return p + 1;
          }
          return NULL;  // It's a comment
        }
      }
      MB_PTR_ADV(p);
    }
  }

  if (!(ea.argt & EX_EXTRA) && *arg != NUL && strchr("|\"", *arg) == NULL) {
    // no arguments allowed but there is something
    return NULL;
  }

  // Find start of last argument (argument just before cursor):
  p = buff;
  xp->xp_pattern = (char *)p;
  len = strlen(buff);
  while (*p && p < buff + len) {
    if (*p == ' ' || *p == TAB) {
      // argument starts after a space
      xp->xp_pattern = (char *)++p;
    } else {
      if (*p == '\\' && *(p + 1) != NUL) {
        p++;  // skip over escaped character
      }
      MB_PTR_ADV(p);
    }
  }

  if (ea.argt & EX_XFILE) {
    set_context_for_wildcard_arg(&ea, arg, usefilter, xp, &context);
  }

  // Switch on command name.
  return set_context_by_cmdname(cmd, ea.cmdidx, arg, ea.argt, context, xp, forceit);
}

/// @param str  start of command line
/// @param len  length of command line (excl. NUL)
/// @param col  position of cursor
/// @param use_ccline  use ccline for info
void set_cmd_context(expand_T *xp, char_u *str, int len, int col, int use_ccline)
{
  CmdlineInfo *const ccline = get_cmdline_info();
  char_u old_char = NUL;

  // Avoid a UMR warning from Purify, only save the character if it has been
  // written before.
  if (col < len) {
    old_char = str[col];
  }
  str[col] = NUL;
  const char *nextcomm = (const char *)str;

  if (use_ccline && ccline->cmdfirstc == '=') {
    // pass CMD_SIZE because there is no real command
    set_context_for_expression(xp, (char *)str, CMD_SIZE);
  } else if (use_ccline && ccline->input_fn) {
    xp->xp_context = ccline->xp_context;
    xp->xp_pattern = ccline->cmdbuff;
    xp->xp_arg = (char *)ccline->xp_arg;
  } else {
    while (nextcomm != NULL) {
      nextcomm = set_one_cmd_context(xp, nextcomm);
    }
  }

  // Store the string here so that call_user_expand_func() can get to them
  // easily.
  xp->xp_line = (char *)str;
  xp->xp_col = col;

  str[col] = old_char;
}

/// Expand the command line "str" from context "xp".
/// "xp" must have been set by set_cmd_context().
/// xp->xp_pattern points into "str", to where the text that is to be expanded
/// starts.
/// Returns EXPAND_UNSUCCESSFUL when there is something illegal before the
/// cursor.
/// Returns EXPAND_NOTHING when there is nothing to expand, might insert the
/// key that triggered expansion literally.
/// Returns EXPAND_OK otherwise.
///
/// @param str  start of command line
/// @param col  position of cursor
/// @param matchcount  return: nr of matches
/// @param matches  return: array of pointers to matches
int expand_cmdline(expand_T *xp, const char_u *str, int col, int *matchcount, char ***matches)
{
  char_u *file_str = NULL;
  int options = WILD_ADD_SLASH|WILD_SILENT;

  if (xp->xp_context == EXPAND_UNSUCCESSFUL) {
    beep_flush();
    return EXPAND_UNSUCCESSFUL;      // Something illegal on command line
  }
  if (xp->xp_context == EXPAND_NOTHING) {
    // Caller can use the character as a normal char instead
    return EXPAND_NOTHING;
  }

  // add star to file name, or convert to regexp if not exp. files.
  assert((str + col) - (char_u *)xp->xp_pattern >= 0);
  xp->xp_pattern_len = (size_t)((str + col) - (char_u *)xp->xp_pattern);
  file_str = (char_u *)addstar(xp->xp_pattern, xp->xp_pattern_len, xp->xp_context);

  if (p_wic) {
    options += WILD_ICASE;
  }

  // find all files that match the description
  if (ExpandFromContext(xp, file_str, matchcount, matches, options) == FAIL) {
    *matchcount = 0;
    *matches = NULL;
  }
  xfree(file_str);

  return EXPAND_OK;
}

/// Expand file or directory names.
static int expand_files_and_dirs(expand_T *xp, char *pat, char ***file, int *num_file, int flags,
                                 int options)
{
  bool free_pat = false;

  // for ":set path=" and ":set tags=" halve backslashes for escaped space
  if (xp->xp_backslash != XP_BS_NONE) {
    free_pat = true;
    pat = xstrdup(pat);
    for (int i = 0; pat[i]; i++) {
      if (pat[i] == '\\') {
        if (xp->xp_backslash == XP_BS_THREE
            && pat[i + 1] == '\\'
            && pat[i + 2] == '\\'
            && pat[i + 3] == ' ') {
          STRMOVE(pat + i, pat + i + 3);
        }
        if (xp->xp_backslash == XP_BS_ONE
            && pat[i + 1] == ' ') {
          STRMOVE(pat + i, pat + i + 1);
        }
      }
    }
  }

  if (xp->xp_context == EXPAND_FILES) {
    flags |= EW_FILE;
  } else if (xp->xp_context == EXPAND_FILES_IN_PATH) {
    flags |= (EW_FILE | EW_PATH);
  } else {
    flags = (flags | EW_DIR) & ~EW_FILE;
  }
  if (options & WILD_ICASE) {
    flags |= EW_ICASE;
  }

  // Expand wildcards, supporting %:h and the like.
  int ret = expand_wildcards_eval(&pat, num_file, file, flags);
  if (free_pat) {
    xfree(pat);
  }
#ifdef BACKSLASH_IN_FILENAME
  if (p_csl[0] != NUL && (options & WILD_IGNORE_COMPLETESLASH) == 0) {
    for (int j = 0; j < *num_file; j++) {
      char *ptr = (*file)[j];
      while (*ptr != NUL) {
        if (p_csl[0] == 's' && *ptr == '\\') {
          *ptr = '/';
        } else if (p_csl[0] == 'b' && *ptr == '/') {
          *ptr = '\\';
        }
        ptr += utfc_ptr2len(ptr);
      }
    }
  }
#endif
  return ret;
}

/// Function given to ExpandGeneric() to obtain the possible arguments of the
/// ":behave {mswin,xterm}" command.
static char *get_behave_arg(expand_T *xp FUNC_ATTR_UNUSED, int idx)
{
  if (idx == 0) {
    return "mswin";
  }
  if (idx == 1) {
    return "xterm";
  }
  return NULL;
}

/// Function given to ExpandGeneric() to obtain the possible arguments of the
/// ":messages {clear}" command.
static char *get_messages_arg(expand_T *xp FUNC_ATTR_UNUSED, int idx)
{
  if (idx == 0) {
    return "clear";
  }
  return NULL;
}

static char *get_mapclear_arg(expand_T *xp FUNC_ATTR_UNUSED, int idx)
{
  if (idx == 0) {
    return "<buffer>";
  }
  return NULL;
}

/// Completion for |:checkhealth| command.
///
/// Given to ExpandGeneric() to obtain all available heathcheck names.
/// @param[in] idx  Index of the healthcheck item.
/// @param[in] xp  Not used.
static char *get_healthcheck_names(expand_T *xp FUNC_ATTR_UNUSED, int idx)
{
  static Object names = OBJECT_INIT;
  static unsigned last_gen = 0;

  if (last_gen != get_cmdline_last_prompt_id() || last_gen == 0) {
    Array a = ARRAY_DICT_INIT;
    Error err = ERROR_INIT;
    Object res = nlua_exec(STATIC_CSTR_AS_STRING("return vim.health._complete()"), a, &err);
    api_clear_error(&err);
    api_free_object(names);
    names = res;
    last_gen = get_cmdline_last_prompt_id();
  }

  if (names.type == kObjectTypeArray && idx < (int)names.data.array.size) {
    return names.data.array.items[idx].data.string.data;
  }
  return NULL;
}

/// Do the expansion based on xp->xp_context and "rmp".
static int ExpandOther(expand_T *xp, regmatch_T *rmp, int *num_file, char ***file)
{
  typedef CompleteListItemGetter ExpandFunc;
  static struct expgen {
    int context;
    ExpandFunc func;
    int ic;
    int escaped;
  } tab[] = {
    { EXPAND_COMMANDS, get_command_name, false, true },
    { EXPAND_BEHAVE, get_behave_arg, true, true },
    { EXPAND_MAPCLEAR, get_mapclear_arg, true, true },
    { EXPAND_MESSAGES, get_messages_arg, true, true },
    { EXPAND_HISTORY, get_history_arg, true, true },
    { EXPAND_USER_COMMANDS, get_user_commands, false, true },
    { EXPAND_USER_ADDR_TYPE, get_user_cmd_addr_type, false, true },
    { EXPAND_USER_CMD_FLAGS, get_user_cmd_flags, false, true },
    { EXPAND_USER_NARGS, get_user_cmd_nargs, false, true },
    { EXPAND_USER_COMPLETE, get_user_cmd_complete, false, true },
    { EXPAND_USER_VARS, get_user_var_name, false, true },
    { EXPAND_FUNCTIONS, get_function_name, false, true },
    { EXPAND_USER_FUNC, get_user_func_name, false, true },
    { EXPAND_EXPRESSION, get_expr_name, false, true },
    { EXPAND_MENUS, get_menu_name, false, true },
    { EXPAND_MENUNAMES, get_menu_names, false, true },
    { EXPAND_SYNTAX, get_syntax_name, true, true },
    { EXPAND_SYNTIME, get_syntime_arg, true, true },
    { EXPAND_HIGHLIGHT, (ExpandFunc)get_highlight_name, true, false },
    { EXPAND_EVENTS, expand_get_event_name, true, false },
    { EXPAND_AUGROUP, expand_get_augroup_name, true, false },
    { EXPAND_SIGN, get_sign_name, true, true },
    { EXPAND_PROFILE, get_profile_name, true, true },
#ifdef HAVE_WORKING_LIBINTL
    { EXPAND_LANGUAGE, get_lang_arg, true, false },
    { EXPAND_LOCALES, get_locales, true, false },
#endif
    { EXPAND_ENV_VARS, get_env_name, true, true },
    { EXPAND_USER, get_users, true, false },
    { EXPAND_ARGLIST, get_arglist_name, true, false },
    { EXPAND_CHECKHEALTH, get_healthcheck_names, true, false },
  };
  int ret = FAIL;

  // Find a context in the table and call the ExpandGeneric() with the
  // right function to do the expansion.
  for (int i = 0; i < (int)ARRAY_SIZE(tab); i++) {
    if (xp->xp_context == tab[i].context) {
      if (tab[i].ic) {
        rmp->rm_ic = true;
      }
      ExpandGeneric(xp, rmp, num_file, file, tab[i].func, tab[i].escaped);
      ret = OK;
      break;
    }
  }

  return ret;
}

/// Do the expansion based on xp->xp_context and "pat".
///
/// @param options  WILD_ flags
static int ExpandFromContext(expand_T *xp, char_u *pat, int *num_file, char ***file, int options)
{
  regmatch_T regmatch;
  int ret;
  int flags;

  flags = EW_DIR;       // include directories
  if (options & WILD_LIST_NOTFOUND) {
    flags |= EW_NOTFOUND;
  }
  if (options & WILD_ADD_SLASH) {
    flags |= EW_ADDSLASH;
  }
  if (options & WILD_KEEP_ALL) {
    flags |= EW_KEEPALL;
  }
  if (options & WILD_SILENT) {
    flags |= EW_SILENT;
  }
  if (options & WILD_NOERROR) {
    flags |= EW_NOERROR;
  }
  if (options & WILD_ALLLINKS) {
    flags |= EW_ALLLINKS;
  }

  if (xp->xp_context == EXPAND_FILES
      || xp->xp_context == EXPAND_DIRECTORIES
      || xp->xp_context == EXPAND_FILES_IN_PATH) {
    return expand_files_and_dirs(xp, (char *)pat, file, num_file, flags, options);
  }

  *file = NULL;
  *num_file = 0;
  if (xp->xp_context == EXPAND_HELP) {
    // With an empty argument we would get all the help tags, which is
    // very slow.  Get matches for "help" instead.
    if (find_help_tags(*pat == NUL ? "help" : (char *)pat,
                       num_file, file, false) == OK) {
      cleanup_help_tags(*num_file, *file);
      return OK;
    }
    return FAIL;
  }

  if (xp->xp_context == EXPAND_SHELLCMD) {
    *file = NULL;
    expand_shellcmd((char *)pat, num_file, file, flags);
    return OK;
  }
  if (xp->xp_context == EXPAND_OLD_SETTING) {
    ExpandOldSetting(num_file, file);
    return OK;
  }
  if (xp->xp_context == EXPAND_BUFFERS) {
    return ExpandBufnames((char *)pat, num_file, file, options);
  }
  if (xp->xp_context == EXPAND_DIFF_BUFFERS) {
    return ExpandBufnames((char *)pat, num_file, file, options | BUF_DIFF_FILTER);
  }
  if (xp->xp_context == EXPAND_TAGS
      || xp->xp_context == EXPAND_TAGS_LISTFILES) {
    return expand_tags(xp->xp_context == EXPAND_TAGS, pat, num_file, file);
  }
  if (xp->xp_context == EXPAND_COLORS) {
    char *directories[] = { "colors", NULL };
    return ExpandRTDir((char *)pat, DIP_START + DIP_OPT + DIP_LUA, num_file, file, directories);
  }
  if (xp->xp_context == EXPAND_COMPILER) {
    char *directories[] = { "compiler", NULL };
    return ExpandRTDir((char *)pat, DIP_LUA, num_file, file, directories);
  }
  if (xp->xp_context == EXPAND_OWNSYNTAX) {
    char *directories[] = { "syntax", NULL };
    return ExpandRTDir((char *)pat, 0, num_file, file, directories);
  }
  if (xp->xp_context == EXPAND_FILETYPE) {
    char *directories[] = { "syntax", "indent", "ftplugin", NULL };
    return ExpandRTDir((char *)pat, DIP_LUA, num_file, file, directories);
  }
  if (xp->xp_context == EXPAND_USER_LIST) {
    return ExpandUserList(xp, num_file, file);
  }
  if (xp->xp_context == EXPAND_USER_LUA) {
    return ExpandUserLua(xp, num_file, file);
  }
  if (xp->xp_context == EXPAND_PACKADD) {
    return ExpandPackAddDir((char *)pat, num_file, file);
  }

  // When expanding a function name starting with s:, match the <SNR>nr_
  // prefix.
  char *tofree = NULL;
  if (xp->xp_context == EXPAND_USER_FUNC && STRNCMP(pat, "^s:", 3) == 0) {
    const size_t len = STRLEN(pat) + 20;

    tofree = xmalloc(len);
    snprintf(tofree, len, "^<SNR>\\d\\+_%s", pat + 3);
    pat = (char_u *)tofree;
  }

  if (xp->xp_context == EXPAND_LUA) {
    ILOG("PAT %s", pat);
    return nlua_expand_pat(xp, (char *)pat, num_file, file);
  }

  regmatch.regprog = vim_regcomp((char *)pat, p_magic ? RE_MAGIC : 0);
  if (regmatch.regprog == NULL) {
    return FAIL;
  }

  // set ignore-case according to p_ic, p_scs and pat
  regmatch.rm_ic = ignorecase(pat);

  if (xp->xp_context == EXPAND_SETTINGS
      || xp->xp_context == EXPAND_BOOL_SETTINGS) {
    ret = ExpandSettings(xp, &regmatch, num_file, file);
  } else if (xp->xp_context == EXPAND_MAPPINGS) {
    ret = ExpandMappings(&regmatch, num_file, file);
  } else if (xp->xp_context == EXPAND_USER_DEFINED) {
    ret = ExpandUserDefined(xp, &regmatch, num_file, file);
  } else {
    ret = ExpandOther(xp, &regmatch, num_file, file);
  }

  vim_regfree(regmatch.regprog);
  xfree(tofree);

  return ret;
}

/// Expand a list of names.
///
/// Generic function for command line completion.  It calls a function to
/// obtain strings, one by one.  The strings are matched against a regexp
/// program.  Matching strings are copied into an array, which is returned.
///
/// @param func  returns a string from the list
static void ExpandGeneric(expand_T *xp, regmatch_T *regmatch, int *num_file, char ***file,
                          CompleteListItemGetter func, int escaped)
{
  int i;
  size_t count = 0;
  char *str;

  // count the number of matching names
  for (i = 0;; i++) {
    str = (*func)(xp, i);
    if (str == NULL) {  // end of list
      break;
    }
    if (*str == NUL) {  // skip empty strings
      continue;
    }
    if (vim_regexec(regmatch, str, (colnr_T)0)) {
      count++;
    }
  }
  if (count == 0) {
    return;
  }
  assert(count < INT_MAX);
  *num_file = (int)count;
  *file = xmalloc(count * sizeof(char_u *));

  // copy the matching names into allocated memory
  count = 0;
  for (i = 0;; i++) {
    str = (*func)(xp, i);
    if (str == NULL) {  // End of list.
      break;
    }
    if (*str == NUL) {  // Skip empty strings.
      continue;
    }
    if (vim_regexec(regmatch, str, (colnr_T)0)) {
      if (escaped) {
        str = (char *)vim_strsave_escaped((char_u *)str, (char_u *)" \t\\.");
      } else {
        str = xstrdup(str);
      }
      (*file)[count++] = str;
      if (func == get_menu_names) {
        // Test for separator added by get_menu_names().
        str += strlen(str) - 1;
        if (*str == '\001') {
          *str = '.';
        }
      }
    }
  }

  // Sort the results.  Keep menu's in the specified order.
  if (xp->xp_context != EXPAND_MENUNAMES && xp->xp_context != EXPAND_MENUS) {
    if (xp->xp_context == EXPAND_EXPRESSION
        || xp->xp_context == EXPAND_FUNCTIONS
        || xp->xp_context == EXPAND_USER_FUNC) {
      // <SNR> functions should be sorted to the end.
      qsort((void *)(*file), (size_t)(*num_file), sizeof(char *), sort_func_compare);
    } else {
      sort_strings(*file, *num_file);
    }
  }

  // Reset the variables used for special highlight names expansion, so that
  // they don't show up when getting normal highlight names by ID.
  reset_expand_highlight();
}

/// Complete a shell command.
///
/// @param      filepat  is a pattern to match with command names.
/// @param[out] num_file is pointer to number of matches.
/// @param[out] file     is pointer to array of pointers to matches.
///                      *file will either be set to NULL or point to
///                      allocated memory.
/// @param      flagsarg is a combination of EW_* flags.
static void expand_shellcmd(char *filepat, int *num_file, char ***file, int flagsarg)
  FUNC_ATTR_NONNULL_ALL
{
  char *pat;
  int i;
  char *path = NULL;
  garray_T ga;
  char *buf = xmalloc(MAXPATHL);
  size_t l;
  char *s, *e;
  int flags = flagsarg;
  int ret;
  bool did_curdir = false;

  // for ":set path=" and ":set tags=" halve backslashes for escaped space
  pat = xstrdup(filepat);
  for (i = 0; pat[i]; i++) {
    if (pat[i] == '\\' && pat[i + 1] == ' ') {
      STRMOVE(pat + i, pat + i + 1);
    }
  }

  flags |= EW_FILE | EW_EXEC | EW_SHELLCMD;

  bool mustfree = false;  // Track memory allocation for *path.
  if (pat[0] == '.' && (vim_ispathsep(pat[1])
                        || (pat[1] == '.' && vim_ispathsep(pat[2])))) {
    path = ".";
  } else {
    // For an absolute name we don't use $PATH.
    if (!path_is_absolute((char_u *)pat)) {
      path = vim_getenv("PATH");
    }
    if (path == NULL) {
      path = "";
    } else {
      mustfree = true;
    }
  }

  // Go over all directories in $PATH.  Expand matches in that directory and
  // collect them in "ga". When "." is not in $PATH also expand for the
  // current directory, to find "subdir/cmd".
  ga_init(&ga, (int)sizeof(char *), 10);
  hashtab_T found_ht;
  hash_init(&found_ht);
  for (s = path;; s = e) {
    e = vim_strchr(s, ENV_SEPCHAR);
    if (e == NULL) {
      e = s + strlen(s);
    }

    if (*s == NUL) {
      if (did_curdir) {
        break;
      }
      // Find directories in the current directory, path is empty.
      did_curdir = true;
      flags |= EW_DIR;
    } else if (STRNCMP(s, ".", e - s) == 0) {
      did_curdir = true;
      flags |= EW_DIR;
    } else {
      // Do not match directories inside a $PATH item.
      flags &= ~EW_DIR;
    }

    l = (size_t)(e - s);
    if (l > MAXPATHL - 5) {
      break;
    }
    STRLCPY(buf, s, l + 1);
    add_pathsep(buf);
    l = strlen(buf);
    STRLCPY(buf + l, pat, MAXPATHL - l);

    // Expand matches in one directory of $PATH.
    ret = expand_wildcards(1, &buf, num_file, file, flags);
    if (ret == OK) {
      ga_grow(&ga, *num_file);
      {
        for (i = 0; i < *num_file; i++) {
          char_u *name = (char_u *)(*file)[i];

          if (STRLEN(name) > l) {
            // Check if this name was already found.
            hash_T hash = hash_hash(name + l);
            hashitem_T *hi =
              hash_lookup(&found_ht, (const char *)(name + l),
                          STRLEN(name + l), hash);
            if (HASHITEM_EMPTY(hi)) {
              // Remove the path that was prepended.
              STRMOVE(name, name + l);
              ((char_u **)ga.ga_data)[ga.ga_len++] = name;
              hash_add_item(&found_ht, hi, name, hash);
              name = NULL;
            }
          }
          xfree(name);
        }
        xfree(*file);
      }
    }
    if (*e != NUL) {
      e++;
    }
  }
  *file = ga.ga_data;
  *num_file = ga.ga_len;

  xfree(buf);
  xfree(pat);
  if (mustfree) {
    xfree(path);
  }
  hash_clear(&found_ht);
}

/// Call "user_expand_func()" to invoke a user defined Vim script function and
/// return the result (either a string, a List or NULL).
static void *call_user_expand_func(user_expand_func_T user_expand_func, expand_T *xp, int *num_file,
                                   char ***file)
  FUNC_ATTR_NONNULL_ALL
{
  CmdlineInfo *const ccline = get_cmdline_info();
  char keep = 0;
  typval_T args[4];
  char *pat = NULL;
  const sctx_T save_current_sctx = current_sctx;

  if (xp->xp_arg == NULL || xp->xp_arg[0] == '\0' || xp->xp_line == NULL) {
    return NULL;
  }
  *num_file = 0;
  *file = NULL;

  if (ccline->cmdbuff != NULL) {
    keep = ccline->cmdbuff[ccline->cmdlen];
    ccline->cmdbuff[ccline->cmdlen] = 0;
  }

  pat = xstrnsave(xp->xp_pattern, xp->xp_pattern_len);
  args[0].v_type = VAR_STRING;
  args[1].v_type = VAR_STRING;
  args[2].v_type = VAR_NUMBER;
  args[3].v_type = VAR_UNKNOWN;
  args[0].vval.v_string = pat;
  args[1].vval.v_string = xp->xp_line;
  args[2].vval.v_number = xp->xp_col;

  current_sctx = xp->xp_script_ctx;

  void *const ret = user_expand_func((char_u *)xp->xp_arg, 3, args);

  current_sctx = save_current_sctx;
  if (ccline->cmdbuff != NULL) {
    ccline->cmdbuff[ccline->cmdlen] = keep;
  }

  xfree(pat);
  return ret;
}

/// Expand names with a function defined by the user.
static int ExpandUserDefined(expand_T *xp, regmatch_T *regmatch, int *num_file, char ***file)
{
  char *e;
  garray_T ga;

  char *const retstr = call_user_expand_func((user_expand_func_T)call_func_retstr, xp, num_file,
                                             file);

  if (retstr == NULL) {
    return FAIL;
  }

  ga_init(&ga, (int)sizeof(char *), 3);
  for (char *s = retstr; *s != NUL; s = e) {
    e = vim_strchr(s, '\n');
    if (e == NULL) {
      e = s + strlen(s);
    }
    const char keep = *e;
    *e = NUL;

    const bool skip = xp->xp_pattern[0] && vim_regexec(regmatch, s, (colnr_T)0) == 0;
    *e = keep;
    if (!skip) {
      GA_APPEND(char *, &ga, xstrnsave(s, (size_t)(e - s)));
    }

    if (*e != NUL) {
      e++;
    }
  }
  xfree(retstr);
  *file = ga.ga_data;
  *num_file = ga.ga_len;
  return OK;
}

/// Expand names with a list returned by a function defined by the user.
static int ExpandUserList(expand_T *xp, int *num_file, char ***file)
{
  list_T *const retlist = call_user_expand_func((user_expand_func_T)call_func_retlist, xp, num_file,
                                                file);
  if (retlist == NULL) {
    return FAIL;
  }

  garray_T ga;
  ga_init(&ga, (int)sizeof(char *), 3);
  // Loop over the items in the list.
  TV_LIST_ITER_CONST(retlist, li, {
    if (TV_LIST_ITEM_TV(li)->v_type != VAR_STRING
        || TV_LIST_ITEM_TV(li)->vval.v_string == NULL) {
      continue;  // Skip non-string items and empty strings.
    }

    GA_APPEND(char *, &ga, xstrdup((const char *)TV_LIST_ITEM_TV(li)->vval.v_string));
  });
  tv_list_unref(retlist);

  *file = ga.ga_data;
  *num_file = ga.ga_len;
  return OK;
}

static int ExpandUserLua(expand_T *xp, int *num_file, char ***file)
{
  typval_T rettv;
  nlua_call_user_expand_func(xp, &rettv);
  if (rettv.v_type != VAR_LIST) {
    tv_clear(&rettv);
    return FAIL;
  }

  list_T *const retlist = rettv.vval.v_list;

  garray_T ga;
  ga_init(&ga, (int)sizeof(char *), 3);
  // Loop over the items in the list.
  TV_LIST_ITER_CONST(retlist, li, {
    if (TV_LIST_ITEM_TV(li)->v_type != VAR_STRING
        || TV_LIST_ITEM_TV(li)->vval.v_string == NULL) {
      continue;  // Skip non-string items and empty strings.
    }

    GA_APPEND(char *, &ga, xstrdup((const char *)TV_LIST_ITEM_TV(li)->vval.v_string));
  });
  tv_list_unref(retlist);

  *file = ga.ga_data;
  *num_file = ga.ga_len;
  return OK;
}

/// Expand `file` for all comma-separated directories in `path`.
/// Adds matches to `ga`.
void globpath(char *path, char *file, garray_T *ga, int expand_options)
{
  expand_T xpc;
  ExpandInit(&xpc);
  xpc.xp_context = EXPAND_FILES;

  char_u *buf = xmalloc(MAXPATHL);

  // Loop over all entries in {path}.
  while (*path != NUL) {
    // Copy one item of the path to buf[] and concatenate the file name.
    copy_option_part(&path, (char *)buf, MAXPATHL, ",");
    if (STRLEN(buf) + strlen(file) + 2 < MAXPATHL) {
      add_pathsep((char *)buf);
      STRCAT(buf, file);  // NOLINT

      char **p;
      int num_p = 0;
      (void)ExpandFromContext(&xpc, buf, &num_p, &p,
                              WILD_SILENT | expand_options);
      if (num_p > 0) {
        ExpandEscape(&xpc, buf, num_p, p, WILD_SILENT | expand_options);

        // Concatenate new results to previous ones.
        ga_grow(ga, num_p);
        // take over the pointers and put them in "ga"
        for (int i = 0; i < num_p; i++) {
          ((char_u **)ga->ga_data)[ga->ga_len] = (char_u *)p[i];
          ga->ga_len++;
        }
        xfree(p);
      }
    }
  }

  xfree(buf);
}

/// Translate some keys pressed when 'wildmenu' is used.
int wildmenu_translate_key(CmdlineInfo *cclp, int key, expand_T *xp, int did_wild_list)
{
  int c = key;

  if (did_wild_list) {
    if (c == K_LEFT) {
      c = Ctrl_P;
    } else if (c == K_RIGHT) {
      c = Ctrl_N;
    }
  }

  // Hitting CR after "emenu Name.": complete submenu
  if (xp->xp_context == EXPAND_MENUNAMES
      && cclp->cmdpos > 1
      && cclp->cmdbuff[cclp->cmdpos - 1] == '.'
      && cclp->cmdbuff[cclp->cmdpos - 2] != '\\'
      && (c == '\n' || c == '\r' || c == K_KENTER)) {
    c = K_DOWN;
  }

  return c;
}

/// Delete characters on the command line, from "from" to the current position.
static void cmdline_del(CmdlineInfo *cclp, int from)
{
  assert(cclp->cmdpos <= cclp->cmdlen);
  memmove(cclp->cmdbuff + from, cclp->cmdbuff + cclp->cmdpos,
          (size_t)cclp->cmdlen - (size_t)cclp->cmdpos + 1);
  cclp->cmdlen -= cclp->cmdpos - from;
  cclp->cmdpos = from;
}

/// Handle a key pressed when wild menu is displayed
int wildmenu_process_key(CmdlineInfo *cclp, int key, expand_T *xp)
{
  int c = key;

  // Special translations for 'wildmenu'
  if (xp->xp_context == EXPAND_MENUNAMES) {
    // Hitting <Down> after "emenu Name.": complete submenu
    if (c == K_DOWN && cclp->cmdpos > 0
        && cclp->cmdbuff[cclp->cmdpos - 1] == '.') {
      c = (int)p_wc;
      KeyTyped = true;  // in case the key was mapped
    } else if (c == K_UP) {
      // Hitting <Up>: Remove one submenu name in front of the
      // cursor
      int found = false;

      int j = (int)(xp->xp_pattern - cclp->cmdbuff);
      int i = 0;
      while (--j > 0) {
        // check for start of menu name
        if (cclp->cmdbuff[j] == ' '
            && cclp->cmdbuff[j - 1] != '\\') {
          i = j + 1;
          break;
        }

        // check for start of submenu name
        if (cclp->cmdbuff[j] == '.'
            && cclp->cmdbuff[j - 1] != '\\') {
          if (found) {
            i = j + 1;
            break;
          } else {
            found = true;
          }
        }
      }
      if (i > 0) {
        cmdline_del(cclp, i);
      }
      c = (int)p_wc;
      KeyTyped = true;  // in case the key was mapped
      xp->xp_context = EXPAND_NOTHING;
    }
  }
  if (xp->xp_context == EXPAND_FILES
      || xp->xp_context == EXPAND_DIRECTORIES
      || xp->xp_context == EXPAND_SHELLCMD) {
    char_u upseg[5];

    upseg[0] = PATHSEP;
    upseg[1] = '.';
    upseg[2] = '.';
    upseg[3] = PATHSEP;
    upseg[4] = NUL;

    if (c == K_DOWN
        && cclp->cmdpos > 0
        && cclp->cmdbuff[cclp->cmdpos - 1] == PATHSEP
        && (cclp->cmdpos < 3
            || cclp->cmdbuff[cclp->cmdpos - 2] != '.'
            || cclp->cmdbuff[cclp->cmdpos - 3] != '.')) {
      // go down a directory
      c = (int)p_wc;
      KeyTyped = true;  // in case the key was mapped
    } else if (STRNCMP(xp->xp_pattern, upseg + 1, 3) == 0
               && c == K_DOWN) {
      // If in a direct ancestor, strip off one ../ to go down
      int found = false;

      int j = cclp->cmdpos;
      int i = (int)(xp->xp_pattern - cclp->cmdbuff);
      while (--j > i) {
        j -= utf_head_off(cclp->cmdbuff, cclp->cmdbuff + j);
        if (vim_ispathsep(cclp->cmdbuff[j])) {
          found = true;
          break;
        }
      }
      if (found
          && cclp->cmdbuff[j - 1] == '.'
          && cclp->cmdbuff[j - 2] == '.'
          && (vim_ispathsep(cclp->cmdbuff[j - 3]) || j == i + 2)) {
        cmdline_del(cclp, j - 2);
        c = (int)p_wc;
        KeyTyped = true;  // in case the key was mapped
      }
    } else if (c == K_UP) {
      // go up a directory
      int found = false;

      int j = cclp->cmdpos - 1;
      int i = (int)(xp->xp_pattern - cclp->cmdbuff);
      while (--j > i) {
        j -= utf_head_off(cclp->cmdbuff, cclp->cmdbuff + j);
        if (vim_ispathsep(cclp->cmdbuff[j])
#ifdef BACKSLASH_IN_FILENAME
            && vim_strchr((const char_u *)" *?[{`$%#", cclp->cmdbuff[j + 1])
            == NULL
#endif
            ) {
          if (found) {
            i = j + 1;
            break;
          } else {
            found = true;
          }
        }
      }

      if (!found) {
        j = i;
      } else if (STRNCMP(cclp->cmdbuff + j, upseg, 4) == 0) {
        j += 4;
      } else if (STRNCMP(cclp->cmdbuff + j, upseg + 1, 3) == 0
                 && j == i) {
        j += 3;
      } else {
        j = 0;
      }

      if (j > 0) {
        // TODO(tarruda): this is only for DOS/Unix systems - need to put in
        // machine-specific stuff here and in upseg init
        cmdline_del(cclp, j);
        put_on_cmdline(upseg + 1, 3, false);
      } else if (cclp->cmdpos > i) {
        cmdline_del(cclp, i);
      }

      // Now complete in the new directory. Set KeyTyped in case the
      // Up key came from a mapping.
      c = (int)p_wc;
      KeyTyped = true;
    }
  }

  return c;
}

/// Free expanded names when finished walking through the matches
void wildmenu_cleanup(CmdlineInfo *cclp)
{
  if (!p_wmnu || wild_menu_showing == 0) {
    return;
  }

  const bool skt = KeyTyped;
  const int old_RedrawingDisabled = RedrawingDisabled;

  if (cclp->input_fn) {
    RedrawingDisabled = 0;
  }

  if (wild_menu_showing == WM_SCROLLED) {
    // Entered command line, move it up
    cmdline_row--;
    redrawcmd();
    wild_menu_showing = 0;
  } else if (save_p_ls != -1) {
    // restore 'laststatus' and 'winminheight'
    p_ls = save_p_ls;
    p_wmh = save_p_wmh;
    last_status(false);
    update_screen();  // redraw the screen NOW
    redrawcmd();
    save_p_ls = -1;
    wild_menu_showing = 0;
    // don't redraw statusline if WM_LIST is showing
  } else if (wild_menu_showing != WM_LIST) {
    win_redraw_last_status(topframe);
    wild_menu_showing = 0;  // must be before redraw_statuslines #8385
    redraw_statuslines();
  } else {
    wild_menu_showing = 0;
  }
  KeyTyped = skt;
  if (cclp->input_fn) {
    RedrawingDisabled = old_RedrawingDisabled;
  }
}

/// "getcompletion()" function
void f_getcompletion(typval_T *argvars, typval_T *rettv, EvalFuncData fptr)
{
  char_u *pat;
  expand_T xpc;
  bool filtered = false;
  int options = WILD_SILENT | WILD_USE_NL | WILD_ADD_SLASH
                | WILD_NO_BEEP | WILD_HOME_REPLACE;

  if (argvars[1].v_type != VAR_STRING) {
    semsg(_(e_invarg2), "type must be a string");
    return;
  }
  const char *const type = tv_get_string(&argvars[1]);

  if (argvars[2].v_type != VAR_UNKNOWN) {
    filtered = (bool)tv_get_number_chk(&argvars[2], NULL);
  }

  if (p_wic) {
    options |= WILD_ICASE;
  }

  // For filtered results, 'wildignore' is used
  if (!filtered) {
    options |= WILD_KEEP_ALL;
  }

  if (argvars[0].v_type != VAR_STRING) {
    emsg(_(e_invarg));
    return;
  }
  const char *pattern = tv_get_string(&argvars[0]);

  if (strcmp(type, "cmdline") == 0) {
    set_one_cmd_context(&xpc, pattern);
    xpc.xp_pattern_len = strlen(xpc.xp_pattern);
    xpc.xp_col = (int)strlen(pattern);
    goto theend;
  }

  ExpandInit(&xpc);
  xpc.xp_pattern = (char *)pattern;
  xpc.xp_pattern_len = strlen(xpc.xp_pattern);
  xpc.xp_context = cmdcomplete_str_to_type(type);
  if (xpc.xp_context == EXPAND_NOTHING) {
    semsg(_(e_invarg2), type);
    return;
  }

  if (xpc.xp_context == EXPAND_MENUS) {
    set_context_in_menu_cmd(&xpc, "menu", xpc.xp_pattern, false);
    xpc.xp_pattern_len = strlen(xpc.xp_pattern);
  }

  if (xpc.xp_context == EXPAND_SIGN) {
    set_context_in_sign_cmd(&xpc, xpc.xp_pattern);
    xpc.xp_pattern_len = strlen(xpc.xp_pattern);
  }

theend:
  pat = (char_u *)addstar(xpc.xp_pattern, xpc.xp_pattern_len, xpc.xp_context);
  ExpandOne(&xpc, (char *)pat, NULL, options, WILD_ALL_KEEP);
  tv_list_alloc_ret(rettv, xpc.xp_numfiles);

  for (int i = 0; i < xpc.xp_numfiles; i++) {
    tv_list_append_string(rettv->vval.v_list, (const char *)xpc.xp_files[i],
                          -1);
  }
  xfree(pat);
  ExpandCleanup(&xpc);
}
