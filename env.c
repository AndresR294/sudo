/*
 * Copyright (c) 2000-2004 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Sponsored in part by the Defense Advanced Research Projects
 * Agency (DARPA) and Air Force Research Laboratory, Air Force
 * Materiel Command, USAF, under agreement number F39502-99-1-0512.
 */

#include <config.h>

#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <stdio.h>
#ifdef STDC_HEADERS
# include <stdlib.h>
# include <stddef.h>
#else
# ifdef HAVE_STDLIB_H
#  include <stdlib.h>
# endif
#endif /* STDC_HEADERS */
#ifdef HAVE_STRING_H
# include <string.h>
#else
# ifdef HAVE_STRINGS_H
#  include <strings.h>
# endif
#endif /* HAVE_STRING_H */
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <pwd.h>

#include "sudo.h"

#ifndef lint
__unused static const char rcsid[] = "$Sudo$";
#endif /* lint */

/*
 * Flags used in rebuild_env()
 */
#undef DID_TERM
#define DID_TERM	0x01
#undef DID_PATH
#define DID_PATH	0x02
#undef DID_HOME
#define DID_HOME	0x04
#undef DID_SHELL
#define DID_SHELL	0x08
#undef DID_LOGNAME
#define DID_LOGNAME	0x10
#undef DID_USER
#define DID_USER    	0x20

#undef VNULL
#define	VNULL	(VOID *)NULL

struct environment {
    char **envp;		/* pointer to the new environment */
    size_t env_size;		/* size of new_environ in char **'s */
    size_t env_len;		/* number of slots used, not counting NULL */
};

/*
 * Prototypes
 */
char **rebuild_env		__P((char **, char **, int, int));
char **zero_env			__P((char **));
static void insert_env		__P((char *, struct environment *, int));
static char *format_env		__P((char *, ...));
static int var_ok		__P((char *));

/*
 * Default table of "bad" variables to remove from the environment.
 * XXX - how to omit TERMCAP if it starts with '/'?
 */
static const char *initial_badenv_table[] = {
    "IFS",
    "CDPATH",
    "LOCALDOMAIN",
    "RES_OPTIONS",
    "HOSTALIASES",
    "NLSPATH",
    "PATH_LOCALE",
    "LD_*",
    "_RLD*",
#ifdef __hpux
    "SHLIB_PATH",
#endif /* __hpux */
#ifdef _AIX
    "LIBPATH",
#endif /* _AIX */
#ifdef __APPLE__
    "DYLD_*",
#endif
#ifdef HAVE_KERB4
    "KRB_CONF*",
    "KRBCONFDIR",
    "KRBTKFILE",
#endif /* HAVE_KERB4 */
#ifdef HAVE_KERB5
    "KRB5_CONFIG*",
#endif /* HAVE_KERB5 */
#ifdef HAVE_SECURID
    "VAR_ACE",
    "USR_ACE",
    "DLC_ACE",
#endif /* HAVE_SECURID */
    "TERMINFO",
    "TERMINFO_DIRS",
    "TERMPATH",
    "TERMCAP",			/* XXX - only if it starts with '/' */
    "ENV",
    "BASH_ENV",
    NULL
};

/*
 * Default table of variables to check for '%' and '/' characters.
 */
static const char *initial_checkenv_table[] = {
    "LC_*",
    "LANG",
    "LANGUAGE",
    NULL
};

/*
 * Default table of variables to preserve in the environment.
 */
static const char *initial_keepenv_table[] = {
    "KRB5CCNAME",
    "PATH",
    "TERM",
    "TZ",
    NULL
};

/*
 * Remove potentially dangerous variables from the environment
 * and returns a vector of what was pruned out.
 * Sets user_path, user_prompt and prev_user as side effects.
 */
char **
clean_env(envp)
    char **envp;
{
    char **ep, **end;
    struct environment pruned_env;
    extern char *prev_user;

    /* Find the end of the environment. */
    for (end = envp; *end; end++)
	continue;

    /*
     * Prune out any bad environment variables and set user_path, user_prompt
     * and prev_user.
     */
    memset(&pruned_env, 0, sizeof(pruned_env));
    for (ep = envp; *ep; ) {
	switch (**ep) {
	    case 'P':
		if (strncmp("PATH=", *ep, 5) == 0)
		    user_path = *ep + 5;
		break;
	    case 'S':
		if (strncmp("SHELL=", *ep, 6) == 0)
		    user_shell = *ep + 6;
		else if (!user_prompt && strncmp("SUDO_PROMPT=", *ep, 12) == 0)
		    user_prompt = *ep + 12;
		else if (strncmp("SUDO_USER=", *ep, 10) == 0)
		    prev_user = *ep + 10;
		break;
	}
	if (var_ok(*ep)) {
	    ep++;
	} else {
	    insert_env(*ep, &pruned_env, 0);
	    memmove(ep, ep + 1, end - ep);
	}
    }

    return (pruned_env.envp);
}

/*
 * Given a variable and value, allocate and format an environment string.
 */
static char *
#ifdef __STDC__
format_env(char *var, ...)
#else
format_env(var, va_alist)
    char *var;
    va_dcl
#endif
{
    char *estring;
    char *val;
    size_t esize;
    va_list ap;

#ifdef __STDC__
    va_start(ap, var);
#else
    va_start(ap);
#endif
    esize = strlen(var) + 2;
    while ((val = va_arg(ap, char *)) != NULL)
	esize += strlen(val);
    va_end(ap);
    estring = (char *) emalloc(esize);

    /* Store variable name and the '=' separator.  */
    if (strlcpy(estring, var, esize) >= esize ||
	strlcat(estring, "=", esize) >= esize) {

	errorx(1, "internal error, format_env() overflow");
    }

    /* Now store the variable's value (if any) */
#ifdef __STDC__
    va_start(ap, var);
#else
    va_start(ap);
#endif
    while ((val = va_arg(ap, char *)) != NULL) {
	if (strlcat(estring, val, esize) >= esize)
	    errorx(1, "internal error, format_env() overflow");
    }
    va_end(ap);

    return(estring);
}

/*
 * Insert str into e->envp, assumes str has an '=' in it.
 */
static void
insert_env(str, e, dupcheck)
    char *str;
    struct environment *e;
    int dupcheck;
{
    char **nep;
    size_t varlen;

    /* Make sure there is room for the new entry plus a NULL. */
    if (e->env_len + 2 > e->env_size) {
	e->env_size += 128;
	e->envp = erealloc3(e->envp, e->env_size, sizeof(char *));
    }

    if (dupcheck) {
	    varlen = (strchr(str, '=') - str) + 1;

	    for (nep = e->envp; *nep; nep++) {
		if (strncmp(str, *nep, varlen) == 0) {
		    *nep = str;
		    return;
		}
	    }
    } else
	nep = e->envp + e->env_len;

    e->env_len++;
    *nep++ = str;
    *nep = NULL;
}

/*
 * Check an environment variable against the env_delete
 * and env_chec lists.  Returns TRUE if allowed, else FALSE.
 */
static int
var_ok(var)
    char *var;
{
    struct list_member *cur;
    size_t len;
    char *cp;
    int iswild, okvar = TRUE;

    /* Skip variables with values beginning with () (bash functions) */
    if ((cp = strchr(var, '=')) != NULL) {
	if (strncmp(cp, "=() ", 3) == 0)
	    return (FALSE);
    }

    /* Skip anything listed in env_delete. */
    for (cur = def_env_delete; cur && okvar; cur = cur->next) {
	len = strlen(cur->value);
	/* Deal with '*' wildcard */
	if (cur->value[len - 1] == '*') {
	    len--;
	    iswild = TRUE;
	} else
	    iswild = FALSE;
	if (strncmp(cur->value, var, len) == 0 &&
	    (iswild || var[len] == '=')) {
	    okvar = FALSE;
	}
    }

    /* Check certain variables for '%' and '/' characters. */
    for (cur = def_env_check; cur && okvar; cur = cur->next) {
	len = strlen(cur->value);
	/* Deal with '*' wildcard */
	if (cur->value[len - 1] == '*') {
	    len--;
	    iswild = TRUE;
	} else
	    iswild = FALSE;
	if (strncmp(cur->value, var, len) == 0 &&
	    (iswild || var[len] == '=') &&
	    strpbrk(var, "/%")) {
	    okvar = FALSE;
	}
    }

    return (okvar);
}

/*
 * Build a new environment and ether clear potentially dangerous
 * variables from the old one or start with a clean slate.
 * Also adds sudo-specific variables (SUDO_*).
 */
char **
rebuild_env(envp1, envp2, sudo_mode, noexec)
    char **envp1;
    char **envp2;
    int sudo_mode;
    int noexec;
{
    struct environment env;
    char **envpv[2], **ep, *cp, *ps1;
    int didvar, i;

    /*
     * Either clean out the environment or reset to a safe default.
     */
    ps1 = NULL;
    didvar = 0;
    envpv[0] = envp1;
    envpv[1] = envp2;
    memset(&env, 0, sizeof(env));
    if (def_env_reset) {
	struct list_member *cur;
	size_t len;
	int iswild, keepit;

	/* Pull in vars we want to keep from the old environment. */
	for (i = 0; i < 2; i++) {
	    if ((ep = envpv[i]) == NULL)
		continue;
	    for (; *ep; ep++) {
		keepit = 0;

		/* Skip variables with values beginning with () (bash functions) */
		if ((cp = strchr(*ep, '=')) != NULL) {
		    if (strncmp(cp, "=() ", 3) == 0)
			continue;
		}

		for (cur = def_env_keep; cur; cur = cur->next) {
		    len = strlen(cur->value);
		    /* Deal with '*' wildcard */
		    if (cur->value[len - 1] == '*') {
			len--;
			iswild = 1;
		    } else
			iswild = 0;
		    if (strncmp(cur->value, *ep, len) == 0 &&
			(iswild || (*ep)[len] == '=')) {
			keepit = 1;
			break;
		    }
		}

		/* For SUDO_PS1 -> PS1 conversion. */
		if (strncmp(*ep, "SUDO_PS1=", 8) == 0)
		    ps1 = *ep + 5;

		if (keepit) {
		    /* Preserve variable. */
		    switch (**ep) {
			case 'H':
			    if (strncmp(*ep, "HOME=", 5) == 0)
				SET(didvar, DID_HOME);
			    break;
			case 'L':
			    if (strncmp(*ep, "LOGNAME=", 8) == 0)
				SET(didvar, DID_LOGNAME);
			    break;
			case 'P':
			    if (strncmp(*ep, "PATH=", 5) == 0)
				SET(didvar, DID_PATH);
			    break;
			case 'S':
			    if (strncmp(*ep, "SHELL=", 6) == 0)
				SET(didvar, DID_SHELL);
			    break;
			case 'T':
			    if (strncmp(*ep, "TERM=", 5) == 0)
				SET(didvar, DID_TERM);
			    break;
			case 'U':
			    if (strncmp(*ep, "USER=", 5) == 0)
				SET(didvar, DID_USER);
			    break;
		    }
		    insert_env(*ep, &env, 0);
		}
	    }
	}

	/*
	 * Add in defaults.  In -i mode these come from the runas user,
	 * otherwise they may be from the user's environment (depends
	 * on sudoers options).
	 */
	if (ISSET(sudo_mode, MODE_LOGIN_SHELL)) {
	    insert_env(format_env("HOME", runas_pw->pw_dir, VNULL), &env, 0);
	    insert_env(format_env("SHELL", runas_pw->pw_shell, VNULL), &env, 0);
	    insert_env(format_env("LOGNAME", runas_pw->pw_name, VNULL), &env, 0);
	    insert_env(format_env("USER", runas_pw->pw_name, VNULL), &env, 0);
	} else {
	    if (!ISSET(didvar, DID_HOME))
		insert_env(format_env("HOME", user_dir, VNULL), &env, 0);
	    if (!ISSET(didvar, DID_SHELL))
		insert_env(format_env("SHELL", sudo_user.pw->pw_shell, VNULL),
		    &env, 0);
	    if (!ISSET(didvar, DID_LOGNAME))
		insert_env(format_env("LOGNAME", user_name, VNULL), &env, 0);
	    if (!ISSET(didvar, DID_USER))
		insert_env(format_env("USER", user_name, VNULL), &env, 0);
	}
    } else {
	/*
	 * Copy envp entries as long as they don't match env_delete or
	 * env_check.
	 */
	for (i = 0; i < 2; i++) {
	    if ((ep = envpv[i]) == NULL)
		continue;
	    for (; *ep; ep++) {
		if (var_ok(*ep)) {
		    if (strncmp(*ep, "SUDO_PS1=", 9) == 0)
			ps1 = *ep + 5;
		    else if (strncmp(*ep, "PATH=", 5) == 0)
			SET(didvar, DID_PATH);
		    else if (strncmp(*ep, "TERM=", 5) == 0)
			SET(didvar, DID_TERM);
		    insert_env(*ep, &env, 0);
		}
	    }
	}
    }
    /* Replace the PATH envariable with a secure one? */
    if (def_secure_path && !user_is_exempt()) {
	insert_env(format_env("PATH", def_secure_path, VNULL), &env, 1);
	SET(didvar, DID_PATH);
    }

    /* Set $USER and $LOGNAME to target if "set_logname" is true. */
    if (def_env_reset && runas_pw->pw_name) {
	insert_env(format_env("LOGNAME", runas_pw->pw_name, VNULL), &env, 1);
	insert_env(format_env("USER", runas_pw->pw_name, VNULL), &env, 1);
    }

    /* Set $HOME for `sudo -H'.  Only valid at PERM_FULL_RUNAS. */
    if ((def_env_reset || ISSET(sudo_mode, MODE_RESET_HOME)) && runas_pw->pw_dir)
	insert_env(format_env("HOME", runas_pw->pw_dir, VNULL), &env, 1);

    /* Provide default values for $TERM and $PATH if they are not set. */
    if (!ISSET(didvar, DID_TERM))
	insert_env("TERM=unknown", &env, 0);
    if (!ISSET(didvar, DID_PATH))
	insert_env(format_env("PATH", _PATH_DEFPATH, VNULL), &env, 0);

    /*
     * Preload a noexec file?  For a list of LD_PRELOAD-alikes, see
     * http://www.fortran-2000.com/ArnaudRecipes/sharedlib.html
     * XXX - should prepend to original value, if any
     */
    if (noexec && def_noexec_file != NULL) {
#if defined(__darwin__) || defined(__APPLE__)
	insert_env(format_env("DYLD_INSERT_LIBRARIES", def_noexec_file, VNULL),
	    &env, 1);
	insert_env(format_env("DYLD_FORCE_FLAT_NAMESPACE", VNULL), &env, 1);
#else
# if defined(__osf__) || defined(__sgi)
	insert_env(format_env("_RLD_LIST", def_noexec_file, ":DEFAULT", VNULL),
	    &env, 1);
# else
	insert_env(format_env("LD_PRELOAD", def_noexec_file, VNULL), &env, 1);
# endif
#endif
    }

    /* Set PS1 if SUDO_PS1 is set. */
    if (ps1)
	insert_env(ps1, &env, 1);

    /* Add the SUDO_COMMAND envariable (cmnd + args). */
    if (user_args)
	insert_env(format_env("SUDO_COMMAND", user_cmnd, " ", user_args, VNULL),
	    &env, 1);
    else
	insert_env(format_env("SUDO_COMMAND", user_cmnd, VNULL), &env, 1);

    /* Add the SUDO_USER, SUDO_UID, SUDO_GID environment variables. */
    insert_env(format_env("SUDO_USER", user_name, VNULL), &env, 1);
    easprintf(&cp, "SUDO_UID=%lu", (unsigned long) user_uid);
    insert_env(cp, &env, 1);
    easprintf(&cp, "SUDO_GID=%lu", (unsigned long) user_gid);
    insert_env(cp, &env, 1);

    return(env.envp);
}

void
init_envtables()
{
    struct list_member *cur;
    const char **p;

    /* Fill in "env_delete" variable. */
    for (p = initial_badenv_table; *p; p++) {
	cur = emalloc(sizeof(struct list_member));
	cur->value = estrdup(*p);
	cur->next = def_env_delete;
	def_env_delete = cur;
    }

    /* Fill in "env_check" variable. */
    for (p = initial_checkenv_table; *p; p++) {
	cur = emalloc(sizeof(struct list_member));
	cur->value = estrdup(*p);
	cur->next = def_env_check;
	def_env_check = cur;
    }

    /* Fill in "env_keep" variable. */
    for (p = initial_keepenv_table; *p; p++) {
	cur = emalloc(sizeof(struct list_member));
	cur->value = estrdup(*p);
	cur->next = def_env_keep;
	def_env_keep = cur;
    }
}
