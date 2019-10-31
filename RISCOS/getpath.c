/* Return the initial module search path. */

#include "Python.h"
#include "pycore_initconfig.h"
#include "osdefs.h"
#include "pycore_fileutils.h"
#include "pycore_pathconfig.h"
#include "pycore_pystate.h"

#include "oslib/os.h"

#include <sys/types.h>
#include <string.h>

/* Search in some common locations for the associated Python libraries.
 *
 * Two directories must be found, the platform independent directory
 * (prefix), containing the common .py and .pyc files, and the platform
 * dependent directory (exec_prefix), containing the shared library
 * modules.  Note that prefix and exec_prefix can be the same directory,
 * but for some installations, they are different.
 *
 * Py_GetPath() carries out separate searches for prefix and exec_prefix.
 * Each search tries a number of different locations until a ``landmark''
 * file or directory is found.  If no prefix or exec_prefix is found, a
 * warning message is issued and the preprocessor defined PREFIX and
 * EXEC_PREFIX are used (even though they will not work); python carries on
 * as best as is possible, but most imports will fail.
 *
 * Before any searches are done, the location of the executable is
 * determined.  If argv[0] has one or more slashes in it, it is used
 * unchanged.  Otherwise, it must have been invoked from the shell's path,
 * so we search $PATH for the named executable and use that.  If the
 * executable was not found on $PATH (or there was no $PATH environment
 * variable), the original argv[0] string is used.
 *
 * Next, the executable location is examined to see if it is a symbolic
 * link.  If so, the link is chased (correctly interpreting a relative
 * pathname if one is found) and the directory of the link target is used.
 *
 * Finally, argv0_path is set to the directory containing the executable
 * (i.e. the last component is stripped).
 *
 * With argv0_path in hand, we perform a number of steps.  The same steps
 * are performed for prefix and for exec_prefix, but with a different
 * landmark.
 *
 * Step 1. Are we running python out of the build directory?  This is
 * checked by looking for a different kind of landmark relative to
 * argv0_path.  For prefix, the landmark's path is derived from the VPATH
 * preprocessor variable (taking into account that its value is almost, but
 * not quite, what we need).  For exec_prefix, the landmark is
 * pybuilddir.txt.  If the landmark is found, we're done.
 *
 * For the remaining steps, the prefix landmark will always be
 * lib/python$VERSION/os.py and the exec_prefix will always be
 * lib/python$VERSION/lib-dynload, where $VERSION is Python's version
 * number as supplied by the Makefile.  Note that this means that no more
 * build directory checking is performed; if the first step did not find
 * the landmarks, the assumption is that python is running from an
 * installed setup.
 *
 * Step 2. See if the $PYTHONHOME environment variable points to the
 * installed location of the Python libraries.  If $PYTHONHOME is set, then
 * it points to prefix and exec_prefix.  $PYTHONHOME can be a single
 * directory, which is used for both, or the prefix and exec_prefix
 * directories separated by a colon.
 *
 * Step 3. Try to find prefix and exec_prefix relative to argv0_path,
 * backtracking up the path until it is exhausted.  This is the most common
 * step to succeed.  Note that if prefix and exec_prefix are different,
 * exec_prefix is more likely to be found; however if exec_prefix is a
 * subdirectory of prefix, both will be found.
 *
 * Step 4. Search the directories pointed to by the preprocessor variables
 * PREFIX and EXEC_PREFIX.  These are supplied by the Makefile but can be
 * passed in as options to the configure script.
 *
 * That's it!
 *
 * Well, almost.  Once we have determined prefix and exec_prefix, the
 * preprocessor variable PYTHONPATH is used to construct a path.  Each
 * relative path on PYTHONPATH is prefixed with prefix.  Then the directory
 * containing the shared library modules is appended.  The environment
 * variable $PYTHONPATH is inserted in front of it all.  Finally, the
 * prefix and exec_prefix globals are tweaked so they reflect the values
 * expected by other code, by stripping the "lib/python$VERSION/..." stuff
 * off.  If either points to the build directory, the globals are reset to
 * the corresponding preprocessor variables (so sys.prefix will reflect the
 * installation location, even though sys.path points into the build
 * directory).  This seems to make more sense given that currently the only
 * known use of sys.prefix and sys.exec_prefix is for the ILU installation
 * process to find the installed Python tree.
 *
 * An embedding application can use Py_SetPath() to override all of
 * these authomatic path computations.
 *
 * NOTE: Windows MSVC builds use PC/getpathp.c instead!
 */


#warning CMJ: Memory leaks - canonicalise_path returns a pointer that needs to be freed
#warning CMJ: Version hardcoded

#ifdef __cplusplus
extern "C" {
#endif

#include <swis.h>

#if !defined(VERSION)
//#error "VERSION must be constant defined"
#endif

#ifndef LANDMARK
#define LANDMARK L"os.py"
#endif

#define DECODE_LOCALE_ERR(NAME, LEN) \
    ((LEN) == (size_t)-2) \
     ? _PyStatus_ERR("cannot decode " NAME) \
     : _PyStatus_NO_MEMORY()

#define PATHLEN_ERR() _PyStatus_ERR("path configuration: path too long")


#if 0
// This is the PC version...

typedef struct {
    const wchar_t *path_env;           /* PATH environment variable */
    const wchar_t *home;               /* PYTHONHOME environment variable */

    /* Registry key "Software\Python\PythonCore\X.Y\PythonPath"
       where X.Y is the Python version (major.minor) */
    wchar_t *machine_path;   /* from HKEY_LOCAL_MACHINE */
    wchar_t *user_path;      /* from HKEY_CURRENT_USER */

    wchar_t *dll_path;

    const wchar_t *pythonpath_env;
} PyCalculatePath;
#endif


typedef struct {
    //wchar_t *path_env;                 /* PATH environment variable */
    wchar_t *pythonpath;               /* PYTHONPATH environment variable */
    wchar_t *pythonhome;               /* PYTHONHOME environment variable */
    wchar_t *lib_dir;                  /* <PythonX.YLib$Dir> */
    wchar_t *dynload_dir;              /* <PythonX.YDynLoad$Dir> */
    //wchar_t *pythonpath;               /* PYTHONPATH define */
    //wchar_t *prefix;                   /* PREFIX define */
    //wchar_t *exec_prefix;              /* EXEC_PREFIX define */

    wchar_t argv0_path[MAXPATHLEN+1];
    //wchar_t zip_path[MAXPATHLEN+1];    /* ".../lib/pythonXY.zip" */

    //int prefix_found;         /* found platform independent libraries? */
    //int exec_prefix_found;    /* found the platform dependent libraries? */
} PyCalculatePath;

static const wchar_t delimiter[2] = {DELIM, '\0'};
static const wchar_t separator[2] = {SEP, '\0'};

char* canonicalise_path(const char *path)
{
   signed int spare;
   _swi(OS_FSControl, _INR(0,5) | _OUT(5),
                      37, path, 0, 0, 0, 0, &spare);

   char *buffer = malloc(1-spare);
   _swi(OS_FSControl, _INR(0,5),
                      37, path, buffer, 0, 0, 1-spare);

   return buffer;
}

/* Get file status. Encode the path to the locale encoding. */
static int
_Py_wstat(const wchar_t* path, struct stat *buf)
{
    printf("_Py_wstat(%ls)\n", path);

    int err;
    char *fname;
    fname = _Py_EncodeLocaleRaw(path, NULL);
    if (fname == NULL) {
        errno = EINVAL;
        return -1;
    }
    err = stat(fname, buf);
    PyMem_RawFree(fname);
    return err;
}


static void
reduce(wchar_t *dir)
{
    printf("reduce(%ls) ", dir);

    size_t i = wcslen(dir);
    while (i > 0 && dir[i] != SEP)
        --i;
    dir[i] = '\0';

    printf(" -> %ls\n", dir);

}


static int
isfile(wchar_t *filename)          /* Is file, not directory */
{
    printf("isfile(%ls)\n", filename);

    struct stat buf;
    if (_Py_wstat(filename, &buf) != 0) {
        return 0;
    }
    if (!S_ISREG(buf.st_mode)) {
        return 0;
    }
    return 1;
}


static int
ismodule(wchar_t *filename)        /* Is module -- check for .pyc too */
{
    printf("ismodule(%ls)\n", filename);

    if (isfile(filename)) {
        return 1;
    }

    /* Check for the compiled version of prefix. */
    if (wcslen(filename) < MAXPATHLEN) {
        wcscat(filename, L"c");
        if (isfile(filename)) {
            return 1;
        }
    }
    return 0;
}


/* Is executable file */
static int
isxfile(wchar_t *filename)
{
    printf("isxfile(%ls)\n", filename);

    struct stat buf;
    if (_Py_wstat(filename, &buf) != 0) {
        return 0;
    }
    if (!S_ISREG(buf.st_mode)) {
        return 0;
    }
    if ((buf.st_mode & 0111) == 0) {
        return 0;
    }
    return 1;
}


/* Is directory */
static int
isdir(wchar_t *filename)
{
    printf("isdir(%ls)\n", filename);

    struct stat buf;
    if (_Py_wstat(filename, &buf) != 0) {
        return 0;
    }
    if (!S_ISDIR(buf.st_mode)) {
        return 0;
    }
    return 1;
}

/* Add a path component, by appending stuff to buffer.
   buffer must have at least MAXPATHLEN + 1 bytes allocated, and contain a
   NUL-terminated string with no more than MAXPATHLEN characters (not counting
   the trailing NUL).  It's a fatal error if it contains a string longer than
   that (callers must be careful!).  If these requirements are met, it's
   guaranteed that buffer will still be a NUL-terminated string with no more
   than MAXPATHLEN characters at exit.  If stuff is too long, only as much of
   stuff as fits will be appended.
*/
static void
joinpath(wchar_t *buffer, wchar_t *stuff)
{
    printf("joinpath(%ls, %ls)\n", buffer, stuff);

    size_t n, k;
    if (stuff[0] == SEP) {
        n = 0;
    }
    else {
        n = wcslen(buffer);
        if (n > 0 && buffer[n-1] != SEP && n < MAXPATHLEN) {
            buffer[n++] = SEP;
        }
    }
    if (n > MAXPATHLEN) {
        Py_FatalError("buffer overflow in getpath.c's joinpath()");
    }
    k = wcslen(stuff);
    if (n + k > MAXPATHLEN) {
        k = MAXPATHLEN - n;
    }
    wcsncpy(buffer+n, stuff, k);
    buffer[n+k] = '\0';

}


/* copy_absolute requires that path be allocated at least
   MAXPATHLEN + 1 bytes and that p be no more than MAXPATHLEN bytes. */
static void
copy_absolute(wchar_t *path, wchar_t *p, size_t pathlen)
{
    printf("copy_absolute(%ls)\n", p);

    if (p[0] == SEP) {
        wcscpy(path, p);
    }
    else {
        if (!_Py_wgetcwd(path, pathlen)) {
            /* unable to get the current directory */
            printf("Can't get cwd\n");
            wcscpy(path, p);
            return;
        }

        if (p[0] == '.' && p[1] == SEP) {
            p += 2;
        }

        joinpath(path, p);
    }
}


/* absolutize() requires that path be allocated at least MAXPATHLEN+1 bytes. */
static void
absolutize(wchar_t *path)
{
    printf("absolutize(%ls)\n", path);

    wchar_t buffer[MAXPATHLEN+1];

    if (path[0] == SEP) {
        return;
    }
    copy_absolute(buffer, path, MAXPATHLEN+1);
    wcscpy(path, buffer);
}


static inline int
safe_wcscpy(wchar_t *dst, const wchar_t *src, size_t n)
{
    size_t srclen = wcslen(src);
    if (n <= srclen) {
        dst[0] = L'\0';
        return -1;
    }
    memcpy(dst, src, (srclen + 1) * sizeof(wchar_t));
    return 0;
}

/* search_for_prefix requires that argv0_path be no more than MAXPATHLEN
   bytes long.
*/

static int
search_for_prefix(PyCalculatePath *calculate, _PyPathConfig *pathconfig, wchar_t *prefix)
{
    printf("search_for_prefix(%ls)\n", prefix);

    size_t n;
    wchar_t *vpath;

    /* If PYTHONHOME is set, we believe it unconditionally */
    if (pathconfig->home) {
        wcsncpy(prefix, pathconfig->home, MAXPATHLEN);
        prefix[MAXPATHLEN] = L'\0';
        wchar_t *delim = wcschr(prefix, DELIM);
        if (delim) {
            *delim = L'\0';
        }
        joinpath(prefix, calculate->lib_dir);
        joinpath(prefix, LANDMARK);
        return 1;
    }

#ifndef RISCOS
    /* Check to see if argv[0] is in the build directory */
    wcsncpy(prefix, calculate->argv0_path, MAXPATHLEN);
    prefix[MAXPATHLEN] = L'\0';
    joinpath(prefix, L"Modules/Setup");
    if (isfile(prefix)) {
        /* Check VPATH to see if argv0_path is in the build directory. */
        vpath = Py_DecodeLocale(VPATH, NULL);
        if (vpath != NULL) {
            wcsncpy(prefix, calculate->argv0_path, MAXPATHLEN);
            prefix[MAXPATHLEN] = L'\0';
            joinpath(prefix, vpath);
            PyMem_RawFree(vpath);
            joinpath(prefix, L"Lib");
            joinpath(prefix, LANDMARK);
            if (ismodule(prefix)) {
                return -1;
            }
        }
    }
#endif

    /* Search from argv0_path, until root is found */
    copy_absolute(prefix, calculate->argv0_path, MAXPATHLEN+1);
    do {
        n = wcslen(prefix);
        joinpath(prefix, calculate->lib_dir);
        joinpath(prefix, LANDMARK);
        if (ismodule(prefix)) {
            return 1;
        }
        prefix[n] = L'\0';
        reduce(prefix);
    } while (prefix[0]);

#ifndef RISCOS
    /* Look at configure's PREFIX */
    wcsncpy(prefix, calculate->prefix, MAXPATHLEN);
    prefix[MAXPATHLEN] = L'\0';
    joinpath(prefix, calculate->lib_python);
    joinpath(prefix, LANDMARK);
    if (ismodule(prefix)) {
        return 1;
    }
#endif

    /* Fail */
    return 0;
}


static PyStatus
calculate_prefix(PyCalculatePath *calculate, _PyPathConfig *pathconfig,
                 const wchar_t *argv0_path,
                 wchar_t *prefix, size_t prefix_len)
{
#ifdef RISCOS
    wcsncpy(prefix, calculate->lib_dir, prefix_len);
#else
    PyStatus status;

    status = search_for_prefix(calculate, pathconfig, argv0_path,
                               prefix, prefix_len,
                               &calculate->prefix_found);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    if (!calculate->prefix_found) {
        if (calculate->warnings) {
            fprintf(stderr,
                "Could not find platform independent libraries <prefix>\n");
        }
        if (safe_wcscpy(prefix, calculate->prefix, prefix_len) < 0) {
            return PATHLEN_ERR();
        }
        status = joinpath(prefix, calculate->lib_python, prefix_len);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }
#endif
    return _PyStatus_OK();
}


#if 0
static PyStatus
calculate_set_prefix(PyCalculatePath *calculate, _PyPathConfig *pathconfig,
                     wchar_t *prefix)
{
    /* Reduce prefix and exec_prefix to their essence,
     * e.g. /usr/local/lib/python1.5 is reduced to /usr/local.
     * If we're loading relative to the build directory,
     * return the compiled-in defaults instead.
     */
    if (calculate->prefix_found > 0) {
        reduce(prefix);
        reduce(prefix);
        /* The prefix is the root directory, but reduce() chopped
         * off the "/". */
        if (!prefix[0]) {
            wcscpy(prefix, separator);
        }
        pathconfig->prefix = _PyMem_RawWcsdup(prefix);
    }
    else {
        pathconfig->prefix = _PyMem_RawWcsdup(calculate->prefix);
    }

    if (pathconfig->prefix == NULL) {
        return _PyStatus_NO_MEMORY();
    }
    return _PyStatus_OK();
}
#endif

#ifndef RISCOS
static void
calculate_reduce_prefix(PyCalculatePath *calculate, wchar_t *prefix)
{
    /* Reduce prefix and exec_prefix to their essence,
     * e.g. /usr/local/lib/python1.5 is reduced to /usr/local.
     * If we're loading relative to the build directory,
     * return the compiled-in defaults instead.
     */
    if (calculate->prefix_found > 0) {
        reduce(prefix);
        reduce(prefix);
        /* The prefix is the root directory, but reduce() chopped
         * off the "/". */
        if (!prefix[0]) {
            wcscpy(prefix, separator);
        }
    }
    else {
        wcsncpy(prefix, calculate->prefix, MAXPATHLEN);
    }
}


/* search_for_exec_prefix requires that argv0_path be no more than
   MAXPATHLEN bytes long.
*/
static int
search_for_exec_prefix(PyCalculatePath *calculate, _PyPathConfig *pathconfig,
                       wchar_t *exec_prefix)
{
    size_t n;

    /* If PYTHONHOME is set, we believe it unconditionally */
    if (pathconfig->home) {
        wchar_t *delim = wcschr(pathconfig->home, DELIM);
        if (delim) {
            wcsncpy(exec_prefix, delim+1, MAXPATHLEN);
        }
        else {
            wcsncpy(exec_prefix, pathconfig->home, MAXPATHLEN);
        }
        exec_prefix[MAXPATHLEN] = L'\0';
        joinpath(exec_prefix, calculate->lib_python);
        joinpath(exec_prefix, L"lib-dynload");
        return 1;
    }

    /* Check to see if argv[0] is in the build directory. "pybuilddir.txt"
       is written by setup.py and contains the relative path to the location
       of shared library modules. */
    wcsncpy(exec_prefix, calculate->argv0_path, MAXPATHLEN);
    exec_prefix[MAXPATHLEN] = L'\0';
    joinpath(exec_prefix, L"pybuilddir.txt");
    if (isfile(exec_prefix)) {
        FILE *f = _Py_wfopen(exec_prefix, L"rb");
        if (f == NULL) {
            errno = 0;
        }
        else {
            char buf[MAXPATHLEN+1];
            wchar_t *rel_builddir_path;
            n = fread(buf, 1, MAXPATHLEN, f);
            buf[n] = '\0';
            fclose(f);
            rel_builddir_path = _Py_DecodeUTF8_surrogateescape(buf, n);
            if (rel_builddir_path) {
                wcsncpy(exec_prefix, calculate->argv0_path, MAXPATHLEN);
                exec_prefix[MAXPATHLEN] = L'\0';
                joinpath(exec_prefix, rel_builddir_path);
                PyMem_RawFree(rel_builddir_path );
                return -1;
            }
        }
    }

    /* Search from argv0_path, until root is found */
    copy_absolute(exec_prefix, calculate->argv0_path, MAXPATHLEN+1);
    do {
        n = wcslen(exec_prefix);
        joinpath(exec_prefix, calculate->lib_python);
        joinpath(exec_prefix, L"lib-dynload");
        if (isdir(exec_prefix)) {
            return 1;
        }
        exec_prefix[n] = L'\0';
        reduce(exec_prefix);
    } while (exec_prefix[0]);

    /* Look at configure's EXEC_PREFIX */
    wcsncpy(exec_prefix, calculate->exec_prefix, MAXPATHLEN);
    exec_prefix[MAXPATHLEN] = L'\0';
    joinpath(exec_prefix, calculate->lib_python);
    joinpath(exec_prefix, L"lib-dynload");
    if (isdir(exec_prefix)) {
        return 1;
    }

    /* Fail */
    return 0;
}
#endif

#ifndef RISCOS
static void
calculate_exec_prefix(const _PyCoreConfig *core_config,
                      PyCalculatePath *calculate, wchar_t *exec_prefix)
{
    calculate->exec_prefix_found = search_for_exec_prefix(core_config,
                                                          calculate,
                                                          exec_prefix);
    if (!calculate->exec_prefix_found) {
        if (!Py_FrozenFlag) {
            fprintf(stderr,
                "Could not find platform dependent libraries <exec_prefix>\n");
        }

        wcsncpy(exec_prefix, calculate->exec_prefix, MAXPATHLEN);
        joinpath(exec_prefix, L"lib/lib-dynload");

    }
    /* If we found EXEC_PREFIX do *not* reduce it!  (Yet.) */
}

static void
calculate_reduce_exec_prefix(PyCalculatePath *calculate, wchar_t *exec_prefix)
{
    if (calculate->exec_prefix_found > 0) {
        reduce(exec_prefix);
        reduce(exec_prefix);
        reduce(exec_prefix);
        if (!exec_prefix[0]) {
            wcscpy(exec_prefix, separator);
        }
    }
    else {
        wcsncpy(exec_prefix, calculate->exec_prefix, MAXPATHLEN);
    }
}
#endif

static PyStatus
calculate_program_full_path(PyCalculatePath *calculate, _PyPathConfig *pathconfig)
{
    wchar_t program_full_path[MAXPATHLEN+1];
    memset(program_full_path, 0, sizeof(program_full_path));
    wcsncpy(program_full_path, pathconfig->program_name, MAXPATHLEN);

//    char execpath[MAXPATHLEN+1];

    /* If there is no slash in the argv0 path, then we have to
     * assume python is on the user's $PATH, since there's no
     * other way to find a directory to start the search from.  If
     * $PATH isn't exported, you lose.
     */
/*
    if (wcschr(pathconfig->program_name, SEP)) {
        wcsncpy(program_full_path, pathconfig->program_name, MAXPATHLEN);
    }

    else if (calculate->path_env) {
        wchar_t *path = calculate->path_env;
        while (1) {
            wchar_t *delim = wcschr(path, DELIM);

            if (delim) {
                size_t len = delim - path;
                if (len > MAXPATHLEN) {
                    len = MAXPATHLEN;
                }
                wcsncpy(program_full_path, path, len);
                program_full_path[len] = '\0';
            }
            else {
                wcsncpy(program_full_path, path, MAXPATHLEN);
            }

            joinpath(program_full_path, pathconfig->program_name);
            if (isxfile(program_full_path)) {
                break;
            }

            if (!delim) {
                program_full_path[0] = L'\0';
                break;
            }
            path = delim + 1;
        }
    }

    else {
        program_full_path[0] = '\0';
    }
    if (program_full_path[0] != SEP && program_full_path[0] != '\0') {
        absolutize(program_full_path);
    }
*/
    pathconfig->program_full_path = _PyMem_RawWcsdup(program_full_path);
    if (pathconfig->program_full_path == NULL) {
        return _PyStatus_NO_MEMORY();
    }
    return _PyStatus_OK();
}



static PyStatus
calculate_argv0_path(PyCalculatePath *calculate, const wchar_t *program_full_path,
                     wchar_t *argv0_path, size_t argv0_path_len)
{
    if (safe_wcscpy(argv0_path, program_full_path, argv0_path_len) < 0) {
        return PATHLEN_ERR();
    }

    reduce(argv0_path);

    /* At this point, argv0_path is guaranteed to be less than
       MAXPATHLEN bytes long. */
    return _PyStatus_OK();
}

/* Search for an "pyvenv.cfg" environment configuration file, first in the
   executable's directory and then in the parent directory.
   If found, open it for use when searching for prefixes.
*/

static void
calculate_read_pyenv(PyCalculatePath *calculate)
{
    wchar_t tmpbuffer[MAXPATHLEN+1];
    wchar_t *env_cfg = L"pyvenv.cfg";
    FILE *env_file;

    wcscpy(tmpbuffer, calculate->argv0_path);

    joinpath(tmpbuffer, env_cfg);
    env_file = _Py_wfopen(tmpbuffer, L"r");
    if (env_file == NULL) {
        errno = 0;

        reduce(tmpbuffer);
        reduce(tmpbuffer);
        joinpath(tmpbuffer, env_cfg);

        env_file = _Py_wfopen(tmpbuffer, L"r");
        if (env_file == NULL) {
            errno = 0;
        }
    }

    if (env_file == NULL) {
        return;
    }

    /* Look for a 'home' variable and set argv0_path to it, if found */
    if (_Py_FindEnvConfigValue(env_file, L"home", tmpbuffer, MAXPATHLEN)) {
        wcscpy(calculate->argv0_path, tmpbuffer);
    }
    fclose(env_file);
}

#if 0
static PyStatus
calculate_zip_path(PyCalculatePath *calculate, const wchar_t *prefix,
                   wchar_t *zip_path, size_t zip_path_len)
{
    PyStatus status;

    if (calculate->prefix_found > 0) {
        /* Use the reduced prefix returned by Py_GetPrefix() */
        if (safe_wcscpy(zip_path, prefix, zip_path_len) < 0) {
            return PATHLEN_ERR();
        }
        reduce(zip_path);
        reduce(zip_path);
    }
    else {
        if (safe_wcscpy(zip_path, calculate->prefix, zip_path_len) < 0) {
            return PATHLEN_ERR();
        }
    }
    status = joinpath(zip_path, L"lib/python00.zip", zip_path_len);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    /* Replace "00" with version */
    size_t bufsz = wcslen(zip_path);
    zip_path[bufsz - 6] = VERSION[0];
    zip_path[bufsz - 5] = VERSION[2];
    return _PyStatus_OK();

#if 0

    wcsncpy(calculate->zip_path, prefix, MAXPATHLEN);
    calculate->zip_path[MAXPATHLEN] = L'\0';

#ifndef RISCOS
    if (calculate->prefix_found > 0) {
        /* Use the reduced prefix returned by Py_GetPrefix() */
        reduce(calculate->zip_path);
        reduce(calculate->zip_path);
    } else {
#endif
        wcsncpy(calculate->zip_path, calculate->exec_prefix, MAXPATHLEN);
#ifndef RISCOS
    }
#endif
    joinpath(calculate->zip_path, L"lib/python00/zip");
#endif
}
#endif


static PyStatus
calculate_module_search_path(PyCalculatePath *calculate,
                             _PyPathConfig *pathconfig)
                             //const wchar_t *prefix,
                             //const wchar_t *exec_prefix,
                             //const wchar_t *zip_path)
{
    //printf("       prefix: %ls\n", prefix);
    //printf("  exec_prefix: %ls\n", exec_prefix);

    /* Calculate size of return buffer */
    size_t bufsz = 256;
/*
    if (core_config->module_search_path_env != NULL) {
        bufsz += wcslen(core_config->module_search_path_env) + 1;
    }
*/
#ifndef RISCOS
    wchar_t *defpath = calculate->pythonpath;
    size_t prefixsz = wcslen(prefix) + 1;
    while (1) {
        wchar_t *delim = wcschr(defpath, DELIM);

        if (defpath[0] != SEP) {
            /* Paths are relative to prefix */
            bufsz += prefixsz;
        }

        if (delim) {
            bufsz += delim - defpath + 1;
        }
        else {
            bufsz += wcslen(defpath) + 1;
            break;
        }
        defpath = delim + 1;
    }

    bufsz += wcslen(calculate->zip_path) + 1;
    bufsz += wcslen(exec_prefix) + 1;
#endif

    /* Allocate the buffer */
    wchar_t *buf = PyMem_RawMalloc(bufsz * sizeof(wchar_t));
    if (buf == NULL) {
        return _PyStatus_NO_MEMORY();
    }
    buf[0] = '\0';

#if 0
    /* Run-time value of $PYTHONPATH goes first */
    if (calculate->pythonpath_env) {
        wcscpy(buf, calculate->pythonpath_env);
        wcscat(buf, delimiter);
    }

    /* Next is the default zip path */
    wcscat(buf, calculate->zip_path);
    wcscat(buf, delimiter);
#endif

#ifndef RISCOS
    /* Next goes merge of compile-time $PYTHONPATH with
     * dynamically located prefix.
     */
    defpath = calculate->pythonpath;
    while (1) {
        wchar_t *delim = wcschr(defpath, DELIM);

        if (defpath[0] != SEP) {
            wcscat(buf, prefix);
            if (prefixsz >= 2 && prefix[prefixsz - 2] != SEP &&
                defpath[0] != (delim ? DELIM : L'\0'))
            {
                /* not empty */
                wcscat(buf, separator);
            }
        }

        if (delim) {
            size_t len = delim - defpath + 1;
            size_t end = wcslen(buf) + len;
            wcsncat(buf, defpath, len);
            buf[end] = '\0';
        }
        else {
            wcscat(buf, defpath);
            break;
        }
        defpath = delim + 1;
    }
    wcscat(buf, delimiter);
#endif

    /* Finally, on goes the directory for dynamic-load modules */
    //wcscat(buf, exec_prefix);
    pathconfig->module_search_path = buf;

    buf[0] = '\0';
    wcscat(buf, calculate->lib_dir);
    //wcscat(buf, delimiter);
    //wcscat(buf, calculate->dynload_dir);

    //wcscat(buf, L"<Python"VERSION"Lib$Dir>,<Python"VERSION"DynLoad$Dir>,");

    //printf("calculate_module_search_path : %ls\n", buf);
    return _PyStatus_OK();
}

static PyStatus
calculate_init(PyCalculatePath *calculate, const PyConfig *config)
{
    size_t len;
/*
    const char* path = getenv("Run$Path");
    if (path) {
        calculate->path_env = Py_DecodeLocale(path, &len);
        if (!calculate->path_env) {
            return DECODE_LOCALE_ERR("<Run$Path> define", len);
        }
    }
*/

    char *python3_dir = canonicalise_path("<Python3$Dir>");
    char *python_lib_dir = canonicalise_path("<Python3.8Lib$Dir>");
    char *python_dynload_dir = canonicalise_path("<Python3.8DynLoad$Dir>");

    calculate->pythonpath = Py_DecodeLocale(python3_dir, &len);
    calculate->lib_dir = Py_DecodeLocale(python_lib_dir, &len);
    calculate->dynload_dir = Py_DecodeLocale(python_dynload_dir, &len);

/*
    calculate->pythonpath = Py_DecodeLocale(getenv("Python$Path"), &len);
    if (!calculate->pythonpath) {
        return DECODE_LOCALE_ERR("<Python$Path> define", len);
    }

    calculate->lib_dir = Py_DecodeLocale(getenv("Python"VERSION"Lib$Dir"), &len);
    if (!calculate->lib_dir) {
        return DECODE_LOCALE_ERR("Python"VERSION"Lib$Dir not defined", len);
    }

    calculate->dynload_dir = Py_DecodeLocale(getenv("Python"VERSION"DynLoad$Dir"), &len);
    if (!calculate->dynload_dir) {
        return DECODE_LOCALE_ERR("Python"VERSION"DynLoad$Dir not defined", len);
    }

*/
    return _PyStatus_OK();
}

static void
calculate_free(PyCalculatePath *calculate)
{
    PyMem_RawFree(calculate->pythonhome);
    PyMem_RawFree(calculate->pythonpath);
    //PyMem_RawFree(calculate->prefix);
    //PyMem_RawFree(calculate->exec_prefix);
    PyMem_RawFree(calculate->lib_dir);
    PyMem_RawFree(calculate->dynload_dir);
    //PyMem_RawFree(calculate->path_env);
}


static PyStatus
calculate_path(PyCalculatePath *calculate, _PyPathConfig *pathconfig)
{
    PyStatus status;

    if (pathconfig->program_full_path == NULL) {
        status = calculate_program_full_path(calculate, pathconfig);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }

    size_t len;
    char *python3_dir = canonicalise_path("<Python3$Dir>");
    pathconfig->prefix = Py_DecodeLocale(python3_dir, &len);
    pathconfig->exec_prefix = Py_DecodeLocale(python3_dir, &len);

/*
    pathconfig->prefix = Py_DecodeLocale("<Python3$Dir>", &len);
    if (!pathconfig->prefix) {
        return DECODE_LOCALE_ERR("<Python3$Dir> define", len);
    }

    pathconfig->exec_prefix = Py_DecodeLocale("<Python3$Dir>", &len);
    if (!pathconfig->exec_prefix) {
        return DECODE_LOCALE_ERR("<Python3$Dir> define", len);
    }
*/
#if 0
/*
    printf("pathconfig->prefix      : %ls\n", pathconfig->prefix);
    printf("pathconfig->exec_prefix : %ls\n", pathconfig->exec_prefix);
*/

    wchar_t argv0_path[MAXPATHLEN+1];
    memset(argv0_path, 0, sizeof(argv0_path));

    status = calculate_argv0_path(calculate, pathconfig->program_full_path,
                                  argv0_path, Py_ARRAY_LENGTH(argv0_path));
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    //calculate_read_pyenv(calculate);

    wchar_t prefix[MAXPATHLEN+1];
    memset(prefix, 0, sizeof(prefix));
    status = calculate_prefix(calculate, pathconfig,
                              argv0_path,
                              prefix, Py_ARRAY_LENGTH(prefix));
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    wchar_t zip_path[MAXPATHLEN+1];    /* ".../lib/pythonXY.zip" */
    memset(zip_path, 0, sizeof(zip_path));

/*
    status = calculate_zip_path(calculate, prefix,
                                zip_path, Py_ARRAY_LENGTH(zip_path));
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }
*/
    wchar_t exec_prefix[MAXPATHLEN+1];
    memset(exec_prefix, 0, sizeof(exec_prefix));
#endif

#if 0
#ifdef RISCOS
    wcsncpy(exec_prefix, calculate->exec_prefix, MAXPATHLEN);
#else
    status = calculate_exec_prefix(calculate, pathconfig, argv0_path,
                                   exec_prefix, Py_ARRAY_LENGTH(exec_prefix));
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    if ((!calculate->prefix_found || !calculate->exec_prefix_found) &&
        calculate->warnings)
    {
        fprintf(stderr,
                "Consider setting $PYTHONHOME to <prefix>[:<exec_prefix>]\n");
    }
#endif
#endif

    if (pathconfig->module_search_path == NULL) {
        status = calculate_module_search_path(calculate, pathconfig);//,
                                              //prefix, exec_prefix, zip_path);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }
    
#if 0
#ifndef RISCOS
    calculate_reduce_prefix(calculate, prefix);
#else
    if (pathconfig->prefix == NULL) {
        status = calculate_set_prefix(calculate, pathconfig, prefix);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }
#endif

#ifndef RISCOS
    calculate_reduce_exec_prefix(calculate, exec_prefix);
#else
    if (pathconfig->exec_prefix == NULL) {
        status = calculate_set_exec_prefix(calculate, pathconfig, exec_prefix);
        if (_PyStatus_EXCEPTION(status)) {
            return status;
        }
    }
#endif
#endif
    return _PyStatus_OK();
}

/* Calculate the Python path configuration.

   Inputs:

   - PATH environment variable
   - Macros: PYTHONPATH, PREFIX, EXEC_PREFIX, VERSION (ex: "3.9").
     PREFIX and EXEC_PREFIX are generated by the configure script.
     PYTHONPATH macro is the default search path.
   - pybuilddir.txt file
   - pyvenv.cfg configuration file
   - PyConfig fields ('config' function argument):

     - pathconfig_warnings
     - pythonpath_env (PYTHONPATH environment variable)

   - _PyPathConfig fields ('pathconfig' function argument):

     - program_name: see config_init_program_name()
     - home: Py_SetPythonHome() or PYTHONHOME environment variable

   - current working directory: see copy_absolute()

   Outputs, 'pathconfig' fields:

   - program_full_path
   - module_search_path
   - prefix
   - exec_prefix

   If a field is already set (non NULL), it is left unchanged. */
PyStatus
_PyPathConfig_Calculate(_PyPathConfig *pathconfig, const PyConfig *config)
{
    PyStatus status;
    PyCalculatePath calculate;
    memset(&calculate, 0, sizeof(calculate));

    status = calculate_init(&calculate, config);
    if (_PyStatus_EXCEPTION(status)) {
        goto done;
    }

    status = calculate_path(&calculate, pathconfig);
    if (_PyStatus_EXCEPTION(status)) {
        goto done;
    }

    status = _PyStatus_OK();

done:
    calculate_free(&calculate);
/*
    printf("_PyPathConfig_Calculate returning %d\n", status);
    printf("program_full_path : %ls\n", pathconfig->program_full_path);
    printf("module_search_path : %ls\n", pathconfig->module_search_path);
    printf("prefix : %ls\n", pathconfig->prefix);
    printf("exec_prefix : %ls\n", pathconfig->exec_prefix);
*/
    return status;
}

#ifdef __cplusplus
}
#endif
